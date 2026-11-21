"""Офлайн-проверки сохранности человеческого input prefix, без COM/ROM/SDK."""
import tempfile
from pathlib import Path
import unittest

from export_playthrough_movies import build_bank, write_bank, select_rois


def start(crc="1234abcd"):
    """Задаёт свежий запуск с независимым CRC, без выдуманного ROM-содержимого."""
    return f"NES_GAME_START index=2 name=Test Game mapper=4 region=NTSC crc={crc}"


def inp(frame, mask, generation=3, action="none"):
    """Задаёт точку до dispatch; UI action не должна попадать в guest movie."""
    return (f"NES_INPUT generation={generation} frame={frame} applied={mask} action={action} "
            f"xbox_raw={mask} xbox_effective={mask} serial=0 touch=0 poll_ok=1")


def perf(frame, generation=3):
    """Завершение N подтверждает ввод до N-1, но не ещё ожидающий input N."""
    return f"NES_OUTPUT_FRAME_PIPELINE_TIMES game=2 frames={frame} generation={generation} mapper=4 region=NTSC samples=120"


def bank(*lines):
    """Закрывает строки как реальный UART log; incomplete EOF тестируется отдельно."""
    return build_bank(("\n".join(lines) + "\n").encode(), "fixture")


class HumanExportTests(unittest.TestCase):
    """Проверяет опасные границы и неоднозначность, а не форму JSON."""

    def test_full_prefix_not_initial_queue_and_tail_not_committed(self):
        """Больше16 смен сохраняются целиком; кадр N не входит в измеренный prefix."""
        result = bank(start(), *(inp(i, (i % 2) * 128) for i in range(41)), perf(40))
        s = result["segments"][0]
        self.assertTrue(s["replayable"])
        self.assertEqual(len(s["events"]), 40)
        self.assertEqual(s["last_frame"], 39)
        self.assertEqual(s["uncommitted_tail"][0]["frame"], 40)
        self.assertEqual(s["terminal_release"]["frame"], 40)
        self.assertTrue(s["terminal_release"]["outside_observed_prefix"])
        self.assertEqual(s["source_input_counts"]["xbox_raw"], 20)

    def test_ui_and_duplicate_last_before_acceptance(self):
        """UI не исполняется; последняя маска одного ещё не принятого кадра побеждает."""
        s = bank(start(), inp(0, 0), inp(20, 16), inp(20, 0, action="settings"),
                 inp(20, 128), perf(120))["segments"][0]
        self.assertEqual([(p["frame"], p["mask"]) for p in s["events"]], [(0, 0), (20, 128)])
        self.assertEqual(s["duplicate_frames"], [20])
        self.assertEqual(len(s["ignored_ui_inputs"]), 1)
        self.assertTrue(s["replayable"])

    def test_late_conflicting_duplicate_holds(self):
        """Смена уже завершённого кадра не исправляется задним числом."""
        s = bank(start(), inp(0, 0), inp(20, 16), perf(120),
                 inp(20, 128), perf(240))["segments"][0]
        self.assertFalse(s["replayable"])
        self.assertIn("conflicting_duplicate_after_completed_PERF", s["hold_reasons"])

    def test_missing_start_or_frame_zero_is_hold(self):
        """Подключение в середине игры не становится свежим movie по догадке."""
        for result in (bank(inp(0, 0), perf(120)), bank(start(), inp(5, 128), perf(120))):
            self.assertEqual(result["segments"][0]["status"], "HOLD")

    def test_repeated_launch_variants_and_failed_next_launch(self):
        """Одинаковый ROM запускается отдельными вариантами; новый load fail не fault старого."""
        result = bank(start(), inp(0, 0), perf(120), "ROM_LOAD_FAIL index=4 mapper=85 reason=unsupported",
                      start(), inp(0, 128, 6), perf(240, 6))
        self.assertEqual([s["variant_id"] for s in result["segments"]],
                         ["1234abcd-v01", "1234abcd-v02"])
        self.assertTrue(all(s["replayable"] for s in result["segments"]))
        self.assertEqual(len(result["excluded_launch_attempts"]), 1)

    def test_known_mode_new_generation_frame_zero_vs_unmarked_state_carry(self):
        """Fresh-load inference требует MODE/new generation/frame0; warm state не угадывается."""
        mode = "NES_OUTPUT_FRAME_PIPELINE_MODE mapper=4 producer=timing_fetch"
        result = bank(start(), inp(0, 0), perf(120), mode, inp(0, 128, 6), perf(120, 6))
        self.assertEqual(result["segments"][1]["start_reason"], "known_fresh_reload_candidate")
        self.assertTrue(result["segments"][1]["replayable"])
        result = bank(start(), inp(0, 0), perf(120), inp(0, 128, 6), perf(120, 6))
        self.assertEqual(result["segments"][1]["status"], "HOLD")

    def test_reorder_and_partial_eof_remain_explicit(self):
        """Потерянный хвост и обратный порядок не маскируются сортировкой."""
        s = bank(start(), inp(0, 0), inp(30, 128), inp(20, 0), perf(120))["segments"][0]
        self.assertIn("input_frame_reordered_or_unmarked_reset", s["hold_reasons"])
        result = build_bank((start() + "\n" + inp(0, 0) + "\n" + perf(120)).encode())
        self.assertEqual(result["parse_issues"][0]["reason"], "incomplete_EOF_line_ignored")
        self.assertEqual(result["segments"][0]["status"], "HOLD")

    def test_actual_window_alignment_and_short_coverage(self):
        """ROI остаётся на фазе1530+N120, а короткая запись не растягивается до1920."""
        s = bank(start(), inp(0, 128), perf(2400))["segments"][0]
        windows = []
        for i in range(3):
            begin = 1530 + i * 120
            windows.append(dict(input_first=begin, input_last=begin+119,
                accepted_first=begin+1, accepted_last=begin+120, elapsed_us=2000000,
                ambiguous_gap=False, counter_anomalies=[], line_first=10+i, line_last=11+i))
        analysis = {"sessions": [{"identity": {"crc32": s["crc32"]}, "generation": 3,
                                 "start_line": 1, "windows": windows}]}
        roi = select_rois(s, analysis)[0]
        self.assertEqual(roi["baseline_report"], 1530)
        self.assertEqual(roi["roi_reports"], [1650, 1770, 1890])
        self.assertEqual(roi["window_count"], 3)
        self.assertTrue(roi["short_coverage"])

    def test_bank_refuses_rewriting_baseline(self):
        """Durable input baseline не перезаписывается другим содержимым."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bank.json"
            result = bank(start(), inp(0, 0), perf(120))
            write_bank(result, path)
            write_bank(result, path)
            with self.assertRaises(FileExistsError):
                write_bank(bank(start(), inp(0, 128), perf(120)), path)


if __name__ == "__main__":
    unittest.main()

