/**
 * @file ConsoleSettings.h
 * @brief Настройки консоли и явный16-байтовый формат с проверкой CRC/версии/диапазонов.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::app {

/** @brief Пользовательский выбор Auto/NTSC/PAL; Force NTSC для PAL является отдельной политикой скорости. */
enum class RegionMode : std::uint8_t { Auto = 0, Ntsc = 1, Pal = 2 };

/** @brief Чистые настройки консоли с тихими исходными значениями. Сериализация не зависит от C++ layout. */
struct ConsoleSettings final {
    std::uint8_t volume{1};
    bool muted{true};
    bool swapAB{false};
    bool showFps{true};
    RegionMode region{RegionMode::Ntsc};

    /**
     * @brief Выбирает PAL по пользовательскому режиму; подсказка ROM учитывается только в Auto.
     * @param hint Подсказка PAL из ROM, используемая только при режиме Auto.
     */
    bool effectivePal(bool hint) const noexcept {
        return region == RegionMode::Pal || (region == RegionMode::Auto && hint);
    }
    /**
     * @brief При включённой настройке меняет местами только NES A/B, сохраняя остальные биты маски.
     * @param mask Маска кнопок/битов в формате вызывающего интерфейса.
     */
    std::uint8_t mapGamepadButtons(std::uint8_t mask) const noexcept {
        if (!swapAB) return mask;
        return static_cast<std::uint8_t>((mask & 0xfcU) | ((mask & 1U) << 1) | ((mask & 2U) >> 1));
    }
};

using EncodedSettings = std::array<std::uint8_t, 16>;

namespace settings_detail {
/**
 * @brief Вычисляет CRC32 полей версионированного16-байтового формата настроек.
 * @param data Заимствованный входной диапазон; функция не освобождает его.
 * @param count Число элементов/байтов в переданном диапазоне.
 */
inline std::uint32_t checksum(const std::uint8_t* data, std::size_t count) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < count; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
/**
 * @brief Проверяет диапазон volume0..100 и допустимое значение режима региона.
 * @param settings Неизменяемые настройки для проверки диапазонов; функция ничего не сохраняет.
 */
inline bool valid(const ConsoleSettings& settings) noexcept {
    return settings.volume <= 100 && static_cast<unsigned>(settings.region) <= 2;
}
} // namespace settings_detail

/**
 * @brief Сериализует настройки в16 явных байтов с сигнатурой/версией/CRC; layout C++-структуры не записывается.
 * @param settings Неизменяемые настройки; неверные диапазоны приводят к false.
 * @param bytes Выходной массив ровно16 байт; сначала обнуляется, при успехе содержит blob с CRC.
 */
inline bool encodeSettings(const ConsoleSettings& settings, EncodedSettings& bytes) noexcept {
    bytes.fill(0);
    if (!settings_detail::valid(settings)) return false;
    // Явный версионированный формат не зависит от layout структуры и endian CPU.
    bytes[0] = 'N'; bytes[1] = 'E'; bytes[2] = 'S'; bytes[3] = 'C';
    bytes[4] = 1; bytes[5] = static_cast<std::uint8_t>(bytes.size());
    bytes[6] = settings.volume;
    bytes[7] = static_cast<std::uint8_t>((settings.muted ? 1U : 0U) |
               (settings.swapAB ? 2U : 0U) | (settings.showFps ? 4U : 0U));
    bytes[8] = static_cast<std::uint8_t>(settings.region);
    const auto crc = settings_detail::checksum(bytes.data(), 12);
    for (unsigned i = 0; i < 4; ++i) bytes[12 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    return true;
}

/**
 * @brief Проверяет длину, сигнатуру, версию, резервные поля и CRC. При любом отказе возвращает тихие настройки по умолчанию.
 * @param bytes Заимствованный входной blob; nullptr или неверная длина дают false.
 * @param size Длина входа в байтах; формат принимает ровно16.
 * @param settings Выход настроек; до проверки установлен тихий default и остаётся им при отказе.
 */
inline bool decodeSettings(const std::uint8_t* bytes, std::size_t size, ConsoleSettings& settings) noexcept {
    settings = ConsoleSettings{}; // Любой неверный payload возвращает тихие значения по умолчанию.
    if (!bytes || size != 16 || bytes[0] != 'N' || bytes[1] != 'E' || bytes[2] != 'S' || bytes[3] != 'C' ||
        bytes[4] != 1 || bytes[5] != 16 || bytes[6] > 100 || (bytes[7] & 0xf8U) ||
        bytes[8] > 2 || bytes[9] || bytes[10] || bytes[11]) return false;
    std::uint32_t stored = 0;
    for (unsigned i = 0; i < 4; ++i) stored |= std::uint32_t(bytes[12 + i]) << (8 * i);
    if (stored != settings_detail::checksum(bytes, 12)) return false;
    settings.volume = bytes[6];
    settings.muted = (bytes[7] & 1U) != 0;
    settings.swapAB = (bytes[7] & 2U) != 0;
    settings.showFps = (bytes[7] & 4U) != 0;
    settings.region = static_cast<RegionMode>(bytes[8]);
    return true;
}

} // namespace nes::app
