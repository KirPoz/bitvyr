/** @file Mmc5HeadForecast.h @brief Pure typed-/RD head до канонического MMC5 Pattern reset. */
#pragma once
#include <cstdint>

namespace nes::core::mmc5epoch {

/** @brief Итог ограниченной проверки; отказ не разрешает расширять CPU/readQuantum. */
enum class HeadStatus : std::uint8_t { SafeReset, DetectorEdge, NoReset, Domain };

/** @brief Малый результат без ссылок на ROM/PPU/mapper и без транспортного ABI.
 * @details Все dot offsets считаются с 1; ноль означает отсутствие события.
 * cartRdMask bit0..7 соответствует cartridge /RD на проверенных dots1..8. */
struct HeadResult {
    HeadStatus status = HeadStatus::Domain; ///< SafeReset требует fresh canonical Pattern /RD.
    std::uint8_t checkedDots = 0; ///< Число проверенных точек 0..8.
    std::uint8_t resetDot = 0; ///< Первый канонический Pattern /RD, либо 0.
    std::uint8_t firstEdgeDot = 0; ///< Первый detectScanline /RD, либо 0.
    std::uint8_t cartRdMask = 0; ///< Cartridge /RD, palette сюда не входит.
};
static_assert(sizeof(HeadResult) == 5, "HeadResult is a local five-byte POD");

/**
 * @brief Проверяет максимум 8 authoritative visible dots до fresh Pattern /RD.
 * @param dot Следующая исполняемая PPU-точка 9..320; prefetch/wrap не входят.
 * @param v Текущий 15-битный loopy v до исполнения dot.
 * @param t Текущий 15-битный loopy t для горизонтального копирования на 257.
 * @param heldBus Фактический удержанный 14-битный PPU address после CPU/MMIO.
 * @param lastPpuAddress Последний MMC5 /RD address либо reset sentinelffff.
 * @param repeat Текущий MMC5 NT-repeat0..3, не предположение о raster phase.
 * @param coarseXSignal Фактическая защёлка phase6→phase7.
 * @param scrollSignal Фактическая защёлка dot255→256/257.
 * @return Pure результат; DetectorEdge проверяется до изменения repeat.
 * @pre Caller сохраняет NTSC stable visible rendering, inFrame/false IRQ,
 * readSinceM2/idle<=1, неизменный MMIO/banks и исходные audio/DMC/lease guards.
 * Перед one-digital-M2 projection caller дополнительно требует mask&7 !=0.
 * @details Не читает ROM/OAM/CHR bytes, не копирует экземпляр состояния и ничего
 * не тактирует. SafeReset не является CPU grant и не закрывает произвольный tail.
 * Нормальный Pattern address заменён только классом<2000 до немедленного выхода;
 * исходный heldBus используется точно до настоящего canonical drive. */
inline HeadResult forecastHead(unsigned dot, std::uint16_t v, std::uint16_t t,
                               std::uint16_t heldBus, std::uint16_t lastPpuAddress,
                               unsigned repeat, bool coarseXSignal,
                               bool scrollSignal) noexcept {
    HeadResult result;
    if (dot < 9U || dot > 320U || v >= 0x8000U || t >= 0x8000U ||
        heldBus >= 0x4000U || (lastPpuAddress >= 0x4000U && lastPpuAddress != 0xffffU) || repeat > 3U)
        return result;
    bool canonical = false;
    result.status = HeadStatus::NoReset;
    for (unsigned offset = 0; offset < 8U && dot <= 320U; ++offset, ++dot) {
        const unsigned phase = (dot - 1U) & 7U;
        const bool sprite = dot >= 257U;
        if (dot == 255U) scrollSignal = true;
        if (dot == 257U) {
            if (scrollSignal) v = static_cast<std::uint16_t>((v & 0x7be0U) | (t & 0x041fU));
            scrollSignal = false;
        }
        if (phase == 0U) {
            heldBus = static_cast<std::uint16_t>(0x2000U | (v & 0x0fffU));
            canonical = false;
        } else if (phase == 2U) {
            heldBus = static_cast<std::uint16_t>(sprite ? 0x2000U | (v & 0x0fffU) :
                0x23c0U | (v & 0x0c00U) | ((v >> 4U) & 0x38U) | ((v >> 2U) & 7U));
            canonical = false;
        } else if (phase == 4U || (sprite && phase == 6U)) {
            heldBus = 0; // Канонический вычисленный Pattern класс; данные для detector не нужны.
            canonical = true;
        } else if (phase == 6U) {
            heldBus = static_cast<std::uint16_t>((heldBus + 8U) & 0x3fffU);
            coarseXSignal = true;
        }
        result.checkedDots = static_cast<std::uint8_t>(offset + 1U);
        if (phase & 1U) {
            if (heldBus < 0x3f00U) {
                result.cartRdMask = static_cast<std::uint8_t>(result.cartRdMask | (1U << offset));
                if (repeat == 3U) {
                    result.status = HeadStatus::DetectorEdge;
                    result.firstEdgeDot = result.checkedDots;
                    return result;
                }
                if (heldBus >= 0x2000U && heldBus < 0x3000U)
                    repeat = heldBus == lastPpuAddress ? repeat + 1U : 1U;
                else repeat = 0;
                lastPpuAddress = heldBus;
                if (canonical) {
                    result.status = HeadStatus::SafeReset;
                    result.resetDot = result.checkedDots;
                    return result;
                }
            }
            if (phase == 3U) {
                heldBus = 0; // Обе scalar ветки после Attribute/Dummy задают свежий Pattern.
                canonical = true;
            }
            if (!sprite && phase == 7U) {
                if (coarseXSignal) {
                    if ((v & 31U) == 31U) v = static_cast<std::uint16_t>((v & 0x7fe0U) ^ 0x0400U);
                    else ++v;
                }
                coarseXSignal = false;
            }
        }
        if (dot == 256U && scrollSignal) {
            if ((v & 0x7000U) != 0x7000U) v = static_cast<std::uint16_t>(v + 0x1000U);
            else {
                v &= 0x0fffU;
                unsigned y = (v >> 5U) & 31U;
                if (y == 29U) { y = 0; v ^= 0x0800U; }
                else if (y == 31U) y = 0;
                else ++y;
                v = static_cast<std::uint16_t>((v & 0x7c1fU) | (y << 5U));
            }
        }
    }
    return result;
}
} // namespace nes::core::mmc5epoch
