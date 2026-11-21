"""Статистика P4+C6 по часам платы и раздельным счётчикам кадра.

statistics(reports, movie) не делает I/O, не открывает COM и не импортирует SDK.
reports: flat {raw_type, ...fields, line?} либо {raw_type|kind, fields, line?}.
movie: baseline_report, roi_reports, roi_input0=[inclusive,inclusive], edges.
"""
from collections import Counter
import json
import math

WINDOW_FRAMES = 120
FPS_RANGE = (59.5, 60.5)

PERF = {"NES_OUTPUT_FRAME_PIPELINE_TIMES": "Non5Frame", "NES_COUPLED_BENCH": "M5Coupled", "NES_PERF": "Legacy"}
NESTED = {
    "Non5Frame": "producer_wall_us includes core/seal/audio_retire/snapshot; CPU0 consumer overlaps CPU1 producer; dsp_us is local publication check, not C6 render",
    "M5Coupled": "producer_wall_us includes thin read/prefix/commit handshakes and remote stage; commit includes remote stage, held is inside stage; joined_main includes consumer work, audio pumping and waits",
    "Legacy": "CPU0/CPU1 wall may overlap; firmware FPS is rounded to 2 decimals",
}
LIMITS = ["min_window_fps is minimum of 120-frame average intervals, not per-frame minimum",
    "True per-frame minimum FPS and p5/p95 frame times unavailable",
    "Software accepted/submitted/produced rates are not physical LCD VSYNC",
    "HOST receipt time and rx_offset never used as FPS clocks",
    "Moving scene/input attribution must be checked externally; whole_route can mix title/menu/gameplay",
    "Wall stages overlap and may include IRQ/preemption; sums of CPU0/CPU1/C6 are not frame time",
    "C6 stage/pump wall cannot separate C6 compute, SDIO, queue or Bluetooth latency",
    "Rounded stage means are weighted by samples; averaging loses less than 1 us per source mean",
    "Audio cumulative maxima and counters need independently verified activation epoch"]


MAIN_PHASES = ("outer_join_us", "cli_menu_us", "pace_us", "input_current_us",
    "dispatch_us", "consume_present_us", "report_us", "end_yield_us",
    "remaining_us", "predispatch_audio_us")


def _number(value):
    """Принимает finite numeric без bool; идентификаторы/таймеры проверяются отдельно."""
    return type(value) in (int,float) and math.isfinite(value)


def _integer(value):
    """Проверяет unsigned firmware counter без приведения float/bool."""
    return type(value) is int and value >= 0


def _record(record):
    """Нормализует два collector envelope без потери полей и адреса исходной строки."""
    row = dict(record.get("fields",{})) if isinstance(record.get("fields"),dict) else dict(record)
    tag = record.get("raw_type",record.get("kind",row.get("raw_type","")))
    if not str(tag).startswith("NES_"):
        if "max_producer_wall_us" in row:
            tag="NES_COUPLED_BENCH"
        elif row.get("producer")=="timing_fetch" and "producer_wall_us" in row:
            tag="NES_OUTPUT_FRAME_PIPELINE_TIMES"
        elif "frames" in row and "fps" in row and "core_us" in row:
            tag="NES_PERF"
    row.update(raw_type=tag,line=record.get("line",row.get("line")),rx_offset=record.get("rx_offset",row.get("rx_offset")))
    return row


def _scope(row):
    """Разделяет boot/generation/game/ROM/region/backend, когда эти поля доступны."""
    return tuple(row.get(key) for key in ("boot_epoch","generation","game","crc32","crc","region","mapper","raw_type"))


def _pressure(window):
    """Описывает наблюдаемый крупный этап, не объявляя causal bottleneck доказанным."""
    s=window["stages"]
    producer=s.get("producer_wall_us",s.get("core_us",0))
    consumer=s.get("consumer_active_us",s.get("render_us",0))
    if window["backend"]=="M5Coupled":
        bridge=s.get("read_wall_us",0)+s.get("commit_wall_us",0)+s.get("prefix_wall_us",0)
        label="CPU1 job wall includes handshakes"
        if producer and bridge>=producer*0.5:
            label="thin/bridge read+prefix+commit wall large; underlying cause unresolved"
        elif consumer>producer:
            label="CPU0 colour consumer wall larger than producer"
    else:
        label="CPU1 producer wall largest" if producer>=consumer else "CPU0 colour consumer wall largest"
        if s.get("present_us",0)>max(producer,consumer):
            label="Main present API/wait wall largest; PPA/LCD cause unresolved"
        if s.get("audio_us",0)>max(producer,consumer,s.get("present_us",0)):
            label="Main I2S write API wall large; C6 render cost unknown"
    return dict(label=label,causal_proof=False,producer_us=producer,consumer_us=consumer)


def _frame_budget(window):
    """Сравнивает средние времена одного окна с бюджетом при 59,5 FPS.

    CPU1 исполняет кадры последовательно. Для Frame выделяем наблюдаемое
    время ядра. Для MMC5 время производителя также включает ожидание
    Thin и аудио. Разность периода и времени производителя сохраняем
    как измеренный разрыв. Наложенные стадии в него не складываем.
    """
    target = 1e6 / FPS_RANGE[0]
    period = window["elapsed_us"] / window["samples"]
    stages = window["stages"]
    producer = stages.get("producer_wall_us")
    core = stages.get("core_us") if window["backend"] == "Non5Frame" else None
    return dict(
        target_us=target,
        period_us=period,
        period_excess_us=max(0., period-target),
        producer_excess_us=max(0., producer-target) if _number(producer) and producer > 0 else None,
        core_excess_us=max(0., core-target) if _number(core) and core > 0 else None,
        period_minus_producer_us=period-producer if _number(producer) and producer > 0 else None,
    )


def _main_phases_valid(row, window):
    """Проверяет окно Main и сдвиг на один кадр у следующего задания Worker."""
    markers = dict(schema=2, clean=1, uart_inclusive=0, main_core=0,
                   unit="completed_main_iteration", phase_map="dispatch_consume_report_admission",
                   admission_scope="check_and_wait", phase_maxima_sum_valid=0,
                   task_exclusive=0, overhead_subtracted=0)
    if any(row.get(key) != value for key, value in markers.items()):
        return False
    if not (row.get("completed_main_iterations") == window["samples"]
            and row.get("accepted_first") == window["accepted_first"]
            and row.get("accepted_last") == window["accepted_last"]
            and row.get("dispatched_input_first") == window["input_first"] + 1
            and row.get("dispatched_input_last") == window["input_last"] + 1):
        return False
    fields = (*MAIN_PHASES, "loop_wall_us", "loop_wall_max_us",
              "loop_wall_max_accepted_frame", "predispatch_audio_max_us")
    if not all(_integer(row.get(key)) for key in fields):
        return False
    return (0 <= row["loop_wall_us"] - sum(row[key] for key in MAIN_PHASES) <= len(MAIN_PHASES)
            and row["loop_wall_max_us"] >= row["loop_wall_us"]
            and window["accepted_first"] <= row["loop_wall_max_accepted_frame"] <= window["accepted_last"]
            and row["predispatch_audio_max_us"] >= row["predispatch_audio_us"])


def _coupled_hart_valid(row, window):
    """Разрешает только полные счётчики одного опубликованного окна Coupled."""
    markers = dict(schema=1, scope="producer_hart", guest_scope="joined_frame_cut",
                   task_exclusive=0, includes_waits=1, diagnostic_only=1, overhead_subtracted=0)
    if any(row.get(key) != value for key, value in markers.items()):
        return False
    if not (row.get("frames") == window["accepted_last"]
            and row.get("device_us") == window.get("device_end_us")
            and row.get("valid") == row.get("samples") == window["samples"]
            and row.get("invalid") == row.get("overflow") == 0):
        return False
    fields = ("cycles", "retired", "guest_frame_cycles", "cycle_min", "cycle_max",
              "retired_min", "retired_max")
    if not all(_integer(row.get(key)) and row[key] > 0 for key in fields):
        return False
    return row["cycle_min"] <= row["cycle_max"] and row["retired_min"] <= row["retired_max"]


def _attach_observer(window, field, row):
    """Исключает неоднозначное окно, если прошивка прислала две строки одного измерителя."""
    if field not in window:
        window[field] = row
    else:
        window[field] = None


def _aggregate(windows):
    """Считает N/time и N-взвешенные этапы; max отдельного этапа не превращает в FPS."""
    if not windows:
        return dict(status="not_measured",windows=0,frames=0,elapsed_us=0,min_window_fps=None,max_window_fps=None,mean_fps=None,
                    min_submitted_window_fps=None,max_submitted_window_fps=None,mean_submitted_fps=None,mean_produced_fps=None,
                    stage_means_us={},stage_maxima_us={},worst3_windows=[],observed_pressure=[],
                    true_per_frame_min_fps=None,frame_time_p5_us=None,frame_time_p95_us=None,slowest_join=None)
    frames=sum(w["samples"] for w in windows)
    elapsed=sum(w["elapsed_us"] for w in windows)
    sums,weights,maxima=Counter(),Counter(),{}
    for window in windows:
        for key,value in window["stages"].items():
            if not _number(value) or value<0:
                continue
            if key.startswith("max_") or "_max_" in key:
                maxima[key]=max(maxima.get(key,0),value)
            else:
                sums[key]+=value*window["samples"]
                weights[key]+=window["samples"]
    means={key:value/weights[key] for key,value in sums.items()}
    result=dict(status="measured_software_rate",windows=len(windows),frames=frames,elapsed_us=elapsed,
        accepted_first=min(w["accepted_first"] for w in windows),accepted_last=max(w["accepted_last"] for w in windows),
        input_first=min(w["input_first"] for w in windows),input_last=max(w["input_last"] for w in windows),
        min_window_fps=min(w["accepted_fps"] for w in windows),
        max_window_fps=max(w["accepted_fps"] for w in windows),mean_fps=frames*1e6/elapsed,
        stage_means_us=means,stage_maxima_us=maxima,
        worst3_windows=sorted(windows,key=lambda w:w["accepted_fps"])[:3],
        observed_pressure=list(dict.fromkeys(_pressure(w)["label"] for w in sorted(windows,key=lambda w:w["accepted_fps"])[:3])),
        true_per_frame_min_fps=None,frame_time_p5_us=None,frame_time_p95_us=None)
    for label in ("submitted","produced"):
        selected=[w for w in windows if w.get(label+"_delta") is not None]
        duration=sum(w["elapsed_us"] for w in selected)
        result["mean_"+label+"_fps"]=sum(w[label+"_delta"] for w in selected)*1e6/duration if duration else None
        result[label+"_coverage_windows"]=len(selected)
        if label=="submitted":
            result["min_submitted_window_fps"]=min((w["submitted_fps"] for w in selected),default=None)
            result["max_submitted_window_fps"]=max((w["submitted_fps"] for w in selected),default=None)
    result["counter_anomaly_windows"]=sum(bool(w["counter_anomalies"]) for w in windows)
    loop=[w["loop_independent"] for w in windows if "loop_independent" in w]
    largest=max(loop,key=lambda r:r.get("loop_wall_max_us",0),default={})
    result["main_loop_max_us"]=largest.get("loop_wall_max_us")
    result["main_loop_max_accepted_frame"]=largest.get("loop_wall_max_accepted_frame")
    if largest and "slow_loop_join_us" in largest:
        stage_us = {name[len("slow_loop_"):-len("_us")]: value
                    for name, value in largest.items()
                    if name.startswith("slow_loop_") and name.endswith("_us") and _integer(value)}
        elapsed = largest["loop_wall_max_us"]
        if sum(stage_us.values()) <= elapsed:
            result["slowest_main_loop"] = dict(
                accepted_frame=largest["loop_wall_max_accepted_frame"], elapsed_us=elapsed,
                stages_us=stage_us, unlisted_us=elapsed-sum(stage_us.values()))
    for field in ("hart", "coupled_hart"):
        hart = [window[field] for window in windows if window.get(field)]
        if hart:
            valid_frames = sum(row["valid"] for row in hart)
            cycles = sum(row["cycles"] for row in hart)
            retired = sum(row["retired"] for row in hart)
            result[field] = dict(covered_windows=len(hart), valid_frames=valid_frames,
                cycles_per_guest_frame=cycles/valid_frames, retired_per_guest_frame=retired/valid_frames,
                cycles_per_retired=cycles/retired, max_frame_cycles=max(row["cycle_max"] for row in hart),
                task_exclusive=False)
            if field == "coupled_hart":
                result[field]["includes_waits"] = True
    phases = [w["main_phases"] for w in windows if w.get("main_phases")]
    if phases:
        result["main_phases"] = dict(covered_windows=len(phases),
            stage_means_us={key: sum(row[key] for row in phases)/len(phases) for key in MAIN_PHASES},
            loop_wall_mean_us=sum(row["loop_wall_us"] for row in phases)/len(phases),
            clock_reads_included=True)
    # Максимум join и максимум всей итерации могут относиться к разным кадрам.
    # В разбивку берём только компоненты той же итерации, которые дала прошивка.
    join = max(loop, key=lambda row: row.get("outer_join_max_us", 0), default={})
    result["slowest_join"] = None
    if "slow_join_input" in join:
        result["slowest_join"] = dict(input0=join["slow_join_input"],
            outer_join_us=join["outer_join_max_us"], worker_wait_us=join["slow_join_worker_us"],
            previous_c6_source_us=join["slow_join_previous_c6_source_us"])
    c6=[w for w in windows if "c6" in w]
    result["c6_coverage_windows"]=len(c6)
    result["c6_mean_us_per_guest_frame"]={}
    result["c6_maxima_us"]={}
    if c6:
        for key in ("stage_mean_us","held_mean_us","pump_cycles_us_per_guest_frame","pump_no_rpc_cycles_us_per_guest_frame",
                    "pump_changed_cycles_us_per_guest_frame","pump_no_rpc_no_i2s_cycles_us_per_guest_frame"):
            valid=[w for w in c6 if _number(w["c6"].get(key))]
            if valid:
                result["c6_mean_us_per_guest_frame"][key]=sum(w["samples"]*w["c6"][key] for w in valid)/sum(w["samples"] for w in valid)
        for key in ("stage_max_us","held_max_us","pump_max_cycles_as_us"):
            result["c6_maxima_us"][key]=max((w["c6"][key] for w in c6 if _number(w["c6"].get(key))),default=None)
        result["c6_window_counter_sums"]={key:sum(w["c6"].get(key,0) for w in c6 if _number(w["c6"].get(key)))
            for key in ("stage_frames","held_entries","held_exits","observer_clock_errors","pump_calls","pump_i2s_calls","pump_i2s_samples","pump_sink_samples")}
        result["c6_overlap"]="stage includes held; pump categories overlap and include preemption; cumulative remote counters are not summed"
    return result


def _roi_fps_range(windows, span):
    """Проверяет непрерывный измеренный участок по часам платы и переданным кадрам."""
    lower, upper = FPS_RANGE
    result = dict(min_fps=lower, max_fps=upper, window_frames=WINDOW_FRAMES,
                  expected_windows=None, observed_windows=len(windows), covered=False,
                  accepted_outside=[], submitted_outside=[], publication_anomalies=[],
                  status="incomplete")
    if (not isinstance(span, (list, tuple)) or len(span) != 2
            or not all(_integer(value) for value in span)
            or span[1] < span[0] or span[0] % WINDOW_FRAMES
            or (span[1] + 1) % WINDOW_FRAMES):
        return result
    starts = list(range(span[0], span[1] + 1, WINDOW_FRAMES))
    result["expected_windows"] = len(starts)
    keys = ("backend", "game", "generation", "region", "crc32")
    scopes = {tuple(window[key] for key in keys) for window in windows}
    if (len(scopes) != 1 or [window["input_first"] for window in windows] != starts
            or any(window["quality"] != "device_us_120frame_publication_boundaries"
                   for window in windows)):
        return result
    result["covered"] = True
    for window in windows:
        frame = window["input_first"]
        if not lower <= window["accepted_fps"] <= upper:
            result["accepted_outside"].append(dict(input_first=frame, fps=window["accepted_fps"]))
        submitted = window["submitted_fps"]
        if submitted is not None and not lower <= submitted <= upper:
            result["submitted_outside"].append(dict(input_first=frame, fps=submitted))
        if window["counter_anomalies"]:
            result["publication_anomalies"].append(dict(input_first=frame, issues=window["counter_anomalies"]))
    if result["accepted_outside"] or result["submitted_outside"]:
        result["status"] = "out_of_range"
    elif result["publication_anomalies"]:
        result["status"] = "publication_unverified"
    else:
        result["status"] = "in_range"
    return result


def _audio(rows):
    """Сохраняет аудио-наблюдения, учитывая reset и неизвестный activation epoch."""
    observations=[]
    aliases={"accepted_frames":("accepted_frames","accepted"),"dma_frames":("dma_frames","dma"),
             "missed_refill_blocks":("missed_refill_blocks","missed_blocks"),"missed_refill_frames":("missed_refill_frames","missed_frames")}
    for row in rows:
        if row["raw_type"] not in ("NES_AUDIO","NES_FRAME_AUDIO_STATUS"):
            continue
        values={key:next((row[k] for k in names if _integer(row.get(k))),None) for key,names in aliases.items()}
        observations.append(dict(values,line=row.get("line"),max_gap_us=row.get("max_write_gap_us",row.get("max_gap_us")),max_wait_us=row.get("max_write_wait_us",row.get("max_wait_us"))))
    if not observations:
        return dict(status="missing_unknown",snapshots=0,gap_max_us=None)
    resets=any(a.get(key) is not None and b.get(key) is not None and b[key]<a[key]
               for a,b in zip(observations,observations[1:]) for key in aliases)
    deltas={key:observations[-1][key]-observations[0][key] for key in aliases
            if len(observations)>1 and not resets and observations[0][key] is not None and observations[-1][key] is not None}
    return dict(status="counter_reset_unknown" if resets else "recorded_epoch_not_proven",snapshots=len(observations),
                observed_counter_deltas=deltas,gap_max_us=max((r["max_gap_us"] for r in observations if _number(r["max_gap_us"])),default=None),
                wait_max_us=max((r["max_wait_us"] for r in observations if _number(r["max_wait_us"])),default=None),
                gap_scope="observed cumulative activation maximum; not proven ROI gap",observations=observations)


def statistics(reports,movie):
    """Возвращает whole-route/ROI/scenes, пропуски и худшие3 device-clock окна одной игры.

    roi_input0 задаёт inclusive диапазон входов; completed frames F соответствует
    input F-1. Без explicit ROI его измерения не выводятся. Допустимый stage scope
    совпадает только с frame/generation окна; соседние input edges не причинный proof.
    """
    movie=movie if isinstance(movie,dict) else {}
    rows=[];seen=set()
    for record in reports:
        if not isinstance(record,dict):
            continue
        row=_record(record)
        # Collector сохраняет одну строку иflat reports, иnested structured_rows.
        key=(row.get("raw_type"),row.get("line"),row.get("rx_offset")) if row.get("line") is not None or row.get("rx_offset") is not None else (row.get("raw_type"),json.dumps(row,sort_keys=True,default=str))
        if key in seen:
            continue
        seen.add(key)
        for identity in ("crc32","region","mapper","game"):
            if identity not in row and identity in movie:
                row[identity]=movie[identity]
        rows.append(row)
    perf=[r for r in rows if r["raw_type"] in PERF]
    windows,discontinuities=[],[]
    previous=None
    for row in perf:
        old,previous=previous,row
        if old is None:
            continue
        n=row.get("samples",WINDOW_FRAMES)
        if not (_integer(n) and n==WINDOW_FRAMES and _integer(row.get("frames")) and _integer(old.get("frames"))):
            discontinuities.append(dict(line=row.get("line"),reason="missing frames/samples or non120 interval"))
            continue
        if _scope(old)!=_scope(row):
            discontinuities.append(dict(line=row.get("line"),reason="scope changed; new baseline"))
            continue
        delta=row["frames"]-old["frames"]
        if delta!=n:
            discontinuities.append(dict(line=row.get("line"),reason="accepted report discontinuity",before=old["frames"],after=row["frames"],samples=n,delta=delta))
            continue
        backend=PERF[row["raw_type"]]
        counter_anomalies=[]
        window=dict(backend=backend,generation=row.get("generation"),game=row.get("game"),mapper=row.get("mapper"),region=row.get("region"),crc32=row.get("crc32",row.get("crc")),
            accepted_first=old["frames"]+1,accepted_last=row["frames"],input_first=old["frames"],input_last=row["frames"]-1,
            samples=n,line_first=old.get("line"),line_last=row.get("line"),counter_anomalies=counter_anomalies,
            stages={key:value for key,value in row.items() if key.endswith("_us") and key!="device_us"},nested_overlap=NESTED[backend])
        if backend=="Legacy":
            fps=row.get("fps")
            if not (_number(fps) and 0<fps<1000):
                discontinuities.append(dict(line=row.get("line"),reason="invalid firmware FPS"))
                continue
            window.update(elapsed_us=n*1e6/fps,accepted_fps=fps,quality="firmware_120frame_average_rounded",submitted_delta=None,produced_delta=None)
        else:
            begin,end=old.get("device_us"),row.get("device_us")
            if not (_integer(begin) and _integer(end) and 0<end-begin<=10000000):
                discontinuities.append(dict(line=row.get("line"),reason="device_us missing/nonmonotone/gap>10s"))
                continue
            elapsed=end-begin
            window.update(device_start_us=begin,device_end_us=end,elapsed_us=elapsed,accepted_fps=n*1e6/elapsed,quality="device_us_120frame_publication_boundaries")
            for label in ("submitted","produced"):
                a,b=old.get(label+"_frames"),row.get(label+"_frames")
                amount=b-a if _integer(a) and _integer(b) and b>=a else None
                if amount is None or amount>n+2:
                    counter_anomalies.append(label+" counter missing/reset/implausible")
                    amount=None
                elif amount!=n:
                    counter_anomalies.append(label+" delta differs from accepted; pending/lead must be checked")
                window[label+"_delta"]=amount
                window[label+"_fps"]=amount*1e6/elapsed if amount is not None else None
            if row.get("accepted_frames",row["frames"])!=row["frames"]:
                counter_anomalies.append("explicit accepted_frames differs from frames")
        window["nearby_input_edges"]=[dict(edge,guest_frames_from_edge_to_window_start=window["input_first"]-edge["frame"],
            guest_frames_from_edge_to_window_end=window["input_last"]-edge["frame"])
            for edge in movie.get("edges",[]) if isinstance(edge,dict) and _integer(edge.get("frame")) and window["input_first"]-120<=edge["frame"]<=window["input_last"]+120]
        if window["quality"]=="device_us_120frame_publication_boundaries":
            window["frame_budget"] = _frame_budget(window)
            window["excess_us_per_guest_frame"] = window["frame_budget"]["period_excess_us"]
        window["observed_pressure"]=_pressure(window)
        windows.append(window)
    # Attach window observers only by exact accepted endpoint; their durations are not added.
    by_end={(w["game"],w["accepted_last"]):w for w in windows}
    hart_windows={(w["generation"],w["game"],w["input_first"],w["input_last"]):w
                  for w in windows if w["backend"]=="Non5Frame"}
    ambiguous_hart=set()
    observed_cuts = {}
    for w in windows:
        key = (w["backend"], w["generation"], w["game"], w["input_first"], w["input_last"])
        observed_cuts.setdefault(key, []).append(w)
    c6=[]
    for row in rows:
        tag=row["raw_type"]
        if tag=="NES_C6_AUDIO_WINDOW":
            c6.append(row)
            target=by_end.get((row.get("game"),row.get("guest_frames")))
            if target is not None:
                values=dict(row)
                hz=row.get("pump_cycle_hz")
                if _number(hz) and hz>0:
                    for key,value in row.items():
                        if key.endswith("_cycles") and _number(value):
                            values[key+"_as_us"]=value*1e6/hz
                            if key!="pump_max_cycles":
                                values[key+"_us_per_guest_frame"]=value*1e6/hz/target["samples"]
                values["interpretation"]="stage includes held; means use guest n, not stage_frames; pump categories nested, preemption included; Non5 stage zeros expected"
                target["c6"]=values
        elif tag=="NES_COUPLED_REPORT_ENDPOINT":
            target=by_end.get((row.get("game"),row.get("frames")))
            if target is not None and row.get("device_us")==target.get("device_end_us"):
                target["endpoint"]=row
        elif tag=="NES_FRAME_HART_STATS":
            identity=(row.get("generation"),row.get("game"),row.get("first_sequence"),row.get("last_sequence"))
            target=hart_windows.get(identity)
            if (target is None or identity in ambiguous_hart or row.get("scope")!="stepFrame_hart"
                    or row.get("schema")!=1 or row.get("task_exclusive")!=0
                    or row.get("overflow")!=0 or row.get("invalid")!=0
                    or row.get("valid")!=target["samples"] or row.get("samples")!=target["samples"]
                    or not all(_integer(row.get(name)) and row[name]>0
                               for name in ("cycles","retired","cycle_max"))):
                continue
            if "hart" in target:
                ambiguous_hart.add(identity)
                del target["hart"]
            else:
                target["hart"]=row
        elif tag == "NES_COUPLED_HART_STATS":
            key = ("M5Coupled", row.get("generation"), row.get("game"),
                   row.get("first_sequence"), row.get("last_sequence"))
            candidates = [w for w in observed_cuts.get(key, ()) if _coupled_hart_valid(row, w)]
            if len(candidates) == 1:
                _attach_observer(candidates[0], "coupled_hart", row)
        elif tag == "NES_FRAME_LOOP_PHASE_METRICS":
            key = ("Non5Frame", row.get("generation"), row.get("game"),
                   row.get("accepted_input_first"), row.get("accepted_input_last"))
            candidates = observed_cuts.get(key, ())
            if len(candidates) == 1 and _main_phases_valid(row, candidates[0]):
                _attach_observer(candidates[0], "main_phases", row)
        elif tag=="NES_FRAME_LOOP_METRICS":
            candidates=[w for w in windows if w["accepted_first"]==row.get("accepted_first") and w["accepted_last"]==row.get("accepted_last") and w["generation"]==row.get("generation")]
            if len(candidates)==1:
                candidates[0]["loop_independent"]=row
    for window in windows:
        for field in ("main_phases", "coupled_hart"):
            if field in window and window[field] is None:
                del window[field]
    roi_range=movie.get("roi_input0")
    roi=[w for w in windows if isinstance(roi_range,(list,tuple)) and len(roi_range)==2 and w["input_first"]>=roi_range[0] and w["input_last"]<=roi_range[1]]
    seen_end={r.get("frames") for r in perf}
    expected=movie.get("roi_reports",[])
    faults=[r for r in rows if r["raw_type"].endswith("_ERROR") or r["raw_type"] in ("NES_RUNTIME_FAIL","NES_WORKER_FAIL")]
    groups={}
    for window in windows:
        key=tuple(window.get(k) for k in ("generation","game","region","mapper","backend","crc32"))
        groups.setdefault(key,[]).append(window)
    scenes=[]
    for scene in movie.get("scenes",[]):
        span=scene.get("input0") if isinstance(scene,dict) else None
        if isinstance(span,(list,tuple)) and len(span)==2:
            scenes.append(dict(name=scene.get("name","declared_scene"),input0=span,**_aggregate([w for w in windows if w["input_first"]>=span[0] and w["input_last"]<=span[1]])))
    return dict(schema="draft_c6_game_statistics_v1",scope=movie.get("scope","mixed_title_gameplay_unverified"),
        scene_verified=movie.get("scene_verified") is True,whole_route=_aggregate(windows),roi=_aggregate(roi),
        roi_input0=roi_range,roi_fps_range=_roi_fps_range(roi,roi_range),
        missing_roi_endpoints=[end for end in expected if end not in seen_end],
        missing_baseline=movie.get("baseline_report") not in seen_end if "baseline_report" in movie else None,
        groups=[dict(generation=key[0],game=key[1],region=key[2],mapper=key[3],backend=key[4],crc32=key[5],**_aggregate(value)) for key,value in groups.items()],
        scenes=scenes,discontinuities=discontinuities,faults=faults,c6_last=c6[-1] if c6 else None,
        c6_windows=len(c6),audio=_audio(rows),limits=LIMITS)
