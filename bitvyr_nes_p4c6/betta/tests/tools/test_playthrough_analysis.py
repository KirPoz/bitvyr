"""HOST-проверки границ UART-анализа без устройства и эмулятора."""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location("playthrough_analysis", Path(__file__).resolve().parents[2] / "tools/analyze_playthrough.py")
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


def event(tag, fields, ms=None, *, kind=None, host=1):
    """Создаёт capture envelope с явным раздельным host/device clock."""
    record = dict(kind=kind or tag.removeprefix("NES_").lower(), raw_type=tag, fields=fields)
    if ms is not None:
        record["device_timestamp_ms"] = ms
    return dict(schema=1, host_utc="2026-11-03T12:00:00Z", host_monotonic_ns=host,
                serial_line="", record=record)


def times(frame, ms, *, generation=1, game=0, samples=120, submitted=None, produced=None, host=1):
    """TIMES содержит stage means, но сам не содержит FPS/elapsed."""
    fields = dict(game=game, generation=generation, frames=frame, samples=samples,
                  region="NTSC",
                  submitted_frames=frame if submitted is None else submitted,
                  produced_frames=frame if produced is None else produced,
                  core_us=20000, producer_wall_us=20500, consumer_active_us=6700, dsp_us=1700)
    return event("NES_OUTPUT_FRAME_PIPELINE_TIMES", fields, ms, host=host)


def marker(phase="note", text="уровень неизвестен"):
    """Ручная заметка не получает выдуманную координату/номер уровня."""
    return event("", dict(phase=phase, text=text), kind="marker")


class PlaythroughAnalysisTests(unittest.TestCase):
    """Проверяет разделение scope и невозможность получить FPS из неподходящих данных."""
    def setUp(self):
        """Все входы/выходы находятся в новом временном каталоге."""
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.session = Path(self.temp.name) / "session"
        self.session.mkdir()

    def run_events(self, rows, tail=b"", **options):
        """Пишет synthetic envelopes без запуска firmware или serial transport."""
        with (self.session / "events.jsonl").open("wb") as file:
            for row in rows:
                file.write(json.dumps(row, ensure_ascii=False).encode("utf-8") + b"\n")
            file.write(tail)
        return analysis.analyze(self.session, **options)

    def csv(self, name):
        """Читает малый fixture CSV, а не все окна реального прохождения."""
        with (self.session / "report" / (name + ".csv")).open(encoding="utf-8", newline="") as file:
            return list(csv.DictReader(file))

    def test_observational_rate_and_launch_identity_binding(self):
        """Первые TIMES — boundary, CRC/name переживают первое присоединение generation."""
        rows = [event("NES_GAME_START", dict(index=0, name="Game = 'test'", crc="00451234", region="NTSC")),
                times(120, 1000), times(240, 3560), marker("dip", "бой")]
        summary = self.run_events(rows)
        windows = self.csv("windows")
        self.assertEqual(len(windows), 1)
        self.assertEqual(windows[0]["quality"], "observational_report_boundaries")
        self.assertAlmostEqual(float(windows[0]["fps"]), 46.875)
        self.assertEqual(windows[0]["crc32"], "00451234")
        self.assertEqual(windows[0]["rom_name"], "Game = 'test'")
        self.assertEqual(windows[0]["accepted_first"], "121")
        self.assertEqual(windows[0]["classification"], "severe")
        self.assertEqual(summary["counts"]["first_boundaries_not_windows"], 1)
        self.assertEqual(self.csv("timeline")[-1]["known_accepted_last"], "240")
        self.assertEqual(self.csv("timeline")[-1]["phase"], "dip")

    def test_host_receipt_is_not_fps_and_missing_device_unknown(self):
        """Огромный host gap не меняет device rate, отсутствующий device clock не подменяется host."""
        self.run_events([times(120, 1000, host=1), times(240, 3000, host=10**15),
                         times(360, None, host=10**15 + 1), times(480, 7000, host=10**15 + 2)])
        self.assertEqual(len(self.csv("windows")), 1)
        self.assertEqual(float(self.csv("windows")[0]["fps"]), 60)

    def test_duplicate_and_regressed_frame_not_counted(self):
        """Повтор boundary и поздний меньший frame не создают дополнительные окна."""
        summary = self.run_events([times(120, 1000), times(240, 3000), times(240, 3000),
                                   times(180, 4000), times(360, 6000)])
        self.assertEqual(len(self.csv("windows")), 1)
        self.assertEqual(summary["counts"]["duplicate_records"], 1)
        self.assertEqual(summary["counts"]["rejected_boundary_intervals"], 2)

    def test_generation_and_same_generation_game_conflict(self):
        """Нельзя соединить разные поколения или конфликтующие game в одном generation."""
        summary = self.run_events([times(120, 1000), times(240, 3000),
            times(120, 4000, generation=4, game=2), times(240, 6000, generation=4, game=2),
            times(360, 8000, generation=4, game=3), times(360, 9000, generation=1)])
        self.assertEqual(len(self.csv("windows")), 2)
        self.assertEqual(summary["counts"]["scope_conflicts"], 1)
        self.assertEqual(summary["counts"]["stale_generation_records"], 1)
        self.assertNotEqual(*[row["segment"] for row in self.csv("windows")])

    def test_partial_missing_and_large_gap_rejected(self):
        """Неполный sample interval, пропущенный отчёт и паузоподобный gap не являются FPS-окном."""
        summary = self.run_events([times(120, 1000), times(240, 3000, samples=60),
            times(360, 5000), times(600, 7000), times(720, 27000), times(840, 29000)], tail=b'{"schema":1')
        self.assertEqual(len(self.csv("windows")), 1)
        self.assertEqual(summary["counts"]["partial_final_lines"], 1)
        self.assertEqual(summary["counts"]["incomplete_times_boundaries"], 1)
        self.assertEqual(summary["counts"]["rejected_boundary_intervals"], 2)

    def test_pause_observer_and_reset_invalidate_adjacent_intervals(self):
        """После observer/resume/reset требуется новая первая граница, а не мост через паузу."""
        rows = [times(120, 1000), marker("pause"), times(240, 3000), marker("resume"),
            times(360, 5000), times(480, 7000), event("", {}, kind="observer_begin"),
            times(600, 9000), event("", {}, kind="observer_end"), times(720, 11000),
            times(840, 13000), event("NES_OUTPUT_FRAME_SETUP", {}), times(960, 15000), times(1080, 17000)]
        self.run_events(rows)
        self.assertEqual([int(row["accepted_last"]) for row in self.csv("windows")], [480, 1080])

    def test_exact_range_join_and_loop_stall_separate(self):
        """Совпадает только точный accepted-range; loop max не превращается в per-frame FPS."""
        loop = dict(generation=1, accepted_first=1, accepted_last=120, completed_main_iterations=120,
                    loop_wall_us=21000, loop_wall_max_us=75000, loop_wall_max_accepted_frame=37)
        self.run_events([times(120, 1000), event("NES_FRAME_LOOP_METRICS", loop),
                         times(240, 3000), event("NES_FRAME_LOOP_METRICS", dict(loop, accepted_first=122, accepted_last=241))])
        self.assertEqual(len(self.csv("joins")), 1)
        self.assertEqual(len(self.csv("windows")), 1)
        stalls = [row for row in self.csv("dips") if row["dip_kind"] == "main_loop_stall"]
        self.assertEqual(len(stalls), 2)
        self.assertEqual(stalls[0]["observed_accepted_frame"], "37")
        self.assertEqual(stalls[0]["fps"], "")

    def test_audio_delta_reset_and_observer_preservation(self):
        """Board PCM counters могут сброситься в том же generation; отрицательные дельты не выводятся."""
        def audio(n, missed):
            """Это PCM frames, не число NES-кадров."""
            return event("NES_FRAME_AUDIO_STATUS", dict(generation=1, accepted_frames=n, dma_frames=n,
                missed_refill_blocks=missed, missed_refill_frames=missed * 128, max_write_gap_us=100))
        self.run_events([times(120, 1000), audio(1000, 2), event("", {}, kind="observer_begin"),
            audio(2000, 3), event("", {}, kind="observer_end"), audio(10, 0), audio(20, 1)])
        rows = self.csv("audio")
        self.assertEqual(json.loads(rows[1]["delta_json"])["missed_blocks"], 1)
        self.assertEqual(rows[2]["quality"], "counter_reset_or_wrap_unknown")
        self.assertEqual(json.loads(rows[2]["delta_json"]), {})
        self.assertEqual(json.loads(rows[3]["delta_json"])["audio_accepted_frames"], 10)

    def test_resources_preserved_without_cpu_utilization(self):
        """Task no-affinity и heap остаются опубликованными полями с units, не придуманным PPU%."""
        self.run_events([event("NES_TASK", dict(id=5, name="worker", core=2147483647, run_us=800, stack_min=7000)),
                         event("NES_HEAP", dict(minimum_internal=80000, largest_internal=40000))])
        rows = self.csv("resources")
        self.assertEqual(len(rows), 2)
        self.assertEqual(json.loads(rows[0]["fields_json"])["core"], 2147483647)
        self.assertNotIn("cpu_utilization", rows[0])
        self.assertEqual(self.csv("windows"), [])

    def test_boot_index_reuse_and_real_perf_quality(self):
        """Game index повторяется между boot; PERF firmware-rate не смешивается с Output boundary-rate."""
        rows = [event("NES_GAME_START", dict(index=0, name="A", crc="aaaaaaaa")),
                times(120, 1000), times(240, 3000), event("NES_RUNTIME_READY", {}),
                event("NES_GAME_START", dict(index=0, name="B", crc="bbbbbbbb")),
                event("NES_PERF", dict(game=0, frames=120, fps=59.5)),
                event("NES_PERF", dict(game=0, frames=240, fps=58.0))]
        summary = self.run_events(rows)
        windows = self.csv("windows")
        self.assertEqual([row["crc32"] for row in windows], ["aaaaaaaa", "bbbbbbbb"])
        self.assertEqual(windows[1]["quality"], "firmware_reported_perf_average")
        self.assertEqual(len(summary["roms"]), 2)

    def test_output_is_new_and_thresholds_validated(self):
        """Нельзя перезаписать даже пустой existing report; NaN threshold отвергается."""
        self.run_events([])
        with self.assertRaises(FileExistsError):
            analysis.analyze(self.session)
        with self.assertRaises(ValueError):
            analysis.analyze(self.session, Path(self.temp.name) / "nan", dip_fps=float("nan"))

    def test_stream_many_windows_and_out_of_order_host(self):
        """Поток больших серий не требует массива окон; host reorder разрывает rate."""
        with (self.session / "events.jsonl").open("w", encoding="utf-8") as file:
            for i in range(1, 1501):
                file.write(json.dumps(times(i * 120, i * 2000, host=i)) + "\n")
            file.write(json.dumps(times(180120, 3002000, host=1)) + "\n")
        summary = analysis.analyze(self.session)
        self.assertEqual(summary["counts"]["windows"], 1499)
        self.assertEqual(summary["counts"]["out_of_order_host_events"], 1)
        self.assertEqual(summary["bounded_memory"]["recent_records"], analysis.MAX_RECENT)

    def test_observer_late_responses_quarantine_and_missing_response_pair(self):
        """Host commands_sent не означает completion; поздний response заново таинтит границы."""
        rows = [times(120, 1000), event("", {}, kind="observer_begin"),
                event("", dict(scope="commands_sent_not_device_completion"), kind="observer_end"),
                times(240, 3000), event("NES_HEAP", dict(minimum_internal=80000)),
                times(360, 5000), times(480, 7000), times(600, 9000), times(720, 11000)]
        summary = self.run_events(rows)
        self.assertEqual([row["accepted_last"] for row in self.csv("windows")], ["720"])
        self.assertEqual(summary["counts"]["observer_quarantine_boundaries"], 3)

    def test_task_complete_snapshots_id_name_reset_and_minimum_semantics(self):
        """Runtime вычисляется из полных соседних snapshots; reused ID/name и reset дают unknown."""
        rows = []
        for total, name, run in [(1000, "worker", 400), (2000, "worker", 900),
                                  (3000, "replacement", 1000), (4000, "replacement", 50)]:
            rows += [event("NES_TASKS", dict(total_us=total, count=1)),
                     event("NES_TASK", dict(id=9, name=name, run_us=run, core=2147483647, stack_min=7000))]
        summary = self.run_events(rows)
        tasks = self.csv("task_runtime")
        self.assertEqual(float(tasks[1]["task_runtime_pct_of_wall"]), 50)
        self.assertEqual(tasks[1]["core_label"], "no_affinity")
        self.assertEqual(tasks[1]["stack_min_bytes"], "7000")
        self.assertEqual(tasks[2]["task_runtime_pct_of_wall"], "")
        self.assertEqual(tasks[3]["quality"], "task_counter_reset_or_wrap_unknown")
        self.assertEqual(summary["counts"]["complete_task_snapshots"], 4)

    def test_capture_partial_completion_and_unobserved_expected_elf(self):
        """Declared expected ELF без boot остаётся непроверенным; disconnect делает session partial."""
        (self.session / "session.json").write_text(json.dumps(dict(transport="replay", expected_elf="a" * 64)), encoding="utf-8")
        (self.session / "status.json").write_text(json.dumps(dict(phase="closed", reason="disconnect", exit_code=1)), encoding="utf-8")
        summary = self.run_events([times(120, 1000), times(240, 3000),
            event("HOST_CAPTURE", dict(reason="disconnect", exit_code=1, trailing_incomplete_line=False), kind="capture_end")])
        self.assertEqual(summary["capture"]["capture_scope"], "partial_or_unknown_capture")
        self.assertEqual(summary["capture"]["firmware_identity_quality"], "not_observed_unknown")
        self.assertFalse(summary["capture"]["expected_elf_is_observed"])
        self.assertEqual(summary["capture"]["transport"], "replay")

    def test_bounded_discard_oversized_and_malformed_jsonl(self):
        """Большая повреждённая строка пропускается чанками, затем допустимый JSONL снова читается."""
        with (self.session / "events.jsonl").open("wb") as file:
            file.write(b"x" * (analysis.MAX_EVENT_BYTES * 3) + b"\n")
            file.write(b"{bad}\n")
            file.write(json.dumps(times(120, 1000)).encode() + b"\n")
            file.write(json.dumps(times(240, 3000)).encode() + b"\n")
        summary = analysis.analyze(self.session)
        self.assertEqual(summary["counts"]["oversized_events"], 1)
        self.assertEqual(summary["counts"]["malformed_jsonl"], 1)
        self.assertEqual(summary["counts"]["windows"], 1)

    def test_parser_integration_from_actual_wire_shape(self):
        """Fallback к общему parser читает полный actual TIMES wire, не выдуманный FPS tag."""
        rows = []
        for frame, ms in [(120, 1000), (240, 3000)]:
            envelope = times(frame, ms)
            fields = envelope["record"]["fields"]
            fields.update(mapper=4, seal_us=1, audio_retire_us=2, snapshot_us=3, witness_copy_us=4,
                rgb_copy_us=0, present_us=100, wait_us=10, audio_us=20, slots=2, schema=2,
                producer="timing_fetch", consumer="output_compositor", read_witness="unavailable",
                effective_tail="off", game_fps_claim=0)
            envelope["record"] = None
            envelope["serial_line"] = (f"I ({ms}) nes_app: NES_OUTPUT_FRAME_PIPELINE_TIMES "
                                       + " ".join(f"{key}={value}" for key, value in fields.items()))
            rows.append(envelope)
        summary = self.run_events(rows)
        self.assertEqual(summary["counts"]["windows"], 1)
        self.assertEqual(float(self.csv("windows")[0]["fps"]), 60)

    def test_bounded_worst_rows_stage_pressure_and_late_exact_loop(self):
        """Worst heap ограничен10; поздний LOOP присоединяется только к совпавшему диапазону."""
        rows = [times(i * 120, i * 2600) for i in range(1, 18)]
        rows += [event("NES_FRAME_LOOP_METRICS", dict(generation=1, accepted_first=121, accepted_last=240,
                 loop_wall_max_us=80000, loop_wall_max_accepted_frame=150))]
        summary = self.run_events(rows)
        self.assertEqual(len(summary["worst_windows"]), 10)
        first = summary["worst_windows"][0]
        self.assertEqual(first["core_us"], 20000)
        self.assertEqual(first["producer_wall_us"], 20500)
        self.assertEqual(first["observed_pressure"], "mean_C1_producer_exceeds_region_budget")
        self.assertEqual(first["loop_wall_max_accepted_frame"], 150)
        self.assertEqual(first["loop_join_quality"], "exact_accepted_range_independent_metric")
        self.assertIn("Худшие средние окна", (self.session / "report/REPORT.md").read_text(encoding="utf-8"))

    def test_unknown_identity_keeps_generations_separate(self):
        """Без CRC/name одинаковый game index в двух scope не становится одной ROM."""
        summary = self.run_events([times(120, 1000), times(240, 3000),
            times(120, 4000, generation=4), times(240, 6000, generation=4)])
        self.assertEqual(len(summary["roms"]), 2)
        self.assertNotEqual(summary["roms"][0]["identity"], summary["roms"][1]["identity"])

    def test_task_resource_names_and_crc_never_replace_rom_identity(self):
        """TIMES→RTOS tasks→status→TIMES сохраняет ROM name либо unknown, а не rpc_tx."""
        for known in (True, False):
            with self.subTest(known_rom=known):
                self.session = Path(self.temp.name) / ("known" if known else "unknown")
                self.session.mkdir()
                rows = [event("NES_GAME_START", dict(index=5, name="Chip 'n Dale 2", crc="91e4a289"))] if known else []
                rows += [times(120, 1000, game=5),
                         event("NES_TASK", dict(id=9, name="rpc_tx", run_us=100, core=0, stack_min=4000)),
                         event("NES_HEAP", dict(name="heap_resource", crc32="ffffffff", region="PAL", minimum_internal=80000)),
                         event("NES_OUTPUT_FRAME_PIPELINE_STATUS", dict(generation=1, game=5, frames=120,
                             playing=True, crc32="91e4a289")),
                         times(240, 3000, game=5)]
                summary = self.run_events(rows)
                expected = "Chip 'n Dale 2" if known else ""
                last = self.csv("metrics")[-1]
                self.assertEqual(last["rom_name"], expected)
                self.assertEqual(last["crc32"], "91e4a289")
                self.assertEqual(last["region"], "NTSC")
                resources = self.csv("resources")
                self.assertTrue(all(row["rom_name"] == expected for row in resources))
                self.assertTrue(all(row["region"] == "NTSC" for row in resources))
                self.assertEqual(json.loads(resources[0]["fields_json"])["name"], "rpc_tx")
                self.assertEqual(summary["counts"].get("scope_conflicts", 0), 0)


if __name__ == "__main__":
    unittest.main()
