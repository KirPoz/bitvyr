# CPU 2A03/2A07

`Cpu6502` исполняет официальный и реализованный неофициальный NMOS-набор.
`OpcodeTable.inl` задаёт декодирование; `CpuAlu.inl` и `QuietImplied.inl` сохраняют
одни арифметические эффекты скалярного и quiet-путей. Decimal-флаг хранится,
но NES не выполняет BCD. Dummy read/write, page crossing, стек, JAM,
IRQ/NMI, OAM/DMC stalls и JMP-indirect поведение остаются наблюдаемыми.

Зависимости — переносимые типы и внутренний bus Machine; SDK/heap/tasks отсутствуют.
`CpuTests` использует независимую scalar bus, включая полный arithmetic/carry
перебор и точные bus sequences. Machine fixtures проверяют DMA/IRQ и active
quiet-путь. Внешний nestest допустим как явно предоставленные данные, не как
код другого эмулятора. Host throughput не является FPS платы.
