#pragma once
#include "RetainedA09Policy.h"
/**
 * @brief Единственный C6 Worker выбирает retained a09 OTA0 через SDK после полного cold guard.
 * @pre Unbound FullAudioControl, нет sourceHeld/colour frame/других Raw slots; передаёт RawWorker.
 * @return Status Code0 и body40B только после postBOOTMETA проверки; reset здесь отсутствует.
 * @details Не принимает offsets/hash target с wire; expected boot/table/slot0 сгенерированы
 * из frozen recovery-a09 manifest. Никаких APP/NVS/eFuse/bootloader/table erase/write.
 */
nes::c6rollback::Code c6SelectRetainedA09(const std::uint8_t*,std::size_t,std::uint8_t*,
    std::size_t,std::size_t&) noexcept;
