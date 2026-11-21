#pragma once
#include "Machine.h"
#include "AudioStageToken.h"
#include "AudioPublished.h"
#include "../../../runtime/bridge/ThinBridge.h"
#include <cstddef>
#include <cstdint>

namespace nes::app::c6_async {
namespace publication=::nes::c6audio::async;
/** @brief Worker может уступить host, но false означает sticky отказ без отмены владельцев. */
using Wait=bool(*)(void*)noexcept;
/**
 * @brief Необязательная Worker-only отметка границ host-ожидания, без SDK внутри header.
 * @param entering true перед первым wait callback, false после снятия held либо перед timeout отказом.
 * @details Caller может только читать host clock и менять собственную Worker-local numeric запись.
 * Запрещены reentry, изменение Loan/Core/publication/Raw и Main вызовы. Exit НЕ доказывает освобождение:
 * false от wait всё ещё сохраняет владельцев; успешность stage caller проверяет отдельно.
 */
using WaitMark=void(*)(void*context,bool entering)noexcept;

/**
 * @brief PRIVATE once-loan stage→immutable publication→настоящий ACK→finalize без local DSP.
 * @details Main предлагает параметры только joined, затем не читает обычные поля до
 * настоящего done. Конкурентно Main наблюдает исключительно отдельный AudioPublished.
 * Никакой RPC, Renderer, PCM copy, аллокации или нового guest clock здесь нет.
 */
class RemoteLoan final {
public:
    /** @brief Этапы описывают Core once-token, а не remote PCM или фактический Worker join. */
    enum class Phase:std::uint8_t {Idle,Offered,Published,Staged,Acknowledged,Finalized,Accepted,Poisoned};
    /** @brief Пустой owner не заимствует источники до offer. */
    RemoteLoan()noexcept=default;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    RemoteLoan(const RemoteLoan&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    RemoteLoan&operator=(const RemoteLoan&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    RemoteLoan(RemoteLoan&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    RemoteLoan&operator=(RemoteLoan&&)=delete;
    /**
     * @brief Joined Main предлагает прежний retired packet и полный независимый App/audio ключ.
     * @param expected audio.endCycle должен быть0: фактический конец публикует Worker после stage.
     * @param publication Стабильный handoff может ещё удерживать N; Worker N+1 ждёт его только перед stage.
     * @param wait Ограниченный deadline/host-yield callback без вызова Main акторов.
     * @details Источники живут до actual join и снятия published/transport borrows.
     */
    bool offer(core::Machine&primary,core::AudioPacket&packet,core::AudioStageToken&token,
               publication::AudioPublished&publication,const core::ColourLease&lease,
               std::uint32_t bridgeSession,const publication::PublishedKey&expected,
               Wait wait,void*waitContext)noexcept{
        if((phase_!=Phase::Idle&&phase_!=Phase::Accepted)||token.valid()||!bridgeSession||!wait||
           lease.producer!=&primary||lease.epoch!=primary.colourTransportEpoch()||lease.region!=primary.region()||
           !lease.generation||expected.appGeneration!=lease.generation||!expected.romLease||expected.romEpoch!=lease.epoch||!expected.session||
           expected.arena!=1||expected.inputFrame!=primary.frames()||expected.inputFrame==~std::uint64_t{0}||
           expected.audio.generation!=primary.audioGeneration()||expected.audio.ppuFrame!=expected.inputFrame+1U||
           expected.audio.region!=lease.region||expected.audio.endCycle||
           overlaps(&packet,sizeof(packet),this,sizeof(*this))||overlaps(&publication,sizeof(publication),&packet,sizeof(packet))||
           overlaps(&publication,sizeof(publication),&primary,sizeof(primary))||overlaps(&publication,sizeof(publication),&token,sizeof(token)))return false;
        primary_=&primary;packet_=&packet;token_=&token;publication_=&publication;lease_=lease;expected_=expected;
        bridgeSession_=bridgeSession;wait_=wait;waitContext_=waitContext;ticket_={};cut_={};sequence_=0;
        phase_=Phase::Offered;return true;
    }
    /**
     * @brief Worker после actual CommitPublished получает свободный retired arena, stage и release-публикацию.
     * @details Ожидание не меняет guest время. Ни local Renderer, ни Main callback не вызываются.
     * @param mark Необязательная bounded Worker-local отметка; ровно enter/exit при observed held, ни одной при свободной публикации.
     * @param markContext Заимствован только до возврата; не сохраняется в объекте и не меняет layout.
     * @details Отметки стоят снаружи do/while: внутри цикла нет новых clock/counter операций.
     */
    bool stageAndPublish(const core::ColourCut&cut,std::uint64_t sequence,bridge::Ticket ticket,
                         WaitMark mark=nullptr,void*markContext=nullptr)noexcept{
        if(phase_!=Phase::Offered||ticket.session!=bridgeSession_||!ticket.serial||cut.frame!=expected_.inputFrame+1U)return fail();
        cut_=cut;sequence_=sequence;ticket_=ticket;phase_=Phase::Published;
        if(publication_->held()){
            if(mark)mark(markContext,true);
            do{
                if(!wait_(waitContext_)){
                    if(mark)mark(markContext,false);
                    return fail();
                }
            }while(publication_->held());
            if(mark)mark(markContext,false);
        }
        if(!primary_->stageAudioPacketForThinCommit(*packet_,*token_))return fail();
        const auto&h=token_->header();const auto&e=expected_.audio;
        if(!token_->valid()||h.fault!=core::AudioFault::None||h.generation!=e.generation||h.sequence!=e.sequence||
           h.ppuFrame!=e.ppuFrame||h.baseCycle!=e.baseCycle||h.endCycle!=cut.cycle||h.endCycle<=h.baseCycle||
           h.endCycle-h.baseCycle>=45000||h.region!=e.region||h.capabilities!=e.capabilities)return fail();
        expected_.audio.endCycle=h.endCycle;
        if(!publication_->publish(expected_,core::AudioPacketView::from(*packet_)))return fail();
        phase_=Phase::Staged;return true;
    }
    /** @brief Только настоящий matching Producer.receipt Ready разрешает последующий Core finalize. */
    bool acknowledged(bridge::Ticket ticket)noexcept{
        if(phase_!=Phase::Staged||ticket.session!=ticket_.session||ticket.position!=ticket_.position||ticket.serial!=ticket_.serial)return fail();
        phase_=Phase::Acknowledged;return true;
    }
    /** @brief Worker после stepFrame и настоящего ACK выполняет прежний once retirement без второй копии. */
    bool finalize(core::Machine&primary,std::uint64_t sequence)noexcept{
        if(phase_!=Phase::Acknowledged||primary_!=&primary||sequence_!=sequence||!matches(primary)||
           !primary.finalizeAudioPacketForThinCommit(*token_)||token_->valid()||!token_->completed())return fail();
        phase_=Phase::Finalized;return true;
    }
    /**
     * @brief Joined Main принимает только immutable ключ N; PCM может ещё не быть готов.
     * @details Caller уже взял настоящий done и проверил generation/result/input/ROM/cut.
     * true не снимает AudioPublished/Raw/PCM borrow и не обещает idle будущего N+1.
     */
    bool acceptJoined(core::Machine&primary,const core::ColourLease&lease,std::uint32_t bridgeSession,
                      std::uint64_t input,std::uint64_t sequence,publication::JoinedFrame&joined)noexcept{
        if(phase_!=Phase::Finalized||primary_!=&primary||!core::sameColourLease(lease,lease_)||
           bridgeSession!=bridgeSession_||input!=expected_.inputFrame||sequence!=sequence_||!token_->completed()||!matches(primary))return fail();
        joined={expected_,true,true};phase_=Phase::Accepted;return true;
    }
    /**
     * @brief Только joined teardown без published borrow очищает Core token.
     * @details Remote/Raw fault не разрешает освобождение: held publication возвращает false.
     */
    bool abandonJoined(core::Machine&primary)noexcept{
        if(phase_==Phase::Idle)return true;
        if(primary_!=&primary||!token_||publication_->held())return false;
        if(token_->valid()&&!primary.abandonAudioPacketForThinCommitJoined(*token_))return false;
        if(!token_->resetJoined())return false;
        primary_=nullptr;packet_=nullptr;token_=nullptr;publication_=nullptr;phase_=Phase::Idle;return true;
    }
    /** @brief Фиксирует sticky отказ; не освобождает и не сбрасывает ни один объект. */
    bool poison()noexcept{return fail();}
    /** @brief Разрешён Worker либо Main после actual join; не является атомарным completion. */
    Phase phase()const noexcept{return phase_;}
private:
    /** @brief Невалидный/переполненный object extent консервативно считается alias. */
    static bool overlaps(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
        if(!a||!b||!an||!bn||an>~std::uintptr_t{0}-x||bn>~std::uintptr_t{0}-y)return true;
        return x<y+bn&&y<x+an;
    }
    /** @brief Worker либо joined Main проверяет фактический закрытый primary cut N. */
    bool matches(const core::Machine&primary)const noexcept{
        return lease_.producer==&primary&&lease_.epoch==primary.colourTransportEpoch()&&lease_.region==primary.region()&&
            primary.audioGeneration()==expected_.audio.generation&&primary.frames()==cut_.frame&&primary.cpuCycles()==cut_.cycle;
    }
    /** @brief Сохраняет все staged/переданные ресурсы до настоящего joined recovery. */
    bool fail()noexcept{phase_=Phase::Poisoned;return false;}
    core::Machine*primary_{};core::AudioPacket*packet_{};core::AudioStageToken*token_{};
    publication::AudioPublished*publication_{};core::ColourLease lease_{};publication::PublishedKey expected_{};
    bridge::Ticket ticket_{};core::ColourCut cut_{};std::uint64_t sequence_{};std::uint32_t bridgeSession_{};
    Wait wait_{};void*waitContext_{};Phase phase_=Phase::Idle;
};
} // namespace nes::app::c6_async
