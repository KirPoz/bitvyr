"""Потоковый HOST-отчёт UART-сессии; не измеряет физический scanout или p99.

Вход: sessiondir/events.jsonl с schema=1 envelopes capture-инструмента.
Выход создаётся только в новом каталоге. TIMES/LOOP сохраняются отдельно:
их 120-кадровые интервалы независимы и никогда не складываются.
"""
import argparse
from collections import Counter, OrderedDict, deque
import csv
import hashlib
import heapq
import json
import math
from pathlib import Path


MAX_EVENT_BYTES = 262144
MAX_RECENT = 512
MAX_IDENTITIES = 256
TIMES = {"NES_OUTPUT_FRAME_PIPELINE_TIMES", "NES_FRAME_PIPELINE_BENCH"}
STATUSES = {"NES_OUTPUT_FRAME_PIPELINE_STATUS", "NES_FRAME_PIPELINE_BENCH_STATUS",
            "NES_CPU_BENCH_STATUS", "NES_COUPLED_BENCH_STATUS", "NES_STATUS"}
ROM_NAME_RECORDS = {"NES_GAME_START", "NES_ROM"}
ROM_IDENTITY_RECORDS = ROM_NAME_RECORDS | {
    "NES_OUTPUT_FRAME_PIPELINE_STATUS", "NES_FRAME_PIPELINE_BENCH_STATUS",
    "NES_CPU_BENCH_STATUS", "NES_COUPLED_BENCH_STATUS"}
AUDIO = {"NES_AUDIO", "NES_FRAME_AUDIO_STATUS"}
OBSERVER_RESPONSES = STATUSES | AUDIO | {"NES_TASKS", "NES_TASK", "NES_HEAP",
    "NES_FRAME_PIPELINE_PRESENT_STATUS", "NES_SETTINGS", "NES_UI_STATUS"}
COMMON = ["event", "segment", "boot_epoch", "observed_elf_prefix", "host_utc", "host_monotonic_ns", "generation", "game",
          "crc32", "rom_name", "region"]
STAGES = ("core_us", "producer_wall_us", "consumer_active_us", "dsp_us", "present_us", "audio_us", "wait_us")
WINDOW = COMMON + ["window", "quality", "source", "accepted_first", "accepted_last",
                   "submitted_first", "submitted_last", "frames", "device_start_ms",
                   "device_end_ms", "elapsed_ms", "fps", "classification", "metric_event",
                   "observed_pressure", "pressure_budget_us", "loop_metric_event", "loop_wall_max_us",
                   "loop_wall_max_accepted_frame", "loop_join_quality"] + list(STAGES)
COUNTERS = {"accepted_frames": "audio_accepted_frames", "accepted": "audio_accepted_frames",
            "dma_frames": "audio_dma_frames", "dma": "audio_dma_frames",
            "missed_refill_blocks": "missed_blocks", "missed_blocks": "missed_blocks",
            "missed_refill_frames": "missed_frames", "missed_frames": "missed_frames"}


def integer(value):
    """Принимает целое счётчика; bool/float не превращаются в номер кадра."""
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


def number(value):
    """Отвергает NaN/infinity и логические значения метрики."""
    if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
        return value
    return None


def crc_text(value):
    """Сохраняет CRC как идентичность, а не индекс текущего каталога."""
    if isinstance(value, str):
        text = value.lower().removeprefix("0x")
        if len(text) == 8 and all(c in "0123456789abcdef" for c in text):
            return text
    if integer(value) is not None and value <= 0xffffffff:
        return f"{value:08x}"
    return ""


class Report:
    """Один активный owner, bounded recent-кэш; все строки пишутся сразу в CSV."""
    def __init__(self, output, dip, severe, max_gap_ms, stall_ms):
        """Создаёт новые потоки отчёта и bounded owner state; исходная сессия остаётся read-only."""
        self.output, self.dip, self.severe = output, dip, severe
        self.max_gap_ms, self.stall_us = max_gap_ms, stall_ms * 1000
        self.files, self.writers = [], {}
        schemas = {
            "metrics": COMMON + ["raw_type", "kind", "scope", "accepted_first", "accepted_last",
                                  "samples", "quality", "fields_json"],
            "windows": WINDOW, "dips": WINDOW + ["dip_kind", "loop_max_us", "observed_accepted_frame"],
            "timeline": COMMON + ["type", "phase", "text", "known_accepted_first", "known_accepted_last"],
            "joins": ["segment", "generation", "game", "accepted_first", "accepted_last",
                      "times_event", "loop_event", "quality"],
            "audio": COMMON + ["quality", "delta_json", "counters_json", "max_gap_us", "max_wait_us"],
            "resources": COMMON + ["raw_type", "resource_kind", "fields_json", "quality"],
            "task_runtime": COMMON + ["task_id", "task_name", "core_affinity", "core_label",
                "run_delta_us", "wall_delta_us", "task_runtime_pct_of_wall", "stack_min_bytes", "quality"],
        }
        for name, fields in schemas.items():
            file = (output / (name + ".csv")).open("w", encoding="utf-8", newline="")
            self.files.append(file)
            writer = csv.DictWriter(file, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            self.writers[name] = writer
        self.counts, self.qualities, self.roms = Counter(), Counter(), OrderedDict()
        self.counts.update(dict(windows=0, segments=0, dip_windows=0, main_loop_stalls=0, audio_records=0))
        self.catalog, self.recent, self.joined, self.seen = OrderedDict(), OrderedDict(), set(), OrderedDict()
        self.segment, self.boot, self.ctx = 0, 0, {}
        self.boundary = self.perf = self.audio = None
        self.known_range = (None, None)
        self.last_host = None
        self.last_device = None
        self.observers, self.paused = 0, False
        self.observer_recovery = 0
        self.task_batch = self.task_previous = None
        self.last_tasks, self.last_heap = [], {}
        self.capture_end, self.firmware_prefixes = {}, OrderedDict()
        self.elf_prefix = ""
        self.worst = []
        self.event = 0
        self.timeline_sample = []
        self.timeline_tail = deque(maxlen=20)

    def close(self):
        """Закрывает только собственные новые файлы, включая аварийный выход."""
        for file in self.files:
            file.close()

    def common(self, envelope):
        """Добавляет только известный контекст; отсутствующая идентичность остаётся пустой."""
        return dict(event=self.event, segment=self.segment, boot_epoch=self.boot, observed_elf_prefix=self.elf_prefix,
                    host_utc=envelope.get("host_utc", ""),
                    host_monotonic_ns=envelope.get("host_monotonic_ns", ""),
                    **{key: self.ctx.get(key, "") for key in ("generation", "game", "crc32", "rom_name", "region")})

    def timeline(self, envelope, kind, text, phase=""):
        """Маркеры привязаны только к receipt time и последнему известному диапазону."""
        row = self.common(envelope)
        row.update(type=kind, phase=phase, text=text, known_accepted_first=self.known_range[0],
                   known_accepted_last=self.known_range[1])
        self.writers["timeline"].writerow(row)
        self.counts["timeline_rows"] += 1
        if len(self.timeline_sample) < 20:
            self.timeline_sample.append(row)
        self.timeline_tail.append(row)

    def invalidate(self, envelope, reason, *, preserve_audio=False):
        """Не соединяет rate/audio интервалы через неизвестный lifecycle-разрыв."""
        self.boundary = self.perf = None
        if not preserve_audio:
            self.audio = None
        self.recent.clear()
        self.joined.clear()
        self.known_range = (None, None)
        self.timeline(envelope, "boundary_invalidated", reason)

    def start(self, envelope, generation, game, reason):
        """Новый scope не наследует rate, аудио-дельты или диапазоны прежнего owner."""
        self.segment += 1
        self.ctx = {"generation": generation, "game": game}
        metadata = self.catalog.get(game, {}) if game is not None else {}
        self.ctx.update(metadata)
        self.task_batch = self.task_previous = None
        self.invalidate(envelope, reason)
        self.counts["segments"] += 1

    def identity(self, fields, raw_type):
        """Дополняет ROM metadata только доверенными ROM-записями, не именами RTOS tasks."""
        crc = crc_text(fields.get("crc32", fields.get("crc"))) if raw_type in ROM_IDENTITY_RECORDS else ""
        name = fields.get("title", fields.get("name")) if raw_type in ROM_NAME_RECORDS else None
        if crc:
            self.ctx["crc32"] = crc
        if isinstance(name, str):
            self.ctx["rom_name"] = name[:1024]
        if raw_type in ROM_IDENTITY_RECORDS | TIMES and isinstance(fields.get("region"), str):
            self.ctx["region"] = fields["region"]

    def scope(self, envelope, fields, raw_type):
        """Не принимает позднюю строку старого поколения за возврат в старую игру."""
        generation, game = integer(fields.get("generation")), integer(fields.get("game"))
        old_gen, old_game = self.ctx.get("generation"), self.ctx.get("game")
        if generation is not None and old_gen is not None and generation < old_gen:
            self.counts["stale_generation_records"] += 1
            self.timeline(envelope, "ignored", "late record from older generation")
            return False
        if generation is not None and old_gen is not None and generation == old_gen and game is not None and old_game is not None and game != old_game:
            self.counts["scope_conflicts"] += 1
            self.invalidate(envelope, "same generation has conflicting game index")
            return False
        known_crc = self.ctx.get("crc32")
        supplied_crc = crc_text(fields.get("crc32", fields.get("crc"))) if raw_type in ROM_IDENTITY_RECORDS else ""
        if (known_crc and supplied_crc and known_crc != supplied_crc
                and generation in (None, old_gen) and game in (None, old_game)):
            self.counts["scope_conflicts"] += 1
            self.invalidate(envelope, "ROM CRC changed inside scope")
            return False
        if self.ctx and old_gen is None and generation is not None and (game is None or old_game in (None, game)):
            self.ctx["generation"] = generation
        elif not self.ctx or (generation is not None and generation != old_gen):
            self.start(envelope, generation, game, "generation change")
        if game is not None and self.ctx.get("game") is None:
            self.ctx["game"] = game
            self.ctx.update(self.catalog.get(game, {}))
        if supplied_crc and self.ctx.get("crc32") not in (None, "", supplied_crc):
            self.ctx.pop("rom_name", None)
        self.identity(fields, raw_type)
        return True

    def metric(self, envelope, raw_type, kind, fields, quality="published_fields"):
        """Записывает исходные величины отдельно, не создавая CPU/PPU-разбивки из суммы."""
        n, end = integer(fields.get("samples")), integer(fields.get("frames"))
        first = integer(fields.get("accepted_first"))
        last = integer(fields.get("accepted_last"))
        if n is not None and end is not None and end >= n and n > 0:
            first, last = end - n + 1, end
        elif raw_type in STATUSES and end is not None:
            first = last = end
        row = self.common(envelope)
        row.update(raw_type=raw_type, kind=kind, scope="independent_published_interval",
                   accepted_first=first, accepted_last=last, samples=n, quality=quality,
                   fields_json=json.dumps(fields, ensure_ascii=False, separators=(",", ":")))
        self.writers["metrics"].writerow(row)
        if first is not None and last is not None and self.ctx.get("generation") is not None and self.ctx.get("game") is not None:
            self.known_range = (first, last)
            if raw_type in TIMES or raw_type == "NES_FRAME_LOOP_METRICS":
                key = (self.segment, self.ctx["generation"], self.ctx["game"], first, last)
                entry = self.recent.setdefault(key, {})
                entry["times" if raw_type in TIMES else "loop"] = self.event
                if raw_type == "NES_FRAME_LOOP_METRICS":
                    entry["loop_wall_max_us"] = fields.get("loop_wall_max_us")
                    entry["loop_wall_max_accepted_frame"] = fields.get("loop_wall_max_accepted_frame")
                    for _, _, kept in self.worst:
                        if kept.get("range_key") == key:
                            self.attach_loop(kept, entry)
                if "times" in entry and "loop" in entry and key not in self.joined:
                    self.joined.add(key)
                    self.writers["joins"].writerow(dict(segment=key[0], generation=key[1], game=key[2],
                        accepted_first=first, accepted_last=last, times_event=entry["times"],
                        loop_event=entry["loop"], quality="same_accepted_range_independent_metrics_not_additive"))
                while len(self.recent) > MAX_RECENT:
                    retired, _ = self.recent.popitem(last=False)
                    self.joined.discard(retired)

    def attach_loop(self, row, entry):
        """Добавляет LOOP только при уже проверенном exact accepted-range key."""
        if "loop" in entry:
            row.update(loop_metric_event=entry["loop"], loop_wall_max_us=entry.get("loop_wall_max_us"),
                       loop_wall_max_accepted_frame=entry.get("loop_wall_max_accepted_frame"),
                       loop_join_quality="exact_accepted_range_independent_metric")

    def window(self, envelope, row):
        """Агрегирует только rate одного качества; ни host receipt, ни LOOP не дают FPS."""
        self.counts["windows"] += 1
        self.qualities[row["quality"]] += 1
        row = dict(self.common(envelope), **row, window=self.counts["windows"])
        fps = row["fps"]
        row["classification"] = "severe" if fps < self.severe else "dip" if fps < self.dip else "above_threshold"
        producer = number(row.get("producer_wall_us"))
        region = self.ctx.get("region")
        budget = 16639 if region == "NTSC" else 19997 if region == "PAL" else None
        row["pressure_budget_us"] = budget
        pressure = "unknown_missing_region_or_C1_producer_mean"
        if producer is not None and producer >= 0 and budget is not None:
            if producer > budget:
                pressure = "mean_C1_producer_exceeds_region_budget"
            elif fps < self.dip:
                pressure = "low_submission_rate_C1_mean_in_budget_main_periphery_or_pause_unknown"
            else:
                pressure = "no_C1_mean_overrun_observed_in_this_window"
        row["observed_pressure"] = pressure
        key = (self.segment, self.ctx.get("generation"), self.ctx.get("game"), row.get("accepted_first"), row.get("accepted_last"))
        row["range_key"] = key
        row["loop_join_quality"] = "separate_or_unavailable_not_aligned"
        self.attach_loop(row, self.recent.get(key, {}))
        self.writers["windows"].writerow(row)
        heapq.heappush(self.worst, (-fps, -row["window"], dict(row)))
        if len(self.worst) > 10:
            heapq.heappop(self.worst)
        if fps < self.dip:
            self.counts["dip_windows"] += 1
            self.writers["dips"].writerow(dict(row, dip_kind="interval_average_rate"))
        identity = (self.ctx.get("crc32") or self.ctx.get("rom_name")
                    or f"unknown_boot{self.boot}_game{self.ctx.get('game')}_segment{self.segment}")
        key = (identity, row["quality"])
        if key not in self.roms and len(self.roms) >= MAX_IDENTITIES:
            key = ("other_identities_bounded_overflow", row["quality"])
        stat = self.roms.setdefault(key, dict(identity=key[0], rom_name=self.ctx.get("rom_name", ""), quality=key[1], windows=0, frames=0,
            elapsed_ms=0.0, min_interval_fps=fps, max_interval_fps=fps, dip_windows=0))
        stat["windows"] += 1
        stat["frames"] += row["frames"]
        stat["elapsed_ms"] += row["elapsed_ms"]
        stat["min_interval_fps"] = min(stat["min_interval_fps"], fps)
        stat["max_interval_fps"] = max(stat["max_interval_fps"], fps)
        stat["dip_windows"] += fps < self.dip

    def times(self, envelope, record, fields):
        """Два настоящих TIMES boundaries дают только coarse submission estimate."""
        if self.observers or self.paused:
            self.boundary = None
            self.counts["observed_or_paused_boundaries"] += 1
            return
        if self.observer_recovery:
            self.observer_recovery -= 1
            self.boundary = None
            self.counts["observer_quarantine_boundaries"] += 1
            return
        now = dict(generation=integer(fields.get("generation")), game=integer(fields.get("game")),
                   accepted=integer(fields.get("frames")), submitted=integer(fields.get("submitted_frames")),
                   produced=integer(fields.get("produced_frames")),
                   samples=integer(fields.get("samples")), ms=integer(record.get("device_timestamp_ms")))
        previous = self.boundary
        self.boundary = now
        if any(now[key] is None for key in now) or now["samples"] != 120:
            self.counts["incomplete_times_boundaries"] += 1
            self.boundary = None
            return
        if previous is None:
            self.counts["first_boundaries_not_windows"] += 1
            return
        if previous["generation"] != now["generation"] or previous["game"] != now["game"]:
            self.counts["mixed_scope_intervals"] += 1
            return
        da, ds, dp, dt = (now["accepted"] - previous["accepted"], now["submitted"] - previous["submitted"],
                          now["produced"] - previous["produced"], now["ms"] - previous["ms"])
        if da != 120 or ds != 120 or dp != 120 or dt <= 0 or dt > self.max_gap_ms:
            self.counts["rejected_boundary_intervals"] += 1
            self.timeline(envelope, "uncertain_interval", f"TIMES deltas accepted={da}, produced={dp}, submitted={ds}, device_ms={dt}")
            return
        self.window(envelope, dict(quality="observational_report_boundaries", source=record["raw_type"],
            accepted_first=previous["accepted"] + 1, accepted_last=now["accepted"],
            submitted_first=previous["submitted"] + 1, submitted_last=now["submitted"], frames=ds,
            device_start_ms=previous["ms"], device_end_ms=now["ms"], elapsed_ms=dt, fps=ds * 1000.0 / dt,
            metric_event=self.event, **{key: fields.get(key) for key in STAGES}))

    def perf_window(self, envelope, fields):
        """Сохраняет настоящий firmware-reported PERF average, отдельно от Output estimate."""
        if self.observers or self.paused:
            self.perf = None
            return
        if self.observer_recovery:
            self.observer_recovery -= 1
            self.perf = None
            self.counts["observer_quarantine_boundaries"] += 1
            return
        end, fps = integer(fields.get("frames")), number(fields.get("fps"))
        previous = self.perf
        self.perf = end
        if end is None or fps is None or fps <= 0 or previous is None:
            return
        if end - previous != 120 or self.ctx.get("game") is None:
            self.counts["rejected_perf_intervals"] += 1
            return
        self.window(envelope, dict(quality="firmware_reported_perf_average", source="NES_PERF",
            accepted_first=previous + 1, accepted_last=end, frames=120, elapsed_ms=120000.0 / fps,
            fps=fps, metric_event=self.event))

    def audio_record(self, envelope, fields):
        """Дельты Board PCM-counters допустимы лишь без уменьшения счётчиков и смены scope."""
        current = {target: integer(fields[key]) for key, target in COUNTERS.items()
                   if key in fields and integer(fields[key]) is not None}
        quality, delta = "baseline_or_missing_unknown", {}
        if self.audio is not None and current and set(current) == set(self.audio):
            if all(current[k] >= self.audio[k] for k in current):
                quality = "counter_delta_same_segment"
                delta = {k: current[k] - self.audio[k] for k in current}
            else:
                quality = "counter_reset_or_wrap_unknown"
                self.counts["audio_counter_resets"] += 1
        self.audio = current or None
        self.counts["audio_records"] += 1
        row = self.common(envelope)
        row.update(quality=quality, delta_json=json.dumps(delta), counters_json=json.dumps(current),
                   max_gap_us=fields.get("max_write_gap_us", fields.get("max_gap_us", "")),
                   max_wait_us=fields.get("max_write_wait_us", fields.get("max_wait_us", "")))
        self.writers["audio"].writerow(row)

    def task_record(self, envelope, raw_type, fields):
        """Считает task runtime только между полными adjacent snapshots с теми же ID/name."""
        if raw_type == "NES_TASKS":
            if self.task_batch is not None:
                self.counts["incomplete_task_snapshots"] += 1
                self.task_previous = None
            total, count = integer(fields.get("total_us")), integer(fields.get("count"))
            self.task_batch = None
            if total is None or count is None or not 0 < count <= MAX_IDENTITIES:
                self.task_previous = None
                self.counts["unavailable_task_snapshots"] += 1
                return
            self.task_batch = dict(total=total, count=count, tasks={})
            return
        if raw_type != "NES_TASK" or self.task_batch is None:
            return
        task_id, run = integer(fields.get("id")), integer(fields.get("run_us"))
        name = fields.get("name")
        batch = self.task_batch
        if task_id is None or run is None or not isinstance(name, str) or task_id in batch["tasks"]:
            self.counts["invalid_task_snapshots"] += 1
            self.task_batch = self.task_previous = None
            return
        batch["tasks"][task_id] = fields
        if len(batch["tasks"]) != batch["count"]:
            return
        current, previous = batch, self.task_previous
        self.last_tasks = []
        for task_id, task in current["tasks"].items():
            old = previous["tasks"].get(task_id) if previous else None
            wall = current["total"] - previous["total"] if previous else None
            delta = task["run_us"] - old["run_us"] if old else None
            quality = "task_baseline_or_new_identity_unknown"
            percent = None
            if old is not None and old["name"] == task["name"] and wall is not None and wall > 0:
                if delta is not None and 0 <= delta <= 2 * wall:
                    quality = "task_cumulative_delta_not_core_or_ppu_split"
                    percent = 100.0 * delta / wall
                else:
                    quality = "task_counter_reset_or_wrap_unknown"
                    delta = None
            else:
                delta = None
                if previous is not None and wall is not None and wall <= 0:
                    quality, wall = "task_total_counter_reset_or_wrap_unknown", None
            core = task.get("core", "")
            row = self.common(envelope)
            row.update(task_id=task_id, task_name=task["name"], core_affinity=core,
                core_label="no_affinity" if core == 2147483647 else str(core),
                run_delta_us=delta, wall_delta_us=wall, task_runtime_pct_of_wall=percent,
                stack_min_bytes=integer(task.get("stack_min")), quality=quality)
            self.writers["task_runtime"].writerow(row)
            self.last_tasks.append(row)
        self.counts["complete_task_snapshots"] += 1
        self.task_previous, self.task_batch = current, None

    def consume(self, envelope):
        """Обрабатывает один capture envelope; stale/duplicate строки не образуют окна."""
        self.event += 1
        if not isinstance(envelope, dict) or envelope.get("schema") != 1:
            self.counts["invalid_envelopes"] += 1
            self.invalidate({}, "invalid envelope")
            return
        host = integer(envelope.get("host_monotonic_ns"))
        if host is not None and self.last_host is not None and host < self.last_host:
            self.counts["out_of_order_host_events"] += 1
            self.invalidate(envelope, "out-of-order host receipt; no rate bridge")
            return
        if host is not None:
            self.last_host = host
        record = envelope.get("record")
        if record is None and isinstance(envelope.get("serial_line"), str):
            try:
                from playthrough_metrics import parse_line
            except ImportError:
                from tools.playthrough_metrics import parse_line
            record = parse_line(envelope["serial_line"])
        if not isinstance(record, dict) or not isinstance(record.get("fields"), dict):
            self.counts["unparsed_events"] += 1
            return
        kind, raw_type, fields = record.get("kind", ""), record.get("raw_type", ""), record["fields"]
        self.counts["parsed_events"] += 1
        if kind == "capture_end":
            self.capture_end = {key: fields.get(key) for key in ("reason", "exit_code", "trailing_incomplete_line")}
            self.timeline(envelope, "capture_end", str(self.capture_end))
            return
        if kind == "firmware":
            prefix = fields.get("elf_sha256_prefix", "")
            if isinstance(prefix, str) and 8 <= len(prefix) <= 64 and all(c in "0123456789abcdefABCDEF" for c in prefix):
                self.elf_prefix = prefix.lower()
                self.firmware_prefixes[self.elf_prefix] = None
                while len(self.firmware_prefixes) > 16:
                    self.firmware_prefixes.popitem(last=False)
            self.boot += 1
            self.last_device = None
            self.catalog.clear()
            self.ctx = {}
            self.start(envelope, None, None, "observed firmware boot identity")
            return
        if kind == "marker":
            self.timeline(envelope, "manual_marker", str(fields.get("text", ""))[:4096], fields.get("phase", "note"))
            if fields.get("phase") in {"pause", "resume"}:
                self.paused = fields["phase"] == "pause"
                self.invalidate(envelope, "manual " + fields["phase"])
            return
        if kind in {"observer_begin", "observer_end"}:
            if kind == "observer_begin":
                self.observers += 1
            elif self.observers:
                self.observers -= 1
            else:
                self.counts["unmatched_observer_ends"] += 1
            if kind == "observer_end" and fields.get("scope") != "device_completion":
                self.observer_recovery = 2
            self.invalidate(envelope, kind, preserve_audio=True)
            return
        if kind == "parse_error":
            self.counts["parser_errors"] += 1
            self.invalidate(envelope, "parser error: " + str(fields.get("error", "")))
            return
        device = integer(record.get("device_timestamp_ms"))
        if device is not None:
            if self.last_device is not None and device < self.last_device:
                self.boot += 1
                self.catalog.clear()
                self.ctx = {}
                self.elf_prefix = ""
                self.start(envelope, None, None, "device log clock regressed; inferred reset/new scope")
            self.last_device = device
        if raw_type == "NES_ROM":
            index = integer(fields.get("index"))
            if index is not None:
                metadata = {"crc32": crc_text(fields.get("crc32")), "rom_name": str(fields.get("title", ""))[:1024]}
                self.catalog[index] = metadata
                while len(self.catalog) > MAX_IDENTITIES:
                    self.catalog.popitem(last=False)
            return
        if kind in {"boot", "reset"} or raw_type == "NES_RUNTIME_READY":
            self.boot += 1
            self.catalog.clear()
            self.ctx = {}
            if kind in {"boot", "reset"}:
                self.elf_prefix = ""
            self.paused = False
            self.observer_recovery = 0
            self.start(envelope, integer(fields.get("generation")), None, "boot/reset/runtime-ready boundary")
            return
        if raw_type == "NES_GAME_START":
            self.start(envelope, integer(fields.get("generation")), integer(fields.get("index")), "game launch")
            self.identity(fields, raw_type)
            game = self.ctx.get("game")
            if game is not None:
                self.catalog[game] = {key: self.ctx[key] for key in ("crc32", "rom_name", "region") if key in self.ctx}
                while len(self.catalog) > MAX_IDENTITIES:
                    self.catalog.popitem(last=False)
            return
        if not self.scope(envelope, fields, raw_type):
            return
        if raw_type in OBSERVER_RESPONSES:
            # Host observer_end означает только отправку команд. Каждый реальный
            # response повторно начинает quarantine; ответы могут приходить поздно.
            self.observer_recovery = 2
            self.invalidate(envelope, "actual observer response: " + raw_type, preserve_audio=True)
        if (fields.get("playing") is False or fields.get("owner") == "unbound"
                or kind in {"pause", "resume", "stop"}
                or (raw_type.endswith(("_SETUP", "_MODE", "_FAULT", "_FAIL", "_ERROR")))
                or raw_type == "NES_STATUS_ERROR"
                or (raw_type == "NES_INPUT" and (fields.get("action") in {"settings", "exit"} or fields.get("frame") == 0))):
            self.invalidate(envelope, "observed pause/resume/playing=false boundary")
        if kind == "pause":
            self.paused = True
        elif kind == "resume":
            self.paused = False
        fingerprint = (self.segment, raw_type, json.dumps(fields, sort_keys=True, ensure_ascii=False), record.get("device_timestamp_ms"))
        digest = hashlib.sha256(repr(fingerprint).encode()).digest()
        rate_record = raw_type in TIMES or raw_type in {"NES_PERF", "NES_FRAME_LOOP_METRICS"}
        if rate_record and digest in self.seen:
            self.counts["duplicate_records"] += 1
            return
        self.seen[digest] = None
        while len(self.seen) > MAX_RECENT:
            self.seen.popitem(last=False)
        self.metric(envelope, raw_type, kind, fields)
        if raw_type in STATUSES or any(token in kind for token in ("task", "heap", "resource")):
            self.writers["resources"].writerow(dict(self.common(envelope), raw_type=raw_type,
                resource_kind=kind, fields_json=json.dumps(fields, ensure_ascii=False),
                quality="published_resource_fields_no_derived_cpu_utilization"))
        self.task_record(envelope, raw_type, fields)
        if raw_type == "NES_HEAP":
            self.last_heap = dict(fields)
        if raw_type in TIMES:
            self.times(envelope, record, fields)
        elif raw_type == "NES_PERF":
            self.perf_window(envelope, fields)
        elif raw_type in AUDIO:
            self.audio_record(envelope, fields)
        elif raw_type == "NES_FRAME_LOOP_METRICS":
            maximum = number(fields.get("loop_wall_max_us"))
            if maximum is not None and maximum >= self.stall_us:
                row = self.common(envelope)
                row.update(quality="observed_main_loop_max_not_scanout", source=raw_type,
                           dip_kind="main_loop_stall", loop_max_us=maximum,
                           observed_accepted_frame=fields.get("loop_wall_max_accepted_frame", ""))
                self.writers["dips"].writerow(row)
                self.counts["main_loop_stalls"] += 1


def small_json(path):
    """Читает ограниченные capture metadata; отсутствие/повреждение остаётся unknown."""
    try:
        with path.open("rb") as file:
            data = file.read(MAX_EVENT_BYTES + 1)
        if len(data) > MAX_EVENT_BYTES:
            return {}
        value = json.loads(data.decode("utf-8-sig"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError, UnicodeError, RecursionError):
        return {}


def capture_context(session_dir, report):
    """Отделяет закрытие транспорта/частичный input и объявленный ELF от observed prefix."""
    metadata = small_json(session_dir / "session.json")
    status = small_json(session_dir / "status.json")
    end = dict(report.capture_end)
    end.update({key: status[key] for key in ("phase", "reason", "exit_code", "trailing_incomplete_line") if key in status})
    reason = str(end.get("reason", "unknown"))
    partial = (status.get("phase") != "closed" or end.get("exit_code") != 0
               or bool(end.get("trailing_incomplete_line"))
               or any(token in reason.lower() for token in ("error", "disconnect", "limit", "kill"))
               or any(report.counts[key] for key in ("partial_final_lines", "malformed_jsonl", "oversized_events")))
    expected = metadata.get("expected_elf")
    prefixes = list(report.firmware_prefixes)
    identity = "not_observed_unknown"
    if prefixes:
        if isinstance(expected, str) and all(expected.lower().startswith(prefix) for prefix in prefixes):
            identity = "observed_full_hash_match" if all(len(prefix) == 64 for prefix in prefixes) else "observed_prefix_match_only"
        else:
            identity = "observed_prefixes_expected_missing_or_mismatch"
    return dict(transport=metadata.get("transport", "unknown"),
        expected_elf=expected, expected_elf_is_observed=False,
        observed_elf_prefixes=prefixes, firmware_identity_quality=identity,
        completion=end, capture_scope="partial_or_unknown_capture" if partial else "closed_recorded_session_not_full_game_completion")


def analyze(session_dir, output=None, *, dip_fps=58.0, severe_fps=50.0,
            max_gap_ms=10000, stall_ms=50.0):
    """Создаёт новый отчёт; входы и существующие результаты никогда не перезаписывает."""
    if not (all(number(x) is not None for x in (severe_fps, dip_fps, max_gap_ms, stall_ms))
            and 0 < severe_fps <= dip_fps and max_gap_ms > 0 and stall_ms > 0):
        raise ValueError("invalid thresholds")
    session_dir = Path(session_dir).resolve()
    events = session_dir / "events.jsonl"
    if not events.is_file():
        raise FileNotFoundError(events)
    output = Path(output).resolve() if output is not None else session_dir / "report"
    output.mkdir(parents=True, exist_ok=False)
    report = Report(output, dip_fps, severe_fps, max_gap_ms, stall_ms)
    digest = hashlib.sha256()
    try:
        with events.open("rb") as file:
            while True:
                line = file.readline(MAX_EVENT_BYTES + 1)
                if not line:
                    break
                digest.update(line)
                if len(line) > MAX_EVENT_BYTES:
                    while not line.endswith(b"\n"):
                        line = file.readline(MAX_EVENT_BYTES + 1)
                        if not line:
                            break
                        digest.update(line)
                    report.counts["oversized_events"] += 1
                    report.invalidate({}, "oversized event skipped")
                    continue
                if not line.endswith(b"\n"):
                    report.counts["partial_final_lines"] += 1
                    report.invalidate({}, "unterminated final JSONL line skipped")
                    break
                try:
                    envelope = json.loads(line.decode("utf-8-sig"))
                except (ValueError, UnicodeError, RecursionError):
                    report.counts["malformed_jsonl"] += 1
                    report.invalidate({}, "malformed JSONL skipped")
                    continue
                report.consume(envelope)
        if report.task_batch is not None:
            report.counts["incomplete_task_snapshots"] += 1
        roms = list(report.roms.values())
        for stat in roms:
            stat["aggregate_interval_fps"] = stat["frames"] * 1000.0 / stat["elapsed_ms"]
        summary = dict(schema=1, status="complete_analysis_not_hardware_acceptance",
            input_events_sha256=digest.hexdigest(), session_dir=str(session_dir),
            capture=capture_context(session_dir, report),
            thresholds=dict(dip_fps=dip_fps, severe_fps=severe_fps, max_gap_ms=max_gap_ms, stall_ms=stall_ms),
            counts=dict(report.counts), window_qualities=dict(report.qualities), roms=roms,
            last_task_snapshot=report.last_tasks, last_heap_snapshot=report.last_heap,
            worst_windows=[{key: value for key, value in row.items() if key != "range_key"}
                           for _, _, row in sorted(report.worst, key=lambda item: (-item[0], -item[1]))],
            audio_observations="recorded_counters_epoch_not_proven" if report.counts["audio_records"] else "missing_unknown",
            limits=["120-frame interval averages, not per-frame FPS/p99 or physical VSYNC",
                    "Output quality observational_report_boundaries uses submission deltas/device-log milliseconds; not qualified native/input A/B",
                    "TIMES/LOOP independent intervals; never summed or joined on approximate ranges",
                    "Unlogged pauses/observer activity can contaminate even valid boundary estimates",
                    "Host receipt time used only for timeline/markers, never FPS",
                    "Game index is session-local; missing CRC/name means unknown ROM identity",
                    "Audio frames are Board PCM counters, not NES accepted frames; missing means unknown",
                    "Audio resets can occur inside a generation; nondecreasing counters alone do not prove unchanged activation epoch/atomic snapshot",
                    "observer_end is command-send only: actual responses and following boundary pair are excluded",
                    "Task runtime delta/wall is per task, SMP sum may approach 200%; affinity is not actual core execution",
                    "Task stack_min_bytes is lifetime minimum, heap minimum is cumulative; neither is instant free memory",
                    "No exact CPU-versus-PPU split derived; stage averages retained as published"],
            bounded_memory=dict(recent_records=MAX_RECENT, rom_identities=MAX_IDENTITIES,
                                note="CSV streamed; no all-window arrays"))
        (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        lines = ["# Анализ прохождения UART", "", "Это HOST-анализ журналов, не аппаратная приёмка.", "",
                 f"Transport: {summary['capture']['transport']}; capture: {summary['capture']['capture_scope']}; "
                 f"firmware identity: {summary['capture']['firmware_identity_quality']}.", "",
                 f"Окон: {report.counts['windows']}; просадок средних: {report.counts['dip_windows']}; "
                 f"наблюдаемых Main-loop stalls: {report.counts['main_loop_stalls']}.", "",
                 "Порог просадки: < " + str(dip_fps) + "; тяжёлой: < " + str(severe_fps) + " кадров/с.", "",
                 "Output48c: `observational_report_boundaries` =120/Δdevice_log_ms при "
                 "дельтах accepted/produced/submitted120. Первый boundary не окно. "
                 "Метод измеряет software submission rate; natural play не имеет native/input/observer квалификации A/B.", "",
                 "Каждая строка windows/dips содержит quality. Метрики TIMES/LOOP независимы; "
                 "их времена не складываются. Loop max относится к Main iteration, не scanout.", "",
                 "Полные потоки: metrics.csv, windows.csv, dips.csv, timeline.csv, joins.csv, audio.csv, resources.csv, task_runtime.csv. "
                 "CSV допускает миллионы окон без удержания их в RAM.", "", "## ROM и интервальные частоты", "",
                 "Это агрегаты 120-кадровых отчётных интервалов, не минимум отдельного кадра или p99.", "",
                 "| ROM identity / name | Quality | Окон | Частота агрегата | Min / max среднего окна | Просадок |",
                 "|---|---|---:|---:|---:|---:|"]
        for stat in roms:
            label = (stat["identity"] + " / " + stat["rom_name"]).replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {label} | {stat['quality']} | {stat['windows']} | {stat['aggregate_interval_fps']:.3f} | "
                         f"{stat['min_interval_fps']:.3f} / {stat['max_interval_fps']:.3f} | {stat['dip_windows']} |")
        lines += ["", "## Худшие средние окна (не более10)", "",
                  "Stage averages опубликованы тем же TIMES/metric_event. Они перекрываются и не складываются. "
                  "C1 producer включает CPU/PPU/APU/обвязку; превышение бюджета не устанавливает виновника или stalls.", "",
                  "| Game / accepted frames | FPS / quality | C1 core / producer, us | C0 colour / DSP, us | Present / audio / wait, us | Pressure | LOOP max / frame / alignment |",
                  "|---|---|---|---|---|---|---|"]
        for row in summary["worst_windows"]:
            lines.append(f"| {row.get('game', '')} / {row.get('accepted_first', '')}..{row.get('accepted_last', '')} | "
                f"{row['fps']:.3f} / {row['quality']} | {row.get('core_us')} / {row.get('producer_wall_us')} | "
                f"{row.get('consumer_active_us')} / {row.get('dsp_us')} | "
                f"{row.get('present_us')} / {row.get('audio_us')} / {row.get('wait_us')} | {row['observed_pressure']} | "
                f"{row.get('loop_wall_max_us')} / {row.get('loop_wall_max_accepted_frame')} / {row['loop_join_quality']} |")
        lines += ["", "## Первые события timeline", ""]
        for row in report.timeline_sample:
            text = str(row["text"]).replace("\n", " ").replace("\r", " ")
            lines.append(f"- {row['host_utc']} segment={row['segment']} {row['type']}: {text}")
        if report.counts["timeline_rows"] > 20:
            lines += ["", "## Последние события timeline", "", "Полная последовательность сохранена в timeline.csv.", ""]
            for row in report.timeline_tail:
                text = str(row["text"]).replace("\n", " ").replace("\r", " ")
                lines.append(f"- {row['host_utc']} segment={row['segment']} {row['type']}: {text}")
        lines += ["", "## Последний полный task snapshot", "",
                  "Runtime — дельта task CPU time / дельта total_us, не CPU/PPU breakdown. "
                  "SMP суммы могут достигать200%; stack_min — минимум lifetime, не текущая свободная память.", "",
                  "| Task | Affinity | Runtime % wall | Stack minimum, B | Quality |",
                  "|---|---|---:|---:|---|"]
        for row in report.last_tasks:
            name = str(row["task_name"]).replace("|", "\\|").replace("\n", " ")
            percent = row["task_runtime_pct_of_wall"]
            lines.append(f"| {name} | {row['core_label']} | {percent if percent is not None else 'unknown'} | "
                         f"{row['stack_min_bytes']} | {row['quality']} |")
        lines += ["", "Последний heap snapshot (как опубликовано, без причинной корреляции с FPS): "
                  + json.dumps(report.last_heap, ensure_ascii=False), ""]
        lines += ["", "## Ограничения", ""] + ["- " + item for item in summary["limits"]]
        (output / "REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return summary
    finally:
        report.close()


def main(argv=None):
    """CLI без доступа к UART: анализирует только уже сохранённые envelopes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session_dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dip-fps", type=float, default=58.0)
    parser.add_argument("--severe-fps", type=float, default=50.0)
    parser.add_argument("--max-gap-ms", type=int, default=10000)
    parser.add_argument("--stall-ms", type=float, default=50.0)
    args = parser.parse_args(argv)
    try:
        result = analyze(args.session_dir, args.output, dip_fps=args.dip_fps, severe_fps=args.severe_fps,
                         max_gap_ms=args.max_gap_ms, stall_ms=args.stall_ms)
    except (OSError, ValueError) as error:
        parser.exit(2, f"analysis refused: {error}\n")
    print(json.dumps(dict(status=result["status"], counts=result["counts"]), ensure_ascii=False))


if __name__ == "__main__":
    main()
