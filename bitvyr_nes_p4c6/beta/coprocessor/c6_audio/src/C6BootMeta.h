#pragma once
#include <cstddef>
#include <cstdint>
/** @brief Только фиксированные публичные boot/table/OTA/app hashes и actual SDK running/next metadata.
 * @pre Вызывает единственный C6 worker после quiesce graphics; output заимствован до возврата.
 * @return ESP_OK и written328 либо SDK/validation error; Flash/eFuse/NVS не записываются.
 */
int c6BootMeta(std::uint8_t* output,std::size_t capacity,std::size_t& written) noexcept;
