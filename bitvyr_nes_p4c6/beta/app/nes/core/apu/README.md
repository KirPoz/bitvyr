# APU и аудиопакеты

[Apu](Apu.h) моделирует pulse/triangle/noise/DMC, frame sequencer и IRQ NTSC/PAL.
[TimingApu](TimingApu.h) записывает регистры, DMC и expansion-переходы в
[AudioPacket](AudioPacket.h). [AudioRenderer](AudioRenderer.h) потребляет sealed
пакет отдельно от живой Machine. I2S/DMA/кодек находятся вне Core.

`ApuBus::dmcRead` возвращает настоящий байт; `dmcStall` учитывает украденные такты.
Callback не исполняет CPU повторно и не начисляет уже прошедшее время второй раз.
Неполный пакет, несовпавшие identity/cut и частичный PCM не публикуются.

Изолированный expansion DAC-фронт может пройти через `trySetExpansionInputQuiet`
только при доказанном quiet deadline и отсутствии совпавшего native/DMC/MMIO
события. Отказ сохраняет обычный setter/tick. Guest clock не движется вперёд ради
этого упрощения; целочисленная дельта поступает в прежний FIR.

`Apu::depositWideDelta` берёт коэффициенты FIR для заданной фазы. Каждый
коэффициент умножается на переход DAC в 64-битной арифметике и добавляется
в очередь импульсов с текущей позиции. Команда формирует вклад в будущие
отсчёты, а их выдачей управляет рендерер.

Проекция `apu_fixed` входит в C6 worker. Формат общего пакета задаёт
[Core CMake](../CMakeLists.txt). Согласованность исходников проверяет
[рецепт C6](../../../../coprocessor/c6_audio/README.md).

Проверки: APU/expansion/frame-audio fixtures, точный PCM и DMC/IRQ/MMIO timing.
Непрерывность I2S и слышимое качество проверяются на плате отдельно.

[Архитектура](../../../../docs/02-architecture.md) · [Сборка](../../../../docs/03-guide.md).
