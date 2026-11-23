/** @file StopPlaybackLifecycleTests.cpp
 * @brief Исполняет настоящий runtime stopPlayback с SDK-free owner callbacks P4+C6.
 * @details Проверяет границу Library/generation, а не эмулирует новый stop алгоритм.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace fixture {
/** @brief Завершает fixture на нарушении ownership либо порядка вызовов. */
void require(bool value,const char* expression,unsigned line){
    if(!value){std::fprintf(stderr,"STOP lifecycle line%u: %s\n",line,expression);std::exit(1);}
}
#define CHECK(x) ::fixture::require(bool(x),#x,__LINE__)
enum class ExecutionMode{Worker,Faulted};
enum class Screen{Game,Pause,Library};
enum class PauseAction{None,Exit};
/** @brief Фиктивный immutable source; false появляется лишь после успешного present drain. */
struct Source{bool held=true;bool valid()const noexcept{return held;}};
/** @brief Отказавший Frame остаётся живым и останавливается без destroy. */
struct Pipeline{unsigned stops=0;void stop()noexcept{++stops;}};
/** @brief Main-only diagnostic counter не является Worker join. */
struct Idle{unsigned resets=0;void reset()noexcept{++resets;}};
/** @brief Boot-long C6 owner может остаться allocated, но bound снимается только release API. */
struct Audio{bool bound=true,drained=false;};
struct Worker{
    bool inFlight=false,legacyMachine=false,presentPending=true;
    Pipeline* framePipeline=nullptr;Pipeline* coupled=nullptr;Audio* c6Audio=nullptr;
    Idle coupledMainIdle;Source presentSource;
};
/** @brief Board mute не подтверждает PPA/LCD/Worker completion. */
struct Board{bool active=true;void setAudioActive(bool value)noexcept{active=value;}};
/** @brief API callbacks моделируют только контракт внешних владельцев, stop берётся из продукта. */
class NesApp{
public:
    Worker state{};Pipeline frame{},coupled{};Audio audio{};Board board_{};
    Worker* worker_=&state;
    ExecutionMode executionMode_=ExecutionMode::Worker;
    Screen screen_=Screen::Game;PauseAction pauseAction_=PauseAction::Exit;
    bool playing_=true,pausedGame_=false,menuDirty_=true,settingsReturnToPause_=true;
    std::uint8_t chromeButtons_=3;std::uint64_t sessionGeneration_=11,frames_=2420,submittedFrames_=2420;
    unsigned selected_=4;bool serialCleared=false;
    bool joinReady=true,audioReady=true,audioDrainReady=true,framePresentReady=true;
    bool frameReleaseReady=true,coupledPresentReady=true,coupledReleaseReady=true;
    bool failCoupledDestroyAfterUnbind=false;
    bool requireNoVideoAtCommit=true;
    unsigned frameReleaseCalls=0,coupledReleaseCalls=0,invalidateCalls=0;
    std::vector<std::string> calls;
    /** @brief Вызывает реальное определение из StopPlayback.inl соответствующей beta. */
    void stopPlayback()noexcept;
    /** @brief Успех означает настоящий done; false сохраняет inFlight и контекст. */
    bool retireFrame()noexcept{
        calls.push_back("join");
        if(!joinReady||executionMode_==ExecutionMode::Faulted){executionMode_=ExecutionMode::Faulted;return false;}
        if(worker_)worker_->inFlight=false;
        return true;
    }
    /** @brief Регистрирует передачу последнего снимка в аудиотракт и возвращает заданный тестом результат. */
    bool prepareSnapshotAudio()noexcept{calls.push_back("audio");return audioReady;}
    /** @brief Drain сам по себе не снимает C6 binding либо video context. */
    bool drainC6AudioBackend()noexcept{
        calls.push_back("audio-drain");
        if(!audioDrainReady||(worker_&&worker_->inFlight))return false;
        if(worker_&&worker_->c6Audio)worker_->c6Audio->drained=true;
        return true;
    }
    /** @brief Регистрирует ожидание вывода. Флаг имитирует подтверждение читателя видеобуфера. */
    bool finishFramePipelinePresent()noexcept{calls.push_back("frame-present");return framePresentReady;}
    /** @brief Black-box существующего checked Frame release; отказ ничего не освобождает. */
    bool releaseFramePipelineForLoad()noexcept{
        ++frameReleaseCalls;calls.push_back("frame-release");
        if(!frameReleaseReady||(worker_&&worker_->inFlight))return false;
        if(worker_){worker_->framePipeline=nullptr;if(worker_->c6Audio)worker_->c6Audio->bound=false;}
        calls.push_back("frame-destroyed");return true;
    }
    /** @brief Только успех означает оба Board readers завершены и source возвращён. */
    bool finishCoupledPresent()noexcept{
        calls.push_back("coupled-present");
        if(!coupledPresentReady)return false;
        if(worker_){worker_->presentPending=false;worker_->presentSource.held=false;}return true;
    }
    /** @brief Existing API postcondition: joined+drained+source free+UNBOUND before context null. */
    bool releaseCoupledForLoad()noexcept{
        ++coupledReleaseCalls;calls.push_back("coupled-release");
        if(!coupledReleaseReady||!worker_||worker_->inFlight||
           worker_->presentPending||worker_->presentSource.valid()||
           (worker_->c6Audio&&!worker_->c6Audio->drained))return false;
        calls.push_back("UNBOUND");
        if(worker_->c6Audio)worker_->c6Audio->bound=false;
        if(failCoupledDestroyAfterUnbind)return false;
        worker_->coupled=nullptr;calls.push_back("coupled-destroyed");return true;
    }
    /** @brief Запоминает отказ и фиксирует переход в Faulted для следующих вызовов. */
    void workerFault(const char* reason)noexcept{calls.push_back(reason);executionMode_=ExecutionMode::Faulted;}
    /** @brief Отмечает очистку ввода в журнале порядка после завершения сеанса. */
    void clearSerialInput()noexcept{calls.push_back("input-clear");serialCleared=true;}
    /** @brief Любая публикация нового generation требует уже завершённого владельца. */
    void invalidateSnapshot()noexcept{
        calls.push_back("invalidate");++invalidateCalls;
        CHECK(!worker_||!worker_->inFlight);
        if(requireNoVideoAtCommit)CHECK(!worker_||(!worker_->coupled&&!worker_->framePipeline));
        CHECK(screen_==Screen::Library);++sessionGeneration_;
    }
};
#define NES_LOOP_TAINT() calls.push_back("taint");
#include "../../app/nes/runtime/app/StopPlayback.inl"
#undef NES_LOOP_TAINT
/** @brief Возвращает порядок callback либо завершает случай при отсутствующем вызове. */
std::size_t at(const NesApp&app,const char* value){
    const auto it=std::find(app.calls.begin(),app.calls.end(),value);
    CHECK(it!=app.calls.end());return static_cast<std::size_t>(it-app.calls.begin());
}
/** @brief Общая quarantine сохраняет ROM/context и не публикует Library либо generation. */
void retained(const NesApp&app){
    CHECK(!app.playing_&&!app.pausedGame_&&!app.board_.active&&!app.menuDirty_);
    CHECK(app.screen_==Screen::Game&&app.sessionGeneration_==11&&app.invalidateCalls==0);
    CHECK(!app.serialCleared);
}
/** @brief Frame stop обоих продуктов завершает existing release до Library. */
void frameSuccess(){
    NesApp app;app.state.framePipeline=&app.frame;
    app.state.c6Audio=&app.audio;app.state.inFlight=true;
    app.stopPlayback();
    CHECK(!app.state.framePipeline&&app.frameReleaseCalls==1);
    CHECK(app.screen_==Screen::Library&&app.sessionGeneration_==12);
    CHECK(!app.playing_&&!app.pausedGame_&&app.menuDirty_&&!app.board_.active&&app.serialCleared);
    CHECK(at(app,"frame-release")<at(app,"invalidate"));
    CHECK(!app.audio.bound&&at(app,"join")<at(app,"frame-release"));
}
/** @brief Неизвестный Frame owner/PPA сохраняет контекст и не маскируется экраном Library. */
void frameFailures(){
    for(bool presentFailure:{false,true}){
        NesApp app;app.state.framePipeline=&app.frame;
        app.framePresentReady=!presentFailure;app.frameReleaseReady=presentFailure;
        app.stopPlayback();retained(app);CHECK(app.state.framePipeline==&app.frame);
        CHECK(app.executionMode_==ExecutionMode::Faulted);
        CHECK(app.frame.stops==(presentFailure?0U:1U));
    }
}
/** @brief Неизвестный Worker сохраняется даже у прежнего legacy stop пути. */
void legacyJoinFailure(){
    NesApp app;app.state.legacyMachine=true;app.state.inFlight=true;app.joinReady=false;
    app.stopPlayback();retained(app);CHECK(app.state.inFlight&&app.state.legacyMachine);
}
/** @brief Повторная Library команда не вызывает повторный backend release или UNBOUND. */
void idleStop(){
    NesApp app;app.playing_=false;app.screen_=Screen::Library;
    app.stopPlayback();app.stopPlayback();
    CHECK(app.frameReleaseCalls==0&&app.coupledReleaseCalls==0&&app.sessionGeneration_==13);
}
/** @brief Реальный happy-path: join/drain/present/release/UNBOUND предшествуют UI commit. */
void coupledSuccess(bool paused,bool remote){
    NesApp app;app.state.coupled=&app.coupled;app.state.inFlight=true;
    if(remote)app.state.c6Audio=&app.audio;
    if(paused){app.playing_=false;app.pausedGame_=true;app.screen_=Screen::Pause;}
    app.stopPlayback();
    CHECK(!app.state.coupled&&app.coupledReleaseCalls==1&&!app.state.inFlight);
    CHECK(app.screen_==Screen::Library&&app.sessionGeneration_==12);
    CHECK(!app.playing_&&!app.pausedGame_&&app.menuDirty_&&!app.board_.active);
    CHECK(at(app,"join")<at(app,"coupled-present"));
    CHECK(at(app,"coupled-present")<at(app,"coupled-release"));
    CHECK(at(app,"UNBOUND")<at(app,"coupled-destroyed"));
    CHECK(at(app,"coupled-destroyed")<at(app,"invalidate"));
    if(remote)CHECK(!app.audio.bound&&app.audio.drained);
    app.stopPlayback();
    CHECK(app.coupledReleaseCalls==1&&app.sessionGeneration_==13);
}
/** @brief Join/drain/PPA/UNBOUND failure не считается completion и не даёт Library. */
void coupledFailures(){
    for(unsigned failure=0;failure<5;++failure){
        NesApp app;app.state.coupled=&app.coupled;app.state.c6Audio=&app.audio;app.state.inFlight=true;
        if(failure==0)app.joinReady=false;
        if(failure==1)app.audioReady=false;
        if(failure==2)app.audioDrainReady=false;
        if(failure==3)app.coupledPresentReady=false;
        if(failure==4)app.coupledReleaseReady=false;
        app.stopPlayback();retained(app);CHECK(app.state.coupled==&app.coupled&&app.audio.bound);
        if(failure<3)CHECK(app.coupledReleaseCalls==0);
        if(failure==0)CHECK(app.state.inFlight);
    }
}
/** @brief Даже после actual UNBOUND последующий RGB/destroy отказ сохраняет context/generation.
 * Boot-long owner может уже быть unbound; это не разрешает публиковать Library. */
void coupledFailureAfterUnbind(){
    NesApp app;app.state.coupled=&app.coupled;app.state.c6Audio=&app.audio;
    app.state.inFlight=true;app.failCoupledDestroyAfterUnbind=true;
    app.stopPlayback();retained(app);
    CHECK(app.state.coupled==&app.coupled&&!app.audio.bound);
    CHECK(app.executionMode_==ExecutionMode::Faulted&&app.coupledReleaseCalls==1);
    CHECK(std::find(app.calls.begin(),app.calls.end(),"coupled-destroyed")==app.calls.end());
}
/** @brief Coupled без remote owner тоже обязан получить actual Worker done. */
void localCoupledJoinFailure(){
    NesApp app;app.state.coupled=&app.coupled;app.state.inFlight=true;app.joinReady=false;
    app.stopPlayback();retained(app);CHECK(app.state.inFlight&&app.coupledReleaseCalls==0);
}
}
/** @brief Все случаи исполняют product method; никаких UART/SDK/Board реализаций в fixture нет. */
int main(){
    fixture::frameSuccess();fixture::frameFailures();fixture::legacyJoinFailure();fixture::idleStop();
    fixture::coupledSuccess(false,true);fixture::coupledSuccess(true,true);
    fixture::coupledSuccess(false,false);fixture::coupledFailures();
    fixture::coupledFailureAfterUnbind();fixture::localCoupledJoinFailure();
    std::puts("Actual C6 stopPlayback lifecycle PASS");
}

