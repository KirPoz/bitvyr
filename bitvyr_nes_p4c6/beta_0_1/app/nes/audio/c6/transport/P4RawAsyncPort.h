#pragma once
#include "RawChannel.h"
#include <cstdint>

namespace nes::c6audio::p4 {
namespace raw=::nes::c6raw;
/**
 * @brief На Main один раз выделяет boot-long Channel в HP internal DMA8BIT.
 * @return nullptr при OOM/capability/повторном чужом owner; SDK/ROM/частоты не меняет.
 * @details Один Main вызывает до первого Raw TX; после Hosted init допустимо.
 * Fully constructed Channel публикуется RX через release/acquire. Нет destroy: callbacks/timeout могут жить
 * до P4 reset. Это одно выделение boot session, не утечка на каждый ROM/кадр.
 */
raw::Channel* initializeMainChannel() noexcept;
/** @brief SDK monotonic microseconds для start/completion, не Pending loop. */
std::uint64_t clockMicroseconds() noexcept;
} // namespace nes::c6audio::p4
