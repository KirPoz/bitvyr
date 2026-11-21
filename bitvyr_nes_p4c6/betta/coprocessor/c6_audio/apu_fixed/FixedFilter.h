#pragma once
#include <cstddef>
#include <cstdint>

/** @brief Собственный portable IIR leaf; не исполняет игровые clocks, каналы APU или DMC. */
namespace nes::audio_probe {
/** @brief Пять состояний signed Q5.27; поток владеет экземпляром единолично. */
struct FixedState final {
    std::int32_t highPass90{}, highPass440{}, lowPass14000{}, previousInput{}, previousHighPass90{};
};
constexpr std::size_t sampleLimit = 800;
constexpr std::int64_t integralLimit = 4LL * 65535LL * 32768LL;
/**
 * @brief Преобразует один реальный outputIntegral в portable PCM и новое состояние.
 * @param state Исключительно заимствованные пять Q5.27 состояний; при отказе не меняются.
 * @param integral Значение после hybrid integrator/16384, до float-нормализации APU.
 * @param pcm Получает saturating int16 только при успехе.
 * @return false при входе вне |normalized input|<=4 или непредставимом промежуточном состоянии.
 * @details Все произведения signed int64; отрицательные значения не сдвигаются влево.
 * Это отдельный PCM-профиль, не обещание побитового равенства IEEE754.
 */
[[nodiscard]] bool fixedSample(FixedState& state, std::int64_t integral, std::int16_t& pcm) noexcept;
/**
 * @brief Обрабатывает до800 последовательных отсчётов без аллокации и программного float.
 * @details Весь вход проверяется до первой мутации. При последующем state fault уже выданный
 * префикс принадлежит вызывающему, но не является завершённым результатом job; повтор запрещён.
 * @param state Состояние единственного DSP-владельца, сохраняемое между chunks.
 * @param integral Неизменяемый массив count реальных integral; жив до возврата.
 * @param count Число отсчётов0..800, а не число guest CPU clocks.
 * @param pcm Исключительно заимствованный массив не менее count элементов, не пересекает integral.
 * @return true только после полного последовательного преобразования.
 */
[[nodiscard]] bool fixedBlock(FixedState& state, const std::int64_t* integral,
    std::size_t count, std::int16_t* pcm) noexcept;
} // namespace nes::audio_probe
