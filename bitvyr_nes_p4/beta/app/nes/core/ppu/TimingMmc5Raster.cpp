/**
 * @file TimingMmc5Raster.cpp
 * @brief Подготовленный MMC5 raster kernel, изолированный от generic PPU codegen.
 * @details Cache metadata принадлежит единственному TimingPpu. Никаких SDK,
 * задач, копий ROM или выделений в цикле; exact scalar tick остаётся эталоном.
 */
#include "TimingPpu.h"
#include "../Machine.h"
#if defined(NES_CORE_PPU_COLOUR_REPLAY_DIAG) && NES_CORE_PPU_COLOUR_REPLAY_DIAG
#include "PpuColourReplay.h"
#endif
#include <algorithm>
#include <type_traits>

#if defined(NES_CORE_MMC5_RASTER_PLAN) && NES_CORE_MMC5_RASTER_PLAN
namespace nes::core {

/** @details Нулевой tag проверяется до чтения ещё не подготовленных полей плана.
 * Epoch0 запрещает cache hit после wrap. Полная ROM/model граница дополнительно
 * проходит через reset/bind/invalidate и существующий owned-frame lease barrier.
 * Проверки backing не заменяют эти границы, но закрывают изменение источника
 * при ошибочном повторном использовании numerically equal mapper version. */
Mmc5RasterPlan* TimingPpu::acquireMmc5RasterPlan(Mmc5& mapper) noexcept {
    const auto version=mapper.rasterPlanVersion();
    if(!version){invalidateMmc5RasterPlan();return nullptr;}
    const auto* const ciram=bus_.nametables_.data();
    if(rasterPlanVersion_==version&&rasterPlan_.owner_==&mapper&&
       rasterPlan_.chr_==mapper.chr_&&std::size_t(rasterPlan_.chrMask_)+1U==mapper.chrBytes_&&
       rasterPlanCiram_==ciram&&rasterPlanControl_==control_)return &rasterPlan_;
    invalidateMmc5RasterPlan();
    if(!mapper.prepareRasterPlan(rasterPlan_,ciram,control_))return nullptr;
    rasterPlanCiram_=ciram;rasterPlanControl_=control_;rasterPlanVersion_=version;
    return &rasterPlan_;
}

/** @details Адресные drive и read идут в прежних фазах. Новый reader уже знает
 * тип доступа; выбор Machine/Cartridge/Fetch enum и повторный NT/CHR mode decode
 * не исполняются. Видимый tileX постоянен внутри тайла, но Name получает именно
 * текущую экранную колонку до возможного coarseX перехода на последнем high. */
template<unsigned Policy,unsigned Phase>
/** @details Исполняет фазу уже квалифицированного MMC5 raster plan с настоящим read/latch эффектом. */
inline void TimingPpu::fetchMmc5PlanPhase(Mmc5RasterPlan& plan) noexcept {
    static_assert(Policy<4U&&Phase<8U,"prepared MMC5 visible phase");
    constexpr auto policy=static_cast<Mmc5RasterPlan::Policy>(Policy);
    /** @brief Сохраняет прежний14битный address drive без дополнительного helper-вызова. */
    const auto drive=[this](std::uint16_t value)noexcept{
        busAddress_=value&0x3fffU;
        if(trackBusAddresses_)bus_.ppuAddress(busAddress_);
    };
    if constexpr(Phase==0)drive(static_cast<std::uint16_t>(0x2000U|(v_&0x0fffU)));
    else if constexpr(Phase==1)nextTile_=plan.readName<policy>(busAddress_,((dot_-1U)>>3U)+2U);
    else if constexpr(Phase==2)drive(static_cast<std::uint16_t>(0x23c0U|(v_&0x0c00U)|((v_>>4U)&0x38U)|((v_>>2U)&7U)));
    else if constexpr(Phase==3){
        const unsigned shift=((v_>>4U)&4U)|(v_&2U);
        const auto value=plan.readAttribute<policy>(busAddress_);
        nextAttribute_=static_cast<std::uint8_t>((value>>shift)&3U);
        event(VideoOp::BgAttribute,shift,0,0,VideoByteRef::literal|value);
        drive(static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+unsigned(nextTile_)*16U+((v_>>12U)&7U)));
    }else if constexpr(Phase==4)
        drive(static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+unsigned(nextTile_)*16U+((v_>>12U)&7U)));
    else if constexpr(Phase==5){
        const auto value=plan.readPattern<policy>(busAddress_);
#if defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
        backgroundAtoms_[LowNext]=VideoByteRef::literal|value;
#else
        nextLow_=value;
#endif
        event(VideoOp::BgLow,0,0,0,VideoByteRef::literal|value);
    }else if constexpr(Phase==6){coarseXSignal_=true;drive(static_cast<std::uint16_t>(busAddress_+8U));}
    else if constexpr(Phase==7){
        const auto value=plan.readPattern<policy>(busAddress_);
#if defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
        backgroundAtoms_[HighNext]=VideoByteRef::literal|value;
#else
        nextHigh_=value;
#endif
        event(VideoOp::BgHigh,0,0,0,VideoByteRef::literal|value);
        if(coarseXSignal_){
            if((v_&0x001fU)==31U)v_=static_cast<std::uint16_t>((v_&0x7fe0U)^0x0400U);
            else ++v_;
        }
        coarseXSignal_=false;
    }
}

/** @details Clocked и Aggregate известны при инстанцировании: в безопасном
 * префиксе остаются только реальные phase-effects и dot; насыщаемые долги
 * складываются один раз в конце. Обычный вариант сохраняет renderPixel,
 * loadBackground и особые255/256 в прежнем порядке, включая Sprite0/evaluation. */
template<unsigned Policy,bool Clocked,bool Aggregate>
/** @details Составляет разрешённую видимую группу по тому же плану, не увеличивая CPU-grant. */
void TimingPpu::runMmc5PlanVisibleGroup(unsigned dots,Mmc5RasterPlan& plan) noexcept {
    const unsigned total=dots;
    /** @brief Исполняет одну исходную фазу; caller не пересекает строку или MMIO. */
    const auto next=[&](auto tag)noexcept{
        constexpr unsigned phase=decltype(tag)::value;
        if constexpr(Clocked)bus_.ppuCycle();
        if constexpr(!Aggregate){
            if constexpr(phase==0)loadBackground();
            renderPixel();
            if(pendingBackgroundShifts_<16U)++pendingBackgroundShifts_;
        }
        fetchMmc5PlanPhase<Policy,phase>(plan);
        if constexpr(phase==6)if(dot_==255U)scrollSignal_=true;
        if constexpr(!Aggregate&&phase==7)if(dot_==256U&&scrollSignal_)incrementY();
        ++dot_;
    };
    do{
        switch((dot_-1U)&7U){
        case 0:next(std::integral_constant<unsigned,0>{});if(!--dots)goto complete;[[fallthrough]];
        case 1:next(std::integral_constant<unsigned,1>{});if(!--dots)goto complete;[[fallthrough]];
        case 2:next(std::integral_constant<unsigned,2>{});if(!--dots)goto complete;[[fallthrough]];
        case 3:next(std::integral_constant<unsigned,3>{});if(!--dots)goto complete;[[fallthrough]];
        case 4:next(std::integral_constant<unsigned,4>{});if(!--dots)goto complete;[[fallthrough]];
        case 5:next(std::integral_constant<unsigned,5>{});if(!--dots)goto complete;[[fallthrough]];
        case 6:next(std::integral_constant<unsigned,6>{});if(!--dots)goto complete;[[fallthrough]];
        case 7:next(std::integral_constant<unsigned,7>{});if(!--dots)goto complete;
        }
    }while(dots);
complete:
    if constexpr(Aggregate){
        spriteCacheClocks_=static_cast<std::uint16_t>(spriteCacheClocks_+total);
        pendingPixels_+=total;
        pendingBackgroundShifts_=static_cast<std::uint8_t>(std::min(16U,unsigned(pendingBackgroundShifts_)+total));
    }
}

/** @details Домен Aggregate полностью повторяет прежний visible-M2 bookkeeping;
 * этот оператор не расширяет horizon. Dispatch policy/clock происходит один раз
 * на группу, typed read внутри отдельной фазы не делает нового enum dispatch. */
void TimingPpu::runMmc5PlanVisibleM2(unsigned dots,Mmc5RasterPlan& plan) noexcept {
#if defined(NES_CORE_MMC5_RASTER_INTERVAL) && NES_CORE_MMC5_RASTER_INTERVAL
    if(tryMmc5RasterInterval(dots,plan))return;
#endif
#if defined(NES_CORE_MMC5_VISIBLE_M2_BOOKKEEPING) && NES_CORE_MMC5_VISIBLE_M2_BOOKKEEPING
    const unsigned phase=(dot_-1U)&7U;
    const bool aggregate=phase&&phase+dots<=8U&&dot_+dots<=width&&bus_.outputJob_&&
        !bus_.outputInitializing_&&packet_->state()==VideoPacket::State::Recording&&
        packet_->header().fault==VideoFault::None&&spriteCacheValid_&&
        ((status_&0x40U)||!spriteZeroRow_)&&pendingPixels_&&
        !pendingFill_&&!pendingPaletteIndex_&&pendingPixelX_<width&&
        pendingPixels_<=width-dots&&pendingPixelX_+pendingPixels_==dot_-1U&&
        spriteCacheClocks_<=width-dots&&pendingBackgroundShifts_<=16U;
#else
    const bool aggregate=false;
#endif
    /** @brief Выбирает одну clock/debt инстанцию для policy всего текущего M2. */
    const auto run=[&](auto policy)noexcept{
        constexpr unsigned selected=decltype(policy)::value;
        if(trackBusCycles_){
            if(aggregate)runMmc5PlanVisibleGroup<selected,true,true>(dots,plan);
            else runMmc5PlanVisibleGroup<selected,true,false>(dots,plan);
        }else{
            if(aggregate)runMmc5PlanVisibleGroup<selected,false,true>(dots,plan);
            else runMmc5PlanVisibleGroup<selected,false,false>(dots,plan);
        }
    };
    switch(plan.policy()){
    case Mmc5RasterPlan::Policy::Normal:run(std::integral_constant<unsigned,0>{});break;
    case Mmc5RasterPlan::Policy::Extended:run(std::integral_constant<unsigned,1>{});break;
    case Mmc5RasterPlan::Policy::SplitNormal:run(std::integral_constant<unsigned,2>{});break;
    case Mmc5RasterPlan::Policy::SplitExtended:run(std::integral_constant<unsigned,3>{});break;
    }
}

#if defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) && NES_RESEARCH_MMC5_INPLACE_EPOCH
/** @details Guard/packet/debt алгоритм общий с прежним endpoint; только источник
 * typed phases и целого тайла заменён prepared policy. Нет нового cutoff или
 * изменения момента NMI/IRQ, M2 выполняется внешним владельцем. */
template<unsigned Policy>
bool TimingPpu::tryMmc5PlanPhase(unsigned dots,Mmc5RasterPlan& plan) noexcept {
#define NES_MMC5_PHASE_TILE(Scroll,TileX) plan.template fetch<static_cast<Mmc5RasterPlan::Policy>(Policy)>((Scroll),(TileX),true)
#define NES_MMC5_PHASE_STEP(Phase) fetchMmc5PlanPhase<Policy,Phase>(plan)
#include "Mmc5PhaseSegment.inl"
#undef NES_MMC5_PHASE_STEP
#undef NES_MMC5_PHASE_TILE
}

/** @details Один dispatch перед всем коротким endpoint, а не перед каждым read. */
bool TimingPpu::tryMmc5PlanPhaseSegment(unsigned dots,Mmc5RasterPlan& plan) noexcept {
#if defined(NES_CORE_MMC5_RASTER_INTERVAL) && NES_CORE_MMC5_RASTER_INTERVAL
    if(tryMmc5RasterInterval(dots,plan))return true;
#endif
    switch(plan.policy()){
    case Mmc5RasterPlan::Policy::Normal:return tryMmc5PlanPhase<0>(dots,plan);
    case Mmc5RasterPlan::Policy::Extended:return tryMmc5PlanPhase<1>(dots,plan);
    case Mmc5RasterPlan::Policy::SplitNormal:return tryMmc5PlanPhase<2>(dots,plan);
    case Mmc5RasterPlan::Policy::SplitExtended:return tryMmc5PlanPhase<3>(dots,plan);
    }
    return false;
}
#endif

#if defined(NES_RESEARCH_MMC5_INPLACE_SPAN) && NES_RESEARCH_MMC5_INPLACE_SPAN
/** @details Общий include сохраняет старый packet codec, logical credits,
 * последние три live planes и raster exit. Подготовка/dispatch выполняются
 * перед циклом, сам tile reader инстанцирован с постоянной hardware policy. */
template<unsigned Policy>
void TimingPpu::runMmc5PlanBgPolicy(unsigned tiles,Mmc5& mapper,Mmc5RasterPlan& plan) noexcept {
    static_cast<void>(mapper);
#if defined(NES_RUN_MMC5_SPAN_VIEW) && NES_RUN_MMC5_SPAN_VIEW
    if(packet_->mmc5SpanReady()){
#include "Mmc5SpanRun.inl"
        return;
    }
#endif
#define NES_MMC5_BG_TILE(Scroll,TileX,Planes) plan.template fetch<static_cast<Mmc5RasterPlan::Policy>(Policy)>((Scroll),(TileX),(Planes))
#include "Mmc5BackgroundRun.inl"
#undef NES_MMC5_BG_TILE
}

/** @details Режим выбран один раз на весь разрешённый BG-run. */
void TimingPpu::runMmc5PlanBg(unsigned tiles,Mmc5& mapper,Mmc5RasterPlan& plan) noexcept {
    switch(plan.policy()){
    case Mmc5RasterPlan::Policy::Normal:runMmc5PlanBgPolicy<0>(tiles,mapper,plan);break;
    case Mmc5RasterPlan::Policy::Extended:runMmc5PlanBgPolicy<1>(tiles,mapper,plan);break;
    case Mmc5RasterPlan::Policy::SplitNormal:runMmc5PlanBgPolicy<2>(tiles,mapper,plan);break;
    case Mmc5RasterPlan::Policy::SplitExtended:runMmc5PlanBgPolicy<3>(tiles,mapper,plan);break;
    }
}
#endif
#if defined(NES_CORE_MMC5_RASTER_INTERVAL) && NES_CORE_MMC5_RASTER_INTERVAL
#include "TimingMmc5Interval.inl"
#endif
} // namespace nes::core
#endif

#if !defined(NES_CORE_MMC5_RASTER_PLAN) || !NES_CORE_MMC5_RASTER_PLAN || \
    !defined(NES_CORE_MMC5_RASTER_INTERVAL) || !NES_CORE_MMC5_RASTER_INTERVAL
namespace nes::core {
/** @details Pure OFF endpoint сохраняет одинаковый private diagnostic API всех TU;
 * не разыменовывает неподготовленный план и удаляется linker GC без потребителя. */
bool TimingPpu::tryMmc5RasterInterval(unsigned dots,Mmc5RasterPlan& plan) noexcept {
    static_cast<void>(dots);static_cast<void>(plan);return false;
}
} // namespace nes::core
#endif


namespace nes::core {

/** @details Отдельный Raster TU оставляет mapper lookup и bool fallback в Flash.
 * IRAM caller передаёт только прежние this+dots одним вызовом, без нового
 * Mmc5 аргумента и условной ветки. Старые горячие функции не перемещаются;
 * первый digital M2 остаётся прежнему replayMmc5InplaceDots. */
void TimingPpu::replayMmc5HeldSpriteGroup(unsigned dots) noexcept {
#if defined(NES_CORE_MMC5_TAIL_SLOT_SEGMENTS) && NES_CORE_MMC5_TAIL_SLOT_SEGMENTS && NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    auto* const mapper=bus_.mmc5QuietMapper();
    if(mapper&&tryMmc5HeldSpriteGroup(dots,*mapper))return;
#endif
    spriteFetchGroup<false>(dots);
}

/** @details Начало258 обычно содержит семь фаз slot0. Их нельзя отдавать
 * прежнему group целиком на63dots: он поглотил бы все будущие aligned slots.
 * Новый outer loop сохраняет старый partial opcode и пробует full8 заново
 * на каждой границе. Первое digital clockCpu(1) остаётся прежнему caller. */
bool TimingPpu::tryMmc5HeldSpriteGroup(unsigned dots,Mmc5& mapper) noexcept {
#if defined(NES_CORE_MMC5_TAIL_SLOT_SEGMENTS) && NES_CORE_MMC5_TAIL_SLOT_SEGMENTS && NES_MMC5_GENERALIZED_QUANTUM_DOMAIN && defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) && NES_RESEARCH_MMC5_INPLACE_EPOCH && defined(NES_CORE_PPU_OUTPUT_SPRITE_ROWS) && NES_CORE_PPU_OUTPUT_SPRITE_ROWS
    if(!dots||dot_<258U||dot_>320U||dots>321U-dot_||
       this!=&bus_.timingPpu_||&mapper!=bus_.mmc5QuietMapper()||
       !bus_.cartridge_.loaded()||bus_.cartridge_.mapper()!=5U||pal_||
       trackBusCycles_||trackBusAddresses_||capture_||!packet_||packet_!=bus_.videoPacket_||
       !bus_.mmc5ReadQuantum_.active||!bus_.outputOwner_||!bus_.outputJob_||
       bus_.outputInitializing_||bus_.outputSealed_||bus_.videoObserver_||bus_.videoObservableOnly_||
       !bus_.outputFrameExecutable()||!outputSpriteRows()||
       unsigned(scanline_)>=height||!renderingEnabled_||!renderingNext_||!(mask_&0x18U)||
       renderDelay_||nmiDelay_||nmiPending_||frameReady_||(status_&0x80U)||pendingPixels_)return false;
    while(dots){
        const unsigned phase=(dot_-257U)&7U;
        if(!phase&&dots>=8U&&tryMmc5HeldSpriteSlot(mapper)){dots-=8U;continue;}
        const unsigned n=std::min(dots,8U-phase);
        spriteFetchGroup<false>(n);dots-=n;
    }
    return true;
#else
    static_cast<void>(dots);static_cast<void>(mapper);return false;
#endif
}

/** @details R0/lastPattern исключают fourth-read edge у NT/NT/low/high.
 * Dummy data никем не используются и не меняют BG latch; sprite bank выбирается
 * единственным mapper алгоритмом. Полные old SpriteMeta/Low/High уже подавлены
 * existing SpriteRow policy. No pending pixels делает loadSpriteMetadata чистым
 * для packet; все потенциальные отказы выполнены прежде его первой записи. */
bool TimingPpu::tryMmc5HeldSpriteSlot(Mmc5& mapper) noexcept {
#if defined(NES_CORE_MMC5_TAIL_SLOT_SEGMENTS) && NES_CORE_MMC5_TAIL_SLOT_SEGMENTS && NES_MMC5_GENERALIZED_QUANTUM_DOMAIN && defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) && NES_RESEARCH_MMC5_INPLACE_EPOCH && defined(NES_CORE_PPU_OUTPUT_SPRITE_ROWS) && NES_CORE_PPU_OUTPUT_SPRITE_ROWS
    if(dot_<265U||dot_>313U||((dot_-257U)&7U)||pendingPixels_||
       !mapper.inFrame_||mapper.irqPending_||mapper.nameRepeat_||
       mapper.lastPpuAddress_>=0x2000U||!mapper.ppuReadSinceM2_||mapper.cpuIdle_!=0U)return false;
    const unsigned slot=(dot_-257U)/8U;
    const auto pattern=spritePattern(slot);
    std::uint8_t low=0,high=0;
    if(!mapper.readQuietSpritePlanes(pattern,low,high))return false;
    loadSpriteMetadata(slot);
    auto& sprite=sprites_[slot];
    sprite.low=(sprite.attributes&0x40U)?reverse(low):low;
    sprite.high=(sprite.attributes&0x40U)?reverse(high):high;
    busAddress_=static_cast<std::uint16_t>(pattern+8U);
    dot_=static_cast<std::uint16_t>(dot_+8U);
    mapper.finishQuietBackground(busAddress_);
    return true;
#else
    static_cast<void>(mapper);return false;
#endif
}
} // namespace nes::core
