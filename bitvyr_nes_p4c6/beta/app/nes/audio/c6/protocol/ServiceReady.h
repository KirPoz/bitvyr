#pragma once
#include "apu/AudioRenderer.h"
#include "AudioPacketWire.h"
#include <array>

namespace nes::c6audio {
/**
 * @brief Конечное typed хранилище caller, не полная ёмкость AudioPacket.
 * @details Размеры выбирает service adapter; это не обещание вместимости любой ROM.
 * Объекты records существуют весь срок storage, никаких raw-byte casts/lifetime UB.
 */
template<std::size_t Inputs,std::size_t Dmc,std::size_t Expansion>
struct UsedStorage final {
    static_assert(Inputs<=core::AudioPacket::inputCapacity&&Dmc<=core::AudioPacket::dmcCapacity&&
        Expansion<=core::AudioPacket::expansionCapacity,"original protocol limits");
    core::AudioPacket::Header header{};
    std::array<core::AudioPacket::Input,Inputs> input{};
    std::array<core::AudioPacket::Dmc,Dmc> dmc{};
    std::array<core::AudioPacket::Expansion,Expansion> expansion{};
    /** @brief Заимствует настоящие typed extents; caller удерживает storage до конца DSP job. */
    wire::Destination destination() noexcept {
        return {&header,input.data(),input.size(),dmc.data(),dmc.size(),expansion.data(),expansion.size()};
    }
};
/** @brief Синхронный результат без передачи/освобождения caller/RawChannel frame ticket. */
struct BorrowedResult final {
    core::AudioRenderResult status=core::AudioRenderResult::InvalidPacket;
    std::uint64_t generation{},sequence{},ppuFrame{},baseCycle{},endCycle{};
    core::Region region=core::Region::Ntsc;
    const std::int16_t* pcm{};
    std::size_t samples{};
};
/**
 * @brief Собирает один готовый used-record wire в typed storage и синхронно считает полный DSP.
 * @param bytes Полностью собранный immutable wire; Raw CRC/key/lease уже проверены адаптером.
 * @param count Точная длина wire; caller не объявляет incomplete chunks полным кадром.
 * @param expected Audio-domain Header key, отдельно от внешнего App generation/lease/session.
 * @param storage Свободные typed arrays/Header caller; не пересекают wire/result/renderer.
 * @param renderer Единственный постоянный DSP-владелец; stream должен начинаться generation/seq/base0.
 * @param result Метаданные/borrowed PCM при успехе; PCM жив лишь до следующего render/reset.
 * @return false на decode/render fault, никогда не публикует частичный PCM как success.
 * @details Это skeleton, НЕ сетевой backend: внешний owner запрещает повторный вызов до реального
 * потребления PCM/TXdone, удерживает source/context при timeout и сам проверяет полный frame ticket.
 * Reset/автоматическое восстановление stale P4 renderer здесь не предоставляются.
 */
[[nodiscard]] bool renderReady(const std::uint8_t* bytes,std::size_t count,const wire::Expected& expected,
    const wire::Destination& storage,core::AudioRenderer& renderer,BorrowedResult& result) noexcept;
} // namespace nes::c6audio
