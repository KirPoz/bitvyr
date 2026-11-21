"""Готовит C6 input bank с равными120-frame ROI, без COM, SDK и изменения Core.

Пример: python -B tools/prepare_c6_routes.py --rom-dir ./roms/nes_rom
    --source pinned --windows 3 --output PATH/routes.json
--native-runner PATH/nes_run.exe --preview-crcs CRC ... добавляет только host PPM.
Старый native CLI имеет16 edges; новый2048 задаётся --native-max-edges 2048.
Полный command line проверяется до запуска; поток не сокращается.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
import zlib

from pad_movie_queue import rolling_queue_for

HERE = Path(__file__).resolve().parent
WINDOW = 120
MAX_ROM_BYTES = 4 * 1024 * 1024
MAX_NATIVE_EDGES = 16
MAX_FRAMES = 360000


def require(okay, reason):
    """Fail-closed guard остаётся действующим при python -O."""
    if not okay:
        raise ValueError(reason)


def uint(value, name, positive=False):
    """Не принимает bool/float вместо guest-frame/маски."""
    require(type(value) is int and value >= int(positive), 'Invalid unsigned ' + name)
    return value


def sha(data):
    """Хеширует исходные байты, не нормализуя ROM или журнал."""
    return hashlib.sha256(data).hexdigest()


def pin(path):
    """Фиксирует существующий вход; ROM payload в JSON не включается."""
    path = Path(path)
    raw = path.read_bytes()
    return dict(path=str(path.resolve()), bytes=len(raw), sha256=sha(raw))


def input_hash(edges):
    """Связывает полные frame/mask, а не только ненулевые нажатия."""
    return sha(json.dumps(edges, sort_keys=True, separators=(',', ':')).encode())


def read_rom(path, member=None):
    """Читает один уже существующий ROM; ZIP не извлекается в исходное дерево.

    Размер проверяется до decompression. zipfile проверяет CRC контейнера;
    identity банка вычисляется заново по полным распакованным NES bytes.
    """
    path = Path(path)
    if member is None:
        require(16 <= path.stat().st_size <= MAX_ROM_BYTES, 'ROM size outside bounded inventory')
        return path.read_bytes()
    with zipfile.ZipFile(path) as archive:
        info = archive.getinfo(member)
        require(not info.is_dir() and not info.flag_bits & 1 and
                16 <= info.file_size <= MAX_ROM_BYTES, 'Unsupported/encrypted/oversized ZIP member')
        return archive.read(info)


def rom_identity(raw):
    """Определяет CRC/SHA/mapper заголовка; это не замена Core load qualification."""
    require(16 <= len(raw) <= MAX_ROM_BYTES and raw[:4] == b'NES\x1a', 'Invalid NES image header/size')
    nes2 = raw[7] & 12 == 8
    mapper = (raw[6] >> 4) | (raw[7] & 0xf0) | ((raw[8] & 15) << 8 if nes2 else 0)
    return dict(crc32=f'{zlib.crc32(raw) & 0xffffffff:08x}', sha256=sha(raw), bytes=len(raw),
                mapper=mapper, submapper=raw[8] >> 4 if nes2 else 0, format='NES2' if nes2 else 'iNES')


def inventory_roms(directory):
    """Индексирует только локальные .nes/.zip; совпавший CRC с разными SHA неоднозначен."""
    directory = Path(directory)
    require(directory.is_dir(), 'Local ROM directory not found')
    by_crc, errors = {}, []
    for path in sorted(directory.iterdir()):
        if not path.is_file() or path.suffix.lower() not in ('.nes', '.zip'):
            continue
        try:
            members = [None]
            if path.suffix.lower() == '.zip':
                with zipfile.ZipFile(path) as archive:
                    members = [i.filename for i in archive.infolist()
                               if not i.is_dir() and i.filename.lower().endswith('.nes')]
                require(members and len(members) == len(set(members)), 'Missing/ambiguous NES ZIP members')
            for member in members:
                identity = rom_identity(read_rom(path, member))
                origin = dict(path=str(path.resolve()), member=member)
                by_crc.setdefault(identity['crc32'], []).append(dict(identity, origin=origin))
        except (ValueError, OSError, zipfile.BadZipFile, RuntimeError) as exc:
            errors.append(dict(path=str(path.resolve()), error=str(exc)))
    return dict(by_crc=by_crc, errors=errors, local_only=True, ROM_bytes_in_bank=False)


def checked_edges(edges):
    """Сохраняет все edges по порядку; не дополняет действия и не сдвигает timestamps."""
    require(isinstance(edges, list) and edges, 'Missing input prefix')
    result = []
    for edge in edges:
        frame, mask = uint(edge.get('frame'), 'frame'), uint(edge.get('mask'), 'mask')
        require(frame <= MAX_FRAMES and mask <= 255 and (not result or frame > result[-1]['frame']), 'Input mask/order/duplicate fault')
        require(frame != 0 or mask == 0, 'UART cannot guarantee nonzero initial frame0')
        result.append(dict(frame=frame, mask=mask))
    return result


def queue_plan(edges):
    """Одна rolling policy: все frames/masks,14 initial reservations и реальные APPLY credits."""
    text = [f"{e['frame']}:{e['mask']}" for e in edges]
    plan = dict(mode='rolling', queue_capacity=16, initial=text[:14], deferred=text[14:])
    rolling_queue_for(dict(edges=edges, queue_plan=plan))
    return plan


def choose_roi(preferred_start, end_exclusive, windows):
    """Ровно N×120 внутри известного prefix; короткий маршрут не растягивается.

    Начальный observer требует baseline>=240. При нехватке tail baseline можно
    сдвинуть назад целыми окнами; новый scene scope всегда маркируется явно.
    """
    length = uint(windows, 'windows', True) * WINDOW
    end = uint(end_exclusive, 'end_exclusive', True) // WINDOW * WINDOW
    require(end >= 2 * WINDOW + length, 'SHORT_PREFIX_FOR_EQUAL_WINDOWS')
    preferred = max(2 * WINDOW, (uint(preferred_start, 'baseline') + WINDOW - 1) // WINDOW * WINDOW)
    start = min(preferred, end - length)
    return start, [start + WINDOW * i for i in range(1, windows + 1)]


def human_source(segments, crc, variant_ids):
    """Не выбирает молча один из повторных запусков того же ROM."""
    matches = [s for s in segments if s.get('crc32') == crc]
    if variant_ids:
        selected = [s for s in matches if s.get('variant_id') in variant_ids or s.get('session_id') in variant_ids]
        if selected:
            matches = selected
    require(len(matches) == 1, 'MISSING_OR_AMBIGUOUS_HUMAN_VARIANT')
    s = matches[0]
    require(s.get('replayable') is True and not s.get('hold_reasons'), 'HUMAN_SCOPE_HOLD')
    require(s.get('first_frame') == 0 and s.get('frame0_source') is not None, 'HUMAN_FRAME0_UNAVAILABLE')
    require(s.get('source_generation') and s.get('start_reason') in ('GAME_START', 'known_fresh_reload_candidate'),
            'HUMAN_INITIAL_STATE_UNSUPPORTED')
    return s


def build_bank(pinned, human, roms, windows=3, source='pinned', crcs=None,
               variant_ids=(), human_kind='moving_input_candidate', roi_starts=None,
               scene_recipe=None, battle_bank=None):
    """Возвращает совместимый с collector банк и отдельную честную coverage таблицу.

    Входные JSON не изменяются. Равны измеряемые ROI, а меню/boot prefix до сцены
    могут иметь разную длину. Scene/full-game/physical BLE qualification не выводится из pad activity.
    """
    require(uint(windows, 'windows', True) <= MAX_FRAMES//WINDOW, 'Too many windows')
    require(source in ('pinned', 'human', 'catalog'), 'Unknown input source')
    require((source == 'catalog') == (scene_recipe is not None), 'Catalog mode requires a scene recipe')
    recipe = {}
    if scene_recipe is not None:
        require(scene_recipe.get('schema') == 'bitvyr_c6_scene_recipe_v1', 'Unsupported scene recipe schema')
        require(not roi_starts and not variant_ids, 'Use pinned or human mode for explicit overrides')
        recipe = scene_recipe['human_overrides']
    references = pinned.get('games', [])
    require(references and len({g['crc32'] for g in references}) == len(references), 'Duplicate/missing pinned CRC')
    routes = {g['crc32']: g for g in references}
    battles = {}
    if battle_bank is not None:
        require(source == 'catalog' and battle_bank.get('schema') == 1, 'Invalid catalog battle bank')
        battle_games = battle_bank['games']
        battles = {g['crc32']: g for g in battle_games}
        require(len(battles) == len(battle_games) and battles.keys() <= routes.keys(),
                'Unknown or duplicate battle CRC')
        require(not battles.keys() & recipe.keys(), 'Battle and recorded scene recipes overlap')
    require(set(recipe) <= routes.keys(), 'Unknown scene recipe CRC')
    for scene in recipe.values():
        start = uint(scene['start'], 'scene start')
        require(start >= 2*WINDOW and start % WINDOW == 0, 'Scene recipe start must be120-aligned and>=240')
        require(scene['variant'] and scene['description'], 'Scene recipe must identify recorded input and scene')
    wanted = list(routes) if crcs is None else list(crcs)
    require(wanted and len(set(wanted)) == len(wanted) and set(wanted) <= routes.keys(), 'Unknown/duplicate requested CRC')
    roi_starts = {} if roi_starts is None else roi_starts
    require(set(roi_starts) <= set(wanted), 'ROI override CRC not requested')
    for start in roi_starts.values():
        require(uint(start, 'ROI override') >= 2*WINDOW and start % WINDOW == 0, 'ROI override must be120-aligned and>=240')
    if source in ('human', 'catalog'):
        require(human.get('schema') == 'bitvyr_human_input_bank_v1', 'Unsupported human bank schema')
        ids = {s.get(k) for s in human.get('segments', []) for k in ('session_id','variant_id')}
        require(set(variant_ids) <= ids, 'Unknown human variant ID')
    games, coverage = [], []
    for crc in wanted:
        battle = battles.get(crc)
        reference = battle if battle else routes[crc]
        scene = recipe.get(crc)
        route_source = source
        if battle:
            route_source = 'battle'
        elif source == 'catalog':
            route_source = 'human' if scene else 'pinned'
        roi_override = scene['start'] if scene else roi_starts.get(crc)
        route_variants = (scene['variant'],) if scene else variant_ids
        row = dict(crc32=crc, title=reference['title'], source=route_source, status='HOLD')
        try:
            local = roms['by_crc'].get(crc, [])
            require(local and len({r['sha256'] for r in local}) == 1, 'ROM_MISSING_OR_CRC_COLLISION')
            require(reference.get('region') == 'NTSC', 'CURRENT_C6_BANK_REQUIRES_EXPLICIT_NTSC')
            identity = {k: v for k, v in local[0].items() if k != 'origin'}
            require(identity['mapper'] == reference['mapper'] == routes[crc]['mapper'], 'ROM_HEADER_MAPPER_MISMATCH')
            if battle:
                require(identity['sha256'] == battle['rom_identity']['sha256'], 'BATTLE_ROM_SHA_MISMATCH')
            if route_source != 'human':
                edges = checked_edges(reference['edges'])
                end_frame = uint(reference['end_frame'], 'end_frame', True)
                require(end_frame <= MAX_FRAMES, 'Route prefix exceeds bounded frame domain')
                require(all(e['frame'] < end_frame for e in edges), 'INPUT_OUTSIDE_DECLARED_ROUTE')
                preferred = reference['baseline_report'] if roi_override is None else roi_override
                start, reports = choose_roi(preferred, end_frame, windows)
                require(roi_override is None or start == roi_override, 'Requested ROI exceeds known prefix')
                if battle:
                    require(start == battle['roi_input0'][0] and reports[-1] - 1 <= battle['roi_input0'][1],
                            'REQUESTED_WINDOWS_EXCEED_PINNED_BATTLE')
                game = copy.deepcopy(reference)
                provenance = dict(kind='PINNED_BATTLE_INPUT' if battle else 'PINNED_FIXED_INPUT',
                                  original_input_sha256=input_hash(edges),
                                  original_roi=reference['roi_input0'], original_requested_end_frame=end_frame,
                                  all_source_edges_preserved=True, exact_human_replay_proof=False)
                if battle:
                    provenance['battle_input_provenance'] = copy.deepcopy(battle['input_provenance'])
            else:
                s = human_source(human.get('segments', []), crc, route_variants)
                require(not scene or s.get('variant_id') == scene['variant'], 'SCENE_RECIPE_VARIANT_MISSING')
                require(s.get('mapper') == reference['mapper'] and s.get('region') == reference['region'], 'HUMAN_ROM_REGION_MISMATCH')
                points = checked_edges(s['events'])
                require(points[0] == dict(frame=0, mask=0), 'HUMAN_FRAME0_UNAVAILABLE')
                candidates = [c for c in s.get('roi_candidates', []) if c.get('kind') == human_kind]
                require(candidates, 'HUMAN_ROI_UNAVAILABLE')
                candidate = candidates[0]
                completed = uint(s['last_completed_frame'], 'last_completed_frame', True)
                require(completed <= MAX_FRAMES, 'Human prefix exceeds bounded frame domain')
                uint(s['source_generation'], 'human generation', True)
                bound = completed if roi_override is not None else min(completed, uint(candidate['roi_input_last'], 'roi_input_last') + 1)
                preferred = candidate['baseline_report'] if roi_override is None else roi_override
                start, reports = choose_roi(preferred, bound, windows)
                minimum = max(2*WINDOW, (preferred+WINDOW-1)//WINDOW*WINDOW)
                require(start >= minimum, 'SHORT_HUMAN_SCENE_INTERVAL')
                # До 120 frames запаса из уже наблюдённого prefix для end observer; никаких новых pad edges.
                end_frame = min(completed // WINDOW * WINDOW, reports[-1] + WINDOW)
                edges = [e for e in points if e['frame'] < end_frame]
                game = {k: copy.deepcopy(reference[k]) for k in ('crc32', 'title', 'mapper', 'region', 'rom_pal') if k in reference}
                provenance = dict(kind='RECORDED_HUMAN_CANDIDATE', source_guid=s['source_guid'],
                    session_id=s['session_id'], variant_id=s['variant_id'], source_generation=s['source_generation'],
                    source_input_prefix_sha256=s['input_prefix_sha256'], last_completed_frame=completed,
                    chosen_prefix_end_exclusive=end_frame, excluded_after_prefix_events=len(points)-len(edges),
                    source_frame0=s['frame0_source'], source_last_completed=s['last_completed_source'],
                    source_input_counts=s['source_input_counts'], source_observed_gaps=s.get('observed_gaps', []),
                    all_chosen_prefix_edges_preserved=True, exact_human_replay_proof=False,
                    physical_BLE_HCI_load_reproduced=False, inherited_limitations=s.get('limitations', []),
                    after_chosen_prefix=dict(held_mask=edges[-1]['mask'],
                        source='collector retains last movie mask until cleanup; not asserted original human input beyond prefix',
                        measured_ROI=False,end_observer_may_advance_beyond_prefix=True))
            require(edges and all(e['frame'] < end_frame for e in edges), 'Missing/incomplete chosen prefix')
            original = [candidate['roi_input0'], candidate['roi_input_last']] if route_source == 'human' else reference['roi_input0']
            overlap = max(0, min(reports[-1], original[1]+1) - max(start, original[0]))
            game.update(baseline_report=start, roi_reports=reports, roi_input0=[start, reports[-1]-1],
                        end_frame=end_frame, edges=edges, queue_plan=queue_plan(edges), rom_identity=identity,
                        input_provenance=provenance,
                        qualification_scope=dict(measured_guest_frames=windows*WINDOW, windows=windows,
                            equal_ROI_not_equal_reset_prefix=True, region=game['region'], scene_verified=False,
                            full_game_coverage=False, hardware_endpoint_verification_required=True,
                            prior_FPS_transferred=False, recorded_activity_is_not_movement_proof=True),
                        scene_scope=dict(label='HUMAN_INPUT_CANDIDATE_SCENE_UNVERIFIED' if route_source == 'human' else 'FIXED_INPUT_SCENE_UNVERIFIED',
                            source_roi=original, original_roi_overlap_frames=overlap,
                            new_or_repositioned_roi=[start,reports[-1]-1]!=original,
                            explicit_ROI_override=roi_override is not None,
                            native_preview_status='NOT_RUN', hardware_RGB_or_physical_LCD_proof=False))
            if scene:
                game['scene_scope']['recipe_description'] = scene['description']
            if battle and game['roi_input0'] == battle['roi_input0']:
                game['scene_scope']['source_native_battle'] = copy.deepcopy(battle['scene_scope'])
            games.append(game)
            row.update(status='PREPARED_INPUT_CANDIDATE', measured_guest_frames=windows*WINDOW,
                       roi_input0=game['roi_input0'], total_prefix_frames=end_frame, edges=len(edges),
                       ROI_changed=game['scene_scope']['new_or_repositioned_roi'], scene_verified=False)
        except (ValueError, KeyError, TypeError) as exc:
            row['reason'] = str(exc)
        coverage.append(row)
    bank = dict(schema=1, settings=dict(region='forced_NTSC', volume=28, muted=False), queue_capacity=16,
                games=games, route_preparation=dict(schema='c6_equal_window_routes_v1', input_source=source,
                    window_frames=WINDOW, requested_windows=windows, measured_guest_frames_per_game=windows*WINDOW,
                    requested_CRCs=wanted, prepared_count=len(games), requested_count=len(wanted),
                    complete_requested_route_coverage=len(games)==len(wanted), scene_qualified_count=0,
                    coverage_definition="prepared/requested ROM routes, not exact input-log or scene completeness",
                    exact_human_input_completeness_proof=False if source in ('human', 'catalog') else None,
                    native_or_hardware_FPS_claim=False, ROM_payload_in_bank=False,
                    limits=['ROI length is equal; reset/menu prefix lengths remain different.',
                            'Changed ROI requires new same-cut native/hardware scene checks; no prior FPS transfer.',
                            'Human changed-edge UART is a replay candidate, not exact input or BLE workload proof.',
                            'Short/missing/ambiguous coverage is HOLD, never padded or silently replaced.']))
    validate_bank(bank)
    return bank, coverage


def validate_bank(bank):
    """Проверяет именно collector frames/edges/queue, не объявляя сцену квалифицированной."""
    n = bank['route_preparation']['requested_windows']
    for g in bank['games']:
        start = g['baseline_report']
        require(start >= 2*WINDOW and start % WINDOW == 0, 'Observer/120 baseline fault')
        require(g['roi_reports'] == [start+WINDOW*i for i in range(1,n+1)] and
                g['roi_input0'] == [start,start+n*WINDOW-1] and g['roi_reports'][-1] <= g['end_frame'], 'Unequal/invalid ROI')
        checked_edges(g['edges'])
        rolling_queue_for(g)
    return True


def native_arguments(runner, rom, game, frame, ppm, max_edges):
    """Передаёт полный input, проверяет Win32 limit и только неисполняемый release sentinel.

    nes_run требует last mask0. Если выбранный prefix заканчивается удержанием,
    sentinel ставится строго ПОСЛЕ всех capture frames и source edges. Он не
    исполняется и не входит в C6 bank. Никакое удержание внутри ROI не меняется.
    """
    uint(max_edges, 'native max edges', True)
    events = copy.deepcopy(game['edges'])
    sentinel = None
    if events[-1]['mask'] != 0:
        sentinel = dict(frame=max(game['end_frame'], game['roi_reports'][-1]+1, events[-1]['frame']+1),mask=0)
        require(sentinel['frame'] <= 360000, 'NATIVE_SENTINEL_OUT_OF_RANGE')
        events.append(sentinel)
    require(len(events) <= max_edges, 'NATIVE_EDGE_LIMIT; full input was not shortened')
    args = [str(runner),str(rom),'--frames',str(frame),'--ppm',str(ppm)]
    if game['region'] == 'PAL':
        args.append('--pal')
    for edge in events:
        args += ['--pad-frame',f"{edge['frame']}:{edge['mask']}"]
    length = len(subprocess.list2cmdline(args))+1
    require(length <= 32767, 'NATIVE_WIN32_COMMAND_LINE_LIMIT; full input was not shortened')
    return args, dict(input_edges=len(game['edges']),native_argument_edges=len(events),
        native_only_unexecuted_sentinel=sentinel, Windows_command_chars_including_NUL=length,
        declared_runner_edge_limit=max_edges, captured_input_prefix_unchanged=True)


def native_previews(bank, roms, runner, output, crcs, max_edges=MAX_NATIVE_EDGES):
    """Снимает начало/середину/конец через закреплённый nes_run; превышение declared limit даёт NA.

    Это host Machine preview, не Frame-owner proof и не аппаратный FPS. ROM
    живёт только во временном каталоге процесса; PPM и raw result сохраняются.
    """
    runner, output = Path(runner).resolve(), Path(output)
    require(runner.is_file(), 'Native runner not found')
    output.mkdir(parents=True, exist_ok=False)
    results = []
    for game in bank['games']:
        if game['crc32'] not in crcs:
            continue
        result = dict(crc32=game['crc32'], runner=pin(runner), status='UNAVAILABLE', cuts=[],
                      host_Machine_not_Frame_or_hardware=True, FPS_claim=False)
        try:
            native_arguments(runner,'input.nes',game,game['roi_reports'][-1],output/'probe.ppm',max_edges)
        except ValueError as exc:
            result['reason'] = str(exc)
            results.append(result)
            continue
        identity = roms['by_crc'][game['crc32']][0]
        raw = read_rom(**identity['origin'])
        require(rom_identity(raw)['sha256'] == game['rom_identity']['sha256'], 'ROM changed after inventory')
        a, end = game['baseline_report'], game['roi_reports'][-1]
        cuts = [('begin_after_first_input',a+1),('middle',a+(end-a)//2),('end',end)]
        with tempfile.TemporaryDirectory(prefix='bitvyr-route-') as temporary:
            rom = Path(temporary)/'input.nes'
            rom.write_bytes(raw)
            for label, frame in cuts:
                prefix = output/(game['crc32']+'-'+label)
                ppm = prefix.with_suffix('.ppm')
                args, input_proof = native_arguments(runner,rom,game,frame,ppm,max_edges)
                completed = subprocess.run(args, capture_output=True, text=True, timeout=180, encoding='utf-8', errors='replace')
                log = prefix.with_suffix('.log')
                log.write_text(completed.stdout+completed.stderr,encoding='utf-8')
                row = json.loads(completed.stdout.strip().splitlines()[-1]) if completed.stdout.strip() else {}
                require(completed.returncode == 0 and row.get('frames') == frame and
                        row.get('mapper') == game['mapper'] and row.get('pal') == (game['region']=='PAL') and ppm.is_file(), 'Native preview failed: '+game['crc32'])
                result['cuts'].append(dict(label=label,completed_frame=frame,input_frame=frame-1,native_input_proof=input_proof,
                    frame_hash=row.get('frame_hash'),pcm_hash=row.get('pcm_hash'),pcm_count=row.get('pcm_count'),
                    changed_frames_in_reset_prefix=row.get('changed_frames'),audio_energy_in_reset_prefix=row.get('audio_energy'),
                    image=pin(ppm),raw_log=pin(log),scene_review='PENDING_VISUAL_REVIEW'))
        result['status']='PASS_HOST_PREVIEW_EXECUTION_NOT_SCENE_QUALIFICATION'
        results.append(result)
    return results


def main():
    """Печатает краткий план либо создаёт новый банк; существующий результат не перезаписывается."""
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rom-dir',type=Path,required=True)
    p.add_argument('--source',choices=['pinned','human','catalog'],default='pinned')
    p.add_argument('--scene-recipe',type=Path,default=HERE/'c6-scene-recipe.json')
    p.add_argument('--pinned-bank',type=Path,default=HERE/'qualification-movies.json')
    p.add_argument('--human-bank',type=Path,default=HERE/'human-playthrough-movies.json')
    p.add_argument('--windows',type=int,default=3)
    p.add_argument('--crcs',nargs='+')
    p.add_argument('--variant',action='append',default=[])
    p.add_argument('--roi-start',action='append',default=[],metavar='CRC:FRAME')
    p.add_argument('--human-kind',choices=['moving_input_candidate','sustained_heavy_candidate'],default='moving_input_candidate')
    p.add_argument('--output',type=Path)
    p.add_argument('--native-runner',type=Path)
    p.add_argument('--native-max-edges',type=int,choices=[16,2048],default=16)
    p.add_argument('--preview-crcs',nargs='+',default=[])
    args = p.parse_args()
    pinned = json.loads(args.pinned_bank.read_text(encoding='utf-8'))
    human = json.loads(args.human_bank.read_text(encoding='utf-8')) if args.source in ('human','catalog') else {}
    recipe = json.loads(args.scene_recipe.read_text(encoding='utf-8')) if args.source=='catalog' else None
    battle_path = HERE / 'c6-battle-scenes.json'
    battles = json.loads(battle_path.read_text(encoding='utf-8')) if recipe is not None else None
    roms = inventory_roms(args.rom_dir)
    roi_starts = {}
    for item in args.roi_start:
        parts=item.split(':')
        require(len(parts)==2 and parts[1].isascii() and parts[1].isdecimal() and parts[0] not in roi_starts,'Invalid/duplicate ROI override')
        roi_starts[parts[0]]=int(parts[1])
    bank, coverage = build_bank(pinned,human,roms,args.windows,args.source,args.crcs,args.variant,args.human_kind,
                                roi_starts,recipe,battle_bank=battles)
    bank['route_preparation']['sources'] = dict(pinned=pin(args.pinned_bank),
        human=pin(args.human_bank) if args.source in ('human', 'catalog') else None,
        scene_recipe=pin(args.scene_recipe) if recipe is not None else None,
        battles=pin(battle_path) if battles is not None else None,tool=pin(Path(__file__)))
    summary = dict(schema='c6_route_preparation_report_v1',preparation=bank['route_preparation'],coverage=coverage,
                   ROM_inventory=roms,COM_opened=False,SDK_built=False,native_previews=[])
    if args.output:
        output = args.output.resolve()
        report = output.with_suffix('.coverage.json')
        require(not output.exists() and not report.exists(), 'Output already exists; preserve previous result')
        output.parent.mkdir(parents=True,exist_ok=True)
        require(bool(args.native_runner)==bool(args.preview_crcs), 'Preview requires runner and explicit CRCs')
        if args.native_runner:
            require(set(args.preview_crcs) <= {g['crc32'] for g in bank['games']}, 'Preview CRC not prepared')
            summary['native_previews'] = native_previews(bank,roms,args.native_runner,output.with_suffix('.previews'),args.preview_crcs,args.native_max_edges)
            for preview in summary['native_previews']:
                game=next(g for g in bank['games'] if g['crc32']==preview['crc32'])
                game['scene_scope']['native_preview_status']=preview['status']
                game['scene_scope']['native_sampled_completed_frames']=[c['completed_frame'] for c in preview['cuts']]
                game['scene_scope']['native_visual_review']='PENDING_SEPARATE_REVIEW'
                game['scene_scope']['native_unavailable_reason']=preview.get('reason')
        output.write_text(json.dumps(bank,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        summary['bank']=pin(output)
        report.write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(dict(bank=pin(output),report=pin(report),prepared=len(bank['games']),requested=len(coverage),
                             measured_frames_per_game=args.windows*WINDOW,COM_opened=False),ensure_ascii=False))
    else:
        require(not args.native_runner and not args.preview_crcs, 'Preview requires output path')
        print(json.dumps(dict(preparation=bank['route_preparation'],coverage=coverage,ROM_inventory_errors=roms['errors'],COM_opened=False),ensure_ascii=False,indent=2))
    return 0 if len(bank['games'])==len(coverage) else 2

if __name__=='__main__':
    raise SystemExit(main())
