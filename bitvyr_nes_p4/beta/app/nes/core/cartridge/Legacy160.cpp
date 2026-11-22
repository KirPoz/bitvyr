/**
 * @file Legacy160.cpp
 * @brief Отдельный идентифицированный аппаратный профиль J.Y. Company mapper90/160.
 * @ingroup nes_cartridge
 */
#include "Legacy160.h"

namespace nes::core {

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 * @param prgBytes Полный размер PRG-области разобранного ROM в байтах, без заголовка и trainer.
 * @param chrBytes Полный размер CHR-области разобранного ROM в байтах; нуль отдельно допускает CHR RAM.
 */
void Legacy160::reset(std::size_t prgBytes, std::size_t chrBytes) noexcept {
    prgBytes_ = prgBytes;
    chrBytes_ = chrBytes;
    for (unsigned i = 0; i < prg_.size(); ++i) prg_[i] = static_cast<std::uint8_t>(i);
    for (unsigned i = 0; i < chr_.size(); ++i) chr_[i] = static_cast<std::uint16_t>(i);
    mode_ = mirror_ = memory_ = outer_ = 0;
    irqMode_ = prescaler_ = counter_ = irqXor_ = 0;
    multiplicand_ = multiplier_ = productCycles_ = 0;
    product_ = pendingProduct_ = ppuAddress_ = 0;
    accumulator_ = test_ = 0;
    irqEnabled_ = irqPending_ = a12_ = false;
    chrLatch_.fill(false);
}

/**
 * @brief Разворачивает порядок битов для горизонтального отражения либо индексирования таблицы.
 * @details Результат применяется к той плоскости и тому моменту fetch, для которых он был рассчитан.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Legacy160::reverse7(std::uint8_t value) noexcept {
    std::uint8_t result = 0;
    for (unsigned i = 0; i < 7; ++i) {
        result = static_cast<std::uint8_t>((result << 1) | (value & 1U));
        value >>= 1;
    }
    return result;
}

/**
 * @brief Вычисляет физическое смещение для текущей банковской раскладки.
 * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
 */
std::size_t Legacy160::prgOffset(std::uint16_t address) const noexcept {
    if (prgBytes_ == 0) return 0;
    const unsigned prgMode = mode_ & 3U;
    const bool switchLast = (mode_ & 4U) != 0;
    std::size_t bank = 0;
    /**
     * @brief Преобразует значение PRG-регистра по текущему prgMode; только режим 3 отражает семь bank-битов.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto bankRegister = [prgMode](std::uint8_t value) {
        return static_cast<std::size_t>(prgMode == 3 ? reverse7(value) : value);
    };
    if (address < 0x8000) {
        bank = bankRegister(prg_[3]);
        if (prgMode == 0) bank = (bank << 2) | 3U;
        else if (prgMode == 1) bank = (bank << 1) | 1U;
    } else if (prgMode == 0) {
        bank = ((switchLast ? bankRegister(prg_[3]) : 15U) << 2) | ((address >> 13) & 3U);
    } else if (prgMode == 1) {
        bank = ((address < 0xC000 ? bankRegister(prg_[1]) : switchLast ? bankRegister(prg_[3]) : 31U) << 1)
            | ((address >> 13) & 1U);
    } else {
        const unsigned slot = (address >> 13) & 3U;
        bank = slot == 3 && !switchLast ? 63U : bankRegister(prg_[slot]);
    }
    // Профиль использует разводку внешнего PRG-банка 512 KiB mapper90. Modulo
    // моделирует отсутствующие старшие ROM-линии у меньшего образа 256 KiB.
    const std::size_t outer = static_cast<std::size_t>((outer_ >> 1) & 3U) * 524288U;
    return (outer + ((bank & 63U) * 8192U) + (address & 8191U)) % prgBytes_;
}

/**
 * @brief Вычисляет физическое смещение для текущей банковской раскладки.
 * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
 */
std::size_t Legacy160::chrOffset(std::uint16_t address) const noexcept {
    if (chrBytes_ == 0) return address & 8191U;
    const unsigned chrMode = (mode_ >> 3) & 3U;
    const unsigned shift = 3U - chrMode;
    const unsigned size = 1024U << shift;
    unsigned index = (address >> 10) & (7U << shift);
    if (chrMode == 1 && (outer_ & 0x80U) != 0) {
        const unsigned half = (address >> 12) & 1U;
        index = half * 4U + (chrLatch_[half] ? 2U : 0U);
    }
    const std::size_t outerMask = (outer_ & 0x20U) ? 524287U : 262143U;
    std::size_t outer = static_cast<std::size_t>((outer_ >> 3) & 3U) * 524288U;
    if ((outer_ & 0x20U) == 0) outer += static_cast<std::size_t>(outer_ & 1U) * 262144U;
    const auto bank = (static_cast<std::size_t>(chr_[index]) * size) & outerMask;
    return (outer + bank + (address & (size - 1U))) % chrBytes_;
}

/**
 * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
 * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Legacy160::cpuRead(std::uint16_t address, std::uint8_t openBus) const noexcept {
    if (address == 0x5000 || address == 0x5400 || address == 0x5C00) return openBus & 0x3FU;
    switch (address & 0xF803U) {
    case 0x5800: return static_cast<std::uint8_t>(product_);
    case 0x5801: return static_cast<std::uint8_t>(product_ >> 8);
    case 0x5802: return accumulator_;
    case 0x5803: return test_;
    default: return openBus;
    }
}

/**
 * @brief Обслуживает счётчик и разрешение IRQ картриджа.
 * @details Подтверждение/отключение и электрический pending сохраняют заданный конкретным mapper порядок.
 */
void Legacy160::disableIrq() noexcept {
    irqEnabled_ = false;
    irqPending_ = false;
    prescaler_ = 0;
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Legacy160::write(std::uint16_t address, std::uint8_t value) noexcept {
    if ((address & 0xF800U) == 0x5800U) {
        switch (address & 3U) {
        case 0: multiplicand_ = value; break;
        case 1:
            multiplier_ = value;
            pendingProduct_ = static_cast<std::uint16_t>(static_cast<unsigned>(multiplicand_) * multiplier_);
            productCycles_ = 8;
            break;
        case 2: accumulator_ = static_cast<std::uint8_t>(accumulator_ + value); break;
        case 3: accumulator_ = 0; test_ = value; break;
        default: break;
        }
    } else if ((address & 0xF800U) == 0x8000U) {
        prg_[address & 3U] = value;
    } else if ((address & 0xF800U) == 0x9000U) {
        auto& bank = chr_[address & 7U];
        bank = static_cast<std::uint16_t>((bank & 0xFF00U) | value);
    } else if ((address & 0xF800U) == 0xA000U) {
        auto& bank = chr_[address & 7U];
        bank = static_cast<std::uint16_t>((bank & 0xFFU) | (static_cast<std::uint16_t>(value) << 8));
    } else if ((address & 0xF000U) == 0xC000U) {
        switch (address & 7U) {
        case 0: if ((value & 1U) != 0) irqEnabled_ = true; else disableIrq(); break;
        case 1: irqMode_ = value; break;
        case 2: disableIrq(); break;
        case 3: irqEnabled_ = true; break;
        case 4: prescaler_ = value ^ irqXor_; break;
        case 5: counter_ = value ^ irqXor_; break;
        case 6: irqXor_ = value; break;
        // C007/C001-bit3 не документирован исследователями аппаратуры и
        // не используется известным ПО; профиль не выдумывает его поведение.
        default: break;
        }
    } else if ((address & 0xF800U) == 0xD000U) {
        switch (address & 3U) {
        case 0: mode_ = value; break;
        case 1: mirror_ = value; break;
        case 2: memory_ = value; break;
        case 3: outer_ = value; break;
        default: break;
        }
    }
    // B000-B007 намеренно неактивны на PCB mapper90: перемычка
    // отключает ROM nametable и расширенный mirroring.
}

/**
 * @brief Обслуживает счётчик и разрешение IRQ картриджа.
 * @details Подтверждение/отключение и электрический pending сохраняют заданный конкретным mapper порядок.
 */
void Legacy160::clockIrq() noexcept {
    const unsigned direction = irqMode_ >> 6;
    if (!irqEnabled_ || (direction != 1 && direction != 2)) return;
    const std::uint8_t mask = (irqMode_ & 4U) ? 7 : 0xFF;
    if (direction == 1) {
        ++prescaler_;
        if ((prescaler_ & mask) == 0) {
            ++counter_;
            if (counter_ == 0) irqPending_ = true;
        }
    } else {
        --prescaler_;
        if ((prescaler_ & mask) == mask) {
            --counter_;
            if (counter_ == 0xFF) irqPending_ = true;
        }
    }
}

/**
 * @brief Продвигает CPU-зависимые счётчики mapper.
 * @details Эти IRQ-источники независимы от PPU A12 и не должны теряться при отложенном PPU.
 */
void Legacy160::cpuCycle() noexcept {
    if (productCycles_ != 0 && --productCycles_ == 0) product_ = pendingProduct_;
    // Промежуточные результаты умножителя в первые семь тактов здесь
    // не определены; обычное ПО ждёт документированные восемь тактов.
    if ((irqMode_ & 3U) == 0) clockIrq();
}

/**
 * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
 * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
 */
void Legacy160::cpuWriteStrobe() noexcept {
    if ((irqMode_ & 3U) == 3) clockIrq();
}

/**
 * @brief Публикует текущий адрес на шине PPU.
 * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 */
void Legacy160::ppuAddress(std::uint16_t address) noexcept {
    ppuAddress_ = address & 0x3FFFU;
    const bool a12 = (address & 0x1000U) != 0;
    // В отличие от MMC3 микросхема считает каждый подъём A12 без фильтра
    // low-времени. Выбираемый 3-битный prescaler обеспечивает деление строки.
    if (!a12_ && a12 && (irqMode_ & 3U) == 1) clockIrq();
    a12_ = a12;
}

/**
 * @brief Читает байт адресного пространства PPU с нужным выбором памяти.
 * @details Паттерны CHR, таблицы имён и палитра имеют разные правила зеркалирования и mapper-эффектов; чтение не заменяет отдельный адресный строб.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Legacy160::ppuRead() noexcept {
    if ((irqMode_ & 3U) == 2) clockIrq();
    if (ppuAddress_ >= 0x2000) return false;
    const auto pattern = ppuAddress_ & 0x0FF8U;
    if (pattern != 0x0FD8 && pattern != 0x0FE8) return false;
    auto& latch = chrLatch_[(ppuAddress_ >> 12) & 1U];
    const bool next = pattern == 0x0FE8;
    if (latch == next) return false;
    latch = next;
    return true;
}

#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
/**
 * @brief Завершает разрешённый низкий Legacy-интервал с его конечным адресом.
 * @details Отказ не должен менять mapper; последующие FD/FE-запросы и CPU-наблюдаемые эффекты сохраняют прежний путь.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 * @param finalHighAddress Последний реально выбранный high-pattern-адрес PPU; не вычисляется по более позднему mapper-состоянию.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Legacy160::completeLowBackgroundSpan(unsigned tiles,std::uint16_t finalHighAddress) noexcept {
    const auto marker=finalHighAddress&0x0FF8U;
    if(!tiles||tiles>32||!canLowBackgroundSpan()||finalHighAddress>=0x1000U||!(finalHighAddress&8U)||
       marker==0x0FD8U||marker==0x0FE8U)return false;
    ppuAddress_=finalHighAddress;
    a12_=false;
    return true;
}
#endif

#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
/**
 * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
 * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
 * @param highAddress Адрес настоящего high-byte чтения PPU, по которому завершается mapper-защёлка.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Legacy160::backgroundTile(std::uint16_t highAddress) noexcept {
    const unsigned source = irqMode_ & 3U;
    const unsigned count = source == 2 ? 4U : (source == 1 && (highAddress & 0x1000U) ? 1U : 0U);
    const unsigned direction = irqMode_ >> 6;
    if (count && irqEnabled_ && (direction == 1 || direction == 2)) {
        const unsigned period = (irqMode_ & 4U) ? 8U : 256U;
        const unsigned phase = prescaler_ & (period - 1U);
        if (direction == 1) {
            const unsigned clocks = (phase + count) / period;
            prescaler_ = static_cast<std::uint8_t>(prescaler_ + count);
            if (clocks > 255U - counter_) irqPending_ = true;
            counter_ = static_cast<std::uint8_t>(counter_ + clocks);
        } else {
            const unsigned clocks = (count + period - 1U - phase) / period;
            prescaler_ = static_cast<std::uint8_t>(prescaler_ - count);
            if (clocks > counter_) irqPending_ = true;
            counter_ = static_cast<std::uint8_t>(counter_ - clocks);
        }
    }
    ppuAddress_ = highAddress & 0x3FFFU;
    a12_ = (highAddress & 0x1000U) != 0;
    const unsigned pattern = highAddress & 0x0FF8U;
    if (pattern != 0x0FD8U && pattern != 0x0FE8U) return false;
    auto& latch = chrLatch_[(highAddress >> 12) & 1U];
    const bool next = pattern == 0x0FE8U;
    if (latch == next) return false;
    latch = next;
    return true;
}
#endif

} // пространство имён nes::core
