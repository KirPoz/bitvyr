#include "FixedFilter.h"
#include <limits>

namespace nes::audio_probe { namespace {
constexpr std::int64_t q27 = 1LL << 27;
constexpr std::int64_t q31 = 1LL << 31;
// Эти Q1.31 числа ТОЧНО представляют binary32 литералы текущего Apu::emitSample.
constexpr std::int32_t hp90 = 2122332288, hp440 = 2027293312, lp14000 = 1803890560;

/** @brief Округляет знаковое частное к ближайшему, ties от нуля; аргументы исключают INT64_MIN. */
std::int64_t roundedDivide(std::int64_t value, std::int64_t divisor) noexcept {
    return value >= 0 ? (value + divisor / 2) / divisor : -((-value + divisor / 2) / divisor);
}
/** @brief Умножает bounded Q5.27 промежуточное значение на Q1.31 без signed overflow. */
bool multiply(std::int64_t value, std::int32_t coefficient, std::int64_t& result) noexcept {
    if (value > 4294967295LL || value < -4294967295LL) return false;
    result = roundedDivide(value * coefficient, q31);
    return true;
}
/** @brief Проверяет сужение state после полного int64 выражения; saturating state не применяется. */
bool fits(std::int64_t value) noexcept {
    return value >= std::numeric_limits<std::int32_t>::min() &&
        value <= std::numeric_limits<std::int32_t>::max();
}
} // namespace

bool fixedSample(FixedState& state, std::int64_t integral, std::int16_t& pcm) noexcept {
    if (integral > integralLimit || integral < -integralLimit) return false;
    const std::int64_t input = roundedDivide(integral * 4096LL, 65535LL);
    std::int64_t first{}, second{}, delta{};
    if (!multiply(std::int64_t(state.highPass90) + input - state.previousInput, hp90, first) ||
        !multiply(std::int64_t(state.highPass440) + first - state.previousHighPass90, hp440, second) ||
        !multiply(second - state.lowPass14000, lp14000, delta)) return false;
    const std::int64_t low = std::int64_t(state.lowPass14000) + delta;
    if (!fits(input) || !fits(first) || !fits(second) || !fits(low)) return false;
    const auto scaled = low * 30000LL / q27; // C++17: truncation toward zero, затем старые PCM limits.
    pcm = static_cast<std::int16_t>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
    state = {static_cast<std::int32_t>(first), static_cast<std::int32_t>(second),
        static_cast<std::int32_t>(low), static_cast<std::int32_t>(input), static_cast<std::int32_t>(first)};
    return true;
}

bool fixedBlock(FixedState& state, const std::int64_t* integral, std::size_t count,
    std::int16_t* pcm) noexcept {
    if (count > sampleLimit || (count && (!integral || !pcm))) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (integral[i] > integralLimit || integral[i] < -integralLimit) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (!fixedSample(state, integral[i], pcm[i])) return false;
    return true;
}
} // namespace nes::audio_probe
