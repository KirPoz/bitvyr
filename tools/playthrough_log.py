"""Общий разбор UART-телеметрии NES; не открывает COM и не изменяет устройство."""
import json
import re

ANSI=re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
PAIR=re.compile(r'(\w+)=([^\s]+)')
TEXT_KEYS={'crc','crc32','ids','pcm_hash','rgb_hash','ram_hash','read_hash','source_addr'}


def parse_line(line):
    """Возвращает исходный tag/fields/device_ms; имя ROM сохраняет пробелы.

    Время ПК сюда не подмешивается. Неизвестный текст остаётся в raw у вызывающего,
    а повреждённый JSON отвергается явно, без исправления измеренных значений.
    """
    clean=ANSI.sub('',line).strip()
    if clean.startswith(('ESP-ROM:','ets Jun ','ets Jul ')):
        return dict(tag='BOOT',fields={'description':clean},device_ms=None,line=clean)
    firmware=re.search(r'ELF file SHA256:\s*([0-9a-fA-F]+)',clean)
    if firmware:
        return dict(tag='FIRMWARE_INFO',fields={'ELF_sha256_prefix':firmware[1].lower()},device_ms=None,line=clean)
    match=re.search(r'\b(NES_[A-Z0-9_]+|ROM_(?:LOAD|READ)_FAIL)\s+(.*)',clean)
    if not match:return None
    tag,body=match.groups();stamp=re.search(r'\b[IWEVD]\s*\(\s*(\d+)\)',clean)
    if body.startswith('{'):
        fields=json.loads(body)
        if not isinstance(fields,dict):raise ValueError('Telemetry JSON must be an object')
    else:
        fields={k:int(v) if k not in TEXT_KEYS and not k.endswith('_hash') and re.fullmatch(r'-?\d+',v) else v for k,v in PAIR.findall(body)}
        if tag=='NES_GAME_START':
            name=re.search(r'\bname=(.*)\s+mapper=(\d+)\s+region=(\S+)\s+crc=([0-9a-fA-F]{8})\b',body)
            if not name:raise ValueError('GAME_START lacks complete ROM identity')
            fields.update(name=name[1],mapper=int(name[2]),region=name[3],crc=name[4].lower())
    if tag in ('ROM_LOAD_FAIL','ROM_READ_FAIL'):
        detail=re.search(r'\b(reason|error)=(.*)$',body)
        if detail:fields[detail[1]]=detail[2]
    return dict(tag=tag,fields=fields,device_ms=int(stamp[1])if stamp else None,line=clean)
