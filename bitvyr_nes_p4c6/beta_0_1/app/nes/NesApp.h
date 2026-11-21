#line 1 "app/nes/NesApp.h"
/**
 * @file NesApp.h
 * @brief Координация консоли: библиотека, настройки, кадры, владение Worker и безопасный UART.
 */
#pragma once

#include "Machine.h"
#include "board/Board.h"
#include "board/BluetoothInput.h"
#include "storage/GameLibrary.h"
#include "settings/SettingsStore.h"
#include "graphics/PresentationRate.h"

namespace nes::app {

/** @brief Координатор жизненного цикла консоли на Main. Владеет Board, библиотекой, настройками и первичной Machine; Worker заимствует её поля до настоящего завершения. */
class NesApp final {
public:
    /**
     * @brief Создаёт долгоживущий координатор консоли без запуска задач. Копирование запрещено: поля заимствуются Worker и аппаратным выводом.
     */
    NesApp() noexcept = default;
    /**
     * @brief Создаёт долгоживущий координатор консоли без запуска задач. Копирование запрещено: поля заимствуются Worker и аппаратным выводом.
     */
    NesApp(const NesApp&) = delete;
    /**
     * @brief Запрещает копирующее присваивание долгоживущего владельца ресурсов; существующие Worker/ROM-заимствования нельзя дублировать.
     */
    NesApp& operator=(const NesApp&) = delete;

    /**
     * @brief Инициализирует плату, настройки, SD и Worker, затем обслуживает основной цикл. Вывод, меню и команды выполняются Main; ошибки владельца переводят выполнение в Faulted.
     */
    void run() noexcept;

private:
#if defined(NES_APP_FRAME_NATIVE_PROOF) && NES_APP_FRAME_NATIVE_PROOF
    // Только native proof: реальный координатор проверяется через явный probe.
    friend struct SplitProbe;
#endif
    /**
     * @brief Перерисовывает библиотеку игр и элементы навигации на Main. Индекс выбирается из текущего поколения GameLibrary.
     */
    void menu() noexcept;
    /**
     * @brief Joined Main связывает boot-long C6 Batch backend до первого guest start.
     * @return false при profile/key/HP/cold-transport отказе; неизвестные owners остаются живыми.
     * @details LOCAL ничего не создаёт. Первая интеграция сохраняет one-ROM/refusal
     * до отдельного подтверждённого UNBIND/rebind adapter; golden требует optional qualification.
     */
    bool configureC6AudioBackend()noexcept;
    /**
     * @brief Выполняет один Main-only C6 step и bounded I2S timeout0 без чтения primary.
     * @param draining true лишь после actual Worker join для выдачи последнего PCM_N.
     * @return false сохраняет packet/publication/transport/PCM owners до joined recovery/reset.
     * @details Всегда проверяет whole PCM key/count/phase; optional qualification добавляет CRC/golden.
     */
    bool pumpC6AudioBackend(bool draining)noexcept;
    /** @brief Main cooperative pump без lifecycle fault; reason читается только после возврата consumer. */
    bool tryPumpC6AudioBackend(bool draining,const char*&reason)noexcept;
    /**
     * @brief После настоящего Worker join доводит remote PCM/source owners до known drain.
     * @return false при незавершённом Worker/transport либо deadline, без free/смены ROM.
     * @details Timeout и SourceDone не подменяют service idle, PCM acceptance или true join.
     */
    bool drainC6AudioBackend()noexcept;
    /** @brief Joined Main доводит общий C6 owner до exact UNBIND ACK перед любой сменой ROM. */
    bool releaseC6AudioForLoad()noexcept;
    /** @brief Печатает один общий C6 witness; Frame передаёт нулевые Thin-stage counters. */
    void reportC6AudioWindow(std::uint64_t n,std::uint64_t stages=0,std::uint64_t stageUs=0,
        std::uint64_t stageMax=0,std::uint64_t heldEntries=0,std::uint64_t heldExits=0,
        std::uint64_t heldUs=0,std::uint64_t heldMax=0,std::uint64_t clockErrors=0)noexcept;
    /**
     * @brief Рисует отдельный экран SOUND/CONTROLLER/VIDEO/SYSTEM с текущими значениями. Не исполняет гостевой кадр и не сохраняет NVS при каждой отрисовке.
     */
    void settingsMenu() noexcept;
    /**
     * @brief Приостанавливает игру на подтверждённой границе и открывает настройки. Удержанные кнопки подавляются до отпускания, чтобы переход не стал игровым нажатием.
     */
    void openSettings() noexcept;
    /**
     * @brief Применяет и при необходимости сохраняет настройки, затем возвращает предыдущий экран/игру. Ошибка сохранения остаётся диагностируемой.
     */
    void closeSettings() noexcept;
    /**
     * @brief Обрабатывает фронты кнопок навигации и повторы в настройках. Игровая маска не передаётся CPU во время настройки.
     * @param pressed Маска фронтов NES-кнопок текущего шага интерфейса.
     */
    void settingsControls(std::uint8_t pressed) noexcept;
    /**
     * @brief Меняет выбранное значение в допустимых пределах и отмечает необходимость сохранения. Громкость применяется через Board, регион — на границе перезапуска игры.
     * @param direction Знак изменения выбранного параметра: уменьшение либо увеличение.
     */
    void changeSetting(int direction) noexcept;
    /**
     * @brief Передаёт сохранённую громкость и mute аудиоадаптеру. Возвращает успешность аппаратного применения, не повышая громкость при отказе.
     */
    bool applyAudioSettings() noexcept;
    /**
     * @brief Печатает фактические настройки, активный регион, паузу и dirty-флаг в UART. Вывод не заменяет проверку физического звука.
     */
    void settingsStatus() const noexcept;
    /**
     * @brief Очищает прежний экран меню и формирует боковые сенсорные панели. Центральная игровая область остаётся640×480 без заполнения всего экрана.
     */
    void prepareGameDisplay() noexcept;
    /**
     * @brief Обновляет FPS только в зарезервированной боковой области. Использует последнюю завершённую метрику, не читает занятую первичную Machine.
     */
    void drawFpsCounter() noexcept;
    /** @brief Проверяет наличие игрового видеотракта; CPU-only benchmark не получает ложный FPS/HUD. Читает только контекст Main. */
    bool hasGameVideo() const noexcept;
    /** @brief Обновляет Main-счётчик успешных выводов по настенным микросекундам; не читает Machine и не запускает PPA. */
    void updateGameOverlay() noexcept;
    /** @brief Приостанавливает завершённый игровой тракт и открывает быстрое меню либо подтверждение выхода. @param confirmExit Открыть подтверждение выхода с безопасным выбором отмены. */
    void openPause(bool confirmExit=false) noexcept;
    /** @brief Рисует быстрое меню паузы/подтверждение поверх отдельного UI framebuffer после завершения владельцев. */
    void pauseMenu() noexcept;
    /** @brief Обрабатывает навигацию и касания паузы; удержанный ввод не переносится между экранами. @param pressed Маска новых/повторных навигационных NES-кнопок. */
    void pauseControls(std::uint8_t pressed) noexcept;
    /** @brief Выполняет выбранный пункт паузы; выход/перезапуск требуют отдельного подтверждения. */
    void activatePauseSelection() noexcept;
    /**
     * @brief Начинает новое окно измерений и сбрасывает его накопители. Гостевые часы и состояние игры не обнуляются.
     */
    void resetPerformanceWindow() noexcept;
    /**
     * @brief Перезапускает активную ROM после безопасного завершения владельцев. Старые снимки/ввод инвалидируются новым поколением.
     */
    void resetGame() noexcept;
    /**
     * @brief Ожидает ближайший срок кадра, сохраняя гостевую скорость выбранного региона. Задержка относится к host-планировщику, а не к CPU-циклам NES.
     */
    void paceFrame() noexcept;
    /**
     * @brief Продвигает срок следующего кадра по гостевым циклам с переносом дробного остатка. Это исключает накопление ошибки округления микросекунд.
     */
    void advanceFrameDeadline() noexcept;
    /**
     * @brief Повторно индексирует SD и обновляет выбор игры. Старые GameInfo после смены поколения больше не разрешают чтение.
     */
    void rescanLibrary() noexcept;
    /**
     * @brief Печатает ограниченную страницу каталога в UART. Имена экранируются; диапазон ограничен фактическим числом записей.
     * @param offset Нулевой индекс первой записи выдаваемой страницы каталога.
     * @param limit Верхняя граница числа выдаваемых записей.
     */
    void catalog(std::size_t offset, std::size_t limit) const noexcept;
    /**
     * @brief Сверяет фактические байты выбранного файла SD без загрузки или изменения ROM.
     * @param index Нулевой индекс живого каталога; команда разрешена только в меню после завершения Worker.
     * @details Выводит CRC32 всего ZIP/.nes и число прочитанных байтов для отличения повреждения файла от ошибки распаковщика.
     */
    void archiveCheck(std::size_t index) const noexcept;
    /**
     * @brief Загружает выбранную распакованную или ZIP-ROM во временную память и связывает новое поколение исполнения. При отказе не публикует частично созданный контекст Worker.
     * @param index Нулевой индекс записи текущего поколения каталога.
     */
    bool startGame(std::size_t index) noexcept;
    /**
     * @brief Фиксирует ошибку Legacy-цветного тракта и прекращает небезопасное продолжение. Заимствованные данные освобождаются только после установленного завершения.
     * @param reason Заимствованное диагностическое описание отказа.
     */
    bool legacyVideoFailure(const char* reason) noexcept;
    /**
     * @brief Возвращает адаптер выделения Legacy-пикселей в PSRAM. Функции освобождения применяются только к собственной, уже не заимствованной памяти.
     */
    static core::LegacyPixelAllocator legacyPixelAllocator() noexcept;
    /**
     * @brief Собирает полные ограниченные строки UART и передаёт их диспетчеру. Переполнение отбрасывает строку, а не исполняет её усечённый префикс.
     */
    void serialCommands() noexcept;

    /**
     * @brief Разбирает одну законченную команду после retire текущего задания. Команды статуса, ввода, меню и диагностики не обходят карантин Worker.
     * @param line Заимствованная завершённая UART-строка; функция не принимает владение памятью.
     */
    void command(const char* line) noexcept;
    /**
     * @brief Сбрасывает сборку незаконченной UART-строки и её overflow-флаг. Гостевой ввод и очередь pad-событий меняются отдельно.
     */
    void clearSerialInput() noexcept;
    /**
     * @brief Проверяет и ставит ограниченное событие serial-pad на заданный гостевой кадр. Неверный формат и переполнение очереди не меняют игровое управление.
     * @param arguments Остаток UART-команды с десятичным номером кадра и маской.
     */
    void schedulePad(const char* arguments) noexcept;
    /**
     * @brief Применяет подошедшие события к маске UART на точной кадровой границе. Будущие события сохраняются, обработанные удаляются из ограниченной очереди.
     */
    void applyScheduledPads() noexcept;
    /**
     * @brief Печатает аппаратный и игровой статус из разрешённых снимков. При параллельном кадре использует принятые CPU/RAM/PCM/RGB, а не живые поля Worker.
     */
    void status() noexcept;
    /**
     * @brief Выполняет шаг Worker-тракта: ввод, retire/dispatch, цвет, звук и вывод после проверки владельца.
     */
    void frame() noexcept;
    struct EmulationWorkerState;
    /** @brief Режим исполнения: синхронный, задача CPU1 либо терминальный Faulted с удержанием неизвестных ресурсов. */
    enum class ExecutionMode : std::uint8_t { Synchronous, Worker, Faulted };
    /**
     * @brief Проверяет готовность диагностического CPU-only владельца. Успех не означает наличие изображения или игровой FPS.
     */
    bool videoWorkerReady() const noexcept;
    /**
     * @brief Настраивает observable CPU/PPU и аудио-пакеты на присоединённой границе. Отказ не выдаётся за допуск к обычной игре.
     */
    bool configureVideoWorker() noexcept;
    /**
     * @brief Печатает CPU-only throughput и контрольные состояния с явным отсутствием игрового FPS/изображения.
     */
    void videoWorkerStatus() noexcept;
    /**
     * @brief Возвращает номер входного задания Frame-тракта из его принятого состояния. Не материализует изменяемый PPU.
     */
    std::uint64_t framePipelineInputFrame()const noexcept;
    /**
     * @brief Возвращает принятый гостевой CPU-cycle Frame-тракта для ввода и отчётов. Значение не является host-таймером.
     */
    std::uint64_t framePipelineCycle()const noexcept;
    /**
     * @brief Создаёт двухслотовый Frame-контекст и связывает ROM, регион, поколение, DSP и RGB. Все заимствованные ресурсы проверяются до допуска Worker.
     */
    bool configureFrameVideoPipeline()noexcept;
    /**
     * @brief Завершает владельцев и освобождает старый Frame-контекст перед новой ROM. При неизвестном Worker/PPA возвращает отказ и удерживает память.
     */
    bool releaseFramePipelineForLoad()noexcept;
    /**
     * @brief Получает настоящее завершение производителя и потребляет предыдущий Frame-slot. Ошибки lease/sequence/cut переводят тракт в отказ.
     */
    bool retireFramePipelineFrame()noexcept;
    /**
     * @brief На Main воспроизводит цвет и DSP одного опубликованного кадра, проверяет совместный результат и принимает слот. Прежний PPA-source должен быть завершён до записи RGB.
     * @param publish Разрешение аппаратного вывода; false используется для безопасного завершения без новой публикации.
     */
    bool consumeFramePipelineFrame(bool publish)noexcept;
    /**
     * @brief Завершает прежнюю асинхронную передачу Frame-изображения и обновляет счётчики только по подтверждённому результату Board.
     */
    bool finishFramePipelinePresent()noexcept;
    /**
     * @brief Публикует принятый кадр Frame-тракта в PPA/LCD. Заимствование RGB сохраняется до соответствующего аппаратного завершения.
     */
    bool presentFramePipelineAsync()noexcept;
    /**
     * @brief Печатает принятые CPU/RAM/PCM/RGB/read/input-срезы и раздельные produced/accepted/submitted. Не объявляет физический VSYNC или качество всей игры.
     */
    void frameVideoStatus()noexcept;
    /**
     * @brief Печатает накопленные120-кадровые метрики Frame и сбрасывает только окно измерений. Перекрывающиеся времена не складываются в FPS.
     */
    void reportFrameVideo()noexcept;
    /**
     * @brief Создаёт отдельный диагностический Coupled-контекст на разрешённой границе. Его область поддержки и результаты не распространяются на весь каталог.
     */
    bool configureCoupledVideoPipeline() noexcept;
    /**
     * @brief Завершает и уничтожает прежний Coupled-контекст только после настоящего join и остановки Main-потребителя.
     */
    bool releaseCoupledForLoad() noexcept;
    /**
     * @brief Получает завершённый сигнальный кадр Coupled и проверяет его совместный цветной результат. Не подменяет обычный Frame-тракт.
     */
    bool retireCoupledFrame() noexcept;
    /**
     * @brief Выдаёт подтверждённый Coupled RGB в Board и PCM в I2S. Срок жизни источника включает незавершённую аппаратную передачу.
     */
    void presentCoupledFrame() noexcept;
    /**
     * @brief Завершает прежний асинхронный Coupled PPA/LCD до переиспользования буфера.
     */
    bool finishCoupledPresent() noexcept;
    /**
     * @brief Возвращает Main-only source credit после подтверждённой Board submission.
     * @return true для текущего generation/frame; false сохраняет неосвобождённый
     * credit при отказе completeSource и не создаёт ложного submitted счётчика.
     * @pre Board уже подтвердил конец чтения PPA и принятую подачу того же native output.
     */
    bool completeCoupledPresent() noexcept;
    /**
     * @brief Запускает асинхронный вывод принятого Coupled-кадра и сохраняет идентичность незавершённой передачи.
     */
    void presentCoupledFrameAsync() noexcept;
    /**
     * @brief Печатает диагностический Coupled-срез и область его допустимости; CPU throughput не превращается в игровую приёмку.
     */
    void coupledVideoStatus() noexcept;
    /**
     * @brief Выдаёт окно времён Coupled-производителя, Main-потребителя, DSP и вывода без суммирования перекрывающихся интервалов.
     */
    void reportCoupledVideo() noexcept;
    /**
     * @brief Создаёт INTERNAL control, PSRAM snapshot, семафоры и задачу CPU1. Частичный отказ освобождает только ещё никем не заимствованные ресурсы.
     */
    bool initializeWorker() noexcept;
    /**
     * @brief Создаёт аудио-пакеты и Main DSP для раздельных владельцев. Допускается только при присоединённом Worker.
     */
    bool initializeSplitAudio() noexcept;
    /**
     * @brief Доводит опубликованное аудио до принятого состояния Main и готовит PCM. Нельзя запускать его повторно на занятом/чужом пакете.
     */
    bool prepareSnapshotAudio() noexcept;
    /**
     * @brief Фиксирует ошибку аудио-владения и останавливает продолжение. Возвращает false для распространения отказа по вызывающей цепочке.
     * @param reason Заимствованное диагностическое описание отказа.
     */
    bool splitAudioFault(const char* reason) noexcept;
    /**
     * @brief Создаёт пакеты и теневой цветной видеоконтекст. Память и wire-привязки проверяются до включения split-режима.
     */
    bool initializeSplitVideo() noexcept;
    /**
     * @brief Включает либо выключает раздельное видео на безопасной границе. Не переключает владельца посреди активного Worker-задания.
     * @param enabled Запрашиваемое состояние режима на присоединённой границе.
     */
    bool setSplitVideoMode(bool enabled) noexcept;
    /**
     * @brief Воспроизводит опубликованный видеопакет либо продвигает его состояние без вывода. Поколение и последовательность проверяются до доступа к цвету.
     * @param rgb Разрешение цветного вывода при воспроизведении пакета.
     */
    bool prepareSnapshotVideo(bool rgb) noexcept;
    /**
     * @brief Фиксирует отказ split-video и удерживает неопределённые заимствования вместо небезопасного освобождения.
     * @param reason Заимствованное диагностическое описание отказа.
     */
    bool splitVideoFault(const char* reason) noexcept;
    /**
     * @brief Точка входа задачи CPU1. Принимает ровно одно задание start, исполняет выбранного владельца и публикует ровно один настоящий done; Board и меню здесь не вызываются.
     * @param context Долгоживущий EmulationWorkerState, переданный FreeRTOS при создании задачи.
     */
    static void workerMain(void* context) noexcept;
    /**
     * @brief Завершает активное задание и обрабатывает принятый результат на Main. Ветви Faulted/диагностического заимствования не считают тайм-аут доказательством park.
     */
    bool retireFrame() noexcept;
    /**
     * @brief Публикует ввод/поколение и запускает Worker в свободный слот. Отказ не разрешает Main обращаться к занятой первичной Machine.
     * @param inputUs Измеренное Main-время подготовки ввода, в микросекундах.
     */
    bool dispatchFrame(std::int64_t inputUs) noexcept;
    /**
     * @brief Выводит принятый снимок изображения и PCM на Main. Источник живёт до завершения PPA и не читается непосредственно у занятого Worker.
     */
    void presentSnapshot() noexcept;
    /**
     * @brief Переводит исполнение в Faulted и отключает опасное продолжение. Не удаляет ресурсы, которыми ещё может владеть CPU1.
     * @param reason Заимствованное диагностическое описание отказа.
     */
    void workerFault(const char* reason) noexcept;
    /**
     * @brief Инвалидирует старый принятый снимок на присоединённой границе паузы/перезапуска. Переносит поколение, сохраняя уже прошедшую историю DSP.
     */
    void invalidateSnapshot() noexcept;
    /**
     * @brief Останавливает игру и возвращает интерфейс в библиотеку. Сначала учитывает настоящих владельцев; неизвестный Worker сохраняет ROM и контекст.
     */
    void stopPlayback() noexcept;
    /**
     * @brief Сбрасывает кадровые/диагностические счётчики при переходе сессии. Не является заменой аппаратного завершения.
     */
    void resetCounters() noexcept;
    /**
     * @brief Печатает законченные окна обычного тракта, включая звук/вывод/ожидание. Числа относятся к конкретной конфигурации и сцене.
     */
    void reportPerformance() noexcept;
    platform::Board board_{};
    platform::BluetoothInput gamepad_{};
    GameLibrary library_{};
    ConsoleSettings settings_{};
    SettingsStore settingsStore_{};
    core::Machine machine_{legacyPixelAllocator()};
    GameInfo current_{};
    std::size_t selected_ = 0;
    /** @brief Текущий экран интерфейса; отделён от playing/pausedGame и режима владельца. */
    enum class Screen : std::uint8_t { Library, Settings, Pause };
    Screen screen_ = Screen::Library;
    bool playing_ = false, menuDirty_ = true, touchHeld_ = false;
    bool pausedGame_ = false, settingsDirty_ = false, pairingHelp_ = false;
    /** @brief Подтверждаемое действие быстрого меню; None означает обычные четыре пункта. */
    enum class PauseAction : std::uint8_t { None, Exit, Restart };
    PauseAction pauseAction_=PauseAction::None;
    unsigned pauseRow_=0;
    bool settingsReturnToPause_=false;
    bool volumeDrag_=false;
    std::uint8_t chromeButtons_=0;
    PresentationRate presentationRate_{};
    bool currentPalHint_ = false;
    unsigned settingsTab_ = 0, settingsRow_ = 0;
    std::uint8_t repeatButtons_ = 0;
    std::uint8_t suppressedGamepad_ = 0;
    bool suppressTouchUntilRelease_ = false;
    std::uint16_t previousTouchIds_ = 0;
    std::int64_t repeatAtUs_ = 0;
    double lastFps_ = 0;
    std::int64_t lastAverageCoreUs_ = 0;
    std::int64_t nextFrameUs_ = 0;
    std::uint64_t guestCycleMark_ = 0, paceRemainder_ = 0;
    std::uint8_t previousButtons_ = 0, serialButtons_ = 0;
    /** @brief Одно запланированное изменение UART-маски на точном гостевом кадре; очередь ограничена maxPadEvents. */
    struct PadEvent { std::uint64_t frame; std::uint8_t mask; };
    static constexpr unsigned maxPadEvents = 16;
    PadEvent padEvents_[maxPadEvents]{};
    unsigned padEventCount_ = 0;
    char serialLine_[96]{};
    unsigned serialLength_ = 0;
    bool serialOverflow_ = false;
    std::uint64_t frames_ = 0, audioShort_ = 0, displayErrors_ = 0;
    std::int64_t periodStart_ = 0, coreUs_ = 0, renderUs_ = 0, maxCoreUs_ = 0;
    std::int64_t inputUs_ = 0, audioUs_ = 0, presentUs_ = 0;
    std::uint64_t periodFrames_ = 0;
    std::uint16_t palette_[8][64]{};
    /** @brief Готовит единственный внешний MMC5-контекст до load после join; PSRAM живёт до перезапуска App. */
    bool prepareCartridgeSession(std::uint16_t nextMapper) noexcept;
    void* mmc5Session_ = nullptr;
    std::uint8_t* activeRom_ = nullptr;
    EmulationWorkerState* worker_ = nullptr;
    ExecutionMode executionMode_ = ExecutionMode::Synchronous;
    std::uint64_t sessionGeneration_ = 0;
    std::int64_t workerWaitUs_ = 0, snapshotUs_ = 0;
    std::uint64_t submittedFrames_ = 0;
};

}  // namespace nes::app

