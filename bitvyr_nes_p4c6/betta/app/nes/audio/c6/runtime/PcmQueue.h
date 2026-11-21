#pragma once
#include "AudioPublished.h"
#include "apu/Apu.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::app::c6_async {
namespace publication=::nes::c6audio::async;
/**
 * @brief PRIVATE Main-only FIFO двух PCM ticket с явным отставанием на один guest frame.
 * @details Storage предоставляет App: прежний Snapshot::pcm плюс один новый4096Б.
 * Класс не читает Machine, не вызывает I2S и не трактует timeout как освобождение.
 */
class PcmQueue final {
public:
    /** @brief Состояние PCM отличается от состояния input wire и Worker join. */
    enum class State:std::uint8_t {Free,Reserved,Ready,Output};
    /** @brief Невладеющий immutable ключ выдаваемого интервала и оставшийся mono16 span. */
    struct Output final {publication::PublishedKey key{};const std::int16_t*data{};std::size_t samples{},offset{};};
    /** @brief Не связывает buffers неявно и не выделяет память. */
    PcmQueue()noexcept=default;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    PcmQueue(const PcmQueue&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    PcmQueue&operator=(const PcmQueue&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    PcmQueue(PcmQueue&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    PcmQueue&operator=(PcmQueue&&)=delete;
    /** @brief Cold Main связывает два настоящих раздельных выровненных PCM span один раз. */
    bool initialize(std::int16_t*a,std::int16_t*b,std::size_t capacity)noexcept{
        if(valid_||!a||!b||!capacity||capacity>core::Apu::sampleCapacity||
           reinterpret_cast<std::uintptr_t>(a)%alignof(std::int16_t)||reinterpret_cast<std::uintptr_t>(b)%alignof(std::int16_t)||
           !apart(a,capacity*sizeof(*a),b,capacity*sizeof(*b))||
           !apart(a,capacity*sizeof(*a),this,sizeof(*this))||!apart(b,capacity*sizeof(*b),this,sizeof(*this)))return false;
        slots_[0].pcm=a;slots_[1].pcm=b;capacity_=capacity;valid_=true;return true;
    }
    /**
     * @brief Main резервирует следующий непрерывный frame и получает его writable PCM destination.
     * @details Не допускает третьего владельца, повторного full-key или смены потока/ROM.
     */
    bool reserve(const publication::PublishedKey&key,std::int16_t*&destination)noexcept{
        const auto&a=key.audio;
        if(!valid_||count_==slots_.size()||key.inputFrame!=nextInput_||key.inputFrame==~std::uint64_t{0}||
           !key.appGeneration||!key.romLease||!key.romEpoch||!key.session||key.arena!=1||!a.generation||
           a.sequence!=nextInput_||a.ppuFrame!=key.inputFrame+1U||a.baseCycle!=nextBase_||a.endCycle<=a.baseCycle||
           a.endCycle-a.baseCycle>=45000||(a.region!=core::Region::Ntsc&&a.region!=core::Region::Pal)||
           (a.capabilities&~core::AudioPacket::externalDacQ16))return false;
        if(nextInput_&&(key.appGeneration!=stream_.appGeneration||key.romLease!=stream_.romLease||key.romEpoch!=stream_.romEpoch||
           key.session!=stream_.session||a.generation!=stream_.audio.generation||a.region!=stream_.audio.region||a.capabilities!=stream_.audio.capabilities))return false;
        auto&slot=slots_[(head_+count_)%slots_.size()];
        if(slot.state!=State::Free)return false;
        slot.key=key;slot.samples=slot.offset=0;slot.state=State::Reserved;
        if(!nextInput_)stream_=key;
        ++nextInput_;nextBase_=a.endCycle;++count_;destination=slot.pcm;return true;
    }
    /** @brief Matching CONSUMED завершает полную remote PCM запись, не выполняя вывода звука. */
    bool ready(const publication::PublishedKey&key,std::size_t samples)noexcept{
        if(!valid_||samples>capacity_)return false;
        for(auto&slot:slots_)if(slot.state==State::Reserved&&publication::same(slot.key,key)){
            slot.samples=samples;slot.state=State::Ready;return true;
        }
        return false;
    }
    /**
     * @brief Открывает только старейший PCM_N после фактического dispatch guestN+1.
     * @param draining Joined stop может явно вывести последний PCM без дополнительного guest frame.
     * @details Неготовый PCM и перепутанный frame возвращают false без снятия owner.
     */
    bool beginOutput(std::uint64_t dispatchedInput,bool draining,Output&output)noexcept{
        if(!valid_||!count_)return false;
        auto&slot=slots_[head_];
        if((slot.state!=State::Ready&&slot.state!=State::Output)||(!draining&&dispatchedInput!=slot.key.inputFrame+1U))return false;
        slot.state=State::Output;output={slot.key,slot.pcm+slot.offset,slot.samples-slot.offset,slot.offset};return true;
    }
    /**
     * @brief Продвигает ровно реально принятые Board/I2S mono16 отсчёты того же oldest ticket.
     * @details Partial write сохраняет остаток;0 при непустом остатке ничего не освобождает.
     * Функцию нельзя вызывать до возврата source-reading Board API.
     */
    bool outputAccepted(const publication::PublishedKey&key,std::size_t accepted)noexcept{
        if(!valid_||!count_)return false;
        auto&slot=slots_[head_];
        if(slot.state!=State::Output||!publication::same(slot.key,key)||accepted>slot.samples-slot.offset||
           (!accepted&&slot.offset!=slot.samples))return false;
        slot.offset+=accepted;
        if(slot.offset==slot.samples){slot.key={};slot.samples=slot.offset=0;slot.state=State::Free;head_=(head_+1U)%slots_.size();--count_;}
        return true;
    }
    /** @brief Только Main может читать количество; оно не является Worker completion. */
    std::size_t occupied()const noexcept{return count_;}
    /** @brief Запрещает запуск N+2, пока старейший PCM_N ещё не принят I2S; это backpressure, не frameskip. */
    bool canDispatch(std::uint64_t input)const noexcept{
        return valid_&&(!count_||input<=slots_[head_].key.inputFrame+1U);
    }
    /** @brief Возвращает фиксированную ёмкость caller PCM в mono16 samples. */
    std::size_t capacity()const noexcept{return capacity_;}
    /** @brief Проверяет отсутствие всех PCM заимствований без очистки ticket по одному count. */
    bool emptyForRebind()const noexcept{
        if(!valid_||count_)return false;
        for(const auto&slot:slots_)if(slot.state!=State::Free||slot.samples||slot.offset||!slot.pcm)return false;
        return true;
    }
    /** @brief На известной Main joined/unbound границе сохраняет два адреса/capacity и начинает новый stream. */
    bool rebindEmpty()noexcept{
        if(!emptyForRebind())return false;
        for(auto&slot:slots_)slot.key={};
        stream_={};nextInput_=nextBase_=0;head_=count_=0;return true;
    }
private:
    /** @brief Невладеющий stable slot никогда не освобождает caller PCM самостоятельно. */
    struct Slot final {publication::PublishedKey key{};std::int16_t*pcm{};std::size_t samples{},offset{};State state=State::Free;};
    /** @brief Сравнивает действительные полуоткрытые extents без pointer-order UB/overflow. */
    static bool apart(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
        if(!a||!b||an>~std::uintptr_t{0}-x||bn>~std::uintptr_t{0}-y)return false;
        return x+an<=y||y+bn<=x;
    }
    std::array<Slot,2>slots_{};publication::PublishedKey stream_{};
    std::uint64_t nextInput_{},nextBase_{};std::size_t capacity_{},head_{},count_{};bool valid_{};
};
} // namespace nes::app::c6_async
