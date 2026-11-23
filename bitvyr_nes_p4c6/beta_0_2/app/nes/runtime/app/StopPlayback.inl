/** @file StopPlayback.inl
 * @brief Реальная Main-only остановка сеанса до публикации Library.
 * @details Private fragment включается из Session.inl после owner-state;
 * SDK-free host fixture исполняет тот же метод через явные lifecycle callbacks.
 */
/**
 * @brief Останавливает игру и возвращает интерфейс в библиотеку. Сначала учитывает настоящих владельцев; неизвестный Worker сохраняет ROM и контекст.
 */
void NesApp::stopPlayback() noexcept {
    const bool hadCoupled=worker_&&worker_->coupled;
    if(worker_&&worker_->c6Audio&&(!retireFrame()||!prepareSnapshotAudio()||!drainC6AudioBackend())){
        playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;return;
    }
    if(worker_)worker_->coupledMainIdle.reset();
    // Прямой UART menu может прийти до done. Generation меняется только после join.
    if(worker_&&(worker_->legacyMachine||worker_->coupled)&&
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
    if(!finishCoupledPresent()){
        playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;
        if(executionMode_!=ExecutionMode::Faulted)workerFault("coupled_stop_ppa");
        return; // После тайм-аута сохраняются generation/context и lease Board.
    }
    // Library завершает сессию; Pause/Cancel сюда не попадают. Уже проверенный
    // release освобождает C6 binding, Thin endpoint и RGB только после настоящих drains.
    if(hadCoupled&&!releaseCoupledForLoad()){
        playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;
        if(executionMode_!=ExecutionMode::Faulted)workerFault("coupled_stop_ownership");
        return; // Не публиковать Library/generation и не терять неизвестного владельца.
    }
    // Только после actual Worker/C6/PPA/LCD/UNBIND и освобождения context.
    if(hadCoupled)std::printf("NES_COUPLED_REPORT_FINAL_DRAIN schema=1 report_no_drain=%u game=%u frames=%llu submitted_frames=%llu generation=%llu in_flight=%u pending_depth=%u source_valid=%u actual_join_checked=1 next_frame_finish_unchanged=1\n",
        unsigned(1),unsigned(selected_),static_cast<unsigned long long>(frames_),
        static_cast<unsigned long long>(submittedFrames_),static_cast<unsigned long long>(sessionGeneration_),
        unsigned(worker_->inFlight),unsigned(worker_->presentPending),unsigned(worker_->presentSource.valid()));
    playing_ = false;
    pausedGame_ = false;
    settingsReturnToPause_=false;pauseAction_=PauseAction::None;chromeButtons_=0;
    screen_ = Screen::Library;
    board_.setAudioActive(false);
    clearSerialInput();
    invalidateSnapshot();
    menuDirty_ = true;
}
