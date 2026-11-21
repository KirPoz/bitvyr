/**
 * @file Ppu.h
 * @brief Шина PPU, растровое состояние и политика владения индексным изображением.
 * @ingroup nes_ppu
 */
#pragma once

#include <array>
#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <memory>

// Опытный точный пакет полного sprite-slot использует только существующую
// чистую direct-шину и прежний допуск CHR view. По умолчанию OFF.

namespace nes::core {

/**
 * @brief Единственный владелец гостевых часов, карты памяти, CPU/PPU/APU и состояния картриджа. ROM заимствован; ввод, хранение, задачи и физический вывод остаются вне ядра.
 * @ingroup nes_ppu
 */
class Machine;
/** @brief MMC5-владелец живого detector/data состояния, заимствованный только на span. */
class Mmc5;
/** @brief Подготовленные неизменные NT/CHR базы текущего MMC5 span без владения памятью. */
class Mmc5RasterPlan;
/** @brief Span-local не владеющий view RGB-pairs/row; срок жизни ограничен одним вызовом. */
struct PpuBackgroundWriter;
/**
 * @brief Контекст заимствованных RGB565/Pair-буферов для точного выбранного PPU. Привязка и teardown требуют исключения активного использования.
 * @ingroup nes_ppu
 */
class PpuRgbOutput;
/**
 * @brief Парные noexcept allocate/release callbacks для индексных пикселей. Политика не зависит от SDK; caller сохраняет допустимость callback на весь срок PixelStorage.
 * @ingroup nes_ppu
 */
struct LegacyPixelAllocator {
    using Allocate = std::uint8_t* (*)(std::size_t) noexcept;
    using Release = void (*)(std::uint8_t*) noexcept;
    Allocate allocate = nullptr;
    Release release = nullptr;
};

// Адаптер cartridge/nametable. Смена адреса и прохождение PPU-точек
// отдельны от чтений: MMC3 фильтрует A12, не считая чтения временем.
/**
 * @brief Назначение реального внешнего чтения PPU; адресный drive и время остаются отдельными сигналами.
 * @details Mapper может выбирать разные CHR/NT данные для фона, спрайтов и CPU. DummyName не начинает фоновый тайл.
 */
enum class PpuFetchKind : std::uint8_t {
    Cpu, BackgroundName, BackgroundAttribute, BackgroundPattern, SpritePattern, DummyName
};

/**
 * @brief Контракт обращений PPU к CHR, таблицам имён, палитре и адресным/тактовым сигналам mapper.
 * @ingroup nes_ppu
 */
class PpuBus {
public:
    /**
     * @brief Завершает время жизни объекта с полиморфным либо владеющим контрактом.
     * @details Связанные внешние bus/ROM/буферы должны сохраняться до окончания всех активных пользователей.
     */
    virtual ~PpuBus() = default;
    /**
     * @brief Читает байт адресного пространства PPU с нужным выбором памяти.
     * @details Паттерны CHR, таблицы имён и палитра имеют разные правила зеркалирования и mapper-эффектов; чтение не заменяет отдельный адресный строб.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    virtual std::uint8_t ppuRead(std::uint16_t address) noexcept = 0;
    /**
     * @brief Читает один реальный байт с назначением текущей растровой выборки.
     * @param address Исходный адрес внешнего PPU-чтения; вызов не добавляет адресных или временных сигналов.
     * @param kind Вид выборки; low/high остаются независимыми чтениями по своим адресам.
     * @param tileX Фоновая колонка будущего пиксельного тайла: видимые fetch дают 2..33, prefetch — 0/1; для остальных видов не используется.
     * @return Байт текущего отображения; default вызывает старый ppuRead ровно один раз, сохраняя прежние buses.
     */
    virtual std::uint8_t ppuReadFetch(std::uint16_t address, PpuFetchKind kind, unsigned tileX) noexcept {
        static_cast<void>(kind); static_cast<void>(tileX);
        return ppuRead(address);
    }
    /**
     * @brief Записывает байт в отображённую память PPU.
     * @details Сохраняет зеркалирование, ограничения CHR-ROM и замораживание уже заимствованных CHR-RAM-значений.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    virtual void ppuWrite(std::uint16_t address, std::uint8_t value) noexcept = 0;
    /**
     * @brief Публикует текущий адрес на шине PPU.
     * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
     */
    virtual void ppuAddress(std::uint16_t) noexcept {}
    /**
     * @brief Продвигает временное состояние картриджа на одну точку PPU.
     * @details Сохраняет возраст A12 и фильтрацию фронтов MMC3, даже когда видеоданные не выводятся.
     */
    virtual void ppuCycle() noexcept {}
};

/**
 * @brief Полный растровый PPU: конвейер, OAM, Sprite0/overflow, индексное изображение и необязательный RGB-выход. Шина заимствована, пиксели управляются выбранной политикой хранения.
 * @ingroup nes_ppu
 */
class Ppu final {
public:
    using Bus = Machine;
    static constexpr unsigned width = 256;
    static constexpr unsigned height = 240;

    /**
     * @brief Создаёт PPU, связанный с указанной шиной.
     * @details Срок жизни шины должен покрывать весь срок жизни PPU. Копия не получает автоматически RGB-контекст или полномочия основного временного владельца.
     * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
     */
    explicit Ppu(Bus& bus) noexcept : Ppu(bus, LegacyPixelAllocator{}) {}
    /**
     * @brief Создаёт PPU, связанный с указанной шиной.
     * @details Срок жизни шины должен покрывать весь срок жизни PPU. Копия не получает автоматически RGB-контекст или полномочия основного временного владельца.
     * @param allocator Политика allocate/free для индексных пикселей; callback-ресурсы должны оставаться действительными.
     * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
     */
    Ppu(Bus& bus, LegacyPixelAllocator allocator) noexcept : bus_(bus), pixels_(allocator) { reset(); }
    /**
     * @brief Создаёт PPU, связанный с указанной шиной.
     * @details Срок жизни шины должен покрывать весь срок жизни PPU. Копия не получает автоматически RGB-контекст или полномочия основного временного владельца.
     * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
     */
    Ppu(const Ppu&) noexcept = default;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    Ppu& operator=(const Ppu&) = delete;
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    Ppu(Ppu&&) = delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    Ppu& operator=(Ppu&&) = delete;
    /**
     * @brief Возвращает состояние или результат текущей привязки.
     * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool pixelsReady() const noexcept { return pixels_.data() != nullptr; }
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
     */
    void reset(bool pal = false) noexcept;
    // Самостоятельные шины по умолчанию наблюдают все сигналы. Machine может отключить
    // mapper-линии, не подключённые к текущему картриджу.
    /**
     * @brief Выбирает требуемое тактование и выставление адресов текущего PPU.
     * @details Флаги определяются режимом mapper; отключение ненужного провода не разрешает терять цветовые защёлки.
     * @param cycles Наличие подключённого наблюдателя PPU-тактов; не количество тактов.
     * @param addresses Наличие подключённого наблюдателя адресных линий; не число обращений.
     */
    void setBusTracking(bool cycles, bool addresses) noexcept { trackBusCycles_ = cycles; trackBusAddresses_ = addresses; }
    /**
     * @brief Продвигает устройство на одну его логическую единицу времени.
     * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
     */
    void tick() noexcept;
    // Продвинуть непрерывный PPU-интервал; CPU/MMIO-барьеры планирует вызывающий.
    /**
     * @brief Продвигает PPU на указанное число уже прошедших точек.
     * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    void tickDots(unsigned dots) noexcept;
    // Отложенное время целиком до видимой dot249 и без NMI-фронта.
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned deferredPrefix() const noexcept { return (1U - dot_) & 7U; }
    /**
     * @brief Проверяет низкий уровень A12 для точной текущей фоновой фазы.
     * @details Сохранённый адрес и возможный перенос +8 важнее простого предположения о выбранной таблице паттернов.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool backgroundA12Low() const noexcept { return (control_ & 0x10) == 0; }
    // Нет fetch/pixel-работы, адресные линии стабильны, нет pending edge/delay.
    // Закончить до vblank dot1 или pre-render-перехода последней холостой точки.
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned nonVisibleQuietDots() const noexcept {
        const int last = pal_ ? 310 : 260;
        if (scanline_ < 240 || scanline_ > last || (renderDelay_ | nmiDelay_) ||
            pendingPixels_ || coarseXSignal_ || scrollSignal_ || busAddress_ != (v_ & 0x3FFF)) return 0;
        unsigned distance = 255;
        if (scanline_ == 240) distance = 342U - dot_;
        else if (scanline_ == 241 && dot_ < 2) distance = 1U - dot_;
        else if (scanline_ == last) distance = 340U - dot_;
        return distance < 255 ? distance : 255;
    }
    // При стабильных CPU-регистрах внутри нет нового NMI/frame-ready-фронта.
    // Status/evaluation/scroll и odd-frame-изменения всё равно исполняются
    // при материализации; CPU-доступ к регистрам всегда предварительно синхронизируется.
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned quietDots() const noexcept {
        if (renderDelay_ | nmiDelay_) return 0;
        if (scanline_ >= 0 && scanline_ <= 240) {
            const unsigned beforeVblank = (241U - static_cast<unsigned>(scanline_)) * 341U + 1U - dot_;
            return beforeVblank < 65535 ? beforeVblank : 65535;
        }
        if (scanline_ == 241 && dot_ <= 1) return 1U - dot_;
        // Pre-render и post-vblank удалены минимум на 82182 точки от следующего
        // frame/NMI-фронта с учётом возможного NTSC odd-frame-пропуска.
        return 65535;
    }
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @param dots Число уже прошедших точек PPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool canDeferDots(unsigned dots) const noexcept {
        return renderingEnabled_ && (renderDelay_ | nmiDelay_) == 0 &&
               static_cast<unsigned>(scanline_) < height && dot_ >= 1 &&
               static_cast<unsigned>(dot_) + dots <= 249;
    }
    /**
     * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
     * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
     * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint8_t cpuRead(std::uint16_t reg) noexcept;
    /**
     * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
     * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
     * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void cpuWrite(std::uint16_t reg, std::uint8_t value) noexcept;
    /**
     * @brief Записывает очередной байт OAM DMA в текущий OAM-адрес.
     * @details Продвигает адрес и инвалидирует связанные sprite-кэши в исходном порядке; не заменяет источник DMA.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void dmaWrite(std::uint8_t value) noexcept;
    /**
     * @brief Забирает подготовленное событие NMI, предназначенное CPU.
     * @details Возврат события отличается от простого просмотра уровня линии и должен соблюсти исходный порядок синхронизации.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool takeNmi() noexcept;
    /**
     * @brief Проверяет готовность гостевой кадровой границы.
     * @details Не означает, что потребитель уже закончил цветовой повтор или физический вывод.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool frameReady() const noexcept { return frameReady_; }
    /**
     * @brief Снимает признак уже обработанной гостевой кадровой границы.
     * @details Не сбрасывает номер кадра и не создаёт новый физический кадр.
     */
    void clearFrameReady() noexcept { frameReady_ = false; }
    /**
     * @brief Возвращает счётчик гостевых кадров выбранного PPU.
     * @details Не является числом физических подач на LCD или игровым FPS.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t frameNumber() const noexcept { return frames_; }
    /**
     * @brief Предоставляет заимствованный индексный буфер текущего видеотракта.
     * @details У Machine сначала материализуются CPU/PPU-долги; observable-владелец возвращает nullptr. Указатель нельзя использовать параллельно с изменением владельца или после освобождения его хранилища.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const std::uint8_t* framebuffer() const noexcept { flushPixels();
        fenceRgbOutput();
        return pixels_.data(); }
    /**
     * @brief Возвращает растровую координату либо дробную фазу текущего владельца.
     * @details Значения относятся к одному уже материализованному срезу; номер строки не заменяет CPU-cycle или PAL-остаток.
     * @return Вычисленное значение или состояние типа std::int16_t согласно описанному контракту.
     */
    [[nodiscard]] std::int16_t scanline() const noexcept { return scanline_; }
    /**
     * @brief Возвращает растровую координату либо дробную фазу текущего владельца.
     * @details Значения относятся к одному уже материализованному срезу; номер строки не заменяет CPU-cycle или PAL-остаток.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint16_t dot() const noexcept { return dot_; }
    /**
     * @brief Возвращает метаданные усиления цвета строки.
     * @details Для Machine доступ может материализовать долги; в observable-режиме без цветового спутника данные изображения недоступны.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint8_t emphasis() const noexcept { return mask_ >> 5; }
    /**
     * @brief Возвращает метаданные усиления цвета строки.
     * @details Для Machine доступ может материализовать долги; в observable-режиме без цветового спутника данные изображения недоступны.
     * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint8_t scanlineEmphasis(unsigned y) const noexcept {
        flushPixels();
        fenceRgbOutput();
        return y < height ? lineEmphasis_[y] : 0;
    }

private:
    friend class PpuColourReplay;
    friend class PpuRgbOutput;
    /**
     * @brief Доводит отложенные RGB-хвосты и перекраску до наблюдаемой границы.
     * @details Это граница ядра, а не PPA/LCD fence; аппаратные читатели буфера управляются приложением.
     */
    void fenceRgbOutput() const noexcept;
    // Только Machine вызывает private-helper в той же unity-единице трансляции.
    friend class Machine;
    /**
     * @brief Продвигает устройство на одну его логическую единицу времени.
     * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
     */
    inline void tickInline() noexcept;
    /**
     * @brief Продвигает PPU на указанное число уже прошедших точек.
     * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    inline void tickDotsInline(unsigned dots) noexcept;
    /**
     * @brief Один выбранный OAM-слот: X/атрибуты/индекс, low/high-плоскости и признаки действительности. Порядок слотов сохраняет приоритет первого непрозрачного.
     * @ingroup nes_ppu
     */
    struct Sprite {
        std::uint8_t low = 0, high = 0, x = 0, attributes = 0, index = 0xFF;
        bool valid = false;
    };

    /**
     * @brief Проверяет текущую разрешённость фонового/спрайтового rendering.
     * @details Delayed-изменения PPUMASK материализуются исходной временной логикой.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool rendering() const noexcept { return renderingEnabled_; }
    /**
     * @brief Записывает предусмотренный текущим видеособытием участок вывода.
     * @details Сохраняет индексную плоскость, порядок перекрытия и ограничения видимых координат.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool renderLine() const noexcept { return scanline_ >= -1 && scanline_ < 240; }
    /**
     * @brief Публикует текущий адрес на шине PPU.
     * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void address(std::uint16_t value) noexcept;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint8_t read(std::uint16_t value) noexcept;
    /**
     * @brief Записывает байт через протокол устройства или шины.
     * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
     * @param location Нормализованный физический индекс CIRAM с учётом nametable-алиасов, а не логический CPU-адрес.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void write(std::uint16_t location, std::uint8_t value) noexcept;
    /**
     * @brief Вычисляет физический индекс с правилами зеркалирования PPU.
     * @details Учитывает таблицы имён либо специальные зеркала палитры; сам адрес не выдаёт строб чтения.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    [[nodiscard]] static unsigned paletteAddress(std::uint16_t value) noexcept;
    /**
     * @brief Продвигает coarse X и переключает горизонтальную таблицу имён при wrap.
     * @details Остальные поля loopy v сохраняются.
     */
    void incrementX() noexcept;
    /**
     * @brief Продвигает fine/coarse Y по правилам видимых и специальных строк PPU.
     * @details Строки 29/31 и переключение вертикальной таблицы имён не заменяются обычным сложением адреса.
     */
    void incrementY() noexcept;
    /**
     * @brief Продвигает v после обращения к PPUDATA по текущему режиму.
     * @details Правила rendering и обычного CPU-инкремента различаются; адресные эффекты сохраняются.
     */
    void incrementDataAddress() noexcept;
    /**
     * @brief Обновляет линию/задержку NMI из VBlank и PPUCTRL.
     * @details Сохраняет подавление и delayed-переходы около реального STATUS-чтения.
     */
    void updateNmi() noexcept;
    /**
     * @brief Вставляет следующие байты/идентичности фона в текущие сдвиговые регистры.
     * @details Частичные сдвиги и независимые high/low-ссылки не заменяются предположением о полном восьмибитном сдвиге.
     */
    void loadBackground() noexcept;
    /**
     * @brief Обслуживает точную фазу фонового конвейера PPU.
     * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
     */
    template<unsigned Phase> inline void fetchBackgroundPhase() noexcept;
    /**
     * @brief Возвращает колонку выбираемого фонового тайла с учётом двухтайлового конвейера.
     * @details Вызывается только внутри BG fetch: dots 1..256 дают 2..33, dots 321..336 — 0/1 следующей строки. Scroll в v не подменяет экранную колонку.
     * @return Колонка 0..33; внеэкранные 32/33 не маскируются в 0/1.
     */
    [[nodiscard]] unsigned backgroundFetchTileX() const noexcept {
        return dot_ <= 256U ? ((dot_ - 1U) >> 3U) + 2U : (dot_ - 321U) >> 3U;
    }
    /**
     * @brief Выполняет предусмотренную текущей точкой загрузку фонового конвейера.
     * @details Не считает текущий адрес автоматически CHR: после переходов rendering может сохраняться иной шинный адрес.
     */
    void fetchBackground() noexcept;
    /**
     * @brief Доводит текущие sprite-плоскости и счётчики X до нужной позиции.
     * @details Накопленные сдвиги и различающиеся моменты low/high-fetch учитываются до использования метаданных.
     */
    void synchronizeSprites() noexcept;
    /**
     * @brief Доводит аппаратный обход OAM до текущей растровой точки.
     * @details Сохраняет odd/even-защёлку, очистку secondary OAM, byte-позиции и диагональное переполнение, включая частичные срезы.
     * @param until Последняя включённая PPU-точка evaluation; ограничивается 256, частичная odd/even-защёлка сохраняется.
     */
    void synchronizeSpriteEvaluation(unsigned until) noexcept;
    /**
     * @brief Находит допустимую серию заведомо неподходящих OAM Y-пар.
     * @details Не перескакивает особые n=63/переполнение/частичную защёлку; последний прочитанный байт и secondary-позиция остаются точными.
     * @param n Начальный OAM-индекс 0..62; индекс 63 и оборачивание остаются скалярными.
     * @param limit Максимум завершённых Y-пар, доступных текущему вызову; последний/wrap-спрайт остаётся скалярным.
     * @param height Высота спрайта 8 или 16 PPU-строк, выбранная из текущего PPUCTRL.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned invalidYRun(unsigned n,unsigned limit,unsigned height) noexcept;
    /**
     * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
     * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
     * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
     * @param pattern Адрес паттерна текущей плоскости в момент fetch.
     */
    template<unsigned Phase, bool CachedPattern = false, bool Wired = true>
    inline void fetchSpritePhase(unsigned slot, std::uint16_t pattern = 0) noexcept;
    /**
     * @brief Загружает спрайтовое состояние следующей строки в исходном порядке слотов.
     * @details Число видимых спрайтов не отменяет шинные обращения пустых слотов.
     */
    void fetchSprites() noexcept;
    /**
     * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
     * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
     * @param slot Текущий sprite-слот 0..7, обновляемый при переходе восьмиточечной группы.
     * @param patternSlot Индекс слота, для которого вычислен локальный pattern-кэш; обновляется вместе с pattern.
     * @param pattern Локальный вычисленный pattern-адрес спрайта; действителен только для соответствующего cached-slot.
     */
    template<bool Clocked, unsigned Phase, bool Wired = true>
    inline void spriteFetchDot(unsigned& slot, unsigned& patternSlot, std::uint16_t& pattern) noexcept;
    /**
     * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
     * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    template<bool Clocked> void spriteFetchGroup(unsigned dots) noexcept;
    /**
     * @brief Строит текущий кэш выбора спрайтов для видимой строки.
     * @details Порядок OAM и окклюзия ранними спрайтами сохраняются; отдельная маска Sprite0 не разрешает менять выигравший пиксель.
     */
    void buildSpriteRow() noexcept;
    /**
     * @brief Доводит отложенное состояние спрайтов до текущего растрового среза.
     * @details Раздельные плоскости, flip и накопленные сдвиги относятся к моменту захвата, а не к новой метаинформации.
     */
    void materializeSprites() const noexcept;
    /**
     * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
     * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
     */
    void renderPixel() noexcept;
    /**
     * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
     * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
     */
    inline void visibleDot() noexcept;
    /**
     * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
     * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
     */
    template<unsigned Phase> inline void visibleDotPhase() noexcept;
    /**
     * @brief Обслуживает ограниченную группу видимых точек без отдельного mapper-clock.
     * @details Шаблонные фазы сохраняют тот же конвейер, что и скалярный путь; режим с нужными mapper-часами сюда не переносится.
     * @param dots Число уже прошедших точек PPU.
     */
    void visibleUnclockedGroup(unsigned dots) noexcept;
    /**
     * @brief Рисует ограниченный восьмипиксельный тайл по опубликованным плоскостям и атрибутам.
     * @details Сохраняет текущие clipping, приоритет и row-метаданные потребителя.
     */
    template<bool Clocked, bool Addressed> inline void tile8() noexcept;
    /**
     * @brief Читает фоновые тайлы ограниченного интервала и сохраняет все живые данные.
     * @details Локальное отображение страниц действительно только внутри этого вызова; реальные mapper-сигналы и четыре вида чтений остаются в нужном порядке.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     */
    template<bool Clocked, bool Addressed, bool Mmc3 = false>
    void fetchTileSpan(unsigned tiles) noexcept;
    /**
     * @brief Читает низкий Legacy-span только после существующих проверок режима и маркеров.
     * @details Не переносит FD/FE-защёлки через будущий тайл и не меняет допуск временного C1-владельца.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     */
    void fetchLegacyTileSpan(unsigned tiles) noexcept;
    /**
     * @brief Выбирает пиксели из уже полученных тайлов и спрайтов для полного PPU.
     * @details Учитывает fineX, частичный хвост, левый клип, приоритет, Sprite0 и итоговые индексные значения до RGB-преобразования.
     * @param fetched Массив физических low-ссылок выбранных тайлов; объём подтверждён вызывающим, заимствуется на вызов.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     */
    void renderFetchedTiles(const std::uint32_t* fetched, unsigned tiles) noexcept;
    /**
     * @brief Исполняет exact prepared MMC5 fetch/render без промежуточного массива тайлов.
     * @tparam PolicyValue Policy плана:0..3, совпадающая с успешно подготовленным plan.
     * @param tiles Полные восьмипиксельные группы текущего qualified C0 span.
     * @param plan Заимствованные bases/policy без CPU/MMIO смены эпохи внутри вызова.
     * @param mapper Единственный владелец живых MMC5-защёлок.
     * @param quietSpan true разрешает quiet data; false сохраняет настоящий 4-RD head.
     * @pre Вызывается только после flushPixels и старых stationary/typed/quiet guards.
     * @details Объявление безусловно для одинакового ABI всех consumers; PRIVATE
     * переключатель меняет только CPP-исполнение и не добавляет полей/буферов.
     */
    template<unsigned PolicyValue>
    void fetchRenderMmc5(unsigned tiles, Mmc5RasterPlan& plan,
                         Mmc5& mapper, bool quietSpan) noexcept;
    /**
     * @brief Продвигает PPU на указанное число уже прошедших точек.
     * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
     * @param dots Число уже прошедших точек PPU.
     */
    void tickLongDots(unsigned dots) noexcept;
    /**
     * @brief Компонует невидимый растровый интервал до ближайшего особого события.
     * @details Не пересекает очистку/публикацию VBlank, NMI и обязательные адресные фазы.
     * @param dots Число уже прошедших точек PPU.
     */
    void nonVisibleSpan(unsigned dots) noexcept;
    /**
     * @brief Накапливает отложенный видимый пиксель в ограниченном текущем интервале.
     * @details Перед наблюдаемой границей очередь должна быть доведена без изменения приоритетов и палитры.
     */
    void queuePixel() noexcept;
    /**
     * @brief Доводит отложенную очередь видимых пикселей до текущего среза.
     * @details Сначала сохраняет исходный индексный выбор и необходимые хвосты; материализация не даёт права переиспользовать аппаратно читаемый буфер.
     */
    void flushPixels() const noexcept;
    /**
     * @brief Фиксирует восемь уже выбранных пикселей в индексный и допустимый RGB-выход.
     * @details Переданный RGB-контекст действителен лишь в том же исключительном вызове PPU; атомарная идентичность внешнего владельца не кэшируется между API/кадрами.
     * @param background Восемь уже выбранных фоновых индексов, по четыре бита на пиксель.
     * @param output Участок индексного результата на восемь пикселей; вызывающий обеспечивает границы и наличие хранения.
     * @param sprites Уже декодированная строка первых непрозрачных sprite-победителей; читается без изменения счётчиков OAM.
     * @param start Начальная позиция ограниченного участка вывода.
     * @param mask Снимок PPUMASK для этого растрового интервала.
     * @param rgb Буфер RGB565 вызывающего с согласованным сроком жизни и исключёнными аппаратными читателями.
     * @param writer Не владеющий span-local BG-view, либо nullptr для прежнего пути.
     * @return Накопленный признак Sprite0-попадания выбранных пикселей; индексы/RGB уже записаны, число пикселей не возвращается.
     */
    [[nodiscard]] inline unsigned writeTile8(std::uint32_t background, std::uint8_t* output,
        const std::uint8_t* sprites, unsigned start, unsigned mask, PpuRgbOutput* rgb,
        PpuBackgroundWriter* writer=nullptr) const noexcept;
    /**
     * @brief Восстанавливает упакованные пары фоновых индексов после изменения конвейера.
     * @details Оптимизация не меняет fineX, палитровые биты или оставшийся частичный хвост.
     */
    void rebuildBackgroundPairs() const noexcept;
    /**
     * @brief Вычисляет адрес плоскости спрайта по текущей высоте, строке, тайлу и flip.
     * @details В 8x16 режиме младший бит tile выбирает таблицу; это влияет и на A12.
     * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint16_t spritePattern(unsigned slot) const noexcept;
    /**
     * @brief Разворачивает порядок битов для горизонтального отражения либо индексирования таблицы.
     * @details Результат применяется к той плоскости и тому моменту fetch, для которых он был рассчитан.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] static std::uint8_t reverse(std::uint8_t value) noexcept;

    Bus& bus_;
    bool trackBusCycles_ = true, trackBusAddresses_ = true;
    std::array<std::uint8_t, 32> palette_{};
    std::array<std::uint8_t, 256> oam_{};
    std::array<std::uint8_t, 32> secondary_{};
    std::array<std::uint8_t, 8> secondaryIndex_{};
    mutable std::array<Sprite, 8> sprites_{};
    alignas(std::uint32_t) std::array<std::uint8_t, width> spriteRow_{};
    mutable std::uint16_t spriteCacheClocks_ = 0;
    mutable bool spriteCacheValid_ = false;
    std::uint8_t control_ = 0, mask_ = 0, oamAddress_ = 0;
    mutable std::uint8_t status_ = 0;
    std::uint8_t openBus_ = 0, readBuffer_ = 0, fineX_ = 0;
    std::uint16_t v_ = 0, t_ = 0, busAddress_ = 0;
    bool writeToggle_ = false;
    /**
     * @brief Потребляет подготовленный E_EV sink горизонтального копирования на 257.
     * @details Active путь вызывает после BG-load и перед sprite-fetch, сохраняя
     * исходный порядок шины. Forced-blank путь вызывает после удержанного address(v).
     */
    void finishHorizontalScrollSignal() noexcept {
        if(scrollSignal_)v_=(v_&0x7BE0U)|(t_&0x041FU);
        scrollSignal_=false;
    }
    bool coarseXSignal_ = false; // Защёлка F_TB предшествует фиксации следующего тайла.
    bool scrollSignal_ = false; // E_EV255 управляет Y256 и копированием coarse-X257.
    bool renderingEnabled_ = false, renderingNext_ = false;
    std::uint8_t renderDelay_ = 0;
    std::uint16_t backgroundLow_ = 0, backgroundHigh_ = 0;
    std::uint16_t attributeLow_ = 0, attributeHigh_ = 0;
    std::uint8_t pendingBackgroundShifts_ = 0;
    mutable std::uint8_t pendingPixels_ = 0;
    std::uint8_t pendingPixelX_ = 0, pendingPixelShift_ = 0;
    std::uint8_t nextTile_ = 0, nextAttribute_ = 0, nextLow_ = 0, nextHigh_ = 0;
    std::uint8_t evaluateN_ = 0, evaluateM_ = 0, secondaryPosition_ = 0;
    std::uint8_t evaluateLatch_ = 0xFF, overflowFetches_ = 0;
    bool evaluationDone_ = false;
    std::uint16_t evaluationNextDot_ = 257;
    std::int16_t scanline_ = -1;
    std::uint16_t dot_ = 0;
    std::uint64_t frames_ = 0;
    bool pal_ = false, odd_ = false, frameReady_ = false;
    bool nmiLine_ = false, nmiPending_ = false, suppressVblank_ = false;
    std::uint8_t nmiDelay_ = 0;
    /**
     * @brief Индексное хранилище полного PPU с исходной встроенной либо callback-heap политикой и поддержкой глубокого const-копирования.
     * @ingroup nes_ppu
     */
    class PixelStorage final {
        /**
         * @brief Сохранённая парная политика освобождения собственного пиксельного выделения; не освобождает чужой RGB/ROM.
         * @ingroup nes_ppu
         */
        struct Deleter {
            LegacyPixelAllocator::Release release;
            /**
             * @brief Обслуживает внутренний шаг состояния Deleter в текущем алгоритме.
             * @details Назначение уточняется окружающим пакетом; допустимые интервалы и побочные эффекты задаёт вызывающий владелец.
             * @param data Пиксельное выделение, освобождаемое сохранённой парной политикой allocator; чужая память не передаётся.
             */
            void operator()(std::uint8_t* data) const noexcept {
                if(release)release(data);else delete[] data;
            }
        };
        std::unique_ptr<std::uint8_t[],Deleter> bytes_;
        LegacyPixelAllocator::Allocate allocate_;
    public:
        /**
         * @brief Создаёт либо копирует владеющее индексное хранилище полного PPU.
         * @details При heap-политике выделение может не удаться; копирование сохраняет политику и данные, а отсутствие pixels не должно останавливать время или Sprite0.
         * @param allocator Политика allocate/free для индексных пикселей; callback-ресурсы должны оставаться действительными.
         */
        explicit PixelStorage(LegacyPixelAllocator allocator) noexcept;
        /**
         * @brief Создаёт либо копирует владеющее индексное хранилище полного PPU.
         * @details При heap-политике выделение может не удаться; копирование сохраняет политику и данные, а отсутствие pixels не должно останавливать время или Sprite0.
         * @param other Источник глубокого копирования индексных пикселей; политика allocator сохраняется, чужое выделение не усваивается.
         */
        PixelStorage(const PixelStorage& other) noexcept;
        /**
         * @brief Возвращает заимствованный адрес индексного хранилища PixelStorage.
         * @details Может быть nullptr после отказа выделения; пользователь обязан сохранять временную логику PPU независимо от возможности записи пикселей.
         * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
         */
        std::uint8_t* data() const noexcept { return bytes_.get(); }
        /**
         * @brief Заполняет допустимый участок уже выбранным индексом или цветом.
         * @details Операция ограничена текущим буфером и не изменяет геометрию физического дисплея.
         * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
         */
        void fill(std::uint8_t value) noexcept { if(bytes_)std::fill_n(bytes_.get(),width*height,value); }
        /**
         * @brief Предоставляет пиксель по индексу внутри собственного хранилища.
         * @details Вызов допустим только после проверки наличия allocation и границ. Ссылка не продлевает срок жизни пиксельного буфера.
         * @param index Индекс байта в выделенном пиксельном массиве.
         * @return Изменяемая ссылка на байт индексного пикселя.
         */
        std::uint8_t& operator[](std::size_t index) const noexcept { return bytes_[index]; }
    };
    alignas(std::uint32_t) mutable PixelStorage pixels_;
    mutable std::array<std::uint8_t, height> lineEmphasis_{};
    alignas(std::uint16_t) mutable std::array<std::array<std::uint8_t, 2>, 256> backgroundPairs_{};
    mutable bool backgroundPairsDirty_ = true;
    // Держать горячую row-маску на малых RV32-смещениях; динамические buckets следуют ниже.
    std::uint32_t oamYLow_=0,oamYHigh_=0;
    std::int16_t oamYRow_=-1;
    std::uint8_t oamYHeight_=0;
    bool oamYDirty_=true;
    std::array<std::array<std::uint32_t,2>,256> oamYBuckets_{};
};

}  // пространство имён nes::core
