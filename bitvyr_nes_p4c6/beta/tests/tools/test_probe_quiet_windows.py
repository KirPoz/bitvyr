"""Keep state observers outside FPS windows without weakening frame completion."""
import importlib.util
import itertools
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('quiet_probe',root/'tools/device_probe.py')
probe=importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
audit_spec=importlib.util.spec_from_file_location('integrated_fps_audit',root/'tools/audit_fps_observers.py')
auditor=importlib.util.module_from_spec(audit_spec);audit_spec.loader.exec_module(auditor)


class QuietWindowTests(unittest.TestCase):
    """Проверяет quiet union кадровых окон и отложенные команды на fake UART без ослабления completion."""
    def test_performance_is_progress_never_completion(self):
        """PERF до launch/без newline игнорируется; после launch он даёт progress, но только playing STATUS завершает goal."""
        goal=probe.FrameGoal(8,6720,track_performance=True)
        goal.feed(b'NES_PERF game=8 frames=9999 fps=60.00\n')
        self.assertEqual(goal.progress_frames,0)
        goal.feed(b'NES_GAME_START index=8\nNES_PERF game=8 frames=6720 fps=60.00')
        self.assertEqual(goal.progress_frames,0)
        goal.feed(b'\n')
        self.assertEqual(goal.progress_frames,6720)
        self.assertFalse(goal.reached)
        goal.feed(b'NES_STATUS {"game":8,"playing":false,"frames":6720}\n')
        self.assertFalse(goal.reached)
        goal.feed(b'NES_STATUS {"game":8,"playing":true,"frames":6720}\n')
        self.assertTrue(goal.reached)

    def test_union_margin_and_end_boundary(self):
        """Проверяет объединённый quiet-интервал с120-кадровым запасом слева и исключённой конечной границей."""
        ends=[720,840,960]
        for frame in range(1100):
            self.assertEqual(probe.in_quiet_fps_window(frame,ends),480<=frame<960)
        self.assertFalse(probe.in_quiet_fps_window(719,[]))

    def test_cli_rejects_unbounded_or_nonreport_endpoints(self):
        """Quiet endpoints требуют game, bounded цель, положительные уникальные кратные120 кадры не позднее завершения."""
        valid=['--game','0','--until-frame','1200','--quiet-fps-endpoint','720']
        self.assertEqual(probe.parse_args(valid).quiet_fps_endpoint,[720])
        for extra in (['--quiet-fps-endpoint','720'],
                      ['--game','0','--until-frame','1200','--quiet-fps-endpoint','0'],
                      ['--game','0','--until-frame','1200','--quiet-fps-endpoint','721'],
                      ['--game','0','--until-frame','1200','--quiet-fps-endpoint','1320'],
                      valid+['--quiet-fps-endpoint','720']):
            with self.assertRaises(SystemExit):probe.parse_args(extra)

    def test_wire_commands_are_deferred_and_status_still_required(self):
        """Fake UART доказывает отсутствие status в quiet-окне; один PERF360 не завершает capture без следующего реального status."""
        class FakeSerial:
            """Моделирует bounded RX и кадры PERF шагом120; хранит команды для проверки observer-порядка без COM."""
            instances=[]
            def __init__(self,**kwargs):
                """Создаёт закрытый fake port, нулевой frame и собственные очереди RX/команд; параметры UART не используют оборудование."""
                self.is_open=False;self.frame=0;self.playing=False
                self.pending=bytearray();self.commands=[]
                self.instances.append(self)
            def open(self):
                """Помечает только тестовый порт открытым; реального устройства нет."""
                self.is_open=True
            def close(self):
                """Снимает тестовый is_open, позволяя проверить освобождение capture owner."""
                self.is_open=False
            def flush(self):
                """Моделирует мгновенный flush уже сохранённых команд без отдельной передачи или изменения кадров."""
                pass
            def write(self,data):
                """Разбирает run/menu/status, сохраняет кадр отправки и формирует только явный status-ответ; возвращает длину записи."""
                command=data.decode('ascii').strip()
                self.commands.append((command,self.frame,self.playing))
                if command=='run 0':
                    self.playing=True;self.pending.extend(b'NES_GAME_START index=0\n')
                elif command=='menu':self.playing=False
                elif command=='status':
                    state=dict(game=0,playing=self.playing,frames=self.frame)
                    self.pending.extend(('NES_STATUS '+json.dumps(state)+'\n').encode())
                return len(data)
            def read(self,size):
                """При активной игре добавляет PERF следующего120-кадрового шага и опустошает накопленный fake RX."""
                if self.playing:
                    self.frame+=120
                    self.pending.extend(f'NES_PERF game=0 frames={self.frame} fps=60.00\n'.encode())
                data=bytes(self.pending);self.pending.clear();return data

        module=types.SimpleNamespace(Serial=FakeSerial)
        with tempfile.TemporaryDirectory() as temporary:
            for protected in (False,True):
                argv=['--game','0','--until-frame','360','--seconds','10',
                      '--status-interval','1','--log',str(Path(temporary)/f'{protected}.log')]
                if protected:argv+=['--quiet-fps-endpoint','240']
                ticks=itertools.count(0,.25)
                with patch.dict(sys.modules,{'serial':module}),patch.object(probe.time,'monotonic',side_effect=lambda:next(ticks)):
                    result=probe.capture(probe.parse_args(argv))
                queries=[frame for command,frame,playing in FakeSerial.instances[-1].commands
                         if command=='status' and playing]
                if protected:
                    self.assertTrue(queries)
                    self.assertTrue(all(frame>=240 for frame in queries))
                    # PERF360 alone must not end the run: the next real status
                    # reports480, proving the original completion gate remains.
                    self.assertIn(480,queries)
                    self.assertIn(b'"frames": 480',result)
                    record=json.loads(Path(temporary,f'{protected}.commands.json').read_text())
                    self.assertTrue(auditor.audit(result,0,[240],record)['passed'])
                else:self.assertTrue(any(frame<240 for frame in queries))
                self.assertFalse(FakeSerial.instances[-1].is_open)


if __name__=='__main__':unittest.main()
