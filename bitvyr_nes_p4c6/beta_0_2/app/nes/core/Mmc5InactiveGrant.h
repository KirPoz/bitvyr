#pragma once
#include <algorithm>

namespace nes::core {
/** @brief Возвращает число полных M2 до ближайшей жёсткой границы неактивного PPU.
 * @pre NTSC, row -1..260, dot 0..340; рендеринг выключен или row >= 240.
 * @details При 3 dots/M2 такт, затрагивающий dot1 vblank/предрендеринга,
 * остаётся скалярному пути. Строка 261 обозначает следующий предрендеринг.
 * Отключение рендеринга и разрешённый хвост до предрендеринга не дают новых /RD.
 */
constexpr unsigned mmc5InactiveGrantClocks(int row,unsigned dot,unsigned requested) noexcept {
    const auto frontier = row==-1&&dot<=1U ? -1 : row<241||(row==241&&dot<=1U) ? 241 : 261;
    const auto dots=static_cast<unsigned>(frontier-row)*341U+1U-dot;
    return std::min(requested,dots/3U);
}
}  // namespace nes::core
