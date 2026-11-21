#pragma once
#include "apu/AudioPacketView.h"
#include <cstddef>
#include <cstdint>

namespace nes::c6audio::wire {
/** @brief Версия PRIVATE input-формата; native C++ layout не передаётся. */
inline constexpr std::uint16_t version=1;
inline constexpr std::size_t headerBytes=80;

/**
 * @brief Ключ ожидаемого запечатанного интервала в домене AudioPacket.
 * @details Не заменяет внешний RawChannel key: adapter ДО decode проверяет App generation,
 * ROM lease/epoch, transport session, input frame, slot/sequence и completion источника.
 * audio generation и App generation не отождествляются по совпадению чисел.
 */
struct Expected final {
    std::uint64_t generation{},sequence{},ppuFrame{},baseCycle{},endCycle{};
    nes::core::Region region=nes::core::Region::Ntsc;
    std::uint16_t capabilities{};
};

/**
 * @brief Собственное typed хранилище вызывающего, без полного массива AudioPacket.
 * @details Ёмкости означают настоящие extents; время жизни и единственного владельца
 * обеспечивает adapter. Массивы/Header, сам Destination и wire не перекрываются.
 * При отказе неизменны; объект Destination живёт отдельно от writable storage.
 */
struct Destination final {
    nes::core::AudioPacket::Header* header{};
    nes::core::AudioPacket::Input* input{};std::size_t inputCapacity{};
    nes::core::AudioPacket::Dmc* dmc{};std::size_t dmcCapacity{};
    nes::core::AudioPacket::Expansion* expansion{};std::size_t expansionCapacity{};
};

/**
 * @brief Считает представимый полный размер wire после проверки Header; ноль означает отказ.
 * @details Полученный размер не разрешает C6 allocation или превышение лимитов adapter.
 */
[[nodiscard]] std::size_t encodedSize(const nes::core::AudioPacket::Header&) noexcept;

/**
 * @brief Кодирует только used records в caller-owned LE массив, не меняя исходный view.
 * @param view Неизменяемые typed arrays и Header, доступные до полного возврата.
 * @param output Незанятый неперекрывающийся буфер не менее capacity байт.
 * @param capacity Настоящая ёмкость output, а не обещанное число из wire.
 * @param written Точный размер при успехе; при отказе неизменен, как output и view.
 * Алиас с output/view/Header/used arrays отвергается до первой записи.
 */
[[nodiscard]] bool encode(const nes::core::AudioPacketView& view,std::uint8_t* output,
    std::size_t capacity,std::size_t& written) noexcept;

/**
 * @brief Проверяет полный wire и ожидаемый временной ключ до первой записи в typed arrays.
 * @details Outer CRC/key проверяет RawChannel. Порядок APU событий/статусы/DMC ещё проверяет
 * AudioRenderer; codec не дублирует его семантику. Wire должен быть полностью собран и
 * неизменен. На успехе Header записывается последним; это не release-публикация между tasks.
 */
[[nodiscard]] bool decode(const std::uint8_t* input,std::size_t bytes,const Expected& expected,
    const Destination& destination) noexcept;
} // namespace nes::c6audio::wire
