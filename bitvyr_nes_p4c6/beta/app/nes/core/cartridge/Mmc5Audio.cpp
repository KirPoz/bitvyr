/**
 * @file Mmc5Audio.cpp
 * @brief Собственные делители, envelope/length и PCM read-strobe MMC5.
 * @ingroup nes_cartridge
 */
#include "Mmc5Audio.h"

#include <algorithm>

// Raw/selected политики делят один unconditional slot и взаимоисключаются.


namespace nes::core {
namespace {
// Табличные аппаратные параметры, не код стороннего эмулятора.
constexpr std::uint8_t lengths[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};
constexpr std::uint8_t duties[4] = {0x02, 0x06, 0x1E, 0xF9};
/** @brief Считает включённые шаги неизменного duty; таблица долей выводится из одного waveform-источника. */
constexpr std::uint8_t onSteps(std::uint8_t waveform) noexcept {
    std::uint8_t count = 0;
    for (unsigned bit = 0; bit != 8; ++bit) count += (waveform >> bit) & 1U;
    return count;
}
constexpr std::uint8_t dutyOn[4] = {onSteps(duties[0]), onSteps(duties[1]), onSteps(duties[2]), onSteps(duties[3])};
/**
 * @brief Вычисляет прежний raw срок до отличного duty-бита, не продвигая фазу.
 * @param control Регистр pulse; используются только два старших duty-бита.
 * @param sequence Текущий шаг 0..7.
 * @param timer Остаток half-rate делителя; edge наступает через timer+1.
 * @param period Reload0..2047.
 * @param halfPhase Уже прошедший одиночный M2 до следующего half-rate tick.
 * @return M2 до следующего отличного duty-бита; gating проверяет вызывающий.
 */
inline std::uint32_t dutyChangeCpuCycles(std::uint8_t control, std::uint8_t sequence,
                                        std::uint16_t timer, std::uint16_t period, bool halfPhase) noexcept {
    const auto waveform = duties[control >> 6U];
    const auto current = (waveform >> sequence) & 1U;
    unsigned steps = 1;
    while (((waveform >> ((sequence + steps) & 7U)) & 1U) == current) ++steps;
    const auto ticks = static_cast<std::uint32_t>(timer) + 1U +
        (steps - 1U) * (static_cast<std::uint32_t>(period) + 1U);
    return 2U * ticks - static_cast<unsigned>(halfPhase);
}
} // namespace

/** @brief Начальная фаза определена только политикой cold reset эмулятора. */
Mmc5Audio::Mmc5Audio(bool mmc5a) noexcept { reset(mmc5a); }

/** @brief Новая сессия не заимствует историю старого ROM либо native APU. */
void Mmc5Audio::reset(bool mmc5a) noexcept {
    pulses_ = {};
    frameRemaining_ = framePeriodCpu;
    pcmControl_ = 0;
    pcm_ = 0;
    halfPhase_ = false;
    pcmIrqTrip_ = false;
    mmc5a_ = mmc5a;
    cachedSampleQ16_ = 0;
    pendingCpu_ = 0;
    eventRemaining_ = framePeriodCpu;
}

/** @brief Reset detection затрагивает управляющий регистр 5010, не DAC. */
void Mmc5Audio::detectedReset() noexcept { pcmControl_ = 0; }

/**
 * @brief Пропускает whole timer periods, не теряя остаток или duty phase.
 * @details timer=t означает ещё t+1 half-rate clocks до следующего фронта.
 */
void Mmc5Audio::Pulse::advance(std::uint32_t ticks) noexcept {
    if (ticks <= timer) {
        timer = static_cast<std::uint16_t>(timer - ticks);
        return;
    }
    ticks -= static_cast<std::uint32_t>(timer) + 1U;
    const auto divisor = static_cast<std::uint32_t>(period) + 1U;
    const auto steps = 1U + ticks / divisor;
    timer = static_cast<std::uint16_t>(period - ticks % divisor);
    sequence = static_cast<std::uint8_t>((sequence + steps) & 7U);
}

/** @brief Envelope продолжает считать и в constant-volume режиме. */
void Mmc5Audio::Pulse::frameClock() noexcept {
    const auto volume = static_cast<std::uint8_t>(control & 15U);
    if (envelopeStart) {
        envelopeStart = false;
        decay = 15;
        envelopeDivider = volume;
    } else if (envelopeDivider) {
        --envelopeDivider;
    } else {
        envelopeDivider = volume;
        if (decay) --decay;
        else if (control & 0x20U) decay = 15;
    }
    if (length && !(control & 0x20U)) --length;
}

/** @brief Gating зависит только от length, waveform и volume/envelope. */
std::uint8_t Mmc5Audio::Pulse::output() const noexcept {
    if (!length || !(duties[control >> 6U] & (1U << sequence))) return 0;
    return (control & 0x10U) ? static_cast<std::uint8_t>(control & 15U) : decay;
}

/** @brief Нулевая громкость/length не создаёт carrier-фронтов, а envelope может ожить только на frame edge. */
bool Mmc5Audio::Pulse::portableCarrier() const noexcept {
    return period <= 1U && length && ((control & 0x10U) ? (control & 15U) : decay);
}

/**
 * @brief Дробит span только по автономным frame edges; half-phase общая.
 * @details segment<=7424, поэтому промежуточная арифметика не переполняется
 * даже при cycles=UINT32_MAX; native APU sequencer ни разу не вызывается.
 */
void Mmc5Audio::advanceTimers(std::uint32_t cycles) noexcept {
    bool selectedDirty=false;
    while (cycles) {
        const auto count = std::min(cycles, static_cast<std::uint32_t>(frameRemaining_));
        const auto halfTicks = (count + static_cast<unsigned>(halfPhase_)) / 2U;
        halfPhase_ = ((count + static_cast<unsigned>(halfPhase_)) & 1U) != 0;
        for (auto& pulse : pulses_) {
            // Period0/1 selected DAC не зависит от raw sequence, даже при volume0.
            selectedDirty=selectedDirty||(pulse.period>1U&&pulse.length&&halfTicks>pulse.timer);
            pulse.advance(halfTicks);
        }
        cycles -= count;
        frameRemaining_ = static_cast<std::uint16_t>(frameRemaining_ - count);
        if (!frameRemaining_) {
            selectedDirty=true;
            for (auto& pulse : pulses_) pulse.frameClock();
            frameRemaining_ = framePeriodCpu;
        }
    }
    if(selectedDirty)refreshSelectedSample();
}

/** @brief Короткий M2-путь не обходит pulse timers до влияющего события. */
void Mmc5Audio::clockCpu(std::uint32_t cycles) noexcept {
    if (cycles < eventRemaining_) {
        pendingCpu_ = static_cast<std::uint16_t>(pendingCpu_ + cycles);
        eventRemaining_ = static_cast<std::uint16_t>(eventRemaining_ - cycles);
        return;
    }
    // pending+первый срок ограничены одним frame interval и не переполняются.
    const auto first = static_cast<std::uint32_t>(eventRemaining_);
    advanceTimers(static_cast<std::uint32_t>(pendingCpu_) + first);
    pendingCpu_ = 0;
    advanceTimers(cycles - first);
    refreshDeadline();
}

/** @brief Материализация не меняет уже наблюдаемые уровни до ближайшего edge. */
void Mmc5Audio::flushPending() noexcept {
    advanceTimers(pendingCpu_);
    pendingCpu_ = 0;
}

/** @brief length0 скрывает waveform, но не останавливает фазу его делителя. */
void Mmc5Audio::refreshDeadline() noexcept {
    auto result = static_cast<std::uint32_t>(frameRemaining_);
    for (const auto& pulse : pulses_) {
        const auto volume = (pulse.control & 0x10U) ? pulse.control & 15U : pulse.decay;
        if (pulse.length && volume) {
            result = std::min(result, dutyChangeCpuCycles(pulse.control, pulse.sequence,
                                                         pulse.timer, pulse.period, halfPhase_));
        }
    }
    eventRemaining_ = static_cast<std::uint16_t>(result);
}

/** @brief Zero не заменяет DAC; enable только маскирует выход trip latch. */
void Mmc5Audio::writePcm(std::uint8_t value) noexcept {
    const auto previous = pcm_;
    pcmIrqTrip_ = value == 0;
    if (value) pcm_ = value;
    if (pcm_ != previous)
        cachedSampleQ16_ -= pcmUnitQ16 * (static_cast<std::int32_t>(pcm_) - previous);
}

/** @brief Полное декодирование сохраняет каждый PRG read-strobe и open bus. */
std::uint8_t Mmc5Audio::cpuRead(std::uint16_t address, std::uint8_t value) noexcept {
    if (address == 0x5010) {
        const auto result = static_cast<std::uint8_t>((value & 0x7EU) |
            (mmc5a_ ? 1U : 0U) | (irqLine() ? 0x80U : 0U));
        pcmIrqTrip_ = false;
        return result;
    }
    if (address == 0x5015) {
        return static_cast<std::uint8_t>((value & 0xFCU) |
            (pulses_[0].length ? 1U : 0U) | (pulses_[1].length ? 2U : 0U));
    }
    if ((pcmControl_ & 1U) && address >= 0x8000 && address <= 0xBFFF) writePcm(value);
    return value;
}

/** @brief Записи high сбрасывают phase/envelope, но не timer divider. */
void Mmc5Audio::cpuWrite(std::uint16_t address, std::uint8_t value) noexcept {
    if (address >= 0x5000 && address <= 0x5007) {
        flushPending();
        auto& pulse = pulses_[(address >> 2U) & 1U];
        switch (address & 3U) {
        case 0: pulse.control = value; break;
        case 2: pulse.period = static_cast<std::uint16_t>((pulse.period & 0x700U) | value); break;
        case 3:
            pulse.period = static_cast<std::uint16_t>((pulse.period & 0xFFU) | ((value & 7U) << 8U));
            if (pulse.enabled) pulse.length = lengths[value >> 3U];
            pulse.sequence = 0;
            pulse.envelopeStart = true;
            break;
        default: break; // MMC5 не имеет sweep unit.
        }
        refreshDeadline();
    } else if (address == 0x5010) {
        pcmControl_ = value & 0x81U;
    } else if (address == 0x5011) {
        if (!(pcmControl_ & 1U)) writePcm(value);
    } else if (address == 0x5015) {
        flushPending();
        for (unsigned i = 0; i != pulses_.size(); ++i) {
            pulses_[i].enabled = (value & (1U << i)) != 0;
            if (!pulses_[i].enabled) pulses_[i].length = 0;
        }
        refreshDeadline();
    }
    // Любая pulse запись — внешний барьер, включая period1↔2 без raw DAC delta.
    if((address>=0x5000&&address<=0x5007)||address==0x5015)refreshSelectedSample();
}

/** @brief Получение уровня не clock, не статус-read и не acknowledge IRQ. */
Mmc5Audio::Levels Mmc5Audio::levels() const noexcept {
    return {pulses_[0].output(), pulses_[1].output(), pcm_};
}

/** @brief Целочисленный номинальный gain не подменяет аппаратную калибровку. */
#if defined(__GNUC__) || defined(__clang__)
// Сохранить resident leaf для raw fallback: inlining сюда затягивал Flash levels().
[[gnu::noinline]]
#endif
std::int32_t Mmc5Audio::sample() const noexcept {
    const auto level = levels();
    return -pulseUnitQ16 * (static_cast<std::int32_t>(level.pulse1) + level.pulse2)
           -pcmUnitQ16 * level.pcm;
}

/** @brief Доля duty не зависит от отложенной raw-фазы; length/envelope не пересекли frame deadline. */
std::int32_t Mmc5Audio::portableSample() const noexcept {
    return cachedSampleQ16_;
}

/** @brief Новый срок отбрасывает только carrier edges; базовый frame/noncarrier срок уменьшается на уже прошедший pending. */
std::uint32_t Mmc5Audio::nextPortableOutputCpuCycles() const noexcept {
    const bool average[2] = {pulses_[0].portableCarrier(), pulses_[1].portableCarrier()};
    if (!average[0] && !average[1]) return nextOutputCpuCycles();
    auto result = static_cast<std::uint32_t>(frameRemaining_);
    for (unsigned i = 0; i != pulses_.size(); ++i) {
        const auto& pulse = pulses_[i];
        const auto volume = (pulse.control & 0x10U) ? pulse.control & 15U : pulse.decay;
        if (!average[i] && pulse.length && volume)
            result = std::min(result, dutyChangeCpuCycles(pulse.control, pulse.sequence,
                                                         pulse.timer, pulse.period, halfPhase_));
    }
    // pending строже raw ближайшего фронта; portable min не может быть раньше raw.
    return result - pendingCpu_;
}


/** @details Сумма eighths округляется один раз после обоих pulse. Carrier-only
 * raw edges не вызывают этот helper; PCM delta обновляется отдельно линейно. */
void Mmc5Audio::refreshSelectedSample() noexcept {
    const bool average[2] = {pulses_[0].portableCarrier(), pulses_[1].portableCarrier()};
    if (!average[0] && !average[1]) { cachedSampleQ16_=sample();return; }
    std::int32_t pulseEighths = 0;
    for (unsigned i = 0; i != pulses_.size(); ++i) {
        const auto& pulse = pulses_[i];
        if (average[i]) {
            const auto volume = (pulse.control & 0x10U) ? pulse.control & 15U : pulse.decay;
            pulseEighths += static_cast<std::int32_t>(volume * dutyOn[pulse.control >> 6U]);
        } else pulseEighths += 8 * static_cast<std::int32_t>(pulse.output());
    }
    cachedSampleQ16_=-(pulseUnitQ16 * pulseEighths / 8) - pcmUnitQ16 * pcm_;
}

/** @brief Countdown включает ближайший clock, а не последний безопасный tick. */
std::uint32_t Mmc5Audio::nextEventCpuCycles() const noexcept {
    auto result = static_cast<std::uint32_t>(frameRemaining_ - pendingCpu_);
    const auto halfTicks = (static_cast<std::uint32_t>(pendingCpu_) + static_cast<unsigned>(halfPhase_)) / 2U;
    const auto phase = (static_cast<unsigned>(pendingCpu_) + static_cast<unsigned>(halfPhase_)) & 1U;
    for (const auto& pulse : pulses_) {
        // Публичный срок включает и неактивные timer edges, как скалярный эталон.
        const auto timer = halfTicks <= pulse.timer
            ? static_cast<std::uint32_t>(pulse.timer) - halfTicks
            : pulse.period - (halfTicks - pulse.timer - 1U) % (static_cast<std::uint32_t>(pulse.period) + 1U);
        const auto distance = 2U * (timer + 1U) - phase;
        result = std::min(result, distance);
    }
    return result;
}

} // namespace nes::core
