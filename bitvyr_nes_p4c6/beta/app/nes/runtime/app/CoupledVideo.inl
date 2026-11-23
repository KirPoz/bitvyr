/** @file CoupledVideo.inl
 * @brief MMC5 Thin producer/Main consumer и PREACK audio ticket.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
bool NesApp::configureCoupledVideoPipeline() noexcept {
    // Explicit selector сохраняет Thin/C6 только MMC5; остальные идут в Frame factory ниже.
    if(!usesCoupledVideo(current_.mapper))return worker_&&!worker_->inFlight&&!worker_->coupled;
    if(worker_&&worker_->framePipeline)return false;
    if(!videoMapperSupported(current_.mapper))return true;
    const bool ownerReady=videoWorkerReady();
    const bool joined=worker_&&!worker_->inFlight;
    std::printf("NES_COUPLED_SETUP stage=pre_create ready=%u in_flight=%u coupled=%u rom=%u generation=%llu cpu_cycles=%llu region=%u mapper=%u video_bound=%u video_pending=%u video_renderer=%u audio_bound=%u\n",
        unsigned(ownerReady),unsigned(worker_&&worker_->inFlight),unsigned(worker_&&worker_->coupled),unsigned(activeRom_!=nullptr),
        static_cast<unsigned long long>(sessionGeneration_),static_cast<unsigned long long>(joined?machine_.cpuCycles():~0ULL),
        joined?unsigned(machine_.region()):255U,unsigned(current_.mapper),unsigned(worker_&&worker_->videoBound),unsigned(worker_&&worker_->videoPending),
        unsigned(worker_&&worker_->videoRenderer),unsigned(joined&&machine_.splitAudio()));
    if(!ownerReady||worker_->inFlight||worker_->coupled||!activeRom_||!sessionGeneration_||machine_.cpuCycles()!=7)return false;
    const auto&startupAudio=worker_->activeAudio->header();
    std::printf("NES_AUDIO_STARTUP input=0 cpu_cycles=%llu ppu_frame=%llu generation=%llu sequence=%llu base=%llu end=%llu inputs=%u dmc=%u region=%u caps=%u expected_sequence=0 expected_base=0\n",
        static_cast<unsigned long long>(machine_.cpuCycles()),static_cast<unsigned long long>(startupAudio.ppuFrame),
        static_cast<unsigned long long>(startupAudio.generation),static_cast<unsigned long long>(startupAudio.sequence),
        static_cast<unsigned long long>(startupAudio.baseCycle),static_cast<unsigned long long>(startupAudio.endCycle),
        unsigned(startupAudio.inputCount),unsigned(startupAudio.dmcCount),unsigned(startupAudio.region),unsigned(startupAudio.capabilities));
    std::uint16_t*alternateRgb=nullptr;
    if(current_.mapper==5){alternateRgb=board_.gameFramebuffer();if(!alternateRgb)return false;}
    worker_->coupled=CoupledVideoPipeline::create(machine_,activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_,alternateRgb);
    std::printf("NES_COUPLED_SETUP stage=post_create ok=%u main_stack_free=%u\n",unsigned(worker_->coupled!=nullptr),unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    if(!worker_->coupled)return false;
    if(worker_->coupled->rgbPoolEnabled()&&!board_.registerGameSource(worker_->coupled->ownedRgb())){
        auto*c=worker_->coupled;c->stop();c->parkMain();machine_.reset();static_cast<void>(machine_.cartridge());
        if(machine_.configureThinPpu(nullptr)&&CoupledVideoPipeline::destroyJoined(c))worker_->coupled=nullptr;
        return false;
    }
    if(!configureC6AudioBackend())return false;
    ESP_LOGI(tag,"NES_COUPLED_BENCH_MODE mapper=%u producer=signals consumer=actual_core rgb=staging maximum_video_burst=%u game_fps_claim=0",
        unsigned(current_.mapper),coupled_done::CoupledPump::maximumVideoBurst);return true;
}
/**
 * @brief Получает завершённый сигнальный кадр Coupled и проверяет его совместный цветной результат. Не подменяет обычный Frame-тракт.
 */
bool NesApp::retireCoupledFrame() noexcept {
    auto&w=*worker_;auto&c=*w.coupled;const auto begin=esp_timer_get_time();
    coupled_done::WaitStats waitStats{}; // На stack Main; нет общей горячей cache-line с Worker.
    coupled_done::CoupledPump service{};
    const auto joined=coupled_done::waitDone<1>(waitStats,
        [&]()noexcept{return xSemaphoreTake(w.done,0)==pdTRUE;},
        [&]()noexcept{
            return service.afterVideo(c.pump(),[&]()noexcept{return pumpC6AudioBackend(false);});
        },
        [&]()noexcept{return c.expired();},
        []()noexcept{std::atomic_signal_fence(std::memory_order_seq_cst);});
    if(joined!=coupled_done::WaitResult::Ready){
        std::printf("NES_COUPLED_DONE_POLL_FAULT batch=%u loops=%u pumps=%u sem_attempts=%u forced_attempts=%u tokens=%u overflow=%u owner_retained=1\n",
            unsigned(1),unsigned(waitStats.loops),unsigned(waitStats.pumpCalls),unsigned(waitStats.semAttempts),unsigned(waitStats.forcedAttempts),unsigned(waitStats.tokens),unsigned(waitStats.overflow));
        workerFault("coupled_join");return false;
    }
    // Ready означает только полученный sem-token: прежние generation/result/cut guards ниже обязательны.
    w.coupledDoneLoops+=waitStats.loops;w.coupledDonePumps+=waitStats.pumpCalls;w.coupledDoneAttempts+=waitStats.semAttempts;
    w.coupledDoneForced+=waitStats.forcedAttempts;w.coupledDoneTokens+=waitStats.tokens;w.coupledDoneOverflow|=waitStats.overflow;
    w.inFlight=false;
    const auto region=current_.pal?core::Region::Pal:core::Region::Ntsc;
    if(w.result!=core::RunStatus::FrameReady||w.generation!=sessionGeneration_||w.inputFrame!=frames_||
       !c.matches(machine_,activeRom_,activeRomBytes_,region,sessionGeneration_)) {workerFault("coupled_join_identity");return false;}
    const auto cycle=machine_.cpuCycles(),frame=machine_.frames();
    if(!videoWorkerReady()||machine_.region()!=region||frame!=frames_+1||!c.jointReady(cycle,frame)){workerFault("coupled_joint");return false;}
    auto&s=*w.snapshot;s.ready=false;s.generation=sessionGeneration_;s.completedFrames=frames_+1;
    s.region=region;s.inputPad=w.inputPad;s.ppuFrame=frame;s.cpuCycles=cycle;s.sampleCount=0;
    if(w.audioPacketPending){workerFault("coupled_audio_pending");return false;}
    const bool preAck=c.preAckDspEnabled();
    if(w.c6Audio){c6audio::async::JoinedFrame joined{};if(!c.acceptRemoteAudioJoined(machine_,w.inputFrame,joined)||!w.c6Audio->session().noteJoined(joined)){workerFault("c6_audio_join");return false;}}
    else
    if(preAck){std::uint32_t count=0;if(!c.acceptPreAckDspJoined(machine_,w.inputFrame,count)){workerFault("coupled_dsp_join");return false;}s.sampleCount=count;}
    else if(!machine_.retireAudioPacket(*w.retiredAudio)){workerFault("coupled_audio_retire");return false;}
    const auto header=w.retiredAudio->header();s.audioGeneration=machine_.audioGeneration();s.audioSequence=header.sequence;

    if(header.region!=region||header.ppuFrame!=frame||header.endCycle!=cycle){workerFault("coupled_audio_cut");return false;}
    w.audioPacketPending=w.packetForSnapshot=!preAck;
    // При loan PCM уже принадлежит присоединённому Main; baseline DSP остаётся прежним.
    if(!preAck&&!prepareSnapshotAudio())return false;
    if(!c.matches(machine_,activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)||
       s.generation!=sessionGeneration_||!c.accept()){workerFault("coupled_accept");return false;}
    ++frames_;++periodFrames_;advanceFrameDeadline();coreUs_+=w.coreUs;maxCoreUs_=std::max(maxCoreUs_,w.coreUs);inputUs_+=w.inputUs;
    w.coupledPumpUs+=esp_timer_get_time()-begin;const auto&t=c.times();
    w.coupledTimes.consumer+=t.consumer;w.coupledTimes.read+=t.read;w.coupledTimes.maxRead=std::max(w.coupledTimes.maxRead,t.maxRead);
    w.coupledTimes.prefix+=t.prefix;w.coupledTimes.reads+=t.reads;w.coupledTimes.prefixes+=t.prefixes;w.coupledTimes.commit+=t.commit;w.coupledTimes.workerDsp+=t.workerDsp;w.coupledTimes.ackReadyBeforeDsp+=t.ackReadyBeforeDsp;w.coupledTimes.dspFrames+=t.dspFrames;
    // Все значения получены после sem/jointReady/identity/accept: Main не читает live Worker fields.
    w.coupledTimes.remoteStage+=t.remoteStage;w.coupledTimes.remoteStageMax=std::max(w.coupledTimes.remoteStageMax,t.remoteStageMax);
    w.coupledTimes.remoteHeld+=t.remoteHeld;w.coupledTimes.remoteHeldMax=std::max(w.coupledTimes.remoteHeldMax,t.remoteHeldMax);
    w.coupledTimes.remoteHeldEntries+=t.remoteHeldEntries;w.coupledTimes.remoteHeldExits+=t.remoteHeldExits;
    w.coupledTimes.remoteStageFrames+=t.remoteStageFrames;w.coupledTimes.remoteClockErrors+=t.remoteClockErrors;
    s.ready=true;return true;
}
/**
 * @brief Выдаёт подтверждённый Coupled RGB в Board и PCM в I2S. Срок жизни источника включает незавершённую аппаратную передачу.
 */
void NesApp::presentCoupledFrame() noexcept {
    presentCoupledFrameAsync();return;
}
/**
 * @brief Печатает диагностический Coupled-срез и область его допустимости; CPU throughput не превращается в игровую приёмку.
 */
void NesApp::coupledVideoStatus() noexcept {
    if(executionMode_==ExecutionMode::Faulted||(worker_&&worker_->inFlight)){std::printf("NES_COUPLED_BENCH_ERROR reason=worker_not_idle\n");return;}
    if(!finishCoupledPresent()){workerFault("coupled_status_ppa");return;}
    auto&w=*worker_;auto&c=*w.coupled;
    if(!prepareSnapshotAudio())return;
    if((playing_||pausedGame_)&&!c.matches(machine_,activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)){workerFault("coupled_status_lease");return;}
    const auto cpu=machine_.cpuState();std::uint64_t ram=1469598103934665603ULL;
    for(unsigned i=0;i<2048;++i)ram=(ram^machine_.debugRead(i))*1099511628211ULL;
    for(unsigned i=0x6000;i<0x8000;++i)ram=(ram^machine_.debugRead(i))*1099511628211ULL;
    const bool remoteAudio=w.c6Audio!=nullptr;
    const auto count=!remoteAudio&&frames_?w.audioRenderer->sampleCount():0U;const auto pcm=remoteAudio?0ULL:pcmStateHash(w.audioRenderer->samples(),count);
    std::printf("NES_AUDIO_PCM_WITNESS game=%u guest_frames=%llu qualified=0 ready_verified_frames=0 count_matches=0 fixed_crc_matches=0 golden_available=0\n",unsigned(selected_),static_cast<unsigned long long>(frames_));
    std::uint64_t rgb=1469598103934665603ULL;const auto*p=
        c.matches(machine_,activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)?c.pixels():nullptr;
    if(p)for(unsigned i=0;i<61440;++i){rgb=(rgb^(p[i]&255U))*1099511628211ULL;rgb=(rgb^(p[i]>>8))*1099511628211ULL;}
    const bool pad=frames_&&w.snapshot->generation==sessionGeneration_&&w.snapshot->completedFrames==frames_;
    std::printf("NES_COUPLED_BENCH_STATUS {\"playing\":%s,\"game\":%u,\"frames\":%llu,\"generation\":%llu,\"mapper\":%u,\"region\":\"%s\",\"crc32\":\"%08lx\",\"owner\":\"%s\",\"game_fps_claim\":false,\"pc\":%u,\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,\"p\":%u,\"cpu_cycles\":%llu,\"cpu_state_cycles\":%llu,\"ram_hash\":\"%016llx\",\"pcm_count\":%u,\"pcm_hash\":\"%016llx\",\"image_available\":%s,\"rgb_hash\":\"%016llx\",\"input_valid\":%s,\"input_frame\":%llu,\"input_mask\":%u,\"submitted_frames\":%llu,\"device_us\":%lld}\n",
        playing_?"true":"false",unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(sessionGeneration_),unsigned(current_.mapper),current_.pal?"PAL":"NTSC",static_cast<unsigned long>(current_.crc),
        current_.mapper==160U?"thin_oracle":"thin_signals",
        unsigned(cpu.pc),unsigned(cpu.a),unsigned(cpu.x),unsigned(cpu.y),unsigned(cpu.sp),unsigned(cpu.p),static_cast<unsigned long long>(machine_.cpuCycles()),static_cast<unsigned long long>(cpu.cycles),
        static_cast<unsigned long long>(ram),unsigned(count),static_cast<unsigned long long>(pcm),p?"true":"false",static_cast<unsigned long long>(p?rgb:0),pad?"true":"false",static_cast<unsigned long long>(pad?frames_-1:0),pad?unsigned(w.snapshot->inputPad):0,static_cast<unsigned long long>(submittedFrames_),static_cast<long long>(esp_timer_get_time()));
}
/**
 * @brief Выдаёт окно времён Coupled-производителя, Main-потребителя, DSP и вывода без суммирования перекрывающихся интервалов.
 */
void NesApp::reportCoupledVideo() noexcept {
    if(!playing_||periodFrames_<120||worker_->inFlight||executionMode_!=ExecutionMode::Worker)return;
    bool reportSkippedDrain=false;
    // Только joined Main: обычный pending RGB reader остаётся заимствованным до next-frame/final finish.
    auto&pending=*worker_;
    if(pending.coupled&&pending.coupled->rgbPoolEnabled()&&pending.presentPending&&pending.presentSource.valid()){
        const auto&source=pending.presentSource;
        if(pending.presentFailed||pending.presentGeneration!=sessionGeneration_||pending.presentFrame!=frames_||
           source.generation!=sessionGeneration_||source.frame!=frames_||!frames_||source.inputFrame!=frames_-1||
           source.region!=std::uint8_t(current_.pal?core::Region::Pal:core::Region::Ntsc)||
           !platform::sameGameSource(source,pending.coupled->acceptedSource())){
            workerFault("coupled_report_pending_identity");return;
        }
        reportSkippedDrain=true;
    }
    if(!reportSkippedDrain&&!finishCoupledPresent()){workerFault("coupled_report_ppa");return;}
    auto&w=*worker_;const auto n=periodFrames_;const auto&t=w.coupledTimes;
    // Та же device-clock publication boundary: после optional drain, ДО всех строк отчёта.
    const auto reportDeviceUs=esp_timer_get_time();
    ESP_LOGI(tag,"NES_COUPLED_BENCH game=%u frames=%llu produced_frames=%llu accepted_frames=%llu submitted_frames=%llu samples=%llu generation=%llu mapper=%u region=%s producer_wall_us=%lld max_producer_wall_us=%lld consumer_active_us=%llu read_wall_us=%llu max_read_wall_us=%llu reads=%llu prefixes=%llu prefix_wall_us=%llu commit_wall_us=%llu joined_main_us=%llu rgb_copy_us=%llu dsp_us=%lld present_us=%lld audio_us=%lld input_us=%lld device_us=%lld game_fps_claim=0",
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(w.snapshot->completedFrames),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(submittedFrames_),static_cast<unsigned long long>(n),static_cast<unsigned long long>(sessionGeneration_),unsigned(current_.mapper),current_.pal?"PAL":"NTSC",static_cast<long long>(coreUs_/n),static_cast<long long>(maxCoreUs_),
        static_cast<unsigned long long>(t.consumer/n),static_cast<unsigned long long>(t.read/n),static_cast<unsigned long long>(t.maxRead),static_cast<unsigned long long>(t.reads),static_cast<unsigned long long>(t.prefixes),static_cast<unsigned long long>(t.prefix/n),static_cast<unsigned long long>(t.commit/n),static_cast<unsigned long long>(w.coupledPumpUs/n),static_cast<unsigned long long>(w.coupledCopyUs/n),static_cast<long long>(w.audioDspUs/n),static_cast<long long>(presentUs_/n),static_cast<long long>(audioUs_/n),static_cast<long long>(inputUs_/n),static_cast<long long>(reportDeviceUs));
    const auto&source=w.presentSource;const bool sourceValid=source.valid();
    std::printf("NES_COUPLED_REPORT_ENDPOINT schema=1 report_no_drain=%u skipped_drain=%u game=%u frames=%llu produced_frames=%llu accepted_frames=%llu submitted_frames=%llu samples=%llu generation=%llu region=%s device_us=%lld pending_depth=%u pending_generation=%llu pending_frame=%llu source_valid=%u source_addr=%p source_generation=%llu source_epoch=%llu source_sequence=%llu source_input=%llu source_frame=%llu source_begin=%llu source_end=%llu source_slot=%u source_region=%u physical_LCD_claim=0\n",
        unsigned(1),unsigned(reportSkippedDrain),unsigned(selected_),
        static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(w.snapshot->completedFrames),
        static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(submittedFrames_),static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(sessionGeneration_),current_.pal?"PAL":"NTSC",static_cast<long long>(reportDeviceUs),
        unsigned(w.presentPending),static_cast<unsigned long long>(w.presentGeneration),static_cast<unsigned long long>(w.presentFrame),
        unsigned(sourceValid),static_cast<const void*>(sourceValid?source.pixels:nullptr),
        static_cast<unsigned long long>(sourceValid?source.generation:0),static_cast<unsigned long long>(sourceValid?source.epoch:0),
        static_cast<unsigned long long>(sourceValid?source.sequence:0),static_cast<unsigned long long>(sourceValid?source.inputFrame:0),
        static_cast<unsigned long long>(sourceValid?source.frame:0),static_cast<unsigned long long>(sourceValid?source.beginCycle:0),
        static_cast<unsigned long long>(sourceValid?source.endCycle:0),sourceValid?unsigned(source.slot):255U,sourceValid?unsigned(source.region):255U);
    ESP_LOGI(tag,"NES_COUPLED_DSP_OVERLAP game=%u frames=%llu samples=%llu worker_dsp_us=%llu dsp_frames=%llu ack_ready_before_dsp=%llu main_dsp_us=%lld result=joined_only",
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(t.workerDsp/n),static_cast<unsigned long long>(t.dspFrames),
        static_cast<unsigned long long>(t.ackReadyBeforeDsp),static_cast<long long>(w.audioDspUs/n));
    std::printf("NES_COUPLED_DONE_POLL game=%u frames=%llu samples=%llu generation=%llu batch=%u loops=%llu pump_calls=%llu sem_attempts=%llu forced_attempts=%llu tokens=%llu overflow=%u actual_sem_only=1\n",
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(n),static_cast<unsigned long long>(sessionGeneration_),unsigned(1),
        static_cast<unsigned long long>(w.coupledDoneLoops),static_cast<unsigned long long>(w.coupledDonePumps),static_cast<unsigned long long>(w.coupledDoneAttempts),static_cast<unsigned long long>(w.coupledDoneForced),static_cast<unsigned long long>(w.coupledDoneTokens),unsigned(w.coupledDoneOverflow));
    reportC6AudioWindow(n,t.remoteStageFrames,t.remoteStage,t.remoteStageMax,t.remoteHeldEntries,t.remoteHeldExits,t.remoteHeld,t.remoteHeldMax,t.remoteClockErrors);
    w.coupledDoneLoops=w.coupledDonePumps=w.coupledDoneAttempts=w.coupledDoneForced=w.coupledDoneTokens=0;w.coupledDoneOverflow=false;
    resetPerformanceWindow();
}

