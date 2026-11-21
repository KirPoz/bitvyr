"""Запускает bounded HOST smoke для явно заданного локального ROM manifest.

Generic ввод не квалифицирован как gameplay movie. Успешный запуск/снимок
не доказывает прохождение игры, корректность сцены или аппаратный FPS.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import json
import subprocess
import time
import hashlib
import re

GENERIC_START = 360
GENERIC_BUTTONS = 131


def runner_cmake_context(runner, explicit_cache=None):
    """Фиксирует ближайший cache как контекст; он не доказывает flags уже собранного runner."""
    if explicit_cache is not None:
        cache_path = explicit_cache.resolve(strict=True)
        if not cache_path.is_file():
            raise ValueError('CMake cache must be a file')
    else:
        # Обычный CMake кладёт cache выше host/ или host/Release, рядом он бывает
        # только у standalone-сборки. За пределы этих трёх уровней не поднимаемся.
        cache_path = next((parent/'CMakeCache.txt' for parent in tuple(runner.parents)[:3]
                           if (parent/'CMakeCache.txt').is_file()), None)
    context = {'available': cache_path is not None,
               'path': str(cache_path) if cache_path else None,
               'sha256': None,
               'scope': 'CMake cache context only; not actual runner compile-flag proof'}
    if cache_path is None:
        context['reason'] = 'No CMakeCache.txt beside runner or in its two parent directories'
        return None, context
    raw = cache_path.read_bytes()
    context['sha256'] = hashlib.sha256(raw).hexdigest()
    options = dict(re.findall(r'^(NES_CORE_[A-Z0-9_]+):BOOL=(ON|OFF)\r?$',
                              raw.decode('utf-8-sig'), re.MULTILINE))
    return options, context


def run_game(game, runner, output, frames):
    """Исполняет один generic input smoke; индекс используется только для имени результата."""
    index = game['index']
    args = [str(runner), game['local_rom'], '--frames', str(frames), '--autoplay',
            '--ppm', str(output / f'{index:02d}-game.ppm'),
            '--wav', str(output / f'{index:02d}.wav')]
    if game['pal']:
        args.append('--pal')
    starts = [GENERIC_START]
    for moment in starts:
        args.extend(['--start',str(moment)])
    args.extend(['--play-at',str(starts[-1]+90)])
    args.extend(['--play-buttons',str(GENERIC_BUTTONS)])
    # Снимок имеет явный generic срез, но нахождение в игре не предполагается.
    requested_capture = starts[-1]+180
    capture_at = min(frames, requested_capture)
    args.extend(['--capture-at', str(capture_at)])
    result = subprocess.run(args, capture_output=True, text=True, timeout=180)
    (output / f'{index:02d}.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    try:
        report = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        report = {'error': result.stdout[-2000:], 'stderr': result.stderr[-2000:]}
    report.update(index=index, name=game['title'], returncode=result.returncode)
    report['start_pulses'] = starts
    report['play_buttons'] = GENERIC_BUTTONS
    report['input_scope'] = 'generic_unreviewed_not_gameplay_acceptance'
    report['screenshot_frame'] = capture_at
    report['requested_screenshot_frame'] = requested_capture
    report['screenshot_clamped'] = capture_at != report['requested_screenshot_frame']
    title_args = [str(runner),game['local_rom'],'--frames','300','--ppm',str(output/f'{index:02d}-title.ppm')]
    if game['pal']:
        title_args.append('--pal')
    title = subprocess.run(title_args, capture_output=True, text=True, timeout=90)
    report['title_returncode'] = title.returncode
    return report


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, default=root/'build/games/manifest.json')
    p.add_argument('--runner', type=Path, default=root/'build/native/host/nes_run.exe')
    p.add_argument('--cmake-cache', type=Path, help='Явный CMakeCache.txt; иначе ближайший из трёх уровней runner. Нет cache: metadata NA')
    p.add_argument('--output', type=Path, default=root/'build/tests/games')
    p.add_argument('--frames', type=int, default=4800)
    p.add_argument('--jobs', type=int, default=2)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    games = json.loads(a.manifest.read_text(encoding='utf-8-sig'))['games']
    runner = a.runner.resolve()
    runner_sha256 = hashlib.sha256(runner.read_bytes()).hexdigest()
    runner_options, runner_cache = runner_cmake_context(runner, a.cmake_cache)
    begin = time.monotonic()
    hashes = {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest()
              for f in sorted((root/'app/nes/core').rglob('*')) if f.is_file() and (f.suffix in ['.h','.hpp','.cpp','.cmake'] or f.name=='CMakeLists.txt')}
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = [pool.submit(run_game,g,a.runner,a.output,a.frames) for g in games]
        reports = []
        for f in futures:
            r = f.result()
            reports.append(r)
            print(json.dumps(r,ensure_ascii=False),flush=True)
    after = {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest()
             for f in sorted((root/'app/nes/core').rglob('*')) if f.is_file() and (f.suffix in ['.h','.hpp','.cpp','.cmake'] or f.name=='CMakeLists.txt')}
    if hashes != after:
        raise SystemExit('Core sources changed during the run; repeat against a stable build')
    data = {'frames_per_game':a.frames,'elapsed_seconds':time.monotonic()-begin,'core_sources':hashes,
            'runner_sha256':runner_sha256,'runner_core_cmake_options':runner_options,
            'runner_cmake_cache':runner_cache,'results':reports,
            'scope':'Bounded HOST execution with generic unreviewed input; no gameplay or hardware FPS acceptance.'}
    (a.output/'results.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    raise SystemExit(0 if all(r['returncode']==0 and r['title_returncode']==0 for r in reports) else 1)
