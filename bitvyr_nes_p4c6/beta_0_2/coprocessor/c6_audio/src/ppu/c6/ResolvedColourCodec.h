#pragma once
#include "ResolvedColourRun.h"
namespace nes::c6_colour::wire {
/** @brief Version1 seed: version8,pal8,palette32,8×(low,high,x,attributes,flags); ровно74Б. */
constexpr std::size_t seedBytes=74;
/** @brief Version1 run:24Б header + tiles×3Б fetched; maximum120Б, без native padding. */
constexpr std::size_t runHeaderBytes=24,maxRunBytes=120;
/** @brief Кодирует immutable seed в caller buffer; false до stores при bad capacity/flags. */
bool encodeSeed(const RowSeed&,std::uint8_t* out,std::size_t capacity,std::size_t& written) noexcept;
/** @brief Проверяет exact seed length/version/PAL/flags, затем публикует DTO; false без mutation. */
bool decodeSeed(const std::uint8_t*,std::size_t bytes,RowSeed& out) noexcept;
/** @brief Кодирует run header и только tiles использованных triples; false без output mutation. */
bool encodeRun(const Run&,std::uint8_t* out,std::size_t capacity,std::size_t& written) noexcept;
/** @brief Проверяет bounds/reserved/exactlength/selector и публикует DTO после полного успеха. */
bool decodeRun(const std::uint8_t*,std::size_t bytes,Run& out) noexcept;
}
