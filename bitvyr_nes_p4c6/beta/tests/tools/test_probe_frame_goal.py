"""Frame-bounded probe must not accept stale or partial UART status."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[2]/'tools/device_probe.py'
spec = importlib.util.spec_from_file_location('probe', path)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

class FrameGoalTests(unittest.TestCase):
    """Проверяет привязку frame-goal к единственному текущему launch, полным wire-строкам и реальному status completion."""
    def test_explicit_load_or_runtime_failure_stops_waiting(self):
        """Не ждёт тысячи кадров после отказа ZIP или явного runtime fault."""
        for marker, launched in ((b'ROM_READ_FAIL', False), (b'NES_RUNTIME_FAIL', True),
                                 (b'NES_WORKER_FAIL', True)):
            with self.subTest(marker=marker):
                goal = probe.FrameGoal(21, 2280)
                if launched:
                    goal.feed(b'NES_GAME_START index=21\n')
                line = b'E (10) nes_app: ' + marker + b' index=21 reason=fixture'
                goal.feed(line)
                self.assertFalse(goal.reached)
                with self.assertRaisesRegex(RuntimeError, 'device failure'):
                    goal.feed(b'\n')

    def test_prior_counter_and_partial_line(self):
        """Старый status до launch и незавершённая строка не достигают frame-goal; завершённый текущий status достигает."""
        goal = probe.FrameGoal(3, 100)
        goal.feed(b'NES_STATUS {"game":3,"playing":true,"frames":900}\n')
        self.assertFalse(goal.reached)
        goal.feed(b'I (1) app: NES_GAME_START index=3\n')
        goal.feed(b'NES_STATUS {"game":3,"playing":true,"frames":99}\n')
        self.assertFalse(goal.reached)
        goal.feed(b'NES_STATUS {"game":3,"playing":true,"frames":100}')
        self.assertFalse(goal.reached)
        goal.feed(b'\n')
        self.assertTrue(goal.reached)

    def test_stopped_state_is_not_success(self):
        """Счётчик playing=false не подтверждает прохождение игрового frame-goal."""
        goal = probe.FrameGoal(0, 10)
        goal.feed(b'NES_GAME_START index=0\nNES_STATUS {"game":0,"playing":false,"frames":20}\n')
        self.assertFalse(goal.reached)

    def test_output_status_requires_current_launch_and_complete_line(self):
        """Output STATUS требует текущий launch и newline; прежний counter и частичный JSON не принимаются."""
        goal = probe.FrameGoal(0, 240)
        goal.feed(b'NES_OUTPUT_FRAME_PIPELINE_STATUS {"game":0,"playing":true,"frames":900}\n')
        self.assertFalse(goal.reached)
        goal.feed(b'NES_GAME_START index=0\nNES_OUTPUT_FRAME_PIPELINE_STATUS {"game":0,"playing":true,"frames":240}')
        self.assertFalse(goal.reached)
        goal.feed(b'\n')
        self.assertTrue(goal.reached)

    def test_output_times_only_progress_and_unbound_cleanup(self):
        """Output TIMES обновляет только progress; goal завершает текущий playing STATUS, а unbound cleanup не подменяет его."""
        goal = probe.FrameGoal(0, 240, track_performance=True)
        goal.feed(b'NES_OUTPUT_FRAME_PIPELINE_TIMES game=0 frames=999 samples=120\n')
        self.assertEqual(goal.progress_frames, 0)
        goal.feed(b'NES_GAME_START index=0\nNES_OUTPUT_FRAME_PIPELINE_TIMES game=0 frames=240 samples=120\n')
        self.assertEqual(goal.progress_frames, 240)
        self.assertFalse(goal.reached)
        goal.feed(b'NES_OUTPUT_FRAME_PIPELINE_STATUS {"playing":false,"frames":0,"owner":"unbound"}\n')
        self.assertFalse(goal.reached)
        goal.feed(b'NES_FRAME_PIPELINE_PRESENT_STATUS {"accepted_frame":999}\n')
        self.assertFalse(goal.reached)
        goal.feed(b'NES_OUTPUT_FRAME_PIPELINE_STATUS {"game":0,"playing":true,"frames":241}\n')
        self.assertTrue(goal.reached)
        goal.feed(b'NES_OUTPUT_FRAME_PIPELINE_STATUS {"playing":false,"frames":0,"owner":"unbound"}\n')

    def test_output_identity_failures(self):
        """После launch чужой либо отсутствующий game в Output STATUS/TIMES отклоняется как смена идентичности."""
        for line in (b'NES_OUTPUT_FRAME_PIPELINE_STATUS {"game":1,"playing":true,"frames":240}\n',
                     b'NES_OUTPUT_FRAME_PIPELINE_STATUS {"playing":true,"frames":240}\n',
                     b'NES_OUTPUT_FRAME_PIPELINE_TIMES game=1 frames=240 samples=120\n'):
            goal = probe.FrameGoal(0, 240, track_performance=True)
            goal.feed(b'NES_GAME_START index=0\n')
            with self.assertRaises(RuntimeError):
                goal.feed(line)

    def test_wrong_launch_or_second_launch(self):
        """Неверный index и второй GameStart нарушают единственный запуск и завершают ожидание отказом."""
        for data in (b'NES_GAME_START index=1\n', b'NES_GAME_START index=0\nNES_GAME_START index=0\n'):
            with self.assertRaises(RuntimeError):probe.FrameGoal(0, 10).feed(data)

    def test_game_changed(self):
        """STATUS другой игры после подтверждённого launch не переносит её кадры в текущий goal."""
        with self.assertRaises(RuntimeError):
            probe.FrameGoal(0, 10).feed(b'NES_GAME_START index=0\nNES_STATUS {"game":1,"playing":true,"frames":10}\n')

    def test_bad_status_and_unterminated_line(self):
        """Повреждённый JSON и строка длиннее64KiB дают явную ошибку вместо бесконечного накопления RX."""
        with self.assertRaises(ValueError):
            probe.FrameGoal(0, 10).feed(b'NES_GAME_START index=0\nNES_STATUS {broken}\n')
        with self.assertRaises(RuntimeError):probe.FrameGoal(0, 10).feed(b'x'*65537)

    def test_argument_bounds(self):
        """CLI принимает допустимый bounded frame-goal и отвергает отсутствие game, нулевую/чрезмерную цель и слишком короткий интервал."""
        self.assertEqual(probe.parse_args(['--game','0','--until-frame','1200','--start-frame','100']).until_frame,1200)
        for args in (['--until-frame','10'], ['--game','0','--until-frame','0'],
                     ['--game','0','--until-frame','360001'],
                     ['--game','0','--until-frame','103','--start-frame','100']):
            with self.assertRaises(SystemExit):probe.parse_args(args)

if __name__ == '__main__':unittest.main()
