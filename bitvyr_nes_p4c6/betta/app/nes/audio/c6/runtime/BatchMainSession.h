#pragma once
#include "../protocol/BatchClient.h"
#include "PcmQueue.h"
#include <cstddef>
#include <cstdint>

namespace nes::app::c6_async {
namespace publication=::nes::c6audio::async;
namespace service=::nes::c6audio::service;
/**
 * @brief PRIVATE cooperative Main-владелец одного encoded wire и двух PCM ticket.
 * @details Worker не обращается к этому объекту. Между pollOne App обслуживает
 * прежний c.pump. Ни Machine, ни token, ни borrowed primary state здесь не читаются.
 * Начальный C6 HELLO/identity является обязанностью внешнего Raw adapter.
 */
class BatchMainSession final {
public:
    /** @brief Связывает существующий client/handoff; transport не запускается конструктором. */
    BatchMainSession(publication::BatchClient&client,publication::AudioPublished&published)noexcept:client_(client),published_(published){}
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    BatchMainSession(const BatchMainSession&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    BatchMainSession&operator=(const BatchMainSession&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    BatchMainSession(BatchMainSession&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    BatchMainSession&operator=(BatchMainSession&&)=delete;
    /**
     * @brief Cold Main открывает один поток и caller-owned buffers до первого guest start.
     * @param wire Ровно доступный span8192Б, CPU-only; actual Raw DMA envelope отдельный.
     * @param first,second Неизменяемые адреса двух PCM массивов capacity mono16 каждый.
     * @details INIT/BIND один раз, второй ROM требует нового протокола; повтор не выполняется.
     */
    bool begin(const ::nes::c6audio::service::Binding&binding,std::uint8_t*wire,std::size_t wireBytes,
               std::int16_t*first,std::int16_t*second,std::size_t capacity)noexcept{
        if(mode_!=Mode::Empty||wireBytes!=8192||!wire||!capacity||capacity>core::Apu::sampleCapacity||
           !separateStorage(wire,wireBytes)||!separateStorage(first,capacity*sizeof(*first))||
           !separateStorage(second,capacity*sizeof(*second))||
           !apart(wire,wireBytes,first,capacity*sizeof(*first))||!apart(wire,wireBytes,second,capacity*sizeof(*second))||
           !pcm_.initialize(first,second,capacity))return false;
        binding_=binding;wire_=wire;wireBytes_=wireBytes;
        if(!client_.beginCold(binding_))return fail();
        mode_=Mode::Init;return true;
    }
    /**
     * @brief Один bounded client poll и не более одного нового start; C0 может работать между вызовами.
     * @details Pending не означает timeout. false удерживает wire/PCM/publication до известного recovery.
     */
    bool pollOne()noexcept{
        if(mode_==Mode::Poisoned||mode_==Mode::Empty)return false;
        const auto progress=client_.poll();
        if(progress==publication::Progress::Poisoned)return fail();
        if(mode_==Mode::Unbinding){if(progress==publication::Progress::Complete){if(!client_.unbound())return fail();mode_=Mode::Unbound;}return true;}
        if(mode_==Mode::Unbound||mode_==Mode::RebindReady)return true;
        if(mode_==Mode::Init){
            if(progress==publication::Progress::Complete){if(!client_.beginBind())return fail();mode_=Mode::Bind;}
            return true;
        }
        if(mode_==Mode::Bind){if(progress==publication::Progress::Complete)mode_=Mode::Running;else return true;}
        if(active_&&client_.inputReleased()&&acquired_&&publication::same(activeKey_,publishedKey_)&&!inputReleased_){
            if(!published_.releaseInput(publishedKey_,true))return fail();
            inputReleased_=true;
        }
        if(!reclaimIfPossible())return false;
        if(active_&&client_.frameComplete()){
            if(!pcm_.ready(activeKey_,client_.samples()))return fail();
            active_=false;activeKey_={};
        }
        // После DATAComplete старого N BatchClient больше не читает wire. N+1 может быть
        // encoded здесь; beginFrame ждёт bound и явно подтверждает held Result_N своим SUBMIT либо CONSUMED.
        if(!queued_&&(!active_||client_.inputReleased())&&!acquired_&&pcm_.occupied()<2){
            publication::PublishedKey key{};
            if(published_.peekPublishedKey(key)){
                if(!expectedPublication(key)||(mode_==Mode::Draining&&key.inputFrame>finalKey_.inputFrame))return fail();
                core::AudioPacketView view{nullptr,nullptr,0,nullptr,0,nullptr,0};
                if(!published_.acquire(key,view))return fail();
                acquired_=true;publishedKey_=key;inputReleased_=false;
                std::size_t bytes=0;
                if(!::nes::c6audio::wire::encode(view,wire_,wireBytes_,bytes)||!bytes)return fail();
                std::int16_t*destination=nullptr;
                if(!pcm_.reserve(key,destination))return fail();
                queuedInput_={wire_,bytes,key.audio,key.inputFrame};queuedKey_=key;queuedPcm_=destination;queued_=true;
                ++nextEncoded_;nextBase_=key.audio.endCycle;
            }
        }
        if(!active_&&queued_&&client_.bound()){
            if(!client_.beginFrame(queuedInput_,queuedPcm_,pcm_.capacity()))return fail();
            activeKey_=queuedKey_;active_=true;queued_=false;queuedKey_={};queuedPcm_=nullptr;queuedInput_={};
        }
        if(mode_==Mode::Draining&&!active_&&!queued_&&!acquired_&&client_.bound()&&!drainStarted_){
            if(!client_.beginDrain())return fail();
            drainStarted_=true;
        }
        return true;
    }
    /**
     * @brief Main записывает exact N joined proof после настоящего sem/result/lease/cut guard.
     * @details Допускается пока CPU1 N+1 уже активен; никаких primary reads/reset отсюда нет.
     */
    bool noteJoined(const publication::JoinedFrame&joined)noexcept{
        if(mode_!=Mode::Running||!joined.actualDoneTaken||!joined.identityValidated||!sameStream(joined.key)||
           joined.key.inputFrame!=nextJoined_)return fail();
        joined_=joined;++nextJoined_;return reclaimIfPossible();
    }
    /** @brief INIT/BIND действительно завершены; это не утверждение Worker или PCM completion. */
    bool initialized()const noexcept{return mode_==Mode::Running||mode_==Mode::Draining;}
    /** @brief Готовность старейшего PCM_N для фактически dispatched N+1 либо joined final drain. */
    bool beginOutput(std::uint64_t dispatchedInput,bool draining,PcmQueue::Output&out)noexcept{
        return (mode_==Mode::Running||mode_==Mode::Draining)&&(!draining||mode_==Mode::Draining)&&pcm_.beginOutput(dispatchedInput,draining,out);
    }
    /** @brief Учитывает только реальные I2S accepted samples после возврата source-reading API. */
    bool outputAccepted(const publication::PublishedKey&key,std::size_t samples)noexcept{
        if((mode_!=Mode::Running&&mode_!=Mode::Draining)||!pcm_.outputAccepted(key,samples))return fail();
        return true;
    }
    /** @brief Успешный idle требует remote CONSUMED, пустой PCM FIFO и снятый published source. */
    bool drained()const noexcept{return mode_==Mode::Draining&&drainStarted_&&client_.remoteIdle()&&!active_&&!queued_&&!acquired_&&!published_.held()&&!pcm_.occupied();}
    /**
     * @brief Main явно замораживает последний full-key после actual Done/identity и отсутствия нового job.
     * @param noJobInFlight Main-owned проверка w.inFlight==false; JoinedN сам не доказывает idleN+1.
     * @details Не читает Machine. Не является отменой: pending/audio/source продолжают poll до drain.
     */
    bool requestDrain(const publication::JoinedFrame&final,bool noJobInFlight)noexcept{
        if(mode_!=Mode::Running||!noJobInFlight||!final.actualDoneTaken||!final.identityValidated||
           !publication::same(final.key,joined_.key)||!sameStream(final.key)||
           final.key.inputFrame==~std::uint64_t{0}||nextJoined_!=final.key.inputFrame+1U||
           nextEncoded_>final.key.inputFrame+1U||
           (active_&&activeKey_.inputFrame>final.key.inputFrame)||(queued_&&queuedKey_.inputFrame>final.key.inputFrame))return fail();
        finalKey_=final.key;mode_=Mode::Draining;return true;
    }
    /**
     * @brief Явный stop использует только последний уже проверенный Main joined ticket.
     * @param noJobInFlight Main-owned отсутствие Worker job; JoinedN не заменяет этот guard.
     * @details Нулевой поток допустим только при доказанном remoteIdle и полном отсутствии owners.
     */
    bool requestFinalDrain(const publication::JoinedFrame&final,bool noJobInFlight)noexcept{
        if(mode_!=Mode::Running||!noJobInFlight)return fail();
        if(!nextJoined_&&!nextEncoded_&&!active_&&!queued_&&!acquired_&&!published_.held()&&client_.remoteIdle()){
            mode_=Mode::Draining;drainStarted_=true;return true;
        }
        return requestDrain(final,noJobInFlight);
    }
    /**
     * @brief После actual Main noJob и полного final drain запускает один checked UNBIND.
     * @details JoinedN не заменяет отсутствие N+1; метод не сбрасывает Machine, transport либо источник по timeout.
     */
    bool beginUnbind(bool noJobInFlight)noexcept{
        if(!noJobInFlight||!drained()||!pcm_.emptyForRebind())return false;
        service::IdleUnbindRequest expected{};
        if(!client_.idleUnbindRequest(expected)||!service::sameIdleBinding(expected.expectedBinding,binding_)||
           expected.completedFrames!=nextJoined_||expected.completedFrames!=nextEncoded_||expected.finalAudioEndCycle!=nextBase_||!client_.beginUnbind(expected))return false;
        mode_=Mode::Unbinding;return true;
    }
    /** @brief Только точный ACK своего UNBIND переводит сессию в состояние для новой привязки. */
    bool unbound()const noexcept{return mode_==Mode::Unbound&&client_.unbound();}
    /**
     * @brief На доказанной unbound/noJob границе переиспользует прежние wire/PCM без allocation.
     * @details Новый RawProfile/HELLO остаётся ответственностью внешнего Main adapter. Старый fault не очищается.
     */
    bool rebindEmpty(const ::nes::c6audio::service::Binding&next,bool noJobInFlight)noexcept{
        if(!noJobInFlight||!unbound()||active_||queued_||acquired_||published_.held()||!pcm_.emptyForRebind())return false;
        if(!client_.rebindEmpty(next))return false;
        if(!pcm_.rebindEmpty())return fail();
        binding_=next;joined_={};activeKey_={};queuedKey_={};publishedKey_={};finalKey_={};queuedInput_={};queuedPcm_=nullptr;
        nextEncoded_=nextJoined_=nextBase_=0;inputReleased_=drainStarted_=false;mode_=Mode::RebindReady;return true;
    }
    /** @brief После отдельно проверенного нового HELLO начинает BIND13; повторного INIT12 нет. */
    bool beginReboundBinding()noexcept{
        if(mode_!=Mode::RebindReady||!client_.beginBind())return false;
        mode_=Mode::Bind;return true;
    }
    /** @brief Возвращает последний полный proof, ранее записанный после actual sem/identity; не читает Worker/Machine. */
    const publication::JoinedFrame&lastJoined()const noexcept{return joined_;}
    /** @brief Числовая занятость двух PCM ticket; не разрешает освобождение по одному этому счётчику. */
    std::size_t occupiedPcm()const noexcept{return pcm_.occupied();}
    /** @brief Максимум один guest frame PCM lead; caller продолжает poll/I2S между отказами. */
    bool canDispatch(std::uint64_t input)const noexcept{return mode_==Mode::Running&&pcm_.canDispatch(input);}
    /** @brief Защёлкивает timeout и сохраняет все заимствования; никакого автоматического abort/reset. */
    bool noteTimeout()noexcept{client_.noteTimeout();return fail();}
private:
    /** @brief Все caller buffers отделены от целых Main/client/publication owners до первой мутации. */
    bool separateStorage(const void*pointer,std::size_t bytes)const noexcept{
        return apart(pointer,bytes,this,sizeof(*this))&&apart(pointer,bytes,&client_,sizeof(client_))&&
            apart(pointer,bytes,&published_,sizeof(published_));
    }
    /** @brief Холодные/рабочие состояния — только Main; Worker их не наблюдает. */
    enum class Mode:std::uint8_t {Empty,Init,Bind,Running,Draining,Unbinding,Unbound,RebindReady,Poisoned};
    /** @brief Проверяет immutable App/audio домены независимо от самой публикации Worker. */
    bool sameStream(const publication::PublishedKey&key)const noexcept{
        return key.appGeneration==binding_.appGeneration&&key.romLease==binding_.romLease&&key.romEpoch==binding_.romEpoch&&
            key.session==binding_.session&&key.arena==1&&key.audio.generation==binding_.audioGeneration&&
            key.audio.region==binding_.region&&key.audio.capabilities==binding_.audioCapabilities;
    }
    /** @brief Принимает actual endCycle только как Worker-sealed bounded продолжение известного base. */
    bool expectedPublication(const publication::PublishedKey&key)const noexcept{
        return sameStream(key)&&key.inputFrame==nextEncoded_&&key.inputFrame!=~std::uint64_t{0}&&
            key.audio.sequence==nextEncoded_&&key.audio.ppuFrame==key.inputFrame+1U&&key.audio.baseCycle==nextBase_&&
            key.audio.endCycle>key.audio.baseCycle&&key.audio.endCycle-key.audio.baseCycle<45000;
    }
    /** @brief Только conjunction конкретных N sourceReleased и joinedN возвращает staged destination Worker. */
    bool reclaimIfPossible()noexcept{
        if(acquired_&&inputReleased_&&publication::same(joined_.key,publishedKey_)){
            if(!published_.reclaimJoined(publishedKey_,joined_))return fail();
            acquired_=false;inputReleased_=false;publishedKey_={};
        }
        return true;
    }
    /** @brief Проверяет полуоткрытые физически доступные extents без overflow/alias. */
    static bool apart(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
        if(!a||!b||an>~std::uintptr_t{0}-x||bn>~std::uintptr_t{0}-y)return false;
        return x+an<=y||y+bn<=x;
    }
    /** @brief Sticky отказ не очищает owned request, staged source, PCM или caller resources. */
    bool fail()noexcept{mode_=Mode::Poisoned;return false;}
    publication::BatchClient&client_;publication::AudioPublished&published_;PcmQueue pcm_{};
    ::nes::c6audio::service::Binding binding_{};publication::JoinedFrame joined_{};
    publication::PublishedKey activeKey_{},queuedKey_{},publishedKey_{},finalKey_{};
    ::nes::c6audio::client::FrameInput queuedInput_{};std::uint8_t*wire_{};std::int16_t*queuedPcm_{};std::size_t wireBytes_{};
    std::uint64_t nextEncoded_{},nextJoined_{},nextBase_{};
    Mode mode_=Mode::Empty;bool active_{},queued_{},acquired_{},inputReleased_{},drainStarted_{};
};
} // namespace nes::app::c6_async
