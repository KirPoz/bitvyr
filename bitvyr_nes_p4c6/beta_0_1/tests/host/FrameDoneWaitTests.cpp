/** @file FrameDoneWaitTests.cpp
 * @brief Исполняет настоящий SDK-free Frame wait с моделью actual semaphore и C6 progress.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace nes::app {
#include "../../app/nes/runtime/app/FrameDoneWait.inl"
}
namespace {
using nes::app::frame_done::WaitResult;
/** @brief Фатальная проверка ownership/порядка без SDK, ROM и физического времени. */
void check(bool value,const char* message,unsigned line){
    if(!value){std::fprintf(stderr,"FrameDoneWait line%u: %s\n",line,message);std::exit(1);}
}
#define CHECK(value) check(bool(value),#value,__LINE__)
/** @brief Main callbacks: token существует независимо от PCM-ready и pump success. */
struct Fixture {
    std::int64_t time=0,step=1000,doneAt=-1;
    unsigned probes=0,pumps=0,idles=0,tokens=0,failAt=0,rpcCompleted=0;
    bool semaphore=false,tokenOnFault=false,tokenOnIdle=false,inFlight=true,ownerHeld=true;
    /** @brief Только настоящий token разрешает успешный возврат; pending не читает job. */
    bool tryDone()noexcept{
        ++probes;
        if(doneAt>=0&&time>=doneAt)semaphore=true;
        if(!semaphore)return false;
        semaphore=false;++tokens;return true;
    }
    /** @brief Каждый вызов продвигает прежний immutable audio; первые8 моделируют8RPC. */
    bool pump()noexcept{
        CHECK(inFlight&&ownerHeld);++pumps;time+=step;
        if(rpcCompleted<8)++rpcCompleted;
        if(failAt&&pumps==failAt){if(tokenOnFault)semaphore=true;return false;}
        return true;
    }
    /** @brief Реальный blocking idle opportunity добавляет один tick только при запросе helper. */
    void idle()noexcept{++idles;time+=1000;if(tokenOnIdle)semaphore=true;}
    /** @brief Вызывает непосредственно production helper; сам не присваивает join либо free. */
    WaitResult run()noexcept{
        return nes::app::frame_done::waitDone(time,
            [&]()noexcept{return tryDone();},[&]()noexcept{return pump();},
            [&]()noexcept{return time;},[&]()noexcept{idle();});
    }
};
/** @brief Уже опубликованный done не вызывает лишних RPC либо tick-delay. */
void immediateDone(){
    Fixture f;f.semaphore=true;
    CHECK(f.run()==WaitResult::Ready&&f.tokens==1&&f.pumps==0&&f.idles==0);
    CHECK(f.inFlight&&f.ownerHeld); // Только внешний checked join меняет владение.
}
/** @brief Все8RPC идут во время реального CPU1 job до30мс; на каждом RPC нет1мс delay. */
void pendingThenActualDone(){
    Fixture f;f.doneAt=30000;
    CHECK(f.run()==WaitResult::Ready&&f.tokens==1&&f.pumps==30&&f.rpcCompleted==8);
    CHECK(f.idles==0&&f.time==30000&&f.probes==31);
    CHECK(f.inFlight&&f.ownerHeld);
}
/** @brief Fault с одновременно прибывшим token не превращается в Ready и не съедает его. */
void pumpFaultRetainsLateDone(){
    Fixture f;f.failAt=3;f.tokenOnFault=true;
    CHECK(f.run()==WaitResult::PumpFault&&f.tokens==0&&f.pumps==3);
    CHECK(f.semaphore&&f.inFlight&&f.ownerHeld&&f.idles==0);
    CHECK(f.tryDone()&&f.tokens==1); // Отдельный поздний quarantine join возможен лишь с actual token.
}
/** @brief Непрерывный pending ограничен1500мс, включая idle; активность не продлевает deadline. */
void deadlineRetainsOwners(){
    Fixture f;
    CHECK(f.run()==WaitResult::Deadline&&f.time==1500000);
    CHECK(f.tokens==0&&f.inFlight&&f.ownerHeld&&f.idles>0);
    CHECK(f.idles<50&&f.pumps>1400); // Coarse32мс idle, а не sleep на каждый pump.
}
/** @brief После coarse idle настоящий done проверяется раньше нового pump. */
void idleThenActualDone(){
    Fixture f;f.step=17000;f.tokenOnIdle=true;
    CHECK(f.run()==WaitResult::Ready&&f.tokens==1&&f.pumps==2&&f.idles==1);
    CHECK(f.time==35000&&f.inFlight&&f.ownerHeld);
}
}
/** @brief Проверки используют тот же private fragment, который включён в реальный FrameVideo. */
int main(){
    immediateDone();pendingThenActualDone();pumpFaultRetainsLateDone();
    deadlineRetainsOwners();idleThenActualDone();
    std::puts("Frame wait actualDone/fault/deadline/idle PASS 5 cases");
}
