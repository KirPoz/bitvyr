#pragma once
#include <cstddef>
#include <cstdint>

namespace nes::audio_probe {
/** @brief Пять binary32 состояний точного фильтра; только независимый диагностический эталон. */
struct FloatState final {
    float highPass90, highPass440, lowPass14000, previousInput, previousHighPass90;
};
/** @brief Повторяет исходные emitSample float операции; никаких FIR/guest clocks не выполняет. */
void floatBlock(FloatState& state, const std::int64_t* integral,
    std::size_t count, std::int16_t* pcm) noexcept;
} // namespace nes::audio_probe
