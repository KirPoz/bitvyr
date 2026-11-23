/** @file FrameVideo.inl
 * @brief Main-сторона Frame: actual join, immutable payload, PPA/LCD и POSTJOIN audio.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
#include "FrameDoneWait.inl"
#ifndef NES_EXPERIMENTAL_FRAME_COLOUR_PUMP
#define NES_EXPERIMENTAL_FRAME_COLOUR_PUMP 0
#endif
static_assert(NES_EXPERIMENTAL_FRAME_COLOUR_PUMP==0||NES_EXPERIMENTAL_FRAME_COLOUR_PUMP==1,"colour pump boolean");
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
namespace {
/** @brief Эксперимент и счётчики только для Main. Включение в Library, без сохранения в NVS. */
struct FrameDirectPresentMode final {
    bool enabled=false;
    bool requested=false; // Запрос Library хранится рядом с эффективным флагом игры.
    std::uint32_t earlySubmitted=0, sourceReused=0;
};
FrameDirectPresentMode frameDirectPresent;
}
#endif
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
 * @brief Продвигает Frame-изображение по аппаратному результату Board.
 * @param poll Неблокирующий опрос во время CPU1. true при Pending сохраняет все владения.
 * @return При обычном finish успех подтверждает завершение. При poll Pending также допустим.
 */
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
bool NesApp::framePipelinePresentMatches(std::uint64_t previousFrame)const noexcept {
    const auto& w=*worker_;
    return previousFrame&&w.pipelinePresentPending&&!w.pipelinePresentFailed&&
        w.pipelinePresentGeneration==sessionGeneration_&&w.pipelinePresentFrame==previousFrame&&
        w.pipelinePresentStarted>w.pipelinePresentCompleted&&
        w.pipelinePresentStarted-w.pipelinePresentCompleted==1;
}
bool NesApp::finishFramePipelinePresent()noexcept {
    return finishFramePipelinePresent(false);
}
bool NesApp::finishFramePipelinePresent(bool poll)noexcept {
#else
bool NesApp::finishFramePipelinePresent()noexcept {
#endif
    if(!worker_||!worker_->pipelinePresentPending)return true;
    auto&w=*worker_;
    // Только скопированный lease Board Main; getter производителя/actor здесь недопустим.
    const bool valid=w.pipelinePresentGeneration==sessionGeneration_&&
        w.pipelinePresentFrame&&w.pipelinePresentFrame<=frames_;
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
    if(poll&&(!frameDirectPresent.enabled||!framePipelinePresentMatches(frames_)))return false;
    using Result=platform::GamePresentPollResult;
    const auto begin=esp_timer_get_time();
    const auto result=poll?board_.pollGamePresent():(board_.finishGamePresent()?Result::Submitted:Result::Fault);
    presentUs_+=esp_timer_get_time()-begin;
    if(result==Result::Pending)return true;
    const bool finished=result==Result::Submitted;
    if(poll&&finished)++frameDirectPresent.earlySubmitted;
#else
    const auto begin=esp_timer_get_time();
    const bool finished=board_.finishGamePresent();
    const auto elapsed=esp_timer_get_time()-begin;presentUs_+=elapsed;
#endif
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
        pipeline.matches(activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_);};
    if(!playing_||!valid()||!pipeline.pixels()||s.generation!=sessionGeneration_||s.completedFrames!=frames_){
        workerFault("frame_pipeline_publication");return false;
    }
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
    // RGB уже переписан после PPA completion; native-кадр ещё может ожидать LCD.
    // Удерживаем его точный lease до успешной подачи старого индекса.
    if(w.pipelinePresentPending&&
       (!frameDirectPresent.enabled||!framePipelinePresentMatches(s.completedFrames-1)||!finishFramePipelinePresent())){
        workerFault("frame_pipeline_direct_pending");return false;
    }
#else
    // Direct-режим завершил прежнее чтение источника ДО записи C0.
    // Новый pending lease здесь нарушает инвариант; его нельзя скрывать drain
    // после того, как общий буфер уже мог быть перезаписан.
    if(w.pipelinePresentPending){workerFault("frame_pipeline_direct_pending");return false;}
#endif
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
bool NesApp::configureFrameVideoPipeline()noexcept {
    // Не выделять второй frontend и не включать Frame fallback после отказа MMC5.
    if(worker_&&worker_->coupled)return usesCoupledVideo(current_.mapper)&&!worker_->inFlight&&!worker_->framePipeline;
    if(usesCoupledVideo(current_.mapper))return false;
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
    bool useCpu0Fm=false;
#if NES_APP_CPU0_VRC7
    useCpu0Fm=machine_.hasVrc7Fm(); // Actual capability после joined fullreset; submapper1 исключён.
#endif
    if(worker_->frameFm)return false;
    worker_->framePipeline=FrameVideoPipeline::createOutput(machine_,activeRom_,activeRomBytes_,
        current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_,*worker_->activeAudio,borrowedRgb,core::Ppu::width*core::Ppu::height,useCpu0Fm);
    if(!worker_->framePipeline)return false;
    // Только cold witness actual буфера: flags не подменяют свойства обоих концов.
    const auto*rgbLast=reinterpret_cast<const std::uint8_t*>(borrowedRgb)+122879U;
    std::printf("NES_FRAME_RGB_SOURCE ptr=%p last=%p bytes=122880 align=128 internal_first=%u internal_last=%u external_first=%u external_last=%u dma_first=%u dma_last=%u rgb_copy=0\n",
        static_cast<void*>(borrowedRgb),const_cast<std::uint8_t*>(rgbLast),unsigned(esp_ptr_internal(borrowedRgb)),unsigned(esp_ptr_internal(rgbLast)),
        unsigned(esp_ptr_external_ram(borrowedRgb)),unsigned(esp_ptr_external_ram(rgbLast)),unsigned(esp_ptr_dma_capable(borrowedRgb)),unsigned(esp_ptr_dma_capable(rgbLast)));
    // Sticky factory handle сохраняется для joined cleanup, его нельзя потерять.
    if(worker_->framePipeline->error()!=FrameVideoPipeline::Error::None)return false;
    if(!worker_->framePipeline->setRamWitnessPolicyAtCreate(worker_->ramWitnessPolicy))return false;
    if(worker_->framePipeline->cpu0Vrc7Enabled()){
        worker_->frameFm=c6_async::createFrameFmBridgePsram<FrameVideoPipeline>();
        if(!worker_->frameFm)return false; // Core/context удерживаются для actual joined teardown.
        std::printf("NES_FRAME_VRC7_MODE enabled=1 events_per_slot=16384 event_bytes=8 slots=2 derived_psram=1 wire_unchanged=1 dsp_includes_fm_pump_wall=1\n");
    }

    ESP_LOGI(tag,"NES_OUTPUT_FRAME_PIPELINE_MODE schema=2 mapper=%u producer=timing_fetch consumer=output_compositor read_witness=unavailable slots=2 rgb=%s borrowed=1 copy=0 early_dsp=1 effective_tail=off game_fps_claim=0",unsigned(current_.mapper),esp_ptr_external_ram(borrowedRgb)?"board_psram_direct":"board_internal_direct");
    if(!configureC6AudioBackend())return false;
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
    if(!releaseC6AudioForLoad()||!worker_->frameC6Audio.empty())return false;
    std::printf("NES_FRAME_C6_FINAL_DRAIN schema=1 game=%u frames=%llu produced_frames=%llu submitted_frames=%llu generation=%llu in_flight=%u pending_depth=0 audio_source_held=0 actual_join_checked=1 c6_unbound=1\n",
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(worker_->framePipeline->producedFrames()),
        static_cast<unsigned long long>(submittedFrames_),static_cast<unsigned long long>(sessionGeneration_),unsigned(worker_->inFlight));
    if(worker_->frameFm){
        if(!c6_async::destroyFrameFmBridgeJoined(worker_->frameFm,worker_->frameC6Audio))return false;
        worker_->frameFm=nullptr;
    }
    if(!FrameVideoPipeline::destroyJoined(worker_->framePipeline))return false;
    worker_->framePipeline=nullptr;return true;
}
/**
 * @brief Получает настоящее завершение производителя и потребляет предыдущий Frame-slot. Ошибки lease/sequence/cut переводят тракт в отказ.
 */
bool NesApp::retireFramePipelineFrame()noexcept {
    auto&w=*worker_;auto&pipeline=*w.framePipeline;const auto begin=esp_timer_get_time();
    // Пока Worker N занят, Main продвигает только прежние immutable audio и native-вывод.
    // Ни готовность PCM, ни завершение PPA не заменяют настоящий Worker done.
    const char* pumpFault="frame_c6_wait_pump";
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
    const bool directPoll=frameDirectPresent.enabled&&framePipelinePresentMatches(frames_)&&board_.supportsDirectGamePresentPoll();
    unsigned pumpCalls=0;
#endif
    auto done=frame_done::WaitResult::Ready;
    if(w.c6Audio)done=frame_done::waitDone(begin,
        [&]()noexcept{return xSemaphoreTake(w.done,0)==pdTRUE;},
        [&]()noexcept{
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
            // Сначала настоящий done. Между опросами PPA/LCD оставляем до128 вызовов C6.
            if(directPoll&&w.pipelinePresentPending&&((pumpCalls++&127U)==0U)&&!finishFramePipelinePresent(true)){
                pumpFault="frame_pipeline_direct_poll";return false;
            }
#endif
            return pumpC6AudioBackend(false);
        },
        []()noexcept{return esp_timer_get_time();},
        []()noexcept{vTaskDelay(1);});
    else if(xSemaphoreTake(w.done,pdMS_TO_TICKS(1500))!=pdTRUE)done=frame_done::WaitResult::Deadline;
    if(done!=frame_done::WaitResult::Ready){
        if(done==frame_done::WaitResult::Deadline){pipeline.stop(FrameVideoPipeline::Error::Deadline);workerFault("frame_pipeline_timeout");}
        else if(executionMode_!=ExecutionMode::Faulted)workerFault(pumpFault);
        return false; // Ни timeout, ни pump fault не дают joined job либо разрешение free/reset.
    }
    w.inFlight=false;
    inputUs_+=w.inputUs; // Только joined job N; dispatch N+1 ещё не перезаписал поле.
    const bool joined=pipeline.joinedProducer(); // Настоящий done подтверждается и для неуспешного задания.
    const auto workerWaitUs=esp_timer_get_time()-begin;
    workerWaitUs_+=workerWaitUs;
    if(!joined||w.result!=core::RunStatus::FrameReady||w.generation!=sessionGeneration_||
       w.inputFrame+1!=pipeline.producedFrames()||!pipeline.matches(activeRom_,activeRomBytes_,
       current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)){
        workerFault("frame_pipeline_producer");return false;
    }
    std::int64_t sourceWaitUs=0;
    if(w.c6Audio){
        const auto sourceBegin=esp_timer_get_time();
        const auto deadline=sourceBegin+1500000;
        auto sourceTime=sourceBegin;
        // Повторно используем метку дедлайна из каждого pump без нового опроса часов.
        while(!w.frameC6Audio.empty()||w.c6Published.held()){
            if(!pumpC6AudioBackend(false)){workerFault("frame_c6_previous_source");return false;}
            sourceTime=esp_timer_get_time();
            if(sourceTime>=deadline){workerFault("frame_c6_previous_source");return false;}
        }
        sourceWaitUs=sourceTime-sourceBegin;
        if(w.frameFm){
            // Только pin rawN. FM-исполнение будет в consume ПОСЛЕ dispatchN+1.
            const auto result=w.frameFm->reserveJoined(w.frameC6Audio,pipeline,w.c6Published,w.c6Audio->binding(),w.inputFrame,true,true);
            if(result!=c6_async::FrameFmBridge<FrameVideoPipeline>::Result::Ready){workerFault("frame_fm_reserve");return false;}
        }else{
            const auto result=w.frameC6Audio.publishJoined(pipeline,w.c6Published,w.c6Audio->session(),w.c6Audio->binding(),w.inputFrame,true,true);
            if(result!=c6_async::PostJoinFrameAudio<FrameVideoPipeline>::Result::Ready){workerFault("frame_c6_postjoin");return false;}
            if(!pumpC6AudioBackend(false))return false; // Прежний non85 порядок не меняется.
        }
    }
    frameLoopMetrics.joined(this,w.inputFrame,w.inputUs,workerWaitUs,sourceWaitUs);
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
        return pipeline.error()==FrameVideoPipeline::Error::None;
    }
    const auto valid=/** @brief Повторно проверяет текущие Main generation/ROM/region и режим Worker до использования заимствованного цветного контекста. */
                     [&]()noexcept{return executionMode_==ExecutionMode::Worker&&pipeline.matches(activeRom_,activeRomBytes_,
        current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_);};
    std::int64_t fmWall=0;
    if(w.frameFm){
        if(!valid()){workerFault("frame_fm_session");return false;}
        const auto start=esp_timer_get_time();const auto deadline=pipeline.pendingDeadlineUs();
        if(!deadline){workerFault("frame_fm_deadline");return false;}
        while(w.frameFm->building()){
            if(esp_timer_get_time()>=deadline){workerFault("frame_fm_deadline");return false;}
            const auto result=w.frameFm->buildStep(w.frameC6Audio,[&]()noexcept{return esp_timer_get_time()<deadline&&pumpC6AudioBackend(false);},512,128);
            if(result==c6_async::FrameFmBridge<FrameVideoPipeline>::Result::Fault){workerFault("frame_fm_build");return false;}
        }
        if(!valid()||w.frameFm->publishReady(w.frameC6Audio,w.c6Published,w.c6Audio->session(),w.c6Audio->binding())!=
           c6_async::FrameFmBridge<FrameVideoPipeline>::Result::Ready){workerFault("frame_fm_publish");return false;}
        if(!pumpC6AudioBackend(false))return false;
        fmWall=esp_timer_get_time()-start; // Включает cooperative pump: это wall, не pure FM cycles.
    }
    if(!valid()||!pipeline.prepareDsp(*w.audioRenderer)){workerFault("frame_pipeline_prepare_dsp");return false;}
    /** @brief RAII-защита подготовленного DSP/цветного потребления; аварийный выход снимает заимствование до fault-cleanup. */
    struct PreparedGuard {
        FrameVideoPipeline*active;
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
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
    std::uint16_t* destination=nullptr;
    if(frameDirectPresent.enabled&&publish&&w.c6Audio&&w.pipelinePresentPending&&board_.supportsDirectGamePresentPoll()){
        if(!framePipelinePresentMatches(frames_))return fail("frame_pipeline_direct_lease");
        const auto begin=esp_timer_get_time();
        const auto source=board_.acquireCompletedGameSource(destination);
        presentUs_+=esp_timer_get_time()-begin;
        if(source!=platform::GameSourceAcquireResult::Ready||!destination||destination!=pipeline.pixels())
            return fail("frame_pipeline_direct_ppa_source");
        ++frameDirectPresent.sourceReused;
    }else{
#endif
    // DSP(N) использовал только неизменяемые пакеты, пока PPA(N-1) держала источник.
    // До настоящего завершения source-drain запрещены getter/запись Ppu/RGB.
    const bool previous=w.pipelinePresentPending;
    if(!finishFramePipelinePresent())return fail("frame_pipeline_direct_previous_ppa");
    if(!previous){
        const auto start=esp_timer_get_time();const bool finished=board_.finishGamePresent();presentUs_+=esp_timer_get_time()-start;
        if(!finished)return fail("frame_pipeline_direct_previous_ppa");
    }
#if defined(NES_EXPERIMENTAL_FRAME_DIRECT_POLL)
        destination=board_.gameFramebuffer();
    }
#else
    auto*const destination=board_.gameFramebuffer();
#endif
    const auto*const accepted=pipeline.pixels();
    if(!destination||(accepted&&accepted!=destination))return fail("frame_pipeline_direct_owned");
#if NES_EXPERIMENTAL_FRAME_COLOUR_PUMP
    // Контекст живёт только до синхронного consumeColour; callback не сохраняется.
    struct ColourPumpContext {
        NesApp*app;FrameVideoPipeline*pipeline;const std::uint8_t*rom;std::size_t bytes;
        core::Region region;std::uint64_t generation;const char*reason;std::int64_t deadline;
    };
    ColourPumpContext colourPump{this,&pipeline,activeRom_,activeRomBytes_,
        current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_,nullptr,pipeline.pendingDeadlineUs()};
    const auto progress=+[](void*raw)noexcept{
        auto&c=*static_cast<ColourPumpContext*>(raw);
        const auto same=[&]()noexcept{return c.app->executionMode_==ExecutionMode::Worker&&c.app->worker_&&
            c.app->worker_->framePipeline==c.pipeline&&c.app->sessionGeneration_==c.generation&&
            c.pipeline->matches(c.rom,c.bytes,c.region,c.generation);};
        if(!same()){c.reason="frame_colour_session";return false;}
        if(!c.deadline||esp_timer_get_time()>=c.deadline){c.reason="frame_colour_deadline";return false;}
        if(!c.app->tryPumpC6AudioBackend(false,c.reason))return false;
        if(!same()){c.reason="frame_colour_session";return false;}
        return true;
    };
    if(!valid()||!pipeline.consumeColour(*w.audioRenderer,w.c6Audio?progress:nullptr,&colourPump)||!valid()||!pipeline.accept(sessionGeneration_))
        return fail(colourPump.reason?colourPump.reason:"frame_pipeline_consumer");
#else
    if(!valid()||!pipeline.consumeColour(*w.audioRenderer)||!valid()||!pipeline.accept(sessionGeneration_))return fail("frame_pipeline_consumer");
#endif
    prepared.disarm(); // Фаза Idle; последующий вложенный fault-cleanup может освободить контекст.
    const auto&v=pipeline.snapshot();const auto&t=pipeline.times();auto&s=*w.snapshot;
    if(v.frame!=frames_+1||v.inputFrame+1!=v.frame||v.appGeneration!=sessionGeneration_||v.sampleCount>core::Apu::sampleCapacity)return fail("frame_pipeline_accept");
    s.ready=false;s.generation=sessionGeneration_;s.completedFrames=v.frame;s.region=v.region;
    s.inputPad=v.pad;s.ppuFrame=v.frame;s.cpuCycles=v.cycle;s.audioGeneration=v.audioGeneration;s.audioSequence=v.audioSequence;
    if(w.c6Audio)s.sampleCount=0;
    else
    {s.sampleCount=v.sampleCount;std::memcpy(s.pcm,w.audioRenderer->samples(),v.sampleCount*sizeof(s.pcm[0]));}
    ++frames_;++periodFrames_;advanceFrameDeadline();coreUs_+=t.core;maxCoreUs_=std::max(maxCoreUs_,std::int64_t(t.core));
    w.pipelineTimes.core+=t.core;w.pipelineTimes.producer+=t.producer;w.pipelineTimes.seal+=t.seal;
    w.pipelineTimes.audioRetire+=t.audioRetire;w.pipelineTimes.snapshot+=t.snapshot;
    w.pipelineTimes.replay+=t.replay;w.pipelineTimes.dsp+=t.dsp+fmWall;w.pipelineTimes.witness+=t.witness;
    if(!publish)return true;
    return presentFramePipelineAsync();
}
/**
 * @brief Печатает принятые CPU/RAM/PCM/RGB/read/input-срезы и раздельные produced/accepted/submitted. Не объявляет физический VSYNC или качество всей игры.
 */
void NesApp::frameVideoStatus()noexcept {
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
    if(executionMode_==ExecutionMode::Faulted||!pipeline.matches(activeRom_,activeRomBytes_,current_.pal?core::Region::Pal:core::Region::Ntsc,sessionGeneration_)){
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
void NesApp::reportFrameVideo()noexcept {
    if(!worker_||!worker_->framePipeline||!playing_||periodFrames_<120)return;
    if(!finishFramePipelinePresent()){workerFault("frame_pipeline_report_ppa");return;}
    const auto n=periodFrames_;auto&w=*worker_;const auto&t=w.pipelineTimes;
    const auto reportDeviceUs=esp_timer_get_time();
    lastAverageCoreUs_=static_cast<std::int64_t>(t.core/n);
#define NES_FRAME_PIPELINE_TIMES_FORMAT "NES_OUTPUT_FRAME_PIPELINE_TIMES game=%u frames=%llu produced_frames=%llu submitted_frames=%llu samples=%llu generation=%llu mapper=%u region=%s core_us=%llu producer_wall_us=%llu seal_us=%llu audio_retire_us=%llu snapshot_us=%llu consumer_active_us=%llu dsp_us=%llu witness_copy_us=%llu rgb_copy_us=%llu present_us=%lld wait_us=%lld audio_us=%lld input_us=%lld device_us=%lld slots=2 schema=2 producer=timing_fetch consumer=output_compositor read_witness=unavailable effective_tail=off game_fps_claim=0"
    ESP_LOGI(tag,NES_FRAME_PIPELINE_TIMES_FORMAT,
        unsigned(selected_),static_cast<unsigned long long>(frames_),static_cast<unsigned long long>(w.framePipeline->producedFrames()),static_cast<unsigned long long>(submittedFrames_),
        static_cast<unsigned long long>(n),static_cast<unsigned long long>(sessionGeneration_),unsigned(current_.mapper),current_.pal?"PAL":"NTSC",
        static_cast<unsigned long long>(t.core/n),static_cast<unsigned long long>(t.producer/n),static_cast<unsigned long long>(t.seal/n),static_cast<unsigned long long>(t.audioRetire/n),
        static_cast<unsigned long long>(t.snapshot/n),static_cast<unsigned long long>(t.replay/n),static_cast<unsigned long long>(t.dsp/n),static_cast<unsigned long long>(t.witness/n),
        static_cast<unsigned long long>(w.pipelineCopyUs/n),static_cast<long long>(presentUs_/n),static_cast<long long>(workerWaitUs_/n),static_cast<long long>(audioUs_/n),static_cast<long long>(inputUs_/n),static_cast<long long>(reportDeviceUs));
#undef NES_FRAME_PIPELINE_TIMES_FORMAT
    w.framePipeline->reportHartStats(unsigned(selected_)); // Только Main-owned accepted окно, не producer N+1.
    reportC6AudioWindow(n);
    w.c6PumpCost.reset();
    w.pipelineTimes={};w.pipelineCopyUs=0;periodFrames_=0;coreUs_=maxCoreUs_=workerWaitUs_=presentUs_=audioUs_=inputUs_=0;periodStart_=esp_timer_get_time();
}

