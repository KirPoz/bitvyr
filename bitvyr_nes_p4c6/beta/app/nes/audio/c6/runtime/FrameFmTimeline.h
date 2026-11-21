#pragma once
#include "cartridge/Vrc7Audio.h"
#include "apu/AudioPacket.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nes::app::frame_fm {
/** @brief Каноническая запись после M2 cycle; одинаковые cycle сохраняют входной порядок. */
struct FmWrite {std::uint64_t cycle;std::uint16_t address;std::uint8_t value;};
/** @brief Срез одного прежнего AudioPacket; offsets native относятся к неизменному packetBase. */
struct FmSlice {
    std::uint64_t packetBase=0,endCycle=0;
    const FmWrite*writes=nullptr;std::size_t writeCount=0;
    const core::AudioPacket::Input*native=nullptr;std::size_t nativeCount=0;
    std::uint16_t nativePrefixBase=0;
    const std::uint64_t*dmcCycles=nullptr;std::size_t dmcCount=0;
};
/** @brief Наблюдаемый seed на границе; не save-state и не право восстановить скрытые FM-регистры. */
struct FmSeed {
    std::uint64_t cycle=0;
    std::int32_t raw=0,portable=0,lastPublished=0;
    std::uint32_t nextSynthesis=36,nextPortable=64;
};
enum class FmProbe:std::uint8_t {Write,Dmc};
enum class FmFault:std::uint8_t {None,Range,Order,Address,Prefix,Output};
/** @brief Убирается constexpr-веткой: обычный consumer не оплачивает test-наблюдение. */
struct NoFmObserver {};
/**
 * @brief Один CPU0-владелец FM timeline без SDK/heap/ROM; Core Vrc7Audio не изменяется.
 * @details M2 T сначала продвигает генератор, затем публикует DAC на T-1; DMC видит
 * уже clock(T), MMIO следует после него. Native Input с timestamp T-1 уже записаны
 * до clock(T), поэтому их точный prefix берётся из исходной Input-ленты.
 * Состояние36/64 и lastPublished непрерывны между пакетами/срезами. Full reset
 * разрешён только внешним владельцем нового audio generation, не на каждом кадре.
 */
class FmTimeline final {
public:
    /** @brief Создаёт полный reset на cycle0; первые reset0..7 входят в эту же ось времени. */
    FmTimeline()noexcept=default;
    /** @brief Сбрасывает только при доказанном новом audio generation без живых loans. */
    void reset()noexcept {fm_.reset();cycle_=packetBase_=0;prefix_=0;last_=0;started_=false;fault_=FmFault::None;}
    /** @brief Возвращает значение для joined-наблюдения; не сериализует внутренний model. */
    [[nodiscard]] FmSeed seed()const noexcept{return {cycle_,fm_.sample(),fm_.portableSample(),last_,fm_.nextOutputCpuCycles(),fm_.nextPortableOutputCpuCycles()};}
    /** @brief Первый sticky отказ запрещает продолжать частично исполненный timeline. */
    [[nodiscard]] FmFault fault()const noexcept{return fault_;}
    /**
     * @brief Исполняет непрерывный срез; sink.expansion(existing Expansion), sink.dmc(index,level) возвращают bool.
     * @details Новый packetBase равен текущему cycle и prefix0. Продолжение того же
     * пакета требует точный nativePrefixBase; native span содержит только новые
     * записи. При отказе частичный output НЕ пригоден к publication; rollback нет.
     * Observer применяется только тестом после каждой записи/DMC и не читает Machine.
     */
    template<class Sink,class Observer=NoFmObserver>
    bool render(const FmSlice&s,Sink&sink,Observer observer={})noexcept {
        if(fault_!=FmFault::None)return false;
        if(!validate(s))return false;
        const bool fresh=!started_||s.packetBase!=packetBase_;
        if(fresh){packetBase_=s.packetBase;prefix_=0;started_=true;}
        std::size_t wi=0,di=0,ni=0;
        const auto prefixAt=[&](std::uint64_t stamp)noexcept{
            while(ni<s.nativeCount&&packetBase_+s.native[ni].offset<=stamp)++ni;
            return static_cast<std::uint16_t>(s.nativePrefixBase+ni);
        };
        const auto advance=[&](std::uint64_t target)noexcept{
            while(cycle_<target){
                const auto step=static_cast<std::uint32_t>(std::min<std::uint64_t>(target-cycle_,fm_.nextPortableOutputCpuCycles()));
                fm_.clockCpu(step);cycle_+=step;
                const auto level=fm_.portableSample();
                if(level!=last_){
                    const auto stamp=cycle_-1U;
                    const core::AudioPacket::Expansion e{static_cast<std::uint16_t>(stamp-packetBase_),prefixAt(stamp),level};
                    if(!sink.expansion(e))return fail(FmFault::Output);
                    last_=level;
                }
            }
            return true;
        };
        while(wi<s.writeCount||di<s.dmcCount){
            auto target=s.endCycle;
            if(wi<s.writeCount)target=std::min(target,s.writes[wi].cycle);
            if(di<s.dmcCount)target=std::min(target,s.dmcCycles[di]);
            if(!advance(target))return false;
            while(di<s.dmcCount&&s.dmcCycles[di]==cycle_){
                const auto level=fm_.portableSample();
                if(!sink.dmc(di,level))return fail(FmFault::Output);
                last_=level;
                if constexpr(!std::is_same_v<Observer,NoFmObserver>)observer(FmProbe::Dmc,di,seed());
                ++di;
            }
            while(wi<s.writeCount&&s.writes[wi].cycle==cycle_){
                fm_.cpuWrite(s.writes[wi].address,s.writes[wi].value);
                if constexpr(!std::is_same_v<Observer,NoFmObserver>)observer(FmProbe::Write,wi,seed());
                ++wi;
            }
        }
        if(!advance(s.endCycle))return false;
        prefix_=static_cast<std::uint16_t>(s.nativePrefixBase+s.nativeCount);
        return true;
    }
private:
    /** @brief Фиксирует первую причину; никаких ложных rollback/reset при частичном output. */
    bool fail(FmFault f)noexcept{if(fault_==FmFault::None)fault_=f;return false;}
    /** @brief Проверяет все входные границы до первого продвижения FM; адресный decoder остаётся вне helper. */
    bool validate(const FmSlice&s)noexcept{
        if(s.packetBase>cycle_||s.endCycle<cycle_||s.endCycle-s.packetBase>65535U||
           s.writeCount>65536U||s.dmcCount>65536U||
           (s.writeCount&&!s.writes)||(s.nativeCount&&!s.native)||(s.dmcCount&&!s.dmcCycles))return fail(FmFault::Range);
        if(started_&&s.packetBase==packetBase_){if(s.nativePrefixBase!=prefix_)return fail(FmFault::Prefix);}
        else if(s.packetBase!=cycle_||s.nativePrefixBase)return fail(FmFault::Prefix);
        if(s.nativePrefixBase>core::AudioPacket::inputCapacity||
           s.nativeCount>core::AudioPacket::inputCapacity-s.nativePrefixBase)return fail(FmFault::Prefix);
        for(std::size_t i=0;i<s.writeCount;++i){const auto&e=s.writes[i];
            if(e.cycle<cycle_||e.cycle>s.endCycle||(i&&e.cycle<s.writes[i-1U].cycle))return fail(FmFault::Order);
            if(e.address!=0x9010U&&e.address!=0x9030U&&e.address!=0xe000U)return fail(FmFault::Address);
        }
        for(std::size_t i=0;i<s.nativeCount;++i){const auto stamp=s.packetBase+s.native[i].offset;
            if(stamp<cycle_||stamp>s.endCycle||(i&&s.native[i].offset<s.native[i-1U].offset))return fail(FmFault::Order);
        }
        for(std::size_t i=0;i<s.dmcCount;++i)
            if(s.dmcCycles[i]<cycle_||s.dmcCycles[i]>s.endCycle||(i&&s.dmcCycles[i]<s.dmcCycles[i-1U]))return fail(FmFault::Order);
        return true;
    }
    core::Vrc7Audio fm_{};
    std::uint64_t cycle_=0,packetBase_=0;
    std::uint16_t prefix_=0;
    std::int32_t last_=0;
    bool started_=false;
    FmFault fault_=FmFault::None;
};
} // namespace nes::app::frame_fm
