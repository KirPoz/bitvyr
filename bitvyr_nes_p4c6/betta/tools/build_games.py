"""Проверяет локальные ZIP read-only и готовит детерминированные HOST копии ROM под build.

Сеть, Flash и содержимое SD не изменяются; игры пользователь предоставляет отдельно."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
import zipfile


def build(source: Path, output: Path) -> dict:
    """Читает ZIP с CRC/размером/iNES проверками и пишет новый raw pack/manifest в явный output."""
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
                name = title.encode("utf-8")
                if len(name) >= 112:
                    raise ValueError(f"Display name too long: {title}")
                crc = zlib.crc32(data) & 0xFFFFFFFF
                # The source labels are a hint, not an authoritative region DB.
                # Aladdin's old pirate label is known to be unreliable; default NTSC.
                pal = "(E)" in title and mapper != 160
                index = len(records)
                extracted = rom_dir / f"{index:02d}.nes"
                extracted.write_bytes(data)
                records.append(dict(index=index, title=title, archive=archive.name,
                                    member=member.filename, header=h.hex(), mapper=mapper,
                                    crc32=f"{crc:08X}", sha256=hashlib.sha256(data).hexdigest(),
                                    size=len(data), pal=pal, region_source="filename hint; user override available",
                                    local_rom=str(extracted.resolve()), _data=data, _name=name))
    if not records:
        raise ValueError("No .nes entries in source ZIP archives")
    if len(records) > 64:
        raise ValueError("Pack is limited to 64 games")
    table_size = len(records) * 160
    start = (32 + table_size + 4095) & ~4095
    pack = bytearray(b"\xff" * start)
    offset = start
    for record in records:
        data, name = record.pop("_data"), record.pop("_name")
        entry = bytearray(160)
        entry[:len(name)] = name
        struct.pack_into("<IIIHBB", entry, 112, offset, len(data), int(record["crc32"], 16),
                         record["mapper"], int(record["pal"]), 0)
        entry[128:160] = bytes.fromhex(record["sha256"])
        pos = 32 + record["index"] * 160
        pack[pos:pos+160] = entry
        pack.extend(data)
        offset += len(data)
        padding = (-offset) & 15
        pack.extend(b"\xff" * padding)
        offset += padding
        record["offset"] = offset - padding - len(data)
    table_crc = zlib.crc32(pack[32:32+table_size]) & 0xFFFFFFFF
    struct.pack_into("<8sIIII", pack, 0, b"NESPK001", len(records), table_size, len(pack), table_crc)
    if len(pack) > 8 * 1024 * 1024:
        raise ValueError("Pack exceeds the 8 MiB ROM partition")
    pack_path = output / "nes-games.bin"
    pack_path.write_bytes(pack)
    manifest = dict(format="NESPK001", count=len(records), pack_bytes=len(pack),
                    pack_sha256=hashlib.sha256(pack).hexdigest(), source=str(source.resolve()),
                    games=records)
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    return manifest


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root.parent / "Games")
    parser.add_argument("--output", type=Path, default=root / "build" / "games")
    args = parser.parse_args()
    result = build(args.source, args.output)
    print(json.dumps({k:v for k,v in result.items() if k != "games"}, ensure_ascii=False, indent=2))
