/** @file C6Audio.inl
 * @brief Полный C6 binding, bounded pump, PCM output, drain и checked UNBIND.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Завершает и уничтожает прежний Coupled-контекст только после настоящего join и остановки Main-потребителя.
 */
bool NesApp::releaseCoupledForLoad() noexcept {
    if(!worker_||!worker_->coupled)return true;
    if(worker_->inFlight||executionMode_!=ExecutionMode::Worker)return false;
    if(!finishCoupledPresent())return false;
    auto*c=worker_->coupled;c->stop();c->parkMain();
    // Владелец кадра присоединён. Reset выполняется, пока endpoint/ROM ещё живы,
    // затем явно отсоединяется до освобождения памяти либо старой ROM.
    if(!drainC6AudioBackend()||!c->abandonPreAckDspJoined(machine_))return false;
    if(!releaseC6AudioForLoad())return false;
    machine_.reset();static_cast<void>(machine_.cartridge());
    if(!machine_.configureThinPpu(nullptr))return false;
    if(c->rgbPoolEnabled()&&!board_.unregisterGameSource(c->ownedRgb()))return false;
    if(!CoupledVideoPipeline::destroyJoined(c))return false;
    worker_->coupled=nullptr;return true;
}
/**
 * @brief Создаёт отдельный диагностический Coupled-контекст на разрешённой границе. Его область поддержки и результаты не распространяются на весь каталог.
 */

/**
 * @details Joined reset7 создаёт только подтверждённый Batch/ae1c session.
 * Cold HELLO/INIT/BIND завершается до первого guest start. Неизвестный владелец
 * не превращается в LOCAL fallback; boot-long объект сохраняется до reset.
 */
/** @brief Общий joined drain/UNBIND; timeout удерживает owner и весь Frame/Coupled source. */
bool NesApp::releaseC6AudioForLoad()noexcept{
    if(worker_&&worker_->c6Audio){
        if(worker_->inFlight||!drainC6AudioBackend())return false;
        if(worker_->framePipeline&&!worker_->frameC6Audio.empty())return false;
    }
    // Только на actual joined load/reset границе: timeout не разрешает free/reset.
    if(worker_&&worker_->c6Audio&&!worker_->c6Audio->unbound()){
        auto&owner=*worker_->c6Audio;
        if(!owner.beginUnbind(true))return false;
        const auto deadline=esp_timer_get_time()+1500000;
        while(!owner.unbound()){
            if(!owner.pollOne()||esp_timer_get_time()>=deadline){owner.noteTimeout();return false;}
            vTaskDelay(1);
        }
        // Exact ACK/P4 TXdone/RX release проверены adapter. C6 rawDone может быть
        // ещё впереди: новый HELLO допускается только actual firmware ACK-loan gate.
        std::printf("NES_C6_AUDIO_UNBOUND ready=1 old_generation=%llu no_job=1 source_held=0\n",
            static_cast<unsigned long long>(owner.binding().appGeneration));
    }
    return true;
}

/**
 * @brief Связывает текущий cold Frame/Coupled источник с boot-long C6 owner после реального join.
 * @details Проверяет начальный audio cut, режим mapper и внутренний published-контроль, затем выполняет обычный HELLO/bind. Отсутствие или отказ C6 не подменяются локальным DSP и не разрешают повторное использование удержанного источника.
 */
bool NesApp::configureC6AudioBackend()noexcept{
    if(!worker_||worker_->inFlight||!worker_->snapshot||!worker_->activeAudio)return false;
    core::OutputFrameLease frameSource{};
    if(worker_->framePipeline){if(worker_->coupled||!worker_->framePipeline->outputLeaseAtCreate(frameSource))return false;}
    else if(!worker_->coupled)return false;
    if(!core::Cartridge::supportsTimingVideoMapper(current_.mapper))return false;
    auto&w=*worker_;auto*last=reinterpret_cast<const std::uint8_t*>(&w.c6Published)+sizeof(w.c6Published)-1;
    if(!esp_ptr_in_dram(&w.c6Published)||!esp_ptr_in_dram(last)||!esp_ptr_internal(&w.c6Published)||!esp_ptr_internal(last))return false;
    const auto&h=w.activeAudio->header();if(h.sequence||h.baseCycle||h.ppuFrame||h.generation!=machine_.audioGeneration())return false;
    auto session=(std::uint64_t(esp_random())<<32U)|esp_random();if(!session)session=1;
    c6audio::service::Binding binding{sessionGeneration_,sessionGeneration_,machine_.colourTransportEpoch(),session,machine_.audioGeneration(),1,
        current_.pal?core::Region::Pal:core::Region::Ntsc,h.capabilities};
    // Нельзя сбрасывать old PCM witness до actual final-drain/UNBIND ACK.
    // Новый Main observer создаётся ниже только после успешного fresh/rebound ready.
    // Product C6 всегда использует принятый Batch protocol; старого DIAG selector нет.
    constexpr auto backend=c6audio::async::BatchClient::Backend::Batch;
    const bool rebound=w.c6Audio!=nullptr;
    if(rebound){
        if(!w.c6Audio->unbound()||!w.c6Audio->rebindEmpty(binding,true))return false;
    }else{
        c6audio::async::RawProfile qualified{};
        qualified.elf=c6_async::batch_image::warmElf; // Принимается ELF выбранного worker C6.
        qualified.hello={binding.appGeneration,binding.romLease,binding.session,0,0,0,0,0,0,std::uint8_t(binding.region)};
        qualified.expectedCapabilities=::nes::c6raw::helloCapabilities|c6audio::lifecycle::capability;
        qualified.warmIdleUnbind=true; // Legacy ae1c/1ff fail-closed; не скрытый cold reset.
        w.c6Audio=c6_async::MainOwner::createQualified(binding,w.c6Published,w.snapshot->pcm,backend,qualified);
        if(!w.c6Audio)return false;
    }
    w.c6Used=true;const auto deadline=esp_timer_get_time()+10000000;
    while(!w.c6Audio->ready()){
        if(!w.c6Audio->pollOne()||esp_timer_get_time()>=deadline){w.c6Audio->noteTimeout();workerFault("c6_audio_cold");return false;}
        vTaskDelay(1);
    }
    audio::PcmStreamGuard newStream{};
    if(!newStream.bind(binding))return false;
    w.c6PcmStream=newStream;w.c6DispatchedInput=0;
    // Frame ROM/transport epochs и C6 binding.romEpoch — разные проверенные домены.
    if(w.framePipeline&&!w.frameC6Audio.configure(frameSource,binding))return false;
    std::printf("NES_C6_AUDIO_BACKEND ready=1 warm_unbind=1 rebound=%u mapper=%u audio_caps=%u delay_guest_frames=1 local_worker_dsp=0 wire_bytes=8192 extra_pcm_bytes=%u source_arenas=2 backend_batch=%u qualified=%u stream_guard=1\n",
        unsigned(rebound),unsigned(current_.mapper),unsigned(h.capabilities),4096U*(w.c6Audio->session().pcmTicketCapacity()-1U),
        unsigned(backend==c6audio::async::BatchClient::Backend::Batch),unsigned(0));
    if(w.c6Audio->session().pcmTicketCapacity()==3U)std::printf("NES_C6_PCM_QUEUE schema=1 tickets=3 minimum_delay_frames=1 maximum_delay_frames=2 pressure_only=1 no_drop=1 owner=main source_arenas=2 dsp=unchanged default=3\n");
    std::printf("NES_COUPLED_REPORT_DIAG schema=1 ready=1 report_no_drain=%u endpoints=1 next_frame_finish_unchanged=1 final_drain_required=1 watchdog_unchanged=1\n",unsigned(1));
    return true;
}
/**
 * @details Main владеет client/I2S; pump выдаёт не более256 mono16 samples с timeout0.
 * До первой и каждой повторной partial/zero-I2S попытки проверяет whole PCM ticket:
 * offset служит только для восстановления стабильного base pointer, не новой фазы.
 * Optional golden observer выполняется после обязательного stream guard.
 */
bool NesApp::tryPumpC6AudioBackend(bool draining,const char*&reason)noexcept{
    reason=nullptr;
    if(worker_&&worker_->c6Audio){auto&owner=*worker_->c6Audio;
        c6_async::MainPumpSpan probe(worker_->c6PumpCost,owner.stats(),c6PumpCycles);
        if(!owner.pollOne()){reason="c6_audio_poll";return false;}
        if(worker_->framePipeline){
            bool sourceFault=false;
            if(worker_->frameFm)sourceFault=worker_->frameFm->releaseIfReclaimed(worker_->frameC6Audio,*worker_->framePipeline,worker_->c6Published)==
                c6_async::FrameFmBridge<FrameVideoPipeline>::Result::Fault;
            else sourceFault=worker_->frameC6Audio.releaseIfReclaimed(*worker_->framePipeline,worker_->c6Published)==
                c6_async::PostJoinFrameAudio<FrameVideoPipeline>::Result::Fault;
            if(sourceFault){reason="frame_c6_source_release";return false;}
        }
        c6_async::PcmQueue::Output output{};
        if(owner.session().beginOutput(worker_->c6DispatchedInput,draining,output)){
            if(!output.data||output.offset>core::Apu::sampleCapacity||output.samples>core::Apu::sampleCapacity-output.offset){reason="c6_audio_pcm_extent";return false;}
            const auto total=output.offset+output.samples;
            const auto*whole=output.data-output.offset;
            if(!worker_->c6PcmStream.observe(output.key,whole,total)){reason="c6_audio_pcm_stream";return false;}
            const auto count=settings_.muted||!settings_.volume?output.samples:std::min<std::size_t>(output.samples,256);
            const auto begin=esp_timer_get_time();
            const auto accepted=settings_.muted||!settings_.volume?count:board_.writeAudio(output.data,count,0);
            audioUs_+=esp_timer_get_time()-begin;
            worker_->c6PumpCost.noteOutput(!settings_.muted&&settings_.volume,accepted);
            // Нулевое принятие непустого span — обычный backpressure, не CONSUMED/free.
            if((accepted||!output.samples)&&!owner.session().outputAccepted(output.key,accepted)){reason="c6_audio_i2s_key";return false;}
        }
    }
    return true;
}
/** @brief Прежний внешний pump сохраняет fault-семантику вне borrowed consumer. */
bool NesApp::pumpC6AudioBackend(bool draining)noexcept{
    const char*reason=nullptr;if(tryPumpC6AudioBackend(draining,reason))return true;
    workerFault(reason?reason:"c6_audio_poll");return false;
}

/** @details Финальный menu drain идёт после actual Worker join; timeout удерживает всех владельцев до reset. */
/** @brief Общая read-only аудио-телеметрия; нулевые Frame-stage поля не означают Thin callback. */
void NesApp::reportC6AudioWindow(std::uint64_t n,std::uint64_t stages,std::uint64_t stageUs,
    std::uint64_t stageMax,std::uint64_t heldEntries,std::uint64_t heldExits,
    std::uint64_t heldUs,std::uint64_t heldMax,std::uint64_t clockErrors)noexcept{
    if(!n||!worker_)return;
    auto&w=*worker_;
    if(w.c6Audio){const auto&stats=w.c6Audio->stats();
        std::printf("NES_C6_AUDIO_WINDOW game=%u guest_frames=%llu remote_frames=%u remote_calls=%u pcm_chunks=%u pcm_ready_verified=0 count_matches=0 fixed_crc_matches=0 outside_golden=0 pending_pcm=%u qualified=%u stream_verified_frames=%llu input_delay=1 local_dsp=0 golden_ROM_embedded=0 stage_frames=%llu stage_mean_us=%llu stage_max_us=%llu held_entries=%llu held_exits=%llu held_mean_us=%llu held_max_us=%llu observer_clock_errors=%llu observer_wall=1 pump_calls=%llu pump_cycles=%llu pump_max_cycles=%llu pump_no_rpc_calls=%llu pump_no_rpc_cycles=%llu pump_changed_calls=%llu pump_changed_cycles=%llu pump_no_rpc_no_i2s_calls=%llu pump_no_rpc_no_i2s_cycles=%llu pump_i2s_calls=%llu pump_i2s_samples=%llu pump_sink_samples=%llu pump_cycle_hz=%llu pump_modulo_bits=32 pump_preemption_wall=1 pump_no_rpc_is_not_pure_pending=1 pump_overhead_subtracted=0\n",
            unsigned(selected_),static_cast<unsigned long long>(frames_),unsigned(stats.frames),unsigned(stats.calls),unsigned(stats.pcmChunks),unsigned(w.c6Audio->session().occupiedPcm()),unsigned(0),static_cast<unsigned long long>(w.c6PcmStream.frames()),
            static_cast<unsigned long long>(stages),static_cast<unsigned long long>(stageUs/n),static_cast<unsigned long long>(stageMax),
            static_cast<unsigned long long>(heldEntries),static_cast<unsigned long long>(heldExits),
            static_cast<unsigned long long>(heldUs/n),static_cast<unsigned long long>(heldMax),static_cast<unsigned long long>(clockErrors),
            static_cast<unsigned long long>(w.c6PumpCost.calls),
            static_cast<unsigned long long>(w.c6PumpCost.cycles),
            static_cast<unsigned long long>(w.c6PumpCost.maxCycles),
            static_cast<unsigned long long>(w.c6PumpCost.noRpcCalls),
            static_cast<unsigned long long>(w.c6PumpCost.noRpcCycles),
            static_cast<unsigned long long>(w.c6PumpCost.changedCalls),
            static_cast<unsigned long long>(w.c6PumpCost.changedCycles),
            static_cast<unsigned long long>(w.c6PumpCost.noRpcNoI2sCalls),
            static_cast<unsigned long long>(w.c6PumpCost.noRpcNoI2sCycles),
            static_cast<unsigned long long>(w.c6PumpCost.i2sCalls),
            static_cast<unsigned long long>(w.c6PumpCost.i2sSamples),
            static_cast<unsigned long long>(w.c6PumpCost.sinkSamples),
            static_cast<unsigned long long>(esp_rom_get_cpu_ticks_per_us())*1000000ULL);
        const auto&s=w.c6Audio->session();
        if(s.pcmTicketCapacity()==3U)std::printf("NES_C6_PCM_PRESSURE schema=1 generation=%llu frames=%llu tickets=3 occupied=%u peak_occupied=%u max_output_lag=%u minimum_delay_frames=1 maximum_delay_frames=2 maxima_since=bind actual_dispatch=existing no_drop=1\n",
            static_cast<unsigned long long>(sessionGeneration_),static_cast<unsigned long long>(frames_),unsigned(s.occupiedPcm()),s.peakOccupiedPcm(),s.maximumOutputLag());}
}

/**
 * @brief После join доводит связанную аудиосессию до настоящего final drain, сохраняя владельца при ошибке.
 * @details inFlight запрещает drain; уже подтверждённый Unbound не запускает новый. Bounded pump ждёт одинаковые guest/stream/remote cuts, а timeout помечает owner и не выдаёт право free или повторного bind.
 * @return true при отсутствии C6 owner либо подтверждённом drain/Unbound; false требует удержать источники и обработать fault.
 */
bool NesApp::drainC6AudioBackend()noexcept{
    if(worker_&&worker_->c6Audio){if(worker_->inFlight)return false;
        // Повторный Library/stop после exact UNBIND ACK не начинает final drain
        // уже отвязанной сессии; timeout/poison никогда не считается Unbound.
        if(worker_->c6Audio->unbound())return true;
        if(!worker_->c6Audio->session().drained()&&!worker_->c6Audio->session().requestFinalDrain(worker_->c6Audio->session().lastJoined(),!worker_->inFlight))return false;
        const auto deadline=esp_timer_get_time()+1500000;
        while(!worker_->c6Audio->session().drained()){
            if(!pumpC6AudioBackend(true)||esp_timer_get_time()>=deadline){worker_->c6Audio->noteTimeout();std::printf("NES_C6_AUDIO_DRAIN success=0 owner_retained=1 actual_join=1\n");return false;}
            vTaskDelay(1);
        }
        const auto&stats=worker_->c6Audio->stats();
        std::printf("NES_C6_AUDIO_DRAIN success=1 guest_frames=%llu remote_frames=%u pcm_verified=0 pending_pcm=%u publication_held=%u actual_join=1 local_renderer_used=0 qualified=%u stream_verified_frames=%llu\n",
            static_cast<unsigned long long>(frames_),unsigned(stats.frames),unsigned(worker_->c6Audio->session().occupiedPcm()),unsigned(worker_->c6Published.held()),unsigned(0),static_cast<unsigned long long>(worker_->c6PcmStream.frames()));
    }
    return true;
}

