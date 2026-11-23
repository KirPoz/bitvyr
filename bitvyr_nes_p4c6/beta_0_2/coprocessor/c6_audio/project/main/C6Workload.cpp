/** @file C6Workload.cpp @brief SDK-free wire и bounded synthetic job для C6 probe. */
#include "C6Workload.h"
#include <cstring>

namespace nes::c6_workload {
namespace {
constexpr std::uint32_t requestMagic = 0x314a3643U;
constexpr std::uint32_t replyMagic = 0x31523643U;

/** @brief Читает LE32 без требований выравнивания; caller проверяет границу. */
std::uint32_t read32(const std::uint8_t* data) noexcept {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8U) |
           (std::uint32_t(data[2]) << 16U) | (std::uint32_t(data[3]) << 24U);
}

/** @brief Сохраняет LE32, не завися от native ABI и padding. */
void write32(std::uint8_t* data, std::uint32_t value) noexcept {
    for (unsigned index = 0; index < 4U; ++index) data[index] = std::uint8_t(value >> (8U * index));
}

/** @brief Обновляет стандартный CRC32/IEEE, совместимый с zlib.crc32(data,seed). */
std::uint32_t crc32(const std::uint8_t* data, std::size_t bytes, std::uint32_t seed = 0U) noexcept {
    std::uint32_t value = seed ^ 0xffffffffU;
    for (std::size_t index = 0; index < bytes; ++index) {
        value ^= data[index];
        for (unsigned bit = 0; bit < 8U; ++bit)
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return value ^ 0xffffffffU;
}
} // namespace

std::size_t execute(const std::uint8_t* input, std::size_t bytes,
                    std::uint8_t* output, std::size_t capacity,
                    std::uint32_t cpuHz, Clock clock) noexcept {
    if (!output || capacity < replyHeaderBytes) return 0U;
    std::memset(output, 0, replyHeaderBytes);
    write32(output, replyMagic); output[4] = 1U;
    write32(output + 44U, cpuHz);
    unsigned status = 0U;
    std::uint32_t length = 0U, rounds = 0U, digest = 0U, cycles = 0U, workBytes = 0U;
    std::size_t resultBytes = replyHeaderBytes;
    if (!input || bytes < requestHeaderBytes) {
        status = 1U;
    } else {
        std::memcpy(output + 8U, input + 8U, 20U); // sequence/generation/nonce.
        length = read32(input + 28U); rounds = read32(input + 32U);
        const unsigned operation = unsigned(input[6]) | (unsigned(input[7]) << 8U);
        if (read32(input) != requestMagic || input[4] != 1U || input[5] != 0U ||
            read32(input + 8U) == 0U || (read32(input + 12U) | read32(input + 16U)) == 0U) {
            status = 1U;
        } else if (crc32(input, 44U) != read32(input + 44U)) {
            status = 2U;
        } else if (length > maxPayloadBytes || bytes - requestHeaderBytes != length ||
                   !rounds || rounds > maxRounds || (operation == 1U && rounds != 1U)) {
            status = 5U;
        } else if (crc32(input + requestHeaderBytes, length) != read32(input + 40U)) {
            status = 3U;
        } else if (operation != 1U && operation != 2U) {
            status = 4U;
        } else {
            if (operation == 1U && capacity - replyHeaderBytes < length) return 0U;
            write32(output + 32U, read32(input + 40U));
            const auto begin = clock ? clock() : 0U;
            if (operation == 1U) {
                std::memcpy(output + replyHeaderBytes, input + requestHeaderBytes, length);
                write32(output + 28U, length);
                write32(output + 52U, read32(input + 40U));
                digest = read32(input + 40U);
                resultBytes += length;
                workBytes = length;
            } else {
                digest = read32(input + 36U);
                for (unsigned round = 0; round < rounds; ++round)
                    digest = crc32(input + requestHeaderBytes, length, digest);
                workBytes = length * rounds;
            }
            cycles = clock ? clock() - begin : 0U;
            if (cycles >= 0x80000000U) status = 6U;
        }
    }
    output[6] = std::uint8_t(status);
    write32(output + 36U, digest); write32(output + 40U, cycles);
    write32(output + 48U, workBytes);
    write32(output + 56U, crc32(output, 56U));
    return resultBytes;
}

} // namespace nes::c6_workload
