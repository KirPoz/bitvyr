"""Общий транспорт UART и проверка сеанса для автоматических прогонов."""
from pathlib import Path
import hashlib
import json
import re
import time

ANSI=re.compile(r'\x1b\[[0-9;]*m')
FAULT=re.compile(r'\bNES_[A-Z0-9_]*(?:FAIL|ERROR)\b|ROM_(?:LOAD|READ)_FAIL|Guru Meditation|panic|assert failed|stack overflow|Task watchdog',re.I)


def require(value,reason):
    """Завершает проверку явным HOLD; не зависит от оптимизации Python/assert."""
    if not value:raise ValueError(reason)


def pin(path):
    """Фиксирует байты текущего входа/лога; старые FPS не подмешиваются."""
    raw=path.read_bytes();return {'path':str(path),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}


def values(text):
    """Разбирает scalar key=value; int оставляет числовым, policy/CRC — строкой."""
    return {k:int(v) if k not in ('crc','crc32','ids') and re.fullmatch(r'-?\d+',v) else v for k,v in re.findall(r'(\w+)=([^\s]+)',text)}


def object_row(line,marker):
    """Читает настоящий JSON после wire-marker без исправления входных данных."""
    at=line.find(marker+' ')
    return json.loads(line[at+len(marker)+1:]) if at>=0 else None


class Link:
    """Единственный UART owner; DTR/RTS остаются false, reset/flash отсутствуют."""
    def __init__(self,args,log):
        """Проверяет USB VID/PID/serial до открытия порта."""
        self.expected_volume = args.expected_volume
        import serial
        from serial.tools import list_ports
        ports=[p for p in list_ports.comports() if p.device.upper()==args.port.upper()]
        require(len(ports)==1 and ports[0].vid==args.vid and ports[0].pid==args.pid,'USB identity mismatch')
        require(not args.serial or ports[0].serial_number==args.serial,'USB serial mismatch')
        self.usb={'port':ports[0].device,'vid':ports[0].vid,'pid':ports[0].pid,'serial':ports[0].serial_number}
        # Большой read с100ms timeout задерживал уже пришедшие APPLY до6 кадров.
        # Короткий RX wait сохраняет bounded-lookahead, не меняя baud или гостевой ввод.
        self.log=log.open('xb');self.port=serial.Serial(port=None,baudrate=115200,timeout=.01,write_timeout=3)
        self.usb['host_rx_timeout_s']=.01
        try:
            self.port.dtr=False;self.port.rts=False;self.port.port=ports[0].device;self.port.open()
        except Exception:self.log.close();self.port.close();raise
        self.pending=b'';self.rows=[];self.commands=[]

    def send(self,commands,phase):
        """Передаёт ограниченные команды и сохраняет rx-offset; NVS write-команд здесь нет."""
        for command in commands:
            require(re.fullmatch(r'(?:ui_status|status|menu|tasks|pad 0|catalog 0 64|run \d+|padat \d+ \d+)',command),'Command outside qualification whitelist')
            data=(command+'\n').encode('ascii');n=self.port.write(data);self.port.flush()
            require(n==len(data),'Short UART command write')
            self.commands.append({'command':command,'phase':phase,'rx_bytes_before':self.log.tell()})

    def read(self):
        """Сохраняет весь raw и выдаёт только завершённые строки; partial line ограничена64KiB."""
        data=self.port.read(16384)
        if data:self.log.write(data);self.log.flush();self.pending+=data
        lines=[]
        while b'\n' in self.pending:
            raw,self.pending=self.pending.split(b'\n',1)
            line=ANSI.sub('',raw.decode('utf-8',errors='replace')).strip();self.rows.append(line);lines.append(line)
        require(len(self.pending)<65536,'Unterminated UART line')
        for line in lines:require(not FAULT.search(line),'Device fault: '+line[:200])
        return lines

    def collect(self,seconds):
        """Читает ограниченное host-время только для транспорта; оно не используется как FPS."""
        until=time.monotonic()+seconds
        while time.monotonic()<until:self.read()

    def close(self):
        """Освобождает только собственный UART и raw файл, без сброса платы."""
        self.port.close();self.log.close()


def latest(lines,marker):
    """Возвращает последнюю фактическую JSON-строку указанного wire-типа."""
    rows=[r for line in lines if (r:=object_row(line,marker)) is not None]
    require(rows,'Missing '+marker);return rows[-1]


def quiet(settings,playing=False,expected_volume=1):
    """Проверяет заданную громкость и реальные параметры; не меняет настройки/NVS."""
    require(type(expected_volume) is int and 1<=expected_volume<=100,'Expected volume must be integer1..100')
    require(type(settings.get('volume')) is int and type(settings.get('hardware_volume')) is int and settings['volume']==settings['hardware_volume']==expected_volume and settings['muted'] is False,'Expected volume/unmuted mismatch')
    require(not settings['dirty'],'Settings already dirty; capture must not persist them')
    require(settings['screen']==('game' if playing else 'library'),'Unexpected UI owner')
    if playing:require(settings['active_region']=='NTSC','Actual active region must be NTSC')


def catalog(link):
    """Получает целую свежую страницу и сопоставляет CRC с текущим index, а не историческим."""
    start=len(link.rows);link.send(['ui_status','catalog 0 64'],'catalog');link.collect(2)
    lines=link.rows[start:];quiet(latest(lines,'NES_SETTINGS'),expected_volume=getattr(link,'expected_volume',1))
    meta=latest(lines,'NES_CATALOG');games=[r for line in lines if (r:=object_row(line,'NES_ROM')) is not None]
    require(not meta['error'] and meta['skipped']==0 and len(games)==meta['count'],'Incomplete live catalog')
    require(len({r['crc32'] for r in games})==len(games),'Ambiguous duplicate CRC')
    return {r['crc32'].lower():r for r in games}



def admit_boot(text, elf, project):
    """Сопоставляет последний запуск с полным хэшем выбранного локального ELF."""
    require(re.fullmatch(r"[0-9a-f]{64}", elf), "Expected full lowercase ELF SHA256")
    text = ANSI.sub("", text)
    cuts = list(re.finditer(r"ELF file SHA256:\s*([0-9a-fA-F]+)", text))
    require(cuts and 8 <= len(cuts[-1][1]) <= 64, "Missing last boot ELF prefix")
    prefix = cuts[-1][1].lower()
    require(elf.startswith(prefix), "Last boot ELF prefix mismatch")
    before = text[:cuts[-1].start()]
    names = re.findall(r"Project name:\s*([^\s]+)", before)
    require(bool(names) and names[-1] == project, "Boot project mismatch")
    tail = text[cuts[-1].end():]
    require(re.search(r"\bBOARD_READY display=1 touch=1 audio=1 sd=1 psram=1\b", tail),
            "Admission lacks healthy post-boot BOARD_READY")
    return prefix, tail


def local_image(project, expected, path=None):
    """Проверяет SHA256 рабочего или явно указанного ELF перед открытием порта."""
    if path is None:
        root = Path(__file__).resolve().parent.parent
        path = root / project / "betta" / "build" / (project + ".elf")
    path = path.resolve()
    require(path.is_file(), "Build the selected project before recording: " + str(path))
    entry = pin(path)
    require(entry["sha256"] == expected, "Current local ELF SHA256 mismatch")
    return entry
