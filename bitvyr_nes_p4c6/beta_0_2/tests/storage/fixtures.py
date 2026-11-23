from pathlib import Path
import argparse,json,zipfile,zlib,hashlib,os,subprocess
parser=argparse.ArgumentParser(description='Generate synthetic NES/storage fixtures in the build directory.')
parser.add_argument('--output',type=Path,required=True)
root=parser.parse_args().output.resolve();root.mkdir(parents=True,exist_ok=True)
data=root
raw=data/'raw';raw.mkdir(exist_ok=True)
header=bytearray(b'NES\x1a'+bytes([1,0])+bytes(10));rom=bytes(header)+bytes((i*13+7)&255 for i in range(16384))
expected=0
def put(name,content=rom):
 """Записывает synthetic ROM в raw, создаёт родительские каталоги и увеличивает expected только после записи; Unicode-путь Windows использует расширенный префикс."""
 global expected
 path=raw/name
 if os.name=='nt' and len(str(path))>=240:path=Path('\\\\?\\'+str(path))
 path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(content);expected+=1
for i in range(1100):put(f'g{i:04d}'+('.NES' if i%2 else '.nes'))
put('a/alpha.nes');put('b/ALPHA.NES');put('Ж'*65+'.nes')
# Только POSIX: raw-путь из шести105-символьных компонентов превышает Windows MAX_PATH.
# Созданный через \\?\ путь не удаляется штатным shutil.rmtree/idf Full Clean; Unicode,
# предел глубины18 и ZIP member1020 проверяются отдельно и сохраняются на Windows.
if os.name!='nt':
 put('/'.join(['long'+str(i)+'x'*100 for i in range(6)])+'/Long.nes')
put('Europe (E).nes');put('Aladdin (E).nes',rom[:6]+bytes([0,0xa0])+rom[8:])
pal160=bytearray(rom);pal160[7]=0xa0;pal160[9]=1;put('ExplicitPAL.nes',pal160)
nes2=bytearray(rom);nes2[7]=8;nes2[8]=0x0a;nes2[6]=0xb0;nes2[7]|=0xc0;nes2[12]=0;put('NES2 NTSC (E).nes',nes2)
nes2pal=bytearray(rom);nes2pal[7]=8;nes2pal[12]=1;put('NES2 PAL.nes',nes2pal)
trainer=bytearray(header);trainer[6]=4;put('Trainer.nes',bytes(trainer)+bytes(512)+rom[16:])
put('Mutable.nes');put('Removed.nes')
(raw/'short.nes').write_bytes(b'NES');(raw/'invalid.nes').write_bytes(b'BAD!'+rom[4:]);(raw/'truncated.nes').write_bytes(rom[:20])
(raw/'unrelated.txt').write_bytes(rom)
deep=raw
for i in range(18):deep=deep/'d';deep.mkdir(exist_ok=True)
(deep/'TooDeep.nes').write_bytes(rom)
link=raw/'loop-link';symlink=link.exists() or link.is_symlink()
try:
 if not symlink:os.symlink(raw,link,target_is_directory=True)
 symlink=True
except OSError:
 if os.name=='nt' and not link.exists():
  command=f"New-Item -ItemType Junction -Path '{link}' -Target '{raw}' | Out-Null"
  p=subprocess.run(['powershell','-NoProfile','-Command',command],capture_output=True)
  symlink=p.returncode==0
ziproot=data/'archives';ziproot.mkdir(exist_ok=True)
with zipfile.ZipFile(ziproot/'mixed.ZIP','w')as z:
 z.writestr('Stored.NES',rom,compress_type=zipfile.ZIP_STORED)
 z.writestr('nested/Compressed (E).nes',rom,compress_type=zipfile.ZIP_DEFLATED)
 z.writestr('nested/ゲーム.nes',rom,compress_type=zipfile.ZIP_DEFLATED)
 z.writestr('Bad.nes',b'BAD!'+rom[4:]);z.writestr('note.txt',b'ignored')
with zipfile.ZipFile(ziproot/'many.zip','w',compression=zipfile.ZIP_DEFLATED)as z:
 for i in range(1105):z.writestr(f'collection/g{i:04d}.nes',rom)
corrupt=data/'corrupt';corrupt.mkdir(exist_ok=True)
with zipfile.ZipFile(corrupt/'BadCRC.zip','w')as z:z.writestr('BadCRC.nes',rom)
p=corrupt/'BadCRC.zip';b=bytearray(p.read_bytes());b[30+len('BadCRC.nes')+99]^=1;p.write_bytes(b)
bad=data/'badarchive';bad.mkdir(exist_ok=True)
with zipfile.ZipFile(bad/'LongMember.zip','w')as z:
 z.writestr('First.nes',rom);z.writestr('x'*1020+'.nes',rom)
(bad/'Truncated.zip').write_bytes(b'PK\x03\x04')
empty=data/'empty';empty.mkdir(exist_ok=True)
prefix=data/'prefix-io';prefix.mkdir(exist_ok=True);(prefix/'Io.zip').write_bytes(b'synthetic I/O fault placeholder')
(root/'FixtureValues.h').write_text(f'#pragma once\nconstexpr unsigned expectedRaw={expected};\nconstexpr unsigned expectedCRC={zlib.crc32(rom)}U;\nconstexpr bool hasSymlink={str(symlink).lower()};\nconstexpr bool hasLongRawFixture={str(os.name != 'nt').lower()};\n')
report={'raw_valid':expected,'rom_size':len(rom),'rom_crc32':f'{zlib.crc32(rom):08x}',
        'zip_valid':1108,'symlink_fixture_created':symlink,'depth_limit_excluded':18,
        'fixture_rom_sha256':hashlib.sha256(rom).hexdigest()}
(root/'fixture-summary.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
