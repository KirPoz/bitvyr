#pragma once
#include "BatchMainSession.h"
#include "OutputFrame.h"

namespace nes::app::c6_async {
/**
 * @brief Main-only мост двух существующих Frame audio slots в прежний C6 handoff.
 * @tparam Pipeline Настоящий FrameVideoPipeline либо явно ограниченный HOST fixture.
 * @details Нет Machine getters, SDK, времени, задач, аллокаций и копии AudioPacket.
 * Video accept не освобождает audio: ticket живёт до actual DATA/TX sourceDone и
 * reclaimJoined в BatchMainSession. Fault не снимает неизвестный borrow.
 */
template<class Pipeline> class PostJoinFrameAudio final {
public:
    using Ticket=typename Pipeline::JoinedAudioTicket;
    /** @brief Обычный пустой Main owner до cold/rebound настройки. */
    PostJoinFrameAudio()noexcept=default;
    /** @brief Запрещает дублировать сохранённую аренду. */
    PostJoinFrameAudio(const PostJoinFrameAudio&)=delete;
    /** @brief Запрещает дублировать сохранённую аренду. */
    PostJoinFrameAudio&operator=(const PostJoinFrameAudio&)=delete;
    /** @brief Запрещает перенос живого owner в другое место. */
    PostJoinFrameAudio(PostJoinFrameAudio&&)=delete;
    /** @brief Запрещает перенос живого owner в другое место. */
    PostJoinFrameAudio&operator=(PostJoinFrameAudio&&)=delete;
    /** @brief Pending сохраняет lease; Fault не означает освобождение источника. */
    enum class Result:std::uint8_t {Ready,Pending,Fault};
    /**
     * @brief На joined cold/rebound границе фиксирует immutable Frame lease до первого job.
     * @param source Именно outputLeaseAtCreate с исходным ROMepoch и назначенным transportEpoch.
     * @param binding MainOwner binding; romLease — логический App domain, не ROM pointer.
     * @details Старый borrow/poison нельзя очистить новым configure. Никакого INIT/BIND здесь нет.
     */
    bool configure(const core::OutputFrameLease&source,const service::Binding&binding)noexcept{
        if(poisoned_||held_||!source.producer||!source.rom||!source.romBytes||!source.romEpoch||
           !source.generation||!source.transportEpoch||!binding.appGeneration||!binding.romEpoch||
           source.region!=binding.region||!binding.romLease||
           !binding.session||!binding.audioGeneration||binding.firstCommandSequence!=1||
           (binding.audioCapabilities&~core::AudioPacket::externalDacQ16))return false;
        source_=source;binding_=binding;configured_=true;return true;
    }
    /**
     * @brief После внешнего actual done и joinedProducer публикует immutable audio N без Thin token.
     * @param input Независимый проверенный App inputFrame завершённого job.
     * @param actualDoneTaken Только доверенный App после настоящего семафора, не timeout/finished.
     * @param identityValidated App уже сверил result/generation/ROM/region/produced/cut.
     * @details После успешного borrow ошибка удерживает ticket. Никакого rollback, local DSP либо повторной публикации.
     */
    Result publishJoined(Pipeline&pipeline,publication::AudioPublished&published,BatchMainSession&session,
                         const service::Binding&binding,std::uint64_t input,
                         bool actualDoneTaken,bool identityValidated)noexcept{
        core::AudioPacketView view{nullptr,nullptr,0,nullptr,0,nullptr,0};Ticket ticket{};
        const auto reserved=reserveJoined(pipeline,published,binding,input,actualDoneTaken,identityValidated,view,ticket);
        return reserved==Result::Ready?publishReservedOriginal(published,session,binding):reserved;
    }
    /**
     * @brief После настоящего внешнего join удерживает original loan без публикации.
     * @details Reserved/Building не освобождается по AudioPublished.empty. Возвращённый
     * ticket годится для immutable Tape getter того же Pipeline, не для live Machine.
     */
    Result reserveJoined(Pipeline&pipeline,const publication::AudioPublished&published,
                         const service::Binding&binding,std::uint64_t input,bool actualDoneTaken,bool identityValidated,
                         core::AudioPacketView&out,Ticket&original)noexcept{
        if(poisoned_)return Result::Fault;
        if(!configured_||!actualDoneTaken||!identityValidated||!bindingMatches(binding)||input==~std::uint64_t{0})return fail();
        if(held_||published.held())return Result::Pending;
        core::AudioPacketView view{nullptr,nullptr,0,nullptr,0,nullptr,0};Ticket next{};
        if(!pipeline.borrowJoinedAudio(view,next))return fail();
        ticket_=next;held_=true;published_=false;sourceView_=view;
        const auto&t=ticket_.output;const auto&h=ticket_.audio;
        if(!sameSource(t.lease,source_)||ticket_.appGeneration!=binding.appGeneration||
           t.inputFrame!=input||t.sequence!=input||t.slot>=2U||t.slot!=unsigned(input%2U)||
           h.fault!=core::AudioFault::None||h.generation!=binding.audioGeneration||h.sequence!=input||
           h.ppuFrame!=input+1U||h.region!=binding.region||h.capabilities!=binding.audioCapabilities||
           h.endCycle<=h.baseCycle||h.endCycle-h.baseCycle>=45000U||
           t.baseCycle!=(input?h.baseCycle:7U)||h.endCycle<=t.baseCycle||
           !view.readable()||!core::sameAudioHeader(view.header(),h))return fail();
        key_={binding.appGeneration,binding.romLease,binding.romEpoch,binding.session,input,
            {h.generation,h.sequence,h.ppuFrame,h.baseCycle,h.endCycle,h.region,h.capabilities},1};
        out=view;original=ticket_;return Result::Ready;
    }
    /** @brief Прежняя оригинальная публикация требует неизменный exact Header и исходный view. */
    template<class JoinedSession>
    Result publishReservedOriginal(publication::AudioPublished&published,JoinedSession&session,const service::Binding&binding)noexcept{
        if(!sourceView_.readable()||!core::sameAudioHeader(sourceView_.header(),ticket_.audio))return fail();
        return postReserved(published,session,binding,sourceView_);
    }
    /**
     * @brief Публикует только derived FM: Header отличается лишь expansionCount, DMC лишь level.
     * @details Native Input обязаны оставаться тем же immutable массивом. Исходный
     * ticket/Header/key сохраняются отдельно до real DATA/TXdone+exact joined reclaim.
     */
    template<class JoinedSession>
    Result publishReservedDerived(publication::AudioPublished&published,JoinedSession&session,
                                  const service::Binding&binding,const core::AudioPacketView&derived)noexcept{
        if(poisoned_||!held_||published_||!sourceView_.readable()||!derived.readable()||
           !core::sameAudioHeader(sourceView_.header(),ticket_.audio))return fail();
        const auto&h=derived.header();auto expected=ticket_.audio;expected.expansionCount=h.expansionCount;
        if(ticket_.audio.capabilities!=core::AudioPacket::externalDacQ16||ticket_.audio.expansionCount||
           !core::sameAudioHeader(h,expected)||derived.inputs()!=sourceView_.inputs()||
           (h.dmcCount&&derived.dmc()==sourceView_.dmc()))return fail();
        for(std::size_t i=0;i<h.dmcCount;++i){const auto&a=sourceView_.dmc()[i];const auto&b=derived.dmc()[i];
            if(a.expansionLevel||a.offset!=b.offset||a.address!=b.address||a.value!=b.value||a.stall!=b.stall||a.reserved!=b.reserved||
               !core::AudioPacket::validExpansionLevel(b.expansionLevel))return fail();}
        return postReserved(published,session,binding,derived);
    }
    /**
     * @brief Возвращает exact физический audio slot лишь после actual publication reclaim.
     * @details Пустая publication после успешного publish означает DATA acceptance/TXdone
     * плюс matching JoinedFrame, поскольку очистить её может только BatchMainSession.
     * PCM ready/CONSUMED, timeout или video accept сами по себе сюда не допускают.
     */
    Result releaseIfReclaimed(Pipeline&pipeline,const publication::AudioPublished&published)noexcept{
        if(poisoned_)return Result::Fault;
        if(!held_)return Result::Ready;
        if(!published_||published.held())return Result::Pending;
        if(!published.reclaimedMainFrame(key_)||!pipeline.releaseJoinedAudio(ticket_,true))return fail();
        held_=published_=false;ticket_={};key_={};sourceView_={nullptr,nullptr,0,nullptr,0,nullptr,0};return Result::Ready;
    }
    /** @brief Читает только Main state; false не заменяет Frame/Worker/PPA teardown guards. */
    bool empty()const noexcept{return !held_&&!poisoned_;}
    /** @brief Poisoned сохраняет все ordinary данные и заимствованные физические адреса. */
    bool poison()noexcept{poisoned_=true;return false;}
private:
    /** @brief Единственная actual publication; flag Published ставится лишь после noteJoined exact key. */
    template<class JoinedSession>
    Result postReserved(publication::AudioPublished&published,JoinedSession&session,const service::Binding&binding,
                        const core::AudioPacketView&view)noexcept{
        if(poisoned_||!held_||published_||published.held()||!bindingMatches(binding))return fail();
        if(!published.publish(key_,view))return fail();
        const publication::JoinedFrame joined{key_,true,true};
        if(!session.noteJoined(joined))return fail();
        published_=true;return Result::Ready;
    }
    /** @brief Полное сравнение immutable Frame source без разыменования producer/ROM. */
    static bool sameSource(const core::OutputFrameLease&a,const core::OutputFrameLease&b)noexcept{
        return a.producer==b.producer&&a.rom==b.rom&&a.romBytes==b.romBytes&&a.romEpoch==b.romEpoch&&
            a.generation==b.generation&&a.transportEpoch==b.transportEpoch&&a.region==b.region;
    }
    /** @brief Сверяет неизменные source домены с текущим MainOwner, а не с живой Machine. */
    bool bindingMatches(const service::Binding&binding)const noexcept{
        return binding_.appGeneration==binding.appGeneration&&binding_.romLease==binding.romLease&&
            binding_.romEpoch==binding.romEpoch&&binding_.session==binding.session&&
            binding_.audioGeneration==binding.audioGeneration&&binding_.region==binding.region&&
            binding_.audioCapabilities==binding.audioCapabilities&&binding.firstCommandSequence==1;
    }
    /** @brief Терминальный отказ не освобождает source, Pipeline либо publication. */
    Result fail()noexcept{poisoned_=true;return Result::Fault;}
    core::OutputFrameLease source_{};
    service::Binding binding_{}; // Независимый C6 domain; равенство epoch между подсистемами не предполагается.
    Ticket ticket_{};
    core::AudioPacketView sourceView_{nullptr,nullptr,0,nullptr,0,nullptr,0};
    publication::PublishedKey key_{};
    bool configured_=false,held_=false,published_=false,poisoned_=false;
};
} // namespace nes::app::c6_async
