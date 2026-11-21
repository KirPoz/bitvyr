"""Создаёт lossless PNG сетку из явно заданных HOST P6 снимков.

Порядок задаёт results.json, рядом записываются координаты ячеек; imaging dependency не нужна."""
from pathlib import Path
import argparse
import json
import struct
import zlib


def read_ppm(path):
    """Проверяет P6/255 header и точную RGB длину; неизвестный формат отвергает."""
    raw = path.read_bytes()
    header, separator, pixels = raw.partition(b'\n255\n')
    fields = header.split()
    if not separator or len(fields) != 3 or fields[0] != b'P6':
        raise ValueError(f'Unsupported PPM header: {path}')
    width, height = map(int, fields[1:])
    if len(pixels) != width * height * 3:
        raise ValueError(f'Invalid pixel length: {path}')
    return width, height, pixels


def png_chunk(kind, data):
    """Формирует один PNG chunk с big-endian длиной и CRC типа/данных."""
    return (struct.pack('>I', len(data)) + kind + data
            + struct.pack('>I', zlib.crc32(kind + data)))


def main():
    """Читает готовые снимки и manifest, пишет сетку и координаты; firmware/ROM не выполняет."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--columns', type=int, default=5)
    parser.add_argument('--screen', choices=['game', 'title'], default='game')
    args = parser.parse_args()
    if args.columns < 1:
        parser.error('--columns must be positive')
    results = json.loads((args.directory / 'results.json').read_text('utf-8'))['results']
    images = [(item, read_ppm(args.directory / f"{item['index']:02}-{args.screen}.ppm"))
              for item in results]
    cell_width, cell_height = images[0][1][:2]
    width = args.columns * cell_width
    height = ((len(images) + args.columns - 1) // args.columns) * cell_height
    pixels = bytearray(width * height * 3)
    cells = []
    for position, (item, (source_width, source_height, source)) in enumerate(images):
        if (source_width, source_height) != (cell_width, cell_height):
            raise ValueError('Screenshots must have equal dimensions')
        x = position % args.columns * cell_width
        y = position // args.columns * cell_height
        for row in range(cell_height):
            offset = ((y + row) * width + x) * 3
            pixels[offset:offset + cell_width * 3] = source[row * cell_width * 3:(row + 1) * cell_width * 3]
        cells.append({'index': item['index'], 'name': item['name'], 'x': x, 'y': y})
    scanlines = b''.join(b'\0' + pixels[row * width * 3:(row + 1) * width * 3] for row in range(height))
    png = (b'\x89PNG\r\n\x1a\n'
           + png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
           + png_chunk(b'IDAT', zlib.compress(scanlines, 9))
           + png_chunk(b'IEND', b''))
    output = args.directory / f'{args.screen}-grid.png'
    output.write_bytes(png)
    output.with_suffix('.json').write_text(json.dumps(cells, indent=2) + '\n', 'utf-8')
    print(output)


if __name__ == '__main__':
    main()
