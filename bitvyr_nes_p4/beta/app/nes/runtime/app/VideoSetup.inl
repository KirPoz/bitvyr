/** @file VideoSetup.inl
 * @brief Подготовка цифрового audio/video Worker и допустимого backend на joined reset-cut.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */

/**
 * @brief Включает либо выключает раздельное видео на безопасной границе. Не переключает владельца посреди активного Worker-задания.
 * @param enabled Запрашиваемое состояние режима на присоединённой границе.
 */
bool NesApp::setSplitVideoMode(bool enabled) noexcept {
    if(!enabled){
        if(executionMode_==ExecutionMode::Faulted||(worker_&&worker_->inFlight))return false;
        if(!machine_.legacyVideoReady())return false;
    }
    if(!worker_||executionMode_!=ExecutionMode::Worker||worker_->inFlight)return !enabled;
    auto& state=*worker_;
    if(!state.videoRenderer)return !enabled;
    if(!prepareSnapshotVideo(false))return false;
    if(state.videoBound==enabled)return true;
    state.videoRenderer->setRgb565Output(nullptr);
    // Оба владельца capture/consumer присоединены. Binding — явный reset;
    // ресурсы живут до перезапуска, в том числе после позднего completion/тайм-аута.
    if(!enabled)machine_.configureSplitVideo(nullptr);
    state.videoStorage[0]->abandon();state.videoStorage[1]->abandon();
    state.videoRenderer->reset();
    state.spareVideo=state.videoStorage[1];state.retiredVideo=nullptr;
    state.videoPending=state.videoForSnapshot=state.videoRgbReady=false;
    state.videoBound=false;
    if(enabled){
        machine_.configureSplitVideo(state.videoStorage[0],state.videoRenderer);
        state.videoBound=machine_.splitVideoEnabled()&&machine_.videoFault()==core::VideoFault::None;
        if(!state.videoBound)machine_.configureSplitVideo(nullptr);
    }
    ESP_LOGI(tag,"NES_VIDEO_EXECUTION mode=%s",state.videoBound?"split":"legacy");
    return state.videoBound==enabled;
}

/**
 * @brief Фиксирует отказ split-video и удерживает неопределённые заимствования вместо небезопасного освобождения.
 * @param reason Заимствованное диагностическое описание отказа.
 */
bool NesApp::splitVideoFault(const char* reason) noexcept {
    board_.setAudioActive(false);
    if(!worker_||!worker_->videoRenderer)return false;
    auto& state=*worker_;state.videoFaultPending=true;
    state.videoRenderer->setRgb565Output(nullptr);
    ESP_LOGE(tag,"NES_VIDEO_SPLIT_FAIL reason=%s in_flight=%u",reason,unsigned(state.inFlight));
    if(state.inFlight||executionMode_==ExecutionMode::Faulted)return false;
    if(!machine_.legacyVideoReady())return legacyVideoFailure("legacy_video_memory");
    machine_.configureSplitVideo(nullptr); // Явный Legacy reset, а не непрерывное продолжение.
    state.videoBound=state.videoPending=state.videoForSnapshot=state.videoRgbReady=false;
    state.videoStorage[0]->abandon();state.videoStorage[1]->abandon();
    state.spareVideo=state.videoStorage[1];state.retiredVideo=nullptr;
    state.videoRenderer->reset();state.videoFaultPending=false;
    // Явный reset Machine также начинает новое поколение аудио.
    if(state.audioSplit){
        state.audioPacketPending=state.packetForSnapshot=false;
        state.audioRenderer->reset();
    }
    stopPlayback();resetCounters();menuDirty_=true;
    return false;
}

/**
 * @brief Воспроизводит опубликованный видеопакет либо продвигает его состояние без вывода. Поколение и последовательность проверяются до доступа к цвету.
 * @param rgb Разрешение цветного вывода при воспроизведении пакета.
 */
bool NesApp::prepareSnapshotVideo(bool rgb) noexcept {
    if(!worker_||!worker_->videoBound)return true;
    auto& state=*worker_;
    if(state.videoFaultPending)return splitVideoFault("pending");
    std::uint16_t* output=rgb?board_.gameFramebuffer():nullptr;
    if(rgb&&!output)return splitVideoFault("rgb_owned");
    state.videoRenderer->setRgb565Output(output);
    if(state.videoPending){
        const auto header=state.retiredVideo->header();
        const auto result=state.videoRenderer->render(*state.retiredVideo);
        state.videoRenderer->setRgb565Output(nullptr);
        if(result!=core::VideoRenderResult::Ok||state.videoRenderer->generation()!=header.generation||
            state.videoRenderer->sequence()!=header.sequence||state.videoRenderer->ppuFrame()!=header.ppuFrame||
            state.videoRenderer->endCycle()!=header.endCycle)return splitVideoFault("render_or_sequence");
        if(state.videoForSnapshot){
            const auto& snapshot=*state.snapshot;
            if(snapshot.videoGeneration!=header.generation||snapshot.videoSequence!=header.sequence||
                snapshot.videoFrame!=header.ppuFrame||snapshot.videoCycles!=header.endCycle||
                (snapshot.region==core::Region::Pal)!=header.pal)return splitVideoFault("snapshot_sequence");
            state.videoRgbReady=rgb;
        }
        if(!state.retiredVideo->release())return splitVideoFault("release");
        state.spareVideo=state.retiredVideo;state.retiredVideo=nullptr;
        state.videoPending=state.videoForSnapshot=false;
    }else{
        if(rgb&&!state.videoRgbReady){state.videoRenderer->refreshRgb565();state.videoRgbReady=true;}
        state.videoRenderer->setRgb565Output(nullptr);
    }
    return true;
}

/**
 * @brief Проверяет готовность Worker, цифрового audio и текущего ROM lease перед настройкой или выполнением кадра.
 */
bool NesApp::videoWorkerReady() const noexcept {
    if(executionMode_!=ExecutionMode::Worker||!worker_||!worker_->snapshot||!worker_->audioSplit||
       worker_->videoBound||worker_->videoPending||worker_->videoFaultPending)return false;
    if(worker_->framePipeline)return worker_->framePipeline->matches(activeRom_,current_.size,
        current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_);
    // Без Frame context это только joined startup check; занятую Machine не читаем.
    return !worker_->inFlight&&machine_.cpuCycles()==7U&&!machine_.frames()&&
        machine_.splitAudio()&&machine_.videoFault()==core::VideoFault::None;
}
/**
 * @brief На присоединённой границе выбирает полный backend либо настраивает observable CPU/PPU и аудио-пакеты.
 * @details Полный backend допускается по capability mapper с собственными PPU/APU;
 * legacyMachine публикуется start-семафором и не меняется до done. Проверки
 * observable владельца остаются отдельными: legacy не подменяет их успешным ответом.
 */
bool NesApp::configureVideoWorker() noexcept {
    if(executionMode_!=ExecutionMode::Worker||!worker_||worker_->inFlight||!worker_->snapshot||
       machine_.cpuCycles()>7U)return false;
    // Новый ROM уже принадлежит App. Только здесь устанавливается его окончательный audio mode.
    // Нельзя сбрасывать заём или pending fault ради смены capability.
    if(worker_->audioPacketPending||worker_->audioFaultPending||worker_->framePipeline||machine_.outputFramesEnabled())return false;
    if(!prepareCartridgeSession(current_.mapper))return false;
    if(!videoMapperSupported(current_.mapper)){
        // Полный PPU/APU — отдельный backend. Он не получает observable/Frame capability.
        auto& state=*worker_;
        if(state.videoBound||state.videoPending||state.videoFaultPending||state.audioSplit||
           state.audioPacketPending||state.audioFaultPending||machine_.splitVideoEnabled()||machine_.splitAudio()||
           machine_.videoFault()!=core::VideoFault::None||machine_.audioFault()!=core::AudioFault::None)return false;
        if(state.framePipeline)return false;
        if(!machine_.legacyVideoReady())return false;
        state.legacyMachine=true;
        ESP_LOGI(tag,"NES_FRAME_BACKEND mode=legacy_machine mapper=%u owner=worker audio=full video=full",unsigned(current_.mapper));
        return true;
    }
    worker_->legacyMachine=false;
    if(executionMode_!=ExecutionMode::Worker||!worker_||worker_->inFlight||!worker_->snapshot||
       !worker_->audioSplit||worker_->audioFaultPending||!worker_->activeAudio||!worker_->retiredAudio||!worker_->audioRenderer||
       machine_.cpuCycles()>7U)return false;
    auto& state=*worker_;
    if(state.videoPending)return false; // Предыдущее поколение завершается до загрузки новой ROM.
    if(state.videoRenderer){state.videoRenderer->setRgb565Output(nullptr);state.videoRenderer->reset();}
    for(auto* packet:state.videoStorage)if(packet)packet->abandon();
    state.spareVideo=state.videoStorage[1];state.retiredVideo=nullptr;
    state.videoBound=state.videoPending=state.videoForSnapshot=state.videoRgbReady=false;
    const bool ready=videoWorkerReady()&&machine_.audioFault()==core::AudioFault::None;
    if(ready){
        constexpr unsigned diagnostic=1;
        ESP_LOGI(tag,"NES_OUTPUT_FRAME_SETUP owner=joined_reset pending_explicit_output_bind=1 core_diag=%u image_available=0 game_fps_claim=0",diagnostic);
    }
    return ready;
}
/**
 * @brief Печатает CPU-only throughput и контрольные состояния с явным отсутствием игрового FPS/изображения.
 */
void NesApp::videoWorkerStatus() noexcept {
    framePipelineStatus();
    // Вызов может вызвать fault/освобождение. Используются текущие метаданные Main, не сохранённый actor.
    frameAudioStatus(board_,sessionGeneration_,frames_,playing_,settings_.muted);
    return;

}

