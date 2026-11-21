/** @file Session.inl
 * @brief Поколения, остановка, counters и pacing; timeout не является join.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Инвалидирует старый принятый снимок на присоединённой границе паузы/перезапуска. Переносит поколение, сохраняя уже прошедшую историю DSP.
 */
void NesApp::invalidateSnapshot() noexcept {
    if(!finishFramePipelinePresent()){if(executionMode_!=ExecutionMode::Faulted)workerFault("frame_pipeline_invalidate_ppa");return;}
    if(worker_&&worker_->framePipeline&&executionMode_==ExecutionMode::Worker&&!worker_->inFlight){
        if(!consumeFramePipelineFrame(false))return;
        ++sessionGeneration_;
        if(!worker_->framePipeline->rebindAppGeneration(sessionGeneration_)){workerFault("frame_pipeline_pause_generation");return;}
        worker_->snapshot->ready=false;return;
    }
    if(!finishCoupledPresent()){if(executionMode_!=ExecutionMode::Faulted)workerFault("coupled_invalidate_ppa");return;}
    if(executionMode_!=ExecutionMode::Faulted&&worker_&&!worker_->inFlight)
        static_cast<void>(prepareSnapshotVideo(false));
    // Пауза/reset/menu отбрасывают вывод, но не уже прошедшую историю DSP.
    // Тайм-аут Worker сохраняет владение и все заимствованные буферы.
    if (executionMode_ != ExecutionMode::Faulted && worker_ && !worker_->inFlight)
        static_cast<void>(prepareSnapshotAudio());
    ++sessionGeneration_;
    if (worker_ && worker_->snapshot) worker_->snapshot->ready = false;
}

#include "StopPlayback.inl"


/**
 * @brief Сбрасывает кадровые/диагностические счётчики при переходе сессии. Не является заменой аппаратного завершения.
 */
void NesApp::resetCounters() noexcept {
    NES_LOOP_TAINT()
    if(!finishFramePipelinePresent()){if(executionMode_!=ExecutionMode::Faulted)workerFault("frame_pipeline_counter_ppa");return;}
    if(worker_){worker_->pipelinePresentStarted=worker_->pipelinePresentCompleted=0;
        worker_->pipelineCompletedGeneration=worker_->pipelineCompletedFrame=0;}
    frames_ = periodFrames_ = audioShort_ = displayErrors_ = 0;
    coreUs_ = renderUs_ = maxCoreUs_ = inputUs_ = audioUs_ = presentUs_ = 0;
    workerWaitUs_ = snapshotUs_ = 0; submittedFrames_ = 0;
    if (worker_) worker_->audioDspUs = 0;
    if(worker_)worker_->c6PumpCost.reset(); // Только Main, вне активного probe scope.
    if(worker_){worker_->coupledTimes={};worker_->coupledPumpUs=worker_->coupledCopyUs=0;worker_->coupledDoneLoops=worker_->coupledDonePumps=worker_->coupledDoneAttempts=worker_->coupledDoneForced=worker_->coupledDoneTokens=0;worker_->coupledDoneOverflow=false;}
    if(worker_){worker_->pipelineTimes={};worker_->pipelineCopyUs=0;}
    if(worker_&&worker_->framePipeline)worker_->framePipeline->clearHartStats();
    periodStart_ = esp_timer_get_time();
    serialButtons_ = previousButtons_ = 0;
    lastFps_ = 0;
    presentationRate_.reset(periodStart_,submittedFrames_);
    lastAverageCoreUs_ = 0;
    nextFrameUs_ = esp_timer_get_time();
    guestCycleMark_ = machine_.cpuCycles();
    paceRemainder_ = 0;
}

/**
 * @brief Начинает новое окно измерений и сбрасывает его накопители. Гостевые часы и состояние игры не обнуляются.
 */
void NesApp::resetPerformanceWindow() noexcept {
    NES_LOOP_TAINT()
    periodFrames_ = 0;
    coreUs_ = renderUs_ = maxCoreUs_ = inputUs_ = audioUs_ = presentUs_ = 0;
    workerWaitUs_ = snapshotUs_ = 0;
    if (worker_) worker_->audioDspUs = 0;
    if(worker_)worker_->c6PumpCost.reset(); // Только Main, вне активного probe scope.
    if(worker_){worker_->coupledTimes={};worker_->coupledPumpUs=worker_->coupledCopyUs=0;worker_->coupledDoneLoops=worker_->coupledDonePumps=worker_->coupledDoneAttempts=worker_->coupledDoneForced=worker_->coupledDoneTokens=0;worker_->coupledDoneOverflow=false;}
    if(worker_){worker_->pipelineTimes={};worker_->pipelineCopyUs=0;}
    if(worker_&&worker_->framePipeline)worker_->framePipeline->clearHartStats();
    periodStart_ = esp_timer_get_time();
    nextFrameUs_ = periodStart_;
    guestCycleMark_ = NES_APP_FRAME_PACE_CYCLES;
    paceRemainder_ = 0;
}

/**
 * @brief Продвигает срок следующего кадра по гостевым циклам с переносом дробного остатка. Это исключает накопление ошибки округления микросекунд.
 */
void NesApp::advanceFrameDeadline() noexcept {
    const auto cycles = NES_APP_FRAME_PACE_CYCLES;
    const auto rate = current_.pal ? 1662607ULL : 1789773ULL;
    const auto elapsed = (cycles-guestCycleMark_)*1000000ULL + paceRemainder_;
    guestCycleMark_ = cycles;
    nextFrameUs_ += static_cast<std::int64_t>(elapsed/rate);
    paceRemainder_ = elapsed%rate;
}

/**
 * @brief Ожидает ближайший срок кадра, сохраняя гостевую скорость выбранного региона. Задержка относится к host-планировщику, а не к CPU-циклам NES.
 */
void NesApp::paceFrame() noexcept {
    auto now = esp_timer_get_time();
    if (now-nextFrameUs_ > (current_.pal ? 20000 : 16639)) nextFrameUs_ = now;
    auto left = nextFrameUs_-now;
    if (left > 1500) vTaskDelay(pdMS_TO_TICKS(static_cast<std::uint32_t>((left-500)/1000)));
    now = esp_timer_get_time();
    left = nextFrameUs_-now;
    if (left > 0) esp_rom_delay_us(static_cast<std::uint32_t>(left));
}

