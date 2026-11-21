"""Независимые synthetic ZIP/NES fixtures для проверки собственных GameLibrary/DEFLATE; выходы принадлежат каталогу native-сборки."""
from pathlib import Path
import argparse
import binascii
import hashlib
import io
import json
import random
import struct
import subprocess
import zipfile
import zlib
ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runner', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
corpus_group = parser.add_mutually_exclusive_group()
corpus_group.add_argument('--archive-list', type=Path, help='Optional UTF-8 list of private ZIP paths; source archives are only read.')
corpus_group.add_argument('--corpus-directory', type=Path, help='Optional directory of private ZIP archives; source files are only read.')
args = parser.parse_args()
HERE = args.output.resolve()
HERE.mkdir(parents=True, exist_ok=True)
RUNNER = args.runner.resolve()
FIXTURES = HERE / 'fixtures'
OUTPUT = HERE / 'output'
FIXTURES.mkdir(exist_ok=True)
OUTPUT.mkdir(exist_ok=True)
ERRORS = ['Ok', 'Io', 'InvalidArchive', 'Unsupported', 'Encrypted', 'TooLarge', 'NameTooLong', 'Cancelled', 'InvalidEntry', 'Truncated', 'InvalidDeflate', 'CrcMismatch', 'OutputTooSmall', 'NoMemory']
cases = []
rng = random.Random(25116612945)

def add(label, blob, entries=(), scan='Ok', directory='fixtures'):
    """Сохраняет synthetic ZIP и ожидаемый исход в отдельном временном подкаталоге; blob задаёт все проверяемые байты."""
    path = HERE / directory / f'{len(cases):03d}-{label}.zip'
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(blob)
    cases.append({'label': label, 'path': path, 'entries': list(entries), 'scan': scan})

def expect(name, data, full='Ok', prefix='Ok'):
    """Создаёт ожидание полного/префиксного чтения; имя приводится к UTF-8-байтам без потери сырого имени."""
    return {'name': name.encode('utf-8') if isinstance(name, str) else name, 'data': data, 'full': full, 'prefix': prefix}

def standard(files, method=zipfile.ZIP_DEFLATED, level=6, comment=b''):
    """Создаёт ZIP через независимый zipfile и перепроверяет содержимое; EOCD внутри комментария проверяется без ложной сигнатуры."""
    memory = io.BytesIO()
    with zipfile.ZipFile(memory, 'w', compression=method, compresslevel=level) as archive:
        for name, data in files:
            archive.writestr(name, data)
        archive.comment = comment
    result = memory.getvalue()
    reference = bytearray(result[:-len(comment)]) if b'PK\x05\x06' in comment else result
    if b'PK\x05\x06' in comment:
        reference[-2:] = b'\x00\x00'
    with zipfile.ZipFile(io.BytesIO(reference)) as archive:
        assert archive.testzip() is None
        for name, data in files:
            assert archive.read(name) == data
    return result

def raw_deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    """Сжимает через zlib без обёртки и требует точной обратной распаковки, чтобы генератор не разделял ошибку C++-декодера."""
    compressor = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    raw = compressor.compress(data) + compressor.flush()
    assert zlib.decompress(raw, -15) == data
    return raw

def custom(name, data, raw=None, method=8, flags=0, descriptor=None, crc=None, size=None, extra=b'', attributes=0):
    """Собирает local/central/EOCD и optional descriptor вручную; crc/size/flags позволяют создавать как валидные, так и повреждённые случаи."""
    name = name.encode('utf-8') if isinstance(name, str) else name
    raw = raw_deflate(data) if raw is None else raw
    crc = binascii.crc32(data) & 4294967295 if crc is None else crc
    size = len(data) if size is None else size
    if descriptor is not None:
        flags |= 8
    local_values = (0, 0, 0) if flags & 8 else (crc, len(raw), size)
    local = struct.pack('<IHHHHHIIIHH', 67324752, 20, flags, method, 0, 0, *local_values, len(name), len(extra)) + name + extra + raw
    if descriptor is not None:
        local += (struct.pack('<I', 134695760) if descriptor else b'') + struct.pack('<III', crc, len(raw), size)
    central = struct.pack('<IHHHHHHIIIHHHHHII', 33639248, 20, 20, flags, method, 0, 0, crc, len(raw), size, len(name), len(extra), 0, 0, 0, attributes, 0) + name + extra
    return local + central + struct.pack('<IHHHHIIH', 101010256, 0, 0, 1, 1, len(central), len(local), 0)
header = b'NES\x1a' + bytes((2, 1)) + bytes(10)
normal = header + bytes(range(256)) * 64
for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
    for level in [None] if method == zipfile.ZIP_STORED else [0, 1, 6, 9]:
        label = f'standard-{method}-{level}'
        add(label, standard([('nested/game.nes', normal), ('empty.bin', b''), ('tiny.nes', b'NES')], method, level), [expect('nested/game.nes', normal), expect('empty.bin', b''), expect('tiny.nes', b'NES')])
for strategy in (zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_DEFAULT_STRATEGY):
    data = header + b'abracadabra\x00' * 10000 + rng.randbytes(6000)
    raw = raw_deflate(data, 6, strategy)
    add(f'strategy-{strategy}', custom('game.nes', data, raw), [expect('game.nes', data)])
for descriptor in (False, True):
    for method in (0, 8):
        raw = normal if method == 0 else raw_deflate(normal)
        add(f'descriptor-{descriptor}-{method}', custom('game.nes', normal, raw, method, descriptor=descriptor), [expect('game.nes', normal)])

class NonSeekable(io.BytesIO):
    """BytesIO без seek вынуждает zipfile использовать потоковый data descriptor; это независимый источник корректного архива."""

    def seekable(self):
        """Сообщает zipfile, что позиционирование недоступно, не меняя накопленные байты."""
        return False

    def seek(self, *args):
        """Отклоняет любые попытки позиционирования исключением UnsupportedOperation."""
        raise io.UnsupportedOperation('no seek')
memory = NonSeekable()
with zipfile.ZipFile(memory, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('stream.nes', normal)
add('zipfile-stream-descriptor', memory.getvalue(), [expect('stream.nes', normal)])
files = [('directory/', b''), ('nested/', b'')] + [(f'nested/game{i:03d}.nes', header + bytes([i]) * 200) for i in range(137)]
files += [('readme.txt', b'not an NES but retained by generic ZIP scanner')]
add('all-members-137-games', standard(files), [expect(name, data) for name, data in files if not name.endswith('/')])
unicode_name = 'Игры/ゼルダ.nes'
add('unicode-member', standard([(unicode_name, normal)]), [expect(unicode_name, normal)])
add('unicode-archive', standard([('a.nes', normal)]), [expect('a.nes', normal)], directory='Архивы')
add('long-comment', standard([('a.nes', normal)], comment=b'PK\x05\x06' + b'comment' * 9361), [expect('a.nes', normal)])
add('empty', standard([]), [])
add('raw-empty-file', b'', scan='InvalidArchive')
add('raw-known-crc', b'123456789', scan='InvalidArchive')
add('directory-attributes', custom('looks-like-file.nes', b'', b'', 0, attributes=16), [])
distance_data = header + rng.randbytes(32768) * 5
add('window-32768', custom('long.nes', distance_data), [expect('long.nes', distance_data)])
large = header + b'large-output-pattern\x00' * 150000
add('large-output', custom('large.nes', large), [expect('large.nes', large)])
oversize = header + b'X' * (5 * 1024 * 1024)
add('output-capacity', custom('large.nes', oversize), [expect('large.nes', oversize, full='OutputTooSmall')])
badcrc = (binascii.crc32(normal) ^ 1) & 4294967295
add('crc-mismatch', custom('game.nes', normal, normal, 0, crc=badcrc), [expect('game.nes', normal, full='CrcMismatch')])
add('encrypted', custom('game.nes', normal, flags=1), [expect('game.nes', normal, full='Encrypted', prefix='Encrypted')])
add('unsupported-method', custom('game.nes', normal, b'unsupported', 12), [expect('game.nes', normal, full='Unsupported', prefix='Unsupported')])
add('output-too-small-declared', custom('game.nes', normal, size=8), [expect('game.nes', normal, full='InvalidDeflate', prefix='InvalidDeflate')])
add('output-too-large-declared', custom('game.nes', normal, size=len(normal) + 1), [expect('game.nes', normal, full='InvalidDeflate')])
bad = bytearray(custom('game.nes', normal))
bad[30] ^= 1
add('local-name-mismatch', bad, [expect('game.nes', normal, full='InvalidEntry', prefix='InvalidEntry')])
bad = bytearray(custom('game.nes', normal))
bad[14] ^= 1
add('local-crc-mismatch', bad, [expect('game.nes', normal, full='InvalidEntry', prefix='InvalidEntry')])
bad = bytearray(custom('game.nes', normal))
struct.pack_into('<I', bad, 18, 4294967295)
add('zip64-local', bad, [expect('game.nes', normal, full='Unsupported', prefix='Unsupported')])
add('zip64-extra', custom('a.nes', normal, extra=struct.pack('<HHQ', 1, 8, len(normal))), scan='Unsupported')
add('invalid-extra', custom('a.nes', normal, extra=b'DU'), scan='InvalidEntry')
add('long-name', custom('a' * 1024, normal), scan='NameTooLong')
add('null-name', custom(b'NES\x00evil.nes', normal), scan='InvalidEntry')
bad = bytearray(custom('game.nes', normal))
struct.pack_into('<H', bad, len(bad) - 18, 1)
add('multidisk', bad, scan='Unsupported')
bad = bytearray(custom('game.nes', normal))
struct.pack_into('<H', bad, len(bad) - 12, 65535)
add('zip64-eocd', bad, scan='Unsupported')
bad = bytearray(custom('game.nes', normal))
del bad[-12:]
add('truncated-eocd', bad, scan='InvalidArchive')
bad = bytearray(custom('game.nes', normal))
cd = bad.index(b'PK\x01\x02')
struct.pack_into('<I', bad, 18, 1048576)
struct.pack_into('<I', bad, cd + 20, 1048576)
add('payload-crosses-central', bad, [expect('game.nes', normal, full='InvalidEntry', prefix='InvalidEntry')])
bad = bytearray(custom('game.nes', normal))
cd = bad.index(b'PK\x01\x02')
struct.pack_into('<I', bad, cd + 42, cd)
add('local-offset-in-central', bad, scan='InvalidEntry')
bad = bytearray(custom('game.nes', normal))
struct.pack_into('<H', bad, 28, 65535)
add('local-extra-crosses-central', bad, [expect('game.nes', normal, full='InvalidEntry', prefix='InvalidEntry')])
bad = bytearray(custom('game.nes', normal))
cd = bad.index(b'PK\x01\x02')
struct.pack_into('<H', bad, cd + 28, 65535)
add('central-name-crosses-directory', bad, scan='InvalidEntry')
bad = bytearray(custom('game.nes', normal))
struct.pack_into('<I', bad, len(bad) - 6, len(bad) + 1000)
add('directory-out-of-file', bad, scan='InvalidArchive')
add('zero-declared-output', custom('game.nes', normal, size=0), [expect('game.nes', normal, full='InvalidDeflate')])
bad = bytearray(custom('game.nes', normal, descriptor=True))
bad[30 + len('game.nes') + len(raw_deflate(normal)) + 4] ^= 1
add('bad-descriptor', bad, [expect('game.nes', normal, full='InvalidEntry', prefix='InvalidEntry')])

class Bits:
    """Упаковывает вручную заданный DEFLATE-поток: поля передаются младшим битом, Huffman-коды — старшим."""

    def __init__(self):
        """Создаёт собственный пустой список битов одного synthetic DEFLATE-потока."""
        self.bits = []

    def put(self, value, count):
        """Добавляет count младших битов value в порядке LSB-first для обычных полей DEFLATE."""
        self.bits.extend((value >> i & 1 for i in range(count)))

    def huff(self, value, count):
        """Добавляет count битов канонического Huffman-кода в порядке MSB-first."""
        self.bits.extend((value >> i & 1 for i in range(count - 1, -1, -1)))

    def bytes(self):
        """Упаковывает биты в байты LSB-first; неполный последний байт заполняется нулями."""
        return bytes((sum((bit << i for i, bit in enumerate(self.bits[p:p + 8]))) for p in range(0, len(self.bits), 8)))
b = Bits()
b.put(3, 3)
b.huff(1, 7)
b.huff(0, 5)
b.huff(0, 7)
add('invalid-distance', custom('a.nes', b'aaa', b.bytes()), [expect('a.nes', b'aaa', full='InvalidDeflate', prefix='InvalidDeflate')])
add('reserved-block', custom('a.nes', b'abc', b'\x07'), [expect('a.nes', b'abc', full='InvalidDeflate', prefix='InvalidDeflate')])
add('stored-invalid-inverse', custom('a.nes', b'abc', b'\x01\x03\x00\x00\x00abc'), [expect('a.nes', b'abc', full='InvalidDeflate', prefix='InvalidDeflate')])
b = Bits()
b.put(5, 3)
b.put(0, 5)
b.put(0, 5)
b.put(0, 4)
for x in [1, 1, 1, 0]:
    b.put(x, 3)
add('oversubscribed-clen', custom('a.nes', b'abc', b.bytes()), [expect('a.nes', b'abc', full='InvalidDeflate', prefix='InvalidDeflate')])
b = Bits()
b.put(5, 3)
b.put(0, 5)
b.put(0, 5)
b.put(0, 4)
for x in [1, 0, 0, 1]:
    b.put(x, 3)
b.huff(1, 1)
add('repeat-without-previous', custom('a.nes', b'abc', b.bytes()), [expect('a.nes', b'abc', full='InvalidDeflate', prefix='InvalidDeflate')])
raw = raw_deflate(normal)
add('truncated-deflate', custom('a.nes', normal, raw[:-1]), [expect('a.nes', normal, full={'Truncated', 'InvalidDeflate'})])
add('trailing-deflate-bytes', custom('a.nes', normal, raw + b'junk'), [expect('a.nes', normal, full='InvalidDeflate')])
add('descriptor-crc-equals-signature', custom('a.nes', normal, descriptor=False, crc=134695760), [expect('a.nes', normal, full='CrcMismatch')])
add('maximum-name-1023', custom('a' * 1019 + '.nes', normal), [expect('a' * 1019 + '.nes', normal)])

def dynamic_empty_distance(literal=False):
    """Создаёт допустимое dynamic-дерево без используемых distance-кодов: только EOB либо literal и EOB; результат перепроверяется zlib."""
    b = Bits()
    b.put(5, 3)
    b.put(0, 5)
    b.put(0, 5)
    b.put(14, 4)
    order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1]
    for symbol in order:
        b.put(1 if symbol == 0 else 2 if symbol in (1, 18) else 0, 3)

    def zeros(count):
        """Кодирует count нулевых длин сериями repeat18 до138элементов и отдельными нулями для короткого хвоста."""
        while count:
            take = min(138, count)
            if take >= 11:
                b.huff(3, 2)
                b.put(take - 11, 7)
            else:
                for _ in range(take):
                    b.huff(0, 1)
            count -= take
    if literal:
        zeros(65)
        b.huff(2, 2)
        zeros(190)
        b.huff(2, 2)
        b.huff(0, 1)
        for _ in range(4):
            b.huff(0, 1)
        b.huff(1, 1)
    else:
        zeros(256)
        b.huff(2, 2)
        b.huff(0, 1)
        b.huff(0, 1)
    result = b.bytes()
    data = b'AAAA' if literal else b''
    assert zlib.decompress(result, -15) == data
    return (result, data)
for literal in (False, True):
    stream, data = dynamic_empty_distance(literal)
    add(f'empty-distance-{literal}', custom('a.nes', data, stream), [expect('a.nes', data)])
for index in range(160):
    length = rng.randrange(0, 100000)
    kind = index % 5
    data = rng.randbytes(length) if kind == 0 else bytes([index & 255]) * length if kind == 1 else (rng.randbytes(1031) * (length // 1031 + 1))[:length] if kind == 2 else (bytes(range(256)) * (length // 256 + 1))[:length] if kind == 3 else bytes((rng.randrange(8) for _ in range(length)))
    level = index % 10
    strategy = [zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE][index % 4]
    add(f'corpus-{index}', custom('random.nes', data, raw_deflate(data, level, strategy)), [expect('random.nes', data)])
for index in range(120):
    raw = rng.randbytes(rng.randrange(0, 192))
    try:
        decoded = zlib.decompress(raw, -15)
    except zlib.error:
        decoded = None
    if decoded is not None:
        add(f'raw-fuzz-valid-{index}', custom('fuzz.nes', decoded, raw), [expect('fuzz.nes', decoded)])
    else:
        expected = expect('fuzz.nes', b'Z' * 512, full={'InvalidDeflate', 'Truncated', 'CrcMismatch'}, prefix={'Ok', 'InvalidDeflate', 'Truncated'})
        expected['prefix_data_unknown'] = True
        add(f'raw-fuzz-{index}', custom('fuzz.nes', expected['data'], raw), [expected])
complete = raw_deflate(normal)
for end in sorted(set([0, 1, 2, 3, 4, 5, 7, 11, 17, len(complete) // 2, len(complete) - 2, len(complete) - 1])):
    add(f'deflate-cut-{end}', custom('cut.nes', normal, complete[:end]), [expect('cut.nes', normal, full={'Truncated', 'InvalidDeflate'}, prefix={'Ok', 'Truncated', 'InvalidDeflate'})])
corpus_sources = []
if args.archive_list:
    corpus_paths = [Path(line.strip()).resolve() for line in args.archive_list.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
elif args.corpus_directory:
    corpus_paths = sorted((path.resolve() for path in args.corpus_directory.iterdir() if path.is_file() and path.suffix.lower() == '.zip'))
else:
    corpus_paths = []
for index, path in enumerate(corpus_paths):
    blob = path.read_bytes()
    with zipfile.ZipFile(io.BytesIO(blob)) as archive:
        assert archive.testzip() is None, path
        entries = []
        for info in archive.infolist():
            if info.is_dir() or info.external_attr & 16 or info.external_attr >> 16 & 61440 == 16384:
                continue
            name = info.filename.encode('utf-8' if info.flag_bits & 2048 else 'cp437')
            entries.append(expect(name, archive.read(info)))
    add(f'external-corpus-{index}', blob, entries)
    corpus_sources.append({'path': str(path), 'sha256': hashlib.sha256(blob).hexdigest(), 'bytes': len(blob), 'members': len(entries)})
list_path = HERE / 'cases.txt'
list_path.write_text(''.join((str(case['path']) + '\n' for case in cases)), encoding='utf-8')
execution = subprocess.run([str(RUNNER), str(list_path), str(OUTPUT)], check=True, capture_output=True, text=True)
(HERE / 'results.txt').write_text(execution.stdout, encoding='utf-8')
scan_records, entry_records, crc_records = ({}, {}, {})
for line in execution.stdout.splitlines():
    p = line.split('|')
    if p[0] == 'SCAN':
        scan_records[int(p[1])] = list(map(int, p[2:]))
    elif p[0] == 'ENTRY':
        entry_records[int(p[1]), int(p[2])] = p[3:]
    elif p[0] == 'CRC':
        crc_records[int(p[1])] = list(map(int, p[2:]))
    else:
        raise AssertionError(line)
assert len(scan_records) == len(cases)
assert len(crc_records) == len(cases)
entries_checked = 0
for ci, case in enumerate(cases):
    crc_error, file_crc, file_bytes = crc_records[ci]
    archive_bytes = case['path'].read_bytes()
    assert crc_error == 0 and file_bytes == len(archive_bytes)
    assert file_crc == binascii.crc32(archive_bytes) & 4294967295
    scan, count, cancelled = scan_records[ci]
    assert ERRORS[scan] == case['scan'], (ci, case['label'], 'scan', ERRORS[scan], case['scan'])
    if case['scan'] != 'Ok':
        continue
    assert count == len(case['entries']), (ci, case['label'], count, len(case['entries']))
    assert ERRORS[cancelled] == ('Cancelled' if count else 'Ok')
    for ei, expected in enumerate(case['entries']):
        fields = entry_records[ci, ei]
        method, flags, crc, size, pe, pn, fe, fn, mutations, guards = map(int, fields[:10])
        assert bytes.fromhex(fields[10]) == expected['name'], (ci, ei, 'name')
        assert mutations == guards == 1, (ci, ei, 'bounds/metadata', fields)
        for code, key in [(pe, 'prefix'), (fe, 'full')]:
            allowed = expected[key] if isinstance(expected[key], set) else {expected[key]}
            assert ERRORS[code] in allowed, (ci, case['label'], ei, key, ERRORS[code], allowed)
        if pe == 0:
            target = min(16, size)
            assert pn == target
            if not expected.get('prefix_data_unknown'):
                assert (OUTPUT / f'{ci}-{ei}.prefix').read_bytes() == expected['data'][:target], (ci, ei, 'prefix bytes')
        if fe == 0:
            assert fn == len(expected['data']) == size
            assert crc == binascii.crc32(expected['data']) & 4294967295
            assert (OUTPUT / f'{ci}-{ei}.full').read_bytes() == expected['data'], (ci, ei, 'full bytes')
        entries_checked += 1
report = {'passed': True, 'archives': len(cases), 'members_checked': entries_checked, 'external_corpus': corpus_sources, 'reference': 'Python standard zipfile + zlib, independently generated bytes/CRC', 'zlib_reference_version': zlib.ZLIB_RUNTIME_VERSION, 'harness_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path(__file__), RUNNER, HERE / 'results.txt']}, 'implementation_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in ['app/nes/storage/ZipArchive.h', 'app/nes/storage/ZipArchive.cpp']}, 'runner': str(RUNNER), 'scope': ['STORE', 'DEFLATE stored/fixed/dynamic', 'signed/unsigned data descriptors', '137 synthetic NES members, no 64-game cap', 'prefix capacities0/1/15/16/17/33/512 and full output', 'UTF8 archive and member names', 'CRC32', 'central/local identity mutation', 'guarded output capacity', 'large output/back-reference windows', 'malformed codes, repeat, distance, bounds, encryption, ZIP64, descriptors, truncation', 'whole-file CRC32 versus Python, empty/known-vector/nonexistent path'], 'cases': [{'label': c['label'], 'archive_sha256': hashlib.sha256(c['path'].read_bytes()).hexdigest(), 'scan': c['scan'], 'members': len(c['entries'])} for c in cases]}
(HERE / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps({key: report[key] for key in ['passed', 'archives', 'members_checked']}, indent=2))
