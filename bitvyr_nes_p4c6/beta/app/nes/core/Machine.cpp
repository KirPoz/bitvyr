/**
 * @file Machine.cpp
 * @brief Планировщик CPU/APU/PPU, шина памяти, DMA и кадровые границы одной машины.
 * @ingroup nes_core
 */
#include <algorithm>
#include "Machine.h"
#if defined(NES_EXPERIMENTAL_MMC5_INACTIVE_GRANT)
#include "Mmc5InactiveGrant.h"
#endif
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "cpu/RomWindow.h"



#define NES_THIN_LARGE_GQ_DOMAIN 1

#define NES_COST_CPU_STEP() cpu_.step()
#define NES_COST_QUIET_MMC5(budget) cpu_.runQuietBlockWithPolicy<true>(budget)
#define NES_COST_QUIET_LEGACY(budget) cpu_.runQuietBlock(budget)
#include "StatusPollSearch.h"

#include "ppu/PpuColourReplay.h"

namespace nes::core {
#include "MachineSignalFused.inl"



/**
 * @brief Создаёт самостоятельный набор CPU/PPU/APU и шинной памяти машины.
 * @details ROM не принадлежит объекту; политика выделения legacy-пикселей может передаваться явно. Конкретная настройка split-режимов выполняется на остановленном владельце.
 */
Machine::Machine() noexcept : Machine(LegacyPixelAllocator{}) {}
/**
 * @brief Связывает CPU, оба PPU-тракта и APU с единственной шиной новой машины.
 * @param allocator Политика выделения legacy-пикселей; ROM и внешние frame/audio-пакеты конструктор не принимает во владение.
 */
Machine::Machine(LegacyPixelAllocator allocator) noexcept : cpu_(*this), ppu_(*this,allocator), apu_(*this)
    , timingPpu_(*this)
    , timingApu_(*this)
{}


/** @details Отказ проверяется до смены указателя; предыдущая арена остаётся borrowed. */


/** @details Ключ сохраняет исходный ROM backing, даже когда execution читает resident copy. */
RomWindow Machine::quietRomWindow(std::uint16_t pc) const noexcept {
    if(pc<0x8000U||!cartridge_.loaded()||cartridge_.prgSize_<8192U)return {};
    const auto* page=cartridge_.prgPages_[(pc>>13U)&3U];
    const auto address=reinterpret_cast<std::uintptr_t>(page);
    const auto begin=reinterpret_cast<std::uintptr_t>(cartridge_.prg_);
    if(!page||address<begin||address-begin>cartridge_.prgSize_-8192U)return {};
    return {page,romMappingEpoch_,0,true};
}

/**
 * @brief Проверяет и подключает заимствованный образ ROM.
 * @details Владелец образа удерживает неизменённые байты на весь срок использования. Отказ загрузки нельзя считать сохранением всех старых указателей картриджа; ранее выданные ссылки и эпохи обрабатываются установленным жизненным циклом.
 * @param image Неизменяемый заимствованный образ ROM; вызывающий сохраняет его срок жизни.
 * @param size Размер переданного диапазона в байтах.
 * @param region Явно выбранный гостевой регион NTSC/PAL.
 * @return Код проверки iNES/поддержки mapper; только Ok означает принятую загрузку, отказ имеет описанные эффекты инвалидирования.
 */
LoadResult Machine::load(const std::uint8_t* image, std::size_t size, Region region) noexcept {
    if(outputOwner_)return LoadResult::InvalidHeader; // Сначала явный cancel joined-аренды.

    invalidateThinPpu();
    invalidateColourCapture();
    flushCpuReads(); cpuReadBudget_ = 0;
    flushPpu(); // Завершить старые CHR-выборки до замены даже отвергаемого образа.
    // Отказ загрузки сохраняет старый конвейер PPU. Зафиксировать заимствованные байты старого ROM
    // и RAM ДО того, как Cartridge сможет инвалидировать или заменить их хранилище.
    if(splitVideoEnabled())timingPpu_.materializeBackgroundRefs();
    videoBgViewValid_=false;videoBgViewDirty_=true;
    // Отвергнутый load также меняет backing. Старый план не удерживает ROM
    // после завершённого выше долга и последующей замены модели/адреса.
    timingPpu_.invalidateMmc5RasterPlan();
    const auto result = cartridge_.load(image, size);
    refreshVideoPolicy(); // Отвергнутая загрузка тоже меняет допустимость по loaded.
    if (result == LoadResult::Ok) { region_ = region; reset(); }
    /**
     * @brief Обновляет число точек PPU, которые можно отложить без наблюдаемого события.
     * @details Предикаты читают текущий материализованный курсор. После MMIO требуется повторная оценка, поскольку изменяются задержки, адреса и NMI.
     * @return Вычисленное значение или состояние типа else согласно описанному контракту.
     */
    else refreshPpuDeferBudget(); // Отказ загрузки сохраняет прежнюю материализованную машину.
    return result;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void Machine::reset() noexcept {
    if(outputOwner_&&!outputInitializing_)return; // Sealed/Reading данные ещё заимствованы.
    if(thin_){invalidateThinPpu();thin_=nullptr;}
    invalidateColourCapture();
    pendingCpuReads_ = cpuReadBudget_ = 0;
    mmc5ReadQuantum_={};
    cycles_ = 0;
    openBus_ = ppuRemainder_ = dmaPage_ = 0;
    dmcStalls_ = 0;
    pendingPpuDots_ = ppuDeferBudget_ = 0;
    dmaPending_ = strobe_ = false;
    ram_.fill(0); nametables_.fill(0);
    // Связано ТОЛЬКО с настоящей очисткой NT, а не с videoReset или отказом загрузки.
    videoNameMarkers_.clear();
    buttons_.fill(0); controllerShift_.fill(0);
    cartridge_.reset();
    refreshNametableMapping();
    ppu_.reset(region_ == Region::Pal);
    const auto mapper = cartridge_.mapper();
    deferPpuAllowed_ = mapper == 0 || mapper == 1 || mapper == 2 || mapper == 3 || mapper == 7 || mapper == 69 || mapper == 71 || mapper == 85 || mapper == 160;
    deferIrqPpu_ = mapper == 160;
    deferIrqPpu_ = deferIrqPpu_ || mapper == 4;
    deferPpuAllowed_ = deferPpuAllowed_ || mapper == 4;
    if (!deferPpuAllowed_) ppuDeferBudget_ = 0;
    ppu_.setBusTracking(mapper == 4, mapper == 4 || mapper == 160);
    videoReset();
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

/**
 * @brief Обновляет число точек PPU, которые можно отложить без наблюдаемого события.
 * @details Предикаты читают текущий материализованный курсор. После MMIO требуется повторная оценка, поскольку изменяются задержки, адреса и NMI.
 */
void Machine::refreshPpuDeferBudget() const noexcept {
    ppuDeferBudget_ = 0;
    if (!deferPpuAllowed_) return;
    if(splitVideoEnabled()){
        unsigned budget=std::min(timingPpu_.quietDots(),cartridge_.ppuIrqQuietDotsAny());
        if(timingPpu_.scanline()>=240){
            budget=std::max(budget,timingPpu_.nonVisibleQuietDots());
        }else if(budget+timingPpu_.dot()<249U&&timingPpu_.canDeferDots(0)){
            const unsigned visible=249U-timingPpu_.dot();
            if(!deferIrqPpu_||(timingPpu_.deferredPrefix()==0&&
                cartridge_.ppuIrqQuietFor(visible,!timingPpu_.backgroundA12Low()))||
                cartridge_.ppuIrqQuietVisibleAny())budget=visible;
        }
        if(cartridge_.mapper_==4U&&outputOwner_&&
           region_==Region::Ntsc&&!thin_&&!videoObserver_&&!timingPpu_.capture_&&
           timingPpu_.canDeferDots(0)&&!(timingPpu_.control_&0x10U)&&
           cartridge_.irqEnabled_&&!cartridge_.irqPending_){
            const unsigned rises=cartridge_.irqCounter_&&!cartridge_.irqReload_?
                unsigned(cartridge_.irqCounter_):unsigned(cartridge_.irqLatch_)+1U;
            // При фоне в нижней таблице штатные фронты дают выборки спрайтов.
            // Для 8×8 первый подъём проходит фильтр A12, у 8×16 их до четырёх.
            const unsigned risesPerLine=(timingPpu_.control_&0x20U)?4U:1U;
            if(rises>=2U+risesPerLine){
                // Запас покрывает переход из незавершённой входной фазы.
                const unsigned rows=(rises-2U)/risesPerLine;
                // Первая точка гашения может выставить высокий A12 из v.
                const unsigned visibleDots=(240U-unsigned(timingPpu_.scanline()))*341U-timingPpu_.dot();
                const unsigned candidate=std::min({65535U,visibleDots,341U*rows+257U-timingPpu_.dot()});
                budget=std::max(budget,candidate);
            }
        }
        ppuDeferBudget_=static_cast<std::uint16_t>(budget);
        return;
    }
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
        if(budget<visible&&cartridge_.ppuIrqQuietVisibleAny())budget=visible;
    }
    ppuDeferBudget_ = static_cast<std::uint16_t>(budget);
}

/**
 * @brief Материализует заданное число точек через выбранный видеотракт и при необходимости обновляет горизонт CPU.
 * @tparam RefreshBudget false допустим для промежуточной части долга, если вызывающий путь сам обновит бюджет после последней части.
 * @param dots Точки PPU, а не M2 CPU; события IRQ/NMI остаются обязанностью обычного videoTickDots.
 */
template <bool RefreshBudget>
[[gnu::always_inline]] inline void Machine::advancePpu(unsigned dots) const noexcept {
    videoTickDots(dots);
    if constexpr (RefreshBudget) refreshPpuDeferBudget();
}

/**
 * @brief Обрабатывает ненулевой долг PPU с исходными временными границами.
 * @details В существующем Frame без Thin/observer/capture обе tickDots-границы сохраняются.
 * Промежуточный чистый horizon не читается PPU; следующий CPU admission получает конечный budget.
 * Остальные владельцы сохраняют прежний пересчёт после каждой части.
 */
void Machine::flushPpuSlow() const noexcept {
    unsigned dots = pendingPpuDots_;
    if (!dots) return;
    pendingPpuDots_ = 0;
    const unsigned prefix = splitVideoEnabled()?timingPpu_.deferredPrefix():ppu_.deferredPrefix();
    if (prefix && dots >= prefix) {
            // Следующий tick не читает CPU defer-cache; внешний callback исключён.
            if(dots>prefix&&outputOwner_&&!thin_&&!videoObserver_&&!timingPpu_.capture_)
                advancePpu<false>(prefix);
            else advancePpu(prefix);
            dots -= prefix;
    }
    if (dots) advancePpu(dots);
}




/**
 * @brief Исполняет допустимый MMC5 CPU-префикс в пределах одного временного горизонта.
 * @details Обычный путь сразу материализует прошедшие M2. Retained-epoch оставляет
 * неиспользованные credits и уже исполненный долг тому же owned frame: следующий
 * strict либо scalar шаг расходует остаток без нового admission. MMIO, исчерпание
 * budget и выход FrameScope сохраняют прежние точные барьеры материализации.
 */
unsigned Machine::runMmc5QuietBlock(unsigned remaining) noexcept {
    const unsigned budget=mmc5QuietCpuBudget(remaining,false);
    if(!budget)return 0;
    mmc5ReadQuantum_.ram6000=cartridge_.mmc5_->cpu6000Page();
    cpuReadBudget_=static_cast<std::uint8_t>(budget);
    const auto consumed=NES_COST_QUIET_MMC5(budget);
    // FrameScope и admission уже фиксируют единственного владельца и horizon.
    // Сумма pending+remaining не превосходит исходный bounded grant; новый допуск
    // здесь не создаётся. Даже consumed==0 может передать прежние credits
    // обычному interpreter: он материализует долг до любого MMIO/mapper effect.
    // Полностью исчерпанный grant закрывается сразу, как в контрольном пути.
    if(cpuReadBudget_)return consumed;
    const unsigned clocks=pendingCpuReads_;
    pendingCpuReads_=cpuReadBudget_=0;
    replayMmc5QuietCpuCycles(clocks);
    return consumed;
}

/** @details Начало frame не наследует неиспользованный grant старого владельца. */
Machine::Mmc5ReadQuantumFrameScope::Mmc5ReadQuantumFrameScope(Machine& machine,std::uint64_t deadline) noexcept
    :machine_(machine){
    bool thinMmc5=false;
    thinMmc5=machine_.thinSignalMode()&&!machine_.thinStickyFault_&&machine_.thin_->fault()==ColourFault::None;
    if(machine_.cartridge_.mapper()==5&&(machine_.outputOwner_||thinMmc5)&&!machine_.mmc5ReadQuantum_.active){
        machine_.flushCpuReads();machine_.cpuReadBudget_=0;
        machine_.mmc5ReadQuantum_.deadline=deadline;
        machine_.mmc5ReadQuantum_.ram6000=nullptr;
        machine_.mmc5ReadQuantum_.previewClocks=0;
        machine_.mmc5ReadQuantum_.active=true;owns_=true;
        // Только wiring текущего эксклюзивного вызова: faults, IRQ, clocks,
        // PPU horizon и отображение WRAM не переживают отдельный admission.
        machine_.mmc5ReadQuantum_.frameQualified=thinMmc5||machine_.mmc5QuietFramePrerequisites();
    }
}
/** @details Flush предшествует снятию active, иначе dedicated commit потеряет identity. */
Machine::Mmc5ReadQuantumFrameScope::~Mmc5ReadQuantumFrameScope() noexcept {
    if(owns_){
        machine_.flushCpuReads();machine_.cpuReadBudget_=0;
        machine_.mmc5ReadQuantum_={};
    }
}

/** @details Ни одно из этих полей не меняется CPU/MMIO внутри owned stepFrame. */
bool Machine::mmc5QuietFramePrerequisites() const noexcept {
    return cartridge_.mapper()==5&&region_==Region::Ntsc&&cartridge_.mmc5_&&cartridge_.mmc5Audio_&&
        outputOwner_&&!outputInitializing_&&outputJob_&&!outputSealed_&&
        !videoObserver_&&!videoObservableOnly_&&videoPacket_&&timingPpu_.packet_==videoPacket_&&!timingPpu_.capture_&&
        audioPacket_&&expansionAudio_&&audioPacket_->header().generation==audioGeneration_&&
        audioPacket_->header().sequence==audioSequence_&&audioPacket_->header().region==region_;
}

/** @details Pure сертификат един для B3 и обычного interpreter bus quantum. */
unsigned Machine::mmc5QuietCpuBudget(unsigned remaining,bool insideStep) noexcept {
    // Rejected retry с live credits не отзывает original endpoint.
    // Новое принятое admission заменит его; настоящий flush отзовёт всегда.
    if(thinSignalMode()&&cartridge_.mapper()==5U){
        const auto&p=timingPpu_;const auto*d=cartridge_.mmc5_;
        // Все меняющиеся guards проверяются до CPUahead. Identity не даёт права
        // кешировать fault/IRQ/purePrgReads или горизонты после guest MMIO.
        if(!mmc5ReadQuantum_.active||!mmc5ReadQuantum_.frameQualified||!d||!cartridge_.mmc5Audio_||
           remaining<8U||region_!=Region::Ntsc||thinStickyFault_||thin_->fault()!=ColourFault::None||
           cpu_.inStep_!=insideStep||dmaPending_||dmcStalls_||pendingCpuReads_||cpuReadBudget_||pendingPpuDots_||
           cpu_.nmiPending_||cpu_.nmiReady_||cpu_.irqPending_||cpu_.irqLine_||p.nmiPending_||p.frameReady_||
           p.renderDelay_||p.nmiDelay_||p.packet_||p.capture_||p.trackBusCycles_||p.trackBusAddresses_||p.pendingPixels_||
           p.scanline_<-1||p.scanline_>260||p.dot_>340U||p.renderingEnabled_!=p.renderingNext_||
           d->nameRepeat_>3U||d->cpuIdle_>3U||cartridge_.irqLine()||audioIrqLine()||
           !cartridge_.mmc5Audio_->purePrgReads()||!audioPacket_||!expansionAudio_||audioFault()!=AudioFault::None||
           audioPacket_->header().generation!=audioGeneration_||audioPacket_->header().sequence!=audioSequence_||
           audioPacket_->header().region!=region_||timingApu_.cycles()!=cycles_||ppuRemainder_!=0U||
           thin_->count_==thin_->capacity_||cycles_<thin_->base_.cycle)return 0;
        constexpr unsigned maximum=AudioPacket::maximumMmc5QuietClocks;
        static_assert(maximum<=255U,"Fused grant must fit pending/credits u8 invariant");
        unsigned upper=std::min({remaining,maximum,timingApu_.quietCpuCycles()});
        // Capacity нехватка не отменяет прежний 80grant и не меняет fault.
#if defined(NES_EXPERIMENTAL_MMC5_GRANT_SINGLE_AUDIO_CHECK)
        if(upper<8U)return 0;
        if(!audioPacket_->canAppendMmc5QuietBounded(cycles_,upper,maximum)){
            // Повтор той же pure проверки даст тот же результат. Второй вызов нужен только
            // для действительно уменьшенного предельного интервала.
            if(upper<=80U)return 0;
            upper=80U;
            if(!audioPacket_->canAppendMmc5QuietBounded(cycles_,upper,maximum))return 0;
        }
        if(cycles_>~std::uint64_t{0}-upper||
           cycles_-thin_->base_.cycle>ColourPacket::maximumCycles-upper)return 0;
#else
        if(!audioPacket_->canAppendMmc5QuietBounded(cycles_,upper,maximum))
            upper=std::min({remaining,80U,timingApu_.quietCpuCycles()});
        const bool audioRoom=audioPacket_->canAppendMmc5QuietBounded(cycles_,upper,maximum);
        if(upper<8U||cycles_>~std::uint64_t{0}-upper||!audioRoom||
           cycles_-thin_->base_.cycle>ColourPacket::maximumCycles-upper)return 0;
#endif
        for(const auto*page:cartridge_.prgPages_)if(!page)return 0;
        // Все прежние динамические CPU/APU/DMC/MMIO/audio/lease/page guards уже
        // прошли. RenderingNext==enabled, dot<=340 и repeat<=3 также проверены.
        // <=240M2 даёт<=720dots и<=3normal line events. Произвольный NT/CHR head
        // добавляет<=2fourthNT; counter<=233 исключает 239/endFrame. Palette head
        // остаётся прежнему planner. IRQdisabled допускает pending/compare;
        // enabled требует !pending и compare0 либо вне(counter,counter+5].
        if(upper<=240U&&p.renderingEnabled_&&unsigned(p.scanline_)<=235U&&
           p.busAddress_<0x3000U&&d->inFrame_&&d->ppuReadSinceM2_&&d->cpuIdle_<=1U&&
           d->scanline_<=233U&&(!d->irqEnabled_||(!d->irqPending_&&
           (!d->irqCompare_||d->irqCompare_<=d->scanline_||unsigned(d->irqCompare_)>unsigned(d->scanline_)+5U)))){
            // Нет нового endpoint preview: даже совпавший старый base/count не
            // даёт права reuse. Exact replay построит plan только consumed prefix.
            mmc5ReadQuantum_.previewClocks=0;
            return upper;
        }
#if defined(NES_EXPERIMENTAL_MMC5_INACTIVE_GRANT)
        if(mmc5InactiveGrantEnabled_&&(!p.renderingEnabled_||p.scanline_>=240)){
            const auto grant=mmc5InactiveGrantClocks(p.scanline_,p.dot_,upper);
            if(grant<8U)return 0; // Отказ, как раньше, не отзывает предыдущий endpoint.
            // Из idle3 не возникает IRQ. Actual replay заново строит точный endpoint
            // только для расходованных M2, с прежним idle reset/read/commit протоколом.
            mmc5ReadQuantum_.previewClocks=0;
            return grant;
        }
#endif
        ThinMmc5SignalPlan plan;
        const unsigned safe=planThinMmc5Signals(upper,plan);
        if(safe>=8U){
            mmc5ReadQuantum_.preview=plan;
            mmc5ReadQuantum_.previewBase=cycles_;
            mmc5ReadQuantum_.previewClocks=static_cast<std::uint8_t>(safe);
        }
        return safe>=8U?safe:0U;
    }

    if(thinSignalMode()&&cartridge_.mapper()==5U){
        // На visible9..336 каждые три dots содержат настоящий /RD. После не
        // более двух NT-read следует pattern /RD: fourth-NT и idle3 невозможны.
        // Head обязан быть pattern/nameRepeat0. Младшие pattern-биты не нужны
        // детектору, но loopy/raster, порядокM2/RD и MMIO/vector барьеры точны.
        const auto& p=timingPpu_;const auto* digital=cartridge_.mmc5_;
        if(!mmc5ReadQuantum_.active||!digital||!cartridge_.mmc5Audio_||
           remaining<8U||region_!=Region::Ntsc||thinStickyFault_||thin_->fault()!=ColourFault::None||
           (cpu_.inStep_!=insideStep)||dmaPending_||dmcStalls_||pendingCpuReads_||cpuReadBudget_||pendingPpuDots_||
           cpu_.nmiPending_||cpu_.nmiReady_||cpu_.irqPending_||cpu_.irqLine_||
           p.nmiPending_||p.frameReady_||p.renderDelay_||p.nmiDelay_||
           p.scanline_<0||p.scanline_>=240||p.dot_<9U||p.dot_>=337U||
           !p.renderingEnabled_||!p.renderingNext_||!(p.mask_&0x18U)||
           !digital->inFrame_||digital->nameRepeat_||digital->lastPpuAddress_>=0x2000U||
           !digital->ppuReadSinceM2_||digital->cpuIdle_>1U||
           cartridge_.irqLine()||audioIrqLine()||!cartridge_.mmc5Audio_->purePrgReads()||
           !audioPacket_||!expansionAudio_||audioFault()!=AudioFault::None||
           audioPacket_->header().generation!=audioGeneration_||audioPacket_->header().sequence!=audioSequence_||
           audioPacket_->header().region!=region_||timingApu_.cycles()!=cycles_||ppuRemainder_>4U)return 0;
        unsigned endDot=337U;
        // Decoder выдаёт только кредит, который visible summary может закрыть целиком.
        // После 255 остаётся прежний строгий GQ; Y256/copy257 здесь не группируются.
        if(p.dot_<255U)endDot=255U;
        // Только внутренний sprite interval: copy257 и BG-prefetch321 снаружи.
        // Иначе прежний grant255..337 скалярно поглотит весь новый sprite domain.
        // Tighter admission оставляет реальный M2/Y256/copy257 старому scalar пути.
        if(p.dot_>=255U&&p.dot_<258U)return 0;
        if(p.dot_>=258U&&p.dot_<321U)endDot=321U;
        const unsigned budget=std::min({remaining,80U,(endDot-p.dot_)/3U,timingApu_.quietCpuCycles()});
        if(budget<8U||cycles_>~std::uint64_t{0}-budget||
           !audioPacket_->canAppendMmc5Quiet(cycles_,budget))return 0;
        for(const auto* page:cartridge_.prgPages_)if(!page)return 0;
        return budget;
    }
    // Scope — единственный владелец: ложный frame verdict не становится
    // положительным до следующего stepFrame. Динамика ниже не кэшируется.
    const bool frameQualified=mmc5ReadQuantum_.active;
    if(frameQualified&&!mmc5ReadQuantum_.frameQualified)return 0;
    if((!frameQualified&&(cartridge_.mapper()!=5||region_!=Region::Ntsc))||remaining<8U)return 0;
    const unsigned noRd=timingPpu_.mmc5InplaceNoRdDots();
    const unsigned horizon=noRd?noRd:timingPpu_.mmc5InplaceActiveDots();
    if(horizon<24U)return 0;
    if(
       (!frameQualified&&!mmc5QuietFramePrerequisites())||
       !cartridge_.mmc5Audio_->purePrgReads()||
       dmaPending_||dmcStalls_||pendingCpuReads_||cpuReadBudget_||pendingPpuDots_||deferPpuAllowed_||
       (cpu_.inStep_!=insideStep)||cpu_.nmiPending_||cpu_.nmiReady_||cpu_.irqPending_||cpu_.irqLine_||
       timingPpu_.nmiPending_||timingPpu_.frameReady_||cartridge_.irqLine()||audioIrqLine()||
       !outputFrameExecutable()||videoHasFault()||audioFault()!=AudioFault::None||timingApu_.cycles()!=cycles_)return 0;
    if(PpuColourReplay::isActiveShadow(this))return 0;
    for(const auto* page:cartridge_.prgPages_)if(!page)return 0;
    const unsigned budget=std::min({remaining,80U,horizon/3U,timingApu_.quietCpuCycles()});
    if(budget<8U||ppuRemainder_>4U||cycles_>~std::uint64_t{0}-budget||
       !videoPacket_->canAppendMmc5Quiet(3U*budget)||
       !audioPacket_->canAppendMmc5Quiet(cycles_,budget))return 0;
    return budget;
}
/** @details Все analog DAC timestamps и scalar prefix/tail сохранены из закрытого B3. */
void Machine::replayMmc5QuietCpuCycles(unsigned clocks) noexcept {
    auto& preview=mmc5ReadQuantum_;
    const bool reuse=preview.active&&preview.frameQualified&&!pendingCpuReads_&&!cpuReadBudget_&&preview.previewClocks>=8U&&
        clocks==preview.previewClocks&&cycles_==preview.previewBase&&
        preview.preview.elapsed==clocks;
    // Этот replay расходует право ровно один раз ДО audio/transport callbacks.
    // Partial/MMIO либо другой base всегда идут в прежний exact replan.
    preview.previewClocks=0;
    /**
     * @brief Закрывает только доказанный quiet audio prefix без MMIO/DMC/IRQ внутри.
     * @param amount Уже потреблённые 1..maximumMmc5QuietClocks M2, с AudioPacket credits на весь worst-case.
     * @details Первый segment1 сохраняет pending DAC предыдущей записи; явные
     * timestamps T−1 совпадают со scalar audioPublishExpansion перед tick(T).
     * Используется и прежним exact executor, и новым ThinLarge вариантом.
     */
    const auto replayQuietAudio=[this](unsigned amount,std::uint64_t baseCycle)noexcept{
        unsigned completed=0;
        auto& expansion=*cartridge_.mmc5Audio_;
        while(completed<amount){
            const unsigned segment=completed?std::min(amount-completed,
                static_cast<unsigned>(cartridge_.selectedExpansionOutputCpuCycles())):1U;
            expansion.clockCpu(segment);
            completed+=segment;
            const auto level=cartridge_.selectedExpansionSample();
            if(level!=lastExpansionLevel_){
                static_cast<void>(audioPacket_->addExpansion(baseCycle+completed-1U,level));
                lastExpansionLevel_=level;
            }
        }
        // Strict quiet certificate исключает IRQ/DMC callback внутри advance.
        timingApu_.advanceCpuCycles(amount);
    };

    if(thinSignalMode()&&cartridge_.mapper()==5U){
    if(thinSignalMode()&&cartridge_.mapper()==5U){
        ThinMmc5SignalPlan plan;
        // Retained credits могут закрыть original grant уже вне runQuietBlock.
        // Только полное совпадение base/count повторно использует pure endpoint;
        // partial prefix и idle-before-prefix сохраняют прежнее перепланирование.
        const unsigned planned=reuse?(plan=preview.preview,clocks):planThinMmc5Signals(clocks,plan);
        if(planned==clocks){
            const auto base=cycles_;
            auto&p=timingPpu_;auto&d=*cartridge_.mmc5_;
            /** @brief Материализует exact device cut без clocks/IRQ/transport callbacks. */
            const auto apply=[&](const ThinMmc5SignalPlan&s)noexcept{
                p.v_=s.v;p.busAddress_=s.address;p.dot_=s.dot;p.scanline_=s.row;
                p.evaluationNextDot_=s.evaluation;p.nextTile_=s.nextTile;p.oamAddress_=s.oam;
                p.coarseXSignal_=s.coarse;p.scrollSignal_=s.scroll;p.odd_=s.odd;
                d.lastPpuAddress_=s.last;d.nameRepeat_=s.repeat;d.cpuIdle_=s.idle;d.ppuReadSinceM2_=s.read;
                d.inFrame_=s.inFrame;d.irqPending_=s.pending;d.scanline_=s.scanline;d.splitY_=s.splitY;
                d.tileExtended_=s.extended;d.tileSplit_=s.split;
            };
            ThinMmc5SignalPlan beforeIdle;
            if(plan.idleOffset&&planThinMmc5Signals(plan.idleOffset-1U,beforeIdle)!=plan.idleOffset-1U){
                invalidateThinPpu();return;
            }
            if(plan.idleOffset){
                const unsigned prefix=plan.idleOffset-1U;
                if(prefix)replayQuietAudio(prefix,base);
                cycles_=base+prefix;apply(beforeIdle);
                // Единственный специальный M2: настоящий digital clock и append
                // на текущем pre-PPU cut. Никаких старых/synthetic cut не подставляем.
                ++cycles_;d.clockCpu(1U);
                recordThinEvent(ColourKind::Mmc5IdleReset,cycles_,0,0);
                if(thinStickyFault_)return;
                // Machine cycles уже T; expansion input этогоM2 имеет timestampT-1.
                replayQuietAudio(1U,cycles_-1U);
                if(clocks>plan.idleOffset)replayQuietAudio(clocks-plan.idleOffset,cycles_);
            }else replayQuietAudio(clocks,base);
            cycles_=base+clocks;apply(plan);
            cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
            // Caller уже закрыл actual debt/credits; здесь только законченный device cut.
            // ThinPrefix не заимствует CPU State: законен и при refresh внутри инструкции.
            if(cycles_-thin_->base_.cycle>=512){
                const bool progressPublished=thin_->flush(thinCut());
                if(!progressPublished){thinStickyFault_=true;videoFaultLatch_=VideoFault::InvalidEvent;}
            }
            return;
        }
        // Conservative plan rejection не имеет side effects; старый путь закрывает
        // фактически исполненный CPU prefix без повторной Audio/device mutation.
    }
        // Доказанный CPU prefix уже завершён: никаких MMIO/DMC/IRQ наблюдателей
        // внутри grant. Cart/APU/DAC исполняются накаждомM2, PPUсрез закрывается
        // точным phase summary после них. Любой несоответствующий head — scalar.
        auto& p=timingPpu_;auto* d=cartridge_.mmc5_;
        const unsigned count=clocks*3U;
        bool spriteWindow=false;
        spriteWindow=p.dot_>=258U&&p.dot_<321U;
        const bool visibleWindow=p.dot_>=9U&&p.dot_<255U;
        const unsigned endDot=spriteWindow?321U:255U;
        const bool eligible=clocks>=8U&&clocks<=80U&&region_==Region::Ntsc&&mmc5ReadQuantum_.active&&
            d&&cartridge_.mmc5Audio_&&unsigned(p.scanline_)<240U&&(visibleWindow||spriteWindow)&&count<=endDot-p.dot_&&
            p.renderingEnabled_&&p.renderingNext_&&(p.mask_&0x18U)&&
            !(p.renderDelay_|p.nmiDelay_)&&!p.frameReady_&&!p.nmiPending_&&
            // Только NT/pattern initial drive: palette >=3F00 не создаёт mapper /RD.
            // Ненормальный head после CPU2007 сохраняет прежний M2/PPU scalar порядок.
            p.busAddress_<0x3000U&&!p.trackBusCycles_&&!p.trackBusAddresses_&&!p.packet_&&!p.capture_&&
            !thinStickyFault_&&thin_->fault()==ColourFault::None&&thin_->count_<thin_->capacity_&&
            !videoObserver_&&!pendingCpuReads_&&!cpuReadBudget_&&!pendingPpuDots_&&ppuRemainder_<5U&&
            !dmaPending_&&!dmcStalls_&&!cpu_.nmiPending_&&!cpu_.nmiReady_&&!cpu_.irqPending_&&!cpu_.irqLine_&&
            !cartridge_.irqLine()&&!audioIrqLine()&&audioFault()==AudioFault::None&&
            d->inFrame_&&!d->nameRepeat_&&d->lastPpuAddress_<0x2000U&&d->ppuReadSinceM2_&&d->cpuIdle_<=1U&&
            cartridge_.mmc5Audio_->purePrgReads()&&timingApu_.cycles()==cycles_&&timingApu_.quietCpuCycles()>=clocks&&
            audioPacket_&&expansionAudio_&&audioPacket_->canAppendMmc5Quiet(cycles_,clocks);
        if(eligible){
            unsigned v=p.v_,address=p.busAddress_,nextTile=p.nextTile_,repeat=d->nameRepeat_,last=d->lastPpuAddress_;
            bool coarse=p.coarseXSignal_,valid=true;unsigned dot=p.dot_,remaining=count,edges=0;
            /** @brief Моделирует один настоящий RD; equality Name/Attr может дать 2, third запрещён proof. */
            const auto rd=[&]()noexcept{
                if(repeat==3U){valid=false;return;}
                repeat=address>=0x2000U&&address<0x3000U?(address==last?repeat+1U:1U):0U;last=address;
                if(repeat>2U)valid=false;
            };
            /** @brief Exact scalar phase только bounded prefix/suffix; не меняет живой PPU до complete proof. */
            const auto edge=[&]()noexcept{
                if(spriteWindow)switch((dot-257U)&7U){
                case 0:case 2:address=0x2000U|(v&0x0fffU);break;
                case 1:case 5:case 7:rd();break;
                case 3:rd();address=0U;break;
                case 4:address=0U;break;
                case 6:address=8U;break;
                }
                else switch((dot-1U)&7U){
                case 0:address=0x2000U|(v&0x0fffU);break;
                case 1:rd();nextTile=0;break;
                case 2:address=0x23c0U|(v&0x0c00U)|((v>>4U)&0x38U)|((v>>2U)&7U);break;
                case 3:rd();address=((p.control_&0x10U)?0x1000U:0U)+((v>>12U)&7U);break;
                case 4:address=((p.control_&0x10U)?0x1000U:0U)+((v>>12U)&7U);break;
                case 5:rd();break;
                case 6:coarse=true;address=(address+8U)&0x3fffU;break;
                case 7:rd();if(coarse){if((v&31U)==31U)v=(v&~31U)^0x400U;else ++v;}coarse=false;break;
                }
                ++dot;--remaining;++edges;
            };
            while(remaining&&((dot-1U)&7U))edge();
            const unsigned tiles=remaining/8U;
            if(tiles){
                if(repeat)valid=false;
                if(spriteWindow){
                    // Два одинаковых NT, затем RD0/RD8: repeat2 никогда не 3.
                    // OAM-elision гарантирует pattern0/8; v/nextTile/coarse не меняются.
                    address=8U;repeat=0;last=8U;
                }else{
                    // Каждый BG tile имеет NT/Attr и два pattern RD, не fourthNT.
                    const unsigned x=(v&31U)+tiles;
                    v=(v&~0x041fU)|(x&31U)|((v^((x>>5U)<<10U))&0x0400U);
                    address=((p.control_&0x10U)?0x1000U:0U)+((v>>12U)&7U)+8U;
                    repeat=0;last=address;nextTile=0;coarse=false;
                }
                dot+=tiles*8U;remaining-=tiles*8U;
            }
            while(remaining)edge();
            if(valid){
                // PPU/digital proof уже завершён, CPU стоит: audio не наблюдает
                // промежуточные NT state. Quiet horizon исключает последний event M2.
                replayQuietAudio(clocks,cycles_);
                // clockCpu(1) с Ptrue только потребляет P и даёт I0. Каждый
                // доказанный 3dot interval восстанавливает Ptrue; idle3/fourthNT
                // невозможны. Других digital clock counters в Mmc5 нет.
                cycles_+=clocks;d->cpuIdle_=0;d->ppuReadSinceM2_=true;
                // CPU не наблюдает середину prefix, все IRQ sources неизменныfalse.
                // setIrqLine(false) идемпотентен при входных pending/linefalse.
                cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
                p.v_=static_cast<std::uint16_t>(v);p.busAddress_=static_cast<std::uint16_t>(address);
                p.nextTile_=static_cast<std::uint8_t>(nextTile);p.coarseXSignal_=coarse;p.dot_=static_cast<std::uint16_t>(dot);
                if(spriteWindow)p.oamAddress_=0;
                d->nameRepeat_=static_cast<std::uint8_t>(repeat);d->lastPpuAddress_=static_cast<std::uint16_t>(last);
                d->ppuReadSinceM2_=true;d->cpuIdle_=0;
                return;
            }
        }
        for(unsigned n=0;n<clocks;++n)clockCpuCycle();
        return;
    }
    // Первый clock сохраняет pending DAC после предшествующей CPU записи.
    // Явный timestamp остаётся T-1, пока audio/APU временно впереди PPU.
    {
    replayQuietAudio(clocks,cycles_);
    }
    // Только pure CPU reads произошли после grant. MMIO/DMA/DMC всегда flush до
    // мутации; rebase не имеет права молча вернуться к старому optimistic CPU.
    if(timingPpu_.mmc5InplaceNoRdDots()>=clocks*3U){
        cartridge_.mmc5_->clockCpu(clocks);cycles_+=clocks;advancePpu<false>(3U*clocks);
    }else{timingPpu_.replayMmc5InplaceDots(3U*clocks);cycles_+=clocks;}
    if(videoTakeNmi())cpu_.requestNmi();
    cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
}

/**
 * @brief Продвигает один логический такт CPU и соответствующие устройства.
 * @details Для PAL сохраняет дробный остаток отношения 16/5, для NTSC три точки PPU.
 */
void Machine::clockCpuCycle() noexcept {
    if(thin_&&thin_->fault()!=ColourFault::None)return;
    if(PpuColourReplay::clockTrapped(this))return;
    // Освободить место для максимум одного DMC-vector события до текущего M2.
    // В callback PPU ещё на прошлом cut; capacity-flush там был бы неверным.
    if(thin_&&cartridge_.mapper()==5U&&thin_->count_==thin_->capacity_)
        if(!thin_->flush(thinCut())){thinStickyFault_=true;return;}
    // Этот owner-local снимок предшествует настоящему digital M2, не прогнозирует его.
    const auto* coarseMapper=thin_&&cartridge_.mapper()==5U?mmc5QuietMapper():nullptr;
    const bool beforeIdle3=coarseMapper&&coarseMapper->cpuIdle_<3U;
    ++cycles_;
    {
    cartridge_.cpuCycle();
    // Один реальный pre-PPU timeout edge. Ёмкость зарезервирована до M2;
    // callback DMC может лишь повысить этот ещё не опубликованный slot до composite.
    if(beforeIdle3&&coarseMapper->cpuIdle_==3U){
        recordThinEvent(ColourKind::Mmc5IdleReset,cycles_,0,0);
    }
    // Cold-arm externalFM начинается с нулевого placeholder; физический DAC полностью принадлежит Main.
    if(Cartridge::hasExpansionAudioMapper(cartridge_.mapper())&&!cartridge_.externalVrc7Enabled()){
        audioPublishExpansion(cartridge_.selectedExpansionSample());
    }
    }
    audioTick();
    {
    ppuRemainder_ = static_cast<std::uint8_t>(ppuRemainder_ + (region_ == Region::Pal ? 16 : 15));
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
            flushPpu();
            advancePpu(ppuDots);
        }
    }
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

/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::memoryRead(std::uint16_t address) noexcept {
    if (address < 0x2000) return ram_[address & 0x7ff];
    if (address >= 0x8000) {
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
std::uint8_t Machine::memoryReadIo(std::uint16_t address) noexcept {
    if(thin_&&thin_->fault()!=ColourFault::None)return openBus_;
    if (
        !thin_&&
        address < 0x4000U && (address & 7U) == 2U && splitVideoEnabled() &&
        cartridge_.loaded() &&
        (cartridge_.mapper()==0 || cartridge_.mapper()==1 || cartridge_.mapper()==2 ||
         cartridge_.mapper()==3 || cartridge_.mapper()==7 || cartridge_.mapper()==71 ||
         (cartridge_.mapper()==4 && (!cartridge_.irqEnabled_ || cartridge_.irqPending_)))
        ) {
        std::uint8_t cached;
        if (timingPpu_.tryReadDeferredStatus(pendingPpuDots_, cached)) {
        recordColourEvent(ColourKind::RetainedRead,cycles_,address&7U,cached);
            return cached;
        }
    }
    // Для 69/85 настоящий read-M2 уже исполнен readSlow. CPU-cart read не меняет PPU,
    // IRQ/банки и защёлки; CPU open bus обновляет прежний readSlow после возврата.
    if(outputOwner_&&!thin_&&!videoObserver_&&!colourCapture_&&!timingPpu_.capture_&&
       address>=0x4020U&&cartridge_.loaded()&&(cartridge_.mapper()==69U||cartridge_.mapper()==85U)){
        const auto value=cartridge_.cpuRead(address,openBus_);
        refreshPpuDeferBudget(); // Прежний bookkeeping сохранён; исключён только PPU flush.
        return value;
    }
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


/** @details У этих плат $6000..$7FFF — RAM, открытая шина либо const PRG Legacy.
 * Доступ не тактирует устройство и не переключает mapping. Legacy CPU/write IRQ
 * остаются отдельными настоящими стробами; существующий budget уже ограничивает
 * IRQ69/85 и selected64 expansion DAC остаются границами прежнего budget;
 * clocked IRQ, native APU/DMC и PPU-наблюдения. MMC5 PCM имеет другой контракт.
 */
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
bool Machine::canRetainPrgRamWindow() const noexcept {
    if(!cpu_.inStep_||!cartridge_.loaded())return false;
    switch(cartridge_.mapper()){
    case 0:case 1:case 2:case 3:case 4:case 7:case 69:case 71:case 85:case 160:break;
    default:return false;
    }
    if(videoObserver_)return false;
    if(colourCapture_)return false;
    if(thin_)return false;
    return true;
}
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/**
 * @brief Пересчитывает безопасный горизонт отложенных CPU-чтений.
 * @details Граница определяется ближайшим наблюдаемым событием PPU/APU/картриджа; значение не даёт права пропускать сами гостевые такты.
 */
void Machine::refreshCpuReadBudget() noexcept {
    cpuReadBudget_ = 0;
    if(cartridge_.mapper()==5){
        if(!mmc5ReadQuantum_.active||!cpu_.inStep_||cycles_>=mmc5ReadQuantum_.deadline)return;
        const auto room=mmc5ReadQuantum_.deadline-cycles_;
        if(room<=1U)return;
        const unsigned remaining=static_cast<unsigned>(std::min<std::uint64_t>(room-1U,255U));
        cpuReadBudget_=static_cast<std::uint8_t>(mmc5QuietCpuBudget(remaining,true));
        if(cpuReadBudget_)mmc5ReadQuantum_.ram6000=cartridge_.mmc5_->cpu6000Page();
        return;
    }
    if (!deferPpuAllowed_ || pendingPpuDots_ > ppuDeferBudget_) return;
    const unsigned dots = ppuDeferBudget_ - pendingPpuDots_;
    unsigned cycles = region_ == Region::Pal
        ? (5U * dots + 4U - ppuRemainder_) >> 4U
        : dots / 3U;
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
    if(thin_&&thin_->fault()!=ColourFault::None)return;
    if(PpuColourReplay::clockTrapped(this))return;
    const unsigned count = pendingCpuReads_;
    pendingCpuReads_ = cpuReadBudget_ = 0;
    if (!count) return;
    if(mmc5ReadQuantum_.active&&cartridge_.mapper()==5){
        replayMmc5QuietCpuCycles(count);
        return;
    }
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


/**
 * @brief Выполняет настоящий медленный read-M2 с текущим отображением памяти и open bus.
 * @details Проверенное WRAM-окно может оставить чтение в существующем quiet-бюджете. Остальные пути сначала материализуют долг, DMA/DMC и необходимые PPU-наблюдения; кешированное значение вместо MMIO не подставляется.
 * @param address Гостевой адрес 6502; возвращённый байт становится текущим CPU open bus по обычному шинному контракту.
 */
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
std::uint8_t Machine::readSlow(std::uint16_t address) noexcept {
#if defined(NES_EXPERIMENTAL_RETAIN_DISCRETE_STATUS)
    if (address >= 0x2000U && address < 0x4000U && (address & 7U) == 2U &&
        cpuReadBudget_ && !dmaPending_ && !dmcStalls_ && region_ == Region::Ntsc &&
        (cartridge_.mapper_ == 7U || cartridge_.mapper_ == 71U) &&
        outputOwner_ && !thin_ && !videoObserver_ && !colourCapture_ && !timingPpu_.capture_) {
        // STATUS должен увидеть все точки до этого M2 включительно. Уже выданный
        // бюджет ограничен IRQ/NMI и DMC, а классификатор проверяет эффекты самого STATUS.
        const unsigned dots = pendingPpuDots_ + 3U * (pendingCpuReads_ + 1U);
        std::uint8_t value;
        if (dots <= ppuDeferBudget_ && timingPpu_.tryReadDeferredStatus(dots, value)) {
            ++pendingCpuReads_;
            --cpuReadBudget_;
            openBus_ = value;
            return value;
        }
    }
#endif
    const bool prgRamWindow=address>=0x6000U&&address<0x8000U&&canRetainPrgRamWindow();
    if(prgRamWindow&&cpuReadBudget_&&!dmaPending_&&!dmcStalls_){
        // Это тот же настоящий read-M2. CPU-visible RAM/open bus выбираются на
        // каждом доступе; между ними разрешены обычные записи и RMW old/new.
        ++pendingCpuReads_;--cpuReadBudget_;
        const auto value=cartridge_.cpuRead(address,openBus_);
        openBus_=value;
        return value;
    }
    const bool ordinary = address < 0x2000 || address >= 0x8000
        ||prgRamWindow
        ||(mmc5ReadQuantum_.active&&address>=0x6000U&&address<0x8000U)
        ;
    flushCpuReads(); cpuReadBudget_ = 0;
    // RDY останавливает первое следующее чтение CPU, включая выборку опкода. Оставить
    // CPU внутри уже начатой инструкции на время DMA: пришедший в DMA IRQ
    // не должен задним числом заменить начатую инструкцию.
    if (dmaPending_) dma();
    drainDmcStalls(); // DMA останавливает CPU на чтениях, но не в такте записи.
    clockCpuCycle();
    const auto value = memoryRead(address);
    if(cartridge_.mapper()==5 && address<0x4020)cartridge_.observeMmc5Read(address,value);
    if(thin_&&cartridge_.mapper()==5U&&(address==0xfffaU||address==0xfffbU||address==0x2002U||address==0x5204U))
        recordThinEvent(ColourKind::Mmc5CpuStrobe,cycles_,address,value);
    if (address != 0x4015) openBus_ = value;
    // Чтения RAM и PRG не имеют эффектов устройства: clockCpuCycle уже
    // предоставил эти точные линии. I/O может подтвердить IRQ или погасить долг PPU.
    if ((address >= 0x2000 && address < 0x8000) || cartridge_.mapper()==5)
        cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
    if (ordinary && !dmcStalls_ && !dmaPending_) refreshCpuReadBudget();
    return value;
}

/**
 * @brief Проводит смену PRG 7/71 без закрытия тихого участка CPU и PPU.
 * @details В уже выданном интервале отсутствуют DMC-чтения из PRG и фронты IRQ/NMI.
 * AxROM проверяет итоговую CIRAM-защёлку после конфликта шины, Camerica допускает только PRG-регистр.
 * Следующее чтение CPU увидит новый банк сразу; прежний CPU open bus хранит исходный байт записи.
 */
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
bool Machine::tryRetainDiscretePrgWrite(std::uint16_t address, std::uint8_t value) noexcept {
    if (!cpu_.inStep_ || !outputOwner_ || thin_ || videoObserver_ || colourCapture_ || timingPpu_.capture_)
        return false;
    if (cartridge_.mapper_ == 7U) {
        const auto latch = cartridge_.axromLatchValue(address, value);
        if (!cartridge_.fourScreen_ && ((cartridge_.bank_ ^ latch) & 0x10U)) return false;
        cartridge_.writeAxromLatch(latch);
    } else {
        if (address < 0xc000U) return false;
        cartridge_.cpuWrite(address, value, cpuCycles() + 1U);
    }
    ++pendingCpuReads_;
    --cpuReadBudget_;
    openBus_ = value;
    cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
    return true;
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::write(std::uint16_t address, std::uint8_t value) noexcept {
    if(thin_&&thin_->fault()!=ColourFault::None)return;
    if(address>=0x8000U&&cpuReadBudget_&&!dmaPending_&&!dmcStalls_&&
       (cartridge_.mapper_==7U||cartridge_.mapper_==71U)&&tryRetainDiscretePrgWrite(address,value))return;
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
        recordColourEvent(ColourKind::CartWrite,writeCycle,address,value);
        if(thin_&&!thinSignalMode())recordThinEvent(ColourKind::CartWrite,writeCycle,address,value);
        cpu_.setIrqLine(cartridge_.irqLine()||audioIrqLine());
        return;
    }
    if (address < 0x2000 && cpuReadBudget_ && !dmaPending_ && !dmcStalls_) {
        ++pendingCpuReads_; --cpuReadBudget_;
        // RAM видна на исходном шинном такте. IRQ Legacy, тактируемые записями,
        // независимы от отложенных CPU-тактов и по-прежнему исполняются на каждой записи.
        cartridge_.cpuWriteStrobe();
        openBus_ = value; ram_[address & 0x7ff] = value;
        cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
        return;
    }
    flushCpuReads(); cpuReadBudget_ = 0;
    clockCpuCycle();
    if (address >= 0x2000) flushPpu();
    cartridge_.cpuWriteStrobe();
    if(cartridge_.mapper()==5 && address<0x4020)cartridge_.observeMmc5Write(address,value);
    if(thin_&&cartridge_.mapper()==5U&&(address==0x2000U||address==0x2001U||address==0x4014U))
        recordThinEvent(ColourKind::Mmc5SignalWrite,cycles_,address,value);
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
        const unsigned spanTarget=cartridge_.mapper()==5U&&cartridge_.mmc5_?cartridge_.mmc5_->cpuExRamWriteTarget(address):~0U;
        // Только текущая полная writable WRAM69/85: store не меняет PPU mapping.
        // Grant не переносится между вызовами; protected/ROM/open bus/5/160 остаются прежними.
        const bool inertWram=address>=0x6000U&&address<0x8000U&&cartridge_.quietWritableWram6000().data;
        const auto mappingMapper=cartridge_.mapper();
        // Эти модели меняют CHR только через refreshChrPages; флаг лишь читается,
        // его единственный acknowledge остаётся у успешного prepareVideoBgView.
        const bool trackedPpuMapping=address>=0x8000U&&cartridge_.loaded()&&(mappingMapper==4U||mappingMapper==7U||mappingMapper==69U||mappingMapper==71U||mappingMapper==85U);
        const auto priorMirroring=trackedPpuMapping?cartridge_.mirroring():Mirroring::Horizontal;
        cartridge_.cpuWrite(address, value, cycles_);
        if(spanTarget!=~0U&&videoPacket_)static_cast<void>(videoPacket_->addMmc5PhysicalWrite(2048U+spanTarget,value));
        recordColourEvent(ColourKind::CartWrite,cycles_,address,value);
        if(thin_)recordThinEvent(ColourKind::CartWrite,cycles_,address,value);
        // Flush/M2/MMIO/IRQ и сами записи выполнены выше без изменений. Только
        // доказанно прежние CHR+mirroring не требуют повторной инвалидации view.
        if(!inertWram){
            if(!trackedPpuMapping||priorMirroring!=cartridge_.mirroring()||cartridge_.chrMappingDirty_){
                refreshNametableMapping();videoBgViewDirty_=true;
            }
        }
    }
    if (address >= 0x2000) refreshPpuDeferBudget();
    cpu_.setIrqLine(cartridge_.irqLine() || audioIrqLine());
}

/**
 * @brief Выполняет OAM DMA с исходными чередующимися чтениями и записями.
 * @details Сохраняет остановку RDY, начальный halt и выравнивание по CPU-фазе; быстрый blank/RAM-путь допустим только при доказанных условиях.
 */
void Machine::dma() noexcept {
    dmaPending_ = false;
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
        if(wires&&blank>=unsigned(pendingPpuDots_)+dots&&
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
    // Один такт остановки и выравнивание чередующихся фаз чтения/записи CPU.
    const bool alignment = (cycles_ & 1) != 0;
    clockCpuCycle();
    if (alignment) clockCpuCycle();
    for (unsigned i = 0; i < 256; ++i) {
        drainDmcStalls();
        clockCpuCycle();
        const auto address = static_cast<std::uint16_t>((dmaPage_ << 8) | i);
        const auto value = memoryRead(address);
        // OAMDMA тоже делает настоящее чтение PPUSTATUS при исходной странице 20.
        if(cartridge_.mapper()==5 && address<0x4020)cartridge_.observeMmc5Read(address,value);
        clockCpuCycle();
        flushPpu();
        videoDmaWrite(value);
        refreshPpuDeferBudget();
    }
}

/**
 * @brief Исполняет одну инструкцию с теми же шинными побочными эффектами, что и кадровый путь.
 * @details Гостевое время машины может включать задержки DMA сверх собственных тактов инструкции.
 */
void Machine::stepInstruction() noexcept {
    // Live staged audio уже привязан к опубликованному commit: новый clock/event до finalize/joined abandon запрещён.
    if(thin_&&(thin_->audioStageOwner_||thin_->audioStageState_!=AudioStagePhase::Empty))return;
    if(mmc5ReadQuantum_.active){flushCpuReads();cpuReadBudget_=0;}
    if(outputOwner_&&!outputFrameExecutable())return;
    if(videoHasFault())return;
    if (audioFault() != AudioFault::None) return;
    static_cast<void>(NES_COST_CPU_STEP());

    if(mmc5ReadQuantum_.active){flushCpuReads();cpuReadBudget_=0;}
}

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


/** @details Допуск определяется моделью аппаратуры и реальным владельцем; одних совпавших bytes недостаточно. */
bool Machine::pureStatusPollContract() const noexcept {
    constexpr unsigned required = Cartridge::PureCpuPrgRead | Cartridge::CpuWritesOnlyPrgMapping |
        Cartridge::NoPpuReadLatch | Cartridge::NoCpuClockEvent | Cartridge::NoCpuWriteStrobe;
    if ((cartridge_.statusPollCapabilities() & required) != required ||
        cartridge_.hasPpuReadSideEffects() || !cartridge_.cpuWriteStrobeIsNoop() ||
        cartridge_.cpuIrqQuietCycles() != ~0U) {
        return false;
    }
    if (&timingPpu_.bus_ != this || timingPpu_.capture_ || videoObserver_) {
        return false;
    }
    if (colourCapture_) { return false; }
    if (videoObservableOnly_) return true;
    if (outputOwner_ && outputFrameExecutable()) return true;
    return false;
}
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
    if (thin_ || cpuReadBudget_ < 14U) return 0;
    const unsigned base = cpu_.state_.pc;
    const bool shortPoll = branchPc == base + 3U;
    const bool pageCross = (base & 0xFF00U) != ((base + (shortPoll ? 5U : 7U)) & 0xFF00U);
    if (base < 0x8000U || base > (shortPoll ? 0xFFFAU : 0xFFF8U) ||
        branchCycles != 3U + unsigned(pageCross && shortPoll) ||
        !splitVideoEnabled() || !deferPpuAllowed_ || region_ != Region::Ntsc ||
        !cartridge_.loaded() || videoFrameReady() || cpu_.inStep_ || cpu_.state_.jammed ||
        cpu_.nmiReady_ || cpu_.nmiPending_ || cpu_.irqPending_ ||
        (cpu_.irqLine_ && !(cpu_.state_.p & 4U)) || dmaPending_ || dmcStalls_) return 0;
    const auto mapper = cartridge_.mapper();
    if (mapper == 5U) return 0; // MMC5 владеет собственным временным и PRG-контрактом.
    const bool discrete = mapper == 7U || mapper == 71U;
    if (shortPoll) {
        if (cpu_.baseAddress_ != 0x2002U) return 0;
    } else if (branchPc != base + 5U || pageCross || (mapper != 2U && !discrete) ||
               cpu_.state_.a || (cpu_.state_.p & 0x82U) != 2U || cpu_.baseAddress_ != base + 4U) {
        return 0;
    }
    // Границы уже выданного quiet-бюджета проверяются до чтения байтов кода.
    const unsigned loopCycles = shortPoll ? (pageCross ? 8U : 7U) : 9U;
    const unsigned heldDots = pendingPpuDots_ + 3U * pendingCpuReads_;
    if (heldDots >= ppuDeferBudget_) return 0;
    const auto now = cpuCycles();
    const unsigned leastCycles = 2U * loopCycles;
    if (now >= deadline || deadline - now < leastCycles ||
        cpuReadBudget_ < leastCycles || 255U - pendingCpuReads_ < leastCycles ||
        heldDots + (3U * leastCycles) > ppuDeferBudget_) return 0;

    // Выше проверены cartridge.loaded(), адреса PRG и отсутствие MMC5.
    // Для таких чтений Cartridge::cpuRead использует те же текущие прямые страницы.
    const auto* const* pages = quietCpuReadPages();
    const auto peek = [pages](unsigned address) noexcept {
        return pages[(address >> 13U) & 3U][address & 8191U];
    };
    bool allowLatchedSpriteZero = false;
    if (shortPoll) {
        const auto read = peek(base);
        const auto branch = peek(base + 3U);
        const bool vblankMapper = discrete || mapper == 1U;
        const bool bit = read == 0x2CU &&
            (branch == 0x50U || (vblankMapper && branch == 0x10U) || (discrete && branch == 0xF0U));
        const bool lda = read == 0xADU && branch == 0x10U; // LDA STATUS; BPL
        if ((!bit && !lda) || peek(base + 1U) != 0x02U ||
            peek(base + 2U) != 0x20U || peek(base + 4U) != 0xFBU) return 0;
        if ((lda || mapper != 2U || pageCross) && !pureStatusPollContract()) return 0;
        allowLatchedSpriteZero = vblankMapper && branch == 0x10U;

        // Последнее физическое чтение уже установило наблюдаемый байт STATUS.
        // Повтор допустим только при неизменных A и затронутых флагах.
        const auto value = timingPpu_.openBus_;
        if (lda) {
            const unsigned flags = value ? 0U : 2U;
            if ((value & 0x80U) || cpu_.state_.a != value ||
                (cpu_.state_.p & 0x82U) != flags) return 0;
        } else {
            const unsigned flags = (value & 0xC0U) | ((cpu_.state_.a & value) ? 0U : 2U);
            // BEQ проверяет пересечение A и байта STATUS через Z; BPL/BVC проверяют N/V.
            const bool taken = branch == 0xF0U ? (flags & 2U) != 0 :
                (flags & (branch == 0x10U ? 0x80U : 0x40U)) == 0;
            if (!taken || (cpu_.state_.p & 0xC2U) != flags) return 0;
        }
        // При пересечении страницы второе фиктивное чтение исправляет PCH.
        const unsigned finalRead = pageCross ? ((base + 5U) & 0xFF00U) | (base & 0xFFU) : base + 5U;
        if (openBus_ != peek(finalRead)) return 0;
    } else {
        if (discrete && !pureStatusPollContract()) return 0;
        // Неизменяемые байты читаются после последнего переключения банка.
        constexpr std::uint8_t bytes[] = {0xAD,0x02,0x20,0x29,0x40,0xF0,0xF9};
        for (unsigned i = 0; i < sizeof(bytes); ++i) if (peek(base + i) != bytes[i]) return 0;
        if (openBus_ != peek(base + 7U)) return 0;
    }
    if (videoHasFault()) return 0;
    unsigned available = std::min<unsigned>(cpuReadBudget_, 255U - pendingCpuReads_);
    available = static_cast<unsigned>(std::min<std::uint64_t>(available, deadline - now));
    if (audioFault() != AudioFault::None) return 0;
    if (audioPacket_) {
        const auto origin = audioPacket_->header().baseCycle;
        if (now < origin || now - origin > 65535U) return 0;
        available = static_cast<unsigned>(std::min<std::uint64_t>(available, 65535U - (now - origin)));
    }
    unsigned iterations = available / loopCycles;
    if (iterations < 2U) return 0;
    if (discrete) {
        const unsigned stepDots = 3U * loopCycles;
        const unsigned limit = std::min<unsigned>(ppuDeferBudget_, heldDots + stepDots * iterations);
        const unsigned horizon = timingPpu_.repeatStatusPollDots(limit, allowLatchedSpriteZero);
        if (horizon <= heldDots) return 0;
        iterations = (horizon - heldDots) / stepDots;
    } else {
        const auto repeatable = [this, heldDots, loopCycles, allowLatchedSpriteZero](unsigned count) noexcept {
            const unsigned dots = heldDots + 3U * count * loopCycles;
            return dots <= ppuDeferBudget_ && timingPpu_.canRepeatStatusPoll(dots, allowLatchedSpriteZero);
        };
        iterations = statuspollsearch::maximumIterations(iterations, repeatable);
    }
    if (iterations < 2) return 0;
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

/**
 * @brief Выполняет внутренний цикл кадра с границами CPU, PPU, APU и DMA.
 * @details Объединённые интервалы допускаются только в пределах уже доказанного тихого горизонта; MMIO, ошибка и готовность кадра остаются обязательными границами.
 * @return Вычисленное значение или состояние типа RunStatus согласно описанному контракту.
 */
RunStatus Machine::stepFrameImpl() noexcept {
    if(outputOwner_&&!outputFrameExecutable())return RunStatus::VideoFault;
    if (!cartridge_.loaded()) return RunStatus::NoCartridge;
    // Хвост предыдущей инструкции может содержать отложенные аудиособытия. Завершить
    // PCM старого кадра перед его отбрасыванием даже без публичных наблюдателей.
    flushCpuReads();
    videoClearFrameReady();
    audioClearSamples();
    if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
    if(videoHasFault())return RunStatus::VideoFault;
    const auto deadline = cpuCycles() + 40000;
    const auto mmc5FrameOkay=[this]()noexcept{
        if(thinSignalMode())return !thinStickyFault_&&thin_->fault()==ColourFault::None;
        return outputFrameExecutable();
    };
    Mmc5ReadQuantumFrameScope readQuantumScope(*this,deadline);
    // Legacy-блоки сохраняют каждый значимый CPU-строб записи на скалярной
    // шине. Неизменяемая политика блока разрешает сохраняющие состояние записи RAM.
    const bool quietBlocksAllowed = true;
    std::uint16_t previousPc = 0;
    unsigned previousCycles = 0;
    bool havePrevious = false;
    constexpr bool progressAtHead=false;
    /**
     * @brief Публикует только уже исполненный CPU/PPU cut, одним прежним body.
     * @param early Для раннего ThinM5 checkpoint отзывает неиспользованные credits.
     * @param fault Получает прежний Audio/Video отказ; false не разрешает output.
     */
    const auto publishThinProgress=[&](bool early,RunStatus&fault)noexcept{
        if(thin_&&cpuCycles()-thin_->base_.cycle>=512){
            observeCpuReads();
            if(early)cpuReadBudget_=0;
            flushPpu();
            if(audioFault()!=AudioFault::None){fault=RunStatus::AudioFault;return false;}
            if(videoHasFault()){fault=RunStatus::VideoFault;return false;}
            if(!thin_->flush(thinCut())){thinStickyFault_=true;videoFaultLatch_=VideoFault::InvalidEvent;fault=RunStatus::VideoFault;return false;}
        }
        return true;
    };
    while (!videoFrameReady() && cpuCycles() < deadline) {
        if(progressAtHead){
            RunStatus progressFault=RunStatus::VideoFault;
            if(!publishThinProgress(true,progressFault))return progressFault;
            // Материализация debt могла достичь кадрового фронта: не запускать следующий CPU шаг.
            if(videoFrameReady())break;
        }
        // Положительный budget, включая pending0, не передаётся generic quiet/idle helpers.
        if(mmc5ReadQuantum_.active&&(pendingCpuReads_||cpuReadBudget_)){
            havePrevious=false;
            // Используем только остаток уже выданного GQ: pending+budget не превышает исходный bounded grant.
            // Новый admission и generic quiet здесь запрещены. Неподдержанный opcode
            // остаётся обычному interpreter с тем же dedicated commit/flush контрактом.
            unsigned decoded=0;
            if(cpuReadBudget_>=8U)decoded=NES_COST_QUIET_MMC5(cpuReadBudget_);
            if(!decoded)static_cast<void>(NES_COST_CPU_STEP());
            if(!mmc5FrameOkay()||videoHasFault())return RunStatus::VideoFault;
            if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
            if(cpu_.state_.jammed)return RunStatus::CpuJammed;
            continue;
        }
        if(!progressAtHead){
            RunStatus progressFault=RunStatus::VideoFault;
            if(!publishThinProgress(false,progressFault))return progressFault;
            if(videoFrameReady())break;
        }
        if(cartridge_.mapper()==5){
            const auto remaining=deadline-cpuCycles();
            if(remaining>=8U&&runMmc5QuietBlock(static_cast<unsigned>(std::min<std::uint64_t>(remaining,255U)))){
                havePrevious=false;
                if(videoHasFault())return RunStatus::VideoFault;
                if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
                continue;
            }
        }
        if (quietBlocksAllowed && cpuReadBudget_ >= 8
            &&!mmc5ReadQuantum_.active
            ) {
            const auto remaining = deadline - cpuCycles();
            if (remaining >= 8 && NES_COST_QUIET_LEGACY(static_cast<unsigned>(std::min<std::uint64_t>(remaining, 255U)))) {
                // Блок может менять RAM и регистры между скалярными шагами.
                havePrevious = false;
                if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
    if(videoHasFault())return RunStatus::VideoFault;
                continue;
            }
        }
        const auto beforePc = cpu_.state_.pc;
        const auto instructionCycles = NES_COST_CPU_STEP();
        if(mmc5ReadQuantum_.active&&(pendingCpuReads_||cpuReadBudget_)){
            havePrevious=false;
            if(!mmc5FrameOkay()||videoHasFault())return RunStatus::VideoFault;
            if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
            if(cpu_.state_.jammed)return RunStatus::CpuJammed;
            continue;
        }
    if(videoHasFault())return RunStatus::VideoFault;

        if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
        const unsigned backwards = static_cast<std::uint16_t>(beforePc - cpu_.state_.pc);
        if ((backwards == 3U || backwards == 5U) && collapseStatusPollLoop(beforePc, instructionCycles, deadline)) {
            if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
        }
        if (backwards >= 1 && backwards <= 3 && havePrevious && previousPc == cpu_.state_.pc) {
            collapseIdleLoop(previousPc, beforePc, previousCycles, instructionCycles, deadline);
            // Отложенное CPU-время может пересечь предел timestamp пакета даже
            // без новой записи ввода. Idle-helper не создаёт PPU-событий,
            // но эта аудиопроверка остаётся настоящей границей.
            if (audioFault() != AudioFault::None) return RunStatus::AudioFault;
        }
        previousPc = beforePc; previousCycles = instructionCycles; havePrevious = true;
        if (cpu_.state().jammed) return RunStatus::CpuJammed;
    }
    if(mmc5ReadQuantum_.active){
        flushCpuReads();cpuReadBudget_=0;
        if(!mmc5FrameOkay()||videoHasFault())return RunStatus::VideoFault;
        if(audioFault()!=AudioFault::None)return RunStatus::AudioFault;
    }
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
        const auto target=cartridge_.mmc5_->ppuWriteTarget(address,true);
        cartridge_.mmc5_->ppuWrite(address&0x3fffU,value,nametables_.data());
        if(target!=~0U&&videoPacket_)static_cast<void>(videoPacket_->addMmc5PhysicalWrite(target,value));
        return;
    }
    if(splitVideoEnabled()&&(address&0x3FFFU)<0x2000U)freezeVideoChrWrite(address&0x1FFFU,value);
    if(splitVideoEnabled() && videoPacket_){
        const auto ref=videoReference(address);
        if((ref&VideoByteRef::kindMask)==VideoByteRef::chrRam)
            videoPacket_->add(VideoOp::ChrWrite,ref&255U,(ref>>8)&255U,0,value);
        else if((ref&VideoByteRef::kindMask)==VideoByteRef::nametable)
            videoPacket_->add(VideoOp::NameWrite,ref&255U,(ref>>8)&255U,0,value);
    }

    address &= 0x3fff;
    if (address < 0x2000) cartridge_.chrWrite(address, value);
    else {
        const auto physical=nametableAddress(address);
        // Физический индекс записей включает атрибуты и все алиасы NT.
        videoNameMarkers_.update(physical,value);
        nametables_[physical]=value;
    }
}
/**
 * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
 * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::dmcRead(std::uint16_t address) noexcept {
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
        const auto* byte=cartridge_.chrPages_[(pattern>>10)&7U]+(pattern&1023U);
        *lowReference=cachedVideoReference(byte); // ДО защёлки после старшего байта!
        // Те же четыре логических чтения/A12/переход защёлки; данные представлены
        // физической идентичностью до строба, пока не потребуется непрозрачность.
        if(cartridge_.legacy_.backgroundTile(static_cast<std::uint16_t>(pattern+8U)))cartridge_.refreshChrPages();
        return (attribute<<16)|(std::uint32_t(tile)<<24);
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

/**
 * @brief Подключает заимствованное хранилище аудиособытий и сбрасывает машину.
 * @details Настраивается потоком-владельцем. nullptr восстанавливает синхронный APU; память пакета остаётся у вызывающего.
 * @param storage Заимствованное хранилище выбранного протокола; nullptr обрабатывается контрактом метода.
 */
void Machine::configureSplitAudio(AudioPacket* storage
                                 ,bool expansionDac
                                 ) noexcept {
    if(outputOwner_)return; // Не менять active audio или сбрасывать чужую output-аренду.
    if(thin_){invalidateThinPpu();return;}
    audioPacket_ = storage;
    expansionAudio_=storage&&expansionDac;
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
    lastExpansionLevel_=0; // Новый полный APU тоже начинает expansion input с 0.
    if (audioPacket_) audioPacket_->begin(audioGeneration_,audioSequence_,0,region_
                                         ,expansionAudio_
                                         );
}
/** @details TimingApu.cycles включает pending и равен текущей границеT−1 перед его tick(T). */
void Machine::audioPublishExpansion(std::int32_t level)noexcept{
    if(!audioPacket_){apu_.setExpansionInput(level);return;}
    if(!expansionAudio_){static_cast<void>(audioPacket_->addExpansion(timingApu_.cycles(),level));return;}
    if(level==lastExpansionLevel_)return;
    static_cast<void>(audioPacket_->addExpansion(timingApu_.cycles(),level));
    lastExpansionLevel_=level;
}
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
    const auto value = cartridge_.cpuRead(address,openBus_);
    if(thin_&&cartridge_.mapper()==5U&&(address==0xfffaU||address==0xfffbU)){
        
        // В audioTick между idle edge и этим callback нет публикации prefix.
        // Не добавлять второй pre-PPU event и не вызывать capacity-flush на полутакте.
        auto* last=thin_->count_?&thin_->events_[thin_->count_-1U]:nullptr;
        if(last&&last->kind==ColourKind::Mmc5IdleReset&&cycle>=thin_->base_.cycle&&
           last->cycleDelta==cycle-thin_->base_.cycle){
            last->kind=ColourKind::Mmc5IdleDmcStrobe;last->address=address;last->value=value;
            
        }else recordThinEvent(ColourKind::Mmc5DmcStrobe,cycle,address,value);
    }
    if (audioPacket_){
        // DMC timing/data/stall прежние; Main исправляет только нулевой FM placeholder этого source packet.
        const auto level=expansionAudio_&&!cartridge_.externalVrc7Enabled()?cartridge_.selectedExpansionSample():0;
        static_cast<void>(audioPacket_->addDmc(cycle,address,value,level));
        if(expansionAudio_)lastExpansionLevel_=level;
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
    if(thinStickyFault_||(thin_&&(!thin_->audioReady()||thin_->audioStageOwner_||thin_->audioStageState_!=AudioStagePhase::Empty)))return false;
    if (!audioPacket_ || &destination == audioPacket_ || audioFault() != AudioFault::None) return false;
    observeCpuReads();
    if (!audioPacket_->retireTo(destination,cpuCycles(),frames())) return false;
    ++audioSequence_;
    if(thin_)thin_->audioRetired_=true;
    audioPacket_->begin(audioGeneration_,audioSequence_,cpuCycles(),region_
                        ,expansionAudio_
                        );
    return true;
}

/**
 * @brief Проверяет завершённый первичный frame cut без flush/clock или изменения Machine.
 * @param beforeAck true требует активный Thin commit callback, false прежний audioReady после ACK.
 * @return false не отзывает packet/token/Thin borrow.
 */
bool Machine::audioStageBoundary(bool beforeAck) const noexcept {
    // Oracle также публикует точный commit: Legacy160 сохраняет полный PPU, а loan — прежний cut/token.
    if (!thin_ || !audioPacket_ || cpu_.inStep_ ||
        pendingCpuReads_ || cpuReadBudget_ || pendingPpuDots_ ||
        timingApu_.cycles() != cycles_ || !timingPpu_.frameReady_ ||
        thinStickyFault_ || videoHasFault() || audioFault() != AudioFault::None ||
        !thin_->begun_ || thin_->fault_ != ColourFault::None || thin_->notifying_ ||
        thin_->count_ || thin_->audioRetired_ || thin_->audioGeneration_ != audioGeneration_ ||
        thin_->lease_.producer != this || !thin_->lease_.generation ||
        thin_->lease_.region != region_ || thin_->lease_.epoch != colourEpoch_ ||
        !sameColourCut(thin_->base_, thinCut()) || !thin_->sequence_) return false;
    const auto frame = timingPpu_.frameNumber();
    if (beforeAck) return thin_->busy_ && !thin_->ready_ &&
        thin_->frame_ != ~std::uint64_t{0} && thin_->frame_ + 1U == frame;
    return !thin_->busy_ && thin_->audioReady() && thin_->frame_ == frame;
}

namespace {
/** @brief Проверяет пересечение object spans без переполнения адресной арифметики; указатели не разыменовывает. */
bool audioStageOverlap(const void* a, std::size_t an, const void* b, std::size_t bn) noexcept {
    const auto x = reinterpret_cast<std::uintptr_t>(a), y = reinterpret_cast<std::uintptr_t>(b);
    return x <= y ? y - x < an : x - y < bn;
}
}

/** @details Ничего не flush-ит: доверенный App вызывает только после Producer.commit Published, до ACK. */
bool Machine::stageAudioPacketForThinCommit(AudioPacket& destination, AudioStageToken& token) noexcept {
    if (!audioStageBoundary(true) || thin_->audioStageOwner_ || thin_->audioStageState_ != AudioStagePhase::Empty) return false;
    const bool empty = token.phase_ == AudioStagePhase::Empty && !token.self_;
    const bool next = token.completed() && token.owner_ == this && token.source_ == audioPacket_ &&
        token.header_.generation == audioGeneration_ && token.header_.sequence < audioSequence_ &&
        token.header_.endCycle <= audioPacket_->header().baseCycle;
    if ((!empty && !next) || audioSequence_ == ~std::uint64_t{0} || !audioGeneration_) return false;
    if (audioStageOverlap(audioPacket_, sizeof(AudioPacket), &destination, sizeof(destination)) ||
        audioStageOverlap(&token, sizeof(token), audioPacket_, sizeof(AudioPacket)) ||
        audioStageOverlap(&token, sizeof(token), &destination, sizeof(destination)) ||
        audioStageOverlap(this, sizeof(*this), &destination, sizeof(destination)) ||
        audioStageOverlap(this, sizeof(*this), &token, sizeof(token))) return false;
    const auto& source = audioPacket_->header();
    if (source.generation != audioGeneration_ || source.sequence != audioSequence_ || source.region != region_ ||
        source.fault != AudioFault::None || source.endCycle != source.baseCycle || source.ppuFrame ||
        cycles_ < source.baseCycle || cycles_ - source.baseCycle > 65535U) return false;
    // retireTo сначала проверяет destination limits и только затем копирует used records; source const.
    if (!audioPacket_->retireTo(destination, cycles_, timingPpu_.frameNumber())) return false;
    token.header_ = destination.header(); token.thinSequence_ = thin_->sequence_;
    token.leaseGeneration_ = thin_->lease_.generation; token.leaseEpoch_ = thin_->lease_.epoch;
    token.owner_ = this; token.source_ = audioPacket_; token.destination_ = &destination;
    token.thin_ = thin_; token.self_ = &token; token.phase_ = AudioStagePhase::Staged;
    thin_->audioStageOwner_ = &token; thin_->audioStageState_ = AudioStagePhase::Staged;
    return true;
}

/** @details Только Worker после stepFrame: все отказы предшествуют sequence/begin и не освобождают borrow. */
bool Machine::finalizeAudioPacketForThinCommit(AudioStageToken& token) noexcept {
    if (!audioStageBoundary(false) || !token.valid() || thin_->audioStageOwner_ != &token ||
        thin_->audioStageState_ != AudioStagePhase::Staged || token.owner_ != this ||
        token.source_ != audioPacket_ || !token.destination_ || token.thin_ != thin_ ||
        token.thinSequence_ != thin_->sequence_ || token.leaseGeneration_ != thin_->lease_.generation ||
        token.leaseEpoch_ != thin_->lease_.epoch || token.header_.generation != audioGeneration_ ||
        token.header_.sequence != audioSequence_ || token.header_.region != region_ ||
        token.header_.endCycle != cycles_ || token.header_.ppuFrame != timingPpu_.frameNumber() ||
        audioSequence_ == ~std::uint64_t{0} || !sameAudioHeader(token.header_, token.destination_->header())) return false;
    auto active = token.header_; active.endCycle = active.baseCycle; active.ppuFrame = 0;
    if (!sameAudioHeader(active, audioPacket_->header())) return false;
    // Прежний retirement transition без второй копии used records и без изменения audioReady semantics.
    ++audioSequence_; thin_->audioRetired_ = true;
    audioPacket_->begin(audioGeneration_, audioSequence_, cycles_, region_
                        , expansionAudio_
    );
    thin_->audioStageOwner_ = nullptr; thin_->audioStageState_ = AudioStagePhase::Empty;
    token.phase_ = AudioStagePhase::Completed;
    return true;
}

/** @details Только Main с внешним доказательством actual Worker join; timeout/fault не дают этого права. */
bool Machine::abandonAudioPacketForThinCommitJoined(AudioStageToken& token) noexcept {
    if (!token.valid() || token.owner_ != this || !token.thin_ || cpu_.inStep_) return false;
    auto* channel = const_cast<ThinPpuChannel*>(token.thin_);
    if (channel->busy_ || channel->notifying_ || channel->audioStageOwner_ != &token ||
        channel->audioStageState_ != AudioStagePhase::Staged) return false;
    channel->audioStageOwner_ = nullptr; channel->audioStageState_ = AudioStagePhase::Empty;
    token.phase_ = AudioStagePhase::Abandoned;
    return true;
}


}  // пространство имён nes::core

#undef NES_COST_CPU_STEP
#undef NES_COST_QUIET_MMC5
#undef NES_COST_QUIET_LEGACY




#undef NES_THIN_LARGE_GQ_DOMAIN
