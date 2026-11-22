/**
 * @file TimingApu.cpp
 * @brief CPU-наблюдаемая проекция APU и хронологическая запись входов звукового рендерера.
 * @ingroup nes_apu
 */
#include "TimingApu.h"

#include <algorithm>

namespace nes::core {
namespace {
constexpr std::uint8_t lengthReload[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
    12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};
constexpr std::uint16_t dmcPeriod[2][16] = {
    {428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54},
    {398,354,316,298,276,236,210,198,176,148,132,118,98,78,66,50}
};
} // пространство имён

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 */
void TimingApu::reset(bool pal) noexcept {
    lengths_.fill(Length{});
    dmc_ = Dmc{};
    cycles_ = frameCycle_ = pendingCycles_ = frameResetDelay_ = 0;
    pal_ = pal;
    fiveStep_ = irqInhibit_ = frameIrq_ = false;
    eventCountdown_ = static_cast<std::uint16_t>(nextEventDistance());
}

/**
 * @brief Продвигает таймер и счётчики DMC без потери выходного бита или запроса байта.
 * @details Условия IRQ, loop, окончания буфера и адресного wrap остаются исходными.
 * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 * @param dmc Изменяемая CPU-наблюдаемая DMC-модель; реальные запросы памяти и украденные такты сохраняют исходную границу.
 */
void TimingApu::advanceDmc(Dmc& dmc, unsigned count, bool pal) noexcept {
    if (count <= dmc.timer) { dmc.timer = static_cast<std::uint16_t>(dmc.timer - count); return; }
    const unsigned period = dmcPeriod[pal ? 1 : 0][dmc.control & 15U];
    const unsigned afterFirst = count - dmc.timer - 1U;
    const unsigned edges = 1U + afterFirst / period;
    dmc.timer = static_cast<std::uint16_t>(period - 1U - afterFirst % period);
    if (edges >= dmc.bits) dmc.bufferEmpty = true;
    dmc.bits = static_cast<std::uint8_t>(8U - ((edges + 8U - dmc.bits) & 7U));
}

/**
 * @brief Материализует накопленное время до следующего значимого события устройства.
 * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
 */
void TimingApu::synchronize() noexcept {
    if (!pendingCycles_) return;
    cycles_ += pendingCycles_;
    frameCycle_ += pendingCycles_;
    if (frameResetDelay_) frameResetDelay_ = static_cast<std::uint8_t>(frameResetDelay_ - pendingCycles_);
    advanceDmc(dmc_, pendingCycles_, pal_);
    pendingCycles_ = 0;
}

/**
 * @brief Обслуживает полукадровые length/sweep события каналов.
 * @details Обновление делителей и запрещённых sweep-целей выполняется в исходном порядке.
 */
void TimingApu::halfFrame() noexcept {
    for (auto& length : lengths_) if (length.value && !length.halt) --length.value;
}

/**
 * @brief Продвигает последовательность quarter/half-frame событий APU.
 * @details Сохраняет NTSC/PAL-расписание, режим четырёх/пяти шагов и семантику IRQ.
 */
void TimingApu::frameCounter() noexcept {
    if (frameResetDelay_ && --frameResetDelay_ == 0) {
        frameCycle_ = 0;
        if (fiveStep_) halfFrame();
        return;
    }
    ++frameCycle_;
    const unsigned second = pal_ ? 16627U : 14913U;
    const unsigned fourth = pal_ ? 33253U : 29829U;
    const unsigned fifth = pal_ ? 41565U : 37281U;
    if (frameCycle_ == second) halfFrame();
    if (fiveStep_) {
        if (frameCycle_ == fifth) halfFrame();
        if (frameCycle_ >= fifth + 1U) frameCycle_ = 0;
    } else {
        if (frameCycle_ >= fourth - 1U && frameCycle_ <= fourth + 1U && !irqInhibit_) frameIrq_ = true;
        if (frameCycle_ == fourth) halfFrame();
        if (frameCycle_ >= fourth + 1U) frameCycle_ = 0;
    }
}

/**
 * @brief Запрашивает либо перезапускает фактическую выборку байта DMC.
 * @details Чтение и украденные CPU-такты согласуются через bus-контракт; значение не генерируется по памяти renderer произвольно.
 */
void TimingApu::restartDmc() noexcept {
    dmc_.address = static_cast<std::uint16_t>(0xC000U | (static_cast<unsigned>(dmc_.start) << 6));
    dmc_.remaining = static_cast<std::uint16_t>(static_cast<unsigned>(dmc_.length) * 16U + 1U);
}

/**
 * @brief Запрашивает либо перезапускает фактическую выборку байта DMC.
 * @details Чтение и украденные CPU-такты согласуются через bus-контракт; значение не генерируется по памяти renderer произвольно.
 */
void TimingApu::fetchDmc() noexcept {
    if (!dmc_.bufferEmpty || !dmc_.remaining) return;
    static_cast<void>(bus_.dmcRead(dmc_.address, cycles_));
    bus_.dmcStall(4, cycles_);
    dmc_.bufferEmpty = false;
    dmc_.address = dmc_.address == 0xFFFF ? 0x8000 : static_cast<std::uint16_t>(dmc_.address + 1U);
    if (--dmc_.remaining == 0) {
        if (dmc_.control & 0x40) restartDmc();
        else if (dmc_.control & 0x80) dmc_.irq = true;
    }
}

/**
 * @brief Вычисляет расстояние до ближайшего события текущего временного APU.
 * @details Отсчёт выражен в CPU-тактах и ограничивает быстрый advance.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned TimingApu::nextEventDistance() const noexcept {
    const unsigned second = pal_ ? 16627U : 14913U;
    const unsigned fourth = pal_ ? 33253U : 29829U;
    const unsigned fifth = pal_ ? 41565U : 37281U;
    const unsigned last = fiveStep_ ? fifth + 1U : fourth + 1U;
    // Запись $4017 меняет режим до отложенного сброса. Прежняя five-step
    // фаза уже может превышать новую four-step границу на следующем такте.
    unsigned distance = frameCycle_ >= last ? 1U : last - frameCycle_;
    /**
     * @brief Уменьшает локальный горизонт до будущего frame-sequencer-события, сохраняя нулевые/уже пройденные границы внешнему пути.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto edge = [this, &distance](unsigned when) noexcept {
        // В SDK RV32 uint32_t — unsigned long, на хосте — unsigned int.
        // Оба расстояния ограничены периодом frame-sequencer.
        if (when > frameCycle_) distance = std::min(distance, static_cast<unsigned>(when - frameCycle_));
    };
    edge(second);
    if (fiveStep_) edge(fifth);
    else { edge(fourth - 1U); edge(fourth); }
    if (frameResetDelay_) distance = std::min(distance, static_cast<unsigned>(frameResetDelay_));
    if (dmc_.remaining) {
        if (dmc_.bufferEmpty) return 1;
        const unsigned period = dmcPeriod[pal_ ? 1 : 0][dmc_.control & 15U];
        distance = std::min(distance, static_cast<unsigned>(dmc_.timer) + 1U + (dmc_.bits - 1U) * period);
    }
    return distance;
}

/**
 * @brief Материализует накопленное время до следующего значимого события устройства.
 * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
 */
void TimingApu::tickEvent() noexcept {
    synchronize();
    ++cycles_;
    frameCounter();
    fetchDmc();
    advanceDmc(dmc_, 1, pal_);
    eventCountdown_ = static_cast<std::uint16_t>(nextEventDistance());
}

/**
 * @brief Продвигает APU на точное число CPU-тактов через его текущие события.
 * @details Сохраняет делители таймеров, frame sequencer, DMC и границы накопления PCM.
 * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
 */
void TimingApu::advanceCpuCycles(unsigned count) noexcept {
    while (count) {
        if (count < eventCountdown_) {
            pendingCycles_ = static_cast<std::uint16_t>(pendingCycles_ + count);
            eventCountdown_ = static_cast<std::uint16_t>(eventCountdown_ - count);
            return;
        }
        count -= eventCountdown_;
        pendingCycles_ = static_cast<std::uint16_t>(pendingCycles_ + eventCountdown_ - 1U);
        eventCountdown_ = 0;
        tickEvent();
    }
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void TimingApu::write(std::uint16_t address, std::uint8_t value) noexcept {
    synchronize();
    if (address >= 0x4000 && address <= 0x4007) {
        auto& length = lengths_[(address >> 2) & 1U];
        if ((address & 3U) == 0) length.halt = (value & 0x20) != 0;
        else if ((address & 3U) == 3 && length.enabled) length.value = lengthReload[value >> 3];
    } else switch (address) {
    case 0x4008: lengths_[2].halt = (value & 0x80) != 0; break;
    case 0x400B: if (lengths_[2].enabled) lengths_[2].value = lengthReload[value >> 3]; break;
    case 0x400C: lengths_[3].halt = (value & 0x20) != 0; break;
    case 0x400F: if (lengths_[3].enabled) lengths_[3].value = lengthReload[value >> 3]; break;
    case 0x4010: dmc_.control = value; if (!(value & 0x80)) dmc_.irq = false; break;
    case 0x4012: dmc_.start = value; break;
    case 0x4013: dmc_.length = value; break;
    case 0x4015:
        for (unsigned i = 0; i < 4; ++i) {
            lengths_[i].enabled = (value & (1U << i)) != 0;
            if (!lengths_[i].enabled) lengths_[i].value = 0;
        }
        if (!(value & 0x10)) dmc_.remaining = 0;
        else if (!dmc_.remaining) restartDmc();
        dmc_.irq = false;
        break;
    case 0x4017:
        fiveStep_ = (value & 0x80) != 0;
        irqInhibit_ = (value & 0x40) != 0;
        if (irqInhibit_) frameIrq_ = false;
        frameResetDelay_ = static_cast<std::uint8_t>((cycles_ & 1U) ? 3 : 4);
        break;
    default: break; // Регистры только формы волны остаются в потоке рендерера.
    }
    eventCountdown_ = static_cast<std::uint16_t>(nextEventDistance());
}

/**
 * @brief Просматривает статус без подтверждения IRQ и других эффектов чтения.
 * @details Используется только там, где вызывающему требуется наблюдение, а не гостевой bus read.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t TimingApu::peekStatus() const noexcept {
    std::uint8_t status = 0;
    for (unsigned i = 0; i < 4; ++i) if (lengths_[i].value) status |= static_cast<std::uint8_t>(1U << i);
    if (dmc_.remaining) status |= 0x10;
    if (frameIrq_) status |= 0x40;
    if (dmc_.irq) status |= 0x80;
    return status;
}

/**
 * @brief Читает CPU-видимый статус каналов и подтверждает предусмотренный frame-IRQ.
 * @details Семантика отличается от peekStatus: побочный эффект чтения должен попасть в соответствующий пакет.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t TimingApu::readStatus() noexcept {
    synchronize();
    const auto status = peekStatus();
    frameIrq_ = false;
    return status;
}

/**
 * @brief Возвращает снимок внутренних счётчиков и флагов устройства для диагностики.
 * @details Снимок не является полным сохраняемым состоянием всей машины.
 * @return Вычисленное значение или состояние типа TimingApu::DebugState согласно описанному контракту.
 */
TimingApu::DebugState TimingApu::debug() const noexcept {
    auto dmc = dmc_;
    advanceDmc(dmc, pendingCycles_, pal_);
    return {{lengths_[0].value, lengths_[1].value, lengths_[2].value, lengths_[3].value},
        {lengths_[0].halt, lengths_[1].halt, lengths_[2].halt, lengths_[3].halt},
        {lengths_[0].enabled, lengths_[1].enabled, lengths_[2].enabled, lengths_[3].enabled},
        cycles(), frameCycle_ + pendingCycles_, dmc.timer, dmc.address, dmc.remaining,
        static_cast<std::uint8_t>(frameResetDelay_ ? frameResetDelay_ - pendingCycles_ : 0),
        dmc.control, dmc.start, dmc.length, dmc.bits, fiveStep_, irqInhibit_, frameIrq_, dmc.bufferEmpty, dmc.irq};
}

} // пространство имён nes::core
