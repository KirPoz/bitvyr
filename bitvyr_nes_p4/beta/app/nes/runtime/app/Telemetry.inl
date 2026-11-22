/** @file Telemetry.inl
 * @brief Исполнение игрового шага и read-only status/performance завершённых кадров.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Выполняет шаг Worker-тракта: ввод, retire/dispatch, цвет, звук и вывод после проверки владельца.
 */
void NesApp::frame() noexcept { NES_APP_TOUCH_PREFETCH
    if (!retireFrame() || !playing_) return;
    if(executionMode_!=ExecutionMode::Worker||!worker_){workerFault("cpu_bench_worker");return;}
    NES_LOOP_PHASE(Pace)
    paceFrame();
    NES_LOOP_PHASE(Input)
    const auto frameBegin = esp_timer_get_time();
    applyScheduledPads(); // frames_ завершённых кадров означает следующий индекс ввода NesRun n.
    const auto rawGamepad = gamepad_.buttons();
    suppressedGamepad_ &= rawGamepad;
    const auto effectiveGamepad=settings_.mapGamepadButtons(static_cast<std::uint8_t>(rawGamepad & ~suppressedGamepad_));
    auto buttons = static_cast<std::uint8_t>(effectiveGamepad | serialButtons_);
    std::uint8_t tracedTouch=0;
    platform::TouchContacts touches{};
    bool haveTouch=NES_APP_TOUCH_CONTACTS(board_,touches,current_.pal);
    if(touches.count>platform::TouchContacts::capacity)haveTouch=false;
    std::uint16_t ids=0;
    if(haveTouch)for(unsigned i=0;i<touches.count;++i) {
        if(touches.points[i].id>=16){haveTouch=false;break;}
        ids=static_cast<std::uint16_t>(ids|(1U<<touches.points[i].id));
    }
    if(!haveTouch)suppressTouchUntilRelease_=true; // NES-кнопки сенсора отпускаются; старое/повторное действие UI не переносится.
    else {
        touchHeld_=touches.count!=0;
        if(!touchHeld_)suppressTouchUntilRelease_=false;
        const auto newIds=static_cast<std::uint16_t>(ids&~previousTouchIds_);
        previousTouchIds_=ids;
        if(!suppressTouchUntilRelease_) {
            bool exit=false,settings=false;
            for(unsigned i=0;i<touches.count;++i) {
                const auto& point=touches.points[i];
                const auto hit=gameControls::hit(point.x,point.y);
                buttons=static_cast<std::uint8_t>(buttons|hit.buttons);
                tracedTouch=static_cast<std::uint8_t>(tracedTouch|hit.buttons);
                if(newIds&(1U<<point.id)) {
                    exit=exit||hit.action==gameControls::Action::ExitLibrary;
                    settings=settings||hit.action==gameControls::Action::Settings;
                }
            }
            if(exit||settings){
                traceInput(sessionGeneration_,NES_APP_FRAME_INPUT_INDEX,rawGamepad,effectiveGamepad,serialButtons_,tracedTouch,0,touches.count,ids,haveTouch,suppressTouchUntilRelease_,exit?"exit":"settings");
                openPause(exit);return;
            }
        }
    }
    if(hasGameVideo()&&chromeButtons_!=buttons){
        gameChrome::updateButtons(board_,chromeButtons_,buttons);chromeButtons_=buttons;
    }
    machine_.setController(0,buttons);
    traceInput(sessionGeneration_,NES_APP_FRAME_INPUT_INDEX,rawGamepad,effectiveGamepad,serialButtons_,tracedTouch,buttons,touches.count,ids,haveTouch,suppressTouchUntilRelease_,"none");
    worker_->inputPad=buttons;
    const auto begin = esp_timer_get_time();
    // Режим меняет только Main; touch-переходы выше сразу возвращаются.
    // Повторный guard сохраняет прежний порядок dispatch и end-yield.
    if (executionMode_ == ExecutionMode::Worker) {
        NES_LOOP_PHASE(Dispatch)
        if(dispatchFrame(NES_APP_TOUCH_INPUT_US(begin-frameBegin))){
            NES_LOOP_PHASE(Consume)
            presentSnapshot();
            NES_LOOP_PHASE(Report)
            reportPerformance();
        }
        NES_LOOP_PHASE(Other)
        {
            NES_LOOP_PHASE(Yield)
            vTaskDelay(1);
            NES_LOOP_PHASE(Other)
        }
        return;
    }
}

/**
 * @brief Печатает законченные окна обычного тракта, включая звук/вывод/ожидание. Числа относятся к конкретной конфигурации и сцене.
 */
void NesApp::reportPerformance() noexcept {
    updateGameOverlay();
    if(worker_&&worker_->framePipeline){reportFramePipeline();return;}
    if(!worker_->legacyMachine){
    if(periodFrames_<120||!playing_||executionMode_!=ExecutionMode::Worker||!worker_||worker_->audioFaultPending)return;
    // Следующее задание CPU1 уже работает: читать только завершённые данные Main.
    const auto now=esp_timer_get_time();
    lastAverageCoreUs_=coreUs_/periodFrames_;
    ESP_LOGI(tag,"NES_CPU_BENCH game=%u frames=%llu samples=%llu generation=%llu mapper=%u region=%s owner=observable core=1 core_us=%lld max_core_us=%lld dsp_us=%lld input_us=%lld wait_us=%lld copy_us=%lld audio_us=%lld audio_short=%llu image_available=0 game_fps_claim=0",
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(periodFrames_),
        static_cast<unsigned long long>(sessionGeneration_),unsigned(current_.mapper),current_.pal?"PAL":"NTSC",
        static_cast<long long>(lastAverageCoreUs_),static_cast<long long>(maxCoreUs_),
        static_cast<long long>(worker_->audioDspUs/periodFrames_),static_cast<long long>(inputUs_/periodFrames_),
        static_cast<long long>(workerWaitUs_/periodFrames_),static_cast<long long>(snapshotUs_/periodFrames_),
        static_cast<long long>(audioUs_/periodFrames_),static_cast<unsigned long long>(audioShort_));
    periodStart_=now;periodFrames_=0;coreUs_=renderUs_=maxCoreUs_=0;
    inputUs_=audioUs_=presentUs_=workerWaitUs_=snapshotUs_=0;worker_->audioDspUs=0;
    return;
    }
    if (periodFrames_ >= 120) {
        const auto now = esp_timer_get_time();
        lastAverageCoreUs_ = coreUs_/periodFrames_;
        ESP_LOGI(tag,"NES_PERF game=%u frames=%llu fps=%.2f core_us=%lld render_us=%lld max_core_us=%lld audio_short=%llu display_errors=%llu input_us=%lld audio_us=%lld present_us=%lld wait_us=%lld copy_us=%lld dsp_us=%lld",
                 unsigned(selected_),static_cast<unsigned long long>(frames_),periodFrames_*1000000.0/double(now-periodStart_),
                 static_cast<long long>(coreUs_/periodFrames_),static_cast<long long>(renderUs_/periodFrames_),
                 static_cast<long long>(maxCoreUs_),static_cast<unsigned long long>(audioShort_),
                 static_cast<unsigned long long>(displayErrors_),
                 static_cast<long long>(inputUs_/periodFrames_),
                 static_cast<long long>(audioUs_/periodFrames_),
                 static_cast<long long>(presentUs_/periodFrames_),
                 static_cast<long long>(workerWaitUs_/periodFrames_),
                 static_cast<long long>(snapshotUs_/periodFrames_),
                 static_cast<long long>(worker_?worker_->audioDspUs/periodFrames_:0));
        periodStart_=now; periodFrames_=0; coreUs_=renderUs_=maxCoreUs_=0;
        inputUs_=audioUs_=presentUs_=0;
        workerWaitUs_=snapshotUs_=0;
        if (worker_) worker_->audioDspUs=0;
    }
}

/**
 * @brief Печатает аппаратный и игровой статус из разрешённых снимков. При параллельном кадре использует принятые CPU/RAM/PCM/RGB, а не живые поля Worker.
 */
void NesApp::status() noexcept {
    NES_LOOP_TAINT()
    if(!worker_||!worker_->legacyMachine){videoWorkerStatus();return;}
    // Явный UART status присоединяет полный backend; это дорогой наблюдатель вне FPS-окна.
    if(!retireFrame())return;
    if (executionMode_ == ExecutionMode::Faulted || (worker_ && worker_->inFlight)) {
        std::printf("NES_STATUS_ERROR reason=worker_not_idle\n"); return;
    }
    const char* audioExecution = "synchronous";
    if (!prepareSnapshotAudio()) { std::printf("NES_STATUS_ERROR reason=audio_recovery\n"); return; }
    const auto& s = board_.status();
    std::uint64_t frameHash=0;
    bool frameHashAvailable=true;
    if(worker_&&worker_->videoBound){
        if(!prepareSnapshotVideo(false)){std::printf("NES_STATUS_ERROR reason=video_recovery\n");return;}
        worker_->videoRenderer->setRgb565Output(nullptr);
        // Владелец присоединён. Вызывается лишь явный Recording-prefix
        // наблюдатель, а не прежний синхронный пиксельный backend.
        if(!machine_.framebuffer()){std::printf("NES_STATUS_ERROR reason=video_observer\n");return;}
        worker_->videoRgbReady=false;
        frameHash=1469598103934665603ULL;
        const auto* pixels=worker_->videoRenderer->framebuffer();
        for(unsigned i=0;i<256*240;++i)frameHash=(frameHash^pixels[i])*1099511628211ULL;
        for(unsigned y=0;y<240;++y)frameHash=(frameHash^worker_->videoRenderer->videoEmphasis(y))*1099511628211ULL;
    }else
    frameHash=frameStateHash(machine_,&frameHashAvailable);
    char frameHashJson[19]{};
    if(frameHashAvailable)std::snprintf(frameHashJson,sizeof(frameHashJson),"\"%016llx\"",static_cast<unsigned long long>(frameHash));
    else std::snprintf(frameHashJson,sizeof(frameHashJson),"null");
    std::size_t pcmCount = 0;
    const std::int16_t* pcmSamples = nullptr;
    if (worker_ && worker_->audioSplit) {
        audioExecution = "split";
        pcmCount = worker_->audioRenderer->sampleCount();
        pcmSamples = worker_->audioRenderer->samples();
    } else
    { pcmCount = machine_.sampleCount(); pcmSamples = machine_.samples(); }
    const auto pcmHash = pcmStateHash(pcmSamples,pcmCount);
    std::printf("NES_STATUS {\"playing\":%s,\"game\":%u,\"frames\":%llu,\"execution\":\"%s\",\"audio_execution\":\"%s\",\"submitted_frames\":%llu,\"lcd\":%s,\"touch\":%s,\"audio\":%s,\"sd\":%s,\"psram\":%s,\"xbox\":%s,\"audio_short\":%llu,\"display_errors\":%llu,\"free_internal\":%u,\"free_psram\":%u,\"pc\":%u,\"cpu_cycles\":%llu,\"frame_hash\":%s,\"pcm_count\":%u,\"pcm_hash\":\"%016llx\"}\n",
        playing_?"true":"false",unsigned(selected_),static_cast<unsigned long long>(frames_),
        executionMode_==ExecutionMode::Worker?"worker":"synchronous",audioExecution,static_cast<unsigned long long>(submittedFrames_),s.display?"true":"false",
        s.touch?"true":"false",s.audio?"true":"false",s.storage?"true":"false",s.psram?"true":"false",
        gamepad_.connected()?"true":"false",static_cast<unsigned long long>(audioShort_),static_cast<unsigned long long>(displayErrors_),
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        unsigned(machine_.cpuState().pc),static_cast<unsigned long long>(machine_.cpuCycles()),
        frameHashJson, unsigned(pcmCount),
        static_cast<unsigned long long>(pcmHash));
    std::printf("NES_VIDEO execution=%s pending=%u renderer_internal=%u shadow_psram=%u\n",
        worker_&&worker_->videoBound?"split":"legacy",unsigned(worker_&&worker_->videoPending),unsigned(sizeof(core::VideoRenderer)),unsigned(sizeof(core::VideoStorage)));
    const auto audio = board_.audioStats();
    std::printf("NES_AUDIO active=%u accepted=%lu dma=%lu missed_blocks=%lu missed_frames=%lu max_gap_us=%lu max_wait_us=%lu\n",
        unsigned(audio.active),static_cast<unsigned long>(audio.acceptedFrames),static_cast<unsigned long>(audio.dmaFrames),
        static_cast<unsigned long>(audio.missedRefillBlocks),static_cast<unsigned long>(audio.missedRefillFrames),
        static_cast<unsigned long>(audio.maxWriteGapUs),static_cast<unsigned long>(audio.maxWriteWaitUs));
    settingsStatus();
}

