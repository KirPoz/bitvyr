"""Шесть независимых synthetic fixtures: таймеры, weighting, ROI, scope и C6 nesting."""
import unittest
from c6_game_statistics import statistics


def frame(frames,us,**extra):
    """Создаёт один синтетический actual FW boundary, а не эмуляцию алгоритма."""
    return dict(raw_type="NES_OUTPUT_FRAME_PIPELINE_TIMES",game=0,generation=7,mapper=0,
                region="NTSC",frames=frames,produced_frames=frames,submitted_frames=frames,
                samples=120,device_us=us,producer_wall_us=12000,consumer_active_us=8000,**extra)


class Fixtures(unittest.TestCase):
    """Проверяет свойства результатов на заданных числовых примерах без COM/SDK."""
    def test_01_time_weighted_average(self):
        """120/2s и120/4s дают40FPS, минимум окна30, а не45FPS."""
        rows=[frame(120,2000000),frame(240,4000000),frame(360,8000000)]
        result=statistics(rows,{})["whole_route"]
        self.assertEqual(result["mean_fps"],40)
        self.assertEqual(result["min_window_fps"],30)
        self.assertEqual(result["max_window_fps"],60)
        self.assertEqual(result["frames"],240)
        self.assertIsNone(result["true_per_frame_min_fps"])

    def test_02_m5_pending_c6_overlap(self):
        """M5 pending не скрывается; remote stage/held и producer не складываются."""
        rows=[]
        for f,t,s in [(120,2000000,119),(240,4000000,238)]:
            rows.append(dict(raw_type="NES_COUPLED_BENCH",game=1,generation=8,mapper=5,region="NTSC",
                frames=f,accepted_frames=f,produced_frames=f,submitted_frames=s,samples=120,device_us=t,
                producer_wall_us=16000,max_producer_wall_us=27000,consumer_active_us=8000,
                read_wall_us=7000,commit_wall_us=3000,prefix_wall_us=1000))
        rows += [dict(raw_type="NES_C6_AUDIO_WINDOW",game=1,guest_frames=240,stage_mean_us=2000,
                     held_mean_us=1500,stage_frames=120,pump_cycles=10000,pump_cycle_hz=100000000),
                 dict(raw_type="NES_COUPLED_REPORT_ENDPOINT",game=1,frames=240,device_us=4000000,pending_depth=1)]
        result=statistics(rows,{})["whole_route"]
        self.assertEqual(result["mean_fps"],60)
        self.assertEqual(result["mean_submitted_fps"],59.5)
        self.assertEqual(result["counter_anomaly_windows"],1)
        worst=result["worst3_windows"][0]
        self.assertEqual(worst["endpoint"]["pending_depth"],1)
        self.assertAlmostEqual(worst["c6"]["pump_cycles_as_us"],100)
        self.assertEqual(result["stage_maxima_us"]["max_producer_wall_us"],27000)
        self.assertIn("handshakes",worst["nested_overlap"])

    def test_03_generation_and_missing_reports(self):
        """Другой generation и пропавший120-кадровый boundary не дают ложного FPS."""
        rows=[frame(120,2000000),frame(240,4000000),frame(480,8000000)]
        changed=frame(600,10000000);changed["generation"]=9
        rows.append(changed)
        result=statistics(rows,{"baseline_report":120,"roi_reports":[240,360,480],"roi_input0":[120,479]})
        self.assertEqual(result["whole_route"]["windows"],1)
        self.assertEqual(result["missing_roi_endpoints"],[360])
        self.assertEqual(len(result["discontinuities"]),2)

    def test_04_host_clock_never_fps(self):
        """Произвольный HOST receipt иrx_offset не изменяют device-clock rate."""
        rows=[frame(120,2000000),frame(240,4000000)]
        for i,row in enumerate(rows):
            row.update(host_monotonic_ns=10**18+i,rx_offset=10**12-i,line=i+1)
        self.assertEqual(statistics(rows,{})["whole_route"]["mean_fps"],60)
        mixed=rows+[dict(raw_type=r["raw_type"],fields=r,line=r["line"],rx_offset=r["rx_offset"]) for r in rows]
        self.assertEqual(statistics(mixed,{})["whole_route"]["windows"],1)
        del rows[1]["device_us"]
        result=statistics(rows,{})
        self.assertEqual(result["whole_route"]["status"],"not_measured")
        self.assertIsNone(result["whole_route"]["mean_fps"])

    def test_05_exact_input_roi_and_scene(self):
        """completed frame361 соответствуетinput360; ROI включается только целиком."""
        rows=[frame(120,2000000),frame(240,4000000),frame(360,8000000),frame(480,10000000)]
        movie=dict(baseline_report=240,roi_reports=[360],roi_input0=[240,359],
                   edges=[dict(frame=250,mask=128)],scenes=[dict(name="declared level",input0=[240,359])])
        result=statistics(rows,movie)
        self.assertEqual(result["roi"]["mean_fps"],30)
        self.assertEqual(result["roi"]["input_first"],240)
        self.assertEqual(result["roi"]["input_last"],359)
        self.assertFalse(result["scene_verified"])
        self.assertEqual(result["scenes"][0]["windows"],1)
        self.assertEqual(result["roi"]["worst3_windows"][0]["nearby_input_edges"][0]["frame"],250)

    def test_roi_range_requires_device_clock_and_exact_coverage(self):
        """Неполный участок не получает статус даже при хорошем среднем."""
        rows = [frame(120, 2000000), frame(240, 4000000), frame(360, 6000000)]
        movie = dict(roi_input0=[120, 359])
        result = statistics(rows, movie)["roi_fps_range"]
        self.assertEqual(result["status"], "in_range")
        self.assertEqual(result["expected_windows"], 2)
        self.assertEqual(statistics(rows[:-1], movie)["roi_fps_range"]["status"], "incomplete")
        rows[-1]["device_us"] = 6020000
        result = statistics(rows, movie)["roi_fps_range"]
        self.assertEqual(result["status"], "out_of_range")
        self.assertEqual(result["accepted_outside"][0]["input_first"], 240)
        rows[-1]["device_us"] = 5995000
        rows[-1]["produced_frames"] = 361
        result = statistics(rows, movie)["roi_fps_range"]
        self.assertEqual(result["status"], "publication_unverified")
        self.assertEqual(len(result["publication_anomalies"]), 1)

    def test_slowest_join_uses_components_from_same_frame(self):
        """Разбивка самого долгого join не заимствует пики других кадров."""
        rows = [frame(120, 2000000), frame(240, 4000000), frame(360, 6000000)]
        rows += [
            dict(raw_type="NES_FRAME_LOOP_METRICS", generation=7,
                 accepted_first=121, accepted_last=240, loop_wall_max_us=52000,
                 loop_wall_max_accepted_frame=172, outer_join_max_us=13000,
                 slow_join_input=229, slow_join_worker_us=12000, slow_join_previous_c6_source_us=0),
            dict(raw_type="NES_FRAME_LOOP_METRICS", generation=7,
                 accepted_first=241, accepted_last=360, loop_wall_max_us=48000,
                 loop_wall_max_accepted_frame=268, outer_join_max_us=41000,
                 slow_join_input=267, slow_join_worker_us=40000, slow_join_previous_c6_source_us=1),
        ]
        result = statistics(rows, {})["whole_route"]
        self.assertEqual(result["main_loop_max_accepted_frame"], 172)
        self.assertEqual(result["slowest_join"], dict(input0=267, outer_join_us=41000,
            worker_wait_us=40000, previous_c6_source_us=1))

    def test_hart_attaches_to_exact_input_window(self):
        """Другой индекс кадра, поколение и partial HART не участвуют в расчёте."""
        rows=[frame(120,2_000_000),frame(240,4_100_000),frame(360,6_100_000)]
        hart=dict(raw_type="NES_FRAME_HART_STATS",schema=1,scope="stepFrame_hart",
                  task_exclusive=0,game=0,generation=7,first_sequence=120,last_sequence=239,
                  samples=120,valid=120,invalid=0,overflow=0,cycles=720_000_000,
                  retired=480_000_000,cycle_max=8_000_000)
        rows += [hart,dict(hart,first_sequence=119),dict(hart,generation=8),
                 dict(hart,first_sequence=240,last_sequence=359,valid=119,invalid=1)]
        result=statistics(rows,{})["whole_route"]
        self.assertEqual(result["hart"]["covered_windows"],1)
        self.assertEqual(result["hart"]["cycles_per_guest_frame"],6_000_000)
        self.assertEqual(result["hart"]["cycles_per_retired"],1.5)
        self.assertAlmostEqual(result["worst3_windows"][0]["excess_us_per_guest_frame"],
                               17_500-1e6/59.5)
        self.assertNotIn("hart",statistics(rows+[dict(hart,retired=480_000_001)],{})["whole_route"])

    def test_frame_budget_distinguishes_core_and_overlap(self):
        """Бюджет считает дефицит кадра и отдельно наложенные этапы."""
        start = frame(120, 2_000_000)
        end = dict(frame(240, 4_280_000), core_us=18_000,
                   producer_wall_us=18_500, consumer_active_us=11_000)
        budget = statistics([start, end], {})["whole_route"]["worst3_windows"][0]["frame_budget"]
        self.assertAlmostEqual(budget["period_excess_us"], 19_000 - 1e6/59.5)
        self.assertAlmostEqual(budget["producer_excess_us"], 18_500 - 1e6/59.5)
        self.assertAlmostEqual(budget["core_excess_us"], 18_000 - 1e6/59.5)
        self.assertEqual(budget["period_minus_producer_us"], 500)
        for row in (start, end):
            row["raw_type"] = "NES_COUPLED_BENCH"
            row["mapper"] = 5
        coupled = statistics([start, end], {})["whole_route"]["worst3_windows"][0]["frame_budget"]
        self.assertIsNone(coupled["core_excess_us"])
        self.assertEqual(coupled["producer_excess_us"], budget["producer_excess_us"])
        del end["producer_wall_us"]
        unknown = statistics([start, end], {})["whole_route"]["worst3_windows"][0]["frame_budget"]
        self.assertIsNone(unknown["producer_excess_us"])
        self.assertIsNone(unknown["period_minus_producer_us"])

    def test_peak_loop_time_uses_stages_from_same_iteration(self):
        """Пик ожидания C6 относится к той же итерации, а не к максимуму окна."""
        rows=[frame(120,2_000_000),frame(240,4_000_000)]
        phase=dict(raw_type="NES_FRAME_LOOP_METRICS",generation=7,accepted_first=121,
                   accepted_last=240,loop_wall_max_us=30_000,loop_wall_max_accepted_frame=190,
                   slow_loop_join_us=9_000,slow_loop_predispatch_audio_us=8_000,
                   slow_loop_pace_us=0,slow_loop_input_us=500,slow_loop_dispatch_us=50,
                   slow_loop_consume_us=10_000,slow_loop_report_us=0,slow_loop_yield_us=1_000)
        peak=statistics(rows+[phase],{})["whole_route"]["slowest_main_loop"]
        self.assertEqual(peak["accepted_frame"],190)
        self.assertEqual(peak["stages_us"]["predispatch_audio"],8_000)
        self.assertEqual(peak["unlisted_us"],1_450)
        invalid=dict(phase,slow_loop_join_us=30_001)
        self.assertNotIn("slowest_main_loop",statistics(rows+[invalid],{})["whole_route"])

    def test_main_phase_row_requires_same_input_dispatch_and_clean_cut(self):
        """Отдельный измеритель Main подтверждает обе границы и не меняет FPS."""
        rows = [frame(120, 2_000_000), frame(240, 4_160_000)]
        phase = dict(raw_type="NES_FRAME_LOOP_PHASE_METRICS", schema=2, main_core=0,
                     game=0, generation=7, accepted_first=121, accepted_last=240,
                     accepted_input_first=120, accepted_input_last=239,
                     dispatched_input_first=121, dispatched_input_last=240,
                     completed_main_iterations=120, clean=1, uart_inclusive=0,
                     unit="completed_main_iteration", phase_map="dispatch_consume_report_admission",
                     admission_scope="check_and_wait", phase_maxima_sum_valid=0,
                     task_exclusive=0, overhead_subtracted=0,
                     loop_wall_us=17_303, loop_wall_max_us=21_000,
                     loop_wall_max_accepted_frame=190, outer_join_us=3000, cli_menu_us=30,
                     pace_us=50, input_current_us=250, dispatch_us=20, consume_present_us=10_900,
                     report_us=100, end_yield_us=450, remaining_us=20, predispatch_audio_us=2480,
                     predispatch_audio_max_us=6_000)
        valid = statistics(rows+[phase], {})["whole_route"]
        self.assertEqual(valid["main_phases"]["covered_windows"], 1)
        self.assertEqual(valid["main_phases"]["stage_means_us"]["predispatch_audio_us"], 2480)
        self.assertAlmostEqual(valid["mean_fps"], 120 / 2.16)
        self.assertNotIn("loop_independent", valid["worst3_windows"][0])
        for mismatch in (dict(dispatched_input_last=239), dict(clean=0), dict(game=1),
                         dict(accepted_first=122), dict(loop_wall_us=16_000)):
            self.assertNotIn("main_phases", statistics(rows+[dict(phase, **mismatch)], {})["whole_route"])
        duplicate = dict(phase, predispatch_audio_max_us=6010)
        self.assertNotIn("main_phases", statistics(rows+[phase,duplicate], {})["whole_route"])

    def test_coupled_hart_uses_board_timestamp_and_keeps_waits(self):
        """Такты Coupled требуют полного среза по часам платы и отдельной области."""
        def coupled(frames, stamp):
            return dict(frame(frames, stamp), raw_type="NES_COUPLED_BENCH", mapper=5)
        rows = [coupled(120, 2_000_000), coupled(240, 4_160_000)]
        hart = dict(raw_type="NES_COUPLED_HART_STATS", schema=1, game=0, generation=7,
                    scope="producer_hart", guest_scope="joined_frame_cut", task_exclusive=0,
                    includes_waits=1, diagnostic_only=1, overhead_subtracted=0,
                    first_sequence=120, last_sequence=239, frames=240, device_us=4_160_000,
                    samples=120, valid=120, invalid=0, overflow=0,
                    cycles=720_000_000, retired=480_000_000, guest_frame_cycles=3_573_660,
                    cycle_min=5_000_000, cycle_max=7_000_000,
                    retired_min=3_000_000, retired_max=5_000_000)
        result = statistics(rows+[hart], {})["whole_route"]
        self.assertEqual(result["coupled_hart"]["cycles_per_retired"], 1.5)
        self.assertTrue(result["coupled_hart"]["includes_waits"])
        self.assertNotIn("hart", result)
        for mismatch in (dict(device_us=4_160_001), dict(last_sequence=240), dict(generation=9),
                         dict(valid=119, invalid=1), dict(scope="stepFrame_hart")):
            self.assertNotIn("coupled_hart", statistics(rows+[dict(hart, **mismatch)], {})["whole_route"])
        duplicate = dict(hart, cycles=720_000_001)
        self.assertNotIn("coupled_hart", statistics(rows+[hart,duplicate], {})["whole_route"])

    def test_06_legacy_rounding_and_audio_reset(self):
        """Legacy использует FW fps; audio reset остаётсяunknown без подстановки0."""
        rows=[dict(raw_type="NES_PERF",game=2,frames=f,fps=fps,core_us=12000) for f,fps in [(120,60),(240,60),(360,30)]]
        rows += [dict(raw_type="NES_AUDIO",accepted=1000,dma=1000,missed_blocks=2,max_gap_us=7000),
                 dict(raw_type="NES_AUDIO",accepted=100,dma=100,missed_blocks=0,max_gap_us=1000)]
        result=statistics(rows,{})
        self.assertEqual(result["whole_route"]["mean_fps"],40)
        self.assertEqual(result["audio"]["status"],"counter_reset_unknown")
        self.assertEqual(result["audio"]["observed_counter_deltas"],{})
        self.assertEqual(result["audio"]["gap_max_us"],7000)
        self.assertIsNone(result["whole_route"]["mean_submitted_fps"])


if __name__=="__main__":
    unittest.main()
