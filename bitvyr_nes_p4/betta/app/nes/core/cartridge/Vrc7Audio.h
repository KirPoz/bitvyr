/**
 * @file Vrc7Audio.h
 * @brief Собственный bounded portable FM VRC7: шесть2-operator голосов и register/timing модель.
 */
#pragma once
#include <array>
#include <cstdint>
namespace nes::core {
/**
 * @brief Single-owner FM без ROM/IRQ/SDK, heap и runtime floating point.
 * @details Это НЕ bit-exact OPLL: ADSR/KSR/KSL/AM/PM и gain портативно аппроксимированы,
 * нет silicon pipeline/damping/TDM/DAC clipping. Fixed patches — опубликованные
 * аппаратные instrument-ROM bytes, не код чужого emulator. Детерминированный tick
 * каждые36 M2 приближает независимый3.58MHz crystal приNTSC; PAL crystal pitch
 * требует отдельного clock-domain adapter. CPU/IRQ никогда не пропускаются.
 */
class Vrc7Audio final {
public:
    static constexpr std::uint32_t synthesisPeriodCpu=36,portablePeriodCpu=64;
    static constexpr std::int32_t maximumChannelQ16=8192;
    /** @brief Создаёт silent cold-состояние; случайный аппаратный power-on не имитируется. */
    Vrc7Audio() noexcept { reset(); }
    /** @brief Сбрасывает patch/channel/LFO/фазы и DAC для новой ROM. */
    void reset() noexcept;
    /** @brief Принимает canonical9010(select)/9030(data), E000bit6(reset); декодирование A3/A4 принадлежит mapper. */
    void cpuWrite(std::uint16_t address,std::uint8_t value) noexcept;
    /**
     * @brief Применяет E000bit6: assert очищает registers/envelopes/tremolo, сохраняя vibrato.
     * @details Пока reset удерживается, FM writes игнорируются, M2/oscillator продолжают
     * учитываться. Raw DAC=0 сразу; portable latency до следующей64-M2 границы сохранена.
     */
    void setMuted(bool resetAsserted) noexcept;
    /** @brief Учитывает прошедшие M2; batching не переносится через CPU/MMIO/цифровой IRQ horizon. */
    void clockCpu(std::uint32_t cycles=1) noexcept;
    /** @brief Даёт текущий nominal inverted unipolar Q16 DAC -49152..0; это reference данной portable FM модели. */
    [[nodiscard]] std::int32_t sample()const noexcept { return rawSample_; }
    /** @brief Даёт64-M2 boxcar предыдущего блока; carrier DC bias удаляет существующий общий HPF. */
    [[nodiscard]] std::int32_t portableSample()const noexcept { return portableSample_; }
    /** @brief Даёт1..36 M2 до следующего synthesis tick; не является quiet IRQ grant. */
    [[nodiscard]] std::uint32_t nextOutputCpuCycles()const noexcept { return synthesisRemaining_; }
    /** @brief Даёт1..64 M2 до следующей portable публикации. */
    [[nodiscard]] std::uint32_t nextPortableOutputCpuCycles()const noexcept { return portableRemaining_; }
private:
    enum class Envelope :std::uint8_t { Off,Attack,Decay,Sustain,Release };
    /** @brief Постоянное состояние одного FM operator; attenuation в0.375dB/Q12, phase по модулю2^32. */
    struct Operator final {std::uint32_t phase=0,attenuation=127U*4096U;Envelope envelope=Envelope::Off;};
    /** @brief Производные одного operator: phase step, attenuation, ADSR rates и flags; пересчитываются из текущих регистров. */
    struct Parameters final {
        std::uint32_t phaseStep=0,sustainTarget=0;
        std::uint16_t externalAttenuation=0;
        std::uint8_t attack=0,decay=0,release=0,flags=0;
    };
    /** @brief Два operator и их производные от регистров; кэш обновляется до следующего tick, без heap/заимствований. */
    struct Channel final {std::array<Operator,2> op{};std::array<Parameters,2> parameters{};std::int32_t previous=0,older=0;};
    /** @brief Чистит register/channel часть reset, не трогая непрерывный vibrato clock и boxcar фазу. */
    void clearRegisters() noexcept;
    /** @brief Выполняет один36-M2 synthesis tick с целочисленными LUT, не изменяя цифровой mapper. */
    void tick() noexcept;
    /** @brief Пишет допустимый внутренний register, edge-sensitive key events без clock advancement. */
    void writeRegister(std::uint8_t index,std::uint8_t value) noexcept;
    /** @brief Выполняет edge key-on/off; повтор того же trigger не retrigger. */
    void keyChange(unsigned channel,bool on) noexcept;
    /** @brief Пересчитывает только зависящие от patch/fnum/control/volume величины; время и envelope не продвигает. */
    void refreshParameters(unsigned channel) noexcept;
    /** @brief Продвигает один envelope в аппроксимации, сохраняя rate0 halt и key semantics. */
    void envelopeTick(Operator&op,const Parameters&parameters) noexcept;
    /** @brief Возвращает Q15 amplitude из envelope/volume/KSL/AM, не меняя времени. */
    [[nodiscard]] std::int32_t amplitude(const Operator&op,const Parameters&parameters,unsigned tremolo)const noexcept;
    std::array<std::uint8_t,64> registers_{};
    std::array<Channel,6> channels_{};
    std::uint16_t tremoloPhase_=0,vibratoPhase_=0;
    std::uint8_t selected_=0,synthesisRemaining_=36,portableRemaining_=64;
    bool muted_=false;
    std::int32_t rawSample_=0,portableSample_=0;
    // Модель ограничена |DAC|<=49152; площадь64 M2 <=3145728, точно в int32.
    std::int32_t portableArea_=0;
};
} // namespace nes::core

