# Геометрия и оформление

`MenuLayout.h`, `GameControls.h`, `PauseMenu.h` объединяют геометрию рисования
и hit-test. `GameChrome.h` рисует кнопки/стрелки и подсветку принятой маски.
`PresentationRate.h` считает software-submit, а не физический VSYNC.

Header helpers не читают SDK/SD. Main использует 800×480; игровое поле 640×480
окружено панелями управления с отдельным RGB565-хромом. Физическую ориентацию
и PPA обслуживает Board. Геометрия сама не загружает ROM и не меняет Machine.

Framebuffer заимствуется. Row-renderer не подставляет вымышленный canonical
index кадр. Проверки: MenuLayout/GameControls/TouchOverlap/PresentationRate;
ориентация, читаемость и LCD подтверждаются на устройстве.

[Архитектура](../../../docs/02-architecture.md) · [Сборка и использование](../../../docs/03-guide.md).

`GameChrome` публикует dirty-область отдельно для каждой изменившейся кнопки и
для всего reserved FPS-прямоугольника. BoardLike предоставляет полный и
прямоугольный overload `invalidateGameBars`; прямоугольник задаётся в логических
пикселях800×480. Реальное хранение долга, отсечение, fallback и безопасное
копирование принадлежат Board/display; painter не знает адреса native-буфера.

Опция Board `NES_BOARD_PARTIAL_GAME_BARS` по умолчанию OFF. В этом режиме новый
overload выполняет полный invalidate и сохраняет прежний вид UI. Тест
`GameControlsTests` использует тот же painter, проверяет границы и пиксели;
его BoardFake принимает прямоугольный API полным invalidate без имитации PPA.
