# Нативные Core/HID проверки

[CMakeLists.txt](CMakeLists.txt) собирает настоящий Core без ESP-IDF.
Текущий фиксированный профиль проверяет CPU, PPU/APU, DMA, frame audio,
mirroring, mapper/MMC5, HID и UI. Assertions включены также в Release;
результаты остаются внутри `build/native`.

`BitvyrTestProfile.h` передаёт consumer metadata общего layout. PRIVATE Core
политики не нужно определять повторно в тестах: CTest передаёт ожидаемое
включение аргументами executable. Нельзя смешивать objects разных профилей.
Fake-bus случаи ограничены generic-ветвями самих fixtures и не означают
поддержку второго профиля прошивки.

`GamePresentPollTests.cpp` проверяет тот же SDK-free автомат, который вызывает
Display: перестановки готовности PPA/LCD, однократное получение токенов,
удержание source при Pending/отказе и retry с сохранённым PPA latch.
Это не доказательство аппаратных callback/cache или физического VSYNC.

`nes_run` нужен для deterministic input и native preview. `nes_test_roms`
исполняет внешний test-ROM по его протоколу; отсутствие signature или timeout
не считается успехом. ROM не входят в CTest и не скачиваются автоматически.
Происхождение/CRC и ограничения отдельного прогона сохраняются вместе с ним.

## Событийные и mapper-регрессии

`MachineEventKernelTests.cpp` сравнивает PPU-срезы и удерживаемые MMC5
интервалы с независимым поточечным/обычным CPU-исполнителем. В фиксированном
CMake target подключён постоянно. CTest задаёт `1 1`, `--raster-plan 1`,
`--raster-interval 1` и `--tail-slots 1`; необязательный positional path
сохраняет canonical stream. Проверяются цифровое состояние, порядок событий,
чистые отказы, RGB/PCM и фактическая активация выбранных путей.

Тот же fixture содержит 350 MMC3 Output случаев объединения PPU-долга.
Reference сохраняет прежние prefix/rest вызовы и промежуточный refresh.
Сравниваются cursor/IRQ/budget, весь packet arena, legacy credits,
capacity0/1, sticky fault/Sealed и следующее MMIO.

`MachineMmc5ReadFoldTests.cpp` использует собственные LDA/BNE ROM: packed
A/NZ/base/open, отказ до peek, ROM window/lease, WRAM/view, IRQ/NMI/DMA/DMC
и будущие MMIO. CTest передаёт ожидание0; это не разрешение включать закрытый
эксперимент в рабочем профиле. Общие immutable seed и цифровые/RGB/PCM
проверки сохраняются без отдельных RUN-копий fixtures.

`cartridge_expansion_audio_exact32` связан с frozen64 реализациями из
[reference64](reference64/README.md). Это независимый широкий арифметический
reference для текущего audio, а не лишняя копия production-провайдера.
Expansion mapper/Frame/fastcap/WRAM fixtures сохраняют собственные scalar
сравнения и проверки IRQ, DMC, raw events, RGB/PCM и wire-границ.

## Портативный native triangle

`ApuTriangleMeanTests.cpp` входит в обычный фиксированный CTest-набор.
Аргументы `1 1` задают PRIVATE Mean и expansion-quiet ожидания данными.
Проверяются nonlinear mean32rawphases/held DAC, настоящий $400A/B переход,
sample-before-deposit, frame/DMC/reset и quiet-expansion границы,
moment-хвост15/16/17 samples, period/length/linear и AudioRenderer0/1.
Rank4 carry и отрицательный kernel-control ограничены соответствующей ветвью.

Reference проходит literal1M2 на прежних clock/DMC/FIR primitives и
независимо усредняет 32 raw-фазы после нелинейного DAC. Он не вызывает новую
mean eligibility или deposit-policy. Сравниваются DMC trace, logical cut,
framing и каждый PCM. Общие DAC/FIR/emit primitives делают это регрессией
расписания/политики, а не независимым эталоном всех аналоговых формул.

Host-время, hardware FPS, physical input, LCD и акустическое качество
этими тестами не квалифицируются. [Архитектура](../../docs/02-architecture.md)
и [руководство](../../docs/03-guide.md) описывают отдельные аппаратные гейты.
