"""Проверяет валидность трассы ввода, последовательность событий и маски контроллера."""

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('input_audit', ROOT/'tools/audit_input_trace.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def record(frame, serial=0, **changes):
    """Формирует один полный NES_INPUT с generation2 и нулевыми физическими источниками; changes вводит проверяемые отклонения."""
    fields = dict(generation=2, frame=frame, xbox_raw=0, xbox_effective=0,
                  serial=serial, touch=0, applied=serial, contacts=0, ids='0000',
                  poll_ok=1, suppressed=0, action='none')
    fields.update(changes)
    return 'NES_INPUT '+' '.join(f'{key}={value}' for key, value in fields.items())


class InputAuditTests(unittest.TestCase):
    """Проверяет фактический serial/applied trace, полноту Start pulse и отказы при чужом вводе или неоднозначном capture."""
    def check_log(self, rows, starts=(100,), launches=1, status_tag='NES_STATUS '):
        """Создаёт временный raw с заданными launch/status и проверяет starts; корневые журналы и оборудование не используются."""
        text = '\n'.join(['NES_GAME_START index=0']*launches+rows+
                         [status_tag+'{"frames": 200, "playing": true}'])+'\n'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'probe.log'
            path.write_text(text, encoding='utf-8')
            return module.audit(path, starts, status_tag=status_tag)

    def test_exact_start_pulse(self):
        """Принимает frame0 и полный Start press100/release103 с ожидаемой фактической applied-маской."""
        self.assertTrue(self.check_log([record(0), record(100, 8), record(103)])['passed'])

    def test_no_start_and_center_touch(self):
        """Контакт без touch-кнопок в центре экрана не меняет applied input и не создаёт ложный Start pulse."""
        self.assertTrue(self.check_log([record(0), record(20, contacts=1, ids='0002')], ())['passed'])

    def test_suppressed_xbox_does_not_change_supplied_input(self):
        """Различает raw Xbox и effective/applied: подавленный raw-бит не меняет supplied serial-маску этого аудита."""
        self.assertTrue(self.check_log([record(0, xbox_raw=1)], ())['passed'])

    def test_physical_xbox_is_detected(self):
        """Effective Xbox-бит с изменённым applied отвергается как примесь физического ввода."""
        with self.assertRaisesRegex(ValueError, 'physical'):
            self.check_log([record(0), record(20, xbox_effective=1, applied=1)], ())

    def test_touch_is_detected_even_when_it_duplicates_start(self):
        """Touch-кнопка отклоняется даже при совпадении с ожидаемым serial Start, чтобы маска не скрывала источник."""
        with self.assertRaisesRegex(ValueError, 'physical'):
            self.check_log([record(0), record(100, 8, touch=8, contacts=1)])

    def test_missing_release_record_is_detected(self):
        """Отсутствие release после зарегистрированного Start не считается полным input movie."""
        with self.assertRaisesRegex(ValueError, 'transition'):
            self.check_log([record(0), record(100, 8)])

    def test_cpu_bench_completion_rejects_a_missing_press_record(self):
        """CPU-bench completion не заменяет отсутствующий press trace перед release."""
        with self.assertRaisesRegex(ValueError, 'transition'):
            self.check_log([record(0), record(103)], status_tag='NES_CPU_BENCH_STATUS ')

    def test_cpu_bench_completion_accepts_the_complete_input_trace(self):
        """Полный press/release trace принимается и при явно выбранном CPU-bench completion marker."""
        self.assertTrue(self.check_log([record(0), record(100, 8), record(103)],
                                      status_tag='NES_CPU_BENCH_STATUS ')['passed'])

    def test_coupled_bench_completion_rejects_a_missing_release_record(self):
        """Coupled completion не оправдывает trace с press без обязательного release."""
        with self.assertRaisesRegex(ValueError, 'transition'):
            self.check_log([record(0), record(100, 8)], status_tag='NES_COUPLED_BENCH_STATUS ')

    def test_unexpected_serial_is_detected(self):
        """Незапланированная serial-маска вне Start pulse отвергается независимо от нулевых физических источников."""
        with self.assertRaisesRegex(ValueError, 'supplied'):
            self.check_log([record(0), record(50, 8)])

    def test_action_is_detected(self):
        """Действие exit отклоняется как изменение игрового маршрута даже без NES-кнопки."""
        with self.assertRaisesRegex(ValueError, 'action'):
            self.check_log([record(0), record(20, action='exit')], ())

    def test_generation_and_order_are_checked(self):
        """Смена generation и повторный frame нарушают однозначный порядок input trace и должны завершать аудит отказом."""
        for rows in ([record(0), record(20, generation=3)], [record(0), record(0)]):
            with self.assertRaisesRegex(ValueError, 'ordering'):
                self.check_log(rows, ())

    def test_incomplete_or_ambiguous_capture_is_rejected(self):
        """Отсутствие trace/frame0 либо повторный launch не позволяет приписать input одному завершённому запуску."""
        for rows, launches in (([], 1), ([record(10)], 1), ([record(0)], 2)):
            with self.assertRaises(ValueError):
                self.check_log(rows, (), launches)

    def test_malformed_trace_is_rejected(self):
        """Повтор поля applied делает wire-строку неоднозначной и отклоняется до проверки movie."""
        with self.assertRaisesRegex(ValueError, 'malformed'):
            self.check_log([record(0)+' applied=0'], ())

    def test_ambiguous_start_pulses_are_rejected(self):
        """Пересекающиеся, повторные, обратные и отрицательные Start-кадры не образуют допустимый movie."""
        for starts in ((100, 102), (100, 100), (110, 100), (-1,)):
            with self.assertRaises(ValueError):
                self.check_log([record(0)], starts)


if __name__ == '__main__':
    unittest.main()
