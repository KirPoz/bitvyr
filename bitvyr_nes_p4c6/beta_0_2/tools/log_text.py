"""Общие байтовые правила чтения сохранённого текста и удаления ANSI CSI.

Модуль не разбирает JSON, теги, generation или допустимость измерения. Строгие
UART-аудиторы сохраняют собственный strict UTF-8 путь. Исходные bytes и их SHA
остаются у вызывающей стороны; CR/LF и пробелы здесь не нормализуются.
"""
import re


ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def decode_text(data: bytes) -> str:
    """Декодирует caller-owned bytes по прежней политике offline-инструментов.

    UTF-16 выбирается только при LE/BE BOM; иначе используется UTF-8-SIG.
    Неполные/ошибочные кодовые единицы заменяются U+FFFD, как в прежних двух
    реализациях. Переводы строк и ANSI сохраняются. Возвращается новый str;
    исходный byte-буфер не меняется и по-прежнему пригоден для provenance SHA.
    """
    encoding = 'utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    return data.decode(encoding, errors='replace')
