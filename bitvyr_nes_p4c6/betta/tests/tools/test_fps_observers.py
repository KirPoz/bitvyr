"""Audit the actual wall-time interval, including queued/stale-command failures."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('fps_audit',Path(__file__).resolve().parents[2]/'tools/audit_fps_observers.py')
auditor=importlib.util.module_from_spec(spec);spec.loader.exec_module(auditor)

def status(frame,game=0,playing=True):
    """Формирует один legacy STATUS с явными game/playing/frame, чтобы проверить границы observer в raw-порядке."""
    return 'NES_STATUS '+json.dumps(dict(game=game,playing=playing,frames=frame))

def perf(frame,game=0):
    """Создаёт завершённый PERF заданной игры и кадра; значение fps служит полем фикстуры, а не аппаратным замером."""
    return f'I (1) nes_app: NES_PERF game={game} frames={frame} fps=60.10'

def checked(lines,request_delta=0):
    """Связывает raw SHA с числом полных status-запросов и проверяет окно до720; request_delta моделирует потерю или лишний ответ."""
    raw=('\n'.join(lines)+'\n').encode()
    count=sum('NES_STATUS ' in line for line in lines)+request_delta
    requests=[dict(command='status',bytes=7,expected_bytes=7) for _ in range(max(count,0))]
    record=dict(completed=True,log_sha256=hashlib.sha256(raw).hexdigest(),quiet_fps_endpoints=[720],commands=requests)
    return auditor.audit(raw,0,[720],record)

class FpsObserverTests(unittest.TestCase):
    """Проверяет запрет status внутри измеряемого интервала и обязательную связь raw, launch и command ledger."""
    def test_clean_and_boundary_order(self):
        """Различает status до начального PERF, внутри окна и после конечного PERF по порядку строк, а не одинаковому frame-полю."""
        initial=[status(0,playing=False),'NES_GAME_START index=0']
        self.assertTrue(checked(initial+[status(600),perf(600),perf(720),status(720)])['passed'])
        self.assertFalse(checked(initial+[perf(600),status(600),perf(720),status(720)])['passed'])
        self.assertFalse(checked(initial+[perf(600),status(720),perf(720)])['passed'])
        self.assertTrue(checked(initial+[perf(600),perf(720),status(720)])['passed'])

    def test_stale_host_progress_cannot_hide_contamination(self):
        # Host may still know only PERF360 when it enqueues STATUS. FIFO records
        # show PERF600 before that response, so the measured window is rejected.
        """Отклоняет внутренний status, даже если host при отправке ещё видел устаревший PERF360; решающим остаётся raw FIFO."""
        lines=[status(0,playing=False),'NES_GAME_START index=0',perf(360),perf(480),perf(600),status(660),perf(720),status(840)]
        report=checked(lines)
        self.assertFalse(report['passed'])
        self.assertEqual(report['windows'][0]['observer_lines'],[6])

    def test_stopped_status_is_also_an_observer(self):
        """Считает playing=false status внутри окна вмешательством, а не безопасным способом обойти quiet-политику."""
        self.assertFalse(checked([status(0,playing=False),'NES_GAME_START index=0',perf(600),status(660,playing=False),perf(720),status(720)])['passed'])

    def test_loss_and_unsolicited_records_fail_closed(self):
        """Недостающий, лишний или незапрошенный status не позволяет объявить окно прошедшим проверку command/response."""
        lines=[status(0,playing=False),'NES_GAME_START index=0',perf(600),perf(720),status(720)]
        self.assertFalse(checked(lines,1)['passed'])
        self.assertFalse(checked(lines,-1)['passed'])
        self.assertFalse(checked([status(0,playing=False),*lines])['passed'])

    def test_prelaunch_and_missing_or_duplicate_bounds(self):
        """Отклоняет PERF до launch, отсутствующую или повторную границу и обратный порядок концов измеряемого окна."""
        self.assertFalse(checked([status(0,playing=False),perf(600),perf(720),'NES_GAME_START index=0',status(720)])['passed'])
        for bounds in ([perf(720)],[perf(600),perf(720),perf(720)],[perf(720),perf(600)]):
            self.assertFalse(checked([status(0,playing=False),'NES_GAME_START index=0',*bounds,status(840)])['passed'])

    def test_changed_game_and_bad_observers(self):
        """Смена game, повреждённый status, STATUS_ERROR и runtime fault сохраняют отказ даже после корректных PERF-границ."""
        start=[status(0,playing=False),'NES_GAME_START index=0',perf(600),perf(720)]
        for tail in ([status(840,game=1)],[perf(840,game=1),status(840)],
                     ['NES_STATUS {broken}'],['NES_STATUS_ERROR reason=video'],['NES_RUNTIME_FAIL reason=owner',status(840)]):
            self.assertFalse(checked(start+tail)['passed'])

    def test_hash_and_completion_evidence(self):
        """Требует завершённый capture, точный SHA raw, запрошенный endpoint и полные записи UART; отдельные подмены отвергаются."""
        raw=('\n'.join([status(0,playing=False),'NES_GAME_START index=0',perf(600),perf(720),status(720)])+'\n').encode()
        record=dict(completed=True,log_sha256=hashlib.sha256(raw).hexdigest(),quiet_fps_endpoints=[720],
                    commands=[dict(command='status',bytes=7,expected_bytes=7)]*2)
        for change in (dict(completed=False),dict(log_sha256='0'*64),dict(quiet_fps_endpoints=[840]),
                       dict(commands=[dict(command='status',bytes=3,expected_bytes=7)]*2)):
            self.assertFalse(auditor.audit(raw,0,[720],dict(record,**change))['passed'])

class InitialUnboundOutputTests(unittest.TestCase):
    """Нейтральный handshake не расширяет приёмку Output окон или завершения."""
    neutral=dict(playing=False,frames=0,generation=0,owner='unbound',image_available=False,game_fps_claim=False)

    def run_protocol(self,lines,*,allow=True,requests=2,completed=True):
        """Связывает literal RAW и command record без подмены числа ответов."""
        raw=('\n'.join(lines)+'\n').encode()
        record=dict(completed=completed,log_sha256=hashlib.sha256(raw).hexdigest(),quiet_fps_endpoints=[720],
                    commands=[dict(command='status',bytes=7,expected_bytes=7)]*requests)
        return auditor.audit(raw,0,[720],record,allow_initial_unbound_output_status=allow)

    def line(self,**changes):
        """Создаёт typed protocol record; отрицательные поля задаёт вызывающий тест."""
        return 'NES_OUTPUT_FRAME_PIPELINE_STATUS '+json.dumps(dict(self.neutral,**changes))

    def clean(self,initial=None):
        """Возвращает окно без внутренних observers и с финальным legacy STATUS."""
        return [self.line() if initial is None else initial,'NES_GAME_START index=0',perf(600),perf(720),status(720)]

    def test_opt_in_counts_exactly_one_initial_response(self):
        """Разрешает ровно один нейтральный Output handshake только при opt-in; default API не засчитывает его как legacy STATUS."""
        lines=self.clean();report=self.run_protocol(lines)
        self.assertTrue(report['passed']);self.assertEqual(report['status_requests'],2)
        self.assertEqual(report['status_responses'],2)
        self.assertEqual(report['initial_unbound_output_status']['accepted'],1)
        default=self.run_protocol(lines,allow=False)
        self.assertFalse(default['passed']);self.assertEqual(default['status_responses'],1)
        self.assertNotIn('initial_unbound_output_status',default)

    def test_default_legacy_api_is_unchanged(self):
        """Подтверждает, что обычный initial legacy STATUS одинаково проходит с выключенным и включённым opt-in."""
        lines=self.clean(status(0,playing=False))
        self.assertTrue(self.run_protocol(lines,allow=False)['passed'])
        self.assertTrue(self.run_protocol(lines)['passed'])

    def test_neutral_after_launch_never_accepted(self):
        """Не принимает нейтральный unbound Output после launch ни перед PERF, ни между его границами, ни в конце."""
        for insert in (2,3,4):
            lines=self.clean();lines.insert(insert,self.line())
            report=self.run_protocol(lines,requests=3)
            self.assertFalse(report['passed']);self.assertEqual(report['initial_unbound_output_status']['accepted'],1)

    def test_multiple_or_mixed_prelaunch_observers_rejected(self):
        """Второй initial handshake или смесь Output/legacy до launch остаются неоднозначным наблюдением и отклоняются."""
        for initial in (self.line(),status(0,playing=False)):
            lines=self.clean();lines.insert(1,initial)
            self.assertFalse(self.run_protocol(lines,requests=3)['passed'])

    def test_active_or_wrong_neutral_fields_fail_closed(self):
        """Требует точные значения и JSON-типы нейтрального handshake; активный owner, кадр, изображение и ложные numeric bool отвергаются."""
        changes=[dict(playing=True),dict(frames=1),dict(generation=1),dict(owner='worker'),
                 dict(image_available=True),dict(game_fps_claim=True),dict(game=0),
                 dict(frames=False),dict(generation=False),dict(frames=0.0),dict(playing=0),
                 dict(image_available=0),dict(game_fps_claim=0),dict(owner=None)]
        for change in changes:
            with self.subTest(change=change):self.assertFalse(self.run_protocol(self.clean(self.line(**change)))['passed'])

    def test_malformed_missing_extra_and_duplicate_keys_rejected(self):
        """Повреждённый JSON, неверный корень, неполные, лишние и повторяющиеся ключи не маскируются нейтральным handshake."""
        missing=dict(self.neutral);del missing['generation']
        records=['{broken}','[]','null',json.dumps(missing),json.dumps(dict(self.neutral,extra=0)),
                 '{"frames":1,'+json.dumps(self.neutral)[1:]]
        for record in records:
            self.assertFalse(self.run_protocol(self.clean('NES_OUTPUT_FRAME_PIPELINE_STATUS '+record))['passed'])

    def test_opt_in_does_not_hide_loss_runtime_error_or_observers(self):
        """Opt-in не ослабляет число ответов, completion, runtime fault или запрет observer внутри FPS-окна."""
        self.assertFalse(self.run_protocol(self.clean(),requests=3)['passed'])
        self.assertFalse(self.run_protocol(self.clean(),requests=1)['passed'])
        self.assertFalse(self.run_protocol(self.clean(),completed=False)['passed'])
        contaminated=self.clean();contaminated.insert(3,status(660))
        report=self.run_protocol(contaminated,requests=3)
        self.assertFalse(report['passed']);self.assertEqual(report['windows'][0]['observer_lines'],[4])
        runtime=self.clean();runtime.insert(3,'NES_RUNTIME_FAIL reason=owner')
        self.assertFalse(self.run_protocol(runtime)['passed'])


if __name__=='__main__':unittest.main()
