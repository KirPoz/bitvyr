#pragma once
#include "FrameService.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::c6audio::client {
namespace raw=::nes::c6raw;
/** @brief Ответ existing Raw adapter; bytes живут до следующего ExchangeFn вызова. */
struct ReplyView final {raw::View raw{};std::uint64_t host_elapsed_us{};};
/**
 * @brief Заимствованный blocking diagnostic exchange, без нового канала или задачи.
 * @details Callback проверяет CRC и завершение чтения TX source, возвращает стабильный
 * непересекающийся RawView. false означает неизвестное завершение; retry запрещён.
 */
using ExchangeFn=bool(*)(void*,const raw::Key&,const std::uint8_t*,std::size_t,ReplyView&) noexcept;
/** @brief Первый отказ сохраняет владельца до внешнего actual join/reboot. */
enum class Error:std::uint8_t {None,State,Input,Exchange,Reply,Remote,Snapshot,Cold,Capacity,Overflow};
/** @brief Числовой cold INIT результат; C6 cycles не складываются с host RTT как независимые стадии. */
struct ColdInfo final {
    std::uint32_t constructCycles{},crcCycles{},stackBefore{},stackAfter{},rendererBytes{},serviceBytes{},typedBytes{},wireBytes{},controlBytes{},serverHz{};
};
/** @brief Последовательные host RTT и отдельные server counters; это не FPS и не latency fixed-only DSP. */
struct Stats final {
    std::uint64_t hostElapsedUs{},serverCycles{},operationCycles{},rxCycles{},previousEncodeCycles{},previousQueueCycles{},previousSourceCycles{};
    std::uint32_t calls{},frames{},pcmChunks{},rendererCalls{},remoteStatus{};
};
/** @brief Immutable used-record wire и отдельный audio-domain ключ; ROM и полный AudioPacket не передаются. */
struct FrameInput final {const std::uint8_t*wire{};std::size_t bytes{};wire::Expected audio{};std::uint64_t inputFrame{};};

/**
 * @brief Один SDK-free клиент actual FullAudioControl поверх существующего Raw transport.
 * @details Все методы вызывает один владелец. Context/encoded wire/PCM принадлежат caller.
 * INIT/BIND один раз; strict stream начинается input0/audioSeq0/base0/ppuFrame1. Timeout,
 * malformed reply или неизвестное completion переводит owner в poison без освобождения,
 * повторов и fallback. Новый owner допустим только после внешнего joined reset/reboot.
 */
class FrameClient final {
public:
    /** @brief Заимствует callback/context; не вызывает транспорт или DSP. */
    FrameClient(void*context,ExchangeFn exchange) noexcept:context_(context),exchange_(exchange){}
    /** @brief Клонирование strict command cursor и borrowed callback owner запрещено. */
    FrameClient(const FrameClient&)=delete;
    /** @brief Присваивание не переносит транспортный frame owner. */
    FrameClient&operator=(const FrameClient&)=delete;
    /** @brief Адрес request body и владельца стабилен до внешнего join. */
    FrameClient(FrameClient&&)=delete;
    /** @brief Не допускает move-assignment живого или poisoned owner. */
    FrameClient&operator=(FrameClient&&)=delete;
    /** @brief INIT12 проверяет все шесть LUT counts/CRC и 48kHz/2048 до BIND. */
    bool initCold(const service::Binding&binding) noexcept;
    /** @brief BIND13 sequence0 фиксирует отдельные App/audio identities и first stream sequence1. */
    bool bind() noexcept;
    /**
     * @brief BEGIN/DATA/RENDER/PCM/CONSUMED одного полного интервала без скрытых копий packet.
     * @param pcm Writable caller массив настоящей capacity; не пересекает client/input/samples.
     * @param samples Публикуется только после actual CONSUMED success, при отказе неизменен.
     * @details При отказе частично записанный PCM недействителен и owner остаётся poison.
     * Capacity сверяется с dynamic sample count, включая интервалы больше800.
     */
    bool renderFrame(const FrameInput&input,std::int16_t*pcm,std::size_t capacity,std::size_t&samples) noexcept;
    /** @brief Возвращает первый отказ; None не является completion отдельного frame. */
    Error error()const noexcept{return error_;}
    /** @brief Только успешный bind/consumed разрешает следующий sequential frame. */
    bool ready()const noexcept{return state_==State::Bound&&error_==Error::None;}
    /** @brief Снимок собственных counters без чтения изменяемой Machine/C6 state. */
    const Stats&stats()const noexcept{return stats_;}
    /** @brief Проверенные cold числовые поля, не runtime watermark P4. */
    const ColdInfo&cold()const noexcept{return cold_;}
private:
    /** @brief Локальная фаза owner; poison не сбрасывается автоматически. */
    enum class State:std::uint8_t {Empty,Cold,Bound,Busy,Poisoned};
    /** @brief Сохраняет первый отказ и запрещает новый транспортный вызов. */
    bool fail(Error error) noexcept;
    /** @brief Строит полный RawKey без truncation absolute guest cycles. */
    raw::Key key(std::uint16_t opcode,std::uint32_t sequence,std::uint64_t inputFrame,std::uint32_t duration)const noexcept;
    /** @brief Проверяет callback/full Raw reply и числовой status32 до чтения body. */
    bool call(const raw::Key&key,std::size_t bytes,ReplyView&reply) noexcept;
    /** @brief Пишет envelope20 с явными epoch/input64; body никогда не передаётся как C++ struct. */
    void envelope(std::uint64_t inputFrame) noexcept;
    /** @brief Проверяет exact state/nextSequence/used extents/rendererCalls числового snapshot40. */
    bool snapshot(const ReplyView&reply,service::State state,std::size_t received,std::size_t expected,
                  std::size_t pcmOffset,std::size_t samples,std::uint32_t rendererCalls) noexcept;
    /** @brief Завершает успешную команду stream; следующий slot чередуется0/1. */
    void advance() noexcept;
    void*context_{};ExchangeFn exchange_{};
    std::array<std::uint8_t,raw::payloadBytes>body_{};
    service::Binding binding_{};ColdInfo cold_{};Stats stats_{};
    std::uint64_t nextInput_{},nextAudio_{},nextPpu_=1,nextBase_{};
    std::uint32_t sequence_=1;std::uint8_t slot_{};
    State state_=State::Empty;Error error_=Error::None;bool exchanging_{};
};
} // namespace nes::c6audio::client
