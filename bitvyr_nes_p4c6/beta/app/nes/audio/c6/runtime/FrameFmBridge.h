#pragma once
#include "FrameFmPacket.h"
#include "PostJoinFrameAudio.h"
#include <array>

namespace nes::app::c6_async {
/**
 * @brief Main FM transform над ЕДИНСТВЕННЫМ внешним PostJoinFrameAudio loan-owner.
 * @details Весь объект создаётся cold в PSRAM; Worker хранит только pointer.
 * Сначала actual join/reserve N, затем Root dispatch N+1, затем bounded build/pump.
 * Никакой live Machine, SDK/clock/heap в buildStep; источник и derived immutable
 * до existing DATA/TXdone+matchingJoined exact reclaim. Pause не сбрасывает FM.
 */
template<class Pipeline> class FrameFmBridge final {
public:
    using Loan=PostJoinFrameAudio<Pipeline>;
    using Ticket=typename Pipeline::JoinedAudioTicket;
    enum class Result:std::uint8_t {Ready,Pending,Fault};
    enum class Phase:std::uint8_t {Empty,Building,Ready,Published,Failed};
    /** @brief Cold FM/audio generation0; production allocation выполняет отдельный PSRAM factory. */
    FrameFmBridge()noexcept=default;
    /** @brief Запрещает копию владельца FM-состояния и заимствований: один source loan не должен возвращаться дважды. */
    FrameFmBridge(const FrameFmBridge&)=delete;
    /** @brief Запрещает заменять действующий bridge копией чужой последовательности и её незавершённых loans. */
    FrameFmBridge&operator=(const FrameFmBridge&)=delete;
    /**
     * @brief Pins originalN и sealedTape того жеticket без синтеза/копии DMC.
     * @details Единственный raw loan берёт существующий owner; wrapper его не дублирует.
     * Root вызывает послеactualdone/identity и доdispatchN+1. Ready здесь значит Reserved.
     */
    Result reserveJoined(Loan&owner,Pipeline&pipeline,const publication::AudioPublished&published,
        const service::Binding&binding,std::uint64_t input,bool actualDoneTaken,bool identityValidated)noexcept{
        if(phase_==Phase::Failed||!sameOwner(owner))return fail(owner);
        if(phase_!=Phase::Empty)return phase_==Phase::Published?Result::Pending:fail(owner);
        core::AudioPacketView view{nullptr,nullptr,0,nullptr,0,nullptr,0};Ticket ticket{};
        const auto reserved=owner.reserveJoined(pipeline,published,binding,input,actualDoneTaken,identityValidated,view,ticket);
        if(reserved==Loan::Result::Pending)return Result::Pending;
        if(reserved!=Loan::Result::Ready)return fail(owner);
        core::Vrc7WriteTape::View tape{};
        if(!pipeline.borrowJoinedVrc7Tape(ticket,tape)||ticket.output.slot>=2U||slots_[ticket.output.slot].storage.phase()!=frame_fm::DerivedFmPhase::Empty)return fail(owner);
        source_=view;tape_=tape;slot_=ticket.output.slot;begun_=false;phase_=Phase::Building;return Result::Ready;
    }
    /**
     * @brief Один bounded FM квант и pump; первый вызов делает только pumped preflight.
     * @details preflight/copy yield каждые128records; FM step≤maxM2, native≤maxNative,
     * write/query≤1. Pump false — stickyFAILED, не release/reset/синтетическийjoin.
     * App сохраняет исходный абсолютный deadline, не начинает новый на каждом кванте.
     */
    template<class Pump> Result buildStep(Loan&owner,Pump pump,unsigned maxM2=512U,unsigned maxNative=128U)noexcept{
        if(!sameOwner(owner)||phase_==Phase::Failed)return fail(owner);
        if(phase_==Phase::Ready)return Result::Ready;
        if(phase_!=Phase::Building||slot_>=2U)return fail(owner);
        if(!begun_){
            if(!builder_.begin(source_,tape_,slots_[slot_].storage,pump))return fail(owner);
            begun_=true;
            if(!pump())return fail(owner);
            return Result::Pending;
        }
        const auto step=builder_.step(maxM2,maxNative);
        if(step==frame_fm::DerivedFmBuilder::StepResult::Fault)return fail(owner);
        if(step==frame_fm::DerivedFmBuilder::StepResult::Ready)phase_=Phase::Ready;
        if(!pump())return fail(owner);
        return phase_==Phase::Ready?Result::Ready:Result::Pending;
    }
    /** @brief Публикует immutable derived через тот же raw owner и existing exact key/noteJoined. */
    template<class JoinedSession> Result publishReady(Loan&owner,publication::AudioPublished&published,JoinedSession&session,const service::Binding&binding)noexcept{
        if(!sameOwner(owner)||phase_!=Phase::Ready||slot_>=2U)return fail(owner);
        core::AudioPacketView view{nullptr,nullptr,0,nullptr,0,nullptr,0};
        if(!slots_[slot_].storage.view(view)||owner.publishReservedDerived(published,session,binding,view)!=Loan::Result::Ready)return fail(owner);
        phase_=Phase::Published;return Result::Ready;
    }
    /**
     * @brief Единственный pump release route для FM: Reserved/Building/Ready не освобождаются.
     * @details Лишь existing owner Ready после Published/exact receipt допускает возврат
     * raw ticket (вместе сTape черезPipeline), затем recycleMain. Empty доpublish не receipt.
     */
    Result releaseIfReclaimed(Loan&owner,Pipeline&pipeline,const publication::AudioPublished&published)noexcept{
        if(!sameOwner(owner)||phase_==Phase::Failed)return fail(owner);
        if(phase_==Phase::Empty)return owner.empty()?Result::Ready:fail(owner);
        if(phase_!=Phase::Published)return Result::Pending;
        const auto result=owner.releaseIfReclaimed(pipeline,published);
        if(result==Loan::Result::Fault)return fail(owner);
        if(result==Loan::Result::Pending)return Result::Pending;
        if(!owner.empty()||slot_>=2U||!slots_[slot_].storage.recycleAfterJoinedReclaim())return fail(owner);
        source_={nullptr,nullptr,0,nullptr,0,nullptr,0};tape_={};slot_=2U;begun_=false;phase_=Phase::Empty;return Result::Ready;
    }
    /** @brief Building/Ready отделены отPublished; getter не смотрит Worker/CPU1. */
    bool building()const noexcept{return phase_==Phase::Building;}
    /** @brief Только Main phase, не proof физического owner завершения. */
    Phase phase()const noexcept{return phase_;}
    /** @brief Условие удаления FM-объекта; Worker/PPA/C6 UNBOUND separately проверяет Root. */
    bool emptyForTeardown(const Loan&owner)const noexcept{
        return (!owner_||owner_==&owner)&&phase_==Phase::Empty&&owner.empty()&&!builder_.building()&&
            slots_[0].storage.phase()==frame_fm::DerivedFmPhase::Empty&&slots_[1].storage.phase()==frame_fm::DerivedFmPhase::Empty;
    }
private:
    /** @brief Один Main-слот; arrays принадлежат PSRAM bridge, Inputs остаются в originalPayload. */
    struct Slot {
        std::array<core::AudioPacket::Dmc,core::AudioPacket::dmcCapacity>dmc{};
        std::array<core::AudioPacket::Expansion,705>expansion{};
        frame_fm::DerivedFmStorage storage{dmc.data(),dmc.size(),expansion.data(),expansion.size()};
    };
    /** @brief Фиксирует адрес единственного внешнего rawowner при первом обращении. */
    bool sameOwner(Loan&owner)noexcept{if(!owner_)owner_=&owner;return owner_==&owner;}
    /** @brief Sticky quarantine держит original owner, buffers и частичную FMphase. */
    Result fail(Loan&owner)noexcept{
        phase_=Phase::Failed;static_cast<void>(builder_.poison());if(owner_)static_cast<void>(owner_->poison());else static_cast<void>(owner.poison());return Result::Fault;
    }
    Loan*owner_=nullptr;
    std::array<Slot,2>slots_{};
    frame_fm::DerivedFmBuilder builder_{};
    core::AudioPacketView source_{nullptr,nullptr,0,nullptr,0,nullptr,0};
    core::Vrc7WriteTape::View tape_{};
    unsigned slot_=2;
    bool begun_=false;
    Phase phase_=Phase::Empty;
};
} // namespace nes::app::c6_async
