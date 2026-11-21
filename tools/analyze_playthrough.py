#!/usr/bin/env python3
"""Офлайн-анализ пассивного UART: никакого COM, SDK или FPS по часам ПК.

python tools/analyze_playthrough.py capture.raw --out build/validation/my-play
Первый отчёт каждой непрерывной области служит только якорем. CSV содержит
измеренные 120-frame окна; сцены/уровни автоматически не распознаются.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

PERF = {"NES_OUTPUT_FRAME_PIPELINE_TIMES", "NES_COUPLED_BENCH"}
from playthrough_log import parse_line as _parse_packet

LIMITS = [
    "Only observed 120-frame window minima/maxima; no true per-frame FPS or p5/p95 frame times.",
    "Mean rate is sum(counter deltas)/sum(device time) over eligible observed 120-frame intervals; missing/scope/clock gaps are listed.",
    "Host receipt time, wall-clock prefixes and rx offsets are never FPS clocks.",
    "device_ms is the ESP logger checkpoint, not the accepted boundary: +/-1ms quantization only; variable Main/logging latency is unbounded.",
    "Produced, accepted and submitted are separate software counters, not physical LCD VSYNC.",
    "Title/menu/gameplay/death/transition are not identified automatically; input changes prove only logged activity.",
    "NES_INPUT action=none is pre-dispatch union of physical/serial/touch input, not completed guest input proof; UI requests are not dispatched and same-frame events are valid.",
    "Stage wall times overlap and may include preemption: producer contains nested work; CPU0/CPU1/C6 must not be summed.",
    "C6 Coupled producer includes read/prefix/commit handshakes; commit/stage/held/pump are nested or overlapping, not pure C6 compute.",
    "Long device gaps remain in all_observed_windows with ambiguous_gap=true; they may be an unlogged pause or a real hang/hitch.",
    "trusted_unambiguous excludes flagged ambiguity only: unmarked short pauses can still remain; it is not verified gameplay or a physical timing guarantee.",
    "Physical pause/menu/resume can emit no lifecycle line; NES_INPUT exit/settings is a UI/confirmation request, not proof of exit.",
    "Audio maxima are cumulative activation observations, not proven session-only gaps; missing telemetry is unknown.",
]


def _uint(value):
    """Проверяет счётчик без принятия bool/float за целое число."""
    return type(value) is int and value >= 0


def parse_line(line, line_number):
    """Добавляет номер строки к общему parser; повреждение JSON не скрывает raw gap."""
    try:
        row = _parse_packet(line)
    except ValueError:
        return {"tag": "MALFORMED", "fields": {}, "line_number": line_number, "parse_error": True}
    if row is None:
        return None
    if row["tag"] == "BOOT":
        return {"boot": True, "line_number": line_number}
    return {**row, "line_number": line_number}


def _clock(row):
    """Выбирает только фактически напечатанный device_us либо logger-ms."""
    fields = row["fields"]
    if _uint(fields.get("device_us")):
        return "device_us", fields["device_us"]
    if _uint(row.get("device_ms")):
        return "logger_ms", row["device_ms"] * 1000
    return None, None


def _identity(fields):
    """Фиксирует identity из явного GAME_START, не из индекса каталога."""
    crc = fields.get("crc32", fields.get("crc"))
    return {"crc32": str(crc).lower().zfill(8) if crc is not None else None,
            "title": fields.get("name"), "index": fields.get("index", fields.get("game")),
            "mapper": fields.get("mapper"), "region": fields.get("region")}


def _summary(windows):
    """Считает взвешенное N/time, а mean stage взвешивает числом sample."""
    result = {"status": "measured" if windows else "not_measured", "windows": len(windows),
              "accepted_frames": sum(w["accepted_delta"] for w in windows),
              "elapsed_us": sum(w["elapsed_us"] for w in windows),
              "true_per_frame_min_fps": None, "frame_time_p5_us": None, "frame_time_p95_us": None}
    for label in ("accepted", "produced", "submitted"):
        valid = [w for w in windows if w.get(label + "_delta") is not None]
        elapsed = sum(w["elapsed_us"] for w in valid)
        result[label] = {"coverage_windows": len(valid),
                         "frames": sum(w[label + "_delta"] for w in valid),
                         "elapsed_us": elapsed,
                         "mean_fps": sum(w[label + "_delta"] for w in valid) * 1e6 / elapsed if elapsed else None,
                         "min_window_fps": min((w[label + "_fps"] for w in valid), default=None),
                         "max_window_fps": max((w[label + "_fps"] for w in valid), default=None)}
    keys = {k for w in windows for k in w["stages"]}
    result["stage_means_us"] = {}
    result["stage_maxima_us"] = {}
    for key in sorted(keys):
        valid = [w for w in windows if key in w["stages"]]
        if key.startswith("max_") or "_max_" in key:
            result["stage_maxima_us"][key] = max(w["stages"][key] for w in valid)
        else:
            result["stage_means_us"][key] = sum(w["stages"][key] * w["samples"] for w in valid) / sum(w["samples"] for w in valid)
    result["worst3_windows"] = [{"input_first": w["input_first"], "input_last": w["input_last"],
                                  "accepted_fps": w["accepted_fps"], "line_last": w["line_last"],
                                  "observed_stage": w["observed_stage"]} for w in sorted(windows, key=lambda x: x["accepted_fps"])[:3]]
    return result


def analyze_text(text, profile="auto", max_gap_seconds=10, marks=None):
    """Разделяет игры/поколения и возвращает JSON-совместимые окна; I/O отсутствует."""
    sessions, global_events = [], []
    current, anchor, pending_mode = None, None, None
    boot_epoch = 0
    marks = list(marks or [])

    def start(reason, line_number, identity=None, inherited=False):
        """Создаёт bounded session; первый report не получает выдуманный FPS."""
        nonlocal current, anchor
        current = {"session": len(sessions) + 1, "boot_epoch": boot_epoch,
                   "identity": identity or _identity({}), "identity_source": "inherited_GAME_START" if inherited else "unknown",
                   "start_reason": reason, "start_line": line_number, "generation": None,
                   "mode": pending_mode, "scope": "unreviewed_mixed_playthrough",
                   "windows": [], "input_changes": [], "telemetry": [], "faults": [],
                   "discontinuities": [], "report_count": 0}
        sessions.append(current)
        anchor = None

    def cut(reason, row, inherit=True):
        """Закрывает область перед reset/rebase; ROM переносится только из известного запуска."""
        nonlocal current, anchor
        identity = dict(current["identity"]) if inherit and current else None
        inherited = bool(current and current["identity_source"] != "unknown")
        if current:
            current["end_reason"] = reason
            current["end_line"] = row["line_number"]
        start(reason, row["line_number"], identity, inherited)

    def gap(reason, row, **extra):
        """Сохраняет видимый пропуск; следующая строка PERF станет новым якорем."""
        nonlocal anchor
        event = dict(reason=reason, line=row["line_number"], **extra)
        (current["discontinuities"] if current else global_events).append(event)
        anchor = None

    for line_number, line in enumerate(text.splitlines(), 1):
        for mark in marks:
            if mark.get("line_number") == line_number and mark.get("analysis_break") is True:
                gap("explicit_manual_analysis_break", {"line_number": line_number}, mark=mark)
        row = parse_line(line, line_number)
        if not row:
            continue
        if row.get("boot"):
            if current:
                current.update(end_reason="boot", end_line=line_number)
            current, anchor, pending_mode = None, None, None
            boot_epoch += 1
            global_events.append({"reason": "boot", "line": line_number})
            continue
        tag, fields = row["tag"], row["fields"]
        if row.get("parse_error"):
            gap("malformed_packet", row)
            continue
        if tag == "FIRMWARE_INFO":
            global_events.append({"reason": "firmware_info", **row})
            continue
        if tag in {"ROM_LOAD_FAIL", "ROM_READ_FAIL"}:
            if current:
                # ?????? ?????????? load ?? ???????? runtime fault ?????????? ROM.
                current.update(end_reason="next_launch_" + tag, end_line=line_number)
            global_events.append({"reason": tag, **row})
            current, anchor, pending_mode = None, None, None
            continue
        if tag == "NES_GAME_START":
            if current:
                current.update(end_reason="GAME_START", end_line=line_number)
            start("GAME_START", line_number, _identity(fields))
            current["identity_source"] = "GAME_START"
            continue
        if tag.endswith("_MODE") and ("PIPELINE" in tag or "COUPLED" in tag):
            pending_mode = {"tag": tag, **fields}
            if current:
                # MODE ????? ?????????? ? ?????????? GAME_START; ?????? ROM ?? ????????????.
                gap("backend_MODE_or_rebind", row)
            continue
        if tag == "NES_LIBRARY":
            if current:
                current.update(end_reason="library", end_line=line_number)
            current, anchor = None, None
            continue
        if tag not in PERF and tag != "NES_INPUT" and current is None:
            if tag.endswith(("_FAIL", "_ERROR")):
                global_events.append({"reason": "fault_without_session", **row})
            continue
        if current is None:
            start("capture_mid_game", line_number)
        if tag.endswith(("_FAIL", "_ERROR", "_FAULT")):
            current["faults"].append(row)
            gap("fault", row)
            continue
        # Все доступные диагностические status/tasks являются observer-разрывом.
        if "STATUS" in tag or tag in {"NES_TASKS", "NES_SETTINGS"}:
            current["telemetry"].append(row)
            gap("explicit_observer_or_pause_status", row)
            continue
        if tag not in PERF and tag != "NES_INPUT":
            current["telemetry"].append(row)
            continue
        generation = fields.get("generation")
        if _uint(generation):
            if current["generation"] is not None and current["generation"] != generation:
                cut("generation_change", row)
            current["generation"] = generation
        if tag == "NES_INPUT":
            current["input_changes"].append({"line": line_number, "guest_pre_dispatch_point": fields.get("action") == "none", **fields})
            if fields.get("action") in {"settings", "pause", "exit"}:
                gap("input_action_" + fields["action"], row)
            continue
        current["report_count"] += 1
        for key, source in (("index", "game"), ("mapper", "mapper"), ("region", "region")):
            value = fields.get(source)
            old = current["identity"].get(key)
            if old is not None and value is not None and old != value:
                cut("identity_or_region_change", row, inherit=key == "region")
                current["generation"] = generation
                current["identity"][key] = value
            elif old is None and value is not None:
                current["identity"][key] = value
        accepted = fields.get("accepted_frames", fields.get("frames"))
        if not _uint(accepted):
            gap("missing_accepted_counter", row)
            continue
        row["accepted"] = accepted
        if anchor:
            before = anchor["fields"]
            old_clock, begin = _clock(anchor)
            clock, end = _clock(row)
            old_accepted = anchor["accepted"]
            if tag != anchor["tag"]:
                cut("backend_report_change", row)
                current["generation"] = generation
            elif accepted < old_accepted:
                cut("frame_counter_reset", row)
                current["generation"] = generation
            elif clock == old_clock and clock and end < begin:
                cut("device_clock_reset", row, inherit=False)
                current["generation"] = generation
            else:
                delta = accepted - old_accepted
                elapsed = end - begin if clock == old_clock and clock else None
                reason = None
                if delta != 120 or fields.get("samples") != 120:
                    reason = "missing_or_incomplete_120_frame_report"
                elif elapsed is None:
                    reason = "missing_or_changed_device_clock"
                elif elapsed <= 0:
                    reason = "device_clock_not_increasing"
                if reason:
                    gap(reason, row, accepted_before=old_accepted, accepted_after=accepted, elapsed_us=elapsed)
                else:
                    ambiguous = bool(max_gap_seconds and elapsed > max_gap_seconds * 1e6)
                    if ambiguous:
                        current["discontinuities"].append({"reason": "ambiguous_gap_possible_unmarked_pause_or_real_hitch",
                                                           "line": line_number, "elapsed_us": elapsed,
                                                           "preserved_in_all_observed_windows": True})
                    backend = "C6Coupled" if tag == "NES_COUPLED_BENCH" else ("P4Frame" if profile == "p4" else "C6Non5Frame" if profile == "c6" else "Frame")
                    stages = {k: v for k, v in fields.items() if k.endswith("_us") and k != "device_us" and type(v) in (int, float) and math.isfinite(v) and v >= 0}
                    observed = max(((k, v) for k, v in stages.items() if k in {"producer_wall_us", "consumer_active_us", "dsp_us", "present_us", "audio_us"}), key=lambda pair: pair[1], default=(None, None))
                    w = {"window": len(current["windows"]) + 1, "backend": backend,
                         "accepted_first": old_accepted + 1, "accepted_last": accepted,
                         "input_first": old_accepted, "input_last": accepted - 1,
                         "accepted_delta": delta, "accepted_fps": delta * 1e6 / elapsed,
                         "samples": 120, "elapsed_us": elapsed, "clock": clock,
                         "ambiguous_gap": ambiguous, "unmarked_pause_cannot_be_ruled_out": True,
                         "device_start_us": begin, "device_end_us": end,
                         "quality": "firmware_device_us" if clock == "device_us" else "device_logger_ms_checkpoint_counter_rate",
                         "quantization_only_bound_us": 1000 if clock == "logger_ms" else None,
                         "line_first": anchor["line_number"], "line_last": line_number,
                         "stages": stages, "counter_anomalies": [],
                         "observed_stage": {"field": observed[0], "mean_us": observed[1], "causal_proof": False}}
                    for label in ("produced", "submitted"):
                        a, b = before.get(label + "_frames"), fields.get(label + "_frames")
                        value = b - a if _uint(a) and _uint(b) and b >= a else None
                        w[label + "_delta"] = value
                        w[label + "_fps"] = value * 1e6 / elapsed if value is not None else None
                        if value is None:
                            w["counter_anomalies"].append(label + "_missing_or_reset")
                        elif value != delta:
                            w["counter_anomalies"].append(label + "_lead_changed")
                    current["windows"].append(w)
        anchor = row

    for session in sessions:
        session.setdefault("end_reason", "capture_end_open")
        session["open_at_capture_end"] = session["end_reason"] == "capture_end_open"
        session["game_completion_inferred"] = False
        session.setdefault("end_line", len(text.splitlines()))
        for window in session["windows"]:
            window["input_changes"] = [p for p in session["input_changes"] if
                _uint(p.get("frame")) and window["input_first"] <= p["frame"] <= window["input_last"] and
                (session["generation"] is None or p.get("generation", session["generation"]) == session["generation"])]
            window["activity"] = {"logged_changes": len(window["input_changes"]),
                                  "nonzero_applied_points": sum(bool(p.get("applied")) for p in window["input_changes"]),
                                  "scene_verified": False, "absence_of_change_is_not_inactivity": True}
            window["telemetry"] = [r for r in session["telemetry"] if r["fields"].get("frames", r["fields"].get("guest_frames", r["fields"].get("accepted_last"))) == window["accepted_last"]]
            loop = next((r["fields"] for r in window["telemetry"] if r["tag"] == "NES_FRAME_LOOP_METRICS" and
                         r["fields"].get("generation") == session["generation"] and
                         r["fields"].get("accepted_first") == window["accepted_first"] and
                         r["fields"].get("accepted_last") == window["accepted_last"] and
                         r["fields"].get("completed_main_iterations") == 120), None)
            if loop:
                window["loop_metrics"] = loop
                # Сравниваем mean loop wall, не max и не выдуманную длительность кадра.
                if _uint(loop.get("loop_wall_us")):
                    loop_elapsed = loop["loop_wall_us"] * 120
                    discrepancy = window["elapsed_us"] - loop_elapsed
                    window["checkpoint_minus_matched_loop_us"] = discrepancy
                    if discrepancy > max(250000, window["elapsed_us"] * 0.15):
                        window["ambiguous_gap"] = True
                        window["ambiguous_checkpoint_gap"] = True
                        session["discontinuities"].append({"reason": "ambiguous_checkpoint_gap",
                            "line": window["line_last"], "checkpoint_minus_matched_loop_us": discrepancy,
                            "possible_unmarked_pause_or_other_unmeasured_latency": True,
                            "preserved_in_all_observed_windows": True})
        session["all_observed_windows"] = _summary(session["windows"])
        session["trusted_unambiguous"] = _summary([w for w in session["windows"] if not w["ambiguous_gap"]])
        session["trusted_unambiguous"]["scope"] = "no_flagged_ambiguity_only_not_verified_gameplay"
        session["manual_marks"] = [m for m in marks if _uint(m.get("line_number")) and session["start_line"] <= m["line_number"] <= session["end_line"]]
        session["summary"] = session["all_observed_windows"]
        session["summary"]["scope"] = "all_observed_120_intervals_including_ambiguous_gaps"
        session["summary"]["discontinuity_count"] = len(session["discontinuities"])
        session["summary"]["excluded_gaps"] = sum(not g.get("preserved_in_all_observed_windows", False) for g in session["discontinuities"])
        session["trusted_unambiguous"]["excluded_ambiguous_windows"] = sum(w["ambiguous_gap"] for w in session["windows"])
        audio = [r for r in session["telemetry"] if r["tag"] in {"NES_AUDIO", "NES_FRAME_AUDIO_STATUS"}]
        session["audio"] = {"observations": audio, "max_gap_us": max((r["fields"].get("max_gap_us", r["fields"].get("max_write_gap_us", 0)) for r in audio), default=None),
                            "scope": "observed_cumulative_activation_maximum_not_proven_session_only"}
    return {"schema": "passive_playthrough_analysis_v1", "profile": profile, "sessions": sessions,
            "global_events": global_events, "manual_marks": marks, "limits": list(LIMITS),
            "config": {"window_frames": 120, "max_gap_seconds": max_gap_seconds}}


def markdown(result):
    """Показывает наблюдаемые FPS и худшие окна без автоматического определения уровня."""
    lines = ["# Анализ пассивного прохождения", "",
             "FPS ниже — темп программных счётчиков по часам устройства. Сцена не проверена: возможна смесь игры, меню, заставки и переходов.", "",
             "| Сессия | CRC / игра | Generation | Окон120 | Accepted средний / мин. / макс. FPS | Produced средний | Submitted средний | Разрывов |",
             "|---:|---|---:|---:|---|---:|---:|---:|"]
    def fmt(value):
        return "не измерено" if value is None else f"{value:.3f}"
    for session in result["sessions"]:
        s, ident = session["summary"], session["identity"]
        a = s["accepted"]
        title = str(ident.get("title") or "неизвестна").replace("|", "/")
        lines.append(f"| {session['session']} | {ident.get('crc32') or 'неизвестен'} / {title} | {session['generation']} | {s['windows']} | {fmt(a['mean_fps'])} / {fmt(a['min_window_fps'])} / {fmt(a['max_window_fps'])} | {fmt(s['produced']['mean_fps'])} | {fmt(s['submitted']['mean_fps'])} | {s['discontinuity_count']} |")
    lines += ["", "Минимум/максимум относятся к среднему за120 кадров, а не к отдельному кадру. Среднее = сумма кадров / сумма device time. Первый отчёт — только якорь.",
              "Все наблюдаемые окна сохранены, включая ambiguous_gap. Неоднозначный разрыв может быть паузой, зависанием или иной задержкой; его причина не доказана.", ""]
    for session in result["sessions"]:
        ident = session["identity"]
        reliable = session["trusted_unambiguous"]["accepted"]
        lines += [f"Сессия {session['session']}: {ident.get('title') or 'неизвестная игра'}. Причина начала: {session['start_reason']}; конец: {session['end_reason']}.",
                  f"Подмножество без выявленных флагов неоднозначности: {session['trusted_unambiguous']['windows']} окон, accepted средний {fmt(reliable['mean_fps'])}, минимум {fmt(reliable['min_window_fps'])}. Это не доказательство отсутствия короткой непомеченной паузы и не проверенный gameplay.", ""]
        worst = sorted(session["windows"], key=lambda w: w["accepted_fps"])[:3]
        if worst:
            lines += ["| Худшее окно: input frames | Accepted frames | FPS | Крупный наблюдаемый этап | Неоднозначность |",
                      "|---|---|---:|---|---|"]
            for w in worst:
                stage = w["observed_stage"]
                detail = (f"{stage['field']}={stage['mean_us']:.0f}us" if stage["field"] else "этапы отсутствуют")
                if w["backend"] == "C6Coupled":
                    detail += "; producer включает handshakes"
                else:
                    detail += "; wall-этапы перекрываются"
                lines.append(f"| {w['input_first']}–{w['input_last']} | {w['accepted_first']}–{w['accepted_last']} | {w['accepted_fps']:.3f} | {detail}; точная причина не доказана | {'да' if w['ambiguous_gap'] else 'явный флаг отсутствует'} |")
            lines.append("")
        if session["discontinuities"]:
            lines.append("Разрывы: " + "; ".join(f"строка{g['line']}: {g['reason']}" for g in session["discontinuities"]) + ".")
            lines.append("")
    lines += ["Ограничения:", "", *("- " + item for item in result["limits"]), ""]
    return "\n".join(lines)


def write_outputs(result, out):
    """Пишет JSON/MD и CSV120; raw захват никогда не перезаписывается."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "summary.md").write_text(markdown(result), encoding="utf-8")
    columns = ["session", "crc32", "title", "generation", "window", "backend", "input_first", "input_last",
               "accepted_first", "accepted_last", "elapsed_us", "clock", "accepted_delta", "accepted_fps",
               "produced_delta", "produced_fps", "submitted_delta", "submitted_fps",
               "line_first", "line_last", "ambiguous_gap", "stages_json", "input_changes_json", "counter_anomalies_json"]
    with (out / "windows120.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, columns)
        writer.writeheader()
        for session in result["sessions"]:
            for w in session["windows"]:
                row = {k: w.get(k) for k in columns}
                row.update(session=session["session"], crc32=session["identity"]["crc32"],
                           title=session["identity"]["title"], generation=session["generation"])
                for target, source in (("stages_json", "stages"), ("input_changes_json", "input_changes"), ("counter_anomalies_json", "counter_anomalies")):
                    row[target] = json.dumps(w[source], ensure_ascii=False, sort_keys=True)
                writer.writerow(row)


def read_marks(path, raw):
    """Читает необязательные PC-аннотации; byte offset служит привязкой, не FPS clock."""
    if path is None or not Path(path).exists():
        return [], []
    marks, errors = [], []
    for ordinal, line in enumerate(Path(path).read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        try:
            mark = json.loads(line)
            if not isinstance(mark, dict):
                raise ValueError("mark must be an object")
            mark = dict(mark, mark_ordinal=ordinal)
            offset = mark.get("byte_offset", mark.get("raw_byte_offset"))
            if _uint(offset) and offset <= len(raw):
                mark["line_number"] = raw[:offset].count(b"\n") + 1
            marks.append(mark)
        except ValueError as exc:
            errors.append({"line": ordinal, "error": str(exc)})
    return marks, errors


def main(argv=None):
    """Снимок raw-файла/каталога захвата: незаконченный EOF не является строкой."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path, help="raw log file or capture directory containing raw.log")
    parser.add_argument("--out", "--output", dest="out", type=Path)
    parser.add_argument("--marks", type=Path, help="optional PC-only JSONL annotations (default: capture/marks.jsonl)")
    parser.add_argument("--profile", choices=("auto", "p4", "c6"), default="auto")
    parser.add_argument("--max-gap-seconds", type=float, default=10, help="seconds to FLAG (never discard) ambiguous long window; 0 disables flag")
    args = parser.parse_args(argv)
    if not math.isfinite(args.max_gap_seconds) or args.max_gap_seconds < 0:
        parser.error("--max-gap-seconds must be finite and >=0")
    raw_path = args.raw / "raw.log" if args.raw.is_dir() else args.raw
    data = raw_path.read_bytes()
    boundary = len(data) if data.endswith(b"\n") else data.rfind(b"\n") + 1
    complete = data[:boundary]
    marks_path = args.marks or raw_path.parent / "marks.jsonl"
    marks, mark_errors = read_marks(marks_path, data)
    result = analyze_text(complete.decode("utf-8", errors="replace"), args.profile, args.max_gap_seconds, marks)
    result["source"] = {"path": str(raw_path.resolve()), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "analyzed_complete_line_bytes": len(complete),
                        "analyzed_sha256": hashlib.sha256(complete).hexdigest(),
                        "incomplete_eof_bytes_ignored": len(data) - len(complete),
                        "snapshot_only_live_source_may_grow": True}
    result["marks_source"] = {"path": str(marks_path.resolve()) if marks_path.exists() else None,
                              "errors": mark_errors, "host_time_used_for_fps": False}
    out = args.out or raw_path.with_name(raw_path.stem + "-analysis")
    write_outputs(result, out)
    print(json.dumps({"sessions": len(result["sessions"]),
                      "windows": sum(s["summary"]["windows"] for s in result["sessions"]),
                      "incomplete_eof_bytes_ignored": len(data) - len(complete),
                      "output": str(out.resolve())}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

