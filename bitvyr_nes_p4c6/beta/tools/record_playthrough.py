"""Записывает UART-прохождение на ПК без reset и автоматических запросов к ядру."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
from itertools import islice
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

try:
    from playthrough_metrics import MAX_LINE_BYTES, parse_line
except ModuleNotFoundError:
    from tools.playthrough_metrics import MAX_LINE_BYTES, parse_line

ROOT = Path(__file__).resolve().parents[1]
RESERVE = 8192


def utc_now():
    """Возвращает UTC ISO8601; длительности измеряются отдельно монотонными часами."""
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds')


def json_bytes(value):
    """Кодирует одну JSON-запись с настоящим LF, сохраняя русские метки."""
    return (json.dumps(value, ensure_ascii=False, separators=(',', ':'))+'\n').encode('utf-8')


def atomic_json(path, value):
    """Атомарно обновляет свой status/ACK; пережидает короткий Windows reader-lock."""
    temporary = path.with_suffix(path.suffix+'.tmp')
    temporary.write_bytes(json_bytes(value))
    for attempt in range(8):
        try:
            os.replace(temporary, path)
            return
        except PermissionError as error:
            # Windows reader без FILE_SHARE_DELETE временно запрещает rename.
            # Постоянный отказ остаётся ошибкой; общий предел ожидания — 390 мс.
            if getattr(error, 'winerror', None) not in (5, 32, 33) or attempt == 7:
                raise
            time.sleep(.01 * (2 ** min(attempt, 3)))


class BudgetReached(RuntimeError):
    """Означает достижение общего лимита RAW+events; старые данные не удаляются."""


class LineFramer:
    """Выделяет строки, удерживая не более max_line байтов даже без перевода строки."""
    def __init__(self, max_line=MAX_LINE_BYTES):
        """Начинает новую нумерацию UART-строк и пустой ограниченный буфер."""
        self.maximum = max_line
        self.pending = bytearray()
        self.discarding = False
        self.line = 0

    def feed(self, data):
        """Возвращает пары номер/байты; None обозначает слишком длинную исходную строку."""
        result = []
        for piece_index, piece in enumerate(data.split(b'\n')):
            if piece_index:
                self.line += 1
                result.append((self.line, None if self.discarding else bytes(self.pending).rstrip(b'\r')))
                self.pending.clear()
                self.discarding = False
            if not self.discarding:
                if len(self.pending)+len(piece) > self.maximum:
                    self.pending.clear()
                    self.discarding = True
                else:
                    self.pending.extend(piece)
        return result


class SessionWriter:
    """Хранит точный RAW и нормализованные события в новых ограниченных файлах."""
    def __init__(self, directory, maximum):
        """Открывает RAW/events исключительно; общий лимит включает оба потока."""
        self.directory = directory
        self.maximum = maximum
        self.raw = (directory/'raw.serial.log').open('xb')
        try:
            self.events = (directory/'events.jsonl').open('xb')
        except BaseException:
            self.raw.close()
            raise
        self.raw_bytes = self.event_bytes = self.event_count = 0

    def write_raw(self, data):
        """Сохраняет байты до разбора; отказ по лимиту не стирает предыдущий RAW."""
        if self.raw_bytes+self.event_bytes+len(data) > self.maximum-RESERVE:
            raise BudgetReached('raw_and_events_byte_limit')
        self.raw.write(data)
        self.raw_bytes += len(data)

    def emit(self, record, serial_line=None, *, final=False):
        """Пишет событие с host UTC/monotonic и исходным номером UART-строки."""
        envelope = {'schema': 1, 'host_utc': utc_now(), 'host_monotonic_ns': time.monotonic_ns(),
                    'serial_line': serial_line, 'record': record}
        data = json_bytes(envelope)
        limit = self.maximum if final else self.maximum-RESERVE
        if self.raw_bytes+self.event_bytes+len(data) > limit:
            raise BudgetReached('raw_and_events_byte_limit')
        self.events.write(data)
        self.event_bytes += len(data)
        self.event_count += 1

    def flush(self):
        """Передаёт накопленные данные ОС без принудительного fsync на каждой строке."""
        self.raw.flush()
        self.events.flush()

    def close(self):
        """Закрывает оба файла; окончательная синхронизация выполняется один раз."""
        first_error = None
        for stream in (self.raw, self.events):
            if not stream.closed:
                try:
                    stream.flush()
                    os.fsync(stream.fileno())
                except OSError as error:
                    first_error = first_error or error
                finally:
                    try:
                        stream.close()
                    except OSError as error:
                        first_error = first_error or error
        if first_error:
            raise first_error


def select_adapter(port, vid, pid, serial_number):
    """Находит ровно один явно идентифицированный USB–UART, а не первый COM-порт."""
    from serial.tools import list_ports
    candidates = [p for p in list_ports.comports()
                  if (p.vid, p.pid) == (vid, pid)
                  and (serial_number is None or p.serial_number == serial_number)
                  and (port == 'auto' or p.device.casefold() == port.casefold())]
    if len(candidates) != 1:
        raise RuntimeError('Не найден единственный USB–UART с заданными VID/PID/serial; проверь подключение')
    adapter = candidates[0]
    return {'port': adapter.device, 'vid': adapter.vid, 'pid': adapter.pid, 'serial': adapter.serial_number}


def open_serial(adapter, baud):
    """Открывает единственного владельца COM с DTR/RTS false; reset/boot-команд нет."""
    import serial
    stream = serial.Serial(port=None, baudrate=baud, timeout=.1, write_timeout=1)
    stream.dtr = stream.rts = False
    stream.port = adapter['port']
    stream.open()
    return stream


def extra_record(line):
    """Распознаёт только boot/ELF-маркеры вне NES-протокола; остальное остаётся в RAW."""
    if line.startswith('ESP-ROM:'):
        return {'kind': 'boot', 'raw_type': 'ESP_ROM_BOOT', 'fields': {'banner': line[:256]}}
    match = re.search(r'ELF file SHA256:\s*([0-9a-fA-F]{8,64})', line)
    if match:
        return {'kind': 'firmware', 'raw_type': 'ESP_ELF_SHA256',
                'fields': {'elf_sha256_prefix': match[1].lower(), 'full_sha256_verified': len(match[1]) == 64}}
    if any(value in line for value in ('Guru Meditation', 'assert failed', 'task_wdt:', 'abort()')):
        return {'kind':'fault','raw_type':'ESP_RUNTIME_FAULT','fields':{'text':line[:512]}}
    return None


def handle_requests(directory, sink, serial_stream):
    """Обрабатывает до16 host-запросов; UART пишет лишь явно запрошенный snapshot."""
    stop = False
    for request_path in islice((directory/'requests').glob('*.json'),16):
        identifier = request_path.stem
        if not re.fullmatch(r'[0-9a-f]{32}', identifier):
            continue
        acknowledgement = {'id': identifier, 'processed_utc': utc_now()}
        try:
            if request_path.stat().st_size > 4096:
                raise ValueError('request_too_large')
            request = json.loads(request_path.read_text(encoding='utf-8'))
            if not isinstance(request,dict):
                raise ValueError('request_must_be_object')
            action = request.get('action')
            if action == 'mark':
                label, phase = request.get('text'), request.get('phase', 'note')
                if not isinstance(label, str) or not 1 <= len(label) <= 512 or phase not in ('note','pause','resume','dip'):
                    raise ValueError('invalid_marker')
                sink.emit({'kind':'marker','raw_type':'HOST_MARKER',
                           'fields':{'text':label,'phase':phase,'request_id':identifier}})
            elif action == 'snapshot':
                if serial_stream is None:
                    raise ValueError('snapshot_requires_live_serial')
                # Эти команды выполняют retire и искажают соседние окна. Никакого polling по умолчанию.
                sink.emit({'kind':'observer_begin','raw_type':'HOST_RESOURCE_SNAPSHOT',
                           'fields':{'request_id':identifier,'commands':['tasks','status'],
                                     'performance_window_tainted':True}})
                for command in (b'tasks\n', b'status\n'):
                    if serial_stream.write(command) != len(command):
                        raise OSError('partial_snapshot_write')
                sink.emit({'kind':'observer_end','raw_type':'HOST_RESOURCE_SNAPSHOT',
                           'fields':{'request_id':identifier,'scope':'commands_sent_not_device_completion',
                                     'performance_window_tainted':True}})
            elif action == 'stop':
                stop = True
            else:
                raise ValueError('unknown_action')
            acknowledgement['status'] = 'accepted'
        except (ValueError, OSError, UnicodeError) as error:
            acknowledgement.update(status='rejected', error=str(error))
        atomic_json(directory/'acks'/request_path.name, acknowledgement)
        os.replace(request_path, directory/'handled'/request_path.name)
        if stop:
            break
    return stop


def capture(args):
    """Записывает до stop/EOF/лимита; в обычном режиме не посылает ни одной команды плате."""
    adapter = None if args.replay else select_adapter(args.port,args.vid,args.pid,args.adapter_serial)
    directory = args.session.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    for child in ('requests','handled','acks'):
        (directory/child).mkdir()
    started = time.monotonic()
    files = [Path(__file__), ROOT/'tools/playthrough_metrics.py', ROOT/'tools/analyze_playthrough.py',
             ROOT/'tools/log_text.py']
    metadata = {'schema':1,'id':directory.name,'created_utc':utc_now(),'pid':os.getpid(),
                'transport':'replay' if args.replay else 'usb_uart','adapter':adapter,'baud':args.baud,
                'max_total_bytes':int(args.max_mib*1024*1024),'max_seconds':args.seconds,
                'default_passive':True,'reset_requested':False,'dtr':False,'rts':False,
                'expected_elf':args.expected_elf,'expected_elf_is_observed':False,
                'replay_source':str(args.replay.resolve()) if args.replay else None,
                'tools':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
                'limits':['120-frame report averages, not physical VSYNC/per-frame trace',
                          'No automatic pause/level detection; manual markers',
                          'Explicit resource snapshots taint performance windows']}
    (directory/'session.json').write_bytes(json_bytes(metadata))
    sink = SessionWriter(directory, metadata['max_total_bytes'])
    framer = LineFramer()
    stream = None
    state = {'schema':1,'pid':os.getpid(),'phase':'starting','started_utc':metadata['created_utc'],
             'session':str(directory),'adapter':adapter,'last_record':None,'parse_errors':0}
    atomic_json(directory/'status.json',state)
    reason, exit_code = 'stop_requested', 0
    last_flush = 0.0
    try:
        stream = args.replay.open('rb') if args.replay else open_serial(adapter,args.baud)
        sink.emit({'kind':'capture_start','raw_type':'HOST_CAPTURE',
                   'fields':{'passive':True,'replay':bool(args.replay),'pid':os.getpid()}})
        state['phase'] = 'recording'
        print(json.dumps({'status':'RECORDING','session':str(directory),'adapter':adapter},ensure_ascii=False),flush=True)
        while True:
            if handle_requests(directory,sink,None if args.replay else stream):
                break
            if args.seconds and time.monotonic()-started >= args.seconds:
                reason = 'duration_limit'
                break
            data = stream.read(4096)
            if not data and args.replay:
                reason = 'replay_eof'
                break
            if data:
                sink.write_raw(data)
                for number, raw_line in framer.feed(data):
                    if raw_line is None:
                        sink.emit({'kind':'parse_error','raw_type':'UART_LINE',
                                   'fields':{'error':'line_too_long','maximum_bytes':MAX_LINE_BYTES}},number)
                        state['parse_errors'] += 1
                        continue
                    line = raw_line.decode('utf-8',errors='replace')
                    record = parse_line(line) or extra_record(line)
                    if record:
                        sink.emit(record,number)
                        if record['kind']=='firmware' and args.expected_elf:
                            prefix = record['fields'].get('elf_sha256_prefix','')
                            if prefix:
                                state['firmware_expected_prefix_match'] = args.expected_elf.startswith(prefix)
                                state['firmware_observed_prefix'] = prefix
                        if record['kind'] == 'parse_error':
                            state['parse_errors'] += 1
                        if record['kind']=='game_start':
                            state['last_game_start'] = record['fields']
                        if record['kind']=='output_frame_pipeline_times':
                            state['last_pipeline_window'] = record
                        if record['kind']=='heap':
                            state['last_heap_snapshot'] = record['fields']
                        if record['kind'] in ('game_start','output_frame_pipeline_times','frame_loop_metrics','firmware'):
                            state['last_record'] = record
            if time.monotonic()-last_flush >= 1:
                sink.flush()
                state.update(updated_utc=utc_now(),serial_lines=framer.line,raw_bytes=sink.raw_bytes,
                             event_bytes=sink.event_bytes,event_count=sink.event_count,
                             elapsed_seconds=round(time.monotonic()-started,3))
                atomic_json(directory/'status.json',state)
                last_flush = time.monotonic()
    except KeyboardInterrupt:
        reason = 'keyboard_interrupt'
    except BudgetReached:
        reason, exit_code = 'byte_limit', 2
    except Exception as error:
        reason, exit_code = 'capture_error', 1
        state['error'] = f'{type(error).__name__}: {error}'
    finally:
        cleanup_errors = []
        if stream is not None:
            try:
                stream.close()
            except Exception as error:
                cleanup_errors.append(f'transport_close: {type(error).__name__}: {error}')
                state['transport_close_failed'] = True
                reason, exit_code = 'capture_cleanup_error', 1
        if framer.pending or framer.discarding:
            state['trailing_incomplete_line'] = True
        try:
            sink.emit({'kind':'capture_end','raw_type':'HOST_CAPTURE',
                       'fields':{'reason':reason,'exit_code':exit_code,
                                 'trailing_incomplete_line':bool(framer.pending or framer.discarding)}},final=True)
        except (OSError,BudgetReached) as error:
            cleanup_errors.append(f'final_event: {type(error).__name__}: {error}')
        finally:
            try:
                sink.close()
            except OSError as error:
                cleanup_errors.append(f'file_close: {type(error).__name__}: {error}')
        if cleanup_errors:
            reason, exit_code = 'capture_cleanup_error', 1
            state['cleanup_errors'] = cleanup_errors
        state.update(phase='closed',reason=reason,exit_code=exit_code,ended_utc=utc_now(),updated_utc=utc_now(),
                     serial_lines=framer.line,raw_bytes=sink.raw_bytes,event_bytes=sink.event_bytes,
                     event_count=sink.event_count,elapsed_seconds=round(time.monotonic()-started,3))
        atomic_json(directory/'status.json',state)
    if not args.no_report and not state.get('transport_close_failed'):
        command = [sys.executable,'-X','utf8',str(ROOT/'tools/analyze_playthrough.py'),str(directory),'--output',str(directory/'report')]
        try:
            with (directory/'analysis.log').open('xb') as log:
                result = subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=120,check=False)
            state['report_exit_code'] = result.returncode
        except (OSError,subprocess.TimeoutExpired) as error:
            state['report_exit_code'] = 3
            state['report_error'] = f'{type(error).__name__}: {error}'
        atomic_json(directory/'status.json',state)
        if state['report_exit_code']:
            exit_code = exit_code or 3
    print(json.dumps({'status':'CLOSED','session':str(directory),'reason':reason,'exit_code':exit_code},ensure_ascii=False))
    return exit_code


def request(args):
    """Создаёт host-запрос; только capture-процесс владеет портом и принимает решение."""
    directory = args.session.resolve()
    state = json.loads((directory/'status.json').read_text(encoding='utf-8'))
    if state.get('phase') != 'recording':
        raise RuntimeError('Capture не находится в состоянии recording')
    age = time.time()-datetime.fromisoformat(state.get('updated_utc',state['started_utc'])).timestamp()
    if age > 10:
        raise RuntimeError('Heartbeat capture старше10 секунд; запрос не отправлен')
    payload = {'action':args.action,'created_utc':utc_now()}
    if args.action == 'mark':
        if not 1 <= len(args.text) <= 512 or any(ord(c)<32 for c in args.text):
            raise ValueError('Метка должна содержать1..512 печатных символов')
        payload.update(text=args.text,phase=args.phase)
    identifier = uuid.uuid4().hex
    request_path = directory/'requests'/f'{identifier}.json'
    temporary = request_path.with_suffix('.tmp')
    with temporary.open('xb') as stream:
        stream.write(json_bytes(payload))
    os.rename(temporary,request_path)
    end = time.monotonic()+args.wait
    acknowledgement = directory/'acks'/request_path.name
    while time.monotonic()<end:
        if acknowledgement.exists():
            response = json.loads(acknowledgement.read_text(encoding='utf-8'))
            print(json.dumps(response,ensure_ascii=False))
            return 0 if response['status']=='accepted' else 1
        time.sleep(.05)
    print(json.dumps({'status':'pending','id':identifier,'session':str(directory)},ensure_ascii=False))
    return 2


def parse_args(argv=None):
    """Проверяет CLI до открытия COM; для каждого прохождения нужен новый каталог."""
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action',required=True)
    start = sub.add_parser('capture',help='Новая пассивная UART-запись или воспроизведение старого RAW')
    start.add_argument('--session',type=Path,required=True)
    start.add_argument('--port',default='auto')
    start.add_argument('--vid',type=lambda v:int(v,0),default=0x1a86)
    start.add_argument('--pid',type=lambda v:int(v,0),default=0x55d3)
    start.add_argument('--adapter-serial')
    start.add_argument('--baud',type=int,default=115200)
    start.add_argument('--max-mib',type=float,default=256)
    start.add_argument('--seconds',type=float,default=0)
    start.add_argument('--expected-elf')
    start.add_argument('--replay',type=Path)
    start.add_argument('--no-report',action='store_true')
    for name in ('mark','snapshot','stop','status'):
        control = sub.add_parser(name)
        control.add_argument('session',type=Path)
        if name != 'status':
            control.add_argument('--wait',type=float,default=3)
        if name == 'mark':
            control.add_argument('--text',required=True)
            control.add_argument('--phase',choices=('note','pause','resume','dip'),default='note')
    args = parser.parse_args(argv)
    if args.action=='capture':
        if not math.isfinite(args.max_mib) or not 1<=args.max_mib<=4096:
            parser.error('--max-mib должен быть в1..4096')
        if not math.isfinite(args.seconds) or args.seconds<0:
            parser.error('--seconds должен быть конечным и неотрицательным')
        if args.baud<=0:
            parser.error('--baud должен быть положительным')
        if args.expected_elf and not re.fullmatch(r'[0-9a-f]{64}',args.expected_elf):
            parser.error('--expected-elf должен быть полным SHA256')
        if args.replay and not args.replay.is_file():
            parser.error('--replay должен указывать существующий RAW')
    elif args.action!='status' and (not math.isfinite(args.wait) or not 0<=args.wait<=30):
        parser.error('--wait должен быть в0..30 секунд')
    return args


def main(argv=None):
    """Диспетчеризует запись и её host-контроль; ошибки не завершают игру на плате."""
    args = parse_args(argv)
    if args.action=='capture':
        return capture(args)
    if args.action=='status':
        print((args.session/'status.json').read_text(encoding='utf-8'))
        return 0
    return request(args)


if __name__=='__main__':
    raise SystemExit(main())
