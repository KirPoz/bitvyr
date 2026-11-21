#pragma once
#include "ServiceReady.h"
#include "RawWire.h"
#include <cstddef>
#include <cstdint>

namespace nes::c6audio::service {
namespace raw=::nes::c6raw;
/** @brief Только PRIVATE предложения opcodes; существующий C6 endpoint здесь не регистрируется. */
enum class Command:std::uint16_t {Begin=0x40,Data=0x41,Render=0x42,Pcm=0x43,Consumed=0x44};
/** @brief Явный результат; никакого скрытого локального fallback или truncation. */
enum class Code:std::uint8_t {Ok,State,Identity,Capacity,Order,Overlap,WireRejected,DspFault,SourceHeld,Wrap};
/** @brief Один владелец всего кадра; timeout не переводит его в Idle. */
enum class State:std::uint8_t {Unbound,Idle,Assembling,Complete,Rendering,Result,Rejected};
/** @brief Внешний session binding без ROM pointer; только App-адаптер знает ROM lease mapping. */
struct Binding final {
    std::uint64_t appGeneration{},romLease{},romEpoch{},session{},audioGeneration{};
    std::uint32_t firstCommandSequence=1;
    core::Region region=core::Region::Ntsc;
    std::uint16_t audioCapabilities=core::AudioPacket::externalDacQ16;
};
/** @brief RawKey остаётся существующим; epoch/input64 передаются adapter mapping отдельно, без усечения. */
struct Envelope final {raw::Key raw{};std::uint64_t romEpoch{},inputFrame{};};
/** @brief Ключ полного input wire, а не обещание вместимости максимального AudioPacket. */
struct Begin final {Envelope key{};wire::Expected audio{};std::size_t bytes{};};
/** @brief Один caller-owned TX buffer остаётся заимствован до настоящего sourceReleased. */
struct PcmLease final {Code code=Code::State;Envelope key{};const std::uint8_t*data{};std::size_t bytes{};std::uint32_t sampleOffset{},samples{},totalSamples{};};
/** @brief Числовой снимок single-owner service, не performance telemetry. */
struct Snapshot final {State state{};std::size_t received{},expected{},pcmOffset{},samples{};std::uint32_t nextSequence{},rendererCalls{};bool sourceHeld{},timedOut{},storageValid{};core::AudioRenderResult rendererStatus=core::AudioRenderResult::InvalidPacket;};

/**
 * @brief SDK-free single-frame service над существующим outer RawChannel.
 * @details Все методы вызываются одним existing worker. Caller владеет wire/typed
 * storage/renderer; ни один объект не освобождается здесь. Driver callback должен
 * передать completed key этому владельцу, не вызывать методы конкурентно.
 */
class FrameService final {
public:
    static constexpr std::size_t maximumWireBytes=8192,maximumBodyBytes=1280,pcmHeaderBytes=32;
    static constexpr std::size_t maximumPcmChunkSamples=(maximumBodyBytes-pcmHeaderBytes)/2;
    /** @brief Заимствует реальные непересекающиеся extents; не сбрасывает renderer или session. */
    FrameService(std::uint8_t*wire,std::size_t capacity,const wire::Destination&typed,core::AudioRenderer&renderer) noexcept;
    /** @brief Запрещает клонирование identity/borrow/state поверх того же renderer и storage. */
    FrameService(const FrameService&)=delete;
    /** @brief Не переносит владельца активного frame через присваивание. */
    FrameService&operator=(const FrameService&)=delete;
    /** @brief Адрес service стабилен до actual join; не разрешает неявный move. */
    FrameService(FrameService&&)=delete;
    /** @brief Move-assignment также не передаёт driver/PCM ownership. */
    FrameService&operator=(FrameService&&)=delete;
    /** @brief Только joined caller открывает новый поток сseq0/base0 и сбрасывает собственный renderer. */
    Code bindJoined(const Binding&binding) noexcept;
    /** @brief Принимает ровно следующий frame и полный audio-domain key до первой записи/DSP. */
    Code begin(const Begin&request) noexcept;
    /** @brief Копирует только следующий непустой chunk; wrong key/order не меняет owner/bytes/DSP. */
    Code data(const Envelope&key,std::size_t offset,const std::uint8_t*data,std::size_t bytes) noexcept;
    /** @brief Только полный wire проходит Rootcodec→view→actual persistent FixedON renderer один раз. */
    Code render(const Envelope&key) noexcept;
    /** @brief Копирует последовательный LE PCM reply в caller TX buffer, удерживаемый до sourceReleased. */
    PcmLease pcm(const Envelope&key,std::size_t sampleOffset,std::size_t requestedSamples,std::uint8_t*output,std::size_t capacity) noexcept;
    /** @brief Только настоящий driver source completion с exact key снимает TX borrow; это не клиентский consumed. */
    Code sourceReleased(const Envelope&key) noexcept;
    /** @brief После всех PCM chunks и TXdone клиент явно разрешает reuse; никакого DSP/replay здесь. */
    Code consumed(const Envelope&key) noexcept;
    /** @brief Сохраняет текущего владельца при timeout; не отменяет job/reset/буферы и не запускает retry. */
    void noteTimeout() noexcept {timedOut_=true;}
    /**
     * @brief Только явный caller с действительным worker/source join может abandon/reset.
     * @details Отказывает пока наш TX sourceHeld. Не является wire-командой или
     * доказательством внешнего join; caller обязан обеспечить отсутствие RX/driver borrows.
     */
    Code abandonJoined() noexcept;
    /** @brief Возвращает только собственные single-owner counters, не читает primary Machine. */
    Snapshot snapshot()const noexcept;
private:
    /** @brief Сверяет все owner fields и отдельно командныйsequence/slot/opcode без их наивного равенства всемукадру. */
    bool command(const Envelope&key,Command command)const noexcept;
    /** @brief Проверяет отсутствие alias с сервисом, typed extents, renderer и assembly wire. */
    bool separateOutput(const void*pointer,std::size_t bytes)const noexcept;
    std::uint8_t*wire_{};std::size_t capacity_{};wire::Destination typed_{};core::AudioRenderer&renderer_;
    Binding binding_{};Begin owner_{};BorrowedResult result_{};Envelope source_{};
    State state_=State::Unbound;std::size_t received_{},pcmOffset_{};
    std::uint64_t nextFrame_{},audioSequence_{},audioFrame_=1,audioBase_{};
    std::uint32_t nextSequence_{},rendererCalls_{};
    bool sourceHeld_{},timedOut_{},valid_{};
};
} // namespace nes::c6audio::service
