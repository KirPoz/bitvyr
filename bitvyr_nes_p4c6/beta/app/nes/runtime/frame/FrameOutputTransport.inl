/**
 * @file FrameOutputTransport.inl
 * @brief Только transport-часть Frame pipeline: Timing CPU1 и output-only CPU0.
 * @details Подключается в namespace nes::app после определения Impl. DSP/loan,
 * external Worker done, PPA и joint acceptance остаются общими с прежним backend.
 */

/** @details Main читает только release/acquire sealed Payload, никогда producer N+1. */
bool FrameVideoPipeline::Impl::stageOutputMetadata(unsigned i,FrameOutputSlot::State wanted)const noexcept {
    if(i>=slotsCount||readyIndex!=int(i)||accepted==~std::uint64_t{0}||
       !control->ready[i].load(std::memory_order_acquire)||control->packets[i].state()!=wanted)return false;
    const auto&p=*payload[i];const auto&t=p.outputTicket;const auto&c=p.outputCut;
    const auto&l=t.lease;const auto&v=p.snapshot;const auto&a=p.audio.header();
    if(!outputBound||!p.outputPacket||p.outputPacket!=outputPackets[i]||
       t.slot!=i||t.slot!=(accepted&1U)||t.sequence!=accepted||t.inputFrame!=accepted||
       l.producer!=outputLease.producer||l.rom!=outputLease.rom||l.romBytes!=outputLease.romBytes||
       l.romEpoch!=outputLease.romEpoch||l.generation!=outputLease.generation||
       l.transportEpoch!=outputLease.transportEpoch||l.region!=outputLease.region||
       v.inputFrame!=t.inputFrame||v.appGeneration!=appGeneration||v.region!=lease.region||v.pad!=p.outputPad||
       v.colourSequence!=accepted||v.cycle!=c.endCycle||v.frame!=c.ppuFrame||
       c.baseCycle!=t.baseCycle||c.baseCycle!=mainColourCut.cycle||c.endCycle<=c.baseCycle||
       c.ppuFrame!=accepted+1U||c.videoGeneration!=outputVideoGeneration||
       c.videoStart!=(accepted?c.baseCycle:0U)||c.remainder>=5U||c.dot>340U||
       c.scanline < -1||c.scanline>(lease.region==core::Region::Pal?311:260))return false;
    if(v.cpu.pc!=c.cpu.pc||v.cpu.a!=c.cpu.a||v.cpu.x!=c.cpu.x||v.cpu.y!=c.cpu.y||v.cpu.sp!=c.cpu.sp||
       v.cpu.p!=c.cpu.p||v.cpu.cycles!=c.cpu.cycles||v.cpu.jammed!=c.cpu.jammed)return false;
    if(!matchesFrameRamWitness(v.ram,t,c,appGeneration,ramPolicy))return false;
    const auto&h=p.outputPacket->header();
    const auto delta=c.endCycle-c.baseCycle;
    const unsigned phase=unsigned((mainColourCut.phase+delta*(lease.region==core::Region::Pal?16U:15U))%5U);
    if(p.outputPacket->state()!=core::VideoPacket::State::Sealed||h.fault!=core::VideoFault::None||
       h.generation!=outputVideoGeneration||h.sequence!=accepted||h.startCycle!=c.videoStart||
       h.endCycle!=c.endCycle||h.ppuFrame!=c.ppuFrame||h.pal!=(lease.region==core::Region::Pal)||
       h.chrRom!=outputChr||h.chrRomBytes!=outputChrBytes||h.seedPresent!=(accepted==0)||
       h.eventCount>core::VideoPacket::eventCapacity||c.remainder!=phase||
       control->packets[i].header().sequence!=accepted)return false;
    if(fmStorage){
        const auto&v=p.fmTape;
        if(v.state!=core::Vrc7WriteTape::State::Sealed||v.fault!=core::Vrc7WriteTape::Fault::None||
           v.events!=fmStorage->events[i]||v.count>Vrc7TapeStorage::capacity||
           v.baseCycle!=t.baseCycle||v.endCycle!=a.endCycle)return false;
    }
    return a.generation&&a.sequence!=~std::uint64_t{0}&&a.fault==core::AudioFault::None&&
        a.region==lease.region&&a.endCycle>=a.baseCycle&&a.endCycle-a.baseCycle<=65535U&&
        a.inputCount<=core::AudioPacket::inputCapacity&&a.dmcCount<=core::AudioPacket::dmcCapacity&&
        a.ppuFrame==c.ppuFrame&&a.endCycle==c.endCycle&&
        a.generation==v.audioGeneration&&a.sequence==v.audioSequence;
}

/** @details Worker использует только свой Payload и не ждёт освобождения соседнего Reading. */
/** @brief Выполняет CPU1-кадр и публикует только согласованный output-слот без cache-наблюдателя. */
core::RunStatus FrameVideoPipeline::Impl::produceOutput(unsigned i)noexcept {
    auto&p=*payload[i];auto&slot=control->packets[i];auto&m=producer;
    p.usage={}; // Ранний отказ не публикует старый SlotN DTO; Core counters здесь не сбрасываются.
    const auto start=esp_timer_get_time();p.times={};p.snapshot={};p.outputPacket=nullptr;p.fmTape={};
    if(!okay()||expired(i)||!control->workerBorrowed.load(std::memory_order_acquire)){
        fail(Error::Stopped);return core::RunStatus::VideoFault;
    }
    if(!outputBound||!m.outputFramesEnabled()||!m.splitVideoEnabled()||!m.splitAudio()||
       m.region()!=outputLease.region||m.videoFault()!=core::VideoFault::None||
       !slot.begin(jobInputFrame)){
        fail(Error::Owner);return core::RunStatus::VideoFault;
    }
    // Field assignments не создают большие automatic Binding/Ticket/Cut объекты.
    p.outputTicket.lease=outputLease;p.outputTicket.slot=i;
    p.outputTicket.sequence=jobInputFrame;p.outputTicket.inputFrame=jobInputFrame;
    p.outputTicket.baseCycle=m.cpuCycles();
    if(!m.beginOutputFrame(p.outputTicket)){fail(Error::Owner);return core::RunStatus::VideoFault;}
    outputWriterAttached=true;
    p.hart={};auto&hi=p.hart.identity;const auto&hl=p.outputTicket.lease;
    hi.producer=reinterpret_cast<std::uintptr_t>(hl.producer);hi.rom=reinterpret_cast<std::uintptr_t>(hl.rom);
    hi.romBytes=hl.romBytes;hi.appGeneration=appGeneration;hi.leaseGeneration=hl.generation;
    hi.romEpoch=hl.romEpoch;hi.transportEpoch=hl.transportEpoch;hi.sequence=p.outputTicket.sequence;
    hi.baseCycle=p.outputTicket.baseCycle;hi.slot=i;
    const auto coreBegin=esp_timer_get_time();
    debug::HartProbe<debug::HartHardware>::begin(p.hart);
    auto result=m.stepFrame();
    debug::HartProbe<debug::HartHardware>::end(p.hart);
    static_cast<void>(m.cartridge());p.times.core=esp_timer_get_time()-coreBegin;
    if(result!=core::RunStatus::FrameReady||!okay()||expired(i)){
        fail(expired(i)?Error::Deadline:Error::Producer);return result==core::RunStatus::FrameReady?core::RunStatus::VideoFault:result;
    }
    p.usage.counts=m.mmc5Usage(); // Worker после stepFrame/RAII materialization, до seal/Free.
    const auto seal=esp_timer_get_time();
    if(!m.sealOutputFrame(p.outputTicket,p.outputPacket,p.outputCut)){fail(Error::Colour);return core::RunStatus::VideoFault;}
    outputWriterAttached=false;p.times.seal=esp_timer_get_time()-seal;
    const auto retire=esp_timer_get_time();if(!m.retireAudioPacket(p.audio)){fail(Error::Audio);return core::RunStatus::AudioFault;}p.times.audioRetire=esp_timer_get_time()-retire;
    const auto&a=p.audio.header();const auto&c=p.outputCut;const auto&h=p.outputPacket->header();
    if(a.generation!=expectedAudio.generation||a.sequence!=expectedAudio.sequence||
       a.baseCycle!=expectedAudio.baseCycle||a.region!=lease.region||a.fault!=core::AudioFault::None||
       a.ppuFrame!=c.ppuFrame||a.endCycle!=c.endCycle||c.ppuFrame!=jobInputFrame+1U||
       h.sequence!=jobInputFrame||h.generation!=outputVideoGeneration||h.fault!=core::VideoFault::None){
        fail(Error::Cut);return core::RunStatus::VideoFault;
    }
    ++expectedAudio.sequence;expectedAudio.baseCycle=a.endCycle;
    const auto snapshotBegin=esp_timer_get_time();auto&v=p.snapshot;
    v.cpu=c.cpu;v.frame=c.ppuFrame;v.cycle=c.endCycle;v.appGeneration=jobAppGeneration;
    v.inputFrame=jobInputFrame;v.pad=jobPad;v.region=lease.region;
    v.audioGeneration=a.generation;v.audioSequence=a.sequence;v.colourSequence=h.sequence;
    // readHash/readCount остаются0: packet не выдаёт вымышленных CPU read witnesses.
    prepareFrameRamWitness(v.ram,p.outputTicket,p.outputCut,jobAppGeneration,ramPolicy);
    const bool ramDone=v.ram.requested
        ?m.copyOutputVisibleRam(p.outputTicket,p.ram.data(),p.ram.size())
        :m.finalizeOutputMetadata(p.outputTicket,p.ram.data(),p.ram.size());
    if(!ramDone){fail(Error::Cut);return core::RunStatus::VideoFault;}
    v.ram.valid=v.ram.requested; // Только успешный full API производит RAM bytes.
    // Core seal выполняется после tail/audio retire/metadata. До Ready переносим
    // только bounded immutable View, не обращаемся к producer с Main позже.
    if(fmStorage&&!m.outputVrc7Tape(p.outputTicket,p.fmTape)){fail(Error::Audio);return core::RunStatus::AudioFault;}
    auto&usage=p.usage;const auto&usageLease=p.outputTicket.lease;
    usage.generation=jobAppGeneration;usage.sequence=jobInputFrame;usage.slot=i;
    usage.baseCycle=p.outputTicket.baseCycle;usage.endCycle=p.outputCut.endCycle;usage.region=usageLease.region;
    usage.leaseGeneration=usageLease.generation;usage.romEpoch=usageLease.romEpoch;usage.transportEpoch=usageLease.transportEpoch;
    const auto maximum=~std::uint32_t{0};
    usage.status=(usage.counts.h2AdmittedM2==maximum||usage.counts.tile8Groups==maximum||
        usage.counts.carryContinuations==maximum)?UsageStatus::Saturated:UsageStatus::Complete;
    p.times.snapshot=esp_timer_get_time()-snapshotBegin;p.times.producer=esp_timer_get_time()-start;
    if(!okay()||expired(i)){fail(Error::Deadline);return core::RunStatus::VideoFault;}
    if(!slot.seal()){fail(Error::Slot);return core::RunStatus::VideoFault;}
    control->ready[i].store(1,std::memory_order_release);return core::RunStatus::FrameReady;
}

/** @details C0 только составляет изображение; CPU read/IRQ/mapper выполняет первичный Timing owner. */
bool FrameVideoPipeline::Impl::renderOutput(unsigned i,bool(*progress)(void*)noexcept,void*context)noexcept {
    if(!outputRenderer||!stageOutputMetadata(i,FrameOutputSlot::State::Sealed)||
       !control->packets[i].read())return fail(Error::Cut);
    if(outputRenderer->render(*payload[i]->outputPacket,progress,context)!=core::VideoRenderResult::Ok||
       outputRenderer->endCycle()!=payload[i]->outputCut.endCycle||
       outputRenderer->ppuFrame()!=payload[i]->outputCut.ppuFrame||
       outputRenderer->generation()!=outputVideoGeneration||
        outputRenderer->sequence()!=accepted||outputRenderer->canonicalVideoAvailable()||
        !outputRenderer->rgbRowScratchReady())return fail(Error::Output);
    return control->packets[i].consumed()||fail(Error::Slot);
}
