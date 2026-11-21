/** @file StopPlayback.inl
 * @brief Реальная Main-only остановка сеанса до публикации Library.
 * @details Private fragment включается из Session.inl после owner-state;
 * SDK-free host fixture исполняет тот же метод через явные lifecycle callbacks.
 */
/**
 * @brief Останавливает игру и возвращает интерфейс в библиотеку. Сначала учитывает настоящих владельцев; неизвестный Worker сохраняет ROM и контекст.
 */
void NesApp::stopPlayback() noexcept {
    // Прямой UART menu может прийти до done. Generation меняется только после join.
    if(worker_&&worker_->legacyMachine&&
       (executionMode_==ExecutionMode::Faulted||!retireFrame())){
        // Quarantine: звук/UI останавливаются, generation и неизвестные заимствования сохраняются.
        playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;
        return;
    }
    NES_LOOP_TAINT()
    if(!finishFramePipelinePresent()){
        playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;
        if(executionMode_!=ExecutionMode::Faulted)workerFault("frame_pipeline_stop_ppa");
        return;
    }
    if(worker_&&worker_->framePipeline&&!releaseFramePipelineForLoad()){
        worker_->framePipeline->stop();
        playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;
        if(executionMode_!=ExecutionMode::Faulted)workerFault("frame_pipeline_stop_ownership");
        return;
    }
    playing_ = false;
    pausedGame_ = false;
    settingsReturnToPause_=false;pauseAction_=PauseAction::None;chromeButtons_=0;
    screen_ = Screen::Library;
    board_.setAudioActive(false);
    clearSerialInput();
    invalidateSnapshot();
    menuDirty_ = true;
}
