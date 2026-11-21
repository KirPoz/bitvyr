#pragma once
#include <cstddef>
#include <cstdint>

namespace nes::c6_workload {

/** @brief Собственный ограниченный протокол synthetic compute; не NES CPU/PPU. */
constexpr std::uint32_t requestId = 0x4e455331U;
constexpr std::uint32_t responseId = 0x4e455332U;
constexpr std::size_t requestHeaderBytes = 48U;
constexpr std::size_t replyHeaderBytes = 64U;
constexpr std::size_t maxPayloadBytes = 4096U;
constexpr unsigned maxRounds = 4U;

/** @brief Часы адаптера дают модульный32-битный host cycle counter, без guest clocks. */
using Clock = std::uint32_t (*)() noexcept;

/**
 * @brief Проверяет wire, исполняет bounded echo/CRC и формирует согласованный ответ.
 * @param input Заимствованные requestHeaderBytes+payload байты, доступны до возврата.
 * @param bytes Фактический размер request; payload не превышает4096Б.
 * @param output Отдельный writable буфер; не перекрывается с input.
 * @param capacity Размер output; требуется минимум64Б, для echo64+payload.
 * @param cpuHz Фактическая частота адаптера, в Гц; не задаёт и не меняет clock.
 * @param clock Модульный host cycle counter; nullptr оставляет measured_cycles0.
 * @return Количество reply bytes либо0 при нехватке output capacity.
 * @details WireLE: magic/version/op/sequence/generation/nonce/length/rounds/seed/
 * payloadCRC/headerCRC. Полный ключ возвращается; header и payload CRC независимы.
 * op1=echo(rounds1), op2=CRC-rounds1..4. Не выделяет память, не читает ROM,
 * не исполняет guest CPU и не обещает NES FPS. Счётчик измеряет только синтетическую
 * работу, возможные IRQ входят в интервал; разность>=2^31 отвергается.
 */
std::size_t execute(const std::uint8_t* input, std::size_t bytes,
                    std::uint8_t* output, std::size_t capacity,
                    std::uint32_t cpuHz, Clock clock) noexcept;

} // namespace nes::c6_workload
