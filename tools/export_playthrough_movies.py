"""Экспорт неизменяемых кандидатов NES input movie из пассивного raw.log.

COM/SDK не используются. NES_INPUT action=none задаёт маску перед dispatch;
PERF N подтверждает только завершение префикса input[0:N], а не полноту UART.
"""
import argparse
import hashlib
import json
from pathlib import Path

from playthrough_log import parse_line

SCHEMA = "bitvyr_human_input_bank_v1"
PERF = {"NES_OUTPUT_FRAME_PIPELINE_TIMES", "NES_COUPLED_BENCH"}
LIMITATIONS = [
    "Sparse changed-input UART trace is a candidate, not proof that every edge was logged.",
    "No passive final CPU/RAM/RGB oracle was captured; exact hardware replay is unverified.",
    "applied is the pre-Core union; feed it directly to the same Core/region without remapping.",
    "accepted frame N consumed input N-1; events at frame >= last_completed_frame are uncommitted.",
    "Logged direction buttons suggest input activity; they do not prove movement or gameplay.",
    "UART replay of applied masks does not reproduce physical Xbox BLE/HCI packet workload.",
    "Short unmarked pauses and missing press/release pairs cannot be excluded by changed logging.",
]


def digest(data):
    """SHA256 исходных байтов либо канонического input prefix, без нормализации raw."""
    return hashlib.sha256(data).hexdigest()


def pin(path):
    """Фиксирует существующий файл; ROM и секретные настройки не читаются."""
    path = Path(path)
    data = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(data), "sha256": digest(data)}


def build_bank(raw, capture_id="capture", analysis=None):
    """Разбирает полный raw; сохраняет все наблюдённые края и лишь PERF-proven prefix.

    Вход заимствуется только на время вызова. Возврат содержит JSON-совместимые
    копии. MODE без GAME_START допускается лишь как помеченный fresh-load
    candidate при frame0/новом generation; прочие разрывы дают HOLD.
    """
    guid = digest(raw)
    segments, excluded, issues = [], [], []
    current, previous_identity, pending_mode = None, None, None
    launch_ordinal = 0
    offset = 0

    def close(reason, ref):
        """Закрывает старый scope до нового MODE/load, не приписывая ему чужой fault."""
        nonlocal current, previous_identity
        if current is not None:
            current["end_reason"] = reason
            current["end_source"] = ref
            previous_identity = {k: current.get(k) for k in
                                 ("crc32", "title", "index", "mapper", "region")}
        current = None

    def start(identity, reason, ref, launch=None):
        """Выделяет отдельный вариант для каждого запуска или state discontinuity."""
        nonlocal current
        current = dict(identity, launch_ordinal=launch, start_reason=reason,
                       start_source=ref, source_generation=None, observed_inputs=[],
                       ignored_ui_inputs=[], perf_reports=[], hold_reasons=[],
                       duplicate_frames=[], mode=pending_mode, last_completed_frame=0)
        if reason not in {"GAME_START", "known_fresh_reload_candidate"}:
            current["hold_reasons"].append("initial_state_not_known_fresh_load")
        segments.append(current)

    def hold(reason):
        """Не исправляет неоднозначность: оставляет prefix и явный запрет replay."""
        if reason not in current["hold_reasons"]:
            current["hold_reasons"].append(reason)

    for number, wire in enumerate(raw.splitlines(keepends=True), 1):
        ref = {"line": number, "byte_offset": offset, "byte_end": offset + len(wire)}
        offset += len(wire)
        if not wire.endswith((b"\n", b"\r")):
            issues.append({"reason": "incomplete_EOF_line_ignored", "source_event": ref})
            break
        try:
            row = parse_line(wire.decode("utf-8"))
        except (ValueError, UnicodeError) as exc:
            issues.append({"reason": "malformed_line", "source_event": ref, "error": str(exc)})
            if current is not None:
                hold("malformed_line_inside_scope")
            continue
        if not row:
            continue
        tag, fields = row["tag"], row["fields"]
        if tag == "BOOT":
            close("boot", ref)
            previous_identity, pending_mode = None, None
            continue
        if tag in {"ROM_LOAD_FAIL", "ROM_READ_FAIL", "NES_LIBRARY"}:
            close("next_" + tag, ref)
            pending_mode = None
            if tag != "NES_LIBRARY":
                excluded.append({"tag": tag, "fields": fields, "source_event": ref})
            continue
        if tag.endswith("_MODE") and ("PIPELINE" in tag or "COUPLED" in tag):
            prior_generation = current["source_generation"] if current else None
            close("MODE_or_rebind", ref)
            pending_mode = {"tag": tag, "fields": fields, "source_event": ref,
                            "prior_generation": prior_generation}
            continue
        if tag == "NES_GAME_START":
            close("GAME_START", ref)
            launch_ordinal += 1
            start({"crc32": fields["crc"], "title": fields["name"],
                   "index": fields.get("index"), "mapper": fields["mapper"],
                   "region": fields["region"]}, "GAME_START", ref, launch_ordinal)
            pending_mode = None
            continue
        if tag not in PERF and tag != "NES_INPUT":
            if current and tag.endswith(("_FAIL", "_FAULT", "_ERROR")):
                hold("runtime_fault")
            continue
        generation = fields.get("generation")
        if current is None:
            fresh = (tag == "NES_INPUT" and fields.get("frame") == 0 and
                     pending_mode is not None and previous_identity is not None and
                     generation is not None and
                     pending_mode.get("prior_generation") is not None and
                     generation != pending_mode["prior_generation"])
            start(previous_identity or {"crc32": None, "title": None, "index": None,
                  "mapper": None, "region": None},
                  "known_fresh_reload_candidate" if fresh else "capture_mid_game", ref)
            pending_mode = None
        if current["source_generation"] is not None and generation != current["source_generation"]:
            identity = {k: current.get(k) for k in ("crc32", "title", "index", "mapper", "region")}
            close("generation_change_without_fresh_MODE_frame0", ref)
            start(identity, "unsupported_state_carry", ref)
        current["source_generation"] = generation
        if tag == "NES_INPUT":
            frame, mask = fields.get("frame"), fields.get("applied")
            if not isinstance(frame, int) or frame < 0 or not isinstance(mask, int) or not 0 <= mask <= 255:
                hold("invalid_input_point")
                continue
            point = {"frame": frame, "mask": mask, "source_event": ref,
                     "input_sources": {k: fields.get(k) for k in
                         ("xbox_raw", "xbox_effective", "serial", "touch", "contacts",
                          "ids", "poll_ok", "suppressed")}}
            if fields.get("action") != "none":
                current["ignored_ui_inputs"].append(dict(point, action=fields.get("action")))
                continue
            prior = current["observed_inputs"]
            if prior and frame < prior[-1]["frame"]:
                hold("input_frame_reordered_or_unmarked_reset")
            if prior and frame == prior[-1]["frame"]:
                current["duplicate_frames"].append(frame)
                if frame < current["last_completed_frame"] and mask != prior[-1]["mask"]:
                    hold("conflicting_duplicate_after_completed_PERF")
            elif frame < current["last_completed_frame"]:
                hold("input_point_arrived_after_completed_PERF")
            current["observed_inputs"].append(point)
        else:
            count = fields.get("frames", fields.get("accepted_frames"))
            if not isinstance(count, int) or count < 0:
                hold("invalid_completed_counter")
                continue
            for key, wirekey in (("index", "game"), ("mapper", "mapper"), ("region", "region")):
                if fields.get(wirekey) is not None and current.get(key) != fields[wirekey]:
                    hold("PERF_identity_or_region_mismatch")
            if count < current["last_completed_frame"]:
                hold("completed_counter_reset_without_fresh_MODE_frame0")
            current["last_completed_frame"] = max(count, current["last_completed_frame"])
            current["perf_reports"].append({"frame": count, "generation": generation,
                                           "source_event": ref})
    close("capture_EOF", {"byte_offset": len(raw), "byte_end": len(raw)})
    variants = {}
    for ordinal, s in enumerate(segments, 1):
        crc = s["crc32"] or "unknown"
        variants[crc] = variants.get(crc, 0) + 1
        s.update(session_id=f"{capture_id}:{ordinal:02d}", variant_id=f"{crc}-v{variants[crc]:02d}",
                 source_guid=guid, first_frame=0, last_frame=s["last_completed_frame"] - 1)
        points = {}
        for p in s["observed_inputs"]:
            if p["frame"] < s["last_completed_frame"]:
                points[p["frame"]] = p
        events = []
        for frame in sorted(points):
            point = points[frame]
            if not events or point["mask"] != events[-1]["mask"]:
                events.append(point)
        s["source_input_counts"] = {key: sum(bool(p["input_sources"].get(key)) for p in s["observed_inputs"])
                                    for key in ("xbox_raw", "xbox_effective", "serial", "touch")}
        s["source_input_counts"]["touch_poll_not_ok"] = sum(p["input_sources"].get("poll_ok") == 0 for p in s["observed_inputs"])
        s["events"] = events
        s["uncommitted_tail"] = [p for p in s["observed_inputs"]
                                  if p["frame"] >= s["last_completed_frame"]]
        if not events or events[0]["frame"] != 0:
            s["hold_reasons"].append("missing_observed_committed_frame0")
        if not s["crc32"] or not s["perf_reports"]:
            s["hold_reasons"].append("missing_ROM_identity_or_completed_PERF")
        s["frame0_source"] = points.get(0, {}).get("source_event")
        s["last_completed_source"] = s["perf_reports"][-1]["source_event"] if s["perf_reports"] else None
        s["replayable"] = not s["hold_reasons"]
        s["status"] = "CANDIDATE" if s["replayable"] else "HOLD"
        s["exact_proof"] = False
        s["input_prefix_sha256"] = digest(json.dumps([[p["frame"], p["mask"]] for p in events],
                                          separators=(",", ":")).encode())
        s["terminal_release"] = {"frame": s["last_completed_frame"], "mask": 0,
                                 "generated": True, "outside_observed_prefix": True,
                                 "measured_input": False}
        s["observed_gaps"] = [gap for scope in (analysis or {}).get("sessions", [])
                              if scope["identity"].get("crc32") == s["crc32"] and
                              scope.get("generation") == s["source_generation"]
                              for gap in scope.get("discontinuities", [])]
        s["roi_candidates"] = select_rois(s, analysis) if analysis else []
        s["limitations"] = list(LIMITATIONS)
        if s["start_reason"] == "known_fresh_reload_candidate":
            s["limitations"].append("Fresh reload inferred from current P4 MODE/new-generation/frame0 contract, not a captured state oracle.")
    return {"schema": SCHEMA, "capture_id": capture_id, "source_guid": guid,
            "source_raw": {"bytes": len(raw), "sha256": guid},
            "segments": segments, "excluded_launch_attempts": excluded,
            "parse_issues": issues, "limitations": list(LIMITATIONS),
            "coverage": [{"session_id": s["session_id"], "crc32": s["crc32"],
                          "generation": s["source_generation"], "frame0_observed": s["frame0_source"] is not None,
                          "last_completed_frame": s["last_completed_frame"],
                          "observed_rows": len(s["observed_inputs"]), "committed_events": len(s["events"]),
                          "uncommitted_rows": len(s["uncommitted_tail"]),
                          "duplicate_frames": s["duplicate_frames"], "status": s["status"]}
                         for s in segments]}


def directional_frames(events, begin, end):
    """Считает кадры с удержанной direction mask; фактическое движение не выводится."""
    active, total, at = 0, 0, begin
    for p in events:
        if p["frame"] <= begin:
            active = p["mask"]
            continue
        if p["frame"] >= end:
            break
        if active & 240:
            total += p["frame"] - at
        active, at = p["mask"], p["frame"]
    return total + (end - at if active & 240 else 0)


def select_rois(segment, analysis, wanted=16):
    """Выбирает actual-aligned цепочки до16 окон; паузы/дырки не склеиваются.

    Moving = ранняя цепочка с direction input, heavy = минимальный weighted FPS.
    Оба scope остаются scene-unreviewed и не дают аппаратного доказательства.
    """
    windows = []
    for scope in analysis.get("sessions", []):
        if (scope["identity"].get("crc32") == segment["crc32"] and
                scope.get("generation") == segment["source_generation"] and
                segment["start_source"]["line"] <= scope["start_line"]):
            windows.extend(w for w in scope["windows"] if not w["ambiguous_gap"] and
                           not w["counter_anomalies"] and w["input_last"] < segment["last_completed_frame"])
    chains = []
    for w in sorted(windows, key=lambda item: item["input_first"]):
        if not chains or chains[-1][-1]["input_last"] + 1 != w["input_first"]:
            chains.append([])
        chains[-1].append(w)
    candidates = []
    for chain in chains:
        length = min(wanted, len(chain))
        for index in range(len(chain) - length + 1):
            part = chain[index:index + length]
            begin, end = part[0]["input_first"], part[-1]["input_last"] + 1
            movement = directional_frames(segment["events"], begin, end)
            if movement:
                candidates.append((part, movement))
    if not candidates or not segment["replayable"]:
        return []
    max_windows = max(len(p) for p, _ in candidates)
    candidates = [(p, m) for p, m in candidates if len(p) == max_windows]
    moving = min(candidates, key=lambda pair: pair[0][0]["input_first"])
    heavy = min(candidates, key=lambda pair: 120e6 * len(pair[0]) / sum(w["elapsed_us"] for w in pair[0]))
    result = []
    for label, (part, movement) in (("moving_input_candidate", moving), ("sustained_heavy_candidate", heavy)):
        result.append({"kind": label, "roi_input0": part[0]["input_first"],
                       "roi_input_last": part[-1]["input_last"],
                       "baseline_report": part[0]["accepted_first"] - 1,
                       "roi_reports": [w["accepted_last"] for w in part],
                       "window_frames": 120, "window_count": len(part),
                       "requested_windows": wanted, "short_coverage": len(part) < wanted,
                       "direction_input_frames": movement, "scene_verified": False,
                       "mean_observed_accepted_fps": 120e6 * len(part) / sum(w["elapsed_us"] for w in part),
                       "source_windows": [{"line_first": w["line_first"], "line_last": w["line_last"]}
                                          for w in part]})
    return result


def write_bank(bank, destination):
    """Создаёт durable baseline; отличающийся существующий файл не перезаписывает."""
    path = Path(destination)
    content = (json.dumps(bank, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if path.exists():
        if path.read_bytes() == content:
            return
        raise FileExistsError(f"Immutable input bank already differs: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(content)


def main(argv=None):
    """CLI читает остановленный capture и пишет source-pinned baseline без устройства."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("human-playthrough-movies.json"))
    args = parser.parse_args(argv)
    rawpath = args.capture / "raw.log"
    statepath = args.capture / "state.json"
    state = json.loads(statepath.read_text(encoding="utf-8-sig"))
    if state.get("status") != "stopped":
        raise ValueError("Capture must be stopped before creating an immutable input bank")
    from analyze_playthrough import analyze_text
    raw = rawpath.read_bytes()
    bank = build_bank(raw, args.capture.name, analyze_text(raw.decode("utf-8"), "p4"))
    bank["sources"] = {name: pin(args.capture / name) for name in
                       ("raw.log", "state.json", "events.jsonl", "chunks.jsonl")
                       if (args.capture / name).exists()}
    bank["export_tools"] = {name: pin(Path(__file__).with_name(name)) for name in
                            ("export_playthrough_movies.py", "playthrough_log.py", "analyze_playthrough.py")}
    bank["capture_bounds"] = {key: state.get(key) for key in
                             ("started_utc", "ended_utc", "complete_lines", "parse_errors",
                              "commands_sent", "observed_boots", "game_launches", "firmware_identity")}
    write_bank(bank, args.output)
    print(json.dumps({"output": str(args.output.resolve()), "source_guid": bank["source_guid"],
                      "segments": len(bank["segments"]), "candidate": sum(s["replayable"] for s in bank["segments"]),
                      "events": sum(len(s["events"]) for s in bank["segments"])}, ensure_ascii=False))


if __name__ == "__main__":
    main()



