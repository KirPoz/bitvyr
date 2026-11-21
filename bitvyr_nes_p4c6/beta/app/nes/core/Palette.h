#pragma once

/**
 * @file Palette.h
 * @brief Общая RGB-палитра и преобразование индекса NES в цвет дисплея.
 * @ingroup nes_ppu
 *
 * Это чистые функции: они не обращаются к шине, не меняют состояние PPU
 * и одинаково используются обычным и раздельным выводом.
 */

#include <cstdint>

namespace nes::core {
// Условная RGB-палитра дисплея; композитный сигнал и поведение ЭЛТ не моделируются.
inline constexpr std::uint32_t rgbPalette[64] = {
    0x666666,0x002a88,0x1412a7,0x3b00a4,0x5c007e,0x6e0040,0x6c0600,0x561d00,
    0x333500,0x0b4800,0x005200,0x004f08,0x00404d,0x000000,0x000000,0x000000,
    0xadadad,0x155fd9,0x4240ff,0x7527fe,0xa01acc,0xb71e7b,0xb53120,0x994e00,
    0x6b6d00,0x388700,0x0c9300,0x008f32,0x007c8d,0x000000,0x000000,0x000000,
    0xfffeff,0x64b0ff,0x9290ff,0xc676ff,0xf36aff,0xfe6ecc,0xfe8170,0xea9e22,
    0xbcbe00,0x88d800,0x5ce430,0x45e082,0x48cdde,0x4f4f4f,0x000000,0x000000,
    0xfffeff,0xc0dfff,0xd3d2ff,0xe8c8ff,0xfbc2ff,0xfec4ea,0xfeccc5,0xf7d8a5,
    0xe4e594,0xcfef96,0xbdf4ab,0xb3f3cc,0xb5ebf2,0xb8b8b8,0x000000,0x000000,
};
/**
 * @brief Преобразует индекс палитры в RGB888 с учётом усиления цветовых каналов.
 * @param index Индекс цвета; используются младшие шесть бит.
 * @param emphasis Три бита усиления из PPUMASK для соответствующей строки.
 * @param pal При true меняет местами PAL-биты красного и зелёного усиления.
 * @return Цвет 0x00RRGGBB; альфа-канал отсутствует.
 * @note Масштабирование каналов целочисленное и не меняет исходный индекс.
 */
inline std::uint32_t displayRgb(std::uint8_t index, std::uint8_t emphasis, bool pal = false) noexcept {
    if (pal) emphasis = static_cast<std::uint8_t>((emphasis & 4) | ((emphasis & 1) << 1) | ((emphasis & 2) >> 1));
    const auto c = rgbPalette[index & 63];
    unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (emphasis & 1) { g = g * 3 / 4; b = b * 3 / 4; }
    if (emphasis & 2) { r = r * 3 / 4; b = b * 3 / 4; }
    if (emphasis & 4) { r = r * 3 / 4; g = g * 3 / 4; }
    return (r << 16) | (g << 8) | b;
}
/**
 * @brief Упаковывает тот же цвет в RGB565 для буфера платы.
 * @param index Индекс NES, ограничиваемый функцией displayRgb до 0..63.
 * @param emphasis Биты усиления переданного цвета; для готового кадра берутся из метаданных строки.
 * @param pal Выбор PAL-перестановки битов усиления.
 * @return Значение RGB565 без перестановки байтов для конкретного транспорта LCD.
 */
inline std::uint16_t displayRgb565(std::uint8_t index, std::uint8_t emphasis, bool pal = false) noexcept {
    const auto rgb = displayRgb(index, emphasis, pal);
    return static_cast<std::uint16_t>(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 31));
}
} // пространство имён nes::core
