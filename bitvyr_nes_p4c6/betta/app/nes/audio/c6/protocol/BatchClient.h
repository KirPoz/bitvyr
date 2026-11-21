#pragma once
#include "PartialClient.h"
#include "BatchProtocol.h"
#include "IdleUnbindWire.h"

namespace nes::c6audio::async {
/**
 * @brief Один Main-владелец lossless batch либо прежнего6RPC в одинаковом layout.
 * @details Transport callbacks имеют прежний контракт PartialClient: Complete
 * только после CRC/full-key и actual request TXsourceDone. Один poll допускает
 * не более одного transport poll и одного start. Нет alloc/часов/задач/повторов.
 */
static_assert(batch::submitPrefixBytes+batch::maximumSubmitWireBytes<=raw::requestPayloadBytes);
static_assert(32+batch::maximumFastBodyBytes<=raw::payloadBytes);
class BatchClient final {
public:
    /** @brief Холодная стратегия общего клиента; не меняет C6 binding или Renderer. */
    enum class Backend:std::uint8_t {Legacy,Batch};
    static constexpr std::size_t submitPrefixBytes=batch::submitPrefixBytes,maximumSubmitWireBytes=batch::maximumSubmitWireBytes,maximumBatchPcmSamples=batch::maximumFastSamples;
    /** @brief Заимствует transport; конструктор не отправляет команды. */
    BatchClient(void*context,StartFn start,PollFn poll,Backend backend) noexcept
        :context_(context),start_(start),poll_(poll),backend_(backend){}
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    BatchClient(const BatchClient&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    BatchClient&operator=(const BatchClient&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    BatchClient(BatchClient&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    BatchClient&operator=(BatchClient&&)=delete;
    /** @brief Один INIT с проверкой прежних LUT CRC перед BIND. */
    bool beginCold(const service::Binding&) noexcept;
    /** @brief Один BIND после actual INIT; sequence0 не подменяет service sequence1. */
    bool beginBind() noexcept;
    /** @brief Принимает exact следующий fullkey и отдельный writable PCM span до запуска RPC. */
    bool beginFrame(const FrameInput&,std::int16_t*pcm,std::size_t capacity) noexcept;
    /** @brief Явно подтверждает последний remote Result; проверенный truejoin обеспечивает MainSession. */
    bool beginDrain() noexcept;
    /** @brief Возвращает полный cursor только после подтверждённого remote CONSUMED и отсутствия RPC/PCM borrow. */
    bool idleUnbindRequest(service::IdleUnbindRequest&out) const noexcept;
    /** @brief Начинает один checked UNBIND; несовпадение cursor отказывает до transport/start. */
    bool beginUnbind(const service::IdleUnbindRequest&expected) noexcept;
    /** @brief ACK160 принят с full-key/echo/snapshot; это не C6 reply sourceDone или Worker join. */
    bool unbound() const noexcept {return phase_==Phase::Unbound&&!pending_&&error_==Error::None;}
    /**
     * @brief Готовит новый BIND без повторного INIT только после своего checked UNBIND ACK.
     * @pre Main отдельно завершил все publication/PCM/Worker owners и квалифицирует новый RawProfile/HELLO до beginBind.
     * @details Не очищает poison/timeout; cold LUT/resources и backend сохраняются, новых буферов нет.
     */
    bool rebindEmpty(const service::Binding&next) noexcept;
    /** @brief Один неблокирующий шаг; ошибка сохраняет все неизвестные заимствования. */
    Progress poll() noexcept;
    /** @brief Timeout не освобождает transport/source/PCM и не делает retry. */
    void noteTimeout() noexcept {fail(Error::Exchange);}
    /** @brief Полный проверенный PCM скопирован в caller slot; remote Result ещё может быть held. */
    bool frameComplete()const noexcept{return completion_==Completion::Frame&&error_==Error::None;}
    /** @brief Новый frame допустим только без pending RPC, с неизменным потоком. */
    bool bound()const noexcept{return phase_==Phase::Bound&&!pending_&&error_==Error::None;}
    /** @brief Remote Idle доказан explicit CONSUMED ACK, а не DMA sourceDone. */
    bool remoteIdle()const noexcept{return bound()&&!remoteHeld_;}
    /** @brief Полный input подтверждён C6 и transport TXsourceDone; это не PCM или Worker completion. */
    bool inputReleased()const noexcept{return inputReleased_;}
    /** @brief Count доступен при full PCM ready, даже если следующий remote consume ещё впереди. */
    std::size_t samples()const noexcept{return frameComplete()?sampleCount_:0;}
    /** @brief Возвращает первый sticky отказ, не изменяя владельца. */
    Error error()const noexcept{return error_;}
    /** @brief Числовая Main-owned статистика, не Worker completion. */
    const Stats&stats()const noexcept{return stats_;}
    /** @brief Проверенные INIT LUT/resources, не новое hardware измерение. */
    const ColdInfo&cold()const noexcept{return cold_;}
private:
    enum class Phase:std::uint8_t {Empty,Cold,Bound,Init,Bind,Begin,Data,Render,Pcm,Consumed,Submit,Fetch,PriorConsumed,FinalConsumed,Unbind,Unbound,Poisoned};
    enum class Completion:std::uint8_t {None,Init,Bind,Frame,Drain,Unbind};
    /** @brief Хранит только immutable metadata предыдущего Result, не указатель на старый wire. */
    struct Previous final {wire::Expected audio{};std::uint64_t inputFrame{};std::uint32_t lastPcmSequence{};};
    /** @brief Защёлкивает первый отказ, не очищая resource pointers. */
    bool fail(Error) noexcept;
    /** @brief Строит immutable Raw key без усечения допустимого input64 в32. */
    raw::Key key(std::uint16_t,std::uint32_t,std::uint64_t,std::uint32_t)const noexcept;
    /** @brief Публикует один стабильный request; callback reentry отравляет owner. */
    bool start(raw::Key,std::size_t) noexcept;
    /** @brief Проверяет outer reply/common32 до чтения payload. */
    bool common(const ReplyView&) noexcept;
    /** @brief Строит следующую old либо batch команду по своему cursor. */
    bool prepare() noexcept;
    /** @brief Принимает один completed ответ; новый start выполняется отдельно poll(). */
    bool accept(const ReplyView&) noexcept;
    /** @brief Проверяет прежний Service snapshot40 без ослабления cursor/call guards. */
    bool snapshot(const ReplyView&,service::State,std::size_t,std::size_t,std::size_t,std::size_t,std::uint32_t) noexcept;
    /** @brief Проверяет PCM32/count/offset/flags и копирует ровно указанный LE prefix. */
    bool acceptPcm(const ReplyView&,bool firstBatch) noexcept;
    /** @brief Публикует full PCM ready и следующий audio-domain, сохраняя independent remote owner. */
    bool finishPcm() noexcept;
    /** @brief Пишет прежний envelope20 для заданного exact input. */
    void envelope(std::uint64_t) noexcept;
    /** @brief Продвигает strict sequence и чередует два existing Raw slots. */
    void advance() noexcept {++sequence_;slot_^=1U;}
    void*context_{};StartFn start_{};PollFn poll_{};Backend backend_{};
    std::array<std::uint8_t,raw::requestPayloadBytes>body_{};
    service::Binding binding_{};FrameInput frame_{};Previous previous_{};
    std::int16_t*pcm_{};std::size_t capacity_{},received_{},offset_{},sampleCount_{},requestCount_{};
    raw::Key request_{};ColdInfo cold_{};Stats stats_{};
    std::uint64_t nextInput_{},nextAudio_{},nextPpu_=1,nextBase_{};
    std::uint32_t sequence_=1;std::uint8_t slot_{};
    Phase phase_=Phase::Empty;Completion completion_=Completion::None;Error error_=Error::None;
    bool pending_{},inside_{},inputReleased_{},remoteHeld_{},fastFrame_{},submitAcknowledgesPrevious_{};
};
} // namespace nes::c6audio::async
