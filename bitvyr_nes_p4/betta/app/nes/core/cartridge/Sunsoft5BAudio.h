/**
 * @file Sunsoft5BAudio.h
 * @brief Собственная YM2149-подобная модель Sunsoft5B: tone/noise/envelope и Q16 DAC.
 */
#pragma once
#include <array>
#include <cstdint>
namespace nes::core {
/**
 * @brief Single-owner PSG без IRQ, ROM, SDK, callback и allocation.
 * @details Цифровые делители: tone/envelope M2/16, noise M2/32; нулевой period=1.
 * sample() даёт raw DAC. portableSample() — явно приближённый boxcar64 M2,
 * задержка до64 тактов, без остановки генераторов и изменения цифрового mapper.
 * Gain12000/channel и логарифмическая DAC-лестница — номинальная аналоговая модель,
 * не измеренная передаточная характеристика физической платы.
 */
class Sunsoft5BAudio final {
public:
    static constexpr std::uint32_t portablePeriodCpu=64;
    static constexpr std::int32_t maximumChannelQ16=12000;
    /** @brief Создаёт определённое cold-состояние; аппаратный случайный power-on не моделируется. */
    Sunsoft5BAudio() noexcept { reset(); }
    /** @brief Сбрасывает registers, фазы, LFSR и оба DAC; новая ROM не заимствует звук старой. */
    void reset() noexcept;
    /** @brief Принимает реальный bus write: C000-DFFF select, E000-FFFF data; часы не продвигает. */
    void cpuWrite(std::uint16_t address,std::uint8_t value) noexcept;
    /**
     * @brief Учитывает действительно прошедшие M2, включая DMA/DMC; caller сначала clock, затем bus strobe.
     * @details Разбиение count сохраняет результат. Между внешними MMIO ограничивать
     * batching своим цифровым IRQ horizon; audio deadline не разрешает пропустить CPU bus.
     */
    void clockCpu(std::uint32_t cycles=1) noexcept;
    /** @brief Возвращает точный DAC этой цифровой модели, Q16 -36000..0, без изменения состояния. */
    [[nodiscard]] std::int32_t sample()const noexcept { return rawSample_; }
    /** @brief Возвращает signed Q16 среднее предыдущего полного64-M2 блока; не продвигает часы. */
    [[nodiscard]] std::int32_t portableSample()const noexcept { return portableSample_; }
    /** @brief Даёт положительный остаток до ближайшего внутреннего16-M2 тика, без разрешения quiet CPU. */
    [[nodiscard]] std::uint32_t nextOutputCpuCycles()const noexcept { return dividerRemaining_; }
    /** @brief Даёт1..64 M2 до публикации следующего boxcar; MMIO не меняет portable DAC немедленно. */
    [[nodiscard]] std::uint32_t nextPortableOutputCpuCycles()const noexcept { return portableRemaining_; }
private:
    /** @brief Выполняет общий16-M2 tick, сохраняя работу заглушённых генераторов. */
    void tick() noexcept;
    /** @brief Исполняет один шаг общего32-уровневого envelope и все16 CAaH shapes. */
    void tickEnvelope() noexcept;
    /** @brief Пересчитывает только DAC; register writes не являются дополнительным clock. */
    void refreshSample() noexcept;
    std::array<std::uint8_t,16> registers_{};
    std::array<std::uint16_t,3> toneCounter_{};
    std::array<bool,3> toneHigh_{};
    std::uint32_t noiseLfsr_=1;
    std::uint16_t envelopeCounter_=0;
    std::uint8_t selected_=0,noiseCounter_=0,envelopeStep_=0,envelopeLevel_=31;
    bool noiseHalf_=false,envelopeAttack_=false,envelopeHolding_=false;
    std::uint8_t dividerRemaining_=16,portableRemaining_=64;
    std::int32_t rawSample_=0,portableSample_=0;
    // Модель ограничена |DAC|<=49152; площадь64 M2 <=3145728, точно в int32.
    std::int32_t portableArea_=0;
};
} // namespace nes::core

