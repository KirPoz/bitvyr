# PPU и пакеты вывода

[TimingPpu](TimingPpu.h) принадлежит CPU-владельцу и ведёт guest raster, Sprite0,
OAM, scroll, NMI и mapper-провода. [VideoPacket](VideoPacket.h) публикуется с
полным ключом и cut; [VideoRenderer](VideoRenderer.h) читает sealed packet и
immutable backing отдельно от живой Machine. Full [Ppu](Ppu.h), цветовой replay
и [PpuRgbOutput](PpuRgbOutput.h) сохраняют отдельные контракты владения.

Объединённые background/sprite/blank интервалы потребляют только уже разрешённые
PPU-точки. Partial groups, CPU/MMIO, NMI/render delay, Sprite0, scroll, mapper
side effects, observer/capture и нехватка packet capacity сохраняют барьеры и
fallback. MMC3 композиция сохраняет A12 age, reload/counter/pending и каждый
возможный IRQ-фронт; совпавшая картинка сама по себе этого не доказывает.

MMC5 typed fetch/raster/phase/span helpers делят данные и цифровые переходы.
CHR/CIRAM/ExRAM views заимствуются на синхронный grant, не продлевают ROM lifetime
и не отменяют проверку mapper policy. Full PPU и Thin primary не подменяют друг
друга: в сигнальном MMC5 настоящие MMIO/STATUS обслуживает полный PPU, поэтому
локальная OAM может не вычисляться только при точном primary owner predicate.
Закрытые native07 ветви для простых mapper 0/7 удалены; MMC5 predicate сохранён.

Прежняя lazy-MMC5 ветвь сейчас не накапливает долг: `tryAppendMmc5Lazy` возвращает
false, а materializer пуст. Совместимые методы, поля и диагностические счётчики
сохранены ради текущего layout; их наличие не означает активный experimental path.

Состав алгоритмов задан исходниками и [CMakeLists.txt](../CMakeLists.txt).
Выбранный OAM terminal-mask путь остаётся PRIVATE с прежним scalar fallback.
Старые имена экспериментальных CMake-флагов не являются текущими options.
Неиспользуемое host-trace API удалено. [c6 codec](c6/README.md) сохраняется:
его исходник действительно входит в отдельный закреплённый worker recipe.

Проверки: PPU/MMIO/OAM/Sprite0/IRQ/NMI fixtures, полные CPU/RAM/index/RGB/PCM
Frame comparisons и raw VideoPacket, когда заявляется byte-exact поток.
Другая допустимая группировка событий не называется byte-exact. Native не
доказывает target placement, стек/heap, физический LCD или слышимое качество.

[Архитектура](../../../../docs/02-architecture.md) · [Сборка](../../../../docs/03-guide.md).
