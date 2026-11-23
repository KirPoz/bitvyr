/**
 * @file Mmc5ChrPadding.h
 * @brief Геометрия временной копии MMC5 CHR-ROM с заполнением незапаянного хвоста.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nes::app {

/** @brief Размер временного образа и число банков CHR в его заголовке. */
struct Mmc5ChrPadding {
    std::size_t bytes = 0;
    std::uint8_t chrBanks = 0;
};

/**
 * @brief Возвращает геометрию прямого CHR-отображения для полного MMC5-образа NES 2.0.
 * @details Разрешены только линейные размеры без дополнительных ROM. Заголовок, trainer,
 * PRG и исходный CHR уже проверены библиотекой. Хвост добавляется только в рабочей
 * копии. Значение $FF совпадает с чтением незаполненных адресов ядром MMC5.
 * @param image Заимствованный полный образ до изменения заголовка.
 * @param bytes Длина исходного образа в байтах.
 * @return Нулевой размер означает, что образ остаётся в исходном виде.
 */
inline Mmc5ChrPadding mmc5ChrPadding(const std::uint8_t* image, std::size_t bytes) noexcept {
    if (!image || bytes < 16 || (image[7] & 0x0fU) != 8U ||
        (image[6] >> 4U) != 5U || (image[7] & 0xf0U) != 0U ||
        image[8] != 0U || image[9] >= 15U || (image[14] & 3U) != 0U)
        return {};
    const unsigned chrBanks = image[5];
    if (!chrBanks || chrBanks >= 128U || (chrBanks & (chrBanks - 1U)) == 0U)
        return {};

    const unsigned prgBanks = image[4] | (unsigned(image[9]) << 8U);
    const std::size_t chrStart = 16U + ((image[6] & 4U) ? 512U : 0U) + prgBanks * 16384U;
    if (!prgBanks || bytes != chrStart + chrBanks * 8192U)
        return {};

    unsigned paddedBanks = 1;
    while (paddedBanks < chrBanks)
        paddedBanks <<= 1U;
    return {chrStart + paddedBanks * 8192U, static_cast<std::uint8_t>(paddedBanks)};
}

} // namespace nes::app
