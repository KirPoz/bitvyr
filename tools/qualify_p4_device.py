"""Прогон P4 с точным вводом, временем платы и снимками принятых кадров."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import re
import time

import device_session as base
from device_session import require, pin
from pad_movie_queue import rolling_queue_for

ROOT = Path(__file__).resolve().parent.parent
PROJECT = "bitvyr_nes_p4"
BASE_PATH = Path(base.__file__)
MOVIES_SHA = "2eb94e9e66c5fcf69bba4b863c5eca46eab3baf8000447a18fbee084f4ab9ed9"


def digest(path):
    """Вычисляет хэш закреплённого банка маршрутов."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def packet(line):
    """Разбирает существующие JSON/KV wire-строки без timestamp из host logger."""
    m = re.search(r'\b(NES_[A-Z0-9_]+)\s+(.*)', line)
    if not m:
        return None
    body = m[2]
    fields = json.loads(body) if body.startswith('{') else base.values(body)
    require(isinstance(fields, dict), 'Wire row is not an object')
    return {'raw_type': m[1], 'fields': fields, 'line': line}


def uint(row, key, positive=False):
    """Не принимает bool/строку вместо гостевого unsigned счётчика."""
    n = row.get(key)
    require(type(n) is int and n >= (1 if positive else 0), 'Invalid unsigned field: ' + key)
    return n


def perf_row(line):
    """Только accepted Frame report с точным device_us; logger-ms fallback запрещён."""
    p = packet(line)
    if not p or p['raw_type'] != 'NES_OUTPUT_FRAME_PIPELINE_TIMES':
        return None
    f = dict(p['fields'])
    for k in ('device_us', 'frames', 'produced_frames', 'submitted_frames', 'generation'):
        uint(f, k, True)
    require(f.get('samples') == 120 and f.get('schema') == 2 and f.get('slots') == 2,
            'Incomplete/non-Frame report')
    require(f.get('producer') == 'timing_fetch' and f.get('consumer') == 'output_compositor'
            and f.get('region') == 'NTSC', 'Unexpected Frame ownership/region')
    f['line'] = line
    return f


def hart_key(row):
    """Ключ HART относится к accepted N, независимо от порядка UART строк."""
    return tuple(uint(row, k) for k in ('game', 'generation', 'first_sequence', 'last_sequence'))


def validate_hart(row, report):
    """Требует аппаратные120 stepFrame samples; preemption остаётся внутри HART."""
    require(row.get('schema') == 1 and row.get('samples') == row.get('valid') == 120
            and row.get('invalid') == row.get('overflow') == 0, 'HART incomplete/invalid/overflow')
    key = (report['game'], report['generation'], report['frames'] - 120, report['frames'] - 1)
    require(hart_key(row) == key, 'HART/report exact120 key mismatch')
    require(row.get('scope') == 'stepFrame_hart' and row.get('task_exclusive') == 0,
            'Unexpected HART scope')
    cycles, retired = uint(row, 'cycles', True), uint(row, 'retired', True)
    uint(row, 'guest_frame_cycles', True)
    return dict(row, mean_us=cycles / 400 / 120, retired_per_frame=retired / 120,
                CPI=cycles / retired, clock_hz=400000000, task_exclusive=False)


def statistics(reports, hart_rows, movie):
    """FPS — accepted delta/device_us на заданных120 окнах; затраты не суммируются."""
    require(reports and len({r['frames'] for r in reports}) == len(reports), 'Duplicate/missing reports')
    require(len({r['generation'] for r in reports}) == 1, 'Report generation drift')
    hmap = {}
    for h in hart_rows:
        key = hart_key(h)
        require(key not in hmap, 'Duplicate HART window')
        hmap[key] = h
    for a, b in zip(reports, reports[1:]):
        require(b['frames'] - a['frames'] == 120 and b['device_us'] > a['device_us'],
                'Noncontinuous device_us120 reports')
    by = {r['frames']: r for r in reports}
    needed = [movie['baseline_report'], *movie['roi_reports']]
    require(all(n in by for n in needed), 'Missing exact movie ROI report')
    require(all(b - a == 120 for a, b in zip(needed, needed[1:])), 'Non120 movie ROI')
    windows = []
    for n0, n1 in zip(needed, needed[1:]):
        a, b = by[n0], by[n1]
        ds = {k: b[k] - a[k] for k in ('frames', 'produced_frames', 'submitted_frames')}
        require(set(ds.values()) == {120}, 'ROI produced/accepted/submitted loss')
        key = (b['game'], b['generation'], n0, n1 - 1)
        require(key in hmap, 'Missing exact HART120 row')
        h = validate_hart(hmap[key], b)
        dt = b['device_us'] - a['device_us']
        windows.append({'input0': [n0, n1 - 1], 'device_us': dt, 'FPS': 120000000 / dt,
                        'counter_deltas': ds, 'metrics': b, 'HART': h})
    dt = by[needed[-1]]['device_us'] - by[needed[0]]['device_us']
    fps = (needed[-1] - needed[0]) * 1000000 / dt
    require(movie['roi_input0'] == [needed[0], needed[-1] - 1], 'ROI input endpoints mismatch')
    return {'ROI_input0': movie['roi_input0'], 'device_delta_us': dt,
            'counter_deltas': {k: by[needed[-1]][k] - by[needed[0]][k]
                               for k in ('frames', 'produced_frames', 'submitted_frames')},
            'FPS': fps, 'min120': min(w['FPS'] for w in windows),
            'max120': max(w['FPS'] for w in windows), 'windows': windows,
            'FPS_definition': 'sum accepted guest frames * 1e6 / sum exact report device_us deltas',
            'min_max_definition': '120 accepted-frame windows; not individual frame latency',
            'first_0_to120': 'NA: no frame0 timestamp', 'host_time_FPS': False,
            'logger_ms_fallback': False, 'overlapping_costs_summed': False,
            'physical_LCD_or_Bluetooth_rerun_claim': False}


def expected_mask(movie, frame):
    """Вычисляет маску только из неизменённого полного movie prefix."""
    mask = 0
    for edge in movie['edges']:
        if edge['frame'] <= frame:
            mask = edge['mask']
    return mask


def validate_scene(scene, present, audio, movie, entry, generation, minimum_frame, volume):
    """Проверяет опубликованный joined/accepted cut; RAM NA не превращает в совпадение."""
    s = scene
    require(s.get('playing') is True and s.get('image_available') is True
            and s.get('input_valid') is True, 'No actual accepted scene snapshot')
    require(s.get('owner') == 'timing_fetch' and s.get('consumer') == 'output_compositor'
            and s.get('cpu_snapshot') == 'actual_primary_cut', 'Snapshot is not actual primary cut')
    require(s.get('crc32') == movie['crc32'] and s.get('mapper') == movie['mapper']
            and s.get('game') == entry['index'] and s.get('region') == 'NTSC'
            and s.get('generation') == generation, 'Scene identity drift')
    frame = uint(s, 'frames', True)
    require(frame >= minimum_frame and s.get('input_frame') == frame - 1,
            'Stale/misaligned scene cut')
    require(s.get('input_mask') == expected_mask(movie, frame - 1), 'Scene input prefix mismatch')
    # CPU retirement и sealed audio/video tail — разные исходные cuts; oracle сравнит оба без уравнивания.
    require(0 < uint(s, 'cpu_state_cycles') <= uint(s, 'cpu_cycles', True), 'CPU state exceeds sealed cut')
    for k, limit in (('pc', 65535), ('a', 255), ('x', 255), ('y', 255), ('sp', 255), ('p', 255)):
        require(uint(s, k) <= limit, 'Invalid CPU snapshot register')
    require(0 < uint(s, 'pcm_count') <= 2048, 'Invalid PCM snapshot size')
    for k in ('pcm_hash', 'rgb_hash'):
        require(isinstance(s.get(k), str) and re.fullmatch(r'[0-9a-fA-F]{16}', s[k])
                and int(s[k], 16) != 0, 'Missing actual snapshot hash: ' + k)
    require(present.get('generation') == generation and present.get('accepted_frame') == frame
            and present.get('pending') is False and 0 <= frame - uint(present, 'completed_frame') <= 1
            and present.get('completed_generation') == generation
            and present.get('started_frames') == present.get('completed_frames') == s.get('submitted_frames')
            and present.get('lcd_submitted_frames') == s.get('submitted_frames'), 'No completed PPA/source witness')
    require(audio.get('generation') == generation and audio.get('accepted_frame') == frame
            and audio.get('playing') is True and audio.get('muted') is False
            and audio.get('volume') == volume and audio.get('active') is True
            and audio.get('source') == 'board_audio_stats', 'Local DSP/audio owner mismatch')
    if s.get('schema_version') == 3:
        available = s.get('ram_valid') is True
        require(s.get('ram_policy') == 'sparse120', 'Unknown sparse RAM policy')
        if available:
            require(s.get('ram_requested') is True and s.get('ram_status') == 'available'
                    and isinstance(s.get('ram_key'), dict) and isinstance(s.get('ram_hash'), str), 'Invalid sparse RAM witness')
        else:
            require(s.get('ram_hash') is None, 'Unavailable RAM must remain null')
    else:
        require(s.get('schema_version') == 2 and isinstance(s.get('ram_hash'), str), 'Unknown RAM schema')
        available = True
    return {'actual_cut': frame, 'CPU_RGB_input_snapshot_available': True,
            'RAM_available': available, 'RAM_value_match': None,
            'RAM_status': s.get('ram_status', 'full'), 'independent_native_oracle': False,
            'scene_verified': False, 'join_proof': 'pinned command retire->actual Worker done->joinedProducer; accepted/PPA witnesses',
            'authoritative_snapshot_key': {'generation': generation, 'crc32': movie['crc32'], 'region': 'NTSC',
                'CPU_frame': frame, 'RGB_frame': frame, 'PCM_frame': frame, 'input_frame': frame - 1,
                'sealed_end_cycle': s['cpu_cycles'], 'CPU_retirement_cycle': s['cpu_state_cycles'],
                'key_basis': 'source-certified single acceptedSnapshot: renderOutput(N)->accept(N)->refreshWitness(N)'},
            'PPA_completed_frame': present['completed_frame'], 'PPA_is_separate_from_owned_RGB_key': True,
            'physical_LCD_completion': False, 'local_PCM_values_not_compared_to_oracle': True}


def validate_cleanup(lines, volume):
    """Library/unbound подтверждается только после drain; C6 UNBIND для P4 не выдумывается."""
    s = base.latest(lines, 'NES_OUTPUT_FRAME_PIPELINE_STATUS')
    require(s.get('owner') == 'unbound' and s.get('playing') is False
            and s.get('image_available') is False, 'No joined final Frame unbound proof')
    base.quiet(base.latest(lines, 'NES_SETTINGS'), expected_volume=volume)
    require(base.latest(lines, 'NES_UI_STATUS').get('video') is False, 'Library still owns video')
    a = base.latest(lines, 'NES_FRAME_AUDIO_STATUS')
    require(a.get('playing') is False and a.get('active') is False
            and a.get('muted') is False and a.get('volume') == volume, 'Local audio not drained/stopped')
    tasks = [p['fields'] for line in lines if (p := packet(line)) and p['raw_type'] == 'NES_TASK']
    for name in ('main', 'nes_emulation'):
        rows = [t for t in tasks if t.get('name') == name]
        require(rows and uint(rows[-1], 'stack_min') >= 2048, 'Runtime stack watermark gate: ' + name)
    return {'final_joined_status': s, 'local_audio': a, 'runtime_tasks': tasks,
            'C6_UNBIND_required': False, 'DSP_owner': 'P4 Main',
            'join_is_not_timeout': True, 'library_video_false': True}


def admission(text, elf):
    """Полный known image плюс последний boot prefix; префикс не выдаётся за полный runtime hash."""
    prefix, tail = base.admit_boot(text, elf, PROJECT)
    require(not re.search(r'NES_(?:C6_AUDIO|EXPERIMENTAL|FRAME_(?:STAGE_COST|CACHE|REMOTE|PREDISPATCH)|PPU_SPLIT)', tail),
            'Foreign C6 DSP/diagnostic image markers')
    require(re.search(r'\bBOARD_READY display=1 touch=1 audio=1 sd=1 psram=1\b', tail), 'Admission lacks healthy post-boot BOARD_READY')
    return {'known_full_ELF': elf, 'observed_last_boot_prefix': prefix,
            'boot_prefix_is_not_runtime_full_hash': True, 'target_review_and_flash_owned_by_Root': True,
            'DSP_owner': 'P4 Main', 'C6_role': 'Bluetooth transport only; no C6 DSP RPC'}


def check_bank(data, path):
    """Требует тот же26-game bank; громкость28 задаётся read-only, edges/ROI не меняются."""
    require(digest(path) == MOVIES_SHA and len(data.get('games', [])) == 26, 'Paired26 movie bank changed')
    for m in data['games']:
        require(m['edges'] and all(a['frame'] < b['frame'] for a, b in zip(m['edges'], m['edges'][1:])), 'Movie edge order')
        rolling_queue_for(m)
    return data['games']


def run_game(link, movie, entry):
    """Один полный movie; status/tasks выполняются до ROI и после последнего exact report."""
    start = len(link.rows)
    cursor = start
    reports, harts, traces, scheduled, applied, pace = [], [], [], [], [], []
    rolling = rolling_queue_for(movie)
    plan = movie['queue_plan']
    initial, deferred = plan['initial'], plan['deferred']
    deferred_sent = not deferred
    early_at, early_scene, early_audio = None, None, None
    started = False
    mode = None
    error = None
    result = {}
    end_report = math.ceil(movie['end_frame'] / 120) * 120
    deadline = time.monotonic() + max(240, end_report / 20 + 90)
    try:
        link.send(['run ' + str(entry['index'])] + (['region ntsc'] if entry['pal'] else []), 'start_movie')
        if rolling:
            rolling.start(link.send)
        else:
            link.send(['padat ' + x.replace(':', ' ') for x in initial], 'initial_pad_queue')
        while time.monotonic() < deadline:
            link.read()
            lines = link.rows[cursor:]
            cursor = len(link.rows)
            packets = [p for line in lines if (p := packet(line))]
            if rolling:
                rolling.observe_batch(packets)
            for p in packets:
                tag, f, line = p['raw_type'], p['fields'], p['line']
                if rolling:
                    rolling.on_packet(p)
                if tag == 'NES_GAME_START':
                    require(str(f['crc']).lower() == movie['crc32'] and f['mapper'] == movie['mapper'], 'Loaded ROM mismatch')
                    started = True
                elif tag == 'NES_OUTPUT_FRAME_PIPELINE_MODE':
                    require(f.get('schema') == 2 and f.get('slots') == 2 and f.get('mapper') == movie['mapper']
                            and f.get('producer') == 'timing_fetch' and f.get('consumer') == 'output_compositor'
                            and f.get('rgb') == 'board_internal_direct' and f.get('borrowed') == 1
                            and f.get('copy') == 0, 'Unexpected local Frame/DSP mode')
                    mode = f
                elif tag == 'NES_INPUT':
                    base.input_trace(f, movie)
                    traces.append(f)
                elif tag == 'NES_PAD_SCHEDULE':
                    require(uint(f, 'queued') <= 16, 'Queue capacity exceeded')
                    scheduled.append((f['frame'], f['mask']))
                elif tag == 'NES_PAD_APPLY':
                    require(f['frame'] == f['scheduled'], 'Late movie edge; no shifting')
                    applied.append((f['scheduled'], f['mask']))
                elif tag == 'NES_OUTPUT_FRAME_PIPELINE_TIMES':
                    r = perf_row(line)
                    require(r['game'] == entry['index'] and r['mapper'] == movie['mapper'], 'Report game drift')
                    reports.append(r)
                elif tag == 'NES_FRAME_HART_STATS':
                    harts.append(f)
                elif tag == 'NES_FRAME_LOOP_METRICS' or 'PACE' in line:
                    pace.append(line)
                if rolling and tag in ('NES_PAD_SCHEDULE', 'NES_PAD_APPLY'):
                    rolling.refill(link.send)
            if rolling:
                rolling.finish_batch()
            progress = max([0, *[r['frames'] for r in reports], *[f for f, _ in applied]])
            if started and len(scheduled) >= len(initial) and early_at is None:
                require(progress < movie['baseline_report'] - 120, 'Late start status would taint ROI')
                early_at = len(link.rows)
                link.send(['status', 'ui_status'], 'early_joined_snapshot_before_ROI')
            early_lines = link.rows[early_at:] if early_at is not None else []
            if early_scene is None and any('NES_UI_STATUS ' in x for x in early_lines):
                early_scene = base.latest(early_lines, 'NES_OUTPUT_FRAME_PIPELINE_STATUS')
                early_audio = base.latest(early_lines, 'NES_FRAME_AUDIO_STATUS')
                require(early_scene.get('playing') is True and early_scene.get('crc32') == movie['crc32']
                        and early_scene.get('game') == entry['index'] and early_scene.get('region') == 'NTSC'
                        and 0 < early_scene.get('frames', 0) < movie['baseline_report'] - 120, 'Early joined scene identity/cut')
                base.quiet(base.latest(early_lines, 'NES_SETTINGS'), True, link.expected_volume)
            if not rolling and not deferred_sent and set(tuple(map(int, x.split(':'))) for x in initial[:2]) <= set(applied):
                require(progress < plan['latest_safe_progress_frame'] and progress < movie['baseline_report'], 'Legacy deferred window missed')
                require(len(scheduled) - len(applied) + len(deferred) <= 16, 'No proven legacy queue room')
                link.send(['padat ' + x.replace(':', ' ') for x in deferred], 'legacy_deferred_after_APPLY')
                deferred_sent = True
            if reports and reports[-1]['frames'] >= end_report and len(applied) == len(movie['edges']):
                last = reports[-1]
                if any(hart_key(h) == (last['game'], last['generation'], last['frames'] - 120, last['frames'] - 1) for h in harts):
                    break
        else:
            raise ValueError('Movie deadline; no host-time FPS fallback')
        require(started and early_scene is not None and mode is not None, 'Missing launch/mode/early joined snapshot')
        expected = [(e['frame'], e['mask']) for e in movie['edges']]
        require(scheduled == applied == expected, 'Complete movie ACK/APPLY mismatch')
        if rolling:
            rolling.finish()
        else:
            require(deferred_sent, 'Missing legacy deferred input')
        input_proof = base.input_coverage(traces, movie, reports)
        stats = statistics(reports, harts, movie)
        cut_start = len(link.rows)
        link.send(['status', 'ui_status'], 'end_actual_joined_snapshot_after_reports')
        link.collect(1)
        end_lines = link.rows[cut_start:]
        scene = base.latest(end_lines, 'NES_OUTPUT_FRAME_PIPELINE_STATUS')
        present = base.latest(end_lines, 'NES_FRAME_PIPELINE_PRESENT_STATUS')
        audio = base.latest(end_lines, 'NES_FRAME_AUDIO_STATUS')
        base.quiet(base.latest(end_lines, 'NES_SETTINGS'), True, link.expected_volume)
        proof = validate_scene(scene, present, audio, movie, entry, reports[0]['generation'], end_report, link.expected_volume)
        for k in ('accepted_frames', 'dma_frames'):
            require(uint(audio, k) > uint(early_audio, k), 'Local audio counter did not advance: ' + k)
        # UART status сам способен задержать обслуживание I2S вне ROI; delta сохраняется, не объявляется pure gameplay quality.
        audio_delta = {k: uint(audio, k) - uint(early_audio, k) for k in ('accepted_frames', 'dma_frames', 'missed_refill_blocks', 'missed_refill_frames')}
        require(all(v >= 0 for v in audio_delta.values()), 'Local audio counter regression')
        result.update(statistics=stats, input_proof=input_proof, end_scene_status={'raw_type': 'NES_OUTPUT_FRAME_PIPELINE_STATUS', 'fields': scene},
                      endpoint_proof=proof, end_present_status=present, end_audio_status=audio,
                      early_joined_status=early_scene, early_audio_status=early_audio, local_audio_counter_delta=audio_delta,
                      local_audio_delta_scope='includes outside-ROI UART status; no physical audio quality claim')
    except Exception as exc:
        error = str(exc)
    # Даже HOLD сохраняет quarantine: menu проходит штатный actual join/drain, timeout не подменяется free.
    cleanup_at = len(link.rows)
    try:
        link.send(['pad 0', 'menu', 'status', 'ui_status', 'tasks'], 'cleanup_after_endpoint')
        link.collect(2)
        result['cleanup'] = validate_cleanup(link.rows[cleanup_at:], link.expected_volume)
    except Exception as exc:
        error = (error + '; cleanup: ' if error else 'cleanup: ') + str(exc)
    result.update(status='HOLD' if error else 'PASS_CAPTURE_SCOPE_ONLY_DEVICE_US_INPUT_HART_JOINED_SNAPSHOTS',
                  errors=[error] if error else [], movie=movie, actual_catalog_entry=entry,
                  requested_end_frame=movie['end_frame'], actual_end_report=reports[-1]['frames'] if reports else None, requested_end_report=end_report,
                  tail_extended_to_report=end_report - movie['end_frame'], reports=reports, hart_rows=harts,
                  input_traces=traces, applied_edges=applied, schedule_ACKs=scheduled, pace_logs=pace,
                  rolling_queue=rolling.snapshot() if rolling else None, frame_mode=mode,
                  actual_scene_oracle_match=False, physical_LCD_or_Bluetooth_rerun_claim=False,
                  expected_volume_readonly=link.expected_volume, DSP_owner='P4 Main', C6_role='Bluetooth transport only')
    return result


def main(argv=None):
    """Сначала offline plan; только Root с явной image attestation может открыть COM."""
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--movies', type=Path, default=ROOT / 'tools/qualification-movies.json')
    p.add_argument('--games', nargs='*')
    p.add_argument('--expected-volume', type=int, required=True, choices=range(1,101))
    p.add_argument('--admission', type=Path)
    p.add_argument('--elf-sha')
    p.add_argument('--attest-admitted-image-current', action='store_true')
    p.add_argument('--execute-root', action='store_true')
    p.add_argument('--port', default='COM4')
    p.add_argument('--vid', type=lambda x: int(x, 0), default=0x1a86)
    p.add_argument('--pid', type=lambda x: int(x, 0), default=0x55d3)
    p.add_argument('--serial')
    p.add_argument('--output', type=Path, default=ROOT / PROJECT / 'betta/build/validation/device-runs')
    a = p.parse_args(argv)
    data = json.loads(a.movies.read_text(encoding='utf-8'))
    games = check_bank(data, a.movies)
    routes = {g['crc32']: g for g in games}
    wanted = [c.lower() for c in a.games] if a.games else list(routes)
    require(wanted and len(wanted) == len(set(wanted)) and set(wanted) <= routes.keys(), 'Unknown/duplicate games')
    if not a.execute_root:
        print(json.dumps({'execute': False, 'games': wanted, 'expected_project': PROJECT,
                          'expected_volume_readonly': a.expected_volume, 'COM_opened': False}, ensure_ascii=False))
        return
    require(a.attest_admitted_image_current, 'Root current-image attestation required')
    require(a.admission and a.elf_sha, 'Current image ELF and boot log required')
    admitted = admission(a.admission.read_text(encoding='utf-8', errors='replace'), a.elf_sha)
    local = base.local_image(PROJECT, a.elf_sha)
    out = a.output.resolve() / time.strftime('p4-%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=False)
    summary = {'schema': 'p4_fresh_paired_device_us_v1', 'tool': pin(Path(__file__)),
               'shared_base': pin(BASE_PATH), 'rolling_helper': pin(ROOT / 'tools/pad_movie_queue.py'),
               'movies': pin(a.movies), 'admission': pin(a.admission), 'image_admission': admitted,
               'ELF': a.elf_sha, 'local_P4_image': local, 'expected_volume_readonly': a.expected_volume, 'games': [],
               'host_time_FPS': False, 'logger_ms_fallback': False, 'NVS_written': False,
               'DSP_owner': 'P4 Main', 'C6_role': 'Bluetooth transport only', 'independent_native_oracle': False}
    link = None
    try:
        link = base.Link(a, out / 'raw.log')
        live = base.catalog(link)
        summary.update(USB=link.usb, actual_catalog=list(live.values()))
        for crc in wanted:
            require(crc in live and live[crc]['mapper'] == routes[crc]['mapper'], 'Catalog ROM/mapper mismatch: ' + crc)
            begin, command_start = link.log.tell(), len(link.commands)
            result = run_game(link, routes[crc], live[crc])
            link.log.flush()
            rawpath = out / (crc + '.raw.log')
            rawpath.write_bytes((out / 'raw.log').read_bytes()[begin:])
            result.update(raw=pin(rawpath), commands=link.commands[command_start:])
            path = out / (crc + '.json')
            path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            summary['games'].append({'crc32': crc, 'status': result['status'],
                                     'FPS': result.get('statistics', {}).get('FPS'), 'result': pin(path)})
            require(not result['errors'], '; '.join(result['errors']))
        summary['status'] = 'PASS_SELECTED_CAPTURE_SCOPE_ONLY_NOT_NATIVE_SCENE_OR_PHYSICAL_QUALITY'
    except Exception as exc:
        summary.update(status='HOLD', error=str(exc))
        raise
    finally:
        if link:
            link.close()
        (out / 'result.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'output': str(out), 'status': summary.get('status'), 'games': len(summary['games'])}))

if __name__ == '__main__':
    main()
