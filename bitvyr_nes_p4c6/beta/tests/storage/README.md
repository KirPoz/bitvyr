# Проверки SD-каталога и ZIP

[CMakeLists.txt](CMakeLists.txt) собирает настоящие GameLibrary/ZipArchive без
ESP_PLATFORM. [fixtures.py](fixtures.py) создаёт synthetic ZIP/каталог;
[zip_reference.py](zip_reference.py) — независимый zlib-эталон. SDK/ROM/SD/UART не нужны.

Проверяются Store/Deflate, CRC/размеры, усечения/повреждения, prefix I/O,
каталог/поколения и ограничения обхода. Каждый fixture владеет FILE/DIR/heap
и освобождает их; ошибка не даёт частичный ROM.

Набор входит в общий CTest и имеет standalone entry. Выходы создаются в выбранном
`build/native` каталоге. Результат не подменяет аппаратную FAT32/SDMMC проверку.

`ZipArchive::archiveCrc` сверяет исходные байты файла, не только CRC распакованной
ROM: проверяются пустой файл, известный вектор и ошибки открытия. Диагностика
ограничена64MiB, не изменяет архив и не выделяет ROM-буфер. Для проверки частного
набора можно передать `zip_reference.py --corpus-directory ПУТЬ` либо
`--archive-list СПИСОК`; файлы источника читаются, временные копии и результаты
создаются только в указанном `--output`. Python `zipfile`/`binascii` независимо
проверяют каждый распакованный байт, префикс и CRC всех байтов архива.

[Архитектура](../../docs/02-architecture.md) · [Сборка и использование](../../docs/03-guide.md).
