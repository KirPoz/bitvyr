#pragma once
#include "FrameClient.h"

namespace nes::c6audio::async {
namespace raw=::nes::c6raw;
using client::Error;using client::ReplyView;using client::FrameInput;using client::ColdInfo;using client::Stats;
/** @brief Ответ владельца транспорта: Pending сохраняет TX/reply borrow; Failed не отменяет его. */
enum class TransportState:std::uint8_t {Pending,Complete,Failed};
/** @brief false означает отказ БЕЗ начала передачи; caller не повторяет запрос автоматически. */
using StartFn=bool(*)(void*,const raw::Key&,const std::uint8_t*,std::size_t) noexcept;
/** @brief Complete допустим лишь после CRC/full reply И настоящего TX sourceDone; reply стабилен до нового StartFn. */
using PollFn=TransportState(*)(void*,ReplyView&) noexcept;
/** @brief Состояние одного Main-owned шага; завершение команды не равно завершению полного кадра. */
enum class Progress:std::uint8_t {Idle,Pending,Complete,Poisoned};
/**
 * @brief PRIVATE cooperative successor FrameClient04 с тем же неизменённым Service05 wire.
 * @details Один Main вызывает beginCold/beginBind/beginFrame/poll, между Pending выполняет c.pump().
 * Ни задач, часов, alloc, callback в Machine, повторов, timeout cancellation или скрытого
 * reset здесь нет. Cold/BIND один раз: production Unbind/rebind ещё НЕ реализован.
 */
class PartialClient final {
public:
    /** @brief Заимствует transport owner; конструктор не вызывает его. */
    PartialClient(void*context,StartFn start,PollFn poll) noexcept:context_(context),start_(start),poll_(poll){}
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    PartialClient(const PartialClient&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    PartialClient&operator=(const PartialClient&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    PartialClient(PartialClient&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    PartialClient&operator=(PartialClient&&)=delete;
    /** @brief Начинает единственный INIT; все LUT CRC проверяются при завершении. */
    bool beginCold(const service::Binding&binding) noexcept;
    /** @brief Начинает BIND после INIT; sequence0 не подменяет strict stream sequence1. */
    bool beginBind() noexcept;
    /** @brief Заимствует immutable wire и caller PCM до Complete либо внешнего joined recovery. */
    bool beginFrame(const FrameInput&input,std::int16_t*pcm,std::size_t capacity) noexcept;
    /** @brief Делает не более одного transport poll И одного нового start; Pending не читает Machine. */
    Progress poll() noexcept;
    /** @brief Защёлкивает неизвестное completion без освобождения wire/PCM/transport state. */
    void noteTimeout() noexcept {fail(Error::Exchange);}
    /** @brief Full frame success только после matching CONSUMED, не после готового PCM. */
    bool frameComplete()const noexcept{return phase_==Phase::Bound&&completion_==Completion::Frame&&error_==Error::None;}
    /** @brief Успешные INIT/BIND/frame являются разными окончаниями. */
    bool bound()const noexcept{return phase_==Phase::Bound&&error_==Error::None;}
    /** @brief Source wire больше не читается клиентом и C6 принял все DATA; НЕ разрешает PCM reuse. */
    bool inputReleased()const noexcept{return inputReleased_;}
    /** @brief Count публикуется только после полного frame success. */
    std::size_t samples()const noexcept{return frameComplete()?sampleCount_:0;}
    /** @brief Возвращает сохранённый код отказа по значению; не сбрасывает owner, wire/PCM borrow и не подтверждает завершение. */ Error error()const noexcept{return error_;}
    /** @brief Заимствует Main-owned counters до уничтожения клиента; последующие операции меняют поля, ссылка не является снимком C6 или completion. */ const Stats&stats()const noexcept{return stats_;}
    /** @brief Заимствует собственные числовые поля INIT до уничтожения клиента; getter не выполняет INIT и сам не подтверждает его завершение или аппаратное измерение. */ const ColdInfo&cold()const noexcept{return cold_;}
private:
    enum class Phase:std::uint8_t {Empty,Cold,Bound,Init,Bind,Begin,Data,Render,Pcm,Consumed,Poisoned};
    enum class Completion:std::uint8_t {None,Init,Bind,Frame};
    /** @brief Сохраняет первый отказ и все заимствованные указатели. */
    bool fail(Error error) noexcept;
    /** @brief Строит полный existing Raw key из immutable App binding. */
    raw::Key key(std::uint16_t opcode,std::uint32_t sequence,std::uint64_t frame,std::uint32_t duration)const noexcept;
    /** @brief Публикует один стабильный request; callback reentry отравляет owner. */
    bool start(raw::Key key,std::size_t bytes) noexcept;
    /** @brief Проверяет common32 и exact Raw identity до body. */
    bool common(const ReplyView&reply) noexcept;
    /** @brief Строит следующую команду по собственному bounded cursor. */
    bool prepare() noexcept;
    /** @brief Принимает один completed ответ, без нового transport call. */
    bool accept(const ReplyView&reply) noexcept;
    /** @brief Проверяет Service05 snapshot40, включая strict cursor и Renderer count. */
    bool snapshot(const ReplyView&,service::State,std::size_t,std::size_t,std::size_t,std::size_t,std::uint32_t) noexcept;
    /** @brief Пишет epoch/input envelope20 без ABI memcpy. */
    void envelope() noexcept;
    /** @brief Продвигает strict sequence и чередует существующие два Raw slots. */
    void advance() noexcept {++sequence_;slot_^=1U;}
    void*context_{};StartFn start_{};PollFn poll_{};
    std::array<std::uint8_t,raw::payloadBytes>body_{};
    service::Binding binding_{};FrameInput frame_{};std::int16_t*pcm_{};std::size_t capacity_{},received_{},offset_{},sampleCount_{},requestCount_{};
    raw::Key request_{};ColdInfo cold_{};Stats stats_{};
    std::uint64_t nextInput_{},nextAudio_{},nextPpu_=1,nextBase_{};
    std::uint32_t sequence_=1;std::uint8_t slot_{};
    Phase phase_=Phase::Empty;Completion completion_=Completion::None;Error error_=Error::None;
    bool pending_{},inside_{},inputReleased_{};
};
} // namespace nes::c6audio::async
