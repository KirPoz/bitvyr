#pragma once

/** @file ClientTypes.h
 * @brief Типы запросов, ответов и управления асинхронным клиентом PCM.
 * @details Общее соглашение между владельцем BatchClient и адаптером RawChannel на P4.
 */
#include "FrameService.h"

#include <cstddef>
#include <cstdint>

namespace nes::c6audio::client {
namespace raw=::nes::c6raw;
/** @brief Проверенный ответ RawChannel. Данные действуют до начала следующего запроса. */
struct ReplyView final {raw::View raw{};std::uint64_t host_elapsed_us{};};
/** @brief Причина первой ошибки. Владелец сохраняется до подтверждённого завершения или перезапуска. */
enum class Error:std::uint8_t {None,State,Input,Exchange,Reply,Remote,Snapshot,Cold,Capacity,Overflow};
/** @brief Снимок размеров, частоты и времени начальной настройки обработчика C6. */
struct ColdInfo final {
    std::uint32_t constructCycles{},crcCycles{},stackBefore{},stackAfter{},rendererBytes{},serviceBytes{},typedBytes{},wireBytes{},controlBytes{},serverHz{};
};
/** @brief Время обмена со стороны P4 и отдельные счётчики обработки команды на C6. */
struct Stats final {
    std::uint64_t hostElapsedUs{},serverCycles{},operationCycles{},rxCycles{},previousEncodeCycles{},previousQueueCycles{},previousSourceCycles{};
    std::uint32_t calls{},frames{},pcmChunks{},rendererCalls{},remoteStatus{};
};
/** @brief Неизменяемый диапазон записей APU и ключ аудио для одного гостевого кадра. */
struct FrameInput final {const std::uint8_t*wire{};std::size_t bytes{};wire::Expected audio{};std::uint64_t inputFrame{};};
} // namespace nes::c6audio::client

namespace nes::c6audio::async {
namespace raw=::nes::c6raw;
using client::ColdInfo;
using client::Error;
using client::FrameInput;
using client::ReplyView;
using client::Stats;
/** @brief Состояние запроса. При ожидании или ошибке буферы остаются у транспорта. */
enum class TransportState:std::uint8_t {Pending,Complete,Failed};
/** @brief При отказе передача не начинается. Клиент не повторяет команду автоматически. */
using StartFn=bool(*)(void*,const raw::Key&,const std::uint8_t*,std::size_t) noexcept;
/** @brief Complete подтверждает CRC, полный ответ и окончание передачи. Ответ действует до нового запроса. */
using PollFn=TransportState(*)(void*,ReplyView&) noexcept;
/** @brief Результат одного шага клиента Main. Кадр завершается отдельным подтверждением. */
enum class Progress:std::uint8_t {Idle,Pending,Complete,Poisoned};
} // namespace nes::c6audio::async
