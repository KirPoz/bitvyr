# Портативное ядро Bitvyr

Собственный C++17 Core содержит CPU6502, cartridge/mappers, FullPPU, TimingPpu,
TimingApu и пакеты видео/аудио. ESP-IDF/FreeRTOS/Board headers отсутствуют. ROM
заимствуется у App, выполняющая Machine принадлежит одному Worker.

Beta задаёт один проверяемый состав алгоритмов в исходниках: direct bus/quiet
CPU, Timing Output для non5, ThinSignals для MMC5 и точный Main consumer.
CMake не предоставляет матрицу экспериментальных вариантов. Два PUBLIC constants
сохранены лишь для single-source C6 AudioPacket/renderer headers и их wire/layout.

Любое объединение M2/PPU работы сохраняет read/write/dummy strobes, IRQ/NMI,
DMA/DMC и guest-visible PPUSTATUS/PPUDATA. Оpaque pointer или completed flag
не даёт Main права читать live Machine. Пакеты содержат full identity и точный cut.
RomWindow — временная immutable8192B physical PRG identity, не executable cache.

В owned Frame чтение картриджа 69/85 по $4020 и выше не материализует PPU,
если отсутствуют Thin, VideoObserver и оба capture-наблюдателя. К моменту
memoryReadIo настоящий read-M2, DMA/DMC и цифровые IRQ уже исполнены readSlow;
сам Cartridge::cpuRead и последующее обновление open bus сохраняются.
PPU-бюджет пересчитывается, а долг погашается на следующей настоящей границе.
Это разрешение не распространяется на чтение PPU/APU/контроллера или mapper5.

Количество и группировка внутренних VideoOp могут меняться при объединении
интервалов. Гостевое состояние, наблюдаемые MMIO/IRQ/NMI, каждый index/RGB пиксель
и звуковые данные проверяются отдельно от побайтовой формы VideoPacket. Другой
raw-поток нельзя объявлять byte-exact; observer/capture сохраняют прежние барьеры.

AudioRenderer здесь остаётся портативным эталоном/consumer для native PCM tests;
product C6 audio использует отдельный явно сохранённый fixed APU projection.
Общие типы не копируются между ELF. Публичные wire bytes не определяются native ABI.

Конструкторы Machine связывают CPU, полный и тайминговый PPU/APU с шиной
одной машины. Политику выделения пикселей полного PPU можно передать при
создании, а ROM и пакеты привязываются позже. `advancePpu` потребляет число
уже прошедших guest-точек; принимающая сторона обрабатывает NMI на предусмотренной
границе. `outputWritableResult` проверяет владельца и границы назначения,
затем исключает пересечение с машиной, ROM, пикселями, пакетами, снимками и
областями звукового рендера.

HOST suite проверяет Machine/mapper/MMIO/CPU-RAM, RGB/PCM, Output leases и CHIP
границы. Native не является RV32 ABI/stack/IRAM/heap или hardware FPS проверкой.
