# Аппаратный адаптер платы

[Board.h](include/board/Board.h) — фасад конкретной ESP32-P4-WIFI6-Touch-LCD-4.3:
[display](display/README.md), [input](input/README.md), [audio](audio/README.md),
[storage](storage/README.md). `BoardInternal.h` скрывает состояние; Core не зависит от BSP.

Main/CPU0 инициализирует периферию и владеет выводом. Конструкторы не запускают
I/O; статусы показывают каждый узел. Handles/кадры принадлежат Board, API указатели
заимствуются до корректного завершения. ISR публикуют completion/counters, не Machine.

PPA source-read и native LCD completion различны. Timeout не возвращает
владение; второй I2C owner не создаётся. ESP-Hosted обслуживает C6 transport.
Проверки host HID/geometry и SDK не заменяют физические LCD/touch/I2S/SD/BLE.

[Архитектура](../../docs/02-architecture.md) · [Сборка и использование](../../docs/03-guide.md).


### Заимствованный NES source A/B

`GameSourceLease.h` не зависит от SDK: два bounded credits и полный immutable
source ticket. Узкие Board register/beginBorrowed/finishBorrowed/unregister APIs
не меняют owned gameFrame и не принимают произвольный третий pointer. A обязан
быть полным PSRAM RGB565, B — прежний Board source; диапазоны disjoint/aligned4.
Требуются triple-native и выключенное Flash encryption. Обычные API вывода не
дренируют чужой borrowed ticket и сохраняют строгий gameFramebuffer gate.

prepareFrame передаёт SDK фактический source. Его pointer/key опубликован до PPA
submission, что покрывает ранний ISR. SDK6.1 сам делает input C2M UNALIGNED;
дополнительной кадровой копии/cache flush нет. После PPA completion прежний latch
различает source-ready и ожидающий LCD/native target; при timeout источник/key
сохраняются. Только точный успешный finishBorrowed снимает lease; Board не free A.
Native fake reader проверяет ownership, но не заменяет SDK/RV32 и аппаратный DMA gate.

`pollBorrowedGamePresent` — отдельный неблокирующий triple-native путь. Pending
сохраняет ticket и native index; PPA token защёлкивается один раз, прежний LCD
token снимается отдельно. Только принятая native submission возвращает source
borrow. Нулевая задержка finishBorrowed не заменяет poll: прежний LCD этап умеет
ожидать80мс. Общий SDK-free автомат в `GamePresentPoll.h` проверяется перестановками
completion и отказом подачи; это не проверка аппаратного VSYNC. Структуры и Board API
общие для OFF/ON экспериментального App-флага, новых задач/буферов нет.

Пока A/B зарегистрированы, обычные gameFramebuffer/presentGame/beginGamePresent и
source-acquire не выдают/не читают B даже в промежутке без PPA: C0 может владеть им
как writer. Отдельное меню пользуется logicalFrame и допустимо после borrowed drain.

### Отложенная копия панелей управления

`NES_BOARD_PARTIAL_GAME_BARS` — отдельная опция компонента с default `OFF`;
её определение `PRIVATE` не меняет поля публичного `Board` или `BoardState`.
Прямоугольный overload `invalidateGameBars(x,y,width,height)` объявлен всегда:
при OFF он сохраняет прежний полный invalidate. После изменения этого заголовка
все его реальные consumers должны пересобираться согласованно.

Main/CPU0 владеет общим sidecar64Б с выравниванием16 в обоих режимах. Он хранит
только owner-key и по две области для трёх native-буферов, не копии кадров.
Включённый путь объединяет долги кнопок, FPS и глифов; копирование выполняется
только внутри прежнего безопасного `prepareFrame`. Меню, неизвестный owner,
пустой долг при требуемом обновлении и alias сохраняют полную скалярную копию.
PPA/LCD fences, два полных cache-sync диапазона панелей и срок ROM/RGB-заимствования
не меняются. Нет новых задач, IRQ, hot allocation и таймеров.

Нативная проверка сравнивает целиком controls, logical и все native-буферы с
независимым прежним painter, включая alias и повторное использование буферов.
Private аппаратная приёмка относится к исходникам того опыта; canonical перенос
требует своей SDK/cohort/HP/stack проверки и не повышает stable/default автоматически.
