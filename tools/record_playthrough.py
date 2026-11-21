"""Пассивная запись прохождения через USB–UART. Команды в устройство не отправляет."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import re
import time

from playthrough_log import parse_line


def now():
    """Возвращает UTC receipt-time только для хронологии, никогда не для FPS."""
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def atomic_json(path,value):
    """Публикует целый heartbeat после записи соседнего временного файла."""
    temp=path.with_suffix('.tmp')
    temp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
    os.replace(temp,path)


def pin(path):
    """Фиксирует версию самого инструмента без чтения ROM/пользовательских ключей."""
    b=path.read_bytes()
    return dict(path=str(path.resolve()),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())


def request_stop(folder):
    """Просит владельца корректно закрыть raw и COM; процесс/плата не сбрасываются."""
    path=folder.resolve()
    if not (path/'state.json').is_file():raise ValueError('Not a capture directory')
    (path/'stop.request').write_text(now()+'\n',encoding='ascii')
    return dict(stop_requested=True,capture=str(path))


def record(args):
    """Читает поток до stop.request/ошибки. DTR/RTS выключены ещё до open.

    На диск сначала попадают неизменённые UART bytes, затем ограниченная строковая
    диагностика и heartbeat. Никакого write/reset_input_buffer/reset/flash здесь нет.
    """
    import serial
    from serial.tools import list_ports
    folder=args.output.resolve();folder.mkdir(parents=True,exist_ok=True)
    if any((folder/x).exists()for x in ('raw.log','state.json','stop.request')):
        raise ValueError('Capture directory is not empty; use a new capture path')
    ports=[p for p in list_ports.comports()if p.device.upper()==args.port.upper()]
    if len(ports)!=1:raise ValueError('Requested serial port is absent')
    p=ports[0]
    if (p.vid,p.pid)!=(args.vid,args.pid) or (args.serial and p.serial_number!=args.serial):
        raise ValueError('USB VID/PID/serial mismatch')
    state=dict(status='opening',pid=os.getpid(),started_utc=now(),updated_utc=now(),
               USB=dict(port=p.device,vid=p.vid,pid=p.pid,serial=p.serial_number),
               baud=args.baud,raw_bytes=0,complete_lines=0,parse_errors=0,
               oversized_partial_lines=0,firmware_faults=0,observed_boots=0,
               game_launches=0,latest_game=None,last_frame=None,last_rx_utc=None,
               commands_sent=0,reset_requested=False,flash=False,NVS_written=False,SD_written=False,
               firmware_identity='unknown_until_boot_ELF_is_observed',
               FPS_clock='device only; host receipts are chronology',
               tool=pin(Path(__file__)),parser=pin(Path(__file__).with_name('playthrough_log.py')))
    atomic_json(folder/'state.json',state)
    port=None;pending=b'';heartbeat=0;raw=None;events=None;chunks=None
    def event(kind,**kw):
        """Сохраняет событие ПК отдельно от исходных байтов UART."""
        value=dict(utc=now(),kind=kind,raw_offset=state['raw_bytes'],**kw)
        events.write(json.dumps(value,ensure_ascii=False)+'\n');events.flush()
    try:
        raw=(folder/'raw.log').open('xb');events=(folder/'events.jsonl').open('x',encoding='utf8');chunks=(folder/'chunks.jsonl').open('x',encoding='utf8')
        port=serial.Serial(port=None,baudrate=args.baud,timeout=.1,write_timeout=1)
        port.dtr=False;port.rts=False;port.port=p.device;port.open()
        state['status']='recording';state['COM_open']=True;atomic_json(folder/'state.json',state);event('opened',DTR=False,RTS=False,commands_sent=0)
        start=time.monotonic()
        while not(folder/'stop.request').exists():
            if args.max_seconds and time.monotonic()-start>=args.max_seconds:
                event('duration_limit');break
            block=port.read(32768)
            if block:
                begin=state['raw_bytes'];raw.write(block);raw.flush();state['raw_bytes']+=len(block);state['last_rx_utc']=now()
                chunks.write(json.dumps(dict(utc=state['last_rx_utc'],begin=begin,end=state['raw_bytes']))+'\n');chunks.flush();pending+=block
                while b'\n'in pending:
                    line,pending=pending.split(b'\n',1);state['complete_lines']+=1
                    text=line.decode('utf8',errors='replace')
                    if re.search(r'Guru Meditation|assert failed|Task watchdog|NES_(?:WORKER|RUNTIME)_FAIL|ROM_(?:LOAD|READ)_FAIL',text):
                        state['firmware_faults']+=1;event('device_fault',line=text[:2000])
                    try:row=parse_line(text)
                    except(ValueError,TypeError)as exc:
                        state['parse_errors']+=1;event('parse_error',description=str(exc));continue
                    if not row:continue
                    tag,f=row['tag'],row['fields']
                    if tag=='NES_GAME_START':
                        state['game_launches']+=1;state['latest_game']=f;state['last_frame']=None;event('game_start',fields=f)
                    elif tag=='BOOT':
                        state['observed_boots']+=1;state['latest_game']=None;state['last_frame']=None;event('boot',fields=f)
                    elif tag=='FIRMWARE_INFO':state['firmware_identity']=f;event('firmware_info',fields=f)
                    elif tag in ('NES_OUTPUT_FRAME_PIPELINE_TIMES','NES_COUPLED_BENCH'):
                        state['last_frame']={k:f[k]for k in ('game','frames','generation','mapper','region')if k in f}
                if len(pending)>262144:
                    state['oversized_partial_lines']+=1;event('oversized_partial_line',bytes=len(pending));pending=b''
            if time.monotonic()-heartbeat>=1:
                state['updated_utc']=now();atomic_json(folder/'state.json',state);heartbeat=time.monotonic()
        state['status']='stopped';event('stopped',partial_tail_bytes=len(pending))
    except Exception as exc:
        state['status']='failed';state['error']=str(exc)
        if events:event('failure',description=str(exc))
    finally:
        if port:port.close()
        if raw:raw.flush();os.fsync(raw.fileno());raw.close()
        if events:events.close()
        if chunks:chunks.close()
        state['updated_utc']=now();state['ended_utc']=now();state['COM_open']=False
        atomic_json(folder/'state.json',state)
    return state


def main(argv=None):
    """Без execute показывает план; stop/status работают только с файлами ПК."""
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path)
    p.add_argument('--port',default='COM4');p.add_argument('--vid',type=lambda x:int(x,0),default=0x1a86);p.add_argument('--pid',type=lambda x:int(x,0),default=0x55d3)
    p.add_argument('--serial');p.add_argument('--baud',type=int,default=115200)
    p.add_argument('--execute-root',action='store_true');p.add_argument('--max-seconds',type=float)
    p.add_argument('--stop',type=Path);p.add_argument('--status',type=Path)
    a=p.parse_args(argv)
    if a.stop:result=request_stop(a.stop)
    elif a.status:result=json.loads((a.status/'state.json').read_text(encoding='utf8'))
    elif not a.execute_root:result=dict(execute=False,COM_opened=False,commands_sent=0,port=a.port,output=str(a.output)if a.output else None)
    else:
        if not a.output:p.error('--output is required for execution')
        result=record(a)
    print(json.dumps(result,ensure_ascii=False));return 2 if result.get('status')=='failed'else 0


if __name__=='__main__':raise SystemExit(main())
