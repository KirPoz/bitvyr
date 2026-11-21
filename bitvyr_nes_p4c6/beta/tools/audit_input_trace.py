"""Сопоставляет фактические input trace и заданный movie отдельно от CPU/RGB/PCM proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
try:
    from input_movie import parse_edges, mask_at, changes
except ModuleNotFoundError:
    from tools.input_movie import parse_edges, mask_at, changes


def audit(path, starts, pad_edges=None, *, status_tag='NES_STATUS '):
    """Проверяет порядок, маски и финальный release trace для выбранной игры; графику и звук не квалифицирует."""
    if status_tag not in ('NES_STATUS ', 'NES_CPU_BENCH_STATUS ', 'NES_COUPLED_BENCH_STATUS ', 'NES_FRAME_PIPELINE_BENCH_STATUS ', 'NES_OUTPUT_FRAME_PIPELINE_STATUS '):
        raise ValueError('unsupported input-completion status tag')
    if pad_edges is not None and starts:
        raise ValueError('input movie cannot mix with Start pulses')
    if any(start < 0 for start in starts) or list(starts) != sorted(set(starts)):
        raise ValueError('Start pulses must have distinct increasing nonnegative frames')
    if any(second < first+3 for first, second in zip(starts, starts[1:])):
        raise ValueError('overlapping Start pulses are ambiguous for the serial scheduler')
    raw = path.read_bytes()
    lines = raw.decode('utf-8-sig', errors='strict').splitlines()
    runs = [i for i, line in enumerate(lines) if 'NES_GAME_START index=' in line]
    if len(runs) != 1:
        raise ValueError('expected exactly one game launch in each probe log')
    records = []
    for line in lines[runs[0]+1:]:
        if 'NES_INPUT ' not in line:
            continue
        pairs = re.findall(r'(\w+)=([^\s]+)', line.split('NES_INPUT ', 1)[1])
        row = dict(pairs)
        required = {'generation', 'frame', 'xbox_raw', 'xbox_effective', 'serial',
                    'touch', 'applied', 'contacts', 'ids', 'poll_ok', 'suppressed', 'action'}
        if set(row) != required or len(row) != len(pairs):
            raise ValueError('malformed input trace record')
        for name in required - {'action'}:
            row[name] = int(row[name], 16 if name == 'ids' else 10)
            if row[name] < 0:
                raise ValueError('negative input field')
        if row['action'] != 'none':
            raise ValueError(f"unexpected touch action at frame {row['frame']}")
        records.append(row)
    if not records or records[0]['frame'] != 0:
        raise ValueError('missing initial frame-zero input trace')
    generation = records[0]['generation']
    previous = -1
    for row in records:
        n = row['frame']
        if row['generation'] != generation or n <= previous:
            raise ValueError('input generation or frame ordering changed')
        previous = n
        expected = mask_at(pad_edges,n) if pad_edges is not None else (8 if any(start <= n < start+3 for start in starts) else 0)
        if row['xbox_effective'] or row['touch']:
            raise ValueError(f'physical controller/touch input at frame {n}')
        if row['serial'] != expected or row['applied'] != expected:
            raise ValueError(f'unexpected supplied input at frame {n}')
    # A changed-mask logger must contain every completed planned transition;
    # otherwise a truncated trace could incorrectly imply a held zero mask.
    completed = 0
    for line in lines[runs[0]+1:]:
        if status_tag in line:
            state = json.loads(line.split(status_tag, 1)[1])
            completed = max(completed, int(state['frames']))
    recorded = {row['frame'] for row in records}
    for start in starts:
        for transition in (start, start+3):
            if transition < completed and transition not in recorded:
                raise ValueError(f'missing input transition at frame {transition}')
    if pad_edges is not None:
        for transition in changes(pad_edges):
            if transition < completed and transition not in recorded:
                raise ValueError(f'missing input transition at frame {transition}')
    return {'path': str(path.resolve()), 'sha256': hashlib.sha256(raw).hexdigest(),
            'records': len(records), 'generation': generation, 'completed_frames': completed,
            'passed': True}


def main():
    """Читает явно заданный RAW и movie, записывает результат без доступа к COM."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+')
    parser.add_argument('--start-frame', type=int, action='append', default=[])
    parser.add_argument('--pad-frame', action='append', default=[], metavar='FRAME:MASK')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        pad_edges=parse_edges(args.pad_frame) if args.pad_frame else None
        if pad_edges is not None and args.start_frame:raise ValueError('input movie cannot mix with Start pulses')
    except ValueError as error:parser.error(str(error))
    results = []
    for path in args.logs:
        try:
            results.append(audit(path, args.start_frame, pad_edges))
        except (OSError, UnicodeError, ValueError, KeyError) as error:
            results.append({'path': str(path), 'passed': False, 'error': str(error)})
    report = {'passed': all(row['passed'] for row in results),
              'start_frames': args.start_frame, 'logs': results,
              'scope': 'Checks the recorded supplied masks; assumes complete UART capture and the pinned changed-input logger.'}
    if pad_edges is not None:report['pad_frame_edges']=pad_edges
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
