#include "board/GamePresentPoll.h"
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks=0;
/** @brief Проверяет фактический portable control-path и печатает нарушенный контракт. */
void require(bool value,const char*meaning){
    ++checks;
    if(!value){std::fprintf(stderr,"FAIL %s check=%u\n",meaning,checks);std::exit(1);}
}
/** @brief Переставляет callback-готовности и проверяет single-consume/submit реального автомата. */
void exercise(unsigned ppaAt,unsigned lcdAt,bool hadPriorLcd){
    bool ppaReady=false,lcdPending=hadPriorLcd,gamePending=true,ppaToken=false,lcdToken=false;
    unsigned ppaTaken=0,lcdTaken=0,submits=0,ppaTries=0,lcdTries=0;
    for(unsigned tick=0;tick<8;++tick){
        if(tick==ppaAt)ppaToken=true;
        if(tick==lcdAt)lcdToken=true;
        const auto result=nes::platform::detail::pollGamePresentation(ppaReady,lcdPending,gamePending,
            /** @brief Пробует однократно забрать PPA completion: считает попытку и расходует готовый token. Возвращает false до готовности; захваченные счётчики и token заимствованы из exercise. */ [&]()noexcept{++ppaTries;if(!ppaToken)return false;ppaToken=false;++ppaTaken;return true;},
            /** @brief Пробует забрать completion прежней LCD передачи только после PPA-ready. Расходует готовый LCD token один раз; возвращает false при его отсутствии. */ [&]()noexcept{++lcdTries;require(ppaReady,"LCD check after PPA");
                if(!lcdToken)return false;
                lcdToken=false;++lcdTaken;return true;},
            /** @brief Имитирует принятие нового LCD draw после обеих границ готовности. Проверяет единственную отправку, увеличивает submits и переводит LCD в pending; возвращает true. */ [&]()noexcept{require(ppaReady&&!lcdPending,"submit after both fences");
                require(!submits,"one submission");++submits;lcdPending=true;return true;});
        if(result==nes::platform::GamePresentPollResult::Submitted){
            require(tick>=ppaAt&&(!hadPriorLcd||tick>=lcdAt),"not before callback");
            require(!gamePending&&!ppaReady&&lcdPending,"LCD and source owners distinct");
            const auto oldPpa=ppaTries,oldLcd=lcdTries;
            const auto repeat=nes::platform::detail::pollGamePresentation(ppaReady,lcdPending,gamePending,
                /** @brief Считает неожиданный повторный запрос PPA после принятого кадра. Возвращает true, но stale poll обязан отвергнуться до вызова callback. */ [&]()noexcept{++ppaTries;return true;},/** @brief Считает неожиданный повторный запрос LCD после принятого кадра. Возвращает true; неизменность lcdTries доказывает ранний отказ stale poll. */ [&]()noexcept{++lcdTries;return true;},
                /** @brief Считает недопустимую повторную отправку уже принятого кадра. Возвращает true; callback оставлен наблюдателем для проверки stale poll. */ [&]()noexcept{++submits;return true;});
            require(repeat==nes::platform::GamePresentPollResult::Fault,"stale poll rejected");
            require(ppaTries==oldPpa&&lcdTries==oldLcd,"stale poll takes no unrelated token");
            break;
        }
        require(result==nes::platform::GamePresentPollResult::Pending,"ordinary wait is Pending");
        require(gamePending&&submits==0,"Pending retains output/source loan");
        require(ppaTaken<=1&&lcdTaken<=1,"consume each token once");
        if(ppaReady)require(ppaTaken==1,"latch requires token");
    }
    require(submits==1&&ppaTaken==1&&lcdTaken==unsigned(hadPriorLcd),"eventual exact submission");
}
/** @brief Failed draw сохраняет PPA-ready output; явный retry не ждёт второго PPA completion. */
void rejectedSubmission(){
    bool ppaReady=false,lcdPending=true,gamePending=true;unsigned ppa=0,lcd=0,submits=0;
    auto takePpa=/** @brief Выдаёт единственный PPA completion для сценария отклонённого draw; считает вызовы, чтобы retry использовал сохранённую готовность. */ [&]()noexcept{++ppa;return true;};auto takeLcd=/** @brief Выдаёт completion прежней LCD передачи и считает вызовы; retry не должен забирать второе завершение. */ [&]()noexcept{++lcd;return true;};
    const auto fault=nes::platform::detail::pollGamePresentation(ppaReady,lcdPending,gamePending,takePpa,takeLcd,
        /** @brief Имитирует отказ LCD submit: увеличивает число попыток и возвращает false, сохраняя source/output loan для явного retry. */ [&]()noexcept{++submits;return false;});
    require(fault==nes::platform::GamePresentPollResult::Fault&&ppaReady&&gamePending&&!lcdPending,
        "fault retains source/output and PPA latch");
    const auto retry=nes::platform::detail::pollGamePresentation(ppaReady,lcdPending,gamePending,takePpa,takeLcd,
        /** @brief Имитирует успешный retry LCD submit: увеличивает число попыток, помечает LCD pending и возвращает true без повторного PPA completion. */ [&]()noexcept{++submits;lcdPending=true;return true;});
    require(retry==nes::platform::GamePresentPollResult::Submitted&&ppa==1&&lcd==1&&submits==2,
        "retry preserves consumed fences");
}
}
/** @brief Проверяет перестановки completion; не заменяет SDK/IRQ/cache/физический LCD тест. */
int main(){
    for(unsigned ppa=0;ppa<5;++ppa)for(unsigned lcd=0;lcd<5;++lcd){exercise(ppa,lcd,false);exercise(ppa,lcd,true);}
    rejectedSubmission();std::printf("PASS game-present-poll schedules=50 checks=%u\n",checks);
}
