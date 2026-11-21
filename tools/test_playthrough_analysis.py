"""Проверки арифметики и жизненного цикла пассивного захвата без UART/SDK."""
import csv
import json
from pathlib import Path
import tempfile
import unittest
from analyze_playthrough import analyze_text, parse_line, write_outputs, main


def start(crc="7cf70cde", name="Castlevania III Dracula's Curse", index=3, mapper=5, region="NTSC"):
    """Строит реальный GAME_START с пробелами в name и явным CRC."""
    return f"I (100) nes_app: NES_GAME_START index={index} name={name} mapper={mapper} region={region} crc={crc} source=sd zip=1 size=262144"


def perf(frame, ms=None, us=None, gen=7, mapper=5, region="NTSC", produced=None, submitted=None, coupled=False, core=15000):
    """Задаёт независимые счётчики и именно device clock для ожидаемой арифметики."""
    tag = "NES_COUPLED_BENCH" if coupled else "NES_OUTPUT_FRAME_PIPELINE_TIMES"
    prefix = f"I ({ms}) nes_app: " if ms is not None else "2026-11-18T20:00:00 HOST 999999.0 "
    body = f"{tag} game=3 frames={frame} produced_frames={frame if produced is None else produced} submitted_frames={frame if submitted is None else submitted} samples=120 generation={gen} mapper={mapper} region={region} producer_wall_us={core} consumer_active_us=6000 dsp_us=400 max_producer_wall_us=23000"
    return prefix + body + (f" device_us={us}" if us is not None else "")


class PassiveAnalysisTests(unittest.TestCase):
    """Сценарии проверяют потерю данных, паузу и reset, а не только структуру вывода."""

    def test_weighted_average_separate_counters_title_and_export(self):
        """Два разных периода дают N/time; среднее FPS по строкам было бы неверным."""
        raw = "\n".join([start(), "\x1b[32m" + perf(120, 1000) + "\x1b[0m",
                         perf(240, 3000, produced=241, submitted=239, core=12000),
                         perf(360, 7000, produced=360, submitted=360, core=20000)])
        result = analyze_text(raw, "p4")
        s = result["sessions"][0]
        self.assertEqual(s["identity"]["title"], "Castlevania III Dracula's Curse")
        self.assertEqual(s["identity"]["crc32"], "7cf70cde")
        self.assertEqual(s["generation"], 7)
        self.assertEqual(s["summary"]["windows"], 2)
        self.assertEqual(s["summary"]["accepted"]["mean_fps"], 40.0)
        self.assertEqual(s["summary"]["accepted"]["min_window_fps"], 30.0)
        self.assertEqual(s["summary"]["accepted"]["max_window_fps"], 60.0)
        self.assertEqual(s["windows"][0]["produced_delta"], 121)
        self.assertEqual(s["windows"][0]["submitted_delta"], 119)
        self.assertEqual(s["summary"]["produced"]["mean_fps"], 40.0)
        self.assertEqual(s["summary"]["stage_means_us"]["producer_wall_us"], 16000)
        self.assertEqual(s["summary"]["stage_maxima_us"]["max_producer_wall_us"], 23000)
        with tempfile.TemporaryDirectory() as tmp:
            write_outputs(result, tmp)
            saved = json.loads((Path(tmp) / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(saved["sessions"][0]["summary"]["accepted_frames"], 240)
            with (Path(tmp) / "windows120.csv").open(encoding="utf-8", newline="") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["input_first"], "120")

    def test_pause_request_same_generation_and_ambiguous_gap_preserved(self):
        """Exit открывает подтверждение; после Cancel та же ROM/gen, длинный gap не скрывается."""
        raw = "\n".join([start(), perf(120, 1000), perf(240, 3000),
                         "NES_INPUT generation=7 frame=241 applied=0 action=exit",
                         perf(360, 63000), perf(480, 65000), perf(600, 95000)])
        s = analyze_text(raw)["sessions"][0]
        self.assertEqual(s["summary"]["windows"], 3)
        self.assertEqual(s["summary"]["accepted"]["mean_fps"], 360e6 / 34000000)
        self.assertEqual(s["all_observed_windows"]["accepted"]["min_window_fps"], 4.0)
        self.assertEqual(s["trusted_unambiguous"]["windows"], 2)
        self.assertEqual(s["trusted_unambiguous"]["accepted"]["mean_fps"], 60.0)
        self.assertEqual(s["identity"]["crc32"], "7cf70cde")
        self.assertTrue(s["windows"][-1]["ambiguous_gap"])
        self.assertIn("input_action_exit", [g["reason"] for g in s["discontinuities"]])

    def test_repeated_start_generation_region_counter_and_boot_boundaries(self):
        """Повторный запуск того же CRC, pause-rebase, PAL/reset и boot не смешивают часы."""
        raw = "\n".join([start(), perf(120, 1000), perf(240, 3000),
                         start(), perf(120, 5000, gen=9), perf(240, 7000, gen=9),
                         perf(360, 9000, gen=10), perf(480, 11000, gen=10),
                         perf(600, 13000, gen=10, region="PAL"), perf(720, 15400, gen=10, region="PAL"),
                         perf(120, 18000, gen=10, region="PAL"), perf(240, 20400, gen=10, region="PAL"),
                         "ESP-ROM:esp32p4-20240419", perf(120, 1000), perf(240, 3000)])
        sessions = analyze_text(raw)["sessions"]
        self.assertEqual([s["start_reason"] for s in sessions], ["GAME_START", "GAME_START", "generation_change", "identity_or_region_change", "frame_counter_reset", "capture_mid_game"])
        self.assertEqual([s["summary"]["windows"] for s in sessions], [1] * 6)
        self.assertEqual(sessions[3]["summary"]["accepted"]["mean_fps"], 50.0)
        self.assertIsNone(sessions[-1]["identity"]["crc32"])
        self.assertIsNone(sessions[-1]["identity"]["title"])

    def test_no_host_clock_missing_report_and_nonmonotonic_clock(self):
        """Host timestamp не заменяет device; потеря отчёта и откат часов явно видны."""
        unknown = analyze_text("\n".join([perf(120), perf(240)]))["sessions"][0]
        self.assertEqual(unknown["summary"]["windows"], 0)
        self.assertIsNone(unknown["summary"]["accepted"]["mean_fps"])
        self.assertIsNone(unknown["identity"]["crc32"])
        raw = "\n".join([start(), perf(120, 1000), perf(360, 5000), perf(480, 7000),
                         perf(600, 7000), perf(720, 9000), perf(840, 500), perf(960, 2500)])
        result = analyze_text(raw)
        self.assertEqual(result["sessions"][0]["summary"]["windows"], 2)
        self.assertEqual(result["sessions"][1]["start_reason"], "device_clock_reset")
        self.assertIsNone(result["sessions"][1]["identity"]["crc32"])
        reasons = [g["reason"] for g in result["sessions"][0]["discontinuities"]]
        self.assertIn("missing_or_incomplete_120_frame_report", reasons)
        self.assertIn("device_clock_not_increasing", reasons)

    def test_c6_us_wins_input_activity_nested_telemetry_and_counter_reset(self):
        """Actual device_us сильнее logger-ms; stage/held не суммируются в выдуманный frame time."""
        raw = "\n".join([start(mapper=5), perf(120, ms=1000, us=1000000, coupled=True),
                         "NES_INPUT generation=7 frame=125 applied=129 action=none",
                         "NES_INPUT generation=7 frame=200 applied=128 action=none",
                         perf(240, ms=99000, us=3000000, coupled=True, submitted=100),
                         "NES_C6_AUDIO_WINDOW game=3 guest_frames=240 stage_mean_us=22 held_mean_us=0 pump_calls=700",
                         "NES_FRAME_AUDIO_STATUS max_gap_us=103625 missed_blocks=50"])
        s = analyze_text(raw, "c6")["sessions"][0]
        w = s["windows"][0]
        self.assertEqual(w["clock"], "device_us")
        self.assertEqual(w["accepted_fps"], 60)
        self.assertEqual(w["backend"], "C6Coupled")
        self.assertIsNone(w["submitted_delta"])
        self.assertIsNone(s["summary"]["submitted"]["mean_fps"])
        self.assertEqual(w["activity"]["logged_changes"], 2)
        self.assertFalse(w["activity"]["scene_verified"])
        self.assertEqual(len(w["telemetry"]), 1)
        self.assertFalse(w["observed_stage"]["causal_proof"])
        self.assertEqual(s["audio"]["max_gap_us"], 103625)
        self.assertIn("not_proven_session_only", s["audio"]["scope"])

    def test_matched_loop_flags_short_ambiguous_checkpoint_gap(self):
        """Точный LOOP120 выявляет короткую неоднозначность; maxLoop не становится FPS."""
        raw = "\n".join([start(), perf(120, 1000), perf(240, 5000),
                         "NES_FRAME_LOOP_METRICS generation=7 accepted_first=121 accepted_last=240 completed_main_iterations=120 loop_wall_us=16600 loop_wall_max_us=85000 loop_wall_max_accepted_frame=204"])
        s = analyze_text(raw)["sessions"][0]
        self.assertEqual(s["all_observed_windows"]["accepted"]["mean_fps"], 30)
        self.assertEqual(s["trusted_unambiguous"]["windows"], 0)
        self.assertTrue(s["windows"][0]["ambiguous_checkpoint_gap"])
        self.assertEqual(s["windows"][0]["checkpoint_minus_matched_loop_us"], 2008000)

    def test_active_capture_partial_eof_marks_and_failed_load_identity(self):
        """Активный EOF игнорируется; PC marks не меняют часы и failed load теряет старый CRC."""
        with tempfile.TemporaryDirectory() as tmp:
            cap = Path(tmp) / "capture"
            cap.mkdir()
            complete = "\n".join([start(), perf(120, 1000), perf(240, 3000)]) + "\n"
            tail = perf(360, 5000)
            (cap / "raw.log").write_text(complete + tail, encoding="utf-8")
            (cap / "marks.jsonl").write_text('{"byte_offset":0,"note":"human level start","host_time":99999999999}\n{bad\n', encoding="utf-8")
            out = Path(tmp) / "snapshot"
            self.assertEqual(main([str(cap), "--output", str(out), "--profile", "p4"]), 0)
            result = json.loads((out / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(result["sessions"][0]["summary"]["accepted"]["mean_fps"], 60)
            self.assertEqual(result["source"]["incomplete_eof_bytes_ignored"], len(tail.encode("utf-8")))
            self.assertTrue(result["sessions"][0]["open_at_capture_end"])
            self.assertFalse(result["sessions"][0]["game_completion_inferred"])
            self.assertEqual(len(result["manual_marks"]), 1)
            self.assertEqual(len(result["marks_source"]["errors"]), 1)
            self.assertFalse(result["marks_source"]["host_time_used_for_fps"])
        raw = "\n".join([start(), perf(120, 1000), "E (2000) nes_app: ROM_LOAD_FAIL index=4 mapper=85 reason=not implemented",
                         perf(120, 3000), perf(240, 5000)])
        sessions = analyze_text(raw)["sessions"]
        self.assertEqual(len(sessions), 2)
        self.assertEqual(sessions[0]["faults"], [])
        self.assertEqual(sessions[0]["end_reason"], "next_launch_ROM_LOAD_FAIL")
        self.assertIsNone(sessions[-1]["identity"]["crc32"])


if __name__ == "__main__":
    unittest.main()

