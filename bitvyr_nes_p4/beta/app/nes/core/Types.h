#pragma once

/**
 * @file Types.h
 * @brief Общие значения состояния машины, региона и кнопок контроллера.
 * @ingroup nes_core
 *
 * Типы не зависят от ESP-IDF и не содержат указателей на ресурсы платы.
 * Результат запуска описывает гостевое исполнение, а не готовность физического LCD.
 */

#include <cstdint>

namespace nes::core {

using Byte = std::uint8_t;
using Address = std::uint16_t;

/** @brief Причина завершения порции исполнения; ошибки запрещают публикацию неполного кадра. */
enum class RunStatus : std::uint8_t {
    FrameReady,
    NoCartridge,
    CpuJammed,
    ClockLimit,
    AudioFault,
    VideoFault,
};

/** @brief Регион определяет частоты CPU/APU и отношение тактов CPU к точкам PPU. */
enum class Region : std::uint8_t { Ntsc, Pal };
/** @brief Битовая маска восьми кнопок NES до последовательного считывания через $4016/$4017. */
enum Button : std::uint8_t {
    A = 1, B = 2, Select = 4, Start = 8,
    Up = 16, Down = 32, Left = 64, Right = 128,
};

}  // пространство имён nes::core
