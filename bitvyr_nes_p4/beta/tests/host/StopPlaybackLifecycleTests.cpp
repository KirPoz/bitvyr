/** @file StopPlaybackLifecycleTests.cpp
 * @brief Исполняет настоящий runtime stopPlayback с SDK-free owner callbacks P4.
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
/** @brief Отказавший Frame остаётся живым и останавливается без destroy. */
struct Pipeline{unsigned stops=0;void stop()noexcept{++stops;}};
/** @brief Main-модель P4: неизвестный Worker и Frame остаются до подтверждённого release. */
struct Worker{
    bool inFlight=false,legacyMachine=false;
    Pipeline* framePipeline=nullptr;
};
/** @brief Board mute не подтверждает PPA/LCD/Worker completion. */
struct Board{bool active=true;void setAudioActive(bool value)noexcept{active=value;}};
/** @brief API callbacks моделируют только контракт внешних владельцев, stop берётся из продукта. */
class NesApp{
public:
    Worker state{};Pipeline frame{};Board board_{};
    Worker* worker_=&state;
    ExecutionMode executionMode_=ExecutionMode::Worker;
    Screen screen_=Screen::Game;PauseAction pauseAction_=PauseAction::Exit;
    bool playing_=true,pausedGame_=false,menuDirty_=true,settingsReturnToPause_=true;
    std::uint8_t chromeButtons_=3;std::uint64_t sessionGeneration_=11,frames_=2420,submittedFrames_=2420;
    unsigned selected_=4;bool serialCleared=false;
    bool joinReady=true,framePresentReady=true,frameReleaseReady=true;
    bool requireNoVideoAtCommit=true;
    unsigned frameReleaseCalls=0,invalidateCalls=0;
    std::vector<std::string> calls;
    /** @brief Вызывает реальное определение P4 из StopPlayback.inl. */
    void stopPlayback()noexcept;
    /** @brief Успех означает настоящий done; false сохраняет inFlight и контекст. */
    bool retireFrame()noexcept{
        calls.push_back("join");
        if(!joinReady||executionMode_==ExecutionMode::Faulted){executionMode_=ExecutionMode::Faulted;return false;}
        if(worker_)worker_->inFlight=false;
        return true;
    }
    /** @brief Регистрирует ожидание вывода. Флаг имитирует подтверждение читателя видеобуфера. */
    bool finishFramePipelinePresent()noexcept{calls.push_back("frame-present");return framePresentReady;}
    /** @brief Модель checked Frame release: отказ удерживает контекст, только успех снимает указатель. */
    bool releaseFramePipelineForLoad()noexcept{
        ++frameReleaseCalls;calls.push_back("frame-release");
        if(!frameReleaseReady||(worker_&&worker_->inFlight))return false;
        if(worker_)worker_->framePipeline=nullptr;
        calls.push_back("frame-destroyed");return true;
    }
    /** @brief Запоминает отказ и фиксирует переход в Faulted для следующих вызовов. */
    void workerFault(const char* reason)noexcept{calls.push_back(reason);executionMode_=ExecutionMode::Faulted;}
    /** @brief Отмечает очистку ввода в журнале порядка после завершения сеанса. */
    void clearSerialInput()noexcept{calls.push_back("input-clear");serialCleared=true;}
    /** @brief Любая публикация нового generation требует уже завершённого владельца. */
    void invalidateSnapshot()noexcept{
        calls.push_back("invalidate");++invalidateCalls;
        CHECK(!worker_||!worker_->inFlight);
        if(requireNoVideoAtCommit)CHECK(!worker_||!worker_->framePipeline);
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
/** @brief Frame stop P4 завершает checked release до публикации Library/generation. */
void frameSuccess(){
    NesApp app;app.state.framePipeline=&app.frame;
    app.stopPlayback();
    CHECK(!app.state.framePipeline&&app.frameReleaseCalls==1);
    CHECK(app.screen_==Screen::Library&&app.sessionGeneration_==12);
    CHECK(!app.playing_&&!app.pausedGame_&&app.menuDirty_&&!app.board_.active&&app.serialCleared);
    CHECK(at(app,"frame-release")<at(app,"invalidate"));
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
/** @brief Повторная Library-команда не освобождает уже возвращённый Frame повторно. */
void idleStop(){
    NesApp app;app.playing_=false;app.screen_=Screen::Library;
    app.stopPlayback();app.stopPlayback();
    CHECK(app.frameReleaseCalls==0&&app.sessionGeneration_==13);
}
}
/** @brief Все случаи исполняют product method; никаких UART/SDK/Board реализаций в fixture нет. */
int main(){
    fixture::frameSuccess();fixture::frameFailures();fixture::legacyJoinFailure();fixture::idleStop();
    std::puts("Actual P4 stopPlayback lifecycle PASS");
}

