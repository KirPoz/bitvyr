/** @file FrameVideo.inl
 * @brief Публикация Frame, приём завершённых слотов и раздельное владение PPA/LCD.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Возвращает номер входного задания Frame-тракта из его принятого состояния. Не материализует изменяемый PPU.
 */
std::uint64_t NesApp::framePipelineInputFrame()const noexcept {
    return worker_&&worker_->framePipeline?worker_->framePipeline->producedFrames():frames_;
}
/**
 * @brief Возвращает принятый гостевой CPU-cycle Frame-тракта для ввода и отчётов. Значение не является host-таймером.
 */
std::uint64_t NesApp::framePipelineCycle()const noexcept {
    return worker_&&worker_->framePipeline?worker_->framePipeline->acceptedCycle():machine_.cpuCycles();
}
/**
 * @brief Завершает прежнюю асинхронную передачу Frame-изображения и обновляет счётчики только по подтверждённому результату Board.
 */
bool NesApp::finishFramePipelinePresent()noexcept {
    if(!worker_||!worker_->pipelinePresentPending)return true;
    auto&w=*worker_;
    // Только скопированный lease Board Main; getter производителя/actor здесь недопустим.
    const bool valid=w.pipelinePresentGeneration==sessionGeneration_&&
        w.pipelinePresentFrame&&w.pipelinePresentFrame<=frames_;
    const auto begin=esp_timer_get_time();
    const bool finished=board_.finishGamePresent();
    const auto elapsed=esp_timer_get_time()-begin;presentUs_+=elapsed;
    if(!finished){
        if(!w.pipelinePresentFailed)++displayErrors_;
        w.pipelinePresentFailed=true;
        return false; // Идентичность и контекст сохраняются до настоящего успешного drain.
    }
    if(valid&&!w.pipelinePresentFailed){
        ++submittedFrames_;++w.pipelinePresentCompleted;
        w.pipelineCompletedGeneration=w.pipelinePresentGeneration;
        w.pipelineCompletedFrame=w.pipelinePresentFrame;
    }
    // Неоднозначный прежний LCD-отказ разрешает cleanup, но не выдуманный счётчик кадров.
    w.pipelinePresentPending=w.pipelinePresentFailed=false;
    w.pipelinePresentGeneration=w.pipelinePresentFrame=0;
    return valid;
}
/**
 * @brief Публикует принятый кадр Frame-тракта в PPA/LCD. Заимствование RGB сохраняется до соответствующего аппаратного завершения.
 */
bool NesApp::presentFramePipelineAsync()noexcept {
    auto&w=*worker_;auto&pipeline=*w.framePipeline;auto&s=*w.snapshot;
    const auto valid=/** @brief Повторно проверяет текущие Main generation/ROM/region и режим Worker до использования заимствованного цветного контекста. */
                     [&]()noexcept{return executionMode_==ExecutionMode::Worker&&
        pipeline.matches(activeRom_,current_.size,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_);};
    if(!playing_||!valid()||!pipeline.pixels()||s.generation!=sessionGeneration_||s.completedFrames!=frames_){
        workerFault("frame_pipeline_publication");return false;
    }
    // Direct-режим завершил прежнее чтение источника ДО записи C0.
    // Новый pending lease здесь нарушает инвариант; его нельзя скрывать drain
    // после того, как общий буфер уже мог быть перезаписан.
    if(w.pipelinePresentPending){workerFault("frame_pipeline_direct_pending");return false;}
    auto*destination=board_.gameFramebuffer();
    if(!destination||!valid()){workerFault("frame_pipeline_rgb_owned");return false;}
    if(destination!=pipeline.pixels()){workerFault("frame_pipeline_direct_identity");return false;}
    if(!valid()){workerFault("frame_pipeline_after_copy");return false;}
    const auto begin=esp_timer_get_time();const bool started=board_.beginGamePresent();presentUs_+=esp_timer_get_time()-begin;
    if(!started){++displayErrors_;workerFault("frame_pipeline_ppa");return false;}
    w.pipelinePresentGeneration=s.generation;w.pipelinePresentFrame=s.completedFrames;
    w.pipelinePresentPending=true;w.pipelinePresentFailed=false;++w.pipelinePresentStarted;
    if(!valid()){workerFault("frame_pipeline_after_present");return false;}
    const auto audio=esp_timer_get_time();
    if(!settings_.muted&&settings_.volume>0)audioShort_+=s.sampleCount-board_.writeAudio(s.pcm,s.sampleCount);
    audioUs_+=esp_timer_get_time()-audio;return true;
}

/**
 * @brief Создаёт двухслотовый Frame-контекст и связывает ROM, регион, поколение, DSP и RGB. Все заимствованные ресурсы проверяются до допуска Worker.
 */
bool NesApp::configureFramePipeline()noexcept {
    // Mapper без Output capability исполняет полные PPU/APU и публикует legacy snapshot.
    // Отсутствующая Output capability не заменяется мнимой успешной арендой кадра.
    if(worker_&&worker_->legacyMachine)return !worker_->inFlight&&!worker_->framePipeline;

    if(!videoWorkerReady()||!worker_||worker_->inFlight||worker_->framePipeline||!activeRom_||!sessionGeneration_||
       !worker_->activeAudio||!worker_->audioRenderer||worker_->audioPacketPending||machine_.cpuCycles()!=7)return false;
    const bool previous=worker_->pipelinePresentPending;
    if(!finishFramePipelinePresent())return false;
    if(!previous){
        const auto start=esp_timer_get_time();const bool finished=board_.finishGamePresent();presentUs_+=esp_timer_get_time()-start;
        if(!finished)return false;
    }
    auto*const borrowedRgb=board_.gameFramebuffer();
    if(!borrowedRgb)return false;
    worker_->framePipeline=FramePipeline::createOutput(machine_,activeRom_,current_.size,
        current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_,*worker_->activeAudio,borrowedRgb,core::Ppu::width*core::Ppu::height,
        FramePipeline::Mmc5SessionSpan{mmc5Session_,mmc5Session_?sizeof(Mmc5Session):0U});
    if(!worker_->framePipeline)return false;
    // Sticky factory handle сохраняется для joined cleanup, его нельзя потерять.
    if(worker_->framePipeline->error()!=FramePipeline::Error::None)return false;
    if(!worker_->framePipeline->setRamWitnessPolicyAtCreate(worker_->ramWitnessPolicy))return false;

    ESP_LOGI(tag,"NES_OUTPUT_FRAME_PIPELINE_MODE schema=2 mapper=%u producer=timing_fetch consumer=output_compositor read_witness=unavailable slots=2 rgb=board_internal_direct borrowed=1 copy=0 early_dsp=1 effective_tail=off game_fps_claim=0",unsigned(current_.mapper));
    return true;
}
/**
 * @brief Завершает владельцев и освобождает старый Frame-контекст перед новой ROM. При неизвестном Worker/PPA возвращает отказ и удерживает память.
 */
bool NesApp::releaseFramePipelineForLoad()noexcept {
    if(!finishFramePipelinePresent())return false;
    if(!worker_||!worker_->framePipeline)return true;
    if(worker_->inFlight||executionMode_!=ExecutionMode::Worker)return false;
    if(worker_->framePipeline->hasReady()&&!consumeFramePipelineFrame(false))return false;
    if(!FramePipeline::destroyJoined(worker_->framePipeline))return false;
    worker_->framePipeline=nullptr;return true;
}
/**
 * @brief Получает настоящее завершение производителя и потребляет предыдущий Frame-slot. Ошибки lease/sequence/cut переводят тракт в отказ.
 */
bool NesApp::retireFramePipelineFrame()noexcept {
    auto&w=*worker_;auto&pipeline=*w.framePipeline;const auto begin=esp_timer_get_time();
    if(xSemaphoreTake(w.done,pdMS_TO_TICKS(1500))!=pdTRUE){pipeline.stop(FramePipeline::Error::Deadline);workerFault("frame_pipeline_timeout");return false;}
    w.inFlight=false;
    const bool joined=pipeline.joinedProducer(); // Настоящий done подтверждается и для неуспешного задания.
    workerWaitUs_+=esp_timer_get_time()-begin;
    if(!joined||w.result!=core::RunStatus::FrameReady||w.generation!=sessionGeneration_||
       w.inputFrame+1!=pipeline.producedFrames()||!pipeline.matches(activeRom_,current_.size,
       current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)){
        workerFault("frame_pipeline_producer");return false;
    }
    frameLoopMetrics.joined(this,w.inputFrame,w.inputUs);
    return true;
}
/**
 * @brief На Main воспроизводит цвет и DSP одного опубликованного кадра, проверяет совместный результат и принимает слот. Прежний PPA-source должен быть завершён до записи RGB.
 * @param publish Разрешение аппаратного вывода; false используется для безопасного завершения без новой публикации.
 */
bool NesApp::consumeFramePipelineFrame(bool publish)noexcept {
    if(!worker_||!worker_->framePipeline){
        if(!publish&&!finishFramePipelinePresent()){workerFault("frame_pipeline_observer_ppa");return false;}
        return true;
    }
    auto&w=*worker_;auto&pipeline=*w.framePipeline;
    if(!pipeline.hasReady()){
        if(!publish&&!finishFramePipelinePresent()){workerFault("frame_pipeline_observer_ppa");return false;}
        return pipeline.error()==FramePipeline::Error::None;
    }
    const auto valid=/** @brief Повторно проверяет текущие Main generation/ROM/region и режим Worker до использования заимствованного цветного контекста. */
                     [&]()noexcept{return executionMode_==ExecutionMode::Worker&&pipeline.matches(activeRom_,current_.size,
        current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_);};
    if(!valid()||!pipeline.prepareDsp(*w.audioRenderer)){workerFault("frame_pipeline_prepare_dsp");return false;}
    /** @brief RAII-защита подготовленного DSP/цветного потребления; аварийный выход снимает заимствование до fault-cleanup. */
    struct PreparedGuard {
        FramePipeline*active;
        /**
         * @brief Отменяет подготовленный Frame-потребитель и снимает RAII-заимствование до обработки ошибки.
         */
        void abortAndDisarm()noexcept{auto*p=active;active=nullptr;if(p)static_cast<void>(p->abortPrepared());}
        /**
         * @brief Снимает RAII-защиту только после явного завершения соответствующей операции.
         */
        void disarm()noexcept{active=nullptr;}
        /**
         * @brief При незавершённом подготовленном потреблении выполняет аварийное снятие/отмену, не выдавая частичный результат за принятый кадр.
         */
        ~PreparedGuard(){abortAndDisarm();}
    }prepared{&pipeline};
    const auto fail=/** @brief Сначала снимает подготовленное DSP-заимствование, затем переводит Worker в fault; возвращает false без публикации частичного кадра. */
                    [&](const char*reason)noexcept{prepared.abortAndDisarm();workerFault(reason);return false;};
    if(!valid())return fail("frame_pipeline_early_session");
    // DSP(N) использовал только неизменяемые пакеты, пока PPA(N-1) держала источник.
    // До настоящего завершения source-drain запрещены getter/запись Ppu/RGB.
    const bool previous=w.pipelinePresentPending;
    if(!finishFramePipelinePresent())return fail("frame_pipeline_direct_previous_ppa");
    if(!previous){
        const auto start=esp_timer_get_time();const bool finished=board_.finishGamePresent();presentUs_+=esp_timer_get_time()-start;
        if(!finished)return fail("frame_pipeline_direct_previous_ppa");
    }
    auto*const destination=board_.gameFramebuffer();const auto*const accepted=pipeline.pixels();
    if(!destination||(accepted&&accepted!=destination))return fail("frame_pipeline_direct_owned");
    if(!valid()||!pipeline.consumeColour(*w.audioRenderer)||!valid()||!pipeline.accept(sessionGeneration_))return fail("frame_pipeline_consumer");
    prepared.disarm(); // Фаза Idle; последующий вложенный fault-cleanup может освободить контекст.
    const auto&v=pipeline.snapshot();const auto&t=pipeline.times();auto&s=*w.snapshot;
    if(v.frame!=frames_+1||v.inputFrame+1!=v.frame||v.appGeneration!=sessionGeneration_||v.sampleCount>core::Apu::sampleCapacity)return fail("frame_pipeline_accept");
    s.ready=false;s.generation=sessionGeneration_;s.completedFrames=v.frame;s.region=v.region;
    s.inputPad=v.pad;s.ppuFrame=v.frame;s.cpuCycles=v.cycle;s.audioGeneration=v.audioGeneration;s.audioSequence=v.audioSequence;
    s.sampleCount=v.sampleCount;std::memcpy(s.pcm,w.audioRenderer->samples(),v.sampleCount*sizeof(s.pcm[0]));
    ++frames_;++periodFrames_;advanceFrameDeadline();coreUs_+=t.core;maxCoreUs_=std::max(maxCoreUs_,std::int64_t(t.core));
    w.pipelineTimes.core+=t.core;w.pipelineTimes.producer+=t.producer;w.pipelineTimes.seal+=t.seal;
    w.pipelineTimes.audioRetire+=t.audioRetire;w.pipelineTimes.snapshot+=t.snapshot;
    w.pipelineTimes.replay+=t.replay;w.pipelineTimes.dsp+=t.dsp;w.pipelineTimes.witness+=t.witness;
    if(!publish)return true;
    return presentFramePipelineAsync();
}
/**
 * @brief Печатает принятые CPU/RAM/PCM/RGB/read/input-срезы и раздельные produced/accepted/submitted. Не объявляет физический VSYNC или качество всей игры.
 */
void NesApp::framePipelineStatus()noexcept {
    NES_LOOP_TAINT()
    if(!finishFramePipelinePresent()){workerFault("frame_pipeline_status_ppa");return;}
    if(!worker_||!worker_->framePipeline){
        std::printf("NES_OUTPUT_FRAME_PIPELINE_STATUS {\"playing\":false,\"frames\":0,\"generation\":%llu,\"owner\":\"unbound\",\"image_available\":false,\"game_fps_claim\":false}\n",static_cast<unsigned long long>(sessionGeneration_));return;
    }
    auto&pipeline=*worker_->framePipeline;
    const auto&w=*worker_;
    std::printf("NES_FRAME_PIPELINE_PRESENT_STATUS {\"generation\":%llu,\"accepted_frame\":%llu,\"started_frames\":%llu,\"completed_frames\":%llu,\"lcd_submitted_frames\":%llu,\"pending\":%s,\"pending_generation\":%llu,\"pending_frame\":%llu,\"completed_generation\":%llu,\"completed_frame\":%llu}\n",
        static_cast<unsigned long long>(sessionGeneration_),static_cast<unsigned long long>(frames_),
        static_cast<unsigned long long>(w.pipelinePresentStarted),static_cast<unsigned long long>(w.pipelinePresentCompleted),static_cast<unsigned long long>(submittedFrames_),
        w.pipelinePresentPending?"true":"false",static_cast<unsigned long long>(w.pipelinePresentGeneration),static_cast<unsigned long long>(w.pipelinePresentFrame),
        static_cast<unsigned long long>(w.pipelineCompletedGeneration),static_cast<unsigned long long>(w.pipelineCompletedFrame));
    if(executionMode_==ExecutionMode::Faulted||!pipeline.matches(activeRom_,current_.size,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)){
        std::printf("NES_FRAME_PIPELINE_BENCH_ERROR reason=session fault=%u\n",unsigned(pipeline.error()));return;
    }
    const bool witnessed=pipeline.acceptedFrames()&&pipeline.refreshWitness(*worker_->audioRenderer);
    const auto&v=pipeline.snapshot();const auto&cpu=v.cpu;
    if(v.ram.policy==FrameRamPolicy::Sparse120){
        char ramJson[768];
        if(!formatFrameRamWitness(ramJson,sizeof(ramJson),v.ram,v.ramHash,witnessed)){
            std::printf("NES_FRAME_PIPELINE_BENCH_ERROR reason=ram_status_format\n");return;
        }
        std::printf("NES_OUTPUT_FRAME_PIPELINE_STATUS {\"playing\":%s,\"game\":%u,\"frames\":%llu,\"produced_frames\":%llu,\"submitted_frames\":%llu,\"generation\":%llu,\"mapper\":%u,\"region\":\"%s\",\"crc32\":\"%08lx\",\"schema_version\":3,\"owner\":\"timing_fetch\",\"consumer\":\"output_compositor\",\"cpu_snapshot\":\"actual_primary_cut\",\"read_witness\":\"unavailable\",\"effective_tail\":\"off\",\"game_fps_claim\":false,\"pc\":%u,\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,\"p\":%u,\"cpu_cycles\":%llu,\"cpu_state_cycles\":%llu,%s,\"pcm_count\":%u,\"pcm_hash\":\"%016llx\",\"rgb_hash\":\"%016llx\",\"image_available\":%s,\"input_valid\":%s,\"input_frame\":%llu,\"input_mask\":%u,\"read_count\":%llu,\"read_hash\":\"%016llx\"}\n",
            playing_?"true":"false",unsigned(selected_),static_cast<unsigned long long>(pipeline.acceptedFrames()),
            static_cast<unsigned long long>(pipeline.producedFrames()),static_cast<unsigned long long>(submittedFrames_),static_cast<unsigned long long>(sessionGeneration_),
            unsigned(current_.mapper),current_.pal?"PAL":"NTSC",static_cast<unsigned long>(current_.crc),unsigned(cpu.pc),unsigned(cpu.a),unsigned(cpu.x),unsigned(cpu.y),unsigned(cpu.sp),unsigned(cpu.p),
            static_cast<unsigned long long>(v.cycle),static_cast<unsigned long long>(cpu.cycles),ramJson,unsigned(witnessed?v.sampleCount:0),
            static_cast<unsigned long long>(witnessed?v.pcmHash:0),static_cast<unsigned long long>(witnessed?v.rgbHash:0),witnessed?"true":"false",witnessed?"true":"false",
            static_cast<unsigned long long>(v.inputFrame),unsigned(v.pad),static_cast<unsigned long long>(v.readCount),static_cast<unsigned long long>(v.readHash));
        return;
    }

    std::printf(
"NES_OUTPUT_FRAME_PIPELINE_STATUS {\"playing\":%s,\"game\":%u,\"frames\":%llu,\"produced_frames\":%llu,\"submitted_frames\":%llu,\"generation\":%llu,\"mapper\":%u,\"region\":\"%s\",\"crc32\":\"%08lx\",\"schema_version\":2,\"owner\":\"timing_fetch\",\"consumer\":\"output_compositor\",\"cpu_snapshot\":\"actual_primary_cut\",\"read_witness\":\"unavailable\",\"effective_tail\":\"off\",\"game_fps_claim\":false,\"pc\":%u,\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,\"p\":%u,\"cpu_cycles\":%llu,\"cpu_state_cycles\":%llu,\"ram_hash\":\"%016llx\",\"pcm_count\":%u,\"pcm_hash\":\"%016llx\",\"rgb_hash\":\"%016llx\",\"image_available\":%s,\"input_valid\":%s,\"input_frame\":%llu,\"input_mask\":%u,\"read_count\":%llu,\"read_hash\":\"%016llx\"}\n",
        playing_?"true":"false",unsigned(selected_),static_cast<unsigned long long>(pipeline.acceptedFrames()),
        static_cast<unsigned long long>(pipeline.producedFrames()),static_cast<unsigned long long>(submittedFrames_),static_cast<unsigned long long>(sessionGeneration_),
        unsigned(current_.mapper),current_.pal?"PAL":"NTSC",static_cast<unsigned long>(current_.crc),unsigned(cpu.pc),unsigned(cpu.a),unsigned(cpu.x),unsigned(cpu.y),unsigned(cpu.sp),unsigned(cpu.p),
        static_cast<unsigned long long>(v.cycle),static_cast<unsigned long long>(cpu.cycles),static_cast<unsigned long long>(witnessed?v.ramHash:0),unsigned(witnessed?v.sampleCount:0),
        static_cast<unsigned long long>(witnessed?v.pcmHash:0),static_cast<unsigned long long>(witnessed?v.rgbHash:0),witnessed?"true":"false",witnessed?"true":"false",
        static_cast<unsigned long long>(witnessed?v.inputFrame:0),witnessed?unsigned(v.pad):0U,static_cast<unsigned long long>(v.readCount),static_cast<unsigned long long>(v.readHash));
}
/**
 * @brief Печатает накопленные120-кадровые метрики Frame и сбрасывает только окно измерений. Перекрывающиеся времена не складываются в FPS.
 */
void NesApp::reportFramePipeline()noexcept {
    if(!worker_||!worker_->framePipeline||!playing_||periodFrames_<120)return;
    if(!finishFramePipelinePresent()){workerFault("frame_pipeline_report_ppa");return;}
    const auto n=periodFrames_;auto&w=*worker_;const auto&t=w.pipelineTimes;
    // Тот же accepted120/PPA-finished cut, что в P4+C6; UART logger-ms не используется.
    const auto reportDeviceUs=esp_timer_get_time();
    lastAverageCoreUs_=static_cast<std::int64_t>(t.core/n);
#define NES_FRAME_PIPELINE_TIMES_FORMAT "NES_OUTPUT_FRAME_PIPELINE_TIMES game=%u frames=%llu produced_frames=%llu submitted_frames=%llu samples=%llu generation=%llu mapper=%u region=%s core_us=%llu producer_wall_us=%llu seal_us=%llu audio_retire_us=%llu snapshot_us=%llu consumer_active_us=%llu dsp_us=%llu witness_copy_us=%llu rgb_copy_us=%llu present_us=%lld wait_us=%lld audio_us=%lld device_us=%lld slots=2 schema=2 producer=timing_fetch consumer=output_compositor read_witness=unavailable effective_tail=off game_fps_claim=0"
    ESP_LOGI(tag,NES_FRAME_PIPELINE_TIMES_FORMAT,
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(w.framePipeline->producedFrames()),static_cast<unsigned long long>(submittedFrames_),
        static_cast<unsigned long long>(n),static_cast<unsigned long long>(sessionGeneration_),unsigned(current_.mapper),current_.pal?"PAL":"NTSC",
        static_cast<unsigned long long>(t.core/n),static_cast<unsigned long long>(t.producer/n),static_cast<unsigned long long>(t.seal/n),static_cast<unsigned long long>(t.audioRetire/n),
        static_cast<unsigned long long>(t.snapshot/n),static_cast<unsigned long long>(t.replay/n),static_cast<unsigned long long>(t.dsp/n),static_cast<unsigned long long>(t.witness/n),
        static_cast<unsigned long long>(w.pipelineCopyUs/n),static_cast<long long>(presentUs_/n),static_cast<long long>(workerWaitUs_/n),static_cast<long long>(audioUs_/n),static_cast<long long>(reportDeviceUs));
#undef NES_FRAME_PIPELINE_TIMES_FORMAT
    w.framePipeline->reportHartStats(unsigned(selected_)); // Только Main-owned accepted окно, не producer N+1.
    w.pipelineTimes={};w.pipelineCopyUs=0;periodFrames_=0;coreUs_=maxCoreUs_=workerWaitUs_=presentUs_=audioUs_=0;periodStart_=esp_timer_get_time();
}


