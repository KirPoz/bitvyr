/** @file Vrc7WriteTape.h
 * @brief Caller-backed канонические FM-записи одного CPU1 owned-frame, без SDK/heap.
 * @details Caller удерживает объект и внешний массив до реального consumer ACK.
 * Recording имеет единственного Worker writer; Sealed immutable. Fault sticky:
 * частичный trace не публикуется, освобождение fault-контекста требует joined destroy.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nes::core {
class Vrc7WriteTape final {
public:
    enum class State:std::uint8_t {Empty,Recording,Sealed,Faulted};
    enum class Fault:std::uint8_t {None,Storage,State,Range,Order,Address,Overflow,MissingWriter};
    /** @brief offset — actual writeCycle-baseCycle; одинаковые cycle сохраняют порядок. */
    struct Event {std::uint32_t offset=0;std::uint16_t address=0;std::uint8_t value=0,reserved=0;};
    static_assert(sizeof(Event)==8,"Bounded local FM event; no wire/ABI guessing");
    /** @brief Заимствованный immutable trace; валиден только Sealed/None и с внешней идентичностью ticket. */
    struct View {
        const Event*events=nullptr;std::size_t count=0;
        std::uint64_t baseCycle=0,endCycle=0;
        State state=State::Empty;Fault fault=Fault::None;
        /** @brief Лишь проверяет Sealed/None; identity ticket, границы и lifetime массива удостоверяет внешний owner. */
        [[nodiscard]] bool sealed()const noexcept{return state==State::Sealed&&fault==Fault::None;}
    };
    /** @brief Создаёт пустой tape без памяти; перед begin требуется единственный configureOnce. */
    Vrc7WriteTape()noexcept=default;
    /** @brief Cold-привязка caller-массива; неверные backing сохраняются как sticky fault, память не выделяется. */
    Vrc7WriteTape(Event*events,std::size_t capacity)noexcept{static_cast<void>(configureOnce(events,capacity));}
    /** @brief Запрещает копирование writer и дублирование заимствованного массива событий. */
    Vrc7WriteTape(const Vrc7WriteTape&)=delete;
    /** @brief Запрещает заменять живую аренду присваиванием другого tape. */
    Vrc7WriteTape&operator=(const Vrc7WriteTape&)=delete;
    /** @brief Единственная cold-конфигурация внешней памяти; после binding её адрес неизменен. */
    bool configureOnce(Event*events,std::size_t capacity)noexcept{
        if(fault_!=Fault::None)return false;
        if(events_||capacity_||state_!=State::Empty||count_)return fail(Fault::State);
        if(!validStorage(events,capacity))return fail(Fault::Storage);
        events_=events;capacity_=capacity;return true;
    }
    /** @brief Начинает только действительно свободный слот; первый base=7, позднее previous audioEnd. */
    bool begin(std::uint64_t baseCycle)noexcept{
        if(fault_!=Fault::None)return false;
        if(state_!=State::Empty||count_)return fail(Fault::State);
        if(!validStorage(events_,capacity_))return fail(Fault::Storage);
        base_=end_=baseCycle;state_=State::Recording;return true;
    }
    /** @brief Дописывает один actual bus write; не coalesce, не переносит clock и не теряет RMW/duplicates. */
    bool append(std::uint64_t cycle,std::uint16_t address,std::uint8_t value)noexcept{
        if(fault_!=Fault::None)return false;
        if(state_!=State::Recording)return fail(Fault::State);
        if(address!=0x9010U&&address!=0x9030U&&address!=0xe000U)return fail(Fault::Address);
        if(cycle<base_||cycle-base_>std::numeric_limits<std::uint32_t>::max())return fail(Fault::Range);
        const auto offset=static_cast<std::uint32_t>(cycle-base_);
        if(count_&&offset<events_[count_-1U].offset)return fail(Fault::Order);
        if(count_>=capacity_)return fail(Fault::Overflow);
        events_[count_++]={offset,address,value,0};return true;
    }
    /** @brief Запечатывает один финальный audioCut после tail+metadata; повторный seal — ошибка. */
    bool seal(std::uint64_t endCycle)noexcept{
        if(fault_!=Fault::None)return false;
        if(state_!=State::Recording)return fail(Fault::State);
        if(endCycle<base_||endCycle-base_>std::numeric_limits<std::uint32_t>::max())return fail(Fault::Range);
        if(count_&&events_[count_-1U].offset>endCycle-base_)return fail(Fault::Order);
        end_=endCycle;state_=State::Sealed;return true;
    }
    /** @brief Только caller после matching joined/DATA/TX ACK разрешает повторную запись в массив. */
    bool release()noexcept{
        if(fault_!=Fault::None)return false;
        if(state_!=State::Sealed)return fail(Fault::State);
        count_=0;base_=end_=0;state_=State::Empty;return true;
    }
    /** @brief Копирует метаданные без clock/flush/reset. Recording/Faulted не являются publishable View. */
    [[nodiscard]] View view()const noexcept{return {events_,count_,base_,end_,state_,fault_};}
    /** @brief Возвращает локальную фазу без memory barrier; межъядерное владение подтверждается внешним slot-протоколом. */
    [[nodiscard]] State state()const noexcept{return state_;}
    /** @brief Возвращает первую sticky ошибку без сброса состояния либо освобождения массива. */
    [[nodiscard]] Fault fault()const noexcept{return fault_;}
    /** @brief Возвращает caller-owned base; mutable тип указателя не разрешает запись в Sealed tape. */
    [[nodiscard]] Event*storage()const noexcept{return events_;}
    /** @brief Даёт ёмкость внешнего массива в событиях, а не число уже записанных событий. */
    [[nodiscard]] std::size_t capacity()const noexcept{return capacity_;}
    /** @brief Даёт полный размер backing в байтах; чтение незаписанной части этим не разрешается. */
    [[nodiscard]] std::size_t storageBytes()const noexcept{return capacity_*sizeof(Event);}
    /** @brief Проверяет Recording/None без подтверждения владельца и без перехода состояния. */
    [[nodiscard]] bool recording()const noexcept{return state_==State::Recording&&fault_==Fault::None;}
    /** @brief Pure storage admission; alias с ROM/RAM/tapes проверяет owned-frame binder. */
    [[nodiscard]] static bool validStorage(const Event*events,std::size_t capacity)noexcept{
        if(!events||!capacity||capacity>std::numeric_limits<std::size_t>::max()/sizeof(Event))return false;
        const auto address=reinterpret_cast<std::uintptr_t>(events),bytes=capacity*sizeof(Event);
        return address%alignof(Event)==0&&bytes<=std::numeric_limits<std::uintptr_t>::max()-address;
    }
private:
    /** @brief Сохраняет первую ошибку и переводит в Faulted; массив и частичный trace остаются у прежнего owner. */
    bool fail(Fault fault)noexcept{if(fault_==Fault::None)fault_=fault;state_=State::Faulted;return false;}
    Event*events_=nullptr;std::size_t capacity_=0,count_=0;
    std::uint64_t base_=0,end_=0;State state_=State::Empty;Fault fault_=Fault::None;
};
} // namespace nes::core
