"""Проверяет локальные ZIP read-only и готовит детерминированные HOST копии ROM под build.

Сеть, Flash и содержимое SD не изменяются; игры пользователь предоставляет отдельно."""
import argparse
import hashlib
import json
from pathlib import Path
import zlib
import zipfile


def build(source: Path, output: Path) -> dict:
    """Читает ZIP с CRC/размером/iNES проверками и пишет HOST-копии ROM с manifest в явный output."""
    output.mkdir(parents=True, exist_ok=True)
    rom_dir = output / "roms"
    rom_dir.mkdir(exist_ok=True)
    records = []
    for archive in sorted(source.glob("*.zip"), key=lambda p: p.name.casefold()):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.infolist():
                if member.is_dir() or not member.filename.lower().endswith(".nes"):
                    continue
                if member.file_size > 8 * 1024 * 1024:
                    raise ValueError(f"Oversize ROM: {archive.name}")
                data = bundle.read(member)  # ZipFile checks entry CRC, including decompression.
                if len(data) < 16 or data[:4] != b"NES\x1a":
                    raise ValueError(f"Not an iNES image: {archive.name}")
                h = data[:16]
                nes2 = h[7] & 12 == 8
                mapper = (h[6] >> 4) | (h[7] & 240) | ((h[8] & 15) << 8 if nes2 else 0)
                title = Path(member.filename).stem.replace("_", " ")
                crc = zlib.crc32(data) & 0xFFFFFFFF
                # Имя файла даёт лишь подсказку региона; старый PAL-label mapper160 ненадёжен.
                pal = "(E)" in title and mapper != 160
                index = len(records)
                extracted = rom_dir / f"{index:02d}.nes"
                extracted.write_bytes(data)
                records.append(dict(index=index, title=title, archive=archive.name,
                                    member=member.filename, header=h.hex(), mapper=mapper,
                                    crc32=f"{crc:08X}", sha256=hashlib.sha256(data).hexdigest(),
                                    size=len(data), pal=pal, region_source="filename hint; user override available",
                                    local_rom=str(extracted.resolve())))
    if not records:
        raise ValueError("No .nes entries in source ZIP archives")
    # У HOST-манифеста нет таблицы Flash-раздела или общего лимита размера ROM pack.
    manifest = dict(format="local_rom_manifest_v1", count=len(records),
                    source=str(source.resolve()), games=records)
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root.parent / "Games", help="Каталог пользовательских ZIP с ROM; только чтение")
    parser.add_argument("--output", type=Path, default=root / "build" / "games", help="HOST-копии roms/*.nes и manifest.json; образ Flash не создаётся")
    args = parser.parse_args()
    result = build(args.source, args.output)
    print(json.dumps({k:v for k,v in result.items() if k != "games"}, ensure_ascii=False, indent=2))
