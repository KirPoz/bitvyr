#pragma once
#include "FrameService.h"
#include "IdleUnbind.h"
#include "BatchProtocol.h"

namespace nes::c6audio::service {
/**
 * @brief SDK-free single-frame service над существующим outer RawChannel.
 * @details Все методы вызываются одним existing worker. Caller владеет wire/typed
 * storage/renderer; ни один объект не освобождается здесь. Driver callback должен
 * передать completed key этому владельцу, не вызывать методы конкурентно.
 */
class BatchFrameService final {
public:
    static constexpr std::size_t maximumSubmitWireBytes=batch::maximumSubmitWireBytes,maximumFastSamples=batch::maximumFastSamples,maximumFastBodyBytes=batch::maximumFastBodyBytes;
    static constexpr std::size_t maximumWireBytes=8192,maximumBodyBytes=1280,pcmHeaderBytes=32;
    static constexpr std::size_t maximumPcmChunkSamples=(maximumBodyBytes-pcmHeaderBytes)/2;
    /** @brief Заимствует реальные непересекающиеся extents; не сбрасывает renderer или session. */
    BatchFrameService(std::uint8_t*wire,std::size_t capacity,const wire::Destination&typed,core::AudioRenderer&renderer) noexcept;
    /** @brief Запрещает клонирование identity/borrow/state поверх того же renderer и storage. */
    BatchFrameService(const BatchFrameService&)=delete;
    /** @brief Не переносит владельца активного frame через присваивание. */
    BatchFrameService&operator=(const BatchFrameService&)=delete;
    /** @brief Адрес service стабилен до actual join; не разрешает неявный move. */
    BatchFrameService(BatchFrameService&&)=delete;
    /** @brief Move-assignment также не передаёт driver/PCM ownership. */
    BatchFrameService&operator=(BatchFrameService&&)=delete;
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
     * @brief Только точный Idle stream и удерживаемый внешний admission barrier допускают Unbound.
     * @param request Ожидаемый полный старый binding/cursor; не заимствуется после возврата.
     * @param admission Caller удерживает реальный Raw barrier до собственного control commit.
     * @return Ok после reset, иначе сервис/renderer/буферы неизменны; timeout не снимается.
     * @details Локальный cold API, не transport ACK и не замена существующего abandonJoined.
     */
    Code unbindIdle(const IdleUnbindRequest&request,const IdleAdmissionLease&admission) noexcept;
    /**
     * @brief Только явный caller с действительным worker/source join может abandon/reset.
     * @details Отказывает пока наш TX sourceHeld. Не является wire-командой или
     * доказательством внешнего join; caller обязан обеспечить отсутствие RX/driver borrows.
     */
    Code abandonJoined() noexcept;
    /** @brief Возвращает только собственные single-owner counters, не читает primary Machine. */
    Snapshot snapshot()const noexcept;
    /**
     * @brief Полный prefix20+usedwire и первый PCM reply в одном strict command.
     * @details Все admission отказы заканчиваются доconsume-old/DSP. Output обязан
     * иметь1696 непересекающихся bytes; common32 добавляет внешний adapter.
     */
    PcmLease submit(const raw::Key&key,const std::uint8_t*body,std::size_t bytes,std::uint8_t*output,std::size_t capacity) noexcept;
    /** @brief Выдаёт только следующий tail до832samples; all-issued/zero frame не выдаётся повторно. */
    PcmLease fetch(const Envelope&key,std::size_t offset,std::size_t wanted,std::uint8_t*output,std::size_t capacity) noexcept;
private:
    /** @brief Общий bounded fast PCM packer; предыдущий ACK flag допустим только для Submit. */
    PcmLease emitFast(const Envelope&key,std::size_t offset,std::size_t wanted,std::uint8_t*output,std::size_t capacity,bool previous) noexcept;
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
