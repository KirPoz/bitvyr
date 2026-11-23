/**
 * @file Legacy160.h
 * @brief Регистры, IRQ-источники и CHR-защёлки узкого Legacy160-профиля.
 * @ingroup nes_cartridge
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::core {

// Аппаратный профиль J.Y. Company mapper90 только для явно распознанного
// исторического iNES-160 образа «Aladdin (E) [p1]». Номер mapper160 неоднозначен;
// Cartridge не должен выбирать этот профиль для любого образа с таким номером.
// Регистры/IRQ реализованы по аппаратной спецификации J.Y. Company ASIC
// из NESdev; чужая реализация эмулятора не импортируется.
/**
 * @brief Состояние поддерживаемого Legacy160: банковые режимы, CPU/PPU-зависимые IRQ и FD/FE-защёлки. Не является универсальной моделью всех mapper160-образов.
 * @ingroup nes_cartridge
 */
class Legacy160 final {
public:
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     * @param prgBytes Полный размер PRG-области разобранного ROM в байтах, без заголовка и trainer.
     * @param chrBytes Полный размер CHR-области разобранного ROM в байтах; нуль отдельно допускает CHR RAM.
     */
    void reset(std::size_t prgBytes, std::size_t chrBytes) noexcept;
    /**
     * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
     * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t cpuRead(std::uint16_t address, std::uint8_t openBus) const noexcept;
    /**
     * @brief Записывает байт через протокол устройства или шины.
     * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void write(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Вычисляет физическое смещение для текущей банковской раскладки.
     * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    std::size_t prgOffset(std::uint16_t address) const noexcept;
    /**
     * @brief Вычисляет физическое смещение для текущей банковской раскладки.
     * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    std::size_t chrOffset(std::uint16_t address) const noexcept;
    /**
     * @brief Продвигает CPU-зависимые счётчики mapper.
     * @details Эти IRQ-источники независимы от PPU A12 и не должны теряться при отложенном PPU.
     */
    void cpuCycle() noexcept;
    /**
     * @brief Ограничивает CPU-интервал до возможного нового mapper-IRQ.
     * @details Горизонт не разрешает удалять реальные чтения или записи.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned cpuIrqQuietCycles() const noexcept {
        const unsigned direction = irqMode_ >> 6;
        if (!irqEnabled_ || irqPending_ || (irqMode_ & 3U) != 0 || (direction != 1 && direction != 2)) return ~0U;
        const unsigned period = (irqMode_ & 4U) ? 8U : 256U;
        const unsigned phase = prescaler_ & (period - 1U);
        const unsigned until = direction == 1 ? period - phase + (255U - counter_) * period
                                              : phase + 1U + unsigned(counter_) * period;
        return until - 1U;
    }
    /**
     * @brief Ограничивает CPU-интервал до возможного нового mapper-IRQ.
     * @details Горизонт не разрешает удалять реальные чтения или записи.
     * @param count Длина допустимого интервала в гостевых CPU-тактах.
     */
    void cpuCyclesQuiet(unsigned count) noexcept {
        if (productCycles_) {
            if (count >= productCycles_) { productCycles_ = 0; product_ = pendingProduct_; }
            else productCycles_ = static_cast<std::uint8_t>(productCycles_ - count);
        }
        const unsigned direction = irqMode_ >> 6;
        if (!irqEnabled_ || (irqMode_ & 3U) != 0 || (direction != 1 && direction != 2)) return;
        const unsigned period = (irqMode_ & 4U) ? 8U : 256U;
        const unsigned phase = prescaler_ & (period - 1U);
        const unsigned clocks = direction == 1 ? (phase + count) / period : (count + period - 1U - phase) / period;
        if (direction == 1) {
            prescaler_ = static_cast<std::uint8_t>(prescaler_ + count);
            if (clocks > 255U - counter_) irqPending_ = true;
            counter_ = static_cast<std::uint8_t>(counter_ + clocks);
        } else {
            prescaler_ = static_cast<std::uint8_t>(prescaler_ - count);
            if (clocks > counter_) irqPending_ = true;
            counter_ = static_cast<std::uint8_t>(counter_ - clocks);
        }
    }
    /**
     * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
     * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
     */
    void cpuWriteStrobe() noexcept;
    // Pending IRQ не прекращает счёт. Только source/enable/direction
    // могут сделать каждый CPU-строб записи действительно сохраняющей состояние операцией.
    /**
     * @brief Проверяет, допустимо ли не выдавать отдельный CPU write-strobe в текущем режиме.
     * @details Предикат ограничивает оптимизацию записей; состояние направления/источника IRQ должно оставаться тем же в интервале.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool cpuWriteStrobeIsNoop() const noexcept {
        const unsigned direction = irqMode_ >> 6;
        return (irqMode_ & 3U) != 3 || !irqEnabled_ ||
            (direction != 1 && direction != 2);
    }
    /**
     * @brief Публикует текущий адрес на шине PPU.
     * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     */
    void ppuAddress(std::uint16_t address) noexcept;
    /**
     * @brief Проверяет или обслуживает стабильную фазу DMA без потери её адресного эффекта.
     * @details Вызывающий код сохраняет исходный порядок такта и доступа.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool dmaStableAddress(std::uint16_t address) const noexcept {
        return ppuAddress_==(address&0x3FFFU)&&a12_==bool(address&0x1000U);
    }
    // Вызывать после настоящего чтения PPU-памяти, включая nametable,
    // а не смены адресной шины. Также продвигает необязательные CHR-защёлки.
    // True только при переключении CHR-защёлки этим чтением; IRQ тактируются всегда.
    /**
     * @brief Читает байт адресного пространства PPU с нужным выбором памяти.
     * @details Паттерны CHR, таблицы имён и палитра имеют разные правила зеркалирования и mapper-эффектов; чтение не заменяет отдельный адресный строб.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool ppuRead() noexcept;
    // Вызывающий дополнительно доказывает BG tableLOW, стабильный phase0-интервал, подключённую
    // адресную линию и ПУСТЫЕ FD/FE-маркеры каждого выбранного физического NT-байта.
    // Без исключений по ROM/mode/enable/pending: source1 видит ноль подъёмов.
    /**
     * @brief Проверяет неизменность отображения внутри низкого фонового Legacy-span.
     * @details Проверка режима не заменяет отдельный контроль опасных FD/FE-маркеров таблицы имён.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool canLowBackgroundSpan() const noexcept { return (irqMode_ & 3U) == 1U; }
    // Завершает только физическое адресное/A12-состояние. Все IRQ-регистры и ОБЕ
    // скрытые CHR-защёлки неизменны. Финальный настоящий адрес high-байта
    // берётся из хвостовой выборки, а не более позднего живого bank/NT-образа.
    /**
     * @brief Завершает разрешённый низкий Legacy-интервал с его конечным адресом.
     * @details Отказ не должен менять mapper; последующие FD/FE-запросы и CPU-наблюдаемые эффекты сохраняют прежний путь.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param finalHighAddress Последний реально выбранный high-pattern-адрес PPU; не вычисляется по более позднему mapper-состоянию.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool completeLowBackgroundSpan(unsigned tiles,std::uint16_t finalHighAddress) noexcept;
    // Точное завершение phase0-тайла: пять адресов/четыре чтения.
    // Вызывающий уже проверил отсутствие нового IRQ-фронта внутри всего интервала.
    /**
     * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
     * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
     * @param highAddress Адрес настоящего high-byte чтения PPU, по которому завершается mapper-защёлка.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool backgroundTile(std::uint16_t highAddress) noexcept;
    /**
     * @brief Проверяет, отображается ли PRG-источник в CPU-окно $6000.
     * @details Отличает этот режим от обычной PRG-RAM и открытой шины.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool mapsPrgAt6000() const noexcept { return (mode_ & 0x80U) != 0; }
    /**
     * @brief Проверяет разрешение записи CHR для текущего режима mapper.
     * @details Не записывает байт и не выполняет соответствующий строб.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool chrWriteEnabled() const noexcept { return (memory_ & 0x40U) != 0; }
    /**
     * @brief Возвращает текущий электрический уровень IRQ устройства.
     * @details Проверка не подтверждает IRQ и не обслуживает его в CPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool irqLine() const noexcept { return irqPending_; }
    // Произвольная PPU-точка даёт максимум одно IRQ-source-чтение или подъём A12.
    // Источники CPU-clock/write отдельно ограничивает Machine.
    /**
     * @brief Проверяет отсутствие нового наблюдаемого PPU-origin IRQ в заданном интервале.
     * @details Учитывает текущую фазу/A12/счётчик; не утверждает отсутствие CPU-clocked IRQ или иных устройств.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned ppuIrqQuietDotsAny() const noexcept {
        const unsigned source = irqMode_ & 3U;
        const unsigned direction = irqMode_ >> 6;
        if (!irqEnabled_ || irqPending_ || (source != 1 && source != 2) ||
            (direction != 1 && direction != 2)) return 65535;
        const unsigned period = (irqMode_ & 4U) ? 8U : 256U;
        const unsigned phase = prescaler_ & (period - 1U);
        const unsigned untilAssert = direction == 1
            ? period - phase + (255U - counter_) * period
            : phase + 1U + unsigned(counter_) * period;
        return untilAssert - 1U;
    }
    // Только стабильные обычные background-выборки, выровненные по phase0. CPU-такты
    // и CPU-write IRQ-источники не входят в эту PPU-гарантию.
    /**
     * @brief Проверяет отсутствие нового наблюдаемого PPU-origin IRQ в заданном интервале.
     * @details Учитывает текущую фазу/A12/счётчик; не утверждает отсутствие CPU-clocked IRQ или иных устройств.
     * @param dots Число уже прошедших точек PPU.
     * @param backgroundHigh Выбор верхней pattern-таблицы фона по PPUCTRL; влияет на настоящую траекторию A12.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool ppuIrqQuietFor(unsigned dots, bool backgroundHigh) const noexcept {
        if (!irqEnabled_ || irqPending_ || !dots) return true;
        const unsigned source = irqMode_ & 3U;
        // У background-тайла нижней таблицы нет подъёма A12. При source1
        // clocks=0, оба расстояния до установки IRQ по направлению >=1.
        if (source == 1 && !backgroundHigh) return true;
        const unsigned direction = irqMode_ >> 6;
        if ((source != 1 && source != 2) || (direction != 1 && direction != 2)) return true;
        const unsigned clocks = source == 1 ? (backgroundHigh ? (dots + 7U) / 8U : 0U)
                                             : (dots + 1U) / 2U;
        const unsigned period = (irqMode_ & 4U) ? 8U : 256U;
        const unsigned prescaler = prescaler_ & (period - 1U);
        const unsigned untilAssert = direction == 1
            ? period - prescaler + (255U - counter_) * period
            : prescaler + 1U + unsigned(counter_) * period;
        return clocks < untilAssert;
    }
    // Сырое кодирование D001: CIRAM A10 <- PPU A10, PPU A11, земля, Vcc.
    /**
     * @brief Возвращает текущую характеристику подключённого картриджа.
     * @details Признак PAL из заголовка не подменяет явно выбранный режим Machine; loaded описывает действительность backing текущего картриджа.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t mirrorMode() const noexcept { return mirror_ & 3U; }

private:
    /**
     * @brief Разворачивает порядок битов для горизонтального отражения либо индексирования таблицы.
     * @details Результат применяется к той плоскости и тому моменту fetch, для которых он был рассчитан.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    static std::uint8_t reverse7(std::uint8_t value) noexcept;
    /**
     * @brief Обслуживает счётчик и разрешение IRQ картриджа.
     * @details Подтверждение/отключение и электрический pending сохраняют заданный конкретным mapper порядок.
     */
    void clockIrq() noexcept;
    /**
     * @brief Обслуживает счётчик и разрешение IRQ картриджа.
     * @details Подтверждение/отключение и электрический pending сохраняют заданный конкретным mapper порядок.
     */
    void disableIrq() noexcept;
    std::array<std::uint8_t, 4> prg_{};
    std::array<std::uint16_t, 8> chr_{};
    std::size_t prgBytes_ = 0, chrBytes_ = 0;
    std::uint8_t mode_ = 0, mirror_ = 0, memory_ = 0, outer_ = 0;
    std::uint8_t irqMode_ = 0, prescaler_ = 0, counter_ = 0, irqXor_ = 0;
    std::uint8_t multiplicand_ = 0, multiplier_ = 0, productCycles_ = 0;
    std::uint16_t product_ = 0, pendingProduct_ = 0, ppuAddress_ = 0;
    std::uint8_t accumulator_ = 0, test_ = 0;
    bool irqEnabled_ = false, irqPending_ = false, a12_ = false;
    std::array<bool, 2> chrLatch_{};
};

} // пространство имён nes::core
