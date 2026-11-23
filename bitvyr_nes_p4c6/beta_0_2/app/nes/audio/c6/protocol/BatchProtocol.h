#pragma once
#include <cstddef>
#include <cstdint>

namespace nes::c6audio::batch {
/** @brief Единственные PRIVATE fast opcodes; legacy40..44 остаются прежними. */
inline constexpr std::uint16_t submitOpcode=0x45,fetchOpcode=0x46;
/** @brief Fast request prefix20 + usedwire; common32 находится вне server PCMbody. */
inline constexpr std::size_t submitPrefixBytes=20,maximumSubmitWireBytes=1316;
/** @brief Raw1792−header64−common32−PCMheader32 в signed16 samples. */
inline constexpr std::size_t maximumFastSamples=832,maximumFastBodyBytes=32+2*maximumFastSamples;
/** @brief Wire flags: host-consumed вrequest; last/previousApplied в PCMreply. */
inline constexpr std::uint16_t previousConsumedFlag=1;
inline constexpr std::uint32_t lastPcmFlag=1,previousConsumedAppliedFlag=2;
} // namespace nes::c6audio::batch
