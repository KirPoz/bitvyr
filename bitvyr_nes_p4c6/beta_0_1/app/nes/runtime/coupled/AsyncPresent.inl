// Включается только внутри nes::app после полного определения WorkerState.
// Copy control удерживает Board RGB; EXP A/B отдельно удерживает фактический source ticket до drain.

/**
 * @brief На Main завершает фактическое владение Board/PPA предыдущим coupled-кадром.
 * @details Счётчик публикаций меняет только при достоверном завершении; не освобождает контекст при неопределённом результате.
 */
bool NesApp::finishCoupledPresent() noexcept {
    if(!worker_||!worker_->presentPending)return true;
    auto&w=*worker_;
    const auto begin=esp_timer_get_time();
    const bool borrowed=w.presentSource.valid();
    const bool finished=borrowed?board_.finishBorrowedGamePresent(w.presentSource):board_.finishGamePresent();
    presentUs_+=esp_timer_get_time()-begin;
    if(!finished){
        if(!w.presentFailed)++displayErrors_;
        w.presentFailed=true;
        return false; // Lease/context не сбрасываем до настоящего успешного drain.
    }
    return completeCoupledPresent();
}

/** @brief После Board success снимает Main source credit, не наблюдая работающую Machine. */
bool NesApp::completeCoupledPresent()noexcept{
    auto&w=*worker_;
    const bool valid=w.presentGeneration==sessionGeneration_&&w.presentFrame&&w.presentFrame<=frames_;
    if(w.presentSource.valid()){
        if(!w.coupled||!w.coupled->completeSource(w.presentSource)){w.presentFailed=true;return false;}
        w.presentSource={};
    }
    // При ошибке bool API Board не различает позднюю submission
    // и уже отказавший двухбуферный LCD draw; cleanup не выдумывает счётчик.
    if(valid&&!w.presentFailed)++submittedFrames_;
    w.presentPending=false;w.presentFailed=false;
    w.presentGeneration=w.presentFrame=0;
    return valid;
}

/**
 * @brief Копирует принятый staging RGB в свободный Board source и запускает асинхронное представление.
 * @details Сохраняет поколение/кадр pending lease до настоящего drain и не считает ошибочный late-submit завершённым кадром.
 */
void NesApp::presentCoupledFrameAsync() noexcept {
    auto&w=*worker_;auto&s=*w.snapshot;auto&c=*w.coupled;
    if(w.inFlight||executionMode_!=ExecutionMode::Worker||!s.ready||!playing_||s.generation!=sessionGeneration_)return;
    const auto valid=[&]()noexcept{return s.generation==sessionGeneration_&&c.matches(machine_,activeRom_,current_.size,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_);};
    if(!valid()||!c.pixels()){workerFault("coupled_present_lease");return;}
    const bool previous=w.presentPending;
    if(!finishCoupledPresent()){workerFault("coupled_previous_ppa");return;}
    // Сохраняем и исходный неучтённый/начальный Board fence.
    if(!previous&&!board_.finishGamePresent()){workerFault("coupled_previous_ppa");return;}
    if(c.rgbPoolEnabled()){
        const auto ticket=c.acceptedSource();
        if(!ticket.valid()||ticket.pixels!=c.pixels()||ticket.generation!=sessionGeneration_||
           ticket.frame!=s.completedFrames||ticket.inputFrame+1!=ticket.frame||!valid()||!c.borrowSource(ticket)){
            workerFault("coupled_rgb_borrow_key");return;
        }
        const auto begin=esp_timer_get_time();const bool started=board_.beginBorrowedGamePresent(ticket);presentUs_+=esp_timer_get_time()-begin;
        if(!started){static_cast<void>(c.cancelSource(ticket));++displayErrors_;workerFault("coupled_rgb_borrow_begin");return;}
        // Сначала сохраняем аппаратный lease: любой последующий отказ удерживает его до реального drain.
        w.presentSource=ticket;w.presentGeneration=sessionGeneration_;w.presentFrame=s.completedFrames;
        w.presentPending=true;w.presentFailed=false;
        if(!c.confirmSource(ticket)||!valid()||!c.prepareNextRgb(ticket)){workerFault("coupled_rgb_retarget");return;}
        const auto audio=esp_timer_get_time();
        // Remote PCM выдаёт MainOwner; Snapshot не должен повторно попасть в I2S.
        if(!w.c6Audio)
        if(!settings_.muted&&settings_.volume>0)audioShort_+=s.sampleCount-board_.writeAudio(s.pcm,s.sampleCount);
        audioUs_+=esp_timer_get_time()-audio;s.ready=false;return;
    }
    auto*destination=board_.gameFramebuffer();
    if(!destination||!valid()){workerFault("coupled_rgb_destination");return;}
    const auto copy=esp_timer_get_time();std::memcpy(destination,c.pixels(),122880);w.coupledCopyUs+=esp_timer_get_time()-copy;
    if(!valid()){workerFault("coupled_after_copy");return;}
    const auto begin=esp_timer_get_time();const bool started=board_.beginGamePresent();presentUs_+=esp_timer_get_time()-begin;
    if(!started){++displayErrors_;workerFault("coupled_ppa");return;}
    w.presentGeneration=sessionGeneration_;w.presentFrame=s.completedFrames;
    w.presentPending=true;w.presentFailed=false;
    if(!valid()){workerFault("coupled_after_present");return;}
    const auto audio=esp_timer_get_time();
    // Тот же запрет повторной локальной выдачи для owned/copy fallback.
    if(!w.c6Audio)
    if(!settings_.muted&&settings_.volume>0)audioShort_+=s.sampleCount-board_.writeAudio(s.pcm,s.sampleCount);
    audioUs_+=esp_timer_get_time()-audio;s.ready=false;
}
