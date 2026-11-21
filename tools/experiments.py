"""Читает и дополняет единый журнал Bitvyr; прежние измерения не изменяются."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import sys


def read(path):
    """Читает автономный UTF-8 JSON и проверяет уникальность идентификаторов опыта."""
    data = json.loads(path.read_text(encoding='utf-8'))
    ids = [event['id'] for event in data['chronology']]
    if len(ids) != len(set(ids)):
        raise ValueError('В журнале повторяются id')
    return data


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

    Старые essential SHA сохраняют приоритет и прежнее разворачивание $shared.
    Новые cleanup-события содержат собственные payloads и provenance исходных
    файлов; хэш проекции проверяется до выдачи результата. Отсутствующий build
    не подменяется повторным чтением или молчаливой реконструкцией.
    """
    pool = data['essential_payloads']
    if digest in pool:
        return expand(pool[digest], data['shared_essential_subtrees'])
    for name in ('artifact_index', 'text_result_index'):
        item = data.get(name, {}).get(digest)
        if item is not None:
            key = item['essential_payload_sha256']
            return expand(pool[key], data['shared_essential_subtrees'])
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
    raise KeyError(digest)

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
        data = read(path)
        existing = next((item for item in data['chronology'] if item['id'] == event['id']), None)
        if existing is not None:
            if existing != event:
                raise ValueError('id уже содержит другой результат; добавьте новый опыт')
            return {'status': 'UNCHANGED', 'id': event['id']}
        previous_sha = hashlib.sha256(path.read_bytes()).hexdigest()
        data['chronology'].append(event)
        data['updated_at_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        with temporary.open('w', encoding='utf-8', newline='\n') as stream:
            json.dump(data, stream, ensure_ascii=False, separators=(',', ':'))
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
    """Показывает список/опыт/измерения либо добавляет подготовленный JSON без обращения к старым builds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path, default=Path(__file__).resolve().parents[1] / 'experiments.json')
    select = parser.add_mutually_exclusive_group()
    select.add_argument('--id')
    select.add_argument('--sha')
    select.add_argument('--event', type=Path)
    parser.add_argument('--date')
    parser.add_argument('--limit', type=int, default=20)
    args = parser.parse_args()
    if args.event:
        result = append(args.file, args.event)
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
