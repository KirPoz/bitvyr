#pragma once

#include "c6/protocol/FrameService.h"
#include "c6/runtime/AudioPublished.h"
#include "apu/Apu.h"
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nes::audio {

/**
 * @brief Main-only проверка полного PCM-потока без привязки к игре или input movie.
 * @details Проверяет identity, непрерывность M2 и точное число выходных сэмплов.
 * Не читает PCM, не вычисляет его CRC и не заменяет wireCRC, публикацию,
 * CONSUMED, настоящий join, проверку синтеза либо владение I2S-буфером.
 * Числовое состояние не владеет памятью; его сброс не освобождает ресурсы.
 */
class PcmStreamGuard final {
public:
    using Binding = ::nes::c6audio::service::Binding;
    using Key = ::nes::c6audio::async::PublishedKey;

    /**
     * @brief Связывает новый пустой observer с подтверждённым потоком один раз.
     * @param binding Полные поколения/lease/epoch/session и режим APU из cold admission.
     * @return false без изменения состояния при повторе или неверном binding.
     * @pre Вызывается Main до первого PCM; готовность C6 подтверждает внешний adapter.
     */
    bool bind(const Binding& binding) noexcept {
        if (bound_ || !binding.appGeneration || !binding.romLease || !binding.romEpoch ||
            !binding.session || !binding.audioGeneration || !binding.firstCommandSequence ||
            binding.firstCommandSequence == std::numeric_limits<std::uint32_t>::max() ||
            (binding.region != core::Region::Ntsc && binding.region != core::Region::Pal) ||
            (binding.audioCapabilities & ~core::AudioPacket::externalDacQ16)) return false;
        binding_ = binding;
        bound_ = true;
        return true;
    }

    /**
     * @brief Принимает полный PCM_N перед первой I2S-выдачей или проверяет его повтор.
     * @param key Полный опубликованный ключ, проверенный transport/session adapter.
     * @param pcm Начало всего immutable PCM_N, а не оставшегося после partial I2S хвоста.
     * @param samples Полное число mono16 сэмплов PCM_N; ёмкость подтверждает внешний adapter.
     * @return true для следующего корректного кадра или неизменного повторного ticket.
     * @details Фаза вычисляется целочисленно из M2; повтор не продвигает её ещё раз.
     * Любой отказ оставляет observer неизменным. Метод не даёт права вернуть storage.
     */
    bool observe(const Key& key, const std::int16_t* pcm, std::size_t samples) noexcept {
        if (!bound_ || (!pcm && samples) || samples > core::Apu::sampleCapacity ||
            reinterpret_cast<std::uintptr_t>(pcm) % alignof(std::int16_t) || key.arena > 1 ||
            key.appGeneration != binding_.appGeneration || key.romLease != binding_.romLease ||
            key.romEpoch != binding_.romEpoch || key.session != binding_.session ||
            key.audio.generation != binding_.audioGeneration || key.audio.region != binding_.region ||
            key.audio.capabilities != binding_.audioCapabilities) return false;

        if (haveLast_ && key.inputFrame == lastKey_.inputFrame)
            return ::nes::c6audio::async::same(key, lastKey_) && pcm == lastPcm_ && samples == lastSamples_;

        const auto& a = key.audio;
        if (key.inputFrame != nextInput_ || key.inputFrame == std::numeric_limits<std::uint64_t>::max() ||
            a.sequence != key.inputFrame || a.ppuFrame != key.inputFrame + 1 ||
            a.baseCycle != nextBase_ || a.endCycle <= a.baseCycle || a.endCycle - a.baseCycle >= 45000)
            return false;

        const std::uint32_t cpuRate = binding_.region == core::Region::Pal ? 1662607U : 1789773U;
        // delta<45000 и constexpr sampleRate гарантируют представимость произведения.
        const std::uint64_t numerator = phase_ + (a.endCycle - a.baseCycle) * core::Apu::sampleRate;
        const auto expected = numerator / cpuRate;
        if (expected != samples) return false;

        phase_ = static_cast<std::uint32_t>(numerator % cpuRate);
        nextBase_ = a.endCycle;
        ++nextInput_;
        lastKey_ = key;
        lastPcm_ = pcm;
        lastSamples_ = samples;
        haveLast_ = true;
        return true;
    }

    /** @brief Число принятых полных PCM tickets; повторная partial-I2S выдача его не увеличивает. */
    std::uint64_t frames() const noexcept { return nextInput_; }

private:
    Binding binding_{};
    Key lastKey_{};
    const std::int16_t* lastPcm_{};
    std::size_t lastSamples_{};
    std::uint64_t nextInput_{}, nextBase_{};
    std::uint32_t phase_{};
    bool bound_{}, haveLast_{};
};

} // namespace nes::audio
