#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

/** @brief CRC32 IEEE/zlib; immutable1024Б table создаётся компилятором, без runtime init/heap. */
namespace nes::c6raw::crc_detail {
/** @brief Вычисляет одну таблицу перехода для байта полиномом0xEDB88320. */
constexpr std::uint32_t entry(std::uint32_t value) noexcept {
    for(unsigned bit=0;bit<8;++bit)value=(value>>1U)^(0xedb88320U&(0U-(value&1U)));
    return value;
}
/** @brief Один источник всех256 переходов; не содержит platform/SDK зависимостей. */
constexpr std::array<std::uint32_t,256> makeTable() noexcept {
    std::array<std::uint32_t,256> result{};for(unsigned value=0;value<256;++value)result[value]=entry(value);return result;
}
inline constexpr auto table=makeTable();
/** @brief Возвращает IEEE CRC; previous — уже инвертированный итог предыдущего блока, как zlib.crc32(data, previous).
 * @pre data заимствован до возврата; nullptr допускается только при bytes0. CRC не является аутентификацией.
 */
constexpr std::uint32_t compute(const std::uint8_t* data,std::size_t bytes,std::uint32_t previous=0) noexcept {
    std::uint32_t value=~previous;
    for(std::size_t i=0;i<bytes;++i)value=(value>>8U)^table[(value^data[i])&255U];
    return ~value;
}
inline constexpr std::array<std::uint8_t,9> checkBytes{49,50,51,52,53,54,55,56,57};
static_assert(compute(checkBytes.data(),checkBytes.size())==0xcbf43926U,"IEEE standard check");
static_assert(compute(nullptr,0,0x13579bdfU)==0x13579bdfU,"Empty stream preserves previous CRC");
}
