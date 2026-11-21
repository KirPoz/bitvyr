# PPU и цветной потребитель

`Ppu` — полная модель, `TimingPpu` — временной producer,
`VideoPacket` — ограниченный упорядоченный префикс,
`VideoRenderer` — независимый цветной consumer. `PpuColourReplay`/`PpuRgbOutput`
сохраняют наблюдаемые цифровые эффекты и выбранный RGB565 output.

Primary timing принадлежит Machine/Worker; consumer читает только опубликованный
пакет и согласованный снимок. Изменяемый primary не является Main live-view.
Пакеты, CIRAM/CHR/ROM и RGB заимствованы; вызывающий сохраняет backing и аренду.
Внешний RGB нельзя перезаписывать до SourceDone PPA, который отличен от native LCD completion.

Tile/span/group kernels сохраняют адресные фронты, /RD, A12/mapper latch,
Sprite0/overflow, PPUDATA/PPUSTATUS и NMI. Объединяются только уже разрешённые
точки до прежнего горизонта. Неизвестная policy, край строки или событие оставляют
скалярный путь. Переполнение пакета — fault, а не frameskip.
Mmc5RasterPlan заимствует текущую mapping policy; in-place span/epoch и raster
interval остаются частью выбранного профиля. Неактивный lazy debt и Thin owner удалены.

Cold backing задаёт адаптер; горячие kernels не выделяют heap и не копируют ROM.
Скалярный reference проверяет register/bus и RGB parity; active fixtures сравнивают
event kernel, packet/capacity, output leases и весь RGB. Геттеры отложенного
вывода могут материализовать собственное состояние; чужой активный PPU читать нельзя.
SDK placement, PPA/LCD timing и физическая плавность этими host-тестами не доказаны.
