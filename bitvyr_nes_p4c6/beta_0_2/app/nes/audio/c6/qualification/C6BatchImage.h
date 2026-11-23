#pragma once
#include <array>
#include <cstdint>

namespace nes::app::c6_async::batch_image {
/** SHA-256 ELF, который C6 предъявляет в HELLO перед приёмом PCM.
 * По умолчанию используется образ из аппаратной проверки каталога Beta 0.2.
 * После отдельной проверки другого образа его полный SHA можно передать в CMake.
 */
#ifdef NES_C6_WORKER_SHA256_BYTES
inline constexpr std::array<std::uint8_t, 32> warmElf{{NES_C6_WORKER_SHA256_BYTES}};
#else
inline constexpr std::array<std::uint8_t, 32> warmElf{{
    0xc2, 0x66, 0x0f, 0xac, 0x17, 0x2c, 0x21, 0x6b,
    0x5a, 0x26, 0x42, 0x4d, 0xd9, 0x93, 0xaf, 0x03,
    0x77, 0x74, 0x7b, 0x56, 0x88, 0xae, 0x4b, 0xca,
    0xf7, 0x39, 0x66, 0x84, 0x65, 0x73, 0x8d, 0x05
}};
#endif
} // namespace nes::app::c6_async::batch_image
