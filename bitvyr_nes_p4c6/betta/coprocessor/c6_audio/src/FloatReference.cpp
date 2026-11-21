#include "FloatReference.h"

namespace nes::audio_probe {
void floatBlock(FloatState& state, const std::int64_t* integral,
    std::size_t count, std::int16_t* pcm) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const auto outputIntegral = integral[i];
        const float integrated = outputIntegral >= (-2147483647LL - 1) && outputIntegral <= 2147483647LL
            ? static_cast<float>(static_cast<std::int32_t>(outputIntegral)) : static_cast<float>(outputIntegral);
        const float input = integrated * (1.0F / (65535.0F * 32768.0F));
        state.highPass90 = 0.988288F * (state.highPass90 + input - state.previousInput);
        state.previousInput = input;
        state.highPass440 = 0.944032F * (state.highPass440 + state.highPass90 - state.previousHighPass90);
        state.previousHighPass90 = state.highPass90;
        state.lowPass14000 += 0.840002F * (state.highPass440 - state.lowPass14000);
        float scaled = state.lowPass14000 * 30000.0F;
        if (scaled > 32767.0F) scaled = 32767.0F;
        if (scaled < -32768.0F) scaled = -32768.0F;
        pcm[i] = static_cast<std::int16_t>(scaled);
    }
}
} // namespace nes::audio_probe
