/** @file RomWindow.h @brief Неизменяемая physical PRG страница для строгого read-fold. */
#pragma once
#include <cstdint>
namespace nes::core {
/** @brief Заимствованная 8192B PRG-ROM страница; Machine подтверждает immutable и оба поколения.
 * @details Не владеет ROM. Действительна только внутри текущего owner-call до смены mapping/ROM lease;
 * PRG-RAM и MMIO не допускаются. Это identity DTO, не decoder cache и не исполняемый код.
 */
struct RomWindow final {
    const std::uint8_t* physicalPage = nullptr;
    std::uint64_t romLease = 0;
    std::uint64_t mappingEpoch = 0;
    bool immutable = false;
};
}
