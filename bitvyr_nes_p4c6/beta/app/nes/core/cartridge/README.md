# Картридж и мапперы

[Cartridge](Cartridge.h) валидирует ROM/NES2, выбирает mapper и связывает PRG/CHR
страницы. ROM заимствуется у App; RAM и банковые окна принадлежат картриджу.
Копирование запрещено из-за идентичности страниц и аренд. Смена банков и загрузка
инвалидируют views/эпохи. Неизвестная карта не подменяется другим mapper.

Mirroring, IRQ, CHR dirty, MMC5 RD/ExRAM/split зависят от настоящего порядка
обращений. Аудиодолг закрывается до влияющей записи или наблюдения. Guest IRQ
и PCM-стробы не откладываются произвольно. Loader проверяет размер RAM/NVRAM,
backing и поддерживаемый submapper до публикации картриджа.

MMC5 разделён на [Mmc5](Mmc5.h), [Mmc5Audio](Mmc5Audio.h) и CartridgeMmc5.
[Mmc5RasterPlan](Mmc5RasterPlan.h) заимствует CHR/CIRAM/ExRAM и подготовленную
policy только для синхронного grant. Успешная подготовка сама по себе не даёт
quiet-сертификата. Typed tile helpers сохраняют Name/attribute, physical offset,
банки и защёлки; caller отдельно материализует исходные цифровые переходы.

Raw MMC5 audio `sample/levels/status` остаётся цифровым эталоном. Portable
selected provider отдельно усредняет разрешённые carrier-фронты; PCM, frame,
envelope, IRQ и MMIO остаются барьерами. Его результат не является raw DAC.
[Sunsoft5BAudio](Sunsoft5BAudio.h) и [Vrc7Audio](Vrc7Audio.h) имеют отдельные
CPU-clocked модели. [Vrc7WriteTape](Vrc7WriteTape.h) связывает immutable writes
с точным output cut; частичная либо повреждённая tape не публикуется.

Состав алгоритмов задан исходниками и [CMakeLists.txt](../CMakeLists.txt).
Прежние имена экспериментальных флагов не являются текущими CMake options.
Проверки: loader/mapper/audio/fetch fixtures, банковые границы, IRQ/MMIO,
alias/dirty и реальные ROM-сцены. Новый ROM не квалифицируется одним unit test.

[Архитектура](../../../../docs/02-architecture.md) · [Сборка](../../../../docs/03-guide.md).
