/** @file Console.inl
 * @brief Ограниченный UART command/input movie parser без JIT/PGO/SD probe.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Сбрасывает сборку незаконченной UART-строки и её overflow-флаг. Гостевой ввод и очередь pad-событий меняются отдельно.
 */
void NesApp::clearSerialInput() noexcept {
    serialButtons_ = 0;
    padEventCount_ = 0;
}

/**
 * @brief Проверяет и ставит ограниченное событие serial-pad на заданный гостевой кадр. Неверный формат и переполнение очереди не меняют игровое управление.
 * @param arguments Остаток UART-команды с десятичным номером кадра и маской.
 */
void NesApp::schedulePad(const char* arguments) noexcept {
    std::uint64_t frame = 0;
    std::uint8_t mask = 0;
    if (!parsePadArguments(arguments, frame, mask)) {
        std::printf("NES_PAD_ERROR reason=arguments\n");
        return;
    }
    if (!playing_) {
        std::printf("NES_PAD_ERROR reason=not_playing\n");
        return;
    }
    if (frame < NES_APP_FRAME_INPUT_INDEX) {
        std::printf("NES_PAD_ERROR reason=past_frame frame=%llu current=%llu\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(NES_APP_FRAME_INPUT_INDEX));
        return;
    }
    if (padEventCount_ == maxPadEvents) {
        std::printf("NES_PAD_ERROR reason=queue_full\n");
        return;
    }
    unsigned position = padEventCount_;
    // Строгое сравнение сохраняет FIFO среди событий одного кадра.
    while (position && padEvents_[position - 1].frame > frame) {
        padEvents_[position] = padEvents_[position - 1];
        --position;
    }
    padEvents_[position] = {frame, mask};
    ++padEventCount_;
    std::printf("NES_PAD_SCHEDULE frame=%llu mask=%u queued=%u\n",
                static_cast<unsigned long long>(frame), unsigned(mask), padEventCount_);
}

/**
 * @brief Применяет подошедшие события к маске UART на точной кадровой границе. Будущие события сохраняются, обработанные удаляются из ограниченной очереди.
 */
void NesApp::applyScheduledPads() noexcept {
    unsigned consumed = 0;
    while (consumed < padEventCount_ && padEvents_[consumed].frame <= NES_APP_FRAME_INPUT_INDEX) {
        const auto& event = padEvents_[consumed++];
        serialButtons_ = event.mask;
        std::printf("NES_PAD_APPLY frame=%llu scheduled=%llu mask=%u\n",
                    static_cast<unsigned long long>(NES_APP_FRAME_INPUT_INDEX),
                    static_cast<unsigned long long>(event.frame), unsigned(event.mask));
    }
    if (!consumed) return;
    for (unsigned i = consumed; i < padEventCount_; ++i) padEvents_[i - consumed] = padEvents_[i];
    padEventCount_ -= consumed;
}


/**
 * @brief Разбирает одну законченную команду после retire текущего задания. Команды статуса, ввода, меню и диагностики не обходят карантин Worker.
 * @param line Заимствованная завершённая UART-строка; функция не принимает владение памятью.
 */
void NesApp::command(const char* line) noexcept {
    NES_LOOP_TAINT()
    if (!retireFrame()) return;
    // Ввод меняет только Main-очередь/маску; sealed RGB/PCM остаются обычному выводу.
    if (!std::strcmp(line,"padat")) { schedulePad(""); return; }
    if (!std::strncmp(line,"padat ",6)) { schedulePad(line+6); return; }
    if (!std::strncmp(line,"pad ",4)) { serialButtons_=static_cast<std::uint8_t>(std::strtoul(line+4,nullptr,0)); return; }
    if (!prepareSnapshotAudio()) return;
#if defined(NES_EXPERIMENTAL_MMC5_ZERO_POLL_EXACT)
    if(!std::strcmp(line,"mmc5_poll exact")||!std::strcmp(line,"mmc5_poll normal")){
        if(playing_||pausedGame_||screen_!=Screen::Library||executionMode_==ExecutionMode::Faulted||
           (worker_&&(worker_->inFlight||worker_->coupled||worker_->framePipeline))){
            std::printf("NES_MMC5_POLL_CONTROL status=reject reason=joined_library_required\n");
            return;
        }
        const bool exact=!std::strcmp(line,"mmc5_poll exact");
        machine_.selectMmc5ZeroPoll(exact);
        std::printf("NES_MMC5_POLL_CONTROL status=ok method=%s nvs=0 applies=next_game\n",exact?"exact":"normal");
        return;
    }
#endif
#if defined(NES_EXPERIMENTAL_MMC5_INACTIVE_GRANT)
    if(!std::strcmp(line,"mmc5_grant closed")||!std::strcmp(line,"mmc5_grant forecast")){
        if(playing_||pausedGame_||screen_!=Screen::Library||executionMode_==ExecutionMode::Faulted||
           (worker_&&(worker_->inFlight||worker_->coupled||worker_->framePipeline))){
            std::printf("NES_MMC5_GRANT_CONTROL status=reject reason=joined_library_required\n");
            return;
        }
        const bool closed=!std::strcmp(line,"mmc5_grant closed");
        machine_.selectMmc5InactiveGrant(closed);
        std::printf("NES_MMC5_GRANT_CONTROL status=ok method=%s nvs=0 applies=next_game\n",closed?"closed":"forecast");
        return;
    }
#endif
#if defined(NES_DIAGNOSTIC_MMC5_INDEX_MEMORY)
    if(!std::strcmp(line,"indexed_memory hp")||!std::strcmp(line,"indexed_memory psram")){
        if(!worker_||executionMode_!=ExecutionMode::Worker||playing_||pausedGame_||screen_!=Screen::Library||
           worker_->inFlight||worker_->coupled||worker_->framePipeline){
            std::printf("NES_INDEXED_MEMORY status=reject reason=joined_library_required\n");return;
        }
        const bool hp=!std::strcmp(line,"indexed_memory hp");
        CoupledVideoPipeline::setIndexedMemoryProbe(hp);
        std::printf("NES_INDEXED_MEMORY status=ok mode=%s indexed_policy=%s nvs=0\n",hp?"hp":"psram",hp?"row_256":"full_61440");return;
    }
#endif
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
    if(!std::strcmp(line,"frame_native_overlap off")||!std::strcmp(line,"frame_native_overlap on")){
        const bool enabled=!std::strcmp(line,"frame_native_overlap on");
        const bool owned=board_.supportsDirectGamePresentPoll();
        if(!worker_||executionMode_!=ExecutionMode::Worker||playing_||pausedGame_||screen_!=Screen::Library||
           worker_->inFlight||pairingHelp_||worker_->framePipeline||worker_->coupled||
           worker_->pipelinePresentPending||worker_->presentPending||(enabled&&!owned)){
            std::printf("NES_FRAME_NATIVE_OVERLAP schema=1 status=reject triple_owned=%u reason=joined_library_required\n",unsigned(owned));return;
        }
        std::printf("NES_FRAME_NATIVE_OVERLAP schema=1 status=ok enabled=%u triple_owned=%u prior_early_submitted=%u prior_source_reused=%u nvs=0\n",
                    unsigned(enabled),unsigned(owned),unsigned(frameDirectPresent.earlySubmitted),unsigned(frameDirectPresent.sourceReused));
        frameDirectPresent={false,enabled,0,0};
        return;
    }
#endif
    if(!std::strcmp(line,"ram_witness full")||!std::strcmp(line,"ram_witness sparse")){
        // Retained activeRom_ допустим: stopPlayback завершил и уничтожил pipeline.
        if(!worker_||executionMode_!=ExecutionMode::Worker||playing_||pausedGame_||screen_!=Screen::Library||
           worker_->inFlight||pairingHelp_||!worker_->start||!worker_->done||worker_->framePipeline||
           worker_->audioPacketPending||worker_->videoPending||(worker_->snapshot&&worker_->snapshot->ready)||
           uxSemaphoreGetCount(worker_->start)||uxSemaphoreGetCount(worker_->done)||board_.audioStats().active||
           !finishFramePipelinePresent()||
           !board_.finishGamePresent()){
            std::printf("NES_FRAME_RAM_CONTROL schema=1 status=reject reason=joined_library_required\n");return;
        }
        worker_->ramWitnessPolicy=!std::strcmp(line,"ram_witness full")?FrameRamPolicy::Full:FrameRamPolicy::Sparse120;
        std::printf("NES_FRAME_RAM_CONTROL schema=1 status=ok policy=%s cadence=%u nvs=0 applies=next_pipeline game_fps_claim=0\n",
            worker_->ramWitnessPolicy==FrameRamPolicy::Full?"full":"sparse120",worker_->ramWitnessPolicy==FrameRamPolicy::Full?1U:120U);return;
    }
    if (!std::strcmp(line,"status")) status();
    else if (!std::strcmp(line,"tasks")) taskStats();
    else if (!std::strcmp(line,"pause")) {openPause();settingsStatus();}
    else if (!std::strcmp(line,"ui_status")) {
        settingsStatus();
        std::printf("NES_UI_STATUS {\"video\":%s,\"fps_enabled\":%s,\"fps_valid\":%s,\"fps\":%.3f,\"submitted_frames\":%llu,\"source\":\"software_lcd_submit\"}\n",
            hasGameVideo()?"true":"false",settings_.showFps?"true":"false",presentationRate_.valid()?"true":"false",presentationRate_.fps(),static_cast<unsigned long long>(submittedFrames_));
    }
    else if (!std::strcmp(line,"settings")) {openSettings();settingsStatus();}
    else if (!std::strncmp(line,"ui ",3)) {
        const char* key=line+3;
        std::uint8_t pressed=0;
        if(!std::strcmp(key,"up"))pressed=core::Up;
        else if(!std::strcmp(key,"down"))pressed=core::Down;
        else if(!std::strcmp(key,"left"))pressed=core::Left;
        else if(!std::strcmp(key,"right"))pressed=core::Right;
        else if(!std::strcmp(key,"a"))pressed=core::A;
        else if(!std::strcmp(key,"b"))pressed=core::B;
        else if(!std::strcmp(key,"select"))pressed=core::Select;
        if(screen_==Screen::Settings && pressed)settingsControls(pressed);
        else if(screen_==Screen::Pause && pressed)pauseControls(pressed);
        settingsStatus();
    }
    else if (!std::strcmp(line,"back") || !std::strcmp(line,"resume")) {
        if(screen_==Screen::Pause){settingsReturnToPause_=false;closeSettings();}
        else if(screen_==Screen::Settings){if(!std::strcmp(line,"resume"))settingsReturnToPause_=false;closeSettings();}
        settingsStatus();
    }
    else if (!std::strcmp(line,"save_settings")) {
        if(settingsStore_.save(settings_))settingsDirty_=false;
        else ESP_LOGW(tag,"SETTINGS_SAVE_FAIL %s",settingsStore_.errorText());
        settingsStatus();
    }
    else if (!std::strncmp(line,"volume ",7)) {
        const char* text=line+7;std::uint64_t value=0;
        if(parseDecimal(text,100,value) && !*text){settings_.volume=static_cast<std::uint8_t>(value);settingsDirty_=true;static_cast<void>(applyAudioSettings());menuDirty_=true;settingsStatus();}
        else std::printf("NES_COMMAND_ERROR reason=volume_arguments\n");
    }
    else if (!std::strncmp(line,"settings_tab ",13)) {
        const char* value=line+13;
        const char* names[]{"sound","controller","video","system"};
        for(unsigned i=0;i<4;++i)if(!std::strcmp(value,names[i])){openSettings();settingsTab_=i;settingsRow_=0;pairingHelp_=false;menuDirty_=true;break;}
        settingsStatus();
    }
    else if (!std::strncmp(line,"layout ",7)) {
        if(!std::strcmp(line+7,"standard") || !std::strcmp(line+7,"swap")) {
            settings_.swapAB=!std::strcmp(line+7,"swap");settingsDirty_=true;menuDirty_=true;
        }
        settingsStatus();
    }
    else if (!std::strncmp(line,"fps ",4)) {
        if(!std::strcmp(line+4,"on") || !std::strcmp(line+4,"off")){
            settings_.showFps=!std::strcmp(line+4,"on");settingsDirty_=true;menuDirty_=true;
            if(playing_)drawFpsCounter();
        }
        settingsStatus();
    }
    else if (!std::strncmp(line,"regionpref ",11)) {
        const auto* value=line+11;
        if(!std::strcmp(value,"auto") || !std::strcmp(value,"ntsc") || !std::strcmp(value,"pal")) {
            settings_.region=!std::strcmp(value,"pal")?RegionMode::Pal:!std::strcmp(value,"ntsc")?RegionMode::Ntsc:RegionMode::Auto;
            settingsDirty_=true;menuDirty_=true;
        }
        settingsStatus();
    }
    else if (!std::strcmp(line,"rescan")) rescanLibrary();
    else if (!std::strcmp(line,"catalog")) catalog(0,20);
    else if (!std::strncmp(line,"catalog ",8)) {
        const char* text = line+8;
        std::uint64_t offset=0,limit=20;
        while (*text==' ') ++text;
        bool valid=parseDecimal(text,UINT32_MAX,offset);
        while (*text==' ') ++text;
        if (*text) { valid=valid && parseDecimal(text,64,limit) && limit>0; while (*text==' ') ++text; }
        if (valid && !*text) catalog(static_cast<std::size_t>(offset),static_cast<std::size_t>(limit));
        else std::printf("NES_COMMAND_ERROR reason=catalog_arguments\n");
    }

    else if (!std::strncmp(line,"archivecheck ",13)) {
        const char* text=line+13;
        std::uint64_t index=0;
        while(*text==' ')++text;
        const bool valid=parseDecimal(text,UINT32_MAX,index);
        while(*text==' ')++text;
        if(valid&&!*text)archiveCheck(static_cast<std::size_t>(index));
        else std::printf("NES_COMMAND_ERROR reason=archivecheck_arguments\n");
    }
    else if (!std::strcmp(line,"list")) {
        const auto first = menuLayout::first(selected_);
        const auto visible = first<library_.count()?std::min(menuLayout::pageSize,library_.count()-first):0;
        for (std::size_t i=first;i<first+visible;++i) {
            GameInfo info{};
            if (library_.game(i,info)) std::printf("NES_GAME %u mapper=%u pal=%u %s\n",unsigned(i),info.mapper,unsigned(info.pal),info.name);
        }
    } else if (!std::strncmp(line,"run ",4)) {
        char* end=nullptr; const auto i=std::strtoul(line+4,&end,10);
        if (end!=line+4 && *end==0 && i<library_.count()) static_cast<void>(startGame(i));
    } else if (!std::strcmp(line,"menu")) stopPlayback();
    else if (!std::strcmp(line,"reset")) resetGame();
    else if (!std::strcmp(line,"mute")) { settings_.muted=true;settingsDirty_=true;board_.setAudioActive(false);menuDirty_=true; }
    else if (!std::strcmp(line,"unmute")) { settings_.muted=false;settingsDirty_=true;static_cast<void>(applyAudioSettings());menuDirty_=true; }
    else if (!std::strncmp(line,"region ",7) && playing_) {
        if(executionMode_==ExecutionMode::Faulted)return; // Region reload использует actual joined UNBIND.
        const bool pal=!std::strcmp(line+7,"pal");
        if (pal || !std::strcmp(line+7,"ntsc")) {
            clearSerialInput();
            if(!releaseFramePipelineForLoad()){workerFault("frame_pipeline_region_lease");return;}
            invalidateSnapshot();
    if(!releaseCoupledForLoad()){workerFault("coupled_load_lease");return;}
            const auto loaded = machine_.load(activeRom_,activeRomBytes_,pal?core::Region::Pal:core::Region::Ntsc);
            if (loaded == core::LoadResult::Ok) {
                if(!configureVideoWorker()){workerFault("cpu_bench_region");return;}
                current_.pal=pal; resetCounters();
                if(!configureCoupledVideoPipeline()){workerFault("coupled_region");return;}
                if(!configureFrameVideoPipeline()){workerFault("frame_pipeline_region");return;}
                prepareGameDisplay();static_cast<void>(applyAudioSettings());
            } else {
                ESP_LOGE(tag,"ROM_LOAD_FAIL reason=region_reload"); stopPlayback();
            }
        }
    } else std::printf("NES_COMMANDS pause | ui_status | settings | settings_tab sound/controller/video/system | back | volume 0..100 | mute/unmute | layout standard/swap | fps on/off | regionpref auto/ntsc/pal | save_settings | catalog [OFFSET [COUNT]] | archivecheck N | rescan | run N | menu | status | tasks | pad MASK | padat FRAME MASK | reset | region ntsc/pal\n");
}

/**
 * @brief Собирает полные ограниченные строки UART и передаёт их диспетчеру. Переполнение отбрасывает строку, а не исполняет её усечённый префикс.
 */
void NesApp::serialCommands() noexcept {
    std::uint8_t c=0;
    for (unsigned n=0;n<96 && uart_read_bytes(UART_NUM_0,&c,1,0)==1;++n) { NES_LOOP_TAINT() 
        if (c=='\r' || c=='\n') {
            if (serialOverflow_) std::printf("NES_COMMAND_ERROR reason=line_too_long\n");
            else if (serialLength_) { serialLine_[serialLength_]=0; command(serialLine_); }
            serialLength_=0; serialOverflow_=false;
        } else if (c>=32 && c<127) {
            if (!serialOverflow_ && serialLength_+1<sizeof(serialLine_)) serialLine_[serialLength_++]=char(c);
            else serialOverflow_=true; // Усечённый префикс, похожий на команду, никогда не исполняется.
        }
        else if (c==8 || c==127) { if(serialLength_) --serialLength_; }
    }
}

