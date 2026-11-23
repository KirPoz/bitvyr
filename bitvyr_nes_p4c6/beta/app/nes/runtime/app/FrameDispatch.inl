/** @file FrameDispatch.inl
 * @brief Dispatch/retire/presentation с точным разделением кадров N и N+1.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Переводит исполнение в Faulted и отключает опасное продолжение. Не удаляет ресурсы, которыми ещё может владеть CPU1.
 * @param reason Заимствованное диагностическое описание отказа.
 */
void NesApp::workerFault(const char* reason) noexcept {
    NES_LOOP_TAINT()
    if(worker_&&worker_->framePipeline)worker_->framePipeline->stop();
    if (executionMode_ == ExecutionMode::Faulted) return;
    executionMode_ = ExecutionMode::Faulted;
    if(worker_&&worker_->coupled){worker_->coupled->stop();worker_->coupled->parkMain();}
    stopPlayback();
    menuDirty_ = false;
    // Без доступа к Machine/reset/free ROM/удаления Worker: владельцем ещё может быть CPU1.
    ESP_LOGE(tag,"NES_RUNTIME_FAIL reason=worker_%s ownership=retained restart_required=1",reason);
    board_.clear(0x0841);
    board_.drawText(24,100,"EMULATION WORKER STOPPED",0xf800,2);
    board_.drawText(24,150,"RESTART THE DEVICE",0xffff,2);
    static_cast<void>(board_.present());
}

/**
 * @brief Завершает активное задание и обрабатывает принятый результат на Main. Ветви Faulted/диагностического заимствования не считают тайм-аут доказательством park.
 */
bool NesApp::retireFrame() noexcept {
    if (executionMode_ == ExecutionMode::Synchronous) return true;
    if (executionMode_ == ExecutionMode::Faulted) {
        static_cast<void>(finishFramePipelinePresent()); // Поздний drain только Main, без доступа к производителю/actor.
        if(worker_&&worker_->presentPending)static_cast<void>(finishCoupledPresent()); // Поздний DMA-drain только Main; Machine не наблюдается.
        // Поздний done освобождает владение, но не возобновляет отказавший конвейер автоматически.
        if(worker_->inFlight&&xSemaphoreTake(worker_->done,0)==pdTRUE){
            worker_->inFlight=false;
            if(worker_->framePipeline)static_cast<void>(worker_->framePipeline->joinedProducer());
        }
        return false;
    }
    if (!worker_->inFlight) return true;
    if(worker_->framePipeline)return retireFramePipelineFrame();
    if(worker_->coupled)return retireCoupledFrame();
    const auto waitBegin = esp_timer_get_time();
    if (xSemaphoreTake(worker_->done,pdMS_TO_TICKS(1500)) != pdTRUE) {
        workerFault("timeout");
        return false;
    }
    worker_->inFlight = false;
    if(worker_->videoFaultPending)return splitVideoFault("worker_joined");
    if(worker_->videoBound&&worker_->videoPending&&!prepareSnapshotVideo(false))return false;
    if (worker_->audioFaultPending) return splitAudioFault("worker_joined");
    if (worker_->audioSplit && worker_->audioPacketPending && !prepareSnapshotAudio()) return false;
    const auto captureBegin = esp_timer_get_time();
    workerWaitUs_ += captureBegin-waitBegin;
    if (worker_->generation != sessionGeneration_ || worker_->inputFrame != frames_) {
        workerFault("sequence"); return false;
    }
    if (worker_->result != core::RunStatus::FrameReady) {
        if(!worker_->legacyMachine){workerFault("cpu_bench_step"); return false;}
        if(worker_->videoBound&&machine_.videoFault()!=core::VideoFault::None)return splitVideoFault("capture");
        if (worker_->audioSplit && (worker_->result == core::RunStatus::AudioFault ||
            machine_.audioFault() != core::AudioFault::None)) return splitAudioFault("capture");
        ESP_LOGE(tag,"NES_RUNTIME_FAIL status=%u pc=%04x frame=%llu",unsigned(worker_->result),machine_.cpuState().pc,
                 static_cast<unsigned long long>(frames_));
        stopPlayback();
        return true;
    }
    if(!worker_->legacyMachine&&!videoWorkerReady()){workerFault("cpu_bench_owner");return false;}
    const std::uint8_t* legacyPixels=nullptr;
    if(!worker_->videoBound
       &&worker_->legacyMachine
       ){
        legacyPixels=machine_.framebuffer();
        if(!legacyPixels)return legacyVideoFailure("snapshot_video_memory");
    }
    ++frames_; ++periodFrames_;
    advanceFrameDeadline();
    coreUs_ += worker_->coreUs;
    inputUs_ += worker_->inputUs;
    maxCoreUs_ = std::max(maxCoreUs_,worker_->coreUs);
    auto& snapshot = *worker_->snapshot;
    snapshot.inputPad=worker_->inputPad;
    if(worker_->legacyMachine){
    // Порядок getter сначала завершает PPU-debt. Все чтения/копии закончены до нового start.
    if(!worker_->videoBound)
    {
        const auto* pixels=legacyPixels;
        std::memcpy(snapshot.pixels,pixels,sizeof(snapshot.pixels));
        for (unsigned row=0;row<240;++row) snapshot.emphasis[row] = machine_.emphasis(row);
    }
    }
    snapshot.region = machine_.region();
    if (worker_->audioSplit) {
        if (!machine_.retireAudioPacket(*worker_->retiredAudio)) return splitAudioFault("retire_frame");
        const auto& header = worker_->retiredAudio->header();
        snapshot.audioGeneration = machine_.audioGeneration();
        snapshot.audioSequence = header.sequence;
        if(worker_->videoBound){
            snapshot.ppuFrame=header.ppuFrame;snapshot.cpuCycles=header.endCycle;
            if(header.region!=snapshot.region)return splitAudioFault("snapshot_region");
        }else
        {
            snapshot.ppuFrame = machine_.frames();
            snapshot.cpuCycles = machine_.cpuCycles();
        }
        snapshot.sampleCount = 0;
        worker_->audioPacketPending = worker_->packetForSnapshot = true;
    } else
    {
        // После join фиксируется именно этот cut и для полного APU, без старой audio identity.
        snapshot.ppuFrame = machine_.frames();
        snapshot.cpuCycles = machine_.cpuCycles();
        snapshot.audioGeneration = snapshot.audioSequence = 0;
        snapshot.sampleCount = machine_.sampleCount();
        if (snapshot.sampleCount > core::Apu::sampleCapacity) { workerFault("sample_count"); return false; }
        std::memcpy(snapshot.pcm,machine_.samples(),snapshot.sampleCount*sizeof(snapshot.pcm[0]));
    }
    if(worker_->videoBound){
        core::VideoPacket* retired=nullptr;
        if(!worker_->spareVideo||!machine_.swapVideoPacket(*worker_->spareVideo,retired))return splitVideoFault("swap");
        worker_->retiredVideo=retired;worker_->spareVideo=nullptr;
        const auto header=retired->header();
        snapshot.videoGeneration=header.generation;snapshot.videoSequence=header.sequence;
        snapshot.videoFrame=header.ppuFrame;snapshot.videoCycles=header.endCycle;
        if(header.endCycle!=machine_.cpuCycles()||header.ppuFrame!=machine_.frames()||
            header.pal!=(snapshot.region==core::Region::Pal))return splitVideoFault("frontend_cut");
        worker_->videoPending=worker_->videoForSnapshot=true;worker_->videoRgbReady=false;
    }
    if(worker_->videoBound&&worker_->audioSplit&&
        (snapshot.videoFrame!=snapshot.ppuFrame||snapshot.videoCycles!=snapshot.cpuCycles))
        return splitVideoFault("audio_video_cut");
    snapshot.generation = sessionGeneration_;
    snapshot.completedFrames = frames_;
    snapshot.ready = true;
    snapshotUs_ += esp_timer_get_time()-captureBegin;
    return true;
}

/**
 * @brief Публикует ввод/поколение и запускает Worker в свободный слот. Отказ не разрешает Main обращаться к занятой первичной Machine.
 * @param inputUs Измеренное Main-время подготовки ввода, в микросекундах.
 */
bool NesApp::dispatchFrame(std::int64_t inputUs) noexcept {
    if (executionMode_ != ExecutionMode::Worker || worker_->inFlight) return false;
    if(!worker_->legacyMachine&&!videoWorkerReady()){workerFault("cpu_bench_dispatch");return false;}
    
    if(worker_->coupled){
        if(!worker_->coupled->matches(machine_,activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)){workerFault("coupled_dispatch_lease");return false;}
        worker_->coupled->startJob();
        if(worker_->c6Audio){
            const auto&b=worker_->c6Audio->binding();const auto&h=worker_->activeAudio->header();
            c6audio::async::PublishedKey expected{b.appGeneration,b.romLease,b.romEpoch,b.session,NES_APP_FRAME_INPUT_INDEX,
                {b.audioGeneration,h.sequence,NES_APP_FRAME_INPUT_INDEX+1U,h.baseCycle,0,b.region,b.audioCapabilities},1};
            if(!worker_->coupled->offerRemoteAudio(machine_,*worker_->retiredAudio,worker_->preAckAudioToken,worker_->c6Published,expected)){workerFault("c6_audio_offer");return false;}
        }else
        if(worker_->coupled->preAckDspEnabled()&&
           !worker_->coupled->offerPreAckDsp(machine_,*worker_->retiredAudio,*worker_->audioRenderer,
               worker_->snapshot->pcm,sizeof(worker_->snapshot->pcm)/sizeof(worker_->snapshot->pcm[0]),
               worker_->preAckAudioToken,NES_APP_FRAME_INPUT_INDEX)){workerFault("coupled_dsp_offer");return false;}
    }
    if(worker_->framePipeline&&(!worker_->framePipeline->matches(activeRom_,activeRomBytes_,
       current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)||
       !worker_->framePipeline->prepareJob(NES_APP_FRAME_INPUT_INDEX,worker_->inputPad,sessionGeneration_))){
        workerFault("frame_pipeline_dispatch");return false;
    }
    worker_->inputFrame = NES_APP_FRAME_INPUT_INDEX;
    worker_->generation = sessionGeneration_;
    worker_->inputUs = inputUs;
    if(worker_->c6Audio)worker_->c6DispatchedInput=worker_->inputFrame;
    worker_->inFlight = true;
    if (xSemaphoreGive(worker_->start) != pdTRUE) {
        workerFault("start_signal"); return false;
    }
    return true;
}

/**
 * @brief Выводит принятый снимок изображения и PCM на Main. Источник живёт до завершения PPA и не читается непосредственно у занятого Worker.
 */
void NesApp::presentSnapshot() noexcept {
    if(worker_&&worker_->framePipeline){static_cast<void>(consumeFramePipelineFrame(true));return;}
    if(worker_&&worker_->coupled){presentCoupledFrame();return;}
    if(!worker_->legacyMachine){
    if(!prepareSnapshotAudio())return;
    auto& snapshot=*worker_->snapshot;
    if(!snapshot.ready||snapshot.generation!=sessionGeneration_||!playing_)return;
    const auto begin=esp_timer_get_time();
    if(!settings_.muted&&settings_.volume>0)
        audioShort_+=snapshot.sampleCount-board_.writeAudio(snapshot.pcm,snapshot.sampleCount);
    audioUs_+=esp_timer_get_time()-begin;
    snapshot.ready=false;
    return;
    }
    auto& snapshot = *worker_->snapshot;
    if (!snapshot.ready || snapshot.generation != sessionGeneration_ || !playing_) {
        // Сохраняется обычная обработка initial/status-пакетов без обращения
        // к Machine при работающем CPU1. Нового video-submit для перекрытия здесь нет.
        static_cast<void>(prepareSnapshotAudio());
        return;
    }
    const auto begin = esp_timer_get_time();
    if(worker_->videoBound){if(!prepareSnapshotVideo(true))return;}else
    if (auto* screen = board_.gameFramebuffer()) {
        const bool pal = snapshot.region == core::Region::Pal;
        for (unsigned y=0;y<240;++y) {
            auto emphasis = snapshot.emphasis[y];
            if (pal) emphasis = static_cast<std::uint8_t>((emphasis&4)|((emphasis&1)<<1)|((emphasis&2)>>1));
            const auto* palette = palette_[emphasis & 7];
            for (unsigned x=0;x<256;++x) screen[y*256+x] = palette[snapshot.pixels[y*256+x] & 63];
        }
    }
    const auto rendered = esp_timer_get_time();
    const bool started = board_.beginGamePresent();
    const auto submitted = esp_timer_get_time();
    bool audioReady = true;
    audioReady = prepareSnapshotAudio(); // Тот же неизменяемый пакет, что соответствует этому RGB-кадру.
    const auto dspDone = esp_timer_get_time();
    // Отказ DSP не должен оставлять успешно запущенную DMA-транзакцию без завершения.
    // Тайм-аут сохраняет GPU-владение в Board и запрещает переиспользование RGB.
    const bool finished = board_.finishGamePresent();
    const auto presented = esp_timer_get_time();
    if (!started || !finished) ++displayErrors_; else ++submittedFrames_;
    if (!audioReady) {
        snapshot.ready = false; return;
    }
    if (!settings_.muted && settings_.volume>0) audioShort_ += snapshot.sampleCount-board_.writeAudio(snapshot.pcm,snapshot.sampleCount);
    const auto audioDone = esp_timer_get_time();
    renderUs_ += rendered-begin;
    presentUs_ += (submitted-rendered)+(presented-dspDone);
    audioUs_ += audioDone-presented;
    snapshot.ready = false;
}

