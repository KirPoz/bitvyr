"""Ограниченный построчный разбор фактической UART-телеметрии NES.

``parse_line(str)`` возвращает ``{kind, fields, raw_type}`` либо None для
неизвестного сообщения. Если есть ESP-префикс, добавляется фактический
``device_timestamp_ms``; это время логгера устройства, не время хоста и не
длительность кадра. Узнаваемая повреждённая запись имеет kind=parse_error и
fields.error/detail. Исходные bytes/строки сохраняет вызывающий сборщик.

Числа KV переводятся в int/конечный float, true/false — в bool; числовые 0/1
остаются int. CRC, ids и хеши сохраняются строками с ведущими нулями. JSON
сохраняет свои типы. Парсер не связывает поколения, не восстанавливает паузы,
не вычисляет FPS/позиции CPU и не считает unavailable read_hash проверкой шины.
Автоматический Output48c выдаёт TIMES+LOOP, а не NES_PERF. FRAME_AUDIO_STATUS
выводится по status, не является периодическим аудиомонитором.

Лимиты одной строки: 16384 UTF-8 bytes, 256 полей/элементов, глубина JSON 8.
Нет состояния, I/O, eval, pyserial или зависимостей вне stdlib и общего ANSI
шаблона tools.log_text. Поддержанные теги перечислены явно; новые неизвестны.
"""

import json
import math
import re

if __package__:
    from .log_text import ANSI
else:
    from log_text import ANSI


MAX_LINE_BYTES = 16384
MAX_FIELDS = 256
MAX_JSON_DEPTH = 8
_U64_MAX = (1 << 64) - 1
_I64_MIN = -(1 << 63)
_ESP = re.compile(r'^[IWEVD] \(\s*([0-9]{1,20})\) [^:\r\n]{1,96}:\s*')
_TAG = re.compile(r'^(NES_[A-Z0-9_]+)(?=\s|$|[\[{])')
_KEY = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=')
_NEXT_KEY = re.compile(r'\s+(?=[A-Za-z_][A-Za-z0-9_]*=)')
_DECIMAL = re.compile(r'[+-]?[0-9]+\Z')
_HEX = re.compile(r'0[xX][0-9a-fA-F]+\Z')
_FLOAT = re.compile(r'[+-]?(?:[0-9]+\.[0-9]*|[0-9]*\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z')
_GAME_START = re.compile(
    r'index=([0-9]+) name=(.*) mapper=([0-9]+) region=(NTSC|PAL) '
    r'crc=([0-9a-fA-F]{8}) source=(\S+) zip=([01]) size=([0-9]+)\Z')

_JSON_TAGS = frozenset(('NES_' + value) for value in '''
STATUS ROM CATALOG SETTINGS UI_STATUS OUTPUT_FRAME_PIPELINE_STATUS
FRAME_PIPELINE_BENCH_STATUS FRAME_PIPELINE_PRESENT_STATUS FRAME_AUDIO_STATUS
CPU_BENCH_STATUS COUPLED_BENCH_STATUS
'''.split())
_KV_TAGS = frozenset(('NES_' + value) for value in '''
OUTPUT_FRAME_PIPELINE_TIMES FRAME_LOOP_METRICS PERF RUNTIME_READY RUNTIME_FAIL
OUTPUT_FRAME_SETUP OUTPUT_FRAME_PIPELINE_MODE EXECUTION AUDIO_EXECUTION
VIDEO_EXECUTION VIDEO_RESOURCES VIDEO_SPLIT_FAIL AUDIO_SPLIT_FAIL LIBRARY
STATUS_ERROR COMMAND_ERROR FRAME_PIPELINE_BENCH FRAME_PIPELINE_BENCH_MODE
FRAME_PIPELINE_BENCH_ERROR CPU_BENCH CPU_BENCH_MODE CPU_BENCH_ERROR
COUPLED_BENCH COUPLED_BENCH_MODE COUPLED_BENCH_ERROR COUPLED_SETUP
INPUT PAD_APPLY PAD_SCHEDULE PAD_ERROR AUDIO VIDEO HEAP TASKS TASK PROFILE
'''.split())

_REQUIRED = {
    'NES_OUTPUT_FRAME_PIPELINE_TIMES': frozenset('''game frames produced_frames
        submitted_frames samples generation mapper region core_us producer_wall_us
        seal_us audio_retire_us snapshot_us consumer_active_us dsp_us witness_copy_us
        rgb_copy_us present_us wait_us audio_us slots schema producer consumer
        read_witness effective_tail game_fps_claim'''.split()),
    'NES_FRAME_LOOP_METRICS': frozenset('''generation accepted_first accepted_last
        completed_main_iterations submitted_before submitted_after present_started_before
        present_started_after pending_after pending_generation_after pending_frame_after
        dispatched_input_first dispatched_input_last accepted_input_first accepted_input_last
        accepted_input_service_us loop_wall_us outer_join_us cli_menu_us pace_us input_current_us
        dispatch_us consume_present_us report_us end_yield_us remaining_us loop_wall_max_us
        loop_wall_max_accepted_frame outer_join_max_us cli_menu_max_us pace_max_us
        input_current_max_us dispatch_max_us consume_present_max_us report_max_us end_yield_max_us
        remaining_max_us stage_maxima_sum_valid main_core unit game_fps_claim
        producer_time_added separate_ppa_elapsed_added'''.split()),
    'NES_FRAME_AUDIO_STATUS': frozenset('''generation accepted_frame playing muted volume active
        accepted_frames dma_frames missed_refill_blocks missed_refill_frames max_write_gap_us
        max_write_wait_us source game_fps_claim'''.split()),
    'NES_OUTPUT_FRAME_PIPELINE_STATUS': frozenset(
        'playing frames generation owner image_available game_fps_claim'.split()),
    'NES_RUNTIME_READY': frozenset('games psram lcd touch audio sd'.split()),
    'NES_PERF': frozenset('game frames fps core_us render_us'.split()),
}
_JSON_BOOLS = frozenset('''playing muted active pending paused dirty swap_ab show_fps
    image_available input_valid game_fps_claim lcd touch audio sd psram xbox
    video fps_enabled fps_valid'''.split())
_INTEGER_KEYS = frozenset('''index game games mapper frames generation samples schema schema_version
    accepted_first accepted_last submitted_before submitted_after present_started_before
    present_started_after dispatched_input_first dispatched_input_last accepted_input_first
    accepted_input_last completed_main_iterations core main_core renderer_core priority slots
    pc a x y sp p volume hardware_volume size count skipped contacts mask queued serial
    xbox_raw xbox_effective touch applied pending_generation_after pending_after
    poll_ok suppressed zip borrowed copy early_dsp game_fps_claim producer_time_added
    separate_ppa_elapsed_added stage_maxima_sum_valid pending_explicit_output_bind core_diag'''.split())
_TEXT_KEYS = frozenset('name title root error reason ids crc crc32'.split())


class _ParseFailure(ValueError):
    """Переносит ограниченный код ошибки до единственной границы parse_line."""

    def __init__(self, code, detail):
        """Сохраняет машинный код и короткое пояснение без копии всей RAW-строки."""
        super().__init__(detail)
        self.code, self.detail = code, detail


def _integer(value):
    """Разбирает не более 20 десятичных цифр в диапазоне FW int64/uint64."""
    if len(value.lstrip('+-')) > 20:
        raise _ParseFailure('invalid_number', 'integer exceeds 64-bit telemetry range')
    result = int(value, 10)
    if not _I64_MIN <= result <= _U64_MAX:
        raise _ParseFailure('invalid_number', 'integer exceeds 64-bit telemetry range')
    return result


def _unquote(value):
    """Снимает парные кавычки KV; экранирует лишь кавычку и обратную черту.

    Не интерпретирует \u005cn/\u005ct и прочие последовательности Windows-путей как
    управляющие символы. Апостроф внутри некавыченного имени остаётся буквальным.
    """
    quote = value[0]
    if len(value) < 2 or value[-1] != quote:
        raise _ParseFailure('invalid_kv', 'unterminated quoted value')
    result = []
    pos = 1
    while pos < len(value) - 1:
        char = value[pos]
        if char == '\\' and pos + 1 < len(value) - 1 and value[pos + 1] in (quote, '\\'):
            pos += 1
            char = value[pos]
        elif char == quote:
            raise _ParseFailure('invalid_kv', 'unexpected quote inside quoted value')
        result.append(char)
        pos += 1
    return ''.join(result)


def _value(key, value):
    """Переводит wire-числа, сохраняя текстовые identity и кавыченные строки."""
    if value.startswith(('"', "'")):
        return _unquote(value)
    if key in _TEXT_KEYS or key.endswith(('_hash', '_sha', '_sha256')):
        return value
    if value in ('true', 'false'):
        return value == 'true'
    if _DECIMAL.fullmatch(value):
        return _integer(value)
    if _HEX.fullmatch(value):
        if len(value) > 18:
            raise _ParseFailure('invalid_number', 'hex integer exceeds uint64')
        return int(value, 16)
    if _FLOAT.fullmatch(value):
        result = float(value)
        if not math.isfinite(result):
            raise _ParseFailure('invalid_number', 'nonfinite float')
        return result
    return value


def _kv(body):
    """Читает ограниченные KV без shlex, сохраняя пробелы в текстовых значениях."""
    fields = {}
    pos = 0
    while pos < len(body):
        match = _KEY.match(body, pos)
        if not match:
            raise _ParseFailure('invalid_kv', 'expected key=value')
        key = match[1]
        if key in fields:
            raise _ParseFailure('duplicate_field', key)
        if len(fields) == MAX_FIELDS:
            raise _ParseFailure('field_limit', 'too many fields')
        pos = match.end()
        start = pos
        if pos < len(body) and body[pos] in ('"', "'"):
            quote = body[pos]
            pos += 1
            while pos < len(body):
                if body[pos] == '\\' and pos + 1 < len(body) and body[pos + 1] in (quote, '\\'):
                    pos += 2
                elif body[pos] == quote:
                    pos += 1
                    break
                else:
                    pos += 1
            else:
                raise _ParseFailure('invalid_kv', 'unterminated quoted value')
            end = pos
            if pos < len(body) and not body[pos].isspace():
                raise _ParseFailure('invalid_kv', 'characters after quoted value')
        else:
            next_key = _NEXT_KEY.search(body, pos)
            end = next_key.start() if next_key else len(body)
            pos = end
        fields[key] = _value(key, body[start:end].rstrip())
        while pos < len(body) and body[pos].isspace():
            pos += 1
    return fields


def _pairs(pairs):
    """Запрещает неявное затирание дублированного JSON-поля."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise _ParseFailure('duplicate_field', key[:64])
        result[key] = value
    return result


def _constant(_value_text):
    """Отказывает нестандартным JSON NaN/Infinity вместо нечисловой статистики."""
    raise _ParseFailure('invalid_number', 'nonfinite JSON constant')


def _json(body):
    """Разбирает только JSON-object и проверяет глубину/общий размер дерева."""
    try:
        result = json.loads(body, object_pairs_hook=_pairs, parse_int=_integer,
                            parse_constant=_constant)
    except (json.JSONDecodeError, RecursionError) as error:
        raise _ParseFailure('invalid_json', 'malformed or excessively nested JSON') from error
    if not isinstance(result, dict):
        raise _ParseFailure('invalid_json', 'expected JSON object')
    pending = [(result, 0)]
    count = 0
    while pending:
        value, depth = pending.pop()
        if depth > MAX_JSON_DEPTH:
            raise _ParseFailure('nested_limit', 'JSON depth exceeds limit')
        if isinstance(value, (dict, list)):
            count += len(value)
            if count > MAX_FIELDS:
                raise _ParseFailure('field_limit', 'too many JSON fields/elements')
            children = value.values() if isinstance(value, dict) else value
            pending.extend((child, depth + 1) for child in children)
        elif isinstance(value, float) and not math.isfinite(value):
            raise _ParseFailure('invalid_number', 'nonfinite JSON float')
        elif isinstance(value, str):
            try:
                value.encode('utf-8')
            except UnicodeError as error:
                raise _ParseFailure('invalid_unicode', 'unpaired JSON surrogate') from error
    return result


def _validate(tag, fields, is_json):
    """Проверяет типы известных FW-полей, не выводя межкадровые инварианты."""
    missing = _REQUIRED.get(tag, frozenset()) - fields.keys()
    if missing:
        raise _ParseFailure('missing_field', ','.join(sorted(missing))[:192])
    for key, value in fields.items():
        if is_json and key in _JSON_BOOLS:
            if type(value) is not bool:
                raise _ParseFailure('invalid_boolean', key)
            continue
        numeric = key in _INTEGER_KEYS or key.endswith(('_us', '_frames', '_frame', '_cycles', '_bytes'))
        if numeric and type(value) is not int:
            raise _ParseFailure('invalid_number', key + ' must be integer')
        if numeric and not key.endswith('_us') and value < 0:
            raise _ParseFailure('invalid_number', key + ' must be nonnegative')
        if key == 'fps' and (type(value) not in (int, float) or not math.isfinite(value)):
            raise _ParseFailure('invalid_number', 'fps must be finite numeric wire value')


def parse_line(text):
    """Разбирает одну caller-owned строку без истории, записи файлов и side effects.

    None означает неизвестный тег/обычный лог. parse_error означает узнаваемую
    испорченную запись либо превышенный общий лимит; RAW не ремонтируется.
    device_timestamp_ms добавляется только из ESP prefix. JSON требует настоящие
    boolean, KV сохраняет 0/1. Наличие записи не доказывает качество/темп игры.
    """
    tag = 'UNKNOWN'
    stamp = None
    try:
        if not isinstance(text, str):
            raise _ParseFailure('input_type', 'expected str')
        if len(text) > MAX_LINE_BYTES or len(text.encode('utf-8')) > MAX_LINE_BYTES:
            raise _ParseFailure('line_too_long', 'line exceeds 16384 UTF-8 bytes')
        line = ANSI.sub('', text).strip()
        prefix = _ESP.match(line)
        if prefix:
            stamp = _integer(prefix[1])
            line = line[prefix.end():]
        match = _TAG.match(line)
        if not match or match[1] not in _JSON_TAGS | _KV_TAGS | {'NES_GAME_START'}:
            return None
        tag = match[1]
        body = line[match.end():].strip()
        if '\n' in body or '\r' in body or '\x00' in body:
            raise _ParseFailure('invalid_value', 'embedded newline/NUL')
        is_json = tag in _JSON_TAGS
        if tag == 'NES_GAME_START':
            game = _GAME_START.fullmatch(body)
            if not game:
                raise _ParseFailure('invalid_kv', 'GAME_START fixed metadata suffix missing')
            fields = {key: value if key == 'name' else _value(key, value) for key, value in zip(
                ('index', 'name', 'mapper', 'region', 'crc', 'source', 'zip', 'size'), game.groups())}
        else:
            fields = _json(body) if is_json else _kv(body)
        if not fields:
            raise _ParseFailure('missing_field', 'empty record')
        _validate(tag, fields, is_json)
        record = {'kind': tag[4:].lower(), 'fields': fields, 'raw_type': tag}
    except UnicodeError:
        record = {'kind': 'parse_error', 'fields': {'error': 'invalid_unicode',
                  'detail': 'input has unpaired surrogate'}, 'raw_type': tag}
    except _ParseFailure as error:
        record = {'kind': 'parse_error', 'fields': {'error': error.code,
                  'detail': error.detail}, 'raw_type': tag}
    if stamp is not None:
        record['device_timestamp_ms'] = stamp
    return record
