#pragma once

#include <cstdint>
#include <cstddef>
#include "GameSourceLease.h"
#include "GamePresentPoll.h"

namespace nes::platform {

// Заданная геометрия изделия, не результат опроса подключённой панели.
/**
 * @brief Геометрия изделия, заданная проектом; не результат опроса подключённой панели.
 */
struct PanelGeometry final {
    std::uint16_t width;
    std::uint16_t height;
};

/**
 * @brief Раздельные результаты запуска узлов и стартовая оценка памяти; Board хранит запись, отсутствие одного узла не означает готовность другого.
 */
struct BoardStatus final {
    bool psram{false};
    bool display{false};
    bool touch{false};
    bool storage{false};
    bool audio{false};
    std::size_t freePsram{0};
};

/**
 * @brief Один контакт в логическом ландшафтном пространстве 800×480; ID0..15 сохраняется контроллером при удержании.
 */
struct TouchPoint final {
    std::uint16_t x{0};
    std::uint16_t y{0};
    bool pressed{false};
    std::uint8_t id{0}; // ID контакта GT911, 0..15, устойчивый во время удержания.
};

/**
 * @brief Ограниченный снимок не более пяти контактов; count=0 допустим при достоверном отсутствии нажатий.
 */
struct TouchContacts final {
    static constexpr unsigned capacity=5;
    TouchPoint points[capacity]{};
    std::uint8_t count{0};
};

/**
 * @brief Раздельные диагностические счётчики Board/I2S. Принятые кадры не равны завершённым DMA; пропущенные пополнения не измеряют все аудиопровалы, uint32 может переполниться.
 */
struct AudioStats final {
    bool active{false};
    std::uint32_t acceptedFrames{0};
    std::uint32_t dmaFrames{0};
    std::uint32_t missedRefillBlocks{0};
    std::uint32_t missedRefillFrames{0};
    std::uint32_t maxWriteGapUs{0};
    std::uint32_t maxWriteWaitUs{0};
};

/**
 * @brief Внутренний владелец дескрипторов периферии и долговечных буферов платы. Main владеет выводом; ISR используют ограниченные атомарные поля/семафоры, фоновая задача сенсора — только свой снимок.
 */
struct BoardState;

// Завершение чтения источника не подаёт LCD и не освобождает сохранённый native-output.
/**
 * @brief Результат получения только свободного RGB-источника; Ready не подаёт LCD и не освобождает сохранённый native-output.
 */
enum class GameSourceAcquireResult:std::uint8_t {Ready,Unsupported,Timeout,InvalidState};

/**
 * @brief Фасад собственной поддержки платы Waveshare P4. Конструктор не делает I/O; ресурсы BoardState и кадров живут до перезагрузки, вызовы вывода принадлежат ядру инициализации, копирование запрещено.
 */
class Board final {
public:
    /**
     * @brief Создаёт пустой объект без аппаратного ввода-вывода.
     * @details Ресурсы ещё не выделены; запуск выполняется явным initialize().
     */
    Board() noexcept = default;
    /**
     * @brief Запрещает копирование владельца аппаратных ресурсов.
     * @details Конструктор удалён; один объект сохраняет стабильное владение ресурсами и callback-контекстом.
     */
    Board(const Board&) = delete;
    /**
     * @brief Запрещает присваивание владельца аппаратных ресурсов.
     * @details Операция удалена: дескрипторы и долговечные буферы нельзя неявно передавать другому объекту.
     * @return Операция недоступна при компиляции.
     */
    Board& operator=(const Board&) = delete;

    static constexpr PanelGeometry nativePanel{480, 800};
    static constexpr PanelGeometry logicalPanel{800, 480};
    static constexpr unsigned audioSampleRate = 48000;

    // Узлы инициализируются независимо; отсутствие SD-карты не фатально.
    /**
     * @brief Однократно создаёт состояние платы и запускает периферию.
     * @details BoardState размещается во внутренней RAM на время работы прошивки. Отсутствие SD или другого узла отражается отдельно в BoardStatus; повторный вызов возвращает сохранённый результат.
     * @return Ссылка на собственный BoardStatus; вызывающий код не освобождает её.
     */
    [[nodiscard]] const BoardStatus& initialize() noexcept;
    /**
     * @brief Возвращает сохранённые результаты инициализации периферии.
     * @details Не опрашивает оборудование и не обновляет текущую свободную память; ссылка принадлежит Board.
     * @return Ссылка на BoardStatus с временем жизни объекта.
     */
    [[nodiscard]] const BoardStatus& status() const noexcept { return status_; }
    // Только из меню: перед remount вызывающий код закрывает все SD-файлы.
    /**
     * @brief Повторно монтирует microSD после закрытия файлов приложением.
     * @details Допустим только из меню без заимствованных SD-файлов. Сначала снимает FATFS и освобождает LDO; ошибка очистки сохраняет оставшийся ресурс и прерывает повторное подключение.
     * @return true при успешном новом монтировании, иначе false.
     */
    [[nodiscard]] bool refreshStorage() noexcept;
    /**
     * @brief Предоставляет изменяемый кадр меню 800×480 в RGB565.
     * @details Буфер принадлежит плате; владение выводом и завершение предыдущей операции обеспечивает вызывающий код.
     * @return Указатель на кадр либо nullptr до успешного выделения.
     */
    [[nodiscard]] std::uint16_t* framebuffer() noexcept;
    // RGB565-источник NES256×240. PPA масштабирует до телевизионного аспекта 640×480,
    // затем поворачивает в центральную область 480×640 физического LCD.
    /**
     * @brief Предоставляет игровой RGB565-источник 256×240 только при свободном владельце.
     * @details Пока gamePresentPending установлен, DMA/PPA или отложенный вывод сохраняет заимствование. Тайм-аут не разрешает перезапись; требуется настоящий finishGamePresent.
     * @return Изменяемый указатель либо nullptr при неготовности или незавершённом выводе.
     */
    [[nodiscard]] std::uint16_t* gameFramebuffer() noexcept;
    /**
     * @brief На Main до кадра регистрирует A вместе с owned B=gameFramebuffer, не передавая владение.
     * @param source Полный caller-owned A RGB565, живущий до unregister и reader drain.
     * @return false при pending, encryption, invalid range/caps или несовместимом native кольце.
     */
    bool registerGameSource(const std::uint16_t*source)noexcept;
    /** @brief После полного drain снимает регистрацию A; B остаётся собственностью Board. */
    bool unregisterGameSource(const std::uint16_t*source)noexcept;
    /**
     * @brief Заимствует ровно зарегистрированный A/B с immutable frame/time ticket.
     * @param ticket Проверенный App результат; pointer/slot/key удерживаются до истинного finish.
     * @return true только после принятого PPA; false не передаёт source аппаратному reader.
     */
    bool beginBorrowedGamePresent(const GameSourceTicket&ticket)noexcept;
    /**
     * @brief Завершает ровно pending ticket; PPA-ready не подменяет LCD/native completion.
     * @param ticket Полный ключ исходного begin, не текущий mutable RGB writer.
     * @param timeoutMs Граница ожидания одной операции, не разрешение освободить источник.
     * @return false сохраняет ticket/источник для позднего drain; true снимает borrow.
     */
    bool finishBorrowedGamePresent(const GameSourceTicket&ticket,unsigned timeoutMs=80)noexcept;
    // Панели шириной 80 пикселей упакованы с шагом 160 и не разделяют пиксели меню.
    /**
     * @brief Предоставляет отдельные боковые панели управления.
     * @details Две полосы 80×480 упакованы рядом со шагом 160; буфер не содержит меню. После изменения следует пометить физические поля недействительными.
     * @return Указатель на собственный буфер платы либо nullptr.
     */
    [[nodiscard]] std::uint16_t* gameControlsFramebuffer() noexcept;
    // Координаты логические 800×480; центральные пиксели отсекаются.
    /**
     * @brief Рисует текст только в боковых игровых панелях.
     * @details Использует логические координаты 800×480, отсекает центр и отмечает физические поля для последующего обновления. Не запускает PPA.
     * @param x Логическая горизонтальная координата в пикселях.
     * @param y Логическая вертикальная координата в пикселях.
     * @param text Заимствованная строка с завершающим нулём; указатель не сохраняется.
     * @param color Цвет RGB565 без передачи владения буфером.
     * @param scale Целый масштаб шрифта 1..8; вне диапазона рисование не выполняется.
     */
    void drawGameText(int x,int y,const char* text,std::uint16_t color=0xffff,unsigned scale=2) noexcept;
    /**
     * @brief Синхронно выводит текущий игровой кадр через PPA.
     * @details Завершает предыдущее заимствование, начинает новое и дожидается его окончания на ядре инициализации дисплея.
     * @return true при успехе всех этапов, иначе false без разрешения игнорировать активный DMA.
     */
    [[nodiscard]] bool presentGame() noexcept;
    // Только CPU0. Источник/выход остаются заимствованными до реального PPA-завершения.
    // Каждый успешный begin завершается finish, даже при ошибке DSP этого кадра.
    /**
     * @brief Начинает асинхронное чтение игрового источника PPA.
     * @details CPU0 заимствует исходный и выбранный физический буферы. Каждый успешный begin должен получить настоящий finish, в том числе при последующей ошибке DSP.
     * @return true при принятой PPA-транзакции; false при ошибке или занятом источнике.
     */
    [[nodiscard]] bool beginGamePresent() noexcept;
    /**
     * @brief Завершает ранее начатый игровой вывод.
     * @details Ожидает реальное завершение PPA и требуемую границу LCD; тайм-аут оставляет незавершённый ресурс заимствованным. Поддерживает позднее завершение.
     * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
     * @return true при успешном завершении либо отсутствии pending-операции; false при ошибке.
     */
    [[nodiscard]] bool finishGamePresent(unsigned timeoutMs = 80) noexcept;
    /** @brief Проверяет наличие трёх native-буферов. Двухбуферный вывод сохраняет finish. */
    [[nodiscard]] bool supportsDirectGamePresentPoll() const noexcept;
    /**
     * @brief Продвигает начатый direct-вывод по настоящим PPA/LCD-токенам без ожидания.
     * @details Pending/Fault сохраняют источник. Для borrowed используется отдельный вызов с ключом.
     */
    [[nodiscard]] GamePresentPollResult pollGamePresent() noexcept;
    // Ландшафтный RGB565-кадр поворачивается в ограждённый native DPI-буфер.
    /**
     * @brief Выводит кадр меню с поворотом в физический буфер LCD.
     * @details Перед записью меню ограждает более старый игровой DMA. Вызывается на том же ядре, что initializeDisplay.
     * @return true при успешных PPA и подаче LCD, иначе false.
     */
    [[nodiscard]] bool present() noexcept;
    /**
     * @brief Помечает все выделенные физические боковые поля устаревшими.
     * @details Следующий безопасный вывод скопирует новые панели управления; текущая развёртка здесь не перезаписывается.
     */
    void invalidateGameBars() noexcept;
    /**
     * @brief Помечает логическую область боковых панелей без записи native-буферов.
     * @details Main-only; OFF сохраняет полный invalidate, ON объединяет clipped debt каждого native.
     * @param x,y Логическое начало в пикселях800×480.
     * @param width,height Размер области; неположительные значения и центр в ON не меняют пиксели.
     */
    void invalidateGameBars(int x,int y,int width,int height) noexcept;
    /**
     * @brief Заливает логический кадр меню одним RGB565-цветом.
     * @details Не запускает вывод; также помечает игровые боковые поля для обновления при возвращении в игру.
     * @param color Цвет RGB565 без передачи владения буфером.
     */
    void clear(std::uint16_t color = 0) noexcept;
    /**
     * @brief Рисует текст собственным растровым шрифтом в кадре меню.
     * @details Масштаб 1..8, отсечение по границам 800×480; буфер и ресурс вывода остаются у платы.
     * @param x Логическая горизонтальная координата в пикселях.
     * @param y Логическая вертикальная координата в пикселях.
     * @param text Заимствованная строка с завершающим нулём; указатель не сохраняется.
     * @param color Цвет RGB565 без передачи владения буфером.
     * @param scale Целый масштаб шрифта 1..8; вне диапазона рисование не выполняется.
     */
    void drawText(int x, int y, const char* text, std::uint16_t color = 0xFFFF,
                  unsigned scale = 2) noexcept;
    /**
     * @brief Читает первый контакт GT911 в логических координатах.
     * @details Результат предварительно обнуляется; отсутствие касания может быть успешным чтением с pressed=false.
     * @param point Выходная точка первого контакта либо обнулённое отсутствие нажатия.
     * @return true при достоверном опросе, false при ошибке или недоступном сенсоре.
     */
    [[nodiscard]] bool pollTouch(TouchPoint& point) noexcept;
    /**
     * @brief Читает до пяти достоверных контактов GT911.
     * @details Обнуляет выход перед обращением к сенсору; синхронный и фоновый режимы выбираются при сборке.
     * @param contacts Выходной ограниченный снимок контактов; при ошибке сбрасывается.
     * @return true при достоверном снимке, включая count=0; false при ошибке/устаревании.
     */
    [[nodiscard]] bool pollTouchContacts(TouchContacts& contacts) noexcept;
    // Моно 48 кГц, знаковый 16-битный PCM; возвращает число отсчётов, принятых I2S.
    /**
     * @brief Передаёт PCM16 mono48 кГц в два одинаковых I2S-слота.
     * @details Буфер вызывающего кода читается синхронно блоками не более 256 отсчётов. При ошибке/тайм-ауте возвращает фактически принятый префикс; принятие не равно завершению DMA или гарантии непрерывного звука.
     * @param samples Заимствованный массив знаковых 16-битных моноотсчётов 48 кГц.
     * @param count Количество элементов/отсчётов/байтов по контракту вызываемой функции.
     * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
     * @return Число принятых моноотсчётов 0..count.
     */
    [[nodiscard]] std::size_t writeAudio(const std::int16_t* samples, std::size_t count,
                                       unsigned timeoutMs = 25) noexcept;
    /**
     * @brief Применяет ограниченную громкость кодека без положительного усиления DAC.
     * @details Сначала запрашивает выключение PA; при ошибке повторно запрашивает тихий режим, не доказывая фактический уровень GPIO. Окно статистики возобновляется лишь при запрошенном воспроизведении и ненулевом уровне.
     * @param percent Желаемый уровень 0..100; значения выше 100 ограничиваются.
     * @return true при успешной настройке; false при отсутствии узла или аппаратной ошибке.
     */
    [[nodiscard]] bool setVolume(unsigned percent) noexcept;
    /**
     * @brief Возвращает последний успешно установленный уровень громкости.
     * @details Значение 0..100 хранится в BoardState; функция не читает регистры кодека.
     * @return Сохранённый процент либо 0 до инициализации.
     */
    [[nodiscard]] unsigned volume() const noexcept;
    /**
     * @brief Вычисляет ёмкость успешно смонтированной карты в байтах.
     * @details Использует CSD и размер сектора существующего sdCard, без чтения новых секторов.
     * @return 64-битная ёмкость либо 0 при недоступной карте.
     */
    [[nodiscard]] std::uint64_t storageCapacity() const noexcept;
    // true запускает новое диагностическое окно; false исключает меню/mute-тишину.
    // Начальная льгота 40 мс; uint32-счётчики переполняются в длительных сеансах.
    /**
     * @brief Задаёт намерение воспроизведения и отдельное окно аудиостатистики.
     * @details Отключает учёт перед аппаратным изменением PA/DAC; true с успешным ненулевым уровнем начинает новое окно с 40 мс льготы. Громкость пользователя этим методом не заменяется.
     * @param active Запрошенная активность воспроизведения/диагностики.
     */
    void setAudioActive(bool active) noexcept;
    /**
     * @brief Считывает подлинные счётчики Board/I2S текущего окна.
     * @details Поля читаются раздельно и могут отражать близкие моменты ISR; это не атомарный снимок всего тракта. uint32-счётчики переполняются. Пропуск пополнения не учитывает все возможные пропавшие отсчёты.
     * @return Копия счётчиков либо нулевые значения до инициализации.
     */
    [[nodiscard]] AudioStats audioStats() const noexcept;
    /**
     * @brief Возвращает постоянную точку монтирования microSD.
     * @details Строка статическая; освобождать или изменять её нельзя.
     * @return Строка /sdcard.
     */
    [[nodiscard]] static constexpr const char* storagePath() noexcept { return "/sdcard"; }
    /**
     * @brief Выводит модель платы и сохранённую готовность узлов.
     * @details Не запускает повторную инициализацию и не доказывает физическое качество изображения или звука.
     */
    void logConfiguration() const noexcept;

    // Только Main; обычный gameFramebuffer остаётся строгим. При Ready разрешена
    // запись RGB-источника; native-output, индекс и pending LCD-идентичность удерживаются.
    /**
     * @brief Разрешает повторную запись RGB после реального чтения PPA.
     * @details Только владелец дисплея: успешное получение источника не подаёт старый LCD-кадр и не освобождает его физический индекс. Обычный gameFramebuffer остаётся строгим.
     * @param source Выходной указатель: nullptr до успеха, при Ready разрешена запись только RGB-источника.
     * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
     * @return Ready/Unsupported/Timeout/InvalidState; при неуспехе source=nullptr.
     */
    [[nodiscard]] GameSourceAcquireResult acquireCompletedGameSource(std::uint16_t*&source,unsigned timeoutMs=80)noexcept;
private:
    BoardState* state_{nullptr};
    BoardStatus status_{};
};

}  // пространство имён nes::platform
