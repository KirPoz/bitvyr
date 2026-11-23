#pragma once
#include <cstddef>
#include <cstdint>
#include "RawLimits.h"

/** @brief Собственный little-endian протокол одного неделимого Hosted SERIAL1 пакета. */
namespace nes::c6raw {
constexpr std::size_t headerBytes = NES_RAW_HEADER_BYTES;
constexpr std::size_t requestPacketBytes = NES_RAW_REQUEST_PACKET_BYTES;
constexpr std::size_t requestPayloadBytes = requestPacketBytes - headerBytes;
constexpr std::size_t replyPacketBytes = NES_RAW_REPLY_PACKET_BYTES;
/** @brief Максимальный stored packet; request остаётся отдельно ограничен1400. */
constexpr std::size_t packetBytes = replyPacketBytes;
constexpr std::size_t payloadBytes = packetBytes - headerBytes;
constexpr std::uint32_t directionalProfile = NES_RAW_DIRECTIONAL_PROFILE;
constexpr std::uint32_t helloCapabilities = NES_RAW_HELLO_CAPABILITIES;
constexpr std::size_t helloBodyBytes = NES_RAW_HELLO_BODY_BYTES;
constexpr unsigned slotCount = 2;
/** @brief Полный ключ результата; lease запрещает смешивать разные ROM без передачи самой ROM.
 * @details begin/end — guest CPU offsets относительно данного frame, в пределах[0,45000),
 * а не усечённые абсолютные64-битные cycles. P4 хранит immutable mapping frame→base64
 * в своём владельце и проверяет generation/lease/session/sequence до применения результата.
 */
struct Key final {
    std::uint64_t generation{}, lease{}, session{};
    std::uint32_t frame{}, sequence{}, begin{}, end{};
    std::uint16_t opcode{};
    std::uint8_t slot{}, region{};
};
/** @brief Заимствованный проверенный пакет; указатель действителен только пока владелец хранит bytes. */
struct View final { Key key{}; std::uint8_t kind{}, flags{}; const std::uint8_t* payload{}; std::size_t size{}; };
/** @brief CRC32 IEEE; не является криптографической аутентификацией. */
std::uint32_t crc(const std::uint8_t* data, std::size_t bytes) noexcept;
/** @brief Кодирует запрос kind1 или ответ kind2 без ABI-dependent структур и выделения памяти. */
std::size_t encode(std::uint8_t* out, std::size_t capacity, const Key& key,
    std::uint8_t kind, std::uint8_t flags, const std::uint8_t* payload, std::size_t bytes) noexcept;
/** @brief Проверяет размер, CRC, reserved-поля и допустимые ключи; ошибка не публикует View. */
bool decode(const std::uint8_t* packet, std::size_t bytes, View& out) noexcept;
/** @brief Сравнивает все поля включая slot, region и границы гостевого времени. */
bool equal(const Key& left, const Key& right) noexcept;
} // namespace nes::c6raw
