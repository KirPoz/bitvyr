/** @file Ui.inl
 * @brief Main-only экран, overlay, пауза и NVS settings без смены active session.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Передаёт сохранённую громкость и mute аудиоадаптеру. Возвращает успешность аппаратного применения, не повышая громкость при отказе.
 */
bool NesApp::applyAudioSettings() noexcept {
    const bool active = playing_ && !settings_.muted && settings_.volume>0;
    if (!active) board_.setAudioActive(false);
    if (!board_.setVolume(settings_.volume)) {
        settings_.muted = true;
        board_.setAudioActive(false);
        return false;
    }
    if (active && !board_.audioStats().active) board_.setAudioActive(true);
    return true;
}

/**
 * @brief Различает реальный видеотракт и CPU-only диагностику по Main-контексту.
 * @return true для обычного видео/Frame/Coupled; первичная Machine не читается.
 */
bool NesApp::hasGameVideo() const noexcept {
    if(worker_&&worker_->legacyMachine)return true;
    if(worker_&&worker_->framePipeline)return true;
    if(worker_&&worker_->coupled)return true;
    return false;
}

/** @brief Рисует/убирает FPS только в отдельном источнике боковых панелей; новый PPA-submit не создаёт. */
void NesApp::drawFpsCounter() noexcept {
    if(!hasGameVideo())return;
    gameChrome::drawFps(board_,settings_.showFps,presentationRate_.fps(),presentationRate_.valid());
}

/** @brief Раз в секунду обновляет частоту успешных программных submit; produced/accepted и часы CPU в числитель не входят. */
void NesApp::updateGameOverlay() noexcept {
    if(!playing_||!hasGameVideo())return;
    if(presentationRate_.observe(esp_timer_get_time(),submittedFrames_)){
        lastFps_=presentationRate_.fps();
        if(settings_.showFps)drawFpsCounter();
    }
}

/** @brief Готовит единый HUD обычного/Frame/Coupled видео; стартовое представление не считается гостевым кадром. */
void NesApp::prepareGameDisplay() noexcept {
    previousTouchIds_=0;chromeButtons_=0;
    presentationRate_.reset(esp_timer_get_time(),submittedFrames_);lastFps_=0;
    if(!hasGameVideo()){
        board_.clear(gameChrome::background);
        board_.drawText(24,80,"CPU1 BENCHMARK",gameChrome::text,3);
        board_.drawText(24,145,"NO GAME VIDEO",gameChrome::mutedText,2);
        static_cast<void>(board_.present());return;
    }
    gameChrome::draw(board_,0,settings_.showFps,0,false);
    // A/B уже зарезервированы: ordinary initial/resume present не имеет reader lease.
    // HUD готов; первый настоящий borrowed frame построит RGB и обновит native bars.
    if(worker_&&worker_->coupled&&worker_->coupled->rgbPoolEnabled())return;
    if(frames_==0)if(auto*pixels=board_.gameFramebuffer())std::fill_n(pixels,256*240,0);
    // Пауза/загрузка уже завершили владельцев; только здесь нужен отдельный начальный present.
    // Последующие изменения панелей переносятся обычным безопасным native-slot путём.
    if(!board_.presentGame())++displayErrors_;
}

/**
 * @brief Открывает быстрое меню после настоящего завершения игрового задания/вывода.
 * @param confirmExit Сразу показать подтверждение выхода; исходный выбор — CANCEL.
 */
void NesApp::openPause(bool confirmExit) noexcept {
    if(executionMode_==ExecutionMode::Faulted)return; // Не сбрасывает poisoned C6 owner.
    if(!playing_&&!pausedGame_)return;
    if(playing_)openSettings();
    if(executionMode_==ExecutionMode::Faulted||!pausedGame_)return;
    screen_=Screen::Pause;settingsReturnToPause_=false;
    pauseAction_=confirmExit?PauseAction::Exit:PauseAction::None;pauseRow_=0;
    previousButtons_=gamepad_.buttons();repeatButtons_=0;menuDirty_=true;
}

/** @brief Рисует четыре крупных действия паузы либо два пункта подтверждения; не использует занятый RGB-source. */
void NesApp::pauseMenu() noexcept {
    board_.clear(gameChrome::background);
    board_.drawText(24,20,"PAUSED",gameChrome::text,3);
    char title[100]{};std::snprintf(title,sizeof(title),"%.92s",current_.name);
    for(auto*p=title;*p;++p)if(*p=='_')*p=' ';
    board_.drawText(24,60,title,gameChrome::mutedText,1);
    const bool confirm=pauseAction_!=PauseAction::None;
    if(confirm){
        board_.drawText(160,126,pauseAction_==PauseAction::Exit?"QUIT TO LIBRARY?":"RESTART GAME?",gameChrome::text,3);
        board_.drawText(160,168,"CURRENT GAME PROGRESS WILL BE LOST",gameChrome::mutedText,2);
    }
    const char* labels[]{"RESUME","SETTINGS","RESTART GAME","LIBRARY"};
    const unsigned count=confirm?2:4;
    for(unsigned index=0;index<count;++index){
        const auto& box=confirm?(index?pauseMenuLayout::confirm:pauseMenuLayout::cancel):pauseMenuLayout::rows[index];
        rectangle(board_.framebuffer(),box.x,box.y,box.width,box.height,pauseRow_==index?gameChrome::pressed:gameChrome::button);
        const char* label=confirm?(index?(pauseAction_==PauseAction::Exit?"QUIT GAME":"RESTART GAME"):"CANCEL"):labels[index];
        const int width=(int(std::strlen(label))*6-1)*2;
        board_.drawText(box.x+(box.width-width)/2,box.y+(box.height-14)/2,label,gameChrome::text,2);
    }
    board_.drawText(160,440,"A: SELECT   B: BACK   UP/DOWN: CHOOSE",gameChrome::mutedText,2);
    static_cast<void>(board_.present());menuDirty_=false;
}

/** @brief Выполняет выбранный пункт; повторный явный выбор требуется для выхода/перезапуска. */
void NesApp::activatePauseSelection() noexcept {
    if(pauseAction_!=PauseAction::None){
        if(pauseRow_==0){pauseAction_=PauseAction::None;pauseRow_=0;menuDirty_=true;return;}
        const auto action=pauseAction_;pauseAction_=PauseAction::None;settingsReturnToPause_=false;
        if(action==PauseAction::Exit){pausedGame_=false;closeSettings();stopPlayback();}
        else {resetGame();if(executionMode_!=ExecutionMode::Faulted)closeSettings();}
        return;
    }
    if(pauseRow_==0){settingsReturnToPause_=false;closeSettings();}
    else if(pauseRow_==1)openSettings();
    else {pauseAction_=pauseRow_==2?PauseAction::Restart:PauseAction::Exit;pauseRow_=0;menuDirty_=true;}
}

/**
 * @brief Обрабатывает паузу по фронтам ввода; касание, открывшее экран, ждёт отпускания.
 * @param pressed Новые/повторные NES-навигационные кнопки Main, не игровой ввод CPU.
 */
void NesApp::pauseControls(std::uint8_t pressed) noexcept {
    if(pressed&core::B){
        if(pauseAction_!=PauseAction::None){pauseAction_=PauseAction::None;pauseRow_=0;menuDirty_=true;}
        else {settingsReturnToPause_=false;closeSettings();}
        return;
    }
    const unsigned count=pauseAction_==PauseAction::None?4:2;
    if(pressed&core::Up){pauseRow_=(pauseRow_+count-1)%count;menuDirty_=true;}
    if(pressed&core::Down){pauseRow_=(pauseRow_+1)%count;menuDirty_=true;}
    if(pressed&(core::A|core::Start)){activatePauseSelection();return;}
    platform::TouchPoint point{};
    if(!board_.pollTouch(point))return;
    const bool tapped=point.pressed&&!touchHeld_;touchHeld_=point.pressed;
    if(!tapped)return;
    const auto index=pauseMenuLayout::hit(point.x,point.y,pauseAction_!=PauseAction::None);
    if(index!=pauseMenuLayout::noItem){pauseRow_=index;activatePauseSelection();}
}

/**
 * @brief Приостанавливает игру на подтверждённой границе и открывает настройки. Удержанные кнопки подавляются до отпускания, чтобы переход не стал игровым нажатием.
 */
void NesApp::openSettings() noexcept {
    if(executionMode_==ExecutionMode::Faulted)return; // Не сбрасывает poisoned C6 owner.
    if(worker_)worker_->coupledMainIdle.reset();
    NES_LOOP_TAINT()
    if(!finishFramePipelinePresent()){workerFault("frame_pipeline_settings_ppa");return;}
    if (!retireFrame() || screen_==Screen::Settings) return;
    if(!finishCoupledPresent()){workerFault("coupled_settings_ppa");return;}
    if (!prepareSnapshotAudio()) return;
    settingsReturnToPause_=screen_==Screen::Pause;volumeDrag_=false;
    pausedGame_ = playing_||pausedGame_;
    playing_ = false;
    board_.setAudioActive(false);
    clearSerialInput();
    machine_.setController(0,0);
    screen_ = Screen::Settings;
    settingsRow_ = 0;
    pairingHelp_ = false;
    previousButtons_ = gamepad_.buttons();
    repeatButtons_ = 0;
    menuDirty_ = true;
}

/**
 * @brief Применяет и при необходимости сохраняет настройки, затем возвращает предыдущий экран/игру. Ошибка сохранения остаётся диагностируемой.
 */
void NesApp::closeSettings() noexcept {
    NES_LOOP_TAINT()
    volumeDrag_=false;
    if (settingsDirty_) {
        if (settingsStore_.save(settings_)) settingsDirty_ = false;
        else ESP_LOGW(tag,"SETTINGS_SAVE_FAIL %s",settingsStore_.errorText());
    }
    pairingHelp_ = false;
    if(settingsReturnToPause_&&pausedGame_&&screen_==Screen::Settings){
        settingsReturnToPause_=false;screen_=Screen::Pause;pauseAction_=PauseAction::None;pauseRow_=0;
        previousButtons_=gamepad_.buttons();repeatButtons_=0;menuDirty_=true;return;
    }
    screen_ = Screen::Library;
    if (pausedGame_) {
        pausedGame_ = false;
        playing_ = true;
        suppressedGamepad_ = gamepad_.buttons();
        suppressTouchUntilRelease_ = touchHeld_;
        resetPerformanceWindow();
        prepareGameDisplay();
        static_cast<void>(applyAudioSettings());
    }
    previousButtons_ = gamepad_.buttons();
    repeatButtons_ = 0;
    menuDirty_ = true;
}

/**
 * @brief Перезапускает активную ROM после безопасного завершения владельцев. Старые снимки/ввод инвалидируются новым поколением.
 */
void NesApp::resetGame() noexcept {
    if(executionMode_==ExecutionMode::Faulted)return; // Не сбрасывает poisoned C6 owner.
    NES_LOOP_TAINT()
    if (!retireFrame() || !activeRom_ || (!playing_ && !pausedGame_)) return;
    if(!releaseFramePipelineForLoad()){workerFault("frame_pipeline_reset_lease");return;}
    const bool paused = pausedGame_;
    board_.setAudioActive(false);
    clearSerialInput();
    invalidateSnapshot();
    current_.pal = settings_.effectivePal(currentPalHint_);
    if(!releaseCoupledForLoad()){workerFault("coupled_load_lease");return;}
    const auto result = machine_.load(activeRom_,current_.size,current_.pal?core::Region::Pal:core::Region::Ntsc);
    if (result != core::LoadResult::Ok) { stopPlayback(); return; }
    if(!configureVideoWorker()){workerFault("cpu_bench_reset");return;}
    resetCounters();
    if(!configureCoupledVideoPipeline()){workerFault("coupled_reset");return;}
    if(!configureFrameVideoPipeline()){workerFault("frame_pipeline_reset");return;}
    previousButtons_ = gamepad_.buttons();
    repeatButtons_ = 0;
    pausedGame_ = paused;
    playing_ = !paused;
    if (playing_) { prepareGameDisplay(); static_cast<void>(applyAudioSettings()); }
    menuDirty_ = true;
}

/**
 * @brief Печатает фактические настройки, активный регион, паузу и dirty-флаг в UART. Вывод не заменяет проверку физического звука.
 */
void NesApp::settingsStatus() const noexcept {
    std::printf("NES_SETTINGS {\"volume\":%u,\"hardware_volume\":%u,\"muted\":%s,\"swap_ab\":%s,\"show_fps\":%s,\"region\":\"%s\",\"active_region\":\"%s\",\"screen\":\"%s\",\"paused\":%s,\"dirty\":%s}\n",
        unsigned(settings_.volume),board_.volume(),settings_.muted?"true":"false",settings_.swapAB?"true":"false",
        settings_.showFps?"true":"false",regionName(settings_.region),(playing_||pausedGame_)?(current_.pal?"PAL":"NTSC"):"none",
        playing_?"game":screen_==Screen::Settings?"settings":screen_==Screen::Pause?"pause":"library",pausedGame_?"true":"false",settingsDirty_?"true":"false");
}

/**
 * @brief Рисует отдельный экран SOUND/CONTROLLER/VIDEO/SYSTEM с текущими значениями. Не исполняет гостевой кадр и не сохраняет NVS при каждой отрисовке.
 */
void NesApp::settingsMenu() noexcept {
    board_.clear(0x0841);
    board_.drawText(24,16,"SETTINGS",0x07ff,3);
    char text[128]{};
    if (pausedGame_) std::snprintf(text,sizeof(text),"PAUSED: %.92s",current_.name); // 100символов с префиксом; x24..623 не пересекаются с BACK на x650.
    else std::snprintf(text,sizeof(text),"NES HANDHELD");
    for (auto* p=text;*p;++p) if(*p=='_') *p=' ';
    board_.drawText(24,52,text,0x8410,1);
    rectangle(board_.framebuffer(),650,14,126,40,gameChrome::button);
    board_.drawText(666,27,pausedGame_&&!settingsReturnToPause_?"RESUME":"BACK",0xffff,2);
    for (unsigned tab=0;tab<4;++tab) {
        const int x=24+int(tab)*188;
        rectangle(board_.framebuffer(),x,76,176,44,tab==settingsTab_?gameChrome::pressed:gameChrome::button);
        board_.drawText(x+10,91,settingsTabs[tab],0xffff,2);
    }
    for (unsigned row=0;row<settingsRows[settingsTab_];++row) {
        const int y=144+int(row)*52;
        rectangle(board_.framebuffer(),24,y,752,46,row==settingsRow_?gameChrome::pressed:gameChrome::button);
        const char* label=""; const char* value="";
        if(settingsTab_==0) {
            label=row==0?"Volume":"Mute";
            if(row==0){std::snprintf(text,sizeof(text),"%u%%",unsigned(settings_.volume));value=text;}
            else value=settings_.muted?"ON":"OFF";
        } else if(settingsTab_==1) {
            const char* labels[]{"A / B layout","How to pair Xbox","Bluetooth"};
            label=labels[row];
            value=row==0?(settings_.swapAB?"SWAPPED":"STANDARD"):row==1?"HELP":gamepad_.statusText();
        } else if(settingsTab_==2) {
            label=row==0?"Region (next launch)":"FPS counter";
            value=row==0?regionName(settings_.region):(settings_.showFps?"ON":"OFF");
        } else {
            const char* labels[]{"Reload SD library","Restart current game","Quit to library","Restore defaults"};
            label=labels[row];
            value=row==1?(pausedGame_?"RESTART":"NO GAME"):"SELECT";
        }
        board_.drawText(40,y+15,label,0xffff,2);
        char displayed[64]{};
        std::snprintf(displayed,sizeof(displayed),"%.21s",value);
        board_.drawText(470,y+15,displayed,0xffe0,2);
        if(settingsTab_==0 && row==0) {
            board_.drawText(630,y+15,"-",0xffff,2);
            board_.drawText(730,y+15,"+",0xffff,2);
            rectangle(board_.framebuffer(),200,y+38,360,4,0x4208);
            rectangle(board_.framebuffer(),200,y+38,int(settings_.volume)*360/100,4,0x07e0);
        }
    }
    if (settingsTab_==1 && pairingHelp_) {
        board_.drawText(40,326,"XBOX SERIES: TURN ON THE CONTROLLER",0xc618,2);
        board_.drawText(40,354,"HOLD PAIR UNTIL THE XBOX LIGHT BLINKS",0xc618,2);
        board_.drawText(40,382,"THE CONSOLE SEARCHES AUTOMATICALLY",0x8410,2);
    } else if(settingsTab_==2) {
        board_.drawText(40,300,"AUTO KEEPS THE GAME'S ORIGINAL TIMING",0x8410,2);
        board_.drawText(40,330,"NTSC: ABOUT 60 FPS   PAL: ABOUT 50 FPS",0x8410,2);
        board_.drawText(40,360,"REGION CHANGES APPLY ON RESTART / LOAD",0x8410,2);
    } else if(settingsTab_==3) {
        std::snprintf(text,sizeof(text),"SD: %.1f GB   GAMES: %u   FREE RAM: %u KB",
            double(board_.storageCapacity())/1000000000.0,unsigned(library_.count()),
            unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024));
        board_.drawText(32,372,text,0x8410,1);
        std::snprintf(text,sizeof(text),"LAST: %.1f FPS  CORE: %.2f MS  ESP-IDF %s",lastFps_,double(lastAverageCoreUs_)/1000,esp_get_idf_version());
        board_.drawText(32,392,text,0x8410,1);
    }
    if (settingsDirty_) board_.drawText(32,416,"CHANGES ARE SAVED WHEN LEAVING SETTINGS",0xffe0,1);
    if (*settingsStore_.errorText()) board_.drawText(32,430,"SETTINGS STORAGE UNAVAILABLE; CHANGES STILL APPLY",0xf800,1);
    board_.drawText(24,452,"UP/DOWN: CHOOSE  LEFT/RIGHT: CHANGE  A: SELECT  B: BACK  SELECT: TAB",0xc618,1);
    static_cast<void>(board_.present());
    menuDirty_=false;
}

/**
 * @brief Меняет выбранное значение в допустимых пределах и отмечает необходимость сохранения. Громкость применяется через Board, регион — на границе перезапуска игры.
 * @param direction Знак изменения выбранного параметра: уменьшение либо увеличение.
 */
void NesApp::changeSetting(int direction) noexcept {
    const unsigned step=direction>0?1:0;
    if(settingsTab_==0) {
        if(settingsRow_==0) {
            const int increment=settings_.volume<10?1:5;
            const int value=int(settings_.volume)+(direction>0?increment:-increment);
            settings_.volume=static_cast<std::uint8_t>(std::max(0,std::min(100,value)));
        } else settings_.muted=!settings_.muted;
        settingsDirty_=true;
        static_cast<void>(applyAudioSettings());
    } else if(settingsTab_==1) {
        if(settingsRow_==0){settings_.swapAB=!settings_.swapAB;settingsDirty_=true;}
        else if(settingsRow_==1) pairingHelp_=!pairingHelp_;
    } else if(settingsTab_==2) {
        if(settingsRow_==0) {
            const auto current=static_cast<unsigned>(settings_.region);
            settings_.region=static_cast<RegionMode>((current+(step?1:2))%3);
        } else settings_.showFps=!settings_.showFps;
        settingsDirty_=true;
    } else {
        if(settingsRow_==0) {
            pausedGame_=false;
            closeSettings();
            rescanLibrary();
        } else if(settingsRow_==1) resetGame();
        else if(settingsRow_==2) {
            pausedGame_=false;
            closeSettings();
            stopPlayback();
        } else {
            settings_=ConsoleSettings{};
            settingsDirty_=true;
            static_cast<void>(applyAudioSettings());
        }
    }
    menuDirty_=true;
}

/**
 * @brief Обрабатывает фронты кнопок навигации и повторы в настройках. Игровая маска не передаётся CPU во время настройки.
 * @param pressed Маска фронтов NES-кнопок текущего шага интерфейса.
 */
void NesApp::settingsControls(std::uint8_t pressed) noexcept {
    if(pressed & core::B){if(pairingHelp_){pairingHelp_=false;menuDirty_=true;}else closeSettings();return;}
    if(pressed & core::Select){settingsTab_=(settingsTab_+1)%4;settingsRow_=0;pairingHelp_=false;menuDirty_=true;}
    const auto rows=settingsRows[settingsTab_];
    if(pressed & core::Down){settingsRow_=(settingsRow_+1)%rows;menuDirty_=true;}
    if(pressed & core::Up){settingsRow_=(settingsRow_+rows-1)%rows;menuDirty_=true;}
    if(pressed & core::Left) changeSetting(-1);
    else if(pressed & (core::Right|core::A|core::Start)) changeSetting(1);
    if(screen_!=Screen::Settings) return;
    platform::TouchPoint point{};
    if(!board_.pollTouch(point)){volumeDrag_=false;return;}
    const bool tapped=point.pressed&&!touchHeld_;
    touchHeld_=point.pressed;
    if(!point.pressed)volumeDrag_=false;
    if(tapped)volumeDrag_=settingsTab_==0&&point.y>=172&&point.y<193&&point.x>=200&&point.x<=560;
    if(tapped && point.x>=650 && point.y>=10 && point.y<65){closeSettings();return;}
    if(tapped && point.y>=76 && point.y<120 && point.x>=24 && point.x<776) {
        const unsigned tab=(point.x-24)/188;
        if(tab<4){settingsTab_=tab;settingsRow_=0;pairingHelp_=false;menuDirty_=true;}
        return;
    }
    if(point.pressed && volumeDrag_ && settingsTab_==0 && point.y>=172 && point.y<193 && point.x>=200 && point.x<=560) {
        const auto value=static_cast<std::uint8_t>((unsigned(point.x)-200)*100/360);
        if(settings_.volume!=value){settings_.volume=value;settingsDirty_=true;menuDirty_=true;static_cast<void>(applyAudioSettings());}
        settingsRow_=0; return;
    }
    if(tapped && point.x>=24 && point.x<776 && point.y>=144) {
        const unsigned row=(point.y-144)/52;
        if(row<settingsRows[settingsTab_] && (point.y-144)%52<46) {
            settingsRow_=row;
            if(settingsTab_!=0 || row!=0) changeSetting(1);
            else if(point.x>=600) changeSetting(point.x<680?-1:1);
            menuDirty_=true;
        }
    }
}

