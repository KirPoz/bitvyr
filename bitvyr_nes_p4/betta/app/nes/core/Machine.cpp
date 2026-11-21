/**
 * @file Machine.cpp
 * @brief Планировщик CPU/APU/PPU, шина памяти, DMA и кадровые границы одной машины.
 * @ingroup nes_core
 */
#include <algorithm>
#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#if (defined(NES_CORE_MMC5_WRAM_QUANTUM) && NES_CORE_MMC5_WRAM_QUANTUM) || (defined(NES_CORE_MMC5_FRAME_ADMISSION) && NES_CORE_MMC5_FRAME_ADMISSION)
#if !NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
#error "MMC5_WRAM_QUANTUM and MMC5_FRAME_ADMISSION require the MMC5 generalized quantum domain"
#endif
#endif
#if defined(NES_CORE_MMC5_RETAINED_EPOCH) && NES_CORE_MMC5_RETAINED_EPOCH
#if !NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
#error "MMC5_RETAINED_EPOCH requires the MMC5 generalized quantum domain"
#endif
#endif
#define NES_COST_CPU_STEP() cpu_.step()
#define NES_COST_QUIET_MMC5(budget) cpu_.runQuietBlockWithPolicy<true>(budget)
#define NES_COST_QUIET_LEGACY(budget) cpu_.runQuietBlock(budget)
#if defined(NES_CORE_STATUS_POLL_SEARCH) && NES_CORE_STATUS_POLL_SEARCH
#include "StatusPollSearch.h"
#endif
#define NES_STATUS_SEARCH_CHECK() ((void)0)
#define NES_STATUS_SEARCH_SELECTED(a,l,n) ((void)0)

#define NES_POLL_PROBE(reason, amount) ((void)0)
#if defined(NES_CORE_PPU_COMPLETED_DEBT_JOIN) && NES_CORE_PPU_COMPLETED_DEBT_JOIN
#if !defined(NES_CORE_DIRECT_BUS) || !NES_CORE_DIRECT_BUS || !defined(NES_CORE_SPLIT_PPU) || !NES_CORE_SPLIT_PPU || !defined(NES_CORE_PPU_OBSERVABLE_ONLY) || !NES_CORE_PPU_OBSERVABLE_ONLY || !defined(NES_CORE_PPU_DEFER8) || !NES_CORE_PPU_DEFER8 || !defined(NES_CORE_PPU_DOT_GROUP) || !NES_CORE_PPU_DOT_GROUP
#error "PPU_COMPLETED_DEBT_JOIN requires direct/split observable PPU, deferred dots and dot groups"
#endif
#endif
#if defined(NES_CORE_PPU_COLOUR_REPLAY_DIAG) && NES_CORE_PPU_COLOUR_REPLAY_DIAG
#include "ppu/PpuColourReplay.h"
#endif

namespace nes::core {

#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
/**
 * @brief Создаёт самостоятельный набор CPU/PPU/APU и шинной памяти машины.
 * @details ROM не принадлежит объекту; политика выделения legacy-пикселей может передаваться явно. Конкретная настройка split-режимов выполняется на остановленном владельце.
 */
Machine::Machine() noexcept : Machine(LegacyPixelAllocator{}) {}
Machine::Machine(LegacyPixelAllocator allocator) noexcept : cpu_(*this), ppu_(*this,allocator), apu_(*this)
#else
Machine::Machine() noexcept : cpu_(*this), ppu_(*this), apu_(*this)
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    , timingPpu_(*this)
#endif
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    , timingApu_(*this)
#endif
{}



/**
 * @brief Проверяет и подключает заимствованный образ ROM.
 * @details Владелец образа удерживает неизменённые байты на весь срок использования. Отказ загрузки нельзя считать сохранением всех старых указателей картриджа; ранее выданные ссылки и эпохи обрабатываются установленным жизненным циклом.
 * @param image Неизменяемый заимствованный образ ROM; вызывающий сохраняет его срок жизни.
 * @param size Размер переданного диапазона в байтах.
 * @param region Явно выбранный гостевой регион NTSC/PAL.
 * @return Код проверки iNES/поддержки mapper; только Ok означает принятую загрузку, отказ имеет описанные эффекты инвалидирования.
 */
LoadResult Machine::load(const std::uint8_t* image, std::size_t size, Region region) noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_)return LoadResult::InvalidHeader; // Сначала явный cancel joined-аренды.
#endif

#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
    invalidateColourCapture();
#endif
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
    flushCpuReads(); cpuReadBudget_ = 0;
#endif
    flushPpu(); // Завершить старые CHR-выборки до замены даже отвергаемого образа.
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
    // Отказ загрузки сохраняет старый конвейер PPU. Зафиксировать заимствованные байты старого ROM
    // и RAM ДО того, как Cartridge сможет инвалидировать или заменить их хранилище.
    if(splitVideoEnabled())timingPpu_.materializeBackgroundRefs();
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
    videoBgViewValid_=false;videoBgViewDirty_=true;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    // Отвергнутый load также меняет backing. Старый план не удерживает ROM
    // после завершённого выше долга и последующей замены модели/адреса.
    timingPpu_.invalidateMmc5RasterPlan();
#endif
    const auto result = cartridge_.load(image, size);
#if defined(NES_CORE_VIDEO_POLICY_CACHE) && NES_CORE_VIDEO_POLICY_CACHE
    refreshVideoPolicy(); // Отвергнутая загрузка тоже меняет допустимость по loaded.
#endif
    if (result == LoadResult::Ok) { region_ = region; reset(); }
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    /**
     * @brief Обновляет число точек PPU, которые можно отложить без наблюдаемого события.
     * @details Предикаты читают текущий материализованный курсор. После MMIO требуется повторная оценка, поскольку изменяются задержки, адреса и NMI.
     * @return Вычисленное значение или состояние типа else согласно описанному контракту.
     */
    else refreshPpuDeferBudget(); // Отказ загрузки сохраняет прежнюю материализованную машину.
#endif
    return result;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void Machine::reset() noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_&&!outputInitializing_)return; // Sealed/Reading данные ещё заимствованы.
#endif
#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
    invalidateColourCapture();
#endif
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
    pendingCpuReads_ = cpuReadBudget_ = 0;
#endif
    mmc5ReadQuantum_={};
    cycles_ = 0;
    openBus_ = ppuRemainder_ = dmaPage_ = 0;
    dmcStalls_ = 0;
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    pendingPpuDots_ = ppuDeferBudget_ = 0;
#endif
    resetCpuReadWindow(); // Закрыть прежние CPU-read credits до сброса устройств.
    dmaPending_ = strobe_ = false;
    ram_.fill(0); nametables_.fill(0);
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN && defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
    // Связано ТОЛЬКО с настоящей очисткой NT, а не с videoReset или отказом загрузки.
    videoNameMarkers_.clear();
#endif
    buttons_.fill(0); controllerShift_.fill(0);
    cartridge_.reset();
    refreshNametableMapping();
    ppu_.reset(region_ == Region::Pal);
    const auto mapper = cartridge_.mapper();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    deferPpuAllowed_ = mapper == 0 || mapper == 1 || mapper == 2 || mapper == 3 || mapper == 7 || mapper == 69 || mapper == 85 || mapper == 160;
    deferIrqPpu_ = mapper == 160;
#if defined(NES_CORE_PPU_DEFER_MMC3_BG0) && NES_CORE_PPU_DEFER_MMC3_BG0
    deferIrqPpu_ = deferIrqPpu_ || mapper == 4;
    deferPpuAllowed_ = deferPpuAllowed_ || mapper == 4;
#endif
    if (!deferPpuAllowed_) ppuDeferBudget_ = 0;
#endif
    ppu_.setBusTracking(mapper == 4, mapper == 4 || mapper == 160);
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    videoReset();
#endif
    audioReset();
    cpu_.setState(Cpu6502::State{});
    cpu_.reset();
}

/**
 * @brief Обновляет текущую битовую маску кнопок порта контроллера.
 * @details Последовательное считывание и защёлка строба остаются у шинного протокола; приложение задаёт маску на согласованной границе ввода.
 * @param port Индекс последовательного контроллерного порта.
 * @param buttons Битовая маска кнопок перед последующим протокольным считыванием.
 */
void Machine::setController(unsigned port, std::uint8_t buttons) noexcept {
    if (port < buttons_.size()) {
        // Противоположные направления физически невозможны на обычном контроллере.
        if ((buttons & (Up | Down)) == (Up | Down)) buttons &= ~(Up | Down);
        if ((buttons & (Left | Right)) == (Left | Right)) buttons &= ~(Left | Right);
        buttons_[port] = buttons;
        if (strobe_) controllerShift_[port] = buttons;
    }
}

#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
/**
 * @brief Обновляет число точек PPU, которые можно отложить без наблюдаемого события.
 * @details Предикаты читают текущий материализованный курсор. После MMIO требуется повторная оценка, поскольку изменяются задержки, адреса и NMI.
 */
void Machine::refreshPpuDeferBudget() const noexcept {
    ppuDeferBudget_ = 0;
    if (!deferPpuAllowed_) return;
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(splitVideoEnabled()){
        unsigned budget=std::min(timingPpu_.quietDots(),cartridge_.ppuIrqQuietDotsAny());
        if(timingPpu_.scanline()>=240){
            budget=std::max(budget,timingPpu_.nonVisibleQuietDots());
        }else if(timingPpu_.canDeferDots(0)){
            const unsigned visible=249U-timingPpu_.dot();
            if(!deferIrqPpu_||(timingPpu_.deferredPrefix()==0&&
                cartridge_.ppuIrqQuietFor(visible,!timingPpu_.backgroundA12Low())))
                budget=std::max(budget,visible);
#if defined(NES_CORE_MMC3_VISIBLE_PHASE_HORIZON) && NES_CORE_MMC3_VISIBLE_PHASE_HORIZON
            if(budget<visible&&cartridge_.ppuIrqQuietVisibleAny())budget=visible;
#endif
        }
        ppuDeferBudget_=static_cast<std::uint16_t>(budget);
        return;
    }
#endif
    unsigned budget = std::min(ppu_.quietDots(), cartridge_.ppuIrqQuietDotsAny());
    // Каждый прежний специализированный горизонт действителен независимо от новой
    // границы произвольной фазы. Все начинаются в одном материализованном курсоре PPU.
    if (ppu_.scanline() >= 240) {
        budget = std::max(budget, ppu_.nonVisibleQuietDots());
    } else if (ppu_.canDeferDots(0)) {
        const unsigned visible = 249U - ppu_.dot();
        if (!deferIrqPpu_ || (ppu_.deferredPrefix() == 0 &&
            cartridge_.ppuIrqQuietFor(visible, !ppu_.backgroundA12Low())))
            budget = std::max(budget, visible);
#if defined(NES_CORE_MMC3_VISIBLE_PHASE_HORIZON) && NES_CORE_MMC3_VISIBLE_PHASE_HORIZON
        if(budget<visible&&cartridge_.ppuIrqQuietVisibleAny())budget=visible;
#endif
    }
    ppuDeferBudget_ = static_cast<std::uint16_t>(budget);
}

template <bool RefreshBudget>
#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
[[gnu::always_inline]] inline void Machine::advancePpu(unsigned dots) const noexcept {
#else
/**
 * @brief Продвигает выбранный PPU на заданное число уже прошедших точек.
 * @details Выбор полного или временного владельца не меняет гостевые clocks. Перенос сигналов CPU выполняется в предусмотренном вызывающим кодом месте.
 * @param dots Число уже прошедших точек PPU.
 */
void Machine::advancePpu(unsigned dots) const noexcept {
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    videoTickDots(dots);
#else
    ppu_.tickDots(dots);
#endif
    if constexpr (RefreshBudget) refreshPpuDeferBudget();
}

/**
 * @brief Обрабатывает ненулевой долг PPU с исходными временными границами.
 * @details Наблюдаемый владелец без пакета и отдельный owned MMC3 Output могут объединить уже прошедший prefix/rest. Оба режима сохраняют внутренние tickDots-границы; остальные владельцы оставляют прежнее разбиение.
 */
void Machine::flushPpuSlow() const noexcept {
    unsigned dots = pendingPpuDots_;
    if (!dots) return;
    pendingPpuDots_ = 0;
#if defined(NES_CORE_PPU_COMPLETED_DEBT_JOIN) && NES_CORE_PPU_COMPLETED_DEBT_JOIN
    // Только основной наблюдаемый владелец не имеет промежуточного наблюдателя пакета или захвата.
    // tickDots сохраняет собственные фазовые, растровые и mapper-границы.
    if (videoObservableOnly_ && videoCapable_ && splitVideoEnabled() &&
        !videoPacket_ && !timingPpu_.packet_ && !timingPpu_.capture_ &&
        cartridge_.loaded() && videoFaultLatch_ == VideoFault::None &&
        elideVideoCaptureData(&timingPpu_)) {
        advancePpu(dots);
        return;
    }
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    const unsigned prefix = splitVideoEnabled()?timingPpu_.deferredPrefix():ppu_.deferredPrefix();
#else
    const unsigned prefix = ppu_.deferredPrefix();
#endif
    if (prefix && dots >= prefix) {
            advancePpu(prefix);
            dots -= prefix;
    }
    if (dots) advancePpu(dots);
}

#endif


/**
 * @brief Исполняет допустимый MMC5 CPU-префикс в пределах одного временного горизонта.
 * @details Обычный путь сразу материализует прошедшие M2. Retained-epoch оставляет
 * неиспользованные credits и уже исполненный долг тому же owned frame: следующий
 * strict либо scalar шаг расходует остаток без нового admission. MMIO, исчерпание
 * budget и выход FrameScope сохраняют прежние точные барьеры материализации.
 */
unsigned Machine::runMmc5QuietBlock(unsigned remaining) noexcept {
#if NES_MMC5_EXACT_QUIET_DOMAIN
    const unsigned budget=mmc5QuietCpuBudget(remaining,false);
    if(!budget)return 0;
#if defined(NES_CORE_MMC5_WRAM_QUANTUM) && NES_CORE_MMC5_WRAM_QUANTUM
    mmc5ReadQuantum_.ram6000=cartridge_.mmc5_->cpu6000Page();
#endif
    cpuReadBudget_=static_cast<std::uint8_t>(budget);
    const auto consumed=NES_COST_QUIET_MMC5(budget);
#if defined(NES_CORE_MMC5_RETAINED_EPOCH) && NES_CORE_MMC5_RETAINED_EPOCH
    // FrameScope и admission уже фиксируют единственного владельца и horizon.
    // Сумма pending+remaining не превосходит первоначальные80 M2; новый допуск
    // здесь не создаётся. Даже consumed==0 может передать прежние credits
    // обычному interpreter: он материализует долг до любого MMIO/mapper effect.
    // Полностью исчерпанный grant закрывается сразу, как в контрольном пути.
    if(cpuReadBudget_)return consumed;
#endif
    const unsigned clocks=pendingCpuReads_;
    pendingCpuReads_=cpuReadBudget_=0;
    replayMmc5QuietCpuCycles(clocks);
    return consumed;
#else
    static_cast<void>(remaining);return 0;
#endif
}

/** @details Начало frame не наследует неиспользованный grant старого владельца. */
Machine::Mmc5ReadQuantumFrameScope::Mmc5ReadQuantumFrameScope(Machine& machine,std::uint64_t deadline) noexcept
    :machine_(machine){
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(machine_.cartridge_.mapper()==5&&machine_.outputOwner_&&!machine_.mmc5ReadQuantum_.active){
        machine_.flushCpuReads();machine_.cpuReadBudget_=0;
        machine_.mmc5ReadQuantum_.deadline=deadline;
        machine_.mmc5ReadQuantum_.ram6000=nullptr;
        machine_.mmc5ReadQuantum_.active=true;owns_=true;
#if defined(NES_CORE_MMC5_FRAME_ADMISSION) && NES_CORE_MMC5_FRAME_ADMISSION
        // Только wiring текущего эксклюзивного вызова: faults, IRQ, clocks,
        // PPU horizon и отображение WRAM не переживают отдельный admission.
        machine_.mmc5ReadQuantum_.frameQualified=machine_.mmc5QuietFramePrerequisites();
#endif
    }
#else
    static_cast<void>(deadline);
#endif
}
/** @details Flush предшествует снятию active, иначе dedicated commit потеряет identity. */
Machine::Mmc5ReadQuantumFrameScope::~Mmc5ReadQuantumFrameScope() noexcept {
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(owns_){
        machine_.flushCpuReads();machine_.cpuReadBudget_=0;
        machine_.mmc5ReadQuantum_={};
    }
#endif
}

/** @details Ни одно из этих полей не меняется CPU/MMIO внутри owned stepFrame. */
bool Machine::mmc5QuietFramePrerequisites() const noexcept {
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    return cartridge_.mapper()==5&&region_==Region::Ntsc&&cartridge_.mmc5_&&cartridge_.mmc5Audio_&&
        outputOwner_&&!outputInitializing_&&outputJob_&&!outputSealed_&&
        !videoObserver_&&!videoObservableOnly_&&videoPacket_&&timingPpu_.packet_==videoPacket_&&!timingPpu_.capture_&&
        audioPacket_&&expansionAudio_&&audioPacket_->header().generation==audioGeneration_&&
        audioPacket_->header().sequence==audioSequence_&&audioPacket_->header().region==region_;
#else
    return false;
#endif
}

/** @details Pure сертификат един для B3 и обычного interpreter bus quantum. */
unsigned Machine::mmc5QuietCpuBudget(unsigned remaining,bool insideStep) noexcept {
#if NES_MMC5_EXACT_QUIET_DOMAIN
#if defined(NES_CORE_MMC5_FRAME_ADMISSION) && NES_CORE_MMC5_FRAME_ADMISSION
    // Scope — единственный владелец: ложный frame verdict не становится
    // положительным до следующего stepFrame. Динамика ниже не кэшируется.
    const bool frameQualified=mmc5ReadQuantum_.active;
    if(frameQualified&&!mmc5ReadQuantum_.frameQualified)return 0;
#endif
#if defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) && NES_RESEARCH_MMC5_INPLACE_EPOCH
#if defined(NES_CORE_MMC5_FRAME_ADMISSION) && NES_CORE_MMC5_FRAME_ADMISSION
    if((!frameQualified&&(cartridge_.mapper()!=5||region_!=Region::Ntsc))||remaining<8U)return 0;
#else
    if(cartridge_.mapper()!=5||region_!=Region::Ntsc||remaining<8U)return 0;
#endif
    const unsigned noRd=timingPpu_.mmc5InplaceNoRdDots();
    const unsigned horizon=noRd?noRd:timingPpu_.mmc5InplaceActiveDots();
    if(horizon<24U)return 0;
#else
    // Геометрия допускает phase0 либо короткий scalar-prefix из phase6/7.
    const auto dot=timingPpu_.dot_;
    const unsigned phase=(dot-1U)&7U;
    if(cartridge_.mapper()!=5||region_!=Region::Ntsc||remaining<8U||
       dot<9U||dot>225U||(phase!=0U&&phase!=6U&&phase!=7U)||static_cast<unsigned>(timingPpu_.scanline_)>=240U||
       !timingPpu_.renderingEnabled_||!timingPpu_.renderingNext_||!(timingPpu_.mask_&0x18U)||
       timingPpu_.renderDelay_||timingPpu_.nmiDelay_)return 0;
    // В phase6 ещё ожидается high=low+8; phase7 уже несёт настоящий CHR high.
    if((phase==6U&&timingPpu_.busAddress_>0x1FF7U)||
       (phase==7U&&timingPpu_.busAddress_>=0x2000U))return 0;
#endif
    if(
#if defined(NES_CORE_MMC5_FRAME_ADMISSION) && NES_CORE_MMC5_FRAME_ADMISSION
       (!frameQualified&&!mmc5QuietFramePrerequisites())||
#else
       !cartridge_.mmc5_||!cartridge_.mmc5Audio_||
#endif
#if !defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) || !NES_RESEARCH_MMC5_INPLACE_EPOCH
       !cartridge_.mmc5_->canRunQuietBackground()||
#endif
       !cartridge_.mmc5Audio_->purePrgReads()||
       dmaPending_||dmcStalls_||pendingCpuReads_||cpuReadBudget_||pendingPpuDots_||deferPpuAllowed_||
       (cpu_.inStep_!=insideStep)||cpu_.nmiPending_||cpu_.nmiReady_||cpu_.irqPending_||cpu_.irqLine_||
       timingPpu_.nmiPending_||timingPpu_.frameReady_||cartridge_.irqLine()||audioIrqLine()||
#if defined(NES_CORE_MMC5_FRAME_ADMISSION) && NES_CORE_MMC5_FRAME_ADMISSION
       !outputFrameExecutable()||videoHasFault()||audioFault()!=AudioFault::None||timingApu_.cycles()!=cycles_)return 0;
#else
       !outputOwner_||outputInitializing_||!outputJob_||outputSealed_||!outputFrameExecutable()||
       videoObserver_||videoObservableOnly_||!videoPacket_||timingPpu_.packet_!=videoPacket_||timingPpu_.capture_||
       videoHasFault()||!audioPacket_||!expansionAudio_||audioFault()!=AudioFault::None||
       timingApu_.cycles()!=cycles_||audioPacket_->header().generation!=audioGeneration_||
       audioPacket_->header().sequence!=audioSequence_||audioPacket_->header().region!=region_)return 0;
#endif
#if defined(NES_CORE_PPU_COLOUR_REPLAY_DIAG) && NES_CORE_PPU_COLOUR_REPLAY_DIAG
    if(PpuColourReplay::isActiveShadow(this))return 0;
#endif
    for(const auto* page:cartridge_.prgPages_)if(!page)return 0;
#if defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) && NES_RESEARCH_MMC5_INPLACE_EPOCH
    const unsigned budget=std::min({remaining,80U,horizon/3U,timingApu_.quietCpuCycles()});
#else
    const unsigned budget=std::min({remaining,80U,(249U-dot)/3U,
        timingPpu_.quietDots()/3U,timingApu_.quietCpuCycles()});
#endif
    if(budget<8U||ppuRemainder_>4U||cycles_>~std::uint64_t{0}-budget||
       !videoPacket_->canAppendMmc5Quiet(3U*budget)||
       !audioPacket_->canAppendMmc5Quiet(cycles_,budget))return 0;
    return budget;
#else
    static_cast<void>(remaining);static_cast<void>(insideStep);return 0;
#endif
}
/** @details Все analog DAC timestamps и scalar prefix/tail сохранены из закрытого B3. */
void Machine::replayMmc5QuietCpuCycles(unsigned clocks) noexcept {
#if NES_MMC5_EXACT_QUIET_DOMAIN
#if !defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) || !NES_RESEARCH_MMC5_INPLACE_EPOCH
    const unsigned phase=(timingPpu_.dot_-1U)&7U;
#endif
    // Первый clock сохраняет pending DAC после предшествующей CPU записи.
    // Явный timestamp остаётся T-1, пока audio/APU временно впереди PPU.
    {
    unsigned completed=0;
    auto& expansion=*cartridge_.mmc5Audio_;
    while(completed<clocks){
        const unsigned segment=completed?std::min(clocks-completed,
#if defined(NES_CORE_MMC5_AUDIO_OUTPUT_DEADLINES) && NES_CORE_MMC5_AUDIO_OUTPUT_DEADLINES
            static_cast<unsigned>(cartridge_.selectedExpansionOutputCpuCycles())):1U;
#else
            static_cast<unsigned>(expansion.nextEventCpuCycles())):1U;
#endif
        expansion.clockCpu(segment);
        completed+=segment;
        const auto level=cartridge_.selectedExpansionSample();
        if(level!=lastExpansionLevel_){
            static_cast<void>(audioPacket_->addExpansion(cycles_+completed-1U,level));
            lastExpansionLevel_=level;
        }
    }
    // Strict quiet certificate исключает IRQ/DMC callback внутри advance.
    timingApu_.advanceCpuCycles(clocks);
    }
#if defined(NES_RESEARCH_MMC5_INPLACE_EPOCH) && NES_RESEARCH_MMC5_INPLACE_EPOCH
    // Только pure CPU reads произошли после grant. MMIO/DMA/DMC всегда flush до
    // мутации; rebase не имеет права молча вернуться к старому optimistic CPU.
    if(timingPpu_.mmc5InplaceNoRdDots()>=clocks*3U){
        cartridge_.mmc5_->clockCpu(clocks);cycles_+=clocks;advancePpu<false>(3U*clocks);
    }else{timingPpu_.replayMmc5InplaceDots(3U*clocks);cycles_+=clocks;}
    if(videoTakeNmi())cpu_.requestNmi();
    cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
#else
    // Каждый digital M2 и прежние PPU dots исполняются на своём Machine time.
    // NTSC remainder после +15-15 остаётся исходным значением0..4.
    /** @brief Воспроизводит один уже потреблённый M2 без повторного audio clock.
     * @details Заимствует только текущую Machine до возврата; сохраняет прежние
     * digital M2, три PPU dots и transfer NMI/IRQ. Prefix и tail имеют один код. */
    const auto replayScalarM2=[this]() noexcept {
        ++cycles_;
        cartridge_.clockMmc5DigitalCpu();
        advancePpu<false>(3U);
        if(videoTakeNmi())cpu_.requestNmi();
        cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
    };
    // k = -3*phase (mod8) измеряется в M2; каждый M2 продвигает ровно три dots.
    const unsigned prefixBudget=(8U-(3U*phase)%8U)%8U;
    const unsigned prefixClocks=std::min(clocks,prefixBudget);
    for(unsigned i=0;i<prefixClocks;++i)replayScalarM2();
    const unsigned grouped=timingPpu_.replayMmc5QuietBlock(clocks-prefixClocks);
    // Внутри certified PPU run время не наблюдают: audio уже учтено, CPU стоит.
    // Материализовать весь завершённый M2-prefix до IRQ/NMI transfer и scalar tail.
    cycles_+=grouped;
    // Все IRQ/NMI внутри certified FSM известны false. Полную проверку делаем
    // один раз после агрегата до прежнего scalar tail и возобновления CPU.
    if(grouped){
        if(videoTakeNmi())cpu_.requestNmi();
        cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
    }
    for(unsigned i=prefixClocks+grouped;i<clocks;++i)replayScalarM2();
#endif
#else
    static_cast<void>(clocks);
#endif
}

/**
 * @brief Продвигает один логический такт CPU и соответствующие устройства.
 * @details Для PAL сохраняет дробный остаток отношения16/5, для NTSC три точки PPU.
 */
void Machine::clockCpuCycle() noexcept {
#if defined(NES_CORE_PPU_COLOUR_REPLAY_DIAG) && NES_CORE_PPU_COLOUR_REPLAY_DIAG
    if(PpuColourReplay::clockTrapped(this))return;
#endif
#if defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
    colourtrace::forbidExecution(this);
#endif
    ++cycles_;
    {
    cartridge_.cpuCycle();
    if(Cartridge::hasExpansionAudioMapper(cartridge_.mapper())){
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET && defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
        audioPublishExpansion(cartridge_.selectedExpansionSample());
#else
        apu_.setExpansionInput(cartridge_.selectedExpansionSample());
#endif
    }
    }
    audioTick();
    {
    ppuRemainder_ = static_cast<std::uint8_t>(ppuRemainder_ + (region_ == Region::Pal ? 16 : 15));
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    const unsigned ppuDots = ppuRemainder_ >= 20 ? 4U : 3U;
    ppuRemainder_ = static_cast<std::uint8_t>(ppuRemainder_ - ppuDots * 5);
    // Отключённая политика маппера, выбранная при сбросе, не допускает долга.
    if (!deferPpuAllowed_) {
        // После сброса неизменяемая неподдерживаемая политика означает нулевой бюджет.
        // Эта специализация сохраняет исходный потактовый путь PPU.
        advancePpu<false>(ppuDots);
    } else {
        unsigned total = pendingPpuDots_ + ppuDots;
        if (total <= ppuDeferBudget_) {
            // Внутри этого проверенного интервала нет CPU-наблюдаемого события PPU.
            // Сохранять все прошедшие точки до наблюдателя, MMIO или границы строки.
            pendingPpuDots_ = static_cast<std::uint16_t>(total);
        } else {
#if defined(NES_CORE_PPU_COMPLETED_DEBT_JOIN) && NES_CORE_PPU_COMPLETED_DEBT_JOIN
            // Оба интервала уже прошли. Передача IRQ/NMI остаётся
            // после полного продвижения PPU на исходном срезе CPU-такта.
            if (pendingPpuDots_ && videoObservableOnly_ && videoCapable_ && splitVideoEnabled() &&
                        !videoPacket_ && !timingPpu_.packet_ && !timingPpu_.capture_ &&
                        cartridge_.loaded() && videoFaultLatch_ == VideoFault::None &&
        elideVideoCaptureData(&timingPpu_)) {
                pendingPpuDots_ = 0;
                advancePpu(total);
            } else {
#endif
            flushPpu();
            advancePpu(ppuDots);
#if defined(NES_CORE_PPU_COMPLETED_DEBT_JOIN) && NES_CORE_PPU_COMPLETED_DEBT_JOIN
            }
#endif
        }
    }
#else
#if defined(NES_CORE_PPU_DOT_GROUP) && NES_CORE_PPU_DOT_GROUP
    const unsigned ppuDots = ppuRemainder_ >= 20 ? 4U : 3U;
    ppuRemainder_ = static_cast<std::uint8_t>(ppuRemainder_ - ppuDots * 5);
#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(splitVideoEnabled())timingPpu_.tickDots(ppuDots);else
#endif
    ppu_.tickDotsInline(ppuDots);
#else
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(splitVideoEnabled())timingPpu_.tickDots(ppuDots);else
#endif
    ppu_.tickDots(ppuDots);
#endif
#else
    while (ppuRemainder_ >= 5) {
        ppuRemainder_ -= 5;
#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
        if(splitVideoEnabled())timingPpu_.tick();else
#endif
        ppu_.tickInline();
#else
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
        if(splitVideoEnabled())timingPpu_.tick();else
#endif
        ppu_.tick();
#endif
    }
#endif
#endif
    // Фронт защёлкнут PPU. Между этими точками нет CPU MMIO,
    // поэтому передать его один раз после группы в том же шинном такте CPU.
    if (videoTakeNmi()) cpu_.requestNmi();
    }
    cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
}

/**
 * @brief Доводит ожидающие задержки DMC до наблюдаемой CPU-границы.
 * @details Не допускает переноса остановки чтения CPU на иной шинный цикл.
 */
void Machine::drainDmcStalls() noexcept {
    while (dmcStalls_) { --dmcStalls_; clockCpuCycle(); }
}

#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::memoryRead(std::uint16_t address) noexcept {
    if (address < 0x2000) return ram_[address & 0x7ff];
    if (address >= 0x8000) {
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && \
    0 && 0
        // NMI-vector /RD сам сбрасывает MMC5 frame. Старые PPU-fetch должны
        // завершиться до этого строба, даже если обычного долга точек уже нет.
#endif
        return cartridge_.cpuRead(address, openBus_);
    }
    return memoryReadIo(address);
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
__declspec(noinline)
#endif
/** @details Разводит настоящий MMIO по регистрам PPU/APU/pad; обращения не подменяются ordinary-memory peek. */
std::uint8_t Machine::memoryReadIo(std::uint16_t address) noexcept {
#if defined(NES_CORE_PPU_STATUS_READ_FENCE) && NES_CORE_PPU_STATUS_READ_FENCE && defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if (
        address < 0x4000U && (address & 7U) == 2U && splitVideoEnabled() &&
        cartridge_.loaded() &&
#if defined(NES_CORE_PPU_STATUS_NOIRQ_MAPPERS) && NES_CORE_PPU_STATUS_NOIRQ_MAPPERS
        (cartridge_.mapper()==0 || cartridge_.mapper()==1 || cartridge_.mapper()==2 ||
         cartridge_.mapper()==3 || cartridge_.mapper()==7 ||
         (cartridge_.mapper()==4 && (!cartridge_.irqEnabled_ || cartridge_.irqPending_)))
#else
        cartridge_.mapper()==2
#endif
        ) {
        std::uint8_t cached;
        if (timingPpu_.tryReadDeferredStatus(pendingPpuDots_, cached)) {
#if defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
            colourtrace::emit(this,colourtrace::Kind::RetainedRead,cycles_,address&7U,cached);
#endif
#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
        recordColourEvent(ColourKind::RetainedRead,cycles_,address&7U,cached);
#endif
            return cached;
        }
    }
#endif
    flushPpu();
    std::uint8_t value;
    if (address < 0x4000) value = videoRead(address & 7);
    else if (address == 0x4015) value = static_cast<std::uint8_t>((openBus_ & 0x20) | audioReadStatus());
    else if (address == 0x4016 || address == 0x4017) {
        const unsigned port = address & 1;
        const std::uint8_t bit = (strobe_ ? buttons_[port] : controllerShift_[port]) & 1;
        if (!strobe_) controllerShift_[port] = static_cast<std::uint8_t>((controllerShift_[port] >> 1) | 0x80);
        value = static_cast<std::uint8_t>((openBus_ & 0xe0) | bit);
    } else if (address >= 0x4020) value = cartridge_.cpuRead(address, openBus_);
    else value = openBus_;
    refreshPpuDeferBudget(); // Чтения статуса/данных могут менять отложенное состояние PPU.
    return value;
}

#else
/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::memoryRead(std::uint16_t address) noexcept {
    if (address < 0x2000) return ram_[address & 0x7ff];
    if (address < 0x4000) return videoRead(address & 7);
    if (address == 0x4015) return static_cast<std::uint8_t>((openBus_ & 0x20) | audioReadStatus());
    if (address == 0x4016 || address == 0x4017) {
        const unsigned port = address & 1;
        const std::uint8_t bit = (strobe_ ? buttons_[port] : controllerShift_[port]) & 1;
        if (!strobe_) controllerShift_[port] = static_cast<std::uint8_t>((controllerShift_[port] >> 1) | 0x80);
        return static_cast<std::uint8_t>((openBus_ & 0xe0) | bit);
    }
    if (address >= 0x4020) return cartridge_.cpuRead(address, openBus_);
    return openBus_;
}

#endif

#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
#if defined(NES_CORE_PRG_RAM_QUIET_WINDOW) && NES_CORE_PRG_RAM_QUIET_WINDOW
/** @details У этих плат $6000..$7FFF — RAM, открытая шина либо const PRG Legacy.
 * Доступ не тактирует устройство и не переключает mapping. Legacy CPU/write IRQ
 * остаются отдельными настоящими стробами; существующий budget уже ограничивает
 * IRQ69/85 и selected64 expansion DAC остаются границами прежнего budget;
 * clocked IRQ, native APU/DMC и PPU-наблюдения. MMC5 PCM имеет другой контракт.
 */
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/** @details Сохраняет только неизменяемую PRG-RAM проводку уже выданного quiet-grant; MMIO и смена банка закрывают допуск. */
bool Machine::canRetainPrgRamWindow() const noexcept {
    if(!cpu_.inStep_||!cartridge_.loaded())return false;
    switch(cartridge_.mapper()){
    case 0:case 1:case 2:case 3:case 4:case 7:case 69:case 85:case 160:break;
    default:return false;
    }
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoObserver_)return false;
#endif
#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
    if(colourCapture_)return false;
#endif
#if defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
    if((colourtrace::producer==this&&colourtrace::hook)||colourtrace::shadow==this)return false;
#endif
    return true;
}
#endif
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/**
 * @brief Пересчитывает безопасный горизонт отложенных CPU-чтений.
 * @details Граница определяется ближайшим наблюдаемым событием PPU/APU/картриджа; значение не даёт права пропускать сами гостевые такты.
 */
void Machine::refreshCpuReadBudget() noexcept {
    cpuReadBudget_ = 0;
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(cartridge_.mapper()==5){
        if(!mmc5ReadQuantum_.active||!cpu_.inStep_||cycles_>=mmc5ReadQuantum_.deadline)return;
        const auto room=mmc5ReadQuantum_.deadline-cycles_;
        if(room<=1U)return;
        const unsigned remaining=static_cast<unsigned>(std::min<std::uint64_t>(room-1U,255U));
        cpuReadBudget_=static_cast<std::uint8_t>(mmc5QuietCpuBudget(remaining,true));
        if(cpuReadBudget_)mmc5ReadQuantum_.ram6000=cartridge_.mmc5_->cpu6000Page();
        return;
    }
#endif
    if (!deferPpuAllowed_ || pendingPpuDots_ > ppuDeferBudget_) return;
    const unsigned dots = ppuDeferBudget_ - pendingPpuDots_;
    const unsigned rate = region_ == Region::Pal ? 16U : 15U;
    unsigned cycles = (5U * (dots + 1U) - 1U - ppuRemainder_) / rate;
    cycles = std::min(cycles, audioQuietCpuCycles());
    cycles = std::min(cycles, cartridge_.cpuIrqQuietCycles());
    cpuReadBudget_ = static_cast<std::uint8_t>(std::min(cycles, 255U));
}
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/**
 * @brief Передаёт накопленные такты чтения CPU в общий синхронизатор машины.
 * @details Сохраняет исходный порядок продвижения устройств и переноса IRQ/NMI; не создаёт будущего бюджета.
 */
void Machine::commitCpuReads() noexcept {
#if defined(NES_CORE_PPU_COLOUR_REPLAY_DIAG) && NES_CORE_PPU_COLOUR_REPLAY_DIAG
    if(PpuColourReplay::clockTrapped(this))return;
#endif
#if defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
    colourtrace::forbidExecution(this);
#endif
    const unsigned count = pendingCpuReads_;
    pendingCpuReads_ = cpuReadBudget_ = 0;
    if (!count) return;
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(mmc5ReadQuantum_.active&&cartridge_.mapper()==5){
        replayMmc5QuietCpuCycles(count);
        return;
    }
#endif
    cycles_ += count;
    cartridge_.cpuCyclesQuiet(count);
    audioAdvance(count);
    const unsigned phase = ppuRemainder_ + count * (region_ == Region::Pal ? 16U : 15U);
    const unsigned dots = phase / 5U;
    ppuRemainder_ = static_cast<std::uint8_t>(phase - dots * 5U);
    pendingPpuDots_ = static_cast<std::uint16_t>(pendingPpuDots_ + dots);
    // Внутренние аудиособытия исполняются хронологически; внутри проверенного горизонта нет
    // APU IRQ/DMC-callback или границы PPU IRQ/NMI/кадра.
    // CPU IRQ и состояние произведения Legacy продвигаются без пересечения IRQ-фронта.
}
#endif

#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
/** @details Выполняет исходное декодирование CPU-шины вне короткого прямого RAM/PRG пути. */
std::uint8_t Machine::readSlow(std::uint16_t address) noexcept {
#if defined(NES_CORE_PRG_RAM_QUIET_WINDOW) && NES_CORE_PRG_RAM_QUIET_WINDOW
    const bool prgRamWindow=address>=0x6000U&&address<0x8000U&&canRetainPrgRamWindow();
    if(prgRamWindow&&cpuReadBudget_&&!dmaPending_&&!dmcStalls_){
        // Это тот же настоящий read-M2. CPU-visible RAM/open bus выбираются на
        // каждом доступе; между ними разрешены обычные записи и RMW old/new.
        ++pendingCpuReads_;--cpuReadBudget_;
        const auto value=cartridge_.cpuRead(address,openBus_);
        openBus_=value;
        return value;
    }
#endif
    const bool ordinary = address < 0x2000 || address >= 0x8000
#if defined(NES_CORE_PRG_RAM_QUIET_WINDOW) && NES_CORE_PRG_RAM_QUIET_WINDOW
        ||prgRamWindow
#endif
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
        ||(mmc5ReadQuantum_.active&&address>=0x6000U&&address<0x8000U)
#endif
        ;
    flushCpuReads(); cpuReadBudget_ = 0;
#else
/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::read(std::uint16_t address) noexcept {
#endif
    // RDY останавливает первое следующее чтение CPU, включая выборку опкода. Оставить
    // CPU внутри уже начатой инструкции на время DMA: пришедший в DMA IRQ
    // не должен задним числом заменить начатую инструкцию.
    if (dmaPending_) dma();
    drainDmcStalls(); // DMA останавливает CPU на чтениях, но не в такте записи.
    clockCpuCycle();
    const auto value = memoryRead(address);
    if(cartridge_.mapper()==5 && address<0x4020)cartridge_.observeMmc5Read(address,value);
    if (address != 0x4015) openBus_ = value;
    // Чтения RAM и PRG не имеют эффектов устройства: clockCpuCycle уже
    // предоставил эти точные линии. I/O может подтвердить IRQ или погасить долг PPU.
    if ((address >= 0x2000 && address < 0x8000) || cartridge_.mapper()==5)
        cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
    if (ordinary && !dmcStalls_ && !dmaPending_) refreshCpuReadBudget();
#endif
    return value;
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::write(std::uint16_t address, std::uint8_t value) noexcept {
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
#if defined(NES_CORE_PRG_RAM_QUIET_WINDOW) && NES_CORE_PRG_RAM_QUIET_WINDOW
    if(address>=0x6000U&&address<0x8000U&&cpuReadBudget_&&!dmaPending_&&!dmcStalls_&&
       canRetainPrgRamWindow()){
        ++pendingCpuReads_;--cpuReadBudget_;
        // Порядок строба, open bus и реальной записи совпадает со scalar.
        // Protection/Legacy PRG проверяет Cartridge сейчас, а не прежний grant.
        cartridge_.cpuWriteStrobe();openBus_=value;
        cartridge_.cpuWrite(address,value,cycles_+pendingCpuReads_);
        cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
        return;
    }
#endif
#if defined(NES_CORE_MMC5_WRAM_QUANTUM) && NES_CORE_MMC5_WRAM_QUANTUM
    if(address>=0x6000U&&address<0x8000U&&mmc5ReadQuantum_.active&&
       cpuReadBudget_&&!dmaPending_&&!dmcStalls_){
        // В отличие от MMIO, WRAM не видна PPU/APU. Защита и физическая
        // страница определяются самим MMC5 на КАЖДОЙ настоящей записи;
        // grant не переносит старое writable-разрешение через $5102/$5103/$5113.
        // RMW проходит здесь дважды с исходными old/new байтами и двумя M2.
        ++pendingCpuReads_;--cpuReadBudget_;
        cartridge_.cpuWriteStrobe();openBus_=value;
        const auto writeCycle=cycles_+pendingCpuReads_;
        cartridge_.cpuWrite(address,value,writeCycle);
#if defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
        colourtrace::emit(this,colourtrace::Kind::CartWrite,writeCycle,address,value);
#endif
#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
        recordColourEvent(ColourKind::CartWrite,writeCycle,address,value);
#endif
        cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
        return;
    }
#endif
    if (address < 0x2000 && cpuReadBudget_ && !dmaPending_ && !dmcStalls_) {
        ++pendingCpuReads_; --cpuReadBudget_;
        // RAM видна на исходном шинном такте. IRQ Legacy, тактируемые записями,
        // независимы от отложенных CPU-тактов и по-прежнему исполняются на каждой записи.
        cartridge_.cpuWriteStrobe();
        openBus_ = value; ram_[address & 0x7ff] = value;
        cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
        return;
    }
#endif
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
    flushCpuReads(); cpuReadBudget_ = 0;
#endif
    clockCpuCycle();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    if (address >= 0x2000) flushPpu();
#endif
    cartridge_.cpuWriteStrobe();
    if(cartridge_.mapper()==5 && address<0x4020)cartridge_.observeMmc5Write(address,value);
    openBus_ = value;
    if (address < 0x2000) ram_[address & 0x7ff] = value;
    else if (address < 0x4000) videoWrite(address & 7, value);
    else if (address == 0x4014) { dmaPage_ = value; dmaPending_ = true; }
    else if (address == 0x4016) {
        const bool next = (value & 1) != 0;
        if (next || strobe_) controllerShift_ = buttons_;
        strobe_ = next;
    } else if (address <= 0x4017) audioWrite(address, value);
    else if (address >= 0x4020) {
#if defined(NES_RUN_MMC5_SPAN_VIEW) && NES_RUN_MMC5_SPAN_VIEW
        const unsigned spanTarget=cartridge_.mapper()==5U&&cartridge_.mmc5_?cartridge_.mmc5_->cpuExRamWriteTarget(address):~0U;
#endif
        cartridge_.cpuWrite(address, value, cycles_);
#if defined(NES_RUN_MMC5_SPAN_VIEW) && NES_RUN_MMC5_SPAN_VIEW
        if(spanTarget!=~0U&&videoPacket_)static_cast<void>(videoPacket_->addMmc5PhysicalWrite(2048U+spanTarget,value));
#endif
#if defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
        colourtrace::emit(this,colourtrace::Kind::CartWrite,cycles_,address,value);
#endif
#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
        recordColourEvent(ColourKind::CartWrite,cycles_,address,value);
#endif
        // Намеренно обновлять после каждой записи картриджа: mapper160 также
        // декодирует регистры ниже $8000. Cartridge сохраняет фиксированную четырёхэкранную
        // разводку, поэтому здесь учитываются все изменения маппера без их дублирования.
        refreshNametableMapping();
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
        videoBgViewDirty_=true;
#endif
    }
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    if (address >= 0x2000) refreshPpuDeferBudget();
#endif
    cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
}

/**
 * @brief Выполняет OAM DMA с исходными чередующимися чтениями и записями.
 * @details Сохраняет остановку RDY, начальный halt и выравнивание по CPU-фазе; быстрый blank/RAM-путь допустим только при доказанных условиях.
 */
void Machine::dma() noexcept {
    dmaPending_ = false;
#if defined(NES_CORE_DMA_BLANK_RAM) && NES_CORE_DMA_BLANK_RAM
    // Обычно readSlow вызывается без зарезервированного CPU-времени. Отвергнуть скрытый
    // сохранённый CPU-долг, а не поглощать его в другой фазе DMA.
    // MMC5 требует исходного M2-порядка mapper/APU/PPU и публикации каждого DAC-перехода.
    // Общий bulk commit этих гарантий не даёт; сохраняем скалярный DMA без пропуска тактов.
    if(cartridge_.mapper()!=5&&splitVideoEnabled()&&videoPacket_&&audioPacket_&&cartridge_.loaded()&&dmaPage_<0x20&&
       !pendingCpuReads_&&!cpuReadBudget_&&!dmcStalls_&&!videoHasFault()&&
       videoPacket_->state()==VideoPacket::State::Recording&&
       audioPacket_->fault()==AudioFault::None&&
       !cpu_.nmiPending_&&!cpu_.nmiReady_&&!timingPpu_.nmiPending_&&timingPpu_.dot()<=340&&ppuRemainder_<5){
        const unsigned clocks=513U+unsigned(cycles_&1U);
        const unsigned dots=(ppuRemainder_+clocks*(region_==Region::Pal?16U:15U))/5U;
        const unsigned blank=timingPpu_.nonVisibleQuietDots();
        const unsigned audioQuiet=audioQuietCpuCycles();
        const auto origin=audioPacket_->header().baseCycle;
        const bool wires=cartridge_.mapper_==4
            ?cartridge_.a12_==bool(timingPpu_.busAddress_&0x1000U)
            :cartridge_.mapper_!=160||cartridge_.legacy_.dmaStableAddress(timingPpu_.busAddress_);
        bool profileQuiet=true;
        if(wires&&profileQuiet&&blank>=unsigned(pendingPpuDots_)+dots&&
           audioQuiet!=~0U&&audioQuiet>=clocks&&cartridge_.cpuIrqQuietCycles()>=clocks&&
           cycles_<=~std::uint64_t{0}-clocks&&cycles_>=origin&&cycles_-origin<=65535U-clocks){
            // Наблюдатели CPU/APU/PPU не различают порядок здесь: холостые
            // такты не читают OAM, а эта страница RAM остаётся неизменной.
            // Сохранить каждый гостевой такт существующими quiet-примитивами.
            unsigned remaining=clocks;
            while(remaining){
                const unsigned part=std::min(remaining,255U);
                pendingCpuReads_=static_cast<std::uint8_t>(part);
                commitCpuReads();remaining-=part;
            }
            flushPpu();
            const auto* source=ram_.data()+((unsigned(dmaPage_)&7U)<<8);
            for(unsigned i=0;i<256;++i)videoDmaWrite(source[i]);
            refreshPpuDeferBudget();
            cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
            return;
        }
    }
#endif
    // Один такт остановки и выравнивание чередующихся фаз чтения/записи CPU.
    const bool alignment = (cycles_ & 1) != 0;
    clockCpuCycle();
    if (alignment) clockCpuCycle();
    for (unsigned i = 0; i < 256; ++i) {
        drainDmcStalls();
        clockCpuCycle();
        const auto address = static_cast<std::uint16_t>((dmaPage_ << 8) | i);
        const auto value = memoryRead(address);
        // OAMDMA тоже делает настоящее чтение PPUSTATUS при исходной странице20.
        if(cartridge_.mapper()==5 && address<0x4020)cartridge_.observeMmc5Read(address,value);
        clockCpuCycle();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
        flushPpu();
#endif
        videoDmaWrite(value);
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
        refreshPpuDeferBudget();
#endif
    }
}

/**
 * @brief Исполняет одну инструкцию с теми же шинными побочными эффектами, что и кадровый путь.
 * @details Гостевое время машины может включать задержки DMA сверх собственных тактов инструкции.
 */
void Machine::stepInstruction() noexcept {
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(mmc5ReadQuantum_.active){flushCpuReads();cpuReadBudget_=0;}
#endif
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_&&!outputFrameExecutable())return;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return;
#endif
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if (audioFault() != AudioFault::None) return;
#endif
    static_cast<void>(NES_COST_CPU_STEP());

#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(mmc5ReadQuantum_.active){flushCpuReads();cpuReadBudget_=0;}
#endif
}

#if defined(NES_CORE_CPU_IDLE_LOOPS) && NES_CORE_CPU_IDLE_LOOPS
/**
 * @brief Сворачивает доказанный цикл обычной памяти без потери гостевых тактов.
 * @details Проверяет код, адреса, флаги и исходный горизонт. Counted-цикл способен изменить итоговый PC; вызывающий код не должен кэшировать прежний PC через этот метод.
 * @param bodyPc PC тела уже проверяемого обычного цикла.
 * @param branchPc PC завершающей ветви проверяемого цикла.
 * @param bodyCycles Реальное число тактов тела цикла.
 * @param branchCycles Реальное число тактов последней ветви, включая page crossing.
 * @param deadline Абсолютный предел гостевых CPU-тактов исходного quiet-интервала.
 */
void Machine::collapseIdleLoop(std::uint16_t bodyPc, std::uint16_t branchPc,
                              unsigned bodyCycles, unsigned branchCycles, std::uint64_t deadline) noexcept {
    const unsigned loopCycles = bodyCycles + branchCycles;
    if (videoFrameReady() || cpu_.state_.jammed || cpu_.nmiReady_ || cpu_.nmiPending_ || cpu_.irqPending_ ||
        (cpu_.irqLine_ && !(cpu_.state_.p & 4U)) || dmaPending_ || dmcStalls_ ||
        cpuReadBudget_ < 2U * loopCycles) return;
    const auto now = cpuCycles();
    if (now >= deadline || deadline - now < 2U * loopCycles) return;
    /**
     * @brief Проверяет допуск адреса к обычной RAM/PRG; область MMIO не становится обычной памятью.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto ordinary = [](std::uint16_t address) { return address < 0x2000 || address >= 0x8000; };
    const unsigned length = static_cast<std::uint16_t>(branchPc - bodyPc);
    if (length < 1 || length > 3 || !ordinary(bodyPc) || !ordinary(branchPc)) return;
    /**
     * @brief Читает только уже допущенную обычную RAM/PRG без продвижения clock; MMIO фильтруется до вызова.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto peek = [this](std::uint16_t a) { return a < 0x2000 ? ram_[a & 0x7ff] : cartridge_.cpuRead(a, openBus_); };
    for (unsigned i = 0; i < length + 2U; ++i)
        if (!ordinary(static_cast<std::uint16_t>(bodyPc + i))) return;
    const auto opcode = peek(bodyPc), branchOpcode = peek(branchPc);
    const auto& instruction = Cpu6502::instructions_[opcode];
    const auto branchOp = Cpu6502::instructions_[branchOpcode].op;
    using Op = Cpu6502::Op; using Mode = Cpu6502::Mode;
    if (branchOp != Op::BNE && branchOp != Op::BEQ && branchOp != Op::BPL && branchOp != Op::BMI &&
        branchOp != Op::BCC && branchOp != Op::BCS && branchOp != Op::BVC && branchOp != Op::BVS) return;
    const auto afterBranch = static_cast<std::uint16_t>(branchPc + 2U);
    if (static_cast<std::uint16_t>(afterBranch + static_cast<std::int8_t>(peek(static_cast<std::uint16_t>(branchPc + 1U)))) != bodyPc) return;
    if (!ordinary(afterBranch)) return; // Фиктивное шинное чтение выполненного перехода.
    const bool cross = (afterBranch & 0xff00U) != (bodyPc & 0xff00U);
    if (cross && !ordinary(static_cast<std::uint16_t>((afterBranch & 0xff00U) | (bodyPc & 0xffU)))) return;
    if (branchCycles != 3U + unsigned(cross)) return;
    const bool counted = instruction.op == Op::DEX || instruction.op == Op::DEY || instruction.op == Op::INX || instruction.op == Op::INY;
    unsigned expectedLength = 0, expectedBodyCycles = 0;
    if (counted) {
        if (branchOp != Op::BNE || length != 1 || bodyCycles != 2) return;
    } else {
        if (instruction.op != Op::LDA && instruction.op != Op::LDX && instruction.op != Op::LDY &&
            instruction.op != Op::BIT && instruction.op != Op::CMP && instruction.op != Op::CPX && instruction.op != Op::CPY) return;
        std::uint16_t address = 0, base = 0;
        const auto low = length >= 2 ? peek(static_cast<std::uint16_t>(bodyPc + 1U)) : 0;
        switch (instruction.mode) {
        case Mode::Imm: expectedLength = 2; expectedBodyCycles = 2; address = static_cast<std::uint16_t>(bodyPc + 1U); break;
        case Mode::Zp: expectedLength = 2; expectedBodyCycles = 3; address = low; break;
        case Mode::ZpX: case Mode::ZpY:
            expectedLength = 2; expectedBodyCycles = 4;
            // Регистр результата не должен изменить собственный следующий адрес.
            if ((instruction.op == Op::LDX && instruction.mode == Mode::ZpX) ||
                (instruction.op == Op::LDY && instruction.mode == Mode::ZpY)) return;
            address = static_cast<std::uint8_t>(low + (instruction.mode == Mode::ZpX ? cpu_.state_.x : cpu_.state_.y)); break;
        case Mode::Abs: case Mode::AbsX: case Mode::AbsY: {
            expectedLength = 3;
            if (length != 3) return;
            base = static_cast<std::uint16_t>(low | (std::uint16_t(peek(static_cast<std::uint16_t>(bodyPc + 2U))) << 8));
            if ((instruction.op == Op::LDX && instruction.mode == Mode::AbsX) ||
                (instruction.op == Op::LDY && instruction.mode == Mode::AbsY)) return;
            address = static_cast<std::uint16_t>(base + (instruction.mode == Mode::AbsX ? cpu_.state_.x : instruction.mode == Mode::AbsY ? cpu_.state_.y : 0));
            const bool pageCross = (base & 0xff00U) != (address & 0xff00U);
            expectedBodyCycles = 4U + unsigned(instruction.mode != Mode::Abs && pageCross);
            if (instruction.mode != Mode::Abs && pageCross && !ordinary(static_cast<std::uint16_t>((base & 0xff00U) | (address & 0xffU)))) return;
            break;
        }
        case Mode::XInd: {
            expectedLength = 2; expectedBodyCycles = 6;
            // Поддерживаемые читающие опкоды этого режима записывают A, а не X.
            const auto pointer = static_cast<std::uint8_t>(low + cpu_.state_.x);
            address = static_cast<std::uint16_t>(peek(pointer) | (std::uint16_t(peek(static_cast<std::uint8_t>(pointer + 1U))) << 8)); break;
        }
        case Mode::IndY: {
            expectedLength = 2;
            base = static_cast<std::uint16_t>(peek(low) | (std::uint16_t(peek(static_cast<std::uint8_t>(low + 1U))) << 8));
            address = static_cast<std::uint16_t>(base + cpu_.state_.y);
            const bool pageCross = (base & 0xff00U) != (address & 0xff00U);
            expectedBodyCycles = 5U + unsigned(pageCross);
            if (pageCross && !ordinary(static_cast<std::uint16_t>((base & 0xff00U) | (address & 0xffU)))) return;
            break;
        }
        default: return;
        }
        if (length != expectedLength || bodyCycles != expectedBodyCycles || !ordinary(address)) return;
    }
    unsigned available = cpuReadBudget_;
    if (deadline - now < available) available = static_cast<unsigned>(deadline - now);
    unsigned iterations = available / loopCycles;
    if (counted) {
        const bool x = instruction.op == Op::DEX || instruction.op == Op::INX;
        const unsigned value = x ? cpu_.state_.x : cpu_.state_.y;
        const bool down = instruction.op == Op::DEX || instruction.op == Op::DEY;
        const unsigned untilZero = down ? (value ? value : 256U) : 256U - value;
        iterations = std::min(iterations, untilZero - 1U);
    }
    if (iterations < 2) return;
    if (counted) {
        auto& value = (instruction.op == Op::DEX || instruction.op == Op::INX) ? cpu_.state_.x : cpu_.state_.y;
        const bool down = instruction.op == Op::DEX || instruction.op == Op::DEY;
        value = static_cast<std::uint8_t>(down ? value - iterations : value + iterations);
        cpu_.setNz(value);
    }
    const unsigned elapsed = iterations * loopCycles;
    // Каждое пропущенное обычное чтение остаётся точным отложенным шинным тактом CPU.
    // P+B <=255 — существующий инвариант квантов чтения; elapsed <= B.
    cpu_.state_.cycles += elapsed;
    pendingCpuReads_ = static_cast<std::uint8_t>(pendingCpuReads_ + elapsed);
    cpuReadBudget_ = static_cast<std::uint8_t>(cpuReadBudget_ - elapsed);
    // Маскированный IRQ мог подняться во время последнего настоящего чтения перехода.
    // Пропущенные чтения опрашивают уже стабильную линию в начале каждого такта.
    cpu_.irqAtLastCycleStart_ = cpu_.irqLine_;
    cpu_.nmiAtLastCycleStart_ = false;
    // PC, завершающая открытая шина перехода и эффективный базовый адрес уже равны
    // неподвижной точке последней завершённой итерации.

}
#endif

#if defined(NES_CORE_STATUS_POLL_LOOP) && NES_CORE_STATUS_POLL_LOOP
#if defined(NES_CORE_STATUS_POLL_BIT) && NES_CORE_STATUS_POLL_BIT

#if defined(NES_CORE_STATUS_POLL_PURE_CAPABILITY) && NES_CORE_STATUS_POLL_PURE_CAPABILITY
/** @details Допуск определяется моделью аппаратуры и реальным владельцем; одних совпавших bytes недостаточно. */
bool Machine::pureStatusPollContract() const noexcept {
    constexpr unsigned required = Cartridge::PureCpuPrgRead | Cartridge::CpuWritesOnlyPrgMapping |
        Cartridge::NoPpuReadLatch | Cartridge::NoCpuClockEvent | Cartridge::NoCpuWriteStrobe;
    if ((cartridge_.statusPollCapabilities() & required) != required ||
        cartridge_.hasPpuReadSideEffects() || !cartridge_.cpuWriteStrobeIsNoop() ||
        cartridge_.cpuIrqQuietCycles() != ~0U) {
        NES_POLL_PROBE(HardwareRefusal, 0); return false;
    }
    if (&timingPpu_.bus_ != this || timingPpu_.capture_ || videoObserver_) {
        NES_POLL_PROBE(OwnerRefusal, 0); return false;
    }
#if defined(NES_CORE_PPU_LIVE_COLOUR_STREAM) && NES_CORE_PPU_LIVE_COLOUR_STREAM
    if (colourCapture_) { NES_POLL_PROBE(OwnerRefusal, 0); return false; }
#endif
    if (videoObservableOnly_) return true;
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if (outputOwner_ && outputFrameExecutable()) return true;
#endif
    NES_POLL_PROBE(OwnerRefusal, 0); return false;
}
#endif
/**
 * @brief Сворачивает только доказанный опрос STATUS с сохранением результата и шинного времени.
 * @details Проверка стабильности статуса и идемпотентности не разрешает чтения за будущей границей. Недопустимый цикл остаётся исходному интерпретатору.
 * @param branchPc PC завершающей ветви проверяемого цикла.
 * @param branchCycles Реальное число тактов последней ветви, включая page crossing.
 * @param deadline Абсолютный предел гостевых CPU-тактов исходного quiet-интервала.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Machine::collapseStatusPollLoop(std::uint16_t branchPc, unsigned branchCycles,
                                         std::uint64_t deadline) noexcept {
    NES_POLL_PROBE(Attempt, 0);
    const unsigned base = cpu_.state_.pc;
    /**
     * @brief Читает только уже допущенную обычную RAM/PRG без продвижения clock; MMIO фильтруется до вызова.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto peek = [this](unsigned address) {
        return cartridge_.cpuRead(static_cast<std::uint16_t>(address), openBus_);
    };
#if defined(NES_CORE_STATUS_POLL_BIT) && NES_CORE_STATUS_POLL_BIT
    const bool bitPoll = branchPc == base + 3U;
#if defined(NES_CORE_STATUS_POLL_PAGE_CROSS) && NES_CORE_STATUS_POLL_PAGE_CROSS
    // Для exact -5 перехода сравнивается PC после operand с началом BIT.
    // Старый диапазон base ниже запрещает 16-битный wrap до любого peek.
    const bool bitPageCross = bitPoll && (base & 0xFF00U) != ((base + 5U) & 0xFF00U);
#endif
    if (bitPoll) {
#if defined(NES_CORE_STATUS_POLL_PURE_CAPABILITY) && NES_CORE_STATUS_POLL_PURE_CAPABILITY && defined(NES_COLOUR_REPLAY_TEST) && NES_COLOUR_REPLAY_TEST
        // Проверочная трасса требует каждое реальное MMIO-наблюдение: даже
        // идемпотентное значение STATUS не разрешает удалить её события.
        if (colourtrace::producer == this && colourtrace::hook) {
            NES_POLL_PROBE(OwnerRefusal, 0); return 0;
        }
#endif
        const auto value = timingPpu_.openBus_;
        const unsigned flags = (value & 0xC0U) | ((cpu_.state_.a & value) ? 0U : 2U);
#if defined(NES_CORE_STATUS_POLL_PAGE_CROSS) && NES_CORE_STATUS_POLL_PAGE_CROSS
        if (branchCycles != 3U + unsigned(bitPageCross) || base < 0x8000U || base > 0xFFFAU ||
            !splitVideoEnabled() ||
#else
        if (branchCycles != 3 || base < 0x8000U || base > 0xFFFAU ||
            (base & 0xFF00U) != ((base + 5U) & 0xFF00U) || !splitVideoEnabled() ||
#endif
            !deferPpuAllowed_ || region_ != Region::Ntsc || !cartridge_.loaded() ||
#if defined(NES_CORE_STATUS_POLL_PURE_CAPABILITY) && NES_CORE_STATUS_POLL_PURE_CAPABILITY
            (cartridge_.mapper() != 2 && !pureStatusPollContract()) ||
#if defined(NES_CORE_STATUS_POLL_PAGE_CROSS) && NES_CORE_STATUS_POLL_PAGE_CROSS
            // Старое M2 исключение сохраняется только для старого 7-тактного пути.
            (bitPageCross && cartridge_.mapper() == 2 && !pureStatusPollContract()) ||
#endif
#else
            cartridge_.mapper() != 2 ||
#endif
            videoFrameReady() || cpu_.inStep_ || cpu_.state_.jammed ||
            cpu_.nmiReady_ || cpu_.nmiPending_ || cpu_.irqPending_ ||
            (cpu_.irqLine_ && !(cpu_.state_.p & 4U)) || dmaPending_ || dmcStalls_ ||
            (cpu_.state_.p & 0xC2U) != flags || (cpu_.state_.p & 0x40U) || cpu_.baseAddress_ != 0x2002U)
            return 0;
        constexpr std::uint8_t bytes[] = {0x2C,0x02,0x20,0x50,0xFB};
        for (unsigned i = 0; i < sizeof(bytes); ++i) if (peek(base + i) != bytes[i]) return 0;
#if defined(NES_CORE_STATUS_POLL_PAGE_CROSS) && NES_CORE_STATUS_POLL_PAGE_CROSS
        // BVC сначала читает base+5, затем исправляет PCH. Оба адреса — чистая
        // PRG; наружу остаётся именно байт второго dummy, а не первого.
        const unsigned finalRead = bitPageCross ?
            ((base + 5U) & 0xFF00U) | (base & 0xFFU) : base + 5U;
        if (openBus_ != peek(finalRead)) return 0;
#else
        if (openBus_ != peek(base + 5U)) return 0;
#endif
    } else
#endif
    {
    if (branchCycles != 3 || base < 0x8000U || base > 0xFFF8U || branchPc != base + 5U ||
        (base & 0xFF00U) != ((base + 7U) & 0xFF00U) || !splitVideoEnabled() ||
        !deferPpuAllowed_ || region_ != Region::Ntsc || !cartridge_.loaded() ||
        cartridge_.mapper() != 2 || videoFrameReady() || cpu_.inStep_ || cpu_.state_.jammed ||
        cpu_.nmiReady_ || cpu_.nmiPending_ || cpu_.irqPending_ ||
        (cpu_.irqLine_ && !(cpu_.state_.p & 4U)) || dmaPending_ || dmcStalls_ ||
        cpu_.state_.a || (cpu_.state_.p & 0x82U) != 2U || cpu_.baseAddress_ != base + 4U)
        return 0;
    // Неизменяемый ROM проверяется после каждого переключения банка. BEQ в одной странице
    // делает три чтения; LDA absolute и AND immediate — четыре и два.
    constexpr std::uint8_t bytes[] = {0xAD,0x02,0x20,0x29,0x40,0xF0,0xF9};
    for (unsigned i = 0; i < sizeof(bytes); ++i) if (peek(base + i) != bytes[i]) return 0;
    if (openBus_ != peek(base + 7U)) return 0;
    }
#if defined(NES_CORE_STATUS_POLL_BIT) && NES_CORE_STATUS_POLL_BIT
#if defined(NES_CORE_STATUS_POLL_PAGE_CROSS) && NES_CORE_STATUS_POLL_PAGE_CROSS
    const unsigned loopCycles = bitPoll ? (bitPageCross ? 8U : 7U) : 9U;
#else
    const unsigned loopCycles = bitPoll ? 7U : 9U;
#endif
#else
    constexpr unsigned loopCycles = 9U;
#endif
    const auto now = cpuCycles();
    if (now >= deadline || videoHasFault()) return 0;
    unsigned available = std::min<unsigned>(cpuReadBudget_, 255U - pendingCpuReads_);
    available = static_cast<unsigned>(std::min<std::uint64_t>(available, deadline - now));
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if (audioFault() != AudioFault::None) return 0;
    if (audioPacket_) {
        const auto origin = audioPacket_->header().baseCycle;
        if (now < origin || now - origin > 65535U) return 0;
        available = static_cast<unsigned>(std::min<std::uint64_t>(available, 65535U - (now - origin)));
    }
#endif
    NES_POLL_PROBE(AvailableGuestCycles, available);
    unsigned iterations = available / loopCycles;
#if defined(NES_CORE_STATUS_POLL_SEARCH) && NES_CORE_STATUS_POLL_SEARCH
    // При фиксированном состоянии увеличивается только конец интервала:
    // same-row, S0-mask и overflow-count допускают ровно начальный префикс.
    const auto repeatable = [this, loopCycles](unsigned count) noexcept {
        NES_STATUS_SEARCH_CHECK();
        const unsigned dots = pendingPpuDots_ + 3U * (pendingCpuReads_ + count * loopCycles);
        return dots <= ppuDeferBudget_ && timingPpu_.canRepeatStatusPoll(dots);
    };
    iterations = statuspollsearch::maximumIterations(iterations, repeatable);
#else
    for (; iterations >= 2; --iterations) {
        NES_STATUS_SEARCH_CHECK();
        const unsigned dots = pendingPpuDots_ + 3U * (pendingCpuReads_ + iterations * loopCycles);
        if (dots <= ppuDeferBudget_ && timingPpu_.canRepeatStatusPoll(dots)) break;
    }
#endif
    NES_STATUS_SEARCH_SELECTED(available, loopCycles, iterations);
    if (iterations < 2) return 0;
    NES_POLL_PROBE(Fold, iterations * loopCycles);
    const unsigned elapsed = iterations * loopCycles;
    cpu_.state_.cycles += elapsed;
    pendingCpuReads_ = static_cast<std::uint8_t>(pendingCpuReads_ + elapsed);
    cpuReadBudget_ = static_cast<std::uint8_t>(cpuReadBudget_ - elapsed);
    cpu_.irqAtLastCycleStart_ = cpu_.irqLine_;
    cpu_.nmiAtLastCycleStart_ = false;
    // PC, A/Z/N, baseAddress и обе шинные защёлки равны неподвижной точке последней
    // настоящей полной итерации. Существующая фиксация квантов чтения продвигает все такты.
    return elapsed;
}
#else
/**
 * @brief Сворачивает только доказанный опрос STATUS с сохранением результата и шинного времени.
 * @details Проверка стабильности статуса и идемпотентности не разрешает чтения за будущей границей. Недопустимый цикл остаётся исходному интерпретатору.
 * @param branchPc PC завершающей ветви проверяемого цикла.
 * @param branchCycles Реальное число тактов последней ветви, включая page crossing.
 * @param deadline Абсолютный предел гостевых CPU-тактов исходного quiet-интервала.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Machine::collapseStatusPollLoop(std::uint16_t branchPc, unsigned branchCycles,
                                         std::uint64_t deadline) noexcept {
    NES_POLL_PROBE(Attempt, 0);
    const unsigned base = cpu_.state_.pc;
    if (branchCycles != 3 || base < 0x8000U || base > 0xFFF8U || branchPc != base + 5U ||
        (base & 0xFF00U) != ((base + 7U) & 0xFF00U) || !splitVideoEnabled() ||
        !deferPpuAllowed_ || region_ != Region::Ntsc || !cartridge_.loaded() ||
        cartridge_.mapper() != 2 || videoFrameReady() || cpu_.inStep_ || cpu_.state_.jammed ||
        cpu_.nmiReady_ || cpu_.nmiPending_ || cpu_.irqPending_ ||
        (cpu_.irqLine_ && !(cpu_.state_.p & 4U)) || dmaPending_ || dmcStalls_ ||
        cpu_.state_.a || (cpu_.state_.p & 0x82U) != 2U || cpu_.baseAddress_ != base + 4U)
        return 0;
    // Неизменяемый ROM проверяется после каждого переключения банка. BEQ в одной странице
    // делает три чтения; LDA absolute и AND immediate — четыре и два.
    /**
     * @brief Читает только уже допущенную обычную RAM/PRG без продвижения clock; MMIO фильтруется до вызова.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto peek = [this](unsigned address) {
        return cartridge_.cpuRead(static_cast<std::uint16_t>(address), openBus_);
    };
    constexpr std::uint8_t bytes[] = {0xAD,0x02,0x20,0x29,0x40,0xF0,0xF9};
    for (unsigned i = 0; i < sizeof(bytes); ++i) if (peek(base + i) != bytes[i]) return 0;
    if (openBus_ != peek(base + 7U)) return 0;
    const auto now = cpuCycles();
    if (now >= deadline || videoHasFault()) return 0;
    unsigned available = std::min<unsigned>(cpuReadBudget_, 255U - pendingCpuReads_);
    available = static_cast<unsigned>(std::min<std::uint64_t>(available, deadline - now));
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if (audioFault() != AudioFault::None) return 0;
    if (audioPacket_) {
        const auto origin = audioPacket_->header().baseCycle;
        if (now < origin || now - origin > 65535U) return 0;
        available = static_cast<unsigned>(std::min<std::uint64_t>(available, 65535U - (now - origin)));
    }
#endif
    unsigned iterations = available / 9U;
    for (; iterations >= 2; --iterations) {
        const unsigned dots = pendingPpuDots_ + 3U * (pendingCpuReads_ + iterations * 9U);
        if (dots <= ppuDeferBudget_ && timingPpu_.canRepeatStatusPoll(dots)) break;
    }
    if (iterations < 2) return 0;
    const unsigned elapsed = iterations * 9U;
    cpu_.state_.cycles += elapsed;
    pendingCpuReads_ = static_cast<std::uint8_t>(pendingCpuReads_ + elapsed);
    cpuReadBudget_ = static_cast<std::uint8_t>(cpuReadBudget_ - elapsed);
    cpu_.irqAtLastCycleStart_ = cpu_.irqLine_;
    cpu_.nmiAtLastCycleStart_ = false;
    // PC, A/Z/N, baseAddress и обе шинные защёлки равны неподвижной точке последней
    // настоящей полной итерации. Существующая фиксация квантов чтения продвигает все такты.
    return elapsed;
}
#endif
#endif

/**
 * @brief Исполняет гостевую машину до границы кадра либо до ошибки.
 * @details Результат FrameReady относится к гостевому PPU. Он не означает завершения RGB, PCM, PPA или вывода на LCD у приложения.
 * @return Вычисленное значение или состояние типа RunStatus согласно описанному контракту.
 */
RunStatus Machine::stepFrame() noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_&&!outputFrameExecutable())return RunStatus::VideoFault;
#endif
    if (!cartridge_.loaded()) return RunStatus::NoCartridge;
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
    // Хвост предыдущей инструкции может содержать отложенные аудиособытия. Завершить
    // PCM старого кадра перед его отбрасыванием даже без публичных наблюдателей.
    flushCpuReads();
#endif
    videoClearFrameReady();
    audioClearSamples();
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return RunStatus::VideoFault;
#endif
    const auto deadline = cpuCycles() + 40000;
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    Mmc5ReadQuantumFrameScope readQuantumScope(*this,deadline);
#endif
#if defined(NES_CORE_CPU_QUIET_BLOCKS) && NES_CORE_CPU_QUIET_BLOCKS
    // Legacy-блоки сохраняют каждый значимый CPU-строб записи на скалярной
    // шине. Неизменяемая политика блока разрешает сохраняющие состояние записи RAM.
    const bool quietBlocksAllowed = true;
#endif
#if defined(NES_CORE_CPU_IDLE_LOOPS) && NES_CORE_CPU_IDLE_LOOPS
    std::uint16_t previousPc = 0;
    unsigned previousCycles = 0;
    bool havePrevious = false;
#endif
    while (!videoFrameReady() && cpuCycles() < deadline) {
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
        // Положительный budget, включая pending0, не передаётся generic quiet/idle helpers.
        if(mmc5ReadQuantum_.active&&(pendingCpuReads_||cpuReadBudget_)){
#if defined(NES_CORE_CPU_IDLE_LOOPS) && NES_CORE_CPU_IDLE_LOOPS
            havePrevious=false;
#endif
            // Используем только остаток уже выданного GQ: pending+budget не превышает80 M2.
            // Новый admission и generic quiet здесь запрещены. Неподдержанный opcode
            // остаётся обычному interpreter с тем же dedicated commit/flush контрактом.
            unsigned decoded=0;
            if(cpuReadBudget_>=8U)decoded=NES_COST_QUIET_MMC5(cpuReadBudget_);
            if(!decoded)static_cast<void>(NES_COST_CPU_STEP());
            if(!outputFrameExecutable()||videoHasFault())return RunStatus::VideoFault;
            if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
            if(cpu_.state_.jammed)return RunStatus::CpuJammed;
            continue;
        }
#endif
#if !defined(NES_CORE_FRAME_FAULT_DEDUP) || !NES_CORE_FRAME_FAULT_DEDUP
        // Все getter-вызовы после предыдущих проверок чисты. Сохранить старые
        // повторные проверки только при отключённой необязательной композиции.
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return RunStatus::VideoFault;
#endif

#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
        if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#endif
#if defined(NES_CORE_CPU_QUIET_BLOCKS) && NES_CORE_CPU_QUIET_BLOCKS
        if(cartridge_.mapper()==5){
            const auto remaining=deadline-cpuCycles();
            if(remaining>=8U&&runMmc5QuietBlock(static_cast<unsigned>(std::min<std::uint64_t>(remaining,255U)))){
#if defined(NES_CORE_CPU_IDLE_LOOPS) && NES_CORE_CPU_IDLE_LOOPS
                havePrevious=false;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
                if(videoHasFault())return RunStatus::VideoFault;
#endif
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
                if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
#endif
                continue;
            }
        }
        if (quietBlocksAllowed && cpuReadBudget_ >= 8
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
            &&!mmc5ReadQuantum_.active
#endif
            ) {
            const auto remaining = deadline - cpuCycles();
            if (remaining >= 8 && NES_COST_QUIET_LEGACY(static_cast<unsigned>(std::min<std::uint64_t>(remaining, 255U)))) {
#if defined(NES_CORE_CPU_IDLE_LOOPS) && NES_CORE_CPU_IDLE_LOOPS
                // Блок может менять RAM и регистры между скалярными шагами.
                havePrevious = false;
#endif
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
                if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return RunStatus::VideoFault;
#endif
                continue;
            }
        }
#endif
#if defined(NES_CORE_CPU_IDLE_LOOPS) && NES_CORE_CPU_IDLE_LOOPS
        const auto beforePc = cpu_.state_.pc;
        const auto instructionCycles = NES_COST_CPU_STEP();
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
        if(mmc5ReadQuantum_.active&&(pendingCpuReads_||cpuReadBudget_)){
            havePrevious=false;
            if(!outputFrameExecutable()||videoHasFault())return RunStatus::VideoFault;
            if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
            if(cpu_.state_.jammed)return RunStatus::CpuJammed;
            continue;
        }
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return RunStatus::VideoFault;
#endif

#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
        if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#if defined(NES_CORE_STATUS_POLL_LOOP) && NES_CORE_STATUS_POLL_LOOP
        if (collapseStatusPollLoop(beforePc, instructionCycles, deadline)) {
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
            if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
        }
#endif
        const unsigned backwards = static_cast<std::uint16_t>(beforePc - cpu_.state_.pc);
        if (backwards >= 1 && backwards <= 3 && havePrevious && previousPc == cpu_.state_.pc) {
            collapseIdleLoop(previousPc, beforePc, previousCycles, instructionCycles, deadline);
#if defined(NES_CORE_FRAME_FAULT_DEDUP) && NES_CORE_FRAME_FAULT_DEDUP && defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
            // Отложенное CPU-время может пересечь предел timestamp пакета даже
            // без новой записи ввода. Idle-helper не создаёт PPU-событий,
            // но эта аудиопроверка остаётся настоящей границей.
            if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
        }
        previousPc = beforePc; previousCycles = instructionCycles; havePrevious = true;
#else
#if defined(NES_CORE_STATUS_POLL_LOOP) && NES_CORE_STATUS_POLL_LOOP
        const auto beforeStatusPc = cpu_.state_.pc;
        const auto beforeStatusCycles = cpu_.state_.cycles;
#endif
        stepInstruction();
#if defined(NES_CORE_STATUS_POLL_LOOP) && NES_CORE_STATUS_POLL_LOOP
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
        if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
        if (videoHasFault()) return RunStatus::VideoFault;
#endif
        if (collapseStatusPollLoop(beforeStatusPc, static_cast<unsigned>(cpu_.state_.cycles - beforeStatusCycles), deadline)) {
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
            if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
        }
#endif
#endif
#if !defined(NES_CORE_FRAME_FAULT_DEDUP) || !NES_CORE_FRAME_FAULT_DEDUP || !defined(NES_CORE_CPU_IDLE_LOOPS) || !NES_CORE_CPU_IDLE_LOOPS
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
        if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return RunStatus::VideoFault;
#endif
#endif
        if (cpu_.state().jammed) return RunStatus::CpuJammed;
    }
#if !defined(NES_CORE_FRAME_FAULT_DEDUP) || !NES_CORE_FRAME_FAULT_DEDUP
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(videoHasFault())return RunStatus::VideoFault;
#endif
#endif
#if NES_MMC5_GENERALIZED_QUANTUM_DOMAIN
    if(mmc5ReadQuantum_.active){
        flushCpuReads();cpuReadBudget_=0;
        if(!outputFrameExecutable()||videoHasFault())return RunStatus::VideoFault;
        if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
    }
#endif
    return videoFrameReady() ? RunStatus::FrameReady : RunStatus::ClockLimit;
}

/**
 * @brief Обновляет физические страницы таблиц имён по текущему mirroring.
 * @details Сохраняет фиксированную four-screen-проводку и не дублирует отдельные правила mapper.
 */
void Machine::refreshNametableMapping() noexcept {
    constexpr std::uint16_t pages[][4]{
        {0, 0, 1024, 1024}, {0, 1024, 0, 1024}, {0, 0, 0, 0},
        {1024, 1024, 1024, 1024}, {0, 1024, 2048, 3072}
    };
    const auto* mapping = pages[static_cast<unsigned>(cartridge_.mirroring())];
    for (unsigned page = 0; page < 4; ++page) nametablePages_[page] = mapping[page];
}

/**
 * @brief Вычисляет физический индекс с правилами зеркалирования PPU.
 * @details Учитывает таблицы имён либо специальные зеркала палитры; сам адрес не выдаёт строб чтения.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
 */
std::uint16_t Machine::nametableAddress(std::uint16_t address) const noexcept {
    return static_cast<std::uint16_t>(nametablePages_[(address >> 10) & 3U] | (address & 1023U));
}
/**
 * @brief Читает байт адресного пространства PPU с нужным выбором памяти.
 * @details Паттерны CHR, таблицы имён и палитра имеют разные правила зеркалирования и mapper-эффектов; чтение не заменяет отдельный адресный строб.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::ppuRead(std::uint16_t address) noexcept {
    address &= 0x3fff;
    if(cartridge_.mapper()==5&&cartridge_.mmc5_)return cartridge_.mmc5_->ppuRead(address,Mmc5::Fetch::Cpu,nametables_.data(),0).value;
    const auto value = address < 0x2000 ? cartridge_.chrRead(address) : nametables_[nametableAddress(address)];
    cartridge_.ppuReadStrobe();
    return value;
}
/**
 * @brief Записывает байт в отображённую память PPU.
 * @details Сохраняет зеркалирование, ограничения CHR-ROM и замораживание уже заимствованных CHR-RAM-значений.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::ppuWrite(std::uint16_t address, std::uint8_t value) noexcept {
    if(cartridge_.mapper()==5&&cartridge_.mmc5_){
#if defined(NES_RUN_MMC5_SPAN_VIEW) && NES_RUN_MMC5_SPAN_VIEW
        const auto target=cartridge_.mmc5_->ppuWriteTarget(address,true);
#endif
        cartridge_.mmc5_->ppuWrite(address&0x3fffU,value,nametables_.data());
#if defined(NES_RUN_MMC5_SPAN_VIEW) && NES_RUN_MMC5_SPAN_VIEW
        if(target!=~0U&&videoPacket_)static_cast<void>(videoPacket_->addMmc5PhysicalWrite(target,value));
#endif
        return;
    }
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
    if(splitVideoEnabled()&&(address&0x3FFFU)<0x2000U)freezeVideoChrWrite(address&0x1FFFU,value);
#endif
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    if(splitVideoEnabled() && videoPacket_){
        const auto ref=videoReference(address);
        if((ref&VideoByteRef::kindMask)==VideoByteRef::chrRam)
            videoPacket_->add(VideoOp::ChrWrite,ref&255U,(ref>>8)&255U,0,value);
        else if((ref&VideoByteRef::kindMask)==VideoByteRef::nametable)
            videoPacket_->add(VideoOp::NameWrite,ref&255U,(ref>>8)&255U,0,value);
    }
#endif

    address &= 0x3fff;
    if (address < 0x2000) cartridge_.chrWrite(address, value);
    else {
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN && defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
        const auto physical=nametableAddress(address);
        // Физический индекс записей включает атрибуты и все алиасы NT.
        videoNameMarkers_.update(physical,value);
        nametables_[physical]=value;
#else
        nametables_[nametableAddress(address)] = value;
#endif
    }
}
/**
 * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
 * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::dmcRead(std::uint16_t address) noexcept {
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && \
    0 && 0
    // Legacy callback имеет тот же vector-строб, что и временной APU.
    // Закрыть прежний raster без повторного входа в APU/CPU clocks.
#endif
    const auto value = cartridge_.cpuRead(address, openBus_);
    // MMC5 PCM snoop происходит внутри текущего APU tickEvent, до его mixerOutput.
    // Pending уже материализован; повторный synchronize здесь вызвал бы reentry.
    if(Cartridge::hasExpansionAudioMapper(cartridge_.mapper()))apu_.setExpansionInputAtDmcFetch(cartridge_.selectedExpansionSample());
    return value;
}
/**
 * @brief Учитывает украденные DMC такты в установленном порядке.
 * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
 * @param cycles Число прошедших или украденных CPU-тактов; гостевой порядок эффектов задаёт вызывающий.
 */
void Machine::dmcStall(unsigned cycles) noexcept {
    dmcStalls_ += cycles;
}

#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
/**
 * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
 * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
 * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @param lowReference Приёмник физической идентичности младшей плоскости до её read-строба.
 * @return Упакованные результаты настоящей NT/attribute/low/high-выборки; lowReference отдельно получает физическую идентичность.
 */
template<bool CaptureReference>
inline std::uint32_t Machine::legacyBackgroundTileFetch(std::uint16_t v,unsigned control,
                                                       std::uint32_t* lowReference) noexcept {
    const auto nametable = static_cast<std::uint16_t>(0x2000U | (v & 0x0FFFU));
    const auto tile = nametables_[nametablePages_[(nametable >> 10) & 3U] | (nametable & 1023U)];
    const auto attributeAddress = static_cast<std::uint16_t>(0x23C0U | (v & 0x0C00U) | ((v >> 4) & 0x38U) | ((v >> 2) & 7U));
    const auto rawAttribute = nametables_[nametablePages_[(attributeAddress >> 10) & 3U] | (attributeAddress & 1023U)];
    const unsigned attribute = (rawAttribute >> (((v >> 4) & 4U) | (v & 2U))) & 3U;
    const auto pattern = static_cast<std::uint16_t>(((control & 0x10U) ? 0x1000U : 0U) + unsigned(tile) * 16U + ((v >> 12) & 7U));
    if constexpr(CaptureReference) {
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
        const auto* byte=cartridge_.chrPages_[(pattern>>10)&7U]+(pattern&1023U);
        *lowReference=cachedVideoReference(byte); // ДО защёлки после старшего байта!
#if defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
        // Те же четыре логических чтения/A12/переход защёлки; данные представлены
        // физической идентичностью до строба, пока не потребуется непрозрачность.
        if(cartridge_.legacy_.backgroundTile(static_cast<std::uint16_t>(pattern+8U)))cartridge_.refreshChrPages();
        return (attribute<<16)|(std::uint32_t(tile)<<24);
#endif
#else
        static_cast<void>(lowReference);
#endif
    }
    const auto planes = cartridge_.readLegacyBackgroundPlanes(pattern);
    return std::uint32_t(planes) | (attribute << 16) | (std::uint32_t(tile) << 24);
}
/**
 * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
 * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
 * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
std::uint32_t Machine::legacyBackgroundTile(std::uint16_t v,unsigned control) noexcept {
    return legacyBackgroundTileFetch<false>(v,control,nullptr);
}
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
/**
 * @brief Получает один Legacy-фоновый тайл через выбранный временной интерфейс.
 * @details Сохраняет фактические байты и порядок адресных/read-стробов.
 * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
 * @return Значения выбранного тайла и физическая low-ссылка с сохранением post-read FD/FE-защёлки.
 */
Machine::VideoTileFetch Machine::videoLegacyBackgroundTile(std::uint16_t v,unsigned control,[[maybe_unused]] const TimingPpu* caller) noexcept {
    VideoTileFetch out{};
    out.packed=legacyBackgroundTileFetch<true>(v,control,&out.lowReference);
    return out;
}
#endif
#else
/**
 * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
 * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
 * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
std::uint32_t Machine::legacyBackgroundTile(std::uint16_t v, unsigned control) noexcept {
    const auto nametable = static_cast<std::uint16_t>(0x2000U | (v & 0x0FFFU));
    const auto tile = nametables_[nametablePages_[(nametable >> 10) & 3U] | (nametable & 1023U)];
    const auto attributeAddress = static_cast<std::uint16_t>(0x23C0U | (v & 0x0C00U) | ((v >> 4) & 0x38U) | ((v >> 2) & 7U));
    const auto rawAttribute = nametables_[nametablePages_[(attributeAddress >> 10) & 3U] | (attributeAddress & 1023U)];
    const unsigned attribute = (rawAttribute >> (((v >> 4) & 4U) | (v & 2U))) & 3U;
    const auto pattern = static_cast<std::uint16_t>(((control & 0x10U) ? 0x1000U : 0U) + unsigned(tile) * 16U + ((v >> 12) & 7U));
    const auto planes = cartridge_.readLegacyBackgroundPlanes(pattern);
    return std::uint32_t(planes) | (attribute << 16) | (std::uint32_t(tile) << 24);
}
#endif
#endif

#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
/**
 * @brief Подключает заимствованное хранилище аудиособытий и сбрасывает машину.
 * @details Настраивается потоком-владельцем. nullptr восстанавливает синхронный APU; память пакета остаётся у вызывающего.
 * @param storage Заимствованное хранилище выбранного протокола; nullptr обрабатывается контрактом метода.
 */
void Machine::configureSplitAudio(AudioPacket* storage
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
                                 ,bool expansionDac
#endif
                                 ) noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_)return; // Не менять active audio или сбрасывать чужую output-аренду.
#endif
    audioPacket_ = storage;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    expansionAudio_=storage&&expansionDac;
#endif
    reset(); // Явная операция владельца; непрерывность при смене режима не переносится.
}
/**
 * @brief Сбрасывает аудиотракт и связанные временные счётчики.
 * @details Настройка режима и lifetime внешнего пакета остаются у Machine/приложения.
 */
void Machine::audioReset() noexcept {
    apu_.reset(region_ == Region::Pal);
    timingApu_.reset(region_ == Region::Pal);
    ++audioGeneration_; audioSequence_ = 0;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    lastExpansionLevel_=0; // Новый полный APU тоже начинает expansion input с0.
#endif
    if (audioPacket_) audioPacket_->begin(audioGeneration_,audioSequence_,0,region_
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
                                         ,expansionAudio_
#endif
                                         );
}
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
/** @details TimingApu.cycles включает pending и равен текущей границеT−1 перед его tick(T). */
void Machine::audioPublishExpansion(std::int32_t level)noexcept{
    if(!audioPacket_){apu_.setExpansionInput(level);return;}
    if(!expansionAudio_){static_cast<void>(audioPacket_->addExpansion(timingApu_.cycles(),level));return;}
    if(level==lastExpansionLevel_)return;
    static_cast<void>(audioPacket_->addExpansion(timingApu_.cycles(),level));
    lastExpansionLevel_=level;
}
#endif
/**
 * @brief Передаёт запись регистра звуковому владельцу с её гостевым порядком.
 * @details В split-режиме сохраняется и вход renderer, и CPU-наблюдаемый эффект временного APU.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::audioWrite(std::uint16_t address,std::uint8_t value) noexcept {
    if (!audioPacket_) { apu_.write(address,value); return; }
    static_cast<void>(audioPacket_->addInput(timingApu_.cycles(),static_cast<std::uint8_t>(address-0x4000),value));
    timingApu_.write(address,value);
}
/**
 * @brief Считывает CPU-видимый статус APU с исходной семантикой очистки IRQ.
 * @details Это не peek: реальное чтение $4015 нельзя заменить безэффектным просмотром.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::audioReadStatus() noexcept {
    if (!audioPacket_) return apu_.readStatus();
    const auto status = timingApu_.readStatus();
    static_cast<void>(audioPacket_->addInput(timingApu_.cycles(),AudioPacket::readStatusOperation,status));
    return status;
}
/**
 * @brief Начинает накопление следующего блока PCM без изменения гостевых регистров каналов.
 * @details В split-режиме соответствующая операция сохраняется для renderer, чтобы границы аудиоблоков совпали.
 */
void Machine::audioClearSamples() noexcept {
    if (!audioPacket_) { apu_.clearSamples(); return; }
    static_cast<void>(audioPacket_->addInput(timingApu_.cycles(),AudioPacket::clearSamplesOperation,0));
}
/**
 * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
 * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::dmcRead(std::uint16_t address,std::uint64_t cycle) noexcept {
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU && \
    0 && 0
    // Callback APU может прийти после Cart.clock текущего M2, до его PPU dots.
    // Не вызывать flushCpuReads из callback: materializer закрывает только
    // прежний raster и сохраняет уже наступивший цифровой срез mapper.
#endif
    const auto value = cartridge_.cpuRead(address,openBus_);
    if (audioPacket_){
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        const auto level=expansionAudio_?cartridge_.selectedExpansionSample():0;
        static_cast<void>(audioPacket_->addDmc(cycle,address,value,level));
        if(expansionAudio_)lastExpansionLevel_=level;
#else
        static_cast<void>(audioPacket_->addDmc(cycle,address,value));
#endif
    }
    return value;
}
/**
 * @brief Учитывает украденные DMC такты в установленном порядке.
 * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 */
void Machine::dmcStall(unsigned count,std::uint64_t cycle) noexcept {
    dmcStalls_ += count;
    if (audioPacket_) audioPacket_->setDmcStall(cycle,count);
}
/**
 * @brief Закрывает аудиоинтервал и копирует только занятые записи в отдельный пакет.
 * @details CPU-владелец должен быть остановлен; функция не запускает AudioRenderer и не копирует неиспользуемую ёмкость.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param destination Отдельный незанятый аудиопакет для копии использованных записей; не алиас активного пакета.
 */
bool Machine::retireAudioPacket(AudioPacket& destination) noexcept {
    if (!audioPacket_ || &destination == audioPacket_ || audioFault() != AudioFault::None) return false;
    observeCpuReads();
    if (!audioPacket_->retireTo(destination,cpuCycles(),frames())) return false;
    ++audioSequence_;
    audioPacket_->begin(audioGeneration_,audioSequence_,cpuCycles(),region_
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
                        ,expansionAudio_
#endif
                        );
    return true;
}
#endif

}  // пространство имён nes::core

#undef NES_COST_CPU_STEP
#undef NES_COST_QUIET_MMC5
#undef NES_COST_QUIET_LEGACY


#undef NES_ROM_DECODE_DOMAIN

