"""Читает и дополняет единый журнал Bitvyr; прежние измерения не изменяются."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import sys
import tempfile


def read_json(path):
    """Загружает UTF-8 JSON с сохранением чисел и внутренних ссылок."""
    return json.loads(path.read_text(encoding='utf-8'))


def part_path(path, relative):
    """Проверяет, что часть журнала размещена рядом с главным файлом."""
    base = path.resolve().parent
    target = (base / relative).resolve()
    if Path(relative).is_absolute() or not target.is_relative_to(base):
        raise ValueError('Часть журнала должна находиться рядом с главным файлом')
    return target


def assemble(path, current):
    """Подставляет неизменяемую историю и индексы, затем проверяет уникальность ID."""
    data = current
    if 'parts' in current:
        parts = current['parts']
        data = {key: value for key, value in current.items() if key != 'parts'}
        evidence = read_json(part_path(path, parts['evidence']))
        if data.keys() & evidence.keys():
            raise ValueError('Поля текущего журнала перекрывают поля хранилища доказательств')
        data.update(evidence)
        history = read_json(part_path(path, parts['chronology']))
        data['chronology'] = history + current['chronology']
    ids = [event['id'] for event in data['chronology']]
    if len(ids) != len(set(ids)):
        raise ValueError('В журнале повторяются id')
    return data


def read(path):
    """Загружает полный журнал из одного файла или из набора связанных частей."""
    return assemble(path, read_json(path))


def export(path, output):
    """Записывает полную копию журнала, сохраняя порядок событий и внутренние ссылки."""
    current = read_json(path)
    sources = [path.resolve()]
    if 'parts' in current:
        sources.extend(part_path(path, current['parts'][key]) for key in ('chronology', 'evidence'))
    target = output.resolve()
    if target in sources or (target.exists() and any(target.samefile(source) for source in sources)):
        raise ValueError('Файл экспорта совпадает с исходным журналом или его частью')

    data = assemble(path, current)
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix='.bitvyr-export-', suffix='.tmp', dir=target.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(data, stream, ensure_ascii=False, separators=(',', ':'), allow_nan=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        size = temporary.stat().st_size
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return {'status': 'EXPORTED', 'path': str(output), 'events': len(data['chronology']), 'bytes': size}


def expand(value, pool, stack=()):
    """Разворачивает только внутренние $shared; циклическая ссылка считается ошибкой."""
    if isinstance(value, dict):
        if set(value) == {'$shared'}:
            key = value['$shared']
            if key in stack:
                raise ValueError('Циклический $shared')
            return expand(pool[key], pool, (*stack, key))
        return {key: expand(child, pool, stack) for key, child in value.items()}
    if isinstance(value, list):
        return [expand(child, pool, stack) for child in value]
    return value



def lookup_payload(data, digest):
    """Читает сохранённый payload по essential либо исходному artifact SHA без открытия старого build.

    Приоритет сохраняется за полными проекциями, для новых записей проверяется
    их хэш. Если исторический индекс хранит только сводку или ссылку на событие,
    результат содержит сохранённую запись индекса и связанное событие.
    """
    pool = data['essential_payloads']
    if digest in pool:
        return expand(pool[digest], data['shared_essential_subtrees'])
    indexed = None
    for name in ('artifact_index', 'text_result_index'):
        item = data.get(name, {}).get(digest)
        if item is None:
            continue
        if 'essential_payload_sha256' in item:
            key = item['essential_payload_sha256']
            return expand(pool[key], data['shared_essential_subtrees'])
        if indexed is None:
            indexed = item
    for event in reversed(data['chronology']):
        result = event.get('result')
        archive = result.get('durable_build_evidence') if isinstance(result, dict) else None
        if not isinstance(archive, dict):
            continue
        item = archive.get('artifacts', {}).get(digest)
        if item is None:
            continue
        key = item['payload_sha256']
        payload = archive['payloads'][key]
        actual = hashlib.sha256(json.dumps(payload, ensure_ascii=False, sort_keys=True,
                                          separators=(',', ':'), allow_nan=False).encode('utf-8')).hexdigest()
        if actual != key:
            raise ValueError('Не совпал хэш сохранённой проекции artifact')
        return payload
    if indexed is None:
        raise KeyError(digest)
    projection = {'source_sha256': digest, 'artifact_index_entry': indexed}
    event_id = indexed.get('already_inline_managed_event_id')
    if event_id is not None:
        event = next((row for row in data['chronology'] if row.get('id') == event_id), None)
        if event is None:
            raise ValueError(f'В журнале отсутствует связанное событие {event_id}')
        projection['event'] = event
    return projection

def append(path, event_path):
    """Атомарно добавляет один опыт под эксклюзивным lock; конфликт id не перезаписывает историю."""
    event = json.loads(event_path.read_text(encoding='utf-8-sig'))
    required = {'id', 'date', 'title', 'status', 'result', 'decision'}
    if not required <= event.keys():
        raise ValueError('Нужны поля: ' + ', '.join(sorted(required)))
    datetime.date.fromisoformat(event['date'])
    lock = path.with_name(path.name + '.lock')
    descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    temporary = path.with_name(path.name + '.tmp')
    try:
        os.close(descriptor)
        current = read_json(path)
        history = assemble(path, current)['chronology']
        existing = next((item for item in history if item['id'] == event['id']), None)
        if existing is not None:
            if existing != event:
                raise ValueError('id уже содержит другой результат; добавьте новый опыт')
            return {'status': 'UNCHANGED', 'id': event['id']}
        previous_sha = hashlib.sha256(path.read_bytes()).hexdigest()
        current['chronology'].append(event)
        current['updated_at_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        with temporary.open('w', encoding='utf-8', newline='\n') as stream:
            json.dump(current, stream, ensure_ascii=False, separators=(',', ':'))
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        return {'status': 'APPENDED', 'id': event['id'], 'previous_sha256': previous_sha,
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    finally:
        if temporary.exists():
            temporary.unlink()
        lock.unlink()


def main():
    """Читает, экспортирует и дополняет журнал без обращения к старым сборкам."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path, default=Path(__file__).resolve().parents[1] / 'experiments.json')
    select = parser.add_mutually_exclusive_group()
    select.add_argument('--id')
    select.add_argument('--sha')
    select.add_argument('--event', type=Path)
    select.add_argument('--export', type=Path, metavar='FILE', help='Сохранить полный журнал одним JSON')
    parser.add_argument('--date')
    parser.add_argument('--limit', type=int, default=20)
    args = parser.parse_args()
    if args.event:
        result = append(args.file, args.event)
    elif args.export:
        result = export(args.file, args.export)
    else:
        data = read(args.file)
        if args.sha:
            result = lookup_payload(data, args.sha)
        elif args.id:
            result = next(event for event in data['chronology'] if event['id'] == args.id)
        else:
            events = [event for event in data['chronology'] if not args.date or event['date'] == args.date]
            result = [{key: event.get(key) for key in ('id', 'date', 'title', 'status')}
                      for event in events[-max(1, args.limit):]]
    json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
    print()


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    main()
