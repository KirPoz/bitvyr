"""Общие zero-based guest input edges для HOST replay и явно запрошенного UART movie."""
from bisect import bisect_right
import re

MAX_FRAME = 360000

def parse_edges(specs):
    """Проверяет до16 strictly increasing FRAME:MASK edges, диапазоны и обязательный финальный release0."""
    if len(specs) > 16:
        raise ValueError('input movie accepts at most 16 edges, matching the UART queue')
    events = []
    for spec in specs:
        parts = spec.split(':')
        if len(parts) != 2:
            raise ValueError('input edge must be FRAME:MASK')
        if not re.fullmatch(r'[0-9]+',parts[0]) or not re.fullmatch(r'0[xX][0-9a-fA-F]+|0|[1-9][0-9]*',parts[1]):
            raise ValueError('frame must be decimal; mask decimal or hexadecimal without signs or trailing text')
        frame, mask = int(parts[0], 10), int(parts[1], 0)
        if not 0 <= frame <= MAX_FRAME or not 0 <= mask <= 255:
            raise ValueError('input frame must be 0..360000 and mask 0..255')
        if events and frame <= events[-1][0]:
            raise ValueError('input edge frames must be strictly increasing')
        events.append((frame, mask))
    if events and events[-1][1] != 0:
        raise ValueError('input movie must end with an explicit zero mask release')
    return events

def mask_at(events, frame):
    """Возвращает последнюю маску не позже кадра; до первого edge контроллер отпущен."""
    i = bisect_right([edge[0] for edge in events], frame)-1
    return events[i][1] if i >= 0 else 0

def changes(events):
    """Выдаёт только кадры фактической смены маски, начиная с исходного released0."""
    previous = 0
    for frame, mask in events:
        if mask != previous:
            yield frame
        previous = mask
