/**
 * @file OutputFrame.h
 * @brief Ограниченные аренды output-only владельца; атомарная передача — у адаптера.
 * @details Собственный переносимый C++17. Никаких задач, SDK, выделений или ожиданий.
 */
#pragma once
#include "Types.h"
#include "cpu/Cpu6502.h"
#include "ppu/VideoPacket.h"
#include <array>
#include <cstddef>
#include <cstdint>

#if !(defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU) || \
    !(defined(NES_CORE_FRAME_RAM_SNAPSHOT) && NES_CORE_FRAME_RAM_SNAPSHOT) || \
    !(defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM)
#error "Output frame lease requires split PPU, RAM snapshot and explicit transport lifetime"
#endif

namespace nes::core {
class Machine;
/** @brief Защищённый реальный внешний диапазон; null/0 означает незанятую запись. */
struct OutputProtectedSpan { const void* data=nullptr; std::size_t bytes=0; };
/**
 * @brief Идентичность первичного владельца без права читать его параллельно JOB.
 * @details romEpoch принадлежит загрузчику, generation — App, transportEpoch —
 * этому Core-протоколу. Все ненулевые; копия не удостоверяет реальный Worker join.
 */
struct OutputFrameLease {
    const Machine* producer=nullptr;
    const std::uint8_t* rom=nullptr;
    std::size_t romBytes=0;
    std::uint64_t romEpoch=0,generation=0,transportEpoch=0;
    Region region=Region::Ntsc;
};
/**
 * @brief Два packet, два RAM результата и фиксированный список внешних заимствований.
 * @details snapshot — реальные отдельные writable выделения по10240 байт.
 * protectedSpans обязан включать все paired AudioPacket, RGB, VideoStorage и
 * внешний control, пока они могут быть заимствованы. Это запреты alias, а не
 * право освобождать ресурс. Все диапазоны и ROM сохраняются до успешного cancel.
 */
struct OutputFrameBinding {
    OutputFrameLease source{}; // transportEpoch входа должен быть0; новый выдаёт Core.
    std::array<VideoPacket*,2> packets{};
    std::array<std::uint8_t*,2> snapshots{};
    std::array<OutputProtectedSpan,8> protectedSpans{};
};
/**
 * @brief Идентичность одного acquire/release задания внешнего двухслотового bridge.
 * @details slot принадлежит[0,2), sequence начинается0, inputFrame — монотонный
 * внешний кадр. baseCycle — реальный guest cut, первый равен7, не video start0.
 */
struct OutputFrameTicket {
    OutputFrameLease lease{};
    std::uint64_t sequence=0,inputFrame=0,baseCycle=0;
    unsigned slot=0;
};
/** @brief Неизменяемый настоящий cut; не скрытый C0 PPU witness и не LCD completion. */
struct OutputFrameCut {
    Cpu6502::State cpu{};
    std::uint64_t baseCycle=0,endCycle=0,ppuFrame=0,videoStart=0,videoGeneration=0;
    std::int16_t scanline=0;
    std::uint16_t dot=0;
    std::uint8_t remainder=0;
};
} // namespace nes::core
