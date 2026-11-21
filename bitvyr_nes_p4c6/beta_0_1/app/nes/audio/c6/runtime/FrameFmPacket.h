#pragma once
#include "FrameFmTimeline.h"
#include "cartridge/Vrc7WriteTape.h"
#include "apu/AudioPacketView.h"
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nes::app::frame_fm {
/** @brief Состояние только Main-converter storage; Ready не равен C6 Published/sourceDone. */
enum class DerivedFmPhase:std::uint8_t {Empty,Building,Ready,Failed};
/** @brief Первая причина отказа; временная лента/partial FM не откатываются. */
enum class DerivedFmFault:std::uint8_t {None,Source,Tape,Sequence,Capacity,Alias,Busy,Timeline,Pump};
/** @brief Native/default preflight не имеет SDK; App передаёт bounded pump callback. */
struct ContinueFmBuild {
    /** @brief Разрешает продолжение native-проверки без SDK и транспорта; реальный App передаёт собственный bounded pump. */
    bool operator()()const noexcept{return true;}
};
class DerivedFmBuilder;
/**
 * @brief Main-owned typed destination: Header/Dmc/Expansion отдельны от Ready Payload.
 * @details Native Input остаются borrowed из original joined source до его реального
 * DATA+TXdone+matchingJoined reclaim. App держит originalJoinedTicket/source-key вне
 * converter. Ни phase Ready, ни empty publication не разрешают вернуть этот loan.
 */
class DerivedFmStorage final {
public:
    /** @brief Заимствует настоящие writable extents одного Main-слота; heap/публикации нет. */
    DerivedFmStorage(core::AudioPacket::Dmc*dmc,std::size_t dmcCapacity,
        core::AudioPacket::Expansion*expansion,std::size_t expansionCapacity)noexcept:
        dmc_(dmc),dmcCapacity_(dmcCapacity),expansion_(expansion),expansionCapacity_(expansionCapacity){}
    /** @brief Запрещает копию заимствованных массивов и phase: это не передача владения готовым audio view. */
    DerivedFmStorage(const DerivedFmStorage&)=delete;
    /** @brief Запрещает потерю прежних backing-массивов и original-source metadata через присваивание. */
    DerivedFmStorage&operator=(const DerivedFmStorage&)=delete;
    /** @brief Даёт immutable derived view лишь после полного построения; source loan ещё удерживается App. */
    bool view(core::AudioPacketView&out)const noexcept{
        if(phase_!=DerivedFmPhase::Ready)return false;
        out={&header_,inputs_,header_.inputCount,dmc_,header_.dmcCount,expansion_,header_.expansionCount};return out.readable();
    }
    /** @brief Точная исходная metadata для внешнего originalJoinedTicket, не новый source key. */
    const core::AudioPacket::Header& originalHeader()const noexcept{return original_;}
    /** @brief Показывает converter state, не владельцев Worker/C6/PPA. */
    DerivedFmPhase phase()const noexcept{return phase_;}
    /** @brief Наблюдаемый seed до исполнения; не save-state. */
    FmSeed beginSeed()const noexcept{return begin_;}
    /** @brief Наблюдаемый seed после точного endCycle; фазы будут продолжены следующим слотом. */
    FmSeed endSeed()const noexcept{return end_;}
    /**
     * @brief Очищает Main metadata ПОСЛЕ внешнего actual joined-source reclaim.
     * @pre App проверил originalJoinedTicket и exact published key, DATA+TXsourceDone+
     * matchingJoined, снял все views. Этот метод не умеет и не утверждает эти проверки.
     */
    bool recycleAfterJoinedReclaim()noexcept{
        if(phase_!=DerivedFmPhase::Ready)return false;
        header_={};original_={};inputs_=nullptr;begin_={};end_={};phase_=DerivedFmPhase::Empty;return true;
    }
private:
    friend class DerivedFmBuilder;
    core::AudioPacket::Header header_{},original_{};
    const core::AudioPacket::Input*inputs_=nullptr;
    core::AudioPacket::Dmc*dmc_;std::size_t dmcCapacity_;
    core::AudioPacket::Expansion*expansion_;std::size_t expansionCapacity_;
    FmSeed begin_{},end_{};
    DerivedFmPhase phase_=DerivedFmPhase::Empty;
};
/**
 * @brief Один непрерывный CPU0 FM converter; raw Tape не попадает в C6 wire.
 * @details FM model принадлежит builder, а два derived storage — Main. На первом
 * seq0 audioBase0 принимается только cold-arm traceBase7; дальнейшие trace/audio
 * base равны предыдущему end. Capture/source/epoch admission и actual join — App.
 */
class DerivedFmBuilder final {
public:
    /** @brief Новый builder соответствует полному audio reset0, ещё без source generation. */
    DerivedFmBuilder()noexcept=default;
    /** @brief Запрещает дублирование непрерывной FM-фазы и указателя на незавершённый destination. */
    DerivedFmBuilder(const DerivedFmBuilder&)=delete;
    /** @brief Запрещает подмену FM-истории и sticky fault копией другого builder. */
    DerivedFmBuilder&operator=(const DerivedFmBuilder&)=delete;
    /** @brief Запрещает перемещение builder, пока внешний Main-owner хранит его адрес и связанные loans. */
    DerivedFmBuilder(DerivedFmBuilder&&)=delete;
    /** @brief Запрещает перенос активного FM-конвейера присваиванием без отдельного протокола возврата источника. */
    DerivedFmBuilder&operator=(DerivedFmBuilder&&)=delete;
    /** @brief Первый sticky отказ требует внешнего fault/teardown; retry частичного кадра запрещён. */
    DerivedFmFault fault()const noexcept{return fault_;}
    /** @brief Последний наблюдаемый FM seed, без чтения primary Machine. */
    FmSeed seed()const noexcept{return timeline_.seed();}
    /**
     * @brief Копирует Header/Dmc, восстанавливает DAC и Dmc.level, заимствует только native Inputs.
     * @details Все declared extents/canonical/order/placeholder/capacity/alias проверки
     * предшествуют первой FM mutation. Destination Ready immutable до внешнего reclaim.
     */
    template<class Pump=ContinueFmBuild>
    bool begin(const core::AudioPacketView&source,const core::Vrc7WriteTape::View&tape,DerivedFmStorage&out,Pump pump={})noexcept{
        if(fault_!=DerivedFmFault::None)return false;
        if(active_||out.phase_!=DerivedFmPhase::Empty)return fail(DerivedFmFault::Busy);
        if(!validate(source,tape,out,pump))return false;
        out.phase_=DerivedFmPhase::Building;out.original_=source.header();out.header_=source.header();
        out.inputs_=source.inputs();out.header_.expansionCount=0;out.begin_=timeline_.seed();
        for(std::size_t i=0;i<out.header_.dmcCount;++i){out.dmc_[i]=source.dmc()[i];
            if((i&127U)==127U&&!pump()){out.phase_=DerivedFmPhase::Failed;return fail(DerivedFmFault::Pump);}}
        active_=&out;tape_=tape;wi_=di_=ni_=0;return true;
    }
    /** @brief Pending сохраняет Building и все loans; Ready не является публикацией C6. */
    enum class StepResult:std::uint8_t {Pending,Ready,Fault};
    /**
     * @brief Выполняет один квант до maxM2, maxNative и не более одной write/query.
     * @details Между вызовами App обязан pump C6; равные timestamps не скрывают
     * безграничный work: native tie обрезается отдельной квотой до следующего M2.
     */
    StepResult step(unsigned maxM2=512U,unsigned maxNative=128U)noexcept{
        if(fault_!=DerivedFmFault::None)return StepResult::Fault;
        if(!active_||!maxM2||!maxNative){fail(DerivedFmFault::Busy);return StepResult::Fault;}
        auto&out=*active_;const auto&h=out.original_;const auto cycle=timeline_.seed().cycle;
        auto target=std::min(h.endCycle,cycle+std::min<std::uint64_t>(maxM2,h.endCycle-cycle));
        if(wi_<tape_.count)target=std::min(target,tape_.baseCycle+tape_.events[wi_].offset);
        if(di_<h.dmcCount)target=std::min(target,h.baseCycle+out.dmc_[di_].offset);
        auto nextNative=ni_;
        while(nextNative<h.inputCount&&nextNative-ni_<maxNative&&h.baseCycle+out.inputs_[nextNative].offset<=target)++nextNative;
        if(nextNative<h.inputCount&&h.baseCycle+out.inputs_[nextNative].offset<=target)
            target=h.baseCycle+out.inputs_[nextNative-1U].offset;
        const bool haveWrite=wi_<tape_.count&&tape_.baseCycle+tape_.events[wi_].offset==target;
        const bool haveDmc=di_<h.dmcCount&&h.baseCycle+out.dmc_[di_].offset==target;
        const FmWrite write{target,haveWrite?tape_.events[wi_].address:std::uint16_t(0),haveWrite?tape_.events[wi_].value:std::uint8_t(0)};
        const auto query=target;
        const FmSlice slice{h.baseCycle,target,haveWrite?&write:nullptr,haveWrite?1U:0U,
            nextNative>ni_?out.inputs_+ni_:nullptr,nextNative-ni_,static_cast<std::uint16_t>(ni_),haveDmc?&query:nullptr,haveDmc?1U:0U};
        /** @brief Квант пишет только owned arrays; original Dmc offset/order не меняются. */
        struct Sink {
            DerivedFmStorage&out;std::size_t dmcIndex;
            /** @brief Append существующего Expansion в зарезервированную ёмкость. */
            bool expansion(const core::AudioPacket::Expansion&e)noexcept{
                if(out.header_.expansionCount>=out.expansionCapacity_)return false;
                out.expansion_[out.header_.expansionCount++]=e;return true;
            }
            /** @brief Патчит только level соответствующего скопированного DMC. */
            bool dmc(std::size_t local,std::int32_t level)noexcept{
                if(local||dmcIndex>=out.header_.dmcCount)return false;
                out.dmc_[dmcIndex].expansionLevel=level;return true;
            }
        }sink{out,di_};
        if(!timeline_.render(slice,sink)){out.phase_=DerivedFmPhase::Failed;fail(DerivedFmFault::Timeline);return StepResult::Fault;}
        ni_=nextNative;if(haveWrite)++wi_;if(haveDmc)++di_;
        if(target!=h.endCycle||wi_!=tape_.count||di_!=h.dmcCount||ni_!=h.inputCount)return StepResult::Pending;
        out.end_=timeline_.seed();out.phase_=DerivedFmPhase::Ready;
        generation_=h.generation;region_=h.region;nextSequence_=h.sequence+1U;active_=nullptr;return StepResult::Ready;
    }
    /** @brief Не очищает FM/storage при pump-fault: частично построенное удерживается до teardown. */
    bool poison()noexcept{if(active_)active_->phase_=DerivedFmPhase::Failed;return fail(DerivedFmFault::Timeline);}
    /** @brief Лишь Main-local занятость, не Worker join и не отсутствие внешних loans. */
    bool building()const noexcept{return active_!=nullptr;}

private:
    /** @brief Запоминает только первую ошибку builder, не освобождая source/derived loans. */
    bool fail(DerivedFmFault f)noexcept{if(fault_==DerivedFmFault::None)fault_=f;return false;}
    /** @brief Ограничивает адресное сложение без перехода за uintptr_t; доступность задаёт caller. */
    static bool range(const void*p,std::size_t bytes)noexcept{return !bytes||(p&&bytes<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(p));}
    /** @brief Сверяет неперекрытие проверенных диапазонов; пустые spans разрешены. */
    static bool apart(const void*a,std::size_t na,const void*b,std::size_t nb)noexcept{
        if(!na||!nb)return true;
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);return x+na<=y||y+nb<=x;
    }
    /** @brief Проверяет полный неизменный source/tape и destination до copy/advance. */
    template<class Pump>
    bool validate(const core::AudioPacketView&source,const core::Vrc7WriteTape::View&tape,const DerivedFmStorage&out,Pump&pump)noexcept{
        if(!source.readable())return fail(DerivedFmFault::Source);
        const auto&h=source.header();
        if(!h.generation||h.sequence==~std::uint64_t{0}||h.ppuFrame!=h.sequence+1U||h.fault!=core::AudioFault::None||
           (h.region!=core::Region::Ntsc&&h.region!=core::Region::Pal)||h.capabilities!=core::AudioPacket::externalDacQ16||
           h.formatVersion!=core::AudioPacket::expansionVersion||h.expansionCount||h.endCycle<=h.baseCycle||h.endCycle-h.baseCycle>=45000U)return fail(DerivedFmFault::Source);
        if(tape.state!=core::Vrc7WriteTape::State::Sealed||tape.fault!=core::Vrc7WriteTape::Fault::None||
           tape.endCycle!=h.endCycle||tape.endCycle<tape.baseCycle||tape.count>45000U||(tape.count&&!tape.events))return fail(DerivedFmFault::Tape);
        if(!generation_){if(h.sequence||h.baseCycle||tape.baseCycle!=7U||timeline_.seed().cycle)return fail(DerivedFmFault::Sequence);}
        else if(h.generation!=generation_||h.region!=region_||h.sequence!=nextSequence_||h.baseCycle!=timeline_.seed().cycle||tape.baseCycle!=h.baseCycle)return fail(DerivedFmFault::Sequence);
        const auto nativeBytes=std::size_t(h.inputCount)*sizeof(core::AudioPacket::Input);
        const auto dmcBytes=std::size_t(h.dmcCount)*sizeof(core::AudioPacket::Dmc);
        const auto maximumExpansion=static_cast<std::size_t>((h.endCycle-h.baseCycle+63U)/64U);
        if(out.dmcCapacity_<h.dmcCount||out.expansionCapacity_<maximumExpansion||
           out.dmcCapacity_>core::AudioPacket::dmcCapacity||out.expansionCapacity_>core::AudioPacket::expansionCapacity)return fail(DerivedFmFault::Capacity);
        const auto outDmcBytes=out.dmcCapacity_*sizeof(core::AudioPacket::Dmc),outExpansionBytes=out.expansionCapacity_*sizeof(core::AudioPacket::Expansion);
        const auto tapeBytes=tape.count*sizeof(core::Vrc7WriteTape::Event);
        if(!range(&h,sizeof(h))||!range(source.inputs(),nativeBytes)||!range(source.dmc(),dmcBytes)||!range(tape.events,tapeBytes)||
           !range(out.dmc_,outDmcBytes)||!range(out.expansion_,outExpansionBytes))return fail(DerivedFmFault::Alias);
        const void*srcs[]={&h,source.inputs(),source.dmc(),tape.events,&source,&tape,this};
        const std::size_t sizes[]={sizeof(h),nativeBytes,dmcBytes,tapeBytes,sizeof(source),sizeof(tape),sizeof(*this)};
        const void*dsts[]={&out,out.dmc_,out.expansion_};
        const std::size_t dsizes[]={sizeof(out),outDmcBytes,outExpansionBytes};
        for(unsigned i=0;i<3;++i){for(unsigned j=0;j<7;++j)if(!apart(dsts[i],dsizes[i],srcs[j],sizes[j]))return fail(DerivedFmFault::Alias);
            for(unsigned j=0;j<i;++j)if(!apart(dsts[i],dsizes[i],dsts[j],dsizes[j]))return fail(DerivedFmFault::Alias);}
        for(std::size_t i=0;i<tape.count;++i){const auto&e=tape.events[i];
            if(e.reserved||e.offset>tape.endCycle-tape.baseCycle||(i&&e.offset<tape.events[i-1U].offset)||
               (e.address!=0x9010U&&e.address!=0x9030U&&e.address!=0xe000U))return fail(DerivedFmFault::Tape);
            if((i&127U)==127U&&!pump())return fail(DerivedFmFault::Pump);}
        for(std::size_t i=0;i<h.inputCount;++i){const auto&e=source.inputs()[i];
            if(e.offset>h.endCycle-h.baseCycle||(i&&e.offset<source.inputs()[i-1U].offset)||
               (e.operation>23U&&e.operation!=core::AudioPacket::readStatusOperation&&e.operation!=core::AudioPacket::clearSamplesOperation))return fail(DerivedFmFault::Source);
            if((i&127U)==127U&&!pump())return fail(DerivedFmFault::Pump);}
        for(std::size_t i=0;i<h.dmcCount;++i){const auto&e=source.dmc()[i];
            if(e.offset>h.endCycle-h.baseCycle||(i&&e.offset<source.dmc()[i-1U].offset)||!e.stall||e.reserved||e.expansionLevel)return fail(DerivedFmFault::Source);
            if((i&127U)==127U&&!pump())return fail(DerivedFmFault::Pump);}
        return true;
    }
    FmTimeline timeline_{};
    DerivedFmStorage*active_=nullptr;
    core::Vrc7WriteTape::View tape_{};
    std::size_t wi_=0,di_=0,ni_=0;
    std::uint64_t generation_=0,nextSequence_=0;
    core::Region region_=core::Region::Ntsc;
    DerivedFmFault fault_=DerivedFmFault::None;
};
} // namespace nes::app::frame_fm
