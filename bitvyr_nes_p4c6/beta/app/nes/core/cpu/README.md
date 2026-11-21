# CPU 6502

[Cpu6502](Cpu6502.h) исполняет инструкции и сохраняет read/write/dummy обращения,
IRQ/NMI sampling, DMA/DMC и флаги CPU. [OpcodeTable.inl](OpcodeTable.inl) задаёт
декодирование, [CpuAlu.inl](CpuAlu.inl) — общие операции над регистрами и флагами.

[CpuQuietBlock.cpp](CpuQuietBlock.cpp) выполняет последовательность инструкций
внутри уже выданного Machine горизонта. Generic, banked WRAM и MMC5 используют
разные проверенные политики. `RomWindow` — временный immutable view текущей
PRG-страницы; это не кеш скомпилированного кода. Банки, page cross, MMIO,
наблюдатели, IRQ/DMC и нехватка бюджета сохраняют прежний fallback.

Prepared tail использует только остаток того же grant. MMC5 сохраняет real M2,
mapper/APU/PPU порядок и admission полного owned Frame; read-fold не разрешает
откладывать влияющие записи или менять открытый bus. STATUS poll folding требует
проверенных байтов петли и pure STATUS-контракта, сохраняет CPU flags/регистры,
реальные границы чтений, IRQ/NMI и точный конечный cut. ROM/PC исключений нет.

Действующий состав задаёт [CMakeLists.txt](../CMakeLists.txt). Выбранная generic
аффинная политика остаётся PRIVATE; public headers/layout и scalar fallback
сохраняются. Прежняя матрица экспериментальных CMake-флагов удалена.

Проверки: CPU/quiet/MMIO/mapper fixtures и полные Frame CPU/RAM/index/RGB/PCM
сравнения. Native elapsed не является RV32 cycle/FPS измерением.

[Архитектура](../../../../docs/02-architecture.md) · [Сборка](../../../../docs/03-guide.md).
