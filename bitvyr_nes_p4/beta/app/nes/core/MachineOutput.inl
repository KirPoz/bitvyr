/**
 * @file MachineOutput.inl
 * @brief Явное Timing-владение с отделяемым output writer, без SDK и ожиданий.
 * @details Подключается ровно один раз MachineVideo.cpp при PUBLIC feature=1.
 */
namespace nes::core {
namespace output_detail {
/** @brief Проверяет непустой uintptr диапазон без переполнения; память не читает. */
static bool valid(const void* p,std::size_t n) noexcept {
    return p&&n&&n<=~std::uintptr_t{0}-reinterpret_cast<std::uintptr_t>(p);
}
/** @brief Проверяет пересечение через разности, не складывая потенциально опасные адреса. */
static bool overlaps(const void* a,std::size_t an,const void* b,std::size_t bn) noexcept {
    if(!a||!an||!b||!bn)return false;
    const auto av=reinterpret_cast<std::uintptr_t>(a),bv=reinterpret_cast<std::uintptr_t>(b);
    return av<bv?bv-av<an:av-bv<bn;
}
/** @brief Проверяет, целиком ли действительное заимствование входит в защитный диапазон. */
// Валидация выполняется при настройке аренды, вне кадрового исполнения.
// Стабильный внешний символ позволяет адаптеру разместить её в cached Flash;
// GCC IPA-clone иначе получает суффикс .isra и обходит точное linker-правило.
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline, gnu::noclone]]
#elif defined(__clang__)
[[gnu::noinline]]
#endif
/** @details Проверяет целиком полуоткрытый диапазон с учётом переполнения uintptr, до разыменования backing. */
static bool contains(const OutputProtectedSpan& a,const void* b,std::size_t bn) noexcept {
    if(!valid(a.data,a.bytes)||!valid(b,bn))return false;
    const auto av=reinterpret_cast<std::uintptr_t>(a.data),bv=reinterpret_cast<std::uintptr_t>(b);
    return bv>=av&&bv-av<=a.bytes&&bn<=a.bytes-(bv-av);
}
/** @brief Сравнивает все поля аренды, не читая байты ROM или изменяемой машины. */
static bool same(const OutputFrameLease& a,const OutputFrameLease& b) noexcept {
    return a.producer==b.producer&&a.rom==b.rom&&a.romBytes==b.romBytes&&
        a.romEpoch==b.romEpoch&&a.generation==b.generation&&
        a.transportEpoch==b.transportEpoch&&a.region==b.region;
}
/** @brief Сравнивает полную идентичность JOB, включая slot, input и настоящий base cut. */
static bool same(const OutputFrameTicket& a,const OutputFrameTicket& b) noexcept {
    return same(a.lease,b.lease)&&a.sequence==b.sequence&&a.inputFrame==b.inputFrame&&
        a.baseCycle==b.baseCycle&&a.slot==b.slot;
}
} // namespace output_detail

/** @details Все входные диапазоны проверяются до reset и записи в output lease. */
bool Machine::configureOutputFrames(const OutputFrameBinding& binding,OutputFrameLease& lease) noexcept {
    using namespace output_detail;
    constexpr std::size_t ramBytes=2048U+8192U;
    const auto& source=binding.source;

    if(outputOwner_||cpu_.inStep_||cpuCycles()!=7U||!cartridge_.loaded()||
       source.producer!=this||!source.romEpoch||!source.generation||source.transportEpoch||
       source.region!=region_||(region_!=Region::Ntsc&&region_!=Region::Pal)||
       !valid(source.rom,source.romBytes)||outputEpoch_==~std::uint64_t{0}||
       colourCapture_||videoPacket_||videoObserver_)return false;
    if(!valid(&lease,sizeof(lease))||overlaps(&lease,sizeof(lease),this,sizeof(*this))||
       overlaps(&lease,sizeof(lease),source.rom,source.romBytes)||
       overlaps(&lease,sizeof(lease),&binding,sizeof(binding))||
       overlaps(&lease,sizeof(lease),ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
#if defined(NES_CORE_CPU_PRG_TAIL_RESIDENT) && NES_CORE_CPU_PRG_TAIL_RESIDENT
    // Result не может записывать действующий immutable PRG-заём до его reset-detach.
    if(overlaps(&lease,sizeof(lease),cartridge_.cpuPrgTail_.resident,
                cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U))return false;
#endif
    for(const auto& span:binding.protectedSpans)
        if(overlaps(&lease,sizeof(lease),span.data,span.bytes))return false;
    const auto mapper=cartridge_.mapper();
    // Только уже существующие packet-возможности. Native-проверка данного API
    // не выбирает будущий App backend и не объявляет эти mapper проверенными P4.
    if(!Cartridge::supportsTimingVideoMapper(mapper))return false;
    // Внешнее MMC5-хранилище принадлежит Worker и защищено на полный срок аренды.
    if(mapper==5){
        if(!cartridge_.mmc5_||!cartridge_.mmc5Audio_)return false;
        bool digital=false,audio=false;
        for(const auto& span:binding.protectedSpans){
            digital|=contains(span,cartridge_.mmc5_,sizeof(Mmc5));
            audio|=contains(span,cartridge_.mmc5Audio_,sizeof(Mmc5Audio));
        }
        if(!digital||!audio)return false;
    }
    const auto image=reinterpret_cast<std::uintptr_t>(source.rom);
    const auto prg=reinterpret_cast<std::uintptr_t>(cartridge_.prg_);
    if(prg<image||prg-image>source.romBytes||cartridge_.prgSize_>source.romBytes-(prg-image)||
       cartridge_.chrSize_>source.romBytes-(prg-image)-cartridge_.prgSize_)return false;
    if(binding.packets[0]==binding.packets[1]||binding.snapshots[0]==binding.snapshots[1])return false;
    for(unsigned i=0;i<2;++i){
        auto* packet=binding.packets[i];auto* snapshot=binding.snapshots[i];
        if(!valid(packet,sizeof(VideoPacket))||!valid(snapshot,ramBytes)||
           (reinterpret_cast<std::uintptr_t>(packet)%alignof(VideoPacket))||
           overlaps(packet,sizeof(VideoPacket),this,sizeof(*this))||
           overlaps(packet,sizeof(VideoPacket),source.rom,source.romBytes)||
           overlaps(packet,sizeof(VideoPacket),ppu_.pixels_.data(),Ppu::width*Ppu::height)||
           overlaps(packet,sizeof(VideoPacket),&binding,sizeof(binding))||
           overlaps(packet,sizeof(VideoPacket),&lease,sizeof(lease))||
           overlaps(snapshot,ramBytes,this,sizeof(*this))||
           overlaps(snapshot,ramBytes,source.rom,source.romBytes)||
           overlaps(snapshot,ramBytes,&binding,sizeof(binding))||
           overlaps(snapshot,ramBytes,&lease,sizeof(lease))||
           overlaps(snapshot,ramBytes,ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
        for(unsigned j=0;j<2;++j){
            if(overlaps(snapshot,ramBytes,binding.packets[j],sizeof(VideoPacket)))return false;
            if(i!=j&&(overlaps(packet,sizeof(VideoPacket),binding.packets[j],sizeof(VideoPacket))||
               overlaps(snapshot,ramBytes,binding.snapshots[j],ramBytes)))return false;
        }
        for(const auto& span:binding.protectedSpans){
            if((span.data||span.bytes)&&!valid(span.data,span.bytes))return false;
            if(overlaps(snapshot,ramBytes,span.data,span.bytes)||
               overlaps(packet,sizeof(VideoPacket),span.data,span.bytes))return false;
        }
#if defined(NES_CORE_CPU_PRG_TAIL_RESIDENT) && NES_CORE_CPU_PRG_TAIL_RESIDENT
        if(overlaps(snapshot,ramBytes,cartridge_.cpuPrgTail_.resident,
                    cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U)||
           overlaps(packet,sizeof(VideoPacket),cartridge_.cpuPrgTail_.resident,
                    cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U))return false;
#endif
    }
    // Сначала проверены адреса/alias ОБОИХ объектов; затем разрешено читать metadata.
    for(auto* packet:binding.packets)if(packet->state()!=VideoPacket::State::Free)return false;
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if(audioPacket_){
        bool covered=false;
        for(const auto& span:binding.protectedSpans)covered|=contains(span,audioPacket_,sizeof(*audioPacket_));
        if(!covered)return false;
    }
#endif
    outputBinding_=binding;outputLease_=source;outputLease_.transportEpoch=++outputEpoch_;
    lease=outputLease_; // При fault reset вызывающий всё равно может joined stop/cancel.
    outputOwner_=outputInitializing_=true;outputJob_=outputSealed_=outputSnapshot_=outputStarted_=false;
    outputNextSequence_=outputNextInput_=0;outputTicket_={};outputCut_={};
    videoPacket_=binding.packets[0];videoObserver_=nullptr;
#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
    videoObservableOnly_=false;
#endif
    reset(); // videoReset seed/start0 предшествует всем7 reset clocks.
    outputInitializing_=false;
    outputCut_.endCycle=cpuCycles();outputCut_.ppuFrame=timingPpu_.frameNumber();
    if(cpuCycles()!=7U||videoFault()!=VideoFault::None||!splitVideoEnabled()||
       !videoPacket_||videoPacket_->state()!=VideoPacket::State::Recording){
        videoFaultLatch_=VideoFault::BadBinding;return false; // Заимствования сохранены.
    }
    return true;
}

/** @details Предикат чистый; detached отказ не меняет даже IRQ/open bus/долги. */
bool Machine::outputFrameExecutable() const noexcept {
    return outputOwner_&&(outputInitializing_||(outputJob_&&videoPacket_&&
        videoPacket_->state()==VideoPacket::State::Recording&&
        timingPpu_.packet_==videoPacket_&&!videoHasFault()));
}

/** @details Free читается только после внешнего acquire: сам packet не атомарен. */
bool Machine::beginOutputFrame(const OutputFrameTicket& ticket) noexcept {
    using namespace output_detail;
    if(!outputOwner_||outputInitializing_||outputJob_||cpu_.inStep_||ticket.slot>=2||
       !same(ticket.lease,outputLease_)||ticket.sequence!=outputNextSequence_||
       ticket.slot!=(ticket.sequence&1U)||ticket.baseCycle!=cpuCycles()||
       ticket.baseCycle!=outputCut_.endCycle||videoHasFault()||
       (outputStarted_&&(!outputSnapshot_||ticket.inputFrame!=outputNextInput_))||
       ticket.sequence==~std::uint64_t{0}||ticket.inputFrame==~std::uint64_t{0})return false;
    auto* packet=outputBinding_.packets[ticket.slot];
    if(!outputStarted_){
        if(ticket.sequence||ticket.slot||videoPacket_!=packet||timingPpu_.packet_!=packet||
           packet->state()!=VideoPacket::State::Recording||!packet->header().seedPresent||
           packet->header().startCycle||packet->header().sequence||cpuCycles()!=7U)return false;
    }else{
        if(videoPacket_||timingPpu_.packet_||packet->state()!=VideoPacket::State::Free)return false;
        if(!packet->begin(videoGeneration_,ticket.sequence,cpuCycles(),region_==Region::Pal,
             cartridge_.chr_,static_cast<std::uint32_t>(cartridge_.chrSize_)))return false;
        packet->cursor(cpuCycles(),timingPpu_.frameNumber());
        videoSequence_=ticket.sequence;videoPacket_=packet;timingPpu_.bind(packet);
    }
    outputTicket_=ticket;outputJob_=outputStarted_=true;outputSealed_=outputSnapshot_=false;
#if defined(NES_RUN_MMC5_SPAN_VIEW) && NES_RUN_MMC5_SPAN_VIEW
    // Pure seed отказ сохраняет исходный fetch-format кадра. Никакого guest эффекта.
    if(cartridge_.mapper()==5U&&cartridge_.mmc5_)
        static_cast<void>(videoPacket_->tryMmc5SpanSeed(nametables_.data(),cartridge_.mmc5_->exRam().data()));
#endif
#if defined(NES_CORE_MMC5_USAGE_COUNTS) && NES_CORE_MMC5_USAGE_COUNTS
    mmc5Usage_={}; // Все ticket/packet проверки уже успешны; failed begin не сбрасывает старый record.
#endif
    return true;
}

/** @details Никакая операция не запрашивает Free другого слота; C1 не ждёт C0. */
bool Machine::sealOutputFrame(const OutputFrameTicket& ticket,const VideoPacket*& borrowed,OutputFrameCut& cut) noexcept {
    using namespace output_detail;
    if(!outputWritableResult(&borrowed,sizeof(borrowed))||!outputWritableResult(&cut,sizeof(cut))||
       overlaps(&borrowed,sizeof(borrowed),&cut,sizeof(cut))||
       overlaps(&borrowed,sizeof(borrowed),&ticket,sizeof(ticket))||
       overlaps(&cut,sizeof(cut),&ticket,sizeof(ticket)))return false;
    borrowed=nullptr;
    if(!outputOwner_||!outputJob_||!same(ticket,outputTicket_)||cpu_.inStep_||
       !outputFrameExecutable()||!timingPpu_.frameReady())return false;
    observeCpuReads();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    flushPpu();
#endif
    timingPpu_.flushPixels();
    const auto end=cpuCycles(),frame=timingPpu_.frameNumber();
    if(videoHasFault()||end<=ticket.baseCycle||frame!=outputCut_.ppuFrame+1U||
       !videoPacket_->seal(end,frame))return false;
    auto* old=videoPacket_;
    outputCut_={};outputCut_.cpu=cpu_.state();outputCut_.baseCycle=ticket.baseCycle;
    outputCut_.endCycle=end;outputCut_.ppuFrame=frame;outputCut_.scanline=timingPpu_.scanline();
    outputCut_.dot=timingPpu_.dot();outputCut_.remainder=ppuRemainder_;
    outputCut_.videoStart=old->header().startCycle;outputCut_.videoGeneration=videoGeneration_;
    timingPpu_.bind(nullptr);videoPacket_=nullptr;outputJob_=false;outputSealed_=true;
    outputNextSequence_=ticket.sequence+1U;outputNextInput_=ticket.inputFrame+1U;
    cut=outputCut_;borrowed=old;return true;
}

/** @details Регистрация обоих RAM результатов исключает незаметную подмену назначения. */
bool Machine::copyOutputVisibleRam(const OutputFrameTicket& ticket,std::uint8_t* destination,std::size_t bytes) noexcept {
    using namespace output_detail;
    constexpr std::size_t ramBytes=2048U+8192U;
    if(!outputOwner_||outputJob_||!outputSealed_||outputSnapshot_||ticket.slot>=2||
       !same(ticket,outputTicket_)||destination!=outputBinding_.snapshots[ticket.slot]||bytes!=ramBytes||
       !valid(destination,bytes)||cpu_.inStep_||pendingCpuReads_||pendingPpuDots_||
       videoPacket_||timingPpu_.packet_||colourCapture_||videoHasFault()||
       cpuCycles()!=outputCut_.endCycle||timingPpu_.frameNumber()!=outputCut_.ppuFrame||
       overlaps(destination,bytes,this,sizeof(*this))||
       overlaps(destination,bytes,&ticket,sizeof(ticket))||
       overlaps(destination,bytes,outputLease_.rom,outputLease_.romBytes)||
       overlaps(destination,bytes,ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
    for(auto* packet:outputBinding_.packets)if(overlaps(destination,bytes,packet,sizeof(*packet)))return false;
    for(const auto& span:outputBinding_.protectedSpans)if(overlaps(destination,bytes,span.data,span.bytes))return false;
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if(overlaps(destination,bytes,audioPacket_,audioPacket_?sizeof(*audioPacket_):0U))return false;
#endif
#if defined(NES_CORE_CPU_PRG_TAIL_RESIDENT) && NES_CORE_CPU_PRG_TAIL_RESIDENT
    if(overlaps(destination,bytes,cartridge_.cpuPrgTail_.resident,
                cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U))return false;
#endif
    const auto seed=openBus_;
    std::memcpy(destination,ram_.data(),ram_.size());
#if defined(NES_CORE_FRAME_RAM_WINDOW) && NES_CORE_FRAME_RAM_WINDOW
    cartridge_.copyCpu6000Window(destination+2048U,seed);
#else
    for(unsigned n=0;n<8192U;++n)destination[2048U+n]=cartridge_.cpuRead(static_cast<std::uint16_t>(0x6000U+n),seed);
#endif
    cpuReadBudget_=0;refreshPpuDeferBudget();outputSnapshot_=true;return true;
}

/** @details Те же admission и phase/budget side effects, без RAM witness. */
bool Machine::finalizeOutputMetadata(const OutputFrameTicket& ticket,std::uint8_t* destination,std::size_t bytes) noexcept {
    using namespace output_detail;
    constexpr std::size_t ramBytes=2048U+8192U;
    if(!outputOwner_||outputJob_||!outputSealed_||outputSnapshot_||ticket.slot>=2||
       !same(ticket,outputTicket_)||destination!=outputBinding_.snapshots[ticket.slot]||bytes!=ramBytes||
       !valid(destination,bytes)||cpu_.inStep_||pendingCpuReads_||pendingPpuDots_||
       videoPacket_||timingPpu_.packet_||colourCapture_||videoHasFault()||
       cpuCycles()!=outputCut_.endCycle||timingPpu_.frameNumber()!=outputCut_.ppuFrame||
       overlaps(destination,bytes,this,sizeof(*this))||
       overlaps(destination,bytes,&ticket,sizeof(ticket))||
       overlaps(destination,bytes,outputLease_.rom,outputLease_.romBytes)||
       overlaps(destination,bytes,ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
    for(auto* packet:outputBinding_.packets)if(overlaps(destination,bytes,packet,sizeof(*packet)))return false;
    for(const auto& span:outputBinding_.protectedSpans)if(overlaps(destination,bytes,span.data,span.bytes))return false;
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if(overlaps(destination,bytes,audioPacket_,audioPacket_?sizeof(*audioPacket_):0U))return false;
#endif
#if defined(NES_CORE_CPU_PRG_TAIL_RESIDENT) && NES_CORE_CPU_PRG_TAIL_RESIDENT
    if(overlaps(destination,bytes,cartridge_.cpuPrgTail_.resident,
                cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U))return false;
#endif
    // Metadata-only: не читать и не записывать зарегистрированные RAM bytes.
    cpuReadBudget_=0;refreshPpuDeferBudget();outputSnapshot_=true;return true;
}

/** @details Только после настоящего join и возврата обоих consumer ack, не по timeout. */
bool Machine::cancelOutputFrames(const OutputFrameLease& lease) noexcept {
    if(!outputOwner_||!output_detail::same(lease,outputLease_)||cpu_.inStep_||
       outputJob_||videoPacket_||timingPpu_.packet_)return false;
    for(auto* packet:outputBinding_.packets)if(packet->state()!=VideoPacket::State::Free)return false;
    outputOwner_=false;outputBinding_={};outputLease_={};outputTicket_={};outputCut_={};
    outputSealed_=outputSnapshot_=outputStarted_=false;
    // Переход к другому владельцу разрешает только последующий явный joined reset.
    videoFaultLatch_=VideoFault::BadBinding;
#if defined(NES_CORE_VIDEO_POLICY_CACHE) && NES_CORE_VIDEO_POLICY_CACHE
    refreshVideoPolicy();
#endif
    return true;
}

/** @details Проверка адресов не доказывает writable-права: их удостоверяет адаптер. */
bool Machine::outputWritableResult(const void* destination,std::size_t bytes) const noexcept {
    using namespace output_detail;
    if(!outputOwner_||!valid(destination,bytes)||overlaps(destination,bytes,this,sizeof(*this))||
       overlaps(destination,bytes,outputLease_.rom,outputLease_.romBytes)||
       overlaps(destination,bytes,ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
    for(unsigned i=0;i<2;++i)
        if(overlaps(destination,bytes,outputBinding_.packets[i],sizeof(VideoPacket))||
           overlaps(destination,bytes,outputBinding_.snapshots[i],10240U))return false;
    for(const auto& span:outputBinding_.protectedSpans)
        if(overlaps(destination,bytes,span.data,span.bytes))return false;
#if defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU
    if(overlaps(destination,bytes,audioPacket_,audioPacket_?sizeof(*audioPacket_):0U))return false;
#endif
#if defined(NES_CORE_CPU_PRG_TAIL_RESIDENT) && NES_CORE_CPU_PRG_TAIL_RESIDENT
    if(overlaps(destination,bytes,cartridge_.cpuPrgTail_.resident,
                cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U))return false;
#endif
    return true;
}

/** @details Явное прекращение записи после join; оно не является откатом гостя. */
bool Machine::stopOutputWriter(const OutputFrameLease& lease) noexcept {
    if(!outputOwner_||!output_detail::same(lease,outputLease_)||cpu_.inStep_||
       outputInitializing_||!videoPacket_||timingPpu_.packet_!=videoPacket_||
       videoPacket_->state()!=VideoPacket::State::Recording)return false;
    observeCpuReads();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    flushPpu();
#endif
    timingPpu_.flushPixels();
    auto* old=videoPacket_;timingPpu_.bind(nullptr);videoPacket_=nullptr;
    old->abandon();outputJob_=false;outputSealed_=outputSnapshot_=false;
    videoFaultLatch_=VideoFault::BadBinding;
    return true;
}
} // namespace nes::core
