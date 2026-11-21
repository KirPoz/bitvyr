# Картридж и mapper

`Cartridge` декодирует iNES и заимствует PRG/CHR/RAM backing.
Реализованные mapper:0,1,2,3,4,5,7,69,85,160; неизвестные варианты/submapper не получают
обещания совместимости. `Legacy160` сохраняет отдельную банковую/latch/IRQ модель.
`Mmc5` отвечает за цифровую проводку, `Mmc5Audio` — expansion-состояние.

Единственный владелец — Machine/Worker. Main не меняет банки и не читает live
состояние во время работы. RAM/CHR/ROM указатели живут до завершения всех
пакетов/consumer; load/reset/rebind инвалидируют зависимые views и версии.
Никаких SDK, задач и копий ROM в пакетах mapper нет.

Peek не заменяет настоящий CPU/PPU strobe. MMC1 serial/RMW, MMC3 A12-фильтр,
MMC5 Name/Attribute/Pattern latch, scanline IRQ и PCM read-mode следуют исходному
порядку. RasterPlan только квалифицирует текущие источники; сам не выдаёт quiet
grant и не пропускает события. Неверный диапазон/режим оставляет прежний fallback.

Native-тесты используют независимые байтовые метки, CIRAM-проводку, защищённые
backing, IRQ/address/read sequences и точные аудиослучаи. Проверка одного ROM
не квалифицирует остальные mapper/игры и не измеряет FPS платы.

## Mapper69/85 и expansion-audio

[Cartridge](Cartridge.h) также содержит цифровую модель FME-7/mapper69 и
VRC7/mapper85: три сменных PRG-окна по8KiB, последнее фиксированное окно,
восемь CHR-окон по1KiB, переключаемые CIRAM A10/A11/0/1 и управляемое окно6000.
Mapper69 выбирает там ROM, RAM либо открытую шину; mapper85 разрешает RAM битом7
E000. Защищённая RAM не принимает записи. CHR-RAM действительно переключается,
а ROM остаётся неизменяемой. Эти две модели принимают только0 или8KiB PRG-RAM/
NVRAM и8KiB CHR-RAM, когда нет CHR-ROM; большие/смешанные неизвестные конфигурации
явно отклоняются, а не усекаются до встроенной арены.

Для mapper85 NES2 submapper1 выбирает регистры поA3 и не имеет FM-генератора;
submapper2 выбирает A4. Неуточнённый submapper0 сохраняет совместимый OR A3/A4.
Прочие submapper85 и ненулевые submapper69 не принимаются. Японский
Lagrange Point с NES2 mapper85/submapper2 является корректным заголовком,
а не случаем «грязного iNES». Допуск определяется форматом и моделью платы,
без имени/CRC игры; прежняя консервативная проверка archaic iNES сохраняется.

FME-7 уменьшает16-bit counter каждый разрешённый CPU M2; IRQ возникает при
0000→FFFF и подтверждается любой записью command13. VRC7 имеет8-bit counter
и cycle/scanline режимы; prescaler341 с шагом3 даёт интервалы114/114/113 CPU M2,
включая VBlank и выключенный rendering. Acknowledge переносит A→E, сохраняя
counter/prescaler. cpuIrqQuietCycles() возвращает только такты строго до
следующего IRQ-фронта. cpuCyclesQuiet() сохраняет counter/phase и не выдаёт
самостоятельного разрешения пропускать CPU/PPU/MMIO; Machine пересекает IRQ
исходным последовательным тактом. statusPollCapabilities() не выдаёт этим
платам свойство NoCpuClockEvent.

[Sunsoft5BAudio](Sunsoft5BAudio.h) владеет тремя tone-каналами, общим17-bit
noise и32-step envelope (56B в текущем RV32-профиле). [Vrc7Audio](Vrc7Audio.h) содержит
собственный ограниченный six-channel/two-operator FM-генератор с15 preset
patches и custom patch (468B в текущем RV32-профиле). Это переносимая аппроксимация FM, не bit-exact
модель реального OPLL. Её36-M2 synthesis period приближает NTSC crystal domain;
подтверждения PAL-тембра/частоты пока нет. E000 bit6 передаётся как sound reset,
а не как удаление CPU-тактов. Оба объекта принадлежат одному Cartridge/Worker,
не создают задач/heap и получают clock ровно один раз через общий M2-путь.

Raw expansionSample() остаётся отдельным reference provider. Signed-Q16 DAC
ограничен диапазонами5B[-36000,0] и VRC7[-49152,0]. Selected provider новых69/85
использует причинное среднее фиксированного64-M2 окна с задержкой до64 M2.
Оно не меняет register/generator/IRQ clocks; waveform/PCM отличается от raw
осознанно. Настоящие MMIO и frame boundaries остаются барьерами. Существующий
MMC5 selected-профиль этим не переопределяется. Пакет/transport/wire limits
по-прежнему проверяются потребителем; event overflow не разрешает терять звук.

Проверки MapperExpansionTests.cpp используют независимые byte labels,
open-bus/RAM gates, NES2 A3/A4 decode, IRQ/quiet границы, serial-versus-batch и
собственные6502 IRQ handlers через Machine. Audio и Frame fixtures проверяют
генераторы, portable64, packets/event spacing и PCM локального AudioRenderer. Native и отдельные
ROM-срезы не являются аппаратной квалификацией всех игр или измерением FPS платы.

Спецификации: [FME-7](https://www.nesdev.org/wiki/Sunsoft_FME-7),
[VRC7](https://www.nesdev.org/wiki/VRC7),
[VRC IRQ](https://www.nesdev.org/wiki/VRC_IRQ),
[NES2 submappers](https://www.nesdev.org/wiki/NES_2.0_submappers).
