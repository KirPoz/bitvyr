/** @file GameSourceLease.h @brief Ограниченные RGB A/B аренды Main; SDK и память не принадлежат политике. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nes::platform {
/** @brief Полная идентичность immutable NES-источника; поля времени измеряются в гостевых CPU cycles. */
struct GameSourceTicket final {
    const std::uint16_t* pixels=nullptr;
    std::uint64_t generation=0,epoch=0,sequence=0,inputFrame=0,frame=0,beginCycle=0,endCycle=0;
    std::uint8_t slot=0,region=0;
    /** @brief Проверяет ненулевую сессию, область NTSC/PAL и строгую пару input/output кадра без переполнения. */
    bool valid()const noexcept{
        return pixels&&generation&&epoch&&sequence&&slot<2&&region<2&&frame&&
            inputFrame!=std::numeric_limits<std::uint64_t>::max()&&inputFrame+1==frame&&endCycle>beginCycle;
    }
};
/** @brief Сравнивает весь ticket, включая адрес, временной срез и slot; не заменяет DMA fence. */
inline bool sameGameSource(const GameSourceTicket&a,const GameSourceTicket&b)noexcept{
    return a.pixels==b.pixels&&a.generation==b.generation&&a.epoch==b.epoch&&a.sequence==b.sequence&&
        a.inputFrame==b.inputFrame&&a.frame==b.frame&&a.beginCycle==b.beginCycle&&a.endCycle==b.endCycle&&a.slot==b.slot&&a.region==b.region;
}
/** @brief Проверяет фиксированный диапазон RGB565 и его четырёхбайтовое выравнивание без SDK. */
inline bool gameSourceRange(const std::uint16_t*p)noexcept{
    const auto n=reinterpret_cast<std::uintptr_t>(p);
    return n&&!(n&3U)&&n<=std::numeric_limits<std::uintptr_t>::max()-122880U;
}
/** @brief Проверяет два непересекающихся полных источника; владение памятью не передаёт. */
inline bool distinctGameSources(const std::uint16_t*a,const std::uint16_t*b)noexcept{
    if(!gameSourceRange(a)||!gameSourceRange(b))return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    return x+122880U<=y||y+122880U<=x;
}
/**
 * @brief Два статических source-credit без задач/очередей/allocation; изменяется только Main.
 * @details Читатель возвращает credit только по настоящему Board completion с тем же ticket.
 * Timeout не имеет перехода, destructor ничего не освобождает; canDestroy требует внешнего join.
 */
class GameSourcePool final {
public:
    enum class State:std::uint8_t{Free,Writing,Ready,Reserved,Reading};
    /** @brief Однократно регистрирует A/B до писателя; ошибочный вызов не меняет прежний пул. */
    bool configure(std::uint16_t*a,std::uint16_t*b)noexcept{
        if(sources_[0]||!distinctGameSources(a,b))return false;
        sources_[0]=a;sources_[1]=b;return true;
    }
    /** @brief Отдаёт точный свободный slot единственному писателю; Reading/Ready повторно не выдаются. */
    bool beginWrite(unsigned slot)noexcept{
        if(slot>=2||!sources_[slot]||states_[slot]!=State::Free)return false;
        for(auto state:states_)if(state==State::Writing)return false;
        states_[slot]=State::Writing;return true;
    }
    /** @brief Публикует принятый frame после Core fence; все проверки предшествуют изменению credit. */
    bool seal(const GameSourceTicket&t)noexcept{
        if(!t.valid()||sources_[t.slot]!=t.pixels||states_[t.slot]!=State::Writing||t.sequence<=lastSequence_)return false;
        if(lastSequence_&&(t.generation!=last_.generation||t.epoch!=last_.epoch||t.region!=last_.region||
            t.sequence!=lastSequence_+1||t.inputFrame!=last_.frame||t.beginCycle!=last_.endCycle))return false;
        tickets_[t.slot]=t;last_=t;lastSequence_=t.sequence;states_[t.slot]=State::Ready;return true;
    }
    /** @brief Резервирует источник до Board begin; отказ begin допускает только явный cancelBorrow. */
    bool borrow(const GameSourceTicket&t)noexcept{
        if(!matches(t,State::Ready))return false;
        for(auto state:states_)if(state==State::Reading||state==State::Reserved)return false;
        states_[t.slot]=State::Reserved;return true;
    }
    /** @brief После успешного Board begin делает резерв настоящим reader, включая ранний ISR completion. */
    bool confirmBorrow(const GameSourceTicket&t)noexcept{
        if(!matches(t,State::Reserved))return false;
        states_[t.slot]=State::Reading;return true;
    }
    /** @brief Отменяет только ещё не принятый Board reader; после успешного begin этот метод запрещён договором. */
    bool cancelBorrow(const GameSourceTicket&t)noexcept{
        if(!matches(t,State::Reserved))return false;
        states_[t.slot]=State::Ready;return true;
    }
    /** @brief Возвращает source-credit по полному реальному completion, никогда по timeout. */
    bool complete(const GameSourceTicket&t)noexcept{
        if(!matches(t,State::Reading))return false;
        states_[t.slot]=State::Free;tickets_[t.slot]={};return true;
    }
    /** @brief Выдаёт указатель только зарегистрированного slot, без разрешения писать его. */
    std::uint16_t* source(unsigned slot)const noexcept{return slot<2?sources_[slot]:nullptr;}
    /** @brief Возвращает состояние credit; invalid slot трактуется как занятый reader. */
    State state(unsigned slot)const noexcept{return slot<2?states_[slot]:State::Reading;}
    /** @brief Проверяет отсутствие Readers; реальный join/park писателя обязан обеспечить вызывающий. */
    bool canDestroy()const noexcept{
        for(auto state:states_)if(state==State::Reading||state==State::Reserved)return false;
        return true;
    }
private:
    /** @brief Сравнивает конкретную сохранённую публикацию только после bounded slot/type checks. */
    bool matches(const GameSourceTicket&t,State wanted)const noexcept{
        return t.valid()&&states_[t.slot]==wanted&&sameGameSource(t,tickets_[t.slot]);
    }
    std::uint16_t*sources_[2]{nullptr,nullptr};
    State states_[2]{State::Free,State::Free};
    GameSourceTicket tickets_[2]{},last_{};
    std::uint64_t lastSequence_=0;
};
}
