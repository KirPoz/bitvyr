"""Прогон P4+C6 по закреплённым игровым сценам с точным вводом."""
import argparse
import hashlib
import json
import math
import re
import time
from pathlib import Path

import device_session as base
import experiments
from c6_game_statistics import statistics
from device_session import pin, require, values
from pad_movie_queue import rolling_queue_for
from prepare_c6_routes import checked_edges, queue_plan, validate_bank

COMMON = Path(__file__).resolve().parent
PROJECT = "bitvyr_nes_p4c6"
CATALOG_EVENT = "p4c6-autonomous-6040-catalog-38-20261123"
# SHA упорядоченного списка ключей 38 записей. Каждый ключ также проверяется по содержимому.
MEASURED_CATALOG_SHA256 = "6607629a6bb92453ebefdcf6cefc8e32c41a382e765c32c90060b10d363647f8"
RELEASE_BUILD = COMMON.parent / "bitvyr_nes_p4c6" / "beta_0_2" / "build"
FRAME = "Non5Frame"
MMC5_COUPLED = "M5Coupled"
PERF_TAGS = {"NES_OUTPUT_FRAME_PIPELINE_TIMES", "NES_COUPLED_BENCH"}
STATUS_TAGS = {"NES_OUTPUT_FRAME_PIPELINE_STATUS", "NES_COUPLED_BENCH_STATUS"}
RELEASE_C6_SHA = "c2660fac172c216b5a26424dd993af0377747b5688ae4bcaf739668465738d05"
FAULT = re.compile(r"\b(?:ROM_(?:LOAD|READ)_FAIL|NES_(?:RUNTIME|WORKER)_FAIL|NES_(?:PAD|COMMAND|STATUS|COUPLED_BENCH|FRAME_PIPELINE_BENCH)_ERROR)\b|NES_C6_AUDIO_DRAIN success=0|Guru Meditation|assert failed|stack overflow|Task watchdog", re.I)


def read_c6_pin(path):
    """Читает 32 байта рабочего символа из загружаемой секции ELF P4 до открытия UART."""
    from elftools.elf.elffile import ELFFile
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        table = elf.get_section_by_name(".symtab")
        symbols = table.get_symbol_by_name("_ZN3nes3app8c6_async11batch_image7warmElfE") if table else None
        require(symbols and len(symbols) == 1, "Missing or ambiguous C6 identity symbol in P4 ELF")
        symbol = symbols[0]
        require(symbol["st_size"] == 32 and type(symbol["st_shndx"]) is int, "Invalid C6 identity symbol")
        section = elf.get_section(symbol["st_shndx"])
        offset = symbol["st_value"] - section["sh_addr"]
        require(section["sh_type"] == "SHT_PROGBITS" and section["sh_flags"] & 2
                and 0 <= offset <= section["sh_size"] - 32, "C6 identity is outside allocated section")
        return section.data()[offset:offset + 32].hex()


def validate_image_admission(text, expected_elf, c6_sha):
    """Связывает последний boot с локальным образом и пятью рабочими политиками."""
    prefix, tail = base.admit_boot(text, expected_elf, PROJECT)
    markers = (
        "NES_EXPERIMENTAL_CPU0_VRC7",
        "NES_EXPERIMENTAL_BATCH_FETCH46",
        "NES_EXPERIMENTAL_FRAME_COLOUR_PUMP",
        "NES_EXPERIMENTAL_GENERIC_AFFINE_QUIET",
        "NES_EXPERIMENTAL_OAM_TERMINAL_MASK",
    )
    for marker in markers:
        lines = [line for line in tail.splitlines() if marker + " " in line]
        require(len(lines) == 1 and re.search(r"\benabled=1\b", lines[0]),
                "Missing active marker: " + marker)
    return dict(app_elf=expected_elf, observed_boot_prefix=prefix,
                active_profile_markers=list(markers), expected_c6_elf=c6_sha)


def expected_lane(mapper):
    """Возвращает конвейер, которым работает данный картридж."""
    require(mapper in (0, 1, 2, 3, 4, 5, 7, 69, 71, 85, 160), "Unsupported C6 mapper")
    return MMC5_COUPLED if mapper == 5 else FRAME


def checked_lane(mapper, lane=None):
    """Сверяет схему отчёта с владельцем текущего кадра."""
    expected = expected_lane(mapper)
    require(lane is None or lane == expected, "Expected lane/mapper mismatch")
    return expected


def report_tag(lane):
    """Тип периодического отчёта."""
    return "NES_OUTPUT_FRAME_PIPELINE_TIMES" if lane == FRAME else "NES_COUPLED_BENCH"


def status_tag(lane):
    """Тип проверенного снимка сеанса."""
    return "NES_OUTPUT_FRAME_PIPELINE_STATUS" if lane == FRAME else "NES_COUPLED_BENCH_STATUS"


def validate_status_lane(status, lane):
    """Сверяет владельца снимка игры."""
    require(status.get("owner") == ("timing_fetch" if lane == FRAME else "thin_signals"),
            "Unexpected status lane owner")


# Разбирает JSON/KV-телеметрию, сохраняя исходную строку и смещение в RAW.
def packet(line, offset=0):
    match = re.search(r"\b(NES_[A-Z0-9_]+)\s+(.*)", line)
    if not match: return None
    tag, body = match.groups()
    fields = json.loads(body) if body.startswith("{") else values(body)
    return dict(kind=tag[4:].lower(), raw_type=tag, fields=fields, line=line, rx_offset=offset)

class Link(base.Link):
    """Владеет единственным UART лишь после явного запуска main с разрешением исполнения."""
    # Создаёт раздельные журналы разобранных строк, faults и подтверждённых смещений.
    def __init__(self, args, log):
        super().__init__(args, log)
        self.packets=[]; self.faults=[]; self.consumed=0

    # Разрешает только whitelist-команды; короткий write остаётся ошибкой без скрытого retry.
    def send(self, commands, phase):
        for command in commands:
            require(re.fullmatch(r"(?:ui_status|status|menu|tasks|pad 0|catalog \d+ (?:[1-9]|[1-5]\d|6[0-4])|run \d+|padat \d+ \d+)", command),
                    "Command outside C6 qualification whitelist: "+command)
            payload=(command+"\n").encode("ascii")
            n=self.port.write(payload); self.port.flush()
            self.commands.append(dict(command=command, phase=phase, bytes=n,
                                      expected_bytes=len(payload), rx_bytes_before=self.log.tell()))
            require(n==len(payload), "Short UART command write")

    # Сохраняет RAW до разбора; незавершённая строка и malformed telemetry не теряются.
    def read(self):
        data=self.port.read(32768)
        if data:
            self.log.write(data); self.log.flush(); self.pending+=data
        lines=[]
        while b"\n" in self.pending:
            raw,self.pending=self.pending.split(b"\n",1)
            offset=self.consumed; self.consumed+=len(raw)+1
            line=base.ANSI.sub("",raw.decode("utf-8",errors="replace")).strip()
            self.rows.append(line); lines.append(line)
            try:
                row=packet(line,offset)
                if row:self.packets.append(row)
            except (ValueError,TypeError) as exc:
                self.faults.append(dict(line=line,rx_offset=offset,error="Malformed telemetry: "+str(exc)))
            if FAULT.search(line):self.faults.append(dict(line=line,rx_offset=offset,error="Device fault"))
        require(len(self.pending)<65536, "Unterminated UART line")
        return lines

# Ожидает явный predicate до host-deadline; timeout сам не подтверждает join или drain.
def collect_until(link, predicate, timeout, reason):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        link.read()
        if predicate():return
    raise ValueError(reason)

# Требует хотя бы один нужный тип и возвращает поля последнего наблюдения.
def last_packet(rows, tags):
    selected=[r for r in rows if r["raw_type"] in tags]
    require(selected, "Missing "+"/".join(sorted(tags)))
    return selected[-1]["fields"]

def catalog(link):
    """Читает все страницы каталога; нулевой CRC raw-ROM остаётся неизвестным до полной загрузки."""
    games=[]; offset=0
    while True:
        at=len(link.packets)
        link.send([f"catalog {offset} 64"], "catalog")
        # Завершает чтение страницы после заголовка каталога и ожидаемого числа записей ROM.
        def done():
            rows=link.packets[at:]
            meta=[r["fields"] for r in rows if r["raw_type"]=="NES_CATALOG"]
            return bool(meta) and len([r for r in rows if r["raw_type"]=="NES_ROM"])>=min(64,max(0,meta[-1]["count"]-offset))
        collect_until(link,done,10,"Catalog timeout")
        rows=link.packets[at:]
        meta=last_packet(rows,{"NES_CATALOG"})
        require(not meta["error"] and not meta["skipped"], "Catalog error/skipped entries")
        page=[r["fields"] for r in rows if r["raw_type"]=="NES_ROM"]
        games.extend(page); offset+=len(page)
        if offset>=meta["count"]:break
        require(page,"Empty catalog page before count")
    require(len(games)==meta["count"] and len({g["index"] for g in games})==len(games),"Incomplete/duplicate catalog indices")
    known=[g["crc32"].lower() for g in games if g["crc32"]!="00000000"]
    require(len(known)==len(set(known)),"Duplicate known ROM CRC")
    return games

def basic_statistics(reports, movie, structured_rows=None, lane=None):
    """Считает FPS по отметкам времени платы на границах окон."""
    result = statistics(reports + (structured_rows or []), movie)
    result["expected_lane"] = checked_lane(movie["mapper"], lane)
    return result


# Сверяет фактическую маску movie и отсутствие примеси Xbox/touch для этого input0.
def input_trace(row,movie):
    expected=0
    for edge in movie["edges"]:
        if edge["frame"]<=row["frame"]:expected=edge["mask"]
    require(all(row.get(k)==0 for k in ("xbox_raw","xbox_effective","touch","contacts","suppressed")) and int(str(row.get("ids","0")),16)==0,
            "Physical input contamination")
    require(row.get("poll_ok")==1 and row.get("action")=="none","Invalid input poll/action")
    require(row.get("serial")==row.get("applied")==expected,"Actual movie input mismatch")

# Проверяет frame0, все смены маски и единое поколение; это не покадровый input-log.
def check_input_coverage(traces,movie,reports):
    require(traces and reports,"Missing input/report proof")
    generation=reports[0]["generation"]
    require(all(t["generation"]==generation for t in traces),"Input/report generation mismatch")
    require(all(a["frame"]<b["frame"] for a,b in zip(traces,traces[1:])),"Input frame regression/duplicate")
    expected={0}; previous=0
    for edge in movie["edges"]:
        if edge["mask"]!=previous:expected.add(edge["frame"])
        previous=edge["mask"]
    require(expected<={t["frame"] for t in traces},"Missing changed input transitions/frame0")
    return dict(generation=generation,required_changed_frames=sorted(expected),trace_count=len(traces))

# Требует готовый batch C6 backend с warm-unbind и stream guard данного mapper.
def validate_backend(rows,mapper,lane=None):
    checked_lane(mapper,lane)
    backends=[r["fields"] for r in rows if r["raw_type"]=="NES_C6_AUDIO_BACKEND"]
    require(backends,"Missing C6 backend admission")
    b=backends[-1]
    for k,v in dict(ready=1,warm_unbind=1,backend_batch=1,local_worker_dsp=0,stream_guard=1,mapper=mapper).items():
        require(b.get(k)==v,"C6 backend guard: "+k)
    return b

# Требует ключ игры и поколения, 120 samples и монотонный device_us без пропусков.
def validate_reports(reports,entry,movie,lane=None):
    require(reports,"Missing C6 reports")
    generation=reports[0]["generation"]
    lane=checked_lane(movie["mapper"],lane);expected_tag=report_tag(lane)
    for r in reports:
        require(r["raw_type"]==expected_tag and r["game"]==entry["index"] and r["mapper"]==movie["mapper"],"Report backend/game drift")
        require(r["generation"]==generation and generation>0 and r["region"]==movie["region"],"Report scope drift")
        require(r["samples"]==120 and type(r.get("device_us")) is int,"Missing complete device-us report")
    for a,b in zip(reports,reports[1:]):
        require(b["frames"]-a["frames"]==120 and b["device_us"]>a["device_us"],"Lost/duplicate/out-of-order reports")


def end_scene_snapshot(link,entry,movie,reports,lane=None):
    """Сохраняет фактический joined accepted-срез после измеренных окон.
    Задержка UART может сдвинуть кадр за end_report; авторитетен полученный ключ.
    RGB-хеш диагностический: он не доказывает изображение LCD или правильность сцены."""
    require(reports and reports[-1]["frames"]>=movie["roi_reports"][-1],"End scene request precedes ROI end")
    after_report=reports[-1]["frames"];generation=reports[-1]["generation"]
    at=len(link.packets);fault_start=len(link.faults)
    lane=checked_lane(movie["mapper"],lane);tag=status_tag(lane)
    link.send(["status","ui_status"],"end_scene_after_reports")
    # Ждёт обе свежие строки status/UI, не используя старые записи до запроса.
    def finished():
        rows=link.packets[at:]
        return any(r["raw_type"]==tag for r in rows) and any(r["raw_type"]=="NES_UI_STATUS" for r in rows)
    collect_until(link,finished,12,"End scene status/UI response timeout")
    require(len(link.faults)==fault_start,"Device fault during end scene snapshot")
    rows=link.packets[at:];statuses=[r for r in rows if r["raw_type"]==tag]
    require(len(statuses)==1,"Ambiguous end scene status response")
    status=statuses[0];s=status["fields"]
    validate_status_lane(s,lane)
    require(s.get("playing") is True and s.get("game")==entry["index"] and
            s.get("generation")==generation and s.get("mapper")==movie["mapper"] and
            s.get("region")==movie["region"] and s.get("crc32")==movie["crc32"],
            "End scene accepted-cut identity mismatch")
    require(s.get("frames",0)>=after_report and s.get("image_available") is True and
            s.get("input_valid") is True and s.get("input_frame")==s["frames"]-1,
            "End scene lacks an exact accepted input/image cut")
    require(type(s.get("rgb_hash")) is str and re.fullmatch(r"[0-9a-fA-F]{16}",s["rgb_hash"]),
            "End scene RGB diagnostic hash unavailable")
    ui=[r for r in rows if r["raw_type"]=="NES_UI_STATUS"]
    require(len(ui)==1,"Ambiguous end scene UI response")
    return dict(status=status,ui_status=ui[0],after_report_frame=after_report,
                actual_accepted_frame=s["frames"],snapshot_input0=s["input_frame"],
                accepted_frames_after_report=s["frames"]-after_report,
                observation_after_measured_intervals=True,
                no_frame_reset=True,physical_LCD_claim=False,scene_verified=False)

# Проверяет полный список после последнего NES_TASKS либо явный disabled-ответ.
def task_response_complete(rows):
    headers=[r for r in rows if r["raw_type"]=="NES_TASKS"]
    if not headers:return False
    header=headers[-1]
    if "disabled" in header["line"]:return True
    count=header["fields"].get("count")
    tasks=[r for r in rows if r["raw_type"]=="NES_TASK" and r["rx_offset"]>=header["rx_offset"]]
    return type(count) is int and len(tasks)>=count


def validate_quiet_settings(settings,playing=False,expected_volume=1):
    """Сверяет прочитанные настройки и регион; не записывает предпочтения или NVS."""
    require(isinstance(settings,dict),"Missing actual settings object")
    require(all(type(settings.get(k)) is int for k in ("volume","hardware_volume")),
            "Quiet volume fields must be actual integers")
    require(all(type(settings.get(k)) is bool for k in ("muted","dirty","paused")),
            "Quiet flags must be actual booleans")
    base.quiet(settings,playing,expected_volume)
    require(settings.get("region")=="NTSC 60","Stored region preference must be NTSC 60")
    require(settings.get("paused") is False,"Paused UI cannot qualify quiet capture")
    if not playing:require(settings.get("active_region")=="none","Library must have no active region")
    return settings

def quiet_preflight(link):
    """Читает свежую пару настроек и UI в Library до запуска movie."""
    at=len(link.packets);fault_start=len(link.faults)
    link.send(["ui_status"],"quiet_preflight")
    # Ждёт свежие настройки и UI из одной области запроса.
    def finished():
        rows=link.packets[at:]
        return any(r["raw_type"]=="NES_SETTINGS" for r in rows) and any(r["raw_type"]=="NES_UI_STATUS" for r in rows)
    collect_until(link,finished,12,"Fresh quiet Library settings/UI timeout")
    require(len(link.faults)==fault_start,"Device fault during quiet preflight")
    rows=link.packets[at:]
    settings=[r for r in rows if r["raw_type"]=="NES_SETTINGS"]
    ui=[r for r in rows if r["raw_type"]=="NES_UI_STATUS"]
    require(len(settings)==len(ui)==1,"Ambiguous fresh quiet preflight response")
    validate_quiet_settings(settings[0]["fields"],False,getattr(link,"expected_volume",1))
    require(ui[0]["fields"].get("video") is False,"Library preflight still has game video")
    require(ui[0]["fields"].get("source")=="software_lcd_submit","Unexpected UI rate source")
    return dict(settings=settings[0],ui_status=ui[0])

def cleanup(link,mapper,at,lane=None,generation=None):
    """Завершает сеанс настоящим меню и проверяет drain/UNBOUND; fault не сбрасывает владельца."""
    lane=checked_lane(mapper,lane)
    link.send(["pad 0","menu","status","ui_status","tasks"],"cleanup")
    # Ждёт Library, остановленный сеанс, UI и полный tasks-ответ перед проверкой drain.
    def finished():
        rows=link.packets[at:]
        settings=[r["fields"] for r in rows if r["raw_type"]=="NES_SETTINGS"]
        statuses=[r["fields"] for r in rows if r["raw_type"] in STATUS_TAGS]
        return bool(settings and statuses and settings[-1].get("screen")=="library" and statuses[-1].get("playing") is False and any(r["raw_type"]=="NES_UI_STATUS" for r in rows) and task_response_complete(rows))
    collect_until(link,finished,12,"No final joined Library status")
    rows=link.packets[at:]
    final=last_packet(rows,STATUS_TAGS)
    validate_quiet_settings(last_packet(rows,{"NES_SETTINGS"}),False,getattr(link,"expected_volume",1))
    drains=[r["fields"] for r in rows if r["raw_type"]=="NES_C6_AUDIO_DRAIN"]
    require(drains,"Missing actual C6 final drain")
    d=drains[-1]
    for k,v in dict(success=1,pending_pcm=0,publication_held=0,actual_join=1,local_renderer_used=0).items():
        require(d.get(k)==v,"C6 drain guard: "+k)
    require(d.get("remote_frames")==d.get("guest_frames")==d.get("stream_verified_frames"),"C6 final stream-count mismatch")
    unbound=last_packet(rows,{"NES_C6_AUDIO_UNBOUND"})
    require(all(unbound.get(k)==v for k,v in dict(ready=1,no_job=1,source_held=0).items()),"C6 actual UNBOUND guard")
    if generation is not None:require(unbound.get("old_generation")==generation,"C6 UNBOUND generation mismatch")
    require(final.get("playing") is False and final.get("owner")=="unbound","Final Library owner still bound")
    ui=last_packet(rows,{"NES_UI_STATUS"});require(ui.get("video") is False,"Final Library video still active")
    if lane!=FRAME:
        f=last_packet(rows,{"NES_COUPLED_REPORT_FINAL_DRAIN"})
        require(f.get("in_flight")==0 and f.get("pending_depth")==0 and f.get("source_valid")==0 and f.get("actual_join_checked")==1,"Coupled final ownership mismatch")
    else:
        f=last_packet(rows,{"NES_FRAME_C6_FINAL_DRAIN"})
        require(f.get("in_flight")==0 and f.get("c6_unbound")==1 and f.get("audio_source_held")==0,"Frame final ownership mismatch")
        require(final.get("owner")=="unbound","Frame context still bound")
    return final

def run_game(link,movie,entry):
    """Воспроизводит маршрут, снимает окна и завершает сеанс через меню."""
    first=len(link.packets); cursor=first; fault_start=len(link.faults)
    rolling=rolling_queue_for(movie)
    initial=movie["queue_plan"]["initial"]; deferred=movie["queue_plan"].get("deferred",[])
    reports=[]; traces=[]; applied=[]; scheduled=[]; errors=[]
    sent_deferred=not deferred; launch=None; start_status=None; bound=False; backend=None; end_scene=None; preflight=None
    admission = getattr(link, "image_admission", {})
    lane = expected_lane(movie["mapper"])
    commands=["run "+str(entry["index"])]+([] if rolling else ["padat "+x.replace(":"," ") for x in initial])
    end_report=math.ceil(movie["end_frame"]/120)*120
    deadline=time.monotonic()+max(240,end_report/20+90)
    try:
        preflight = quiet_preflight(link)
        link.send(commands,"start_movie")
        if rolling:rolling.start(link.send)
        while time.monotonic()<deadline:
            link.read(); rows=link.packets[cursor:]; cursor=len(link.packets)
            require(len(link.faults)==fault_start,"Device fault in moving capture")
            if rolling:rolling.observe_batch(rows)
            for p in rows:
                tag,f=p["raw_type"],p["fields"]
                if rolling:rolling.on_packet(p)
                if tag=="NES_GAME_START":
                    require(launch is None,"Unexpected extra launch")
                    require(str(f.get("crc","")).lower()==movie["crc32"].lower() and f.get("mapper")==movie["mapper"] and f.get("index")==entry["index"],"Loaded ROM mismatch")
                    launch=f
                elif tag=="NES_INPUT":
                    input_trace(f,movie); traces.append(f)
                elif tag=="NES_PAD_SCHEDULE":
                    require(f["queued"]<=16,"Pad queue overflow"); scheduled.append((f["frame"],f["mask"]))
                elif tag=="NES_PAD_APPLY":
                    require(f["frame"]==f["scheduled"],"Late movie edge"); applied.append((f["scheduled"],f["mask"]))
                elif tag in PERF_TAGS:
                    require(tag==report_tag(lane),"Unexpected performance lane")
                    reports.append(dict(f,raw_type=tag,line=p["line"],rx_offset=p["rx_offset"],
                                        input0=[f["frames"]-f["samples"],f["frames"]-1]))
                if rolling and tag in ("NES_PAD_SCHEDULE","NES_PAD_APPLY"):rolling.refill(link.send)
            if rolling:
                rolling.finish_batch()
                sent_deferred=rolling.all_sent
            require(len(link.faults)==fault_start,"Device fault in moving capture")
            progress=max([0]+[r["frames"] for r in reports]+[f for f,m in applied])
            if launch is not None and len(scheduled)>=len(initial) and start_status is None:
                require(progress<movie["baseline_report"]-120,"Initial status would taint ROI")
                start_status=len(link.packets); link.send(["status","ui_status"],"start_status")
            if start_status is not None and not bound:
                startup_rows=link.packets[start_status:]
                candidates=[p["fields"] for p in startup_rows if p["raw_type"]==status_tag(lane)]
                require(not any(p["raw_type"] in STATUS_TAGS and p["raw_type"]!=status_tag(lane) and p["fields"].get("playing") is True for p in startup_rows),"Unexpected bound status lane")
                startup_settings=[p["fields"] for p in startup_rows if p["raw_type"]=="NES_SETTINGS"]
                startup_ui=[p for p in startup_rows if p["raw_type"]=="NES_UI_STATUS"]
                if candidates and startup_settings and startup_ui:
                    validate_quiet_settings(startup_settings[-1],True,getattr(link,"expected_volume",1))
                    s=candidates[-1]
                    require(s.get("playing") is True and s.get("game")==entry["index"] and s.get("crc32")==movie["crc32"] and s.get("mapper")==movie["mapper"] and s.get("region")==movie["region"],"Actual bound snapshot identity mismatch")
                    require(s["frames"]<movie["baseline_report"]-120 and s.get("image_available") is True,"Initial status too late/no image")
                    validate_status_lane(s,lane)
                    backend=validate_backend(link.packets[first:],movie["mapper"],lane)
                    bound=True
            if rolling is None and not sent_deferred and set(tuple(map(int,x.split(":"))) for x in initial[:2])<=set(applied):
                require(progress<movie["queue_plan"]["latest_safe_progress_frame"] and progress<movie["baseline_report"],"Deferred queue window missed")
                require(len(scheduled)-len(applied)+len(deferred)<=16,"Deferred queue has no proven room")
                link.send(["padat "+x.replace(":"," ") for x in deferred],"deferred_after_apply")
                sent_deferred=True
            if reports and reports[-1]["frames"]>=end_report and len(applied)==len(movie["edges"]):
                break
        else:raise ValueError("Movie timeout; host-time FPS unavailable")
        require(launch is not None and bound and sent_deferred,"Missing launch/backend/input proof")
        expected=[(e["frame"],e["mask"]) for e in movie["edges"]]
        require(applied==scheduled==expected,"Exact movie ACK/APPLY sequence mismatch")
        if rolling:rolling.finish()
        validate_reports(reports,entry,movie,lane)
        input_proof=check_input_coverage(traces,movie,reports)
        end_scene=end_scene_snapshot(link,entry,movie,reports,lane)
        stats=basic_statistics(reports,movie,link.packets[first:],lane)
    except Exception as exc:errors.append(str(exc));input_proof=None;stats=None
    at=len(link.packets);final=None
    try:final=cleanup(link,movie["mapper"],at,lane,reports[-1]["generation"] if reports else None)
    except Exception as exc:errors.append("Cleanup: "+str(exc))
    packets=link.packets[first:]
    result=dict(status="HOLD" if errors else "PASS_INPUT_DEVICE_US_C6_STREAM_NOT_SCENE_OR_PHYSICAL_LCD",
                errors=errors,expected_lane=lane,image_admission=admission,movie=movie,actual_catalog_entry=entry,launch=launch,backend=backend,quiet_preflight=preflight,
                reports=reports,structured_rows=packets,input_traces=traces,input_proof=input_proof,
                applied_edges=applied,schedule_ACKs=scheduled,statistics=stats,
                rolling_queue=rolling.snapshot() if rolling else None,
                c6_windows=[p for p in packets if p["raw_type"]=="NES_C6_AUDIO_WINDOW"],
                c6_drains=[p for p in packets if p["raw_type"]=="NES_C6_AUDIO_DRAIN"],
                c6_unbounds=[p for p in packets if p["raw_type"]=="NES_C6_AUDIO_UNBOUND"],
                faults=link.faults[fault_start:],final_status=final,can_continue=final is not None,
                end_scene_status=end_scene["status"] if end_scene else None,
                end_scene_ui_status=end_scene["ui_status"] if end_scene else None,
                end_scene_observation=end_scene,
                requested_end_frame=movie["end_frame"],actual_end_report=end_report,
                scene_verified=False,physical_LCD_claim=False,per_frame_min_FPS=None)
    return result

def load_measured_catalog(journal):
    """Восстанавливает ввод и окна 38 игровых сцен непосредственно из журнала измерений."""
    archive = experiments.read(journal)
    event = next((e for e in archive["chronology"] if e["id"] == CATALOG_EVENT), None)
    require(event is not None, "Release catalog event missing")
    evidence = event["result"]["durable_build_evidence"]
    digest = hashlib.sha256(("\n".join(sorted(evidence["payloads"])) + "\n").encode()).hexdigest()
    require(len(evidence["artifacts"]) == 38 and digest == MEASURED_CATALOG_SHA256, "Release catalog changed")
    bank = dict(games=[], route_preparation={"requested_windows": 3})
    for source in evidence["artifacts"]:
        game = experiments.lookup_payload(archive, source)
        crc = game["crc32"]
        require(re.fullmatch(r"[a-f0-9]{8}", crc) and game["source_sha256"] == source,
                "Invalid catalog identity")
        start, last = game["roi_input0"]
        edges = checked_edges(game["edges"])
        require(edges[-1]["frame"] <= last, "Input route extends beyond measured interval")
        bank["games"].append(dict(crc32=crc, title=game["title"], mapper=game["mapper"], region=game["region"],
                                  baseline_report=start, end_frame=last+1, roi_input0=[start,last],
                                  roi_reports=[start+120, start+240, start+360], edges=edges,
                                  queue_plan=queue_plan(edges)))
    validate_bank(bank)
    return bank


def release_window_rates(stats):
    """Возвращает скорость подачи только при трёх полных окнах с непрерывными счётчиками."""
    roi = stats["roi"]
    require(not stats["missing_baseline"] and not stats["missing_roi_endpoints"] and not stats["discontinuities"],
            "Incomplete device-time interval")
    require(stats["roi_fps_range"]["covered"] and roi["submitted_coverage_windows"] == 3
            and roi["windows"] == 3 and roi["frames"] == 360, "Incomplete LCD submission coverage")
    return {key: roi[key] for key in ("min_submitted_window_fps", "mean_submitted_fps",
                                      "max_submitted_window_fps")}


def main(argv=None):
    """Проверяет параметры запуска до открытия UART."""
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--movies",type=Path,help="Свои маршруты; по умолчанию используются 38 измеренных сцен Beta 0.2")
    p.add_argument("--crcs",nargs="*");p.add_argument("--catalog-only",action="store_true")
    p.add_argument("--port",default="COM4");p.add_argument("--vid",type=lambda x:int(x,0),default=0x1a86)
    p.add_argument("--pid",type=lambda x:int(x,0),default=0x55d3);p.add_argument("--serial")
    p.add_argument("--expected-elf", "--elf-sha", dest="expected_elf")
    p.add_argument("--local-elf", type=Path, help="Сохранённый ELF для проверки SHA256")
    p.add_argument("--c6-sha", default=RELEASE_C6_SHA, help="Ожидаемый ELF SHA-256 C6 для протокола HELLO")
    p.add_argument("--admission", type=Path)
    p.add_argument("--execute-root", action="store_true")
    p.add_argument("--attest-admitted-image-current", action="store_true")
    p.add_argument("--output",type=Path,default=RELEASE_BUILD/"validation"/"device-runs")
    p.add_argument("--expected-volume",type=int,required=True,choices=range(1,101))
    a=p.parse_args(argv)
    require(re.fullmatch(r"[0-9a-f]{64}", a.c6_sha), "Expected full lowercase C6 ELF SHA256")
    if a.movies is None:
        data = load_measured_catalog(COMMON.parent / "experiments.json")
        movie_source = dict(sha256=MEASURED_CATALOG_SHA256, scope="measured_catalog_projection", event=CATALOG_EVENT)
    else:
        data = json.loads(a.movies.read_text(encoding="utf-8"))
        movie_source = pin(a.movies)
    routes = {g["crc32"].lower(): g for g in data["games"]}
    wanted=[s.lower() for s in a.crcs] if a.crcs else list(routes)
    require(len(wanted)==len(set(wanted)) and all(s in routes for s in wanted),"Unknown/duplicate movie CRC")
    require(len(routes) == len(data["games"]), "Duplicate CRC in movie bank")
    lanes = {crc: expected_lane(routes[crc]["mapper"]) for crc in wanted}
    for crc in wanted:
        rolling_queue_for(routes[crc])
    local = None
    if a.local_elf is not None:
        require(a.expected_elf, "--local-elf requires --expected-elf")
        local = base.local_image(PROJECT, a.expected_elf, a.local_elf)
    if not a.execute_root:
        summary = dict(execute=False, COM_opened=False, games=wanted,
                       expected_volume=a.expected_volume, expected_C6_ELF=a.c6_sha, expected_lanes=lanes, movies=movie_source,
                       roi_inputs={crc: routes[crc].get("roi_input0") for crc in wanted})
        if local is not None:
            summary["local_P4_image"] = local
        print(json.dumps(summary))
        return 0
    require(a.expected_elf and a.admission and a.attest_admitted_image_current,
            "Current image ELF, boot log and attestation are required")
    local = local or base.local_image(PROJECT, a.expected_elf, RELEASE_BUILD / (PROJECT + ".elf"))
    require(read_c6_pin(Path(local["path"])) == a.c6_sha, "Expected C6 ELF differs from the P4 HELLO pin")
    image_admission = validate_image_admission(a.admission.read_text(encoding="utf-8", errors="replace"),
                                               a.expected_elf, a.c6_sha)
    out=a.output.resolve()/time.strftime("c6-%Y%m%d-%H%M%S");out.mkdir(parents=True,exist_ok=False)
    summary=dict(tool=pin(Path(__file__)),helper=pin(COMMON/"device_session.py"),rolling_queue_helper=pin(COMMON/"pad_movie_queue.py"),movies=movie_source,admission=pin(a.admission),expected_P4_ELF=a.expected_elf,expected_volume=a.expected_volume,
                 C6_admission="runtime exact source-pinned HELLO; Root verifies current P4 image separately",
                 expected_C6_ELF=a.c6_sha,image_admission=image_admission,games=[],host_time_FPS=False,NVS_written=False,SD_written=False)
    summary["local_P4_image"] = local
    link=None
    try:
        link=Link(a,out/"raw.log");link.image_admission=image_admission;link.collect(.5)
        summary["quiet_preflight"]=quiet_preflight(link)
        live=catalog(link)
        summary.update(USB=link.usb,actual_catalog=live)
        (out/"catalog.json").write_text(json.dumps(live,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
        if not a.catalog_only:
            by_crc={g["crc32"].lower():g for g in live if g["crc32"]!="00000000"}
            for crc in wanted:
                begin=link.log.tell();cmd=len(link.commands)
                if crc not in by_crc:
                    result=dict(status="HOLD",errors=["Movie CRC absent/unknown in current inventory"],can_continue=True)
                else:
                    try:
                        result=run_game(link,routes[crc],by_crc[crc])
                        if a.movies is None and result["status"] != "HOLD":
                            try:
                                result["lcd_submission_fps"] = release_window_rates(result["statistics"])
                            except (KeyError, ValueError) as exc:
                                result["status"] = "HOLD"
                                result["errors"].append(str(exc))
                    except Exception as exc:result=dict(status="HOLD",errors=[str(exc)],can_continue=False)
                link.log.flush();raw=(out/"raw.log").read_bytes()[begin:]
                rawpath=out/(crc+".raw.log");rawpath.write_bytes(raw)
                result.update(raw=pin(rawpath),commands=link.commands[cmd:])
                path=out/(crc+".json");path.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
                summary["games"].append(dict(crc32=crc,status=result["status"],result=pin(path),
                                             lcd_submission_fps=result.get("lcd_submission_fps")))
                (out/"result.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
                if not result["can_continue"]:break
        summary["status"]="HOLD" if any(g["status"]=="HOLD" for g in summary["games"]) else "PASS_CAPTURE_SCOPE_ONLY"
    except Exception as exc:summary.update(status="HOLD",error=str(exc))
    finally:
        if link:link.close()
        (out/"result.json").write_text(json.dumps(summary,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(dict(output=str(out),status=summary["status"],games=len(summary["games"]))))
    return 0 if summary["status"]!="HOLD" else 2

if __name__=="__main__":raise SystemExit(main())

