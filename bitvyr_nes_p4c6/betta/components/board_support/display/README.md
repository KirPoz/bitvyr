# Дисплей и PPA

[Display.cpp](Display.cpp) настраивает DSI/DPI/ST7701, physical 480×800
и logical 800×480. Menu/game RGB565 source 256×240 масштабирует/поворачивает PPA;
поля отведены управлению. [Text.cpp](Text.cpp) использует `Font5x7.h`.

LCD/PPA обслуживаются на ядре инициализации. Native/source handles принадлежат
Board. Два native-буфера — обычный режим, третий опциональный с отдельным OOM fallback.

После `beginGamePresent` завершается соответствующий путь.
`acquireCompletedGameSource` освобождает доказанное PPA-чтение; LCD submission
имеет другой срок. Timeout сохраняет заимствование и запрещает запись в source.
Проверки: geometry/fence fixtures и SDK/caps/heap; LCD/tearing/VSYNC — на плате.

`GameBarsDirty.h` содержит Main-only долг прямоугольников левого/правого поля
для каждого из трёх возможных native-буферов. Единственный sidecar создаёт
`Board.cpp`; владелец, 64Б и выравнивание16 одинаковы при OFF/ON опции
`NES_BOARD_PARTIAL_GAME_BARS`. ISR/Worker его не читают. `Text.cpp` отслеживает
фактически записанные отсечённые глифы, а неизвестный формат/перенос строки
сохраняет полный invalidate. `GameChrome` передаёт области кнопок и reserved FPS.

После разрешения записи `prepareFrame` копирует накопленную область с прежним
поворотом270°. Центр игры не затрагивается. Неизвестный owner/index, пустой долг
и пересечение source/output возвращают прежнюю полную скалярную копию; меню
снова делает выбранный native неизвестным. Снимается только долг обновлённого
native. Cache-sync по-прежнему охватывает обе полные панели. Ни готовность PPA,
ни LCD completion этот учёт не создаёт и не заменяет.

[Архитектура](../../../docs/02-architecture.md) · [Сборка и использование](../../../docs/03-guide.md).
