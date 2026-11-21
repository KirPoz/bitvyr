/** @file Worker.inl
 * @brief CPU1 mailbox, primary Machine и split audio boundaries.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Создаёт INTERNAL control, PSRAM snapshot, семафоры и задачу CPU1. Частичный отказ освобождает только ещё никем не заимствованные ресурсы.
 */
bool NesApp::initializeWorker() noexcept {
    if (worker_) return executionMode_ == ExecutionMode::Worker;
    auto* stateMemory = heap_caps_aligned_alloc(alignof(EmulationWorkerState),sizeof(EmulationWorkerState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stateMemory) {
        ESP_LOGW(tag,"NES_EXECUTION mode=synchronous reason=worker_state_allocation");
        return false;
    }
    auto* state = new (stateMemory) EmulationWorkerState{};
    auto* snapshotMemory = heap_caps_aligned_alloc(alignof(EmulationWorkerState::Snapshot),sizeof(EmulationWorkerState::Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snapshotMemory) state->snapshot = new (snapshotMemory) EmulationWorkerState::Snapshot{};
    state->app = this;
    state->start = xSemaphoreCreateBinaryStatic(&state->startStorage);
    state->done = xSemaphoreCreateBinaryStatic(&state->doneStorage);
    // Дополнительный 1 KiB стека CPU1 для actual H2 nesting; ESP-IDF принимает байты.
    // PC-диагностика сохраняет полный прежний ISR-резерв1536 байт сверх игрового стека.
    constexpr std::uint32_t workerStackBytes = 12288;
    if (!state->snapshot || !state->start || !state->done ||
        xTaskCreatePinnedToCore(workerMain,"nes_emulation",workerStackBytes,state,1,&state->task,1) != pdPASS) {
        // Задача создаётся только после всех ресурсов; неуспешный create не оставляет задачу.
        if (state->start) vSemaphoreDelete(state->start);
        if (state->done) vSemaphoreDelete(state->done);
        if (state->snapshot) { state->snapshot->~Snapshot(); heap_caps_free(state->snapshot); }
        state->~EmulationWorkerState(); heap_caps_free(state);
        ESP_LOGW(tag,"NES_EXECUTION mode=synchronous reason=worker_resources");
        return false;
    }
    worker_ = state;
    executionMode_ = ExecutionMode::Worker;
    ESP_LOGI(tag,"NES_EXECUTION mode=worker core=1 priority=1 stack_bytes=%u snapshot_bytes=%u",
             unsigned(workerStackBytes),unsigned(sizeof(EmulationWorkerState::Snapshot)));
    return true;
}

/**
 * @brief Создаёт аудио-пакеты и Main DSP для раздельных владельцев. Допускается только при присоединённом Worker.
 */
bool NesApp::initializeSplitAudio() noexcept {
    if (executionMode_ != ExecutionMode::Worker || !worker_ || worker_->inFlight) return false;
    auto& state = *worker_;
    if (state.audioSplit) return true;
    void* active = heap_caps_aligned_alloc(alignof(core::AudioPacket),sizeof(core::AudioPacket),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* retired = heap_caps_aligned_alloc(alignof(core::AudioPacket),sizeof(core::AudioPacket),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* renderer = heap_caps_aligned_alloc(alignof(core::AudioRenderer),sizeof(core::AudioRenderer),MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!active || !retired || !renderer) {
        heap_caps_free(active); heap_caps_free(retired); heap_caps_free(renderer);
        ESP_LOGW(tag,"NES_AUDIO_EXECUTION mode=synchronous reason=split_resources reset=0");
        return false;
    }
    state.activeAudio = new (active) core::AudioPacket{};
    state.retiredAudio = new (retired) core::AudioPacket{};
    state.audioRenderer = new (renderer) core::AudioRenderer{};
    // Worker ждёт start; настройка — явный reset владельца
    // до первой ROM. Последующий fallback также разрешён только при простое владельца.
    machine_.configureSplitAudio(state.activeAudio);
    state.audioSplit = true;
    ESP_LOGI(tag,"NES_AUDIO_EXECUTION mode=split renderer_core=0 packet_bytes=%u renderer_internal_bytes=%u",
             unsigned(2*sizeof(core::AudioPacket)),unsigned(sizeof(core::AudioRenderer)));
    return true;
}

/**
 * @brief Фиксирует ошибку аудио-владения и останавливает продолжение. Возвращает false для распространения отказа по вызывающей цепочке.
 * @param reason Заимствованное диагностическое описание отказа.
 */
bool NesApp::splitAudioFault(const char* reason) noexcept {
    board_.setAudioActive(false);
    if (!worker_ || !worker_->audioSplit) return false;
    auto& state = *worker_;
    state.audioFaultPending = true;
    ESP_LOGE(tag,"NES_AUDIO_SPLIT_FAIL reason=%s in_flight=%u",reason,unsigned(state.inFlight));
    // Задание CPU1 ещё может держать активный пакет и Machine. Reset,
    // отсоединение и free запрещены до настоящего семафора done.
    if (state.inFlight || executionMode_ == ExecutionMode::Faulted) return false;
    workerFault("cpu_bench_audio"); return false;
}

/**
 * @brief Доводит опубликованное аудио до принятого состояния Main и готовит PCM. Нельзя запускать его повторно на занятом/чужом пакете.
 */
bool NesApp::prepareSnapshotAudio() noexcept {
    // Remote FIFO — единственный PCM owner: status/pause/menu не выполняют повторный local retire/render.
    if(worker_&&worker_->c6Audio){
        if(worker_->framePipeline){if(worker_->inFlight)return true;return consumeFramePipelineFrame(false);}
        return true;
    }
    if (!worker_ || !worker_->audioSplit) return true;
    auto& state = *worker_;
    if(state.framePipeline){
        if(state.inFlight)return true;
        return consumeFramePipelineFrame(false); // Завершаются только sealed-парные кадры, а не частичный интервал.
    }
    // Thin capture принимает начальные reset-часы вместе с первым кадром.
    // Наблюдение frame0/пауза/EXIT не должно принимать ещё не sealed пакет.
    // Между load/reset и configureCoupledVideoPipeline invalidateSnapshot уже вызывает этот observer.
    // Не отделяем reset[0,7] в sequence0: первый sealed frame должен включать его точные APU события.
    const bool coupledResetStartup=!state.inFlight&&videoMapperSupported(current_.mapper)&&
        machine_.cpuCycles()==7U&&!machine_.frames();
    if(!state.audioPacketPending&&((state.coupled&&!frames_)||coupledResetStartup))return true;
    if (state.audioFaultPending) return splitAudioFault("deferred_recovery");
    if (!state.audioPacketPending) {
        // При обычном перекрытии читать разрешено только неизменяемый retired-пакет.
        // Если pending-пакета нет, это первый dispatch либо уже отрисованный снимок;
        // ни одно из состояний не разрешает наблюдателя живой Machine.
        if (state.inFlight) return true;
        if (state.audioRenderer->valid() &&
            state.audioRenderer->generation() == machine_.audioGeneration() &&
            state.audioRenderer->cycles() == machine_.cpuCycles()) return true;
        // Статус/загрузка/пауза при присоединённом владельце могут наблюдать reset-часы
        // или хвост инструкции до появления завершённого видеоснимка.
        if (!machine_.retireAudioPacket(*state.retiredAudio)) return splitAudioFault("retire_status");
        state.audioPacketPending = true;
        state.packetForSnapshot = false;
    }
    const auto begin = esp_timer_get_time();
    const auto header = state.retiredAudio->header();
    const auto rendered = state.audioRenderer->render(*state.retiredAudio);
    if (rendered != core::AudioRenderResult::Ok || !state.audioRenderer->valid() ||
        state.audioRenderer->generation() != header.generation ||
        state.audioRenderer->sequence() != header.sequence ||
        state.audioRenderer->ppuFrame() != header.ppuFrame ||
        state.audioRenderer->cycles() != header.endCycle)
        return splitAudioFault("render_or_sequence");
    if (state.audioRenderer->sampleCount() > core::Apu::sampleCapacity)
        return splitAudioFault("sample_count");
    const bool completedFramePacket = state.packetForSnapshot;
    if (completedFramePacket) {
        auto& snapshot = *state.snapshot;
        if (snapshot.audioGeneration != header.generation || snapshot.audioSequence != header.sequence ||
            snapshot.ppuFrame != header.ppuFrame || snapshot.cpuCycles != header.endCycle ||
            snapshot.region != header.region) return splitAudioFault("snapshot_sequence");
        snapshot.sampleCount = state.audioRenderer->sampleCount();
        std::memcpy(snapshot.pcm,state.audioRenderer->samples(),snapshot.sampleCount*sizeof(snapshot.pcm[0]));
    }
    state.audioPacketPending = state.packetForSnapshot = false;
    if (completedFramePacket) state.audioDspUs += esp_timer_get_time()-begin;
    return true;
}

/**
 * @brief Единственная задача CPU1 исполняет принятый JOB и подтверждает его настоящий выход.
 * @details Первичная Machine и заимствованные frame-ресурсы доступны только текущему Worker;
 * Main получает право чтения/освобождения после соответствующего done, а не по тайм-ауту.
 * @param context Долгоживущий EmulationWorkerState, опубликованный Main до запуска задачи.
 */
void NesApp::workerMain(void* context) noexcept {
    auto& state = *static_cast<EmulationWorkerState*>(context);
    auto lastIdleYieldUs=esp_timer_get_time();
    // Worker закреплён на CPU1. Idle1 уже переключён с CPU, поэтому его U32 runtime стабилен.
    static_assert(sizeof(configRUN_TIME_COUNTER_TYPE)==sizeof(std::uint32_t),"U32 idle runtime required");
    auto lastIdleRunTime=ulTaskGetIdleRunTimeCounter();
    for (;;) {
        if (xSemaphoreTake(state.start,portMAX_DELAY) == pdTRUE) {
            {
            const auto begin = esp_timer_get_time();
            if(state.legacyMachine){
                // Ввод/ROM/маршрут опубликованы start-семафором; Machine принадлежит CPU1 до done.
                state.result=state.app->machine_.stepFrame();
            }else{
            // Во время задания только состояние Machine принадлежит CPU1. Main может
            // после тайм-аута опубликовать терминальный Faulted; здесь нельзя читать
            // его обычный lifecycle-флаг. Ресурсы уже проверены при старте.
            auto& machine=state.app->machine_;
            const auto ownerReady=/** @brief На CPU1 проверяет готовность первичного observable/split владельца и отсутствие VideoFault без чтения lifecycle Main. */
                                  [&machine]() noexcept {
                return machine.observableVideoEnabled()&&machine.splitVideoEnabled()&&machine.splitAudio()&&
                    machine.videoFault()==core::VideoFault::None;
            };
            if(state.framePipeline)state.result=state.framePipeline->produce();
            else {
            state.result = core::RunStatus::VideoFault;
            if (ownerReady()
                &&(!state.coupled||state.coupled->workerCanRun())
                ) {
                state.result = machine.stepFrame();
                // Перед остановкой таймера завершаются уже учтённые хвосты CPU/PPU
                // на их владельце без вызова цветного наблюдателя.
                static_cast<void>(state.app->machine_.cartridge());
                if (!ownerReady()) state.result = core::RunStatus::VideoFault;
                if (state.app->machine_.audioFault()!=core::AudioFault::None) state.result=core::RunStatus::AudioFault;
            }
            }
            }

            if(state.coupled&&state.result==core::RunStatus::FrameReady&&
               !state.coupled->finalizePreAckDsp(state.app->machine_))state.result=core::RunStatus::AudioFault;
            if(state.coupled&&!state.coupled->workerFinished())state.result=core::RunStatus::VideoFault;
            state.coreUs = esp_timer_get_time()-begin;
            }
            const auto signalled = xSemaphoreGive(state.done);
            configASSERT(signalled == pdTRUE); // Ровно один completion на каждый start.
        }
        // IDLE1 обслуживает watchdog. Бюджет ограничен 50 ms плюс текущая работа;
        // guest-time, completion и приём каждого кадра от host-паузы не зависят.
        const auto now=esp_timer_get_time();
        // Успех start/done сам по себе не означает простой: кредитуется только реальный Idle runtime.
        const auto idleRunTime=ulTaskGetIdleRunTimeCounter();
        if(idleRunTime!=lastIdleRunTime){
            // Неравенство сохраняет смысл при U32 wrap; это не ручное подтверждение/кормление TWDT.
            lastIdleRunTime=idleRunTime;lastIdleYieldUs=now;
        }else if(now-lastIdleYieldUs>=50000){
            // Без наблюдаемого прогресса остаётся прежний50ms fallback на границе заданий.
            vTaskDelay(1);lastIdleYieldUs=esp_timer_get_time();
            // Учесть forced idle сейчас, чтобы не засчитать тот же прогресс повторно в следующем job.
            lastIdleRunTime=ulTaskGetIdleRunTimeCounter();
        }
    }
}

