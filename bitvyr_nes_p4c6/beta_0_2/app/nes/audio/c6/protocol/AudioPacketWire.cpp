#include "AudioPacketWire.h"
#include <limits>

namespace nes::c6audio::wire {
namespace {
using Packet=nes::core::AudioPacket;
constexpr std::uint32_t magic=0x3155414eU; // Literal bytes NAU1.
/** @brief Пишет LE без unaligned native store или зависимости от endian CPU. */
void put(std::uint8_t* p,std::uint64_t value,unsigned count) noexcept {
    for(unsigned i=0;i<count;++i)p[i]=std::uint8_t(value>>(8U*i));
}
/** @brief Читает уже ограниченное поле; count не превышает восемь байт. */
std::uint64_t get(const std::uint8_t* p,unsigned count) noexcept {
    std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t(p[i])<<(8U*i);return value;
}
/** @brief Проверяет extent без вычисления указателя за пределами объекта. */
bool range(const void* p,std::size_t bytes) noexcept {
    return bytes==0||(p&&bytes<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(p));
}
/** @brief Сверяет неперекрытие двух непустых ранее проверенных диапазонов. */
bool disjoint(const void* a,std::size_t na,const void* b,std::size_t nb) noexcept {
    if(!na||!nb)return true;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    return x+na<=y||y+nb<=x;
}
/** @brief Сверяет packet-domain identity; внешний ROM/transport key не читается здесь. */
bool matches(const Packet::Header& h,const Expected& e) noexcept {
    return h.generation==e.generation&&h.sequence==e.sequence&&h.ppuFrame==e.ppuFrame&&
        h.baseCycle==e.baseCycle&&h.endCycle==e.endCycle&&h.region==e.region&&h.capabilities==e.capabilities;
}
/** @brief Восстанавливает signed32 из LE без implementation-defined unsigned→signed overflow. */
std::int32_t signed32(const std::uint8_t* p) noexcept {
    const auto u=std::uint32_t(get(p,4));
    return u<=0x7fffffffU?std::int32_t(u):std::int32_t(-1-std::int64_t(0xffffffffU-u));
}
} // namespace

/** @brief Проверяет used counts/capabilities и считает wire bytes; ноль означает отказ без allocation. */
std::size_t encodedSize(const Packet::Header& h) noexcept {
    if(!h.generation||h.fault!=nes::core::AudioFault::None||
       (h.region!=nes::core::Region::Ntsc&&h.region!=nes::core::Region::Pal)||
       h.endCycle<h.baseCycle||h.endCycle-h.baseCycle>65535||
       h.inputCount>Packet::inputCapacity||h.dmcCount>Packet::dmcCapacity||
       h.expansionCount>Packet::expansionCapacity||h.formatVersion!=Packet::expansionVersion||
       (h.capabilities&~Packet::externalDacQ16)||(!h.capabilities&&h.expansionCount))return 0;
    const std::uint64_t bytes=headerBytes+std::uint64_t(h.inputCount)*4U+
        std::uint64_t(h.dmcCount)*12U+std::uint64_t(h.expansionCount)*8U;
    return bytes<=std::numeric_limits<std::size_t>::max()?std::size_t(bytes):0;
}

/** @brief Проверяет typed used extents и alias до первой записи; кодирует LE records без native padding. */
bool encode(const nes::core::AudioPacketView& view,std::uint8_t* output,
            std::size_t capacity,std::size_t& written) noexcept {
    if(!view.readable())return false;
    const auto& h=view.header();const auto bytes=encodedSize(h);
    const std::size_t ni=std::size_t(h.inputCount)*4U,nd=std::size_t(h.dmcCount)*12U,ne=std::size_t(h.expansionCount)*8U;
    if(!bytes||capacity<bytes||!range(output,bytes)||!range(&h,sizeof(h))||
       !range(view.inputs(),ni)||!range(view.dmc(),nd)||!range(view.expansions(),ne)||
       !disjoint(output,bytes,&view,sizeof(view))||!disjoint(output,bytes,&written,sizeof(written))||
       !disjoint(&written,sizeof(written),&view,sizeof(view))||!disjoint(&written,sizeof(written),&h,sizeof(h))||
       !disjoint(&written,sizeof(written),view.inputs(),ni)||!disjoint(&written,sizeof(written),view.dmc(),nd)||
       !disjoint(&written,sizeof(written),view.expansions(),ne)||
       !disjoint(output,bytes,&h,sizeof(h))||!disjoint(output,bytes,view.inputs(),ni)||
       !disjoint(output,bytes,view.dmc(),nd)||!disjoint(output,bytes,view.expansions(),ne))return false;
    // Все возможные отказы закончились до первого изменения output.
    for(std::size_t i=0;i<headerBytes;++i)output[i]=0;
    put(output,magic,4);put(output+4,version,2);put(output+6,headerBytes,2);put(output+8,bytes,4);
    put(output+12,h.formatVersion,2);put(output+14,h.capabilities,2);
    put(output+16,h.generation,8);put(output+24,h.sequence,8);put(output+32,h.ppuFrame,8);
    put(output+40,h.baseCycle,8);put(output+48,h.endCycle,8);
    put(output+56,h.inputCount,4);put(output+60,h.dmcCount,4);put(output+64,h.expansionCount,4);
    output[68]=std::uint8_t(h.region);output[69]=std::uint8_t(h.fault);
    auto* p=output+headerBytes;
    for(std::uint32_t i=0;i<h.inputCount;++i,p+=4){const auto& e=view.inputs()[i];put(p,e.offset,2);p[2]=e.operation;p[3]=e.value;}
    for(std::uint32_t i=0;i<h.dmcCount;++i,p+=12){const auto& e=view.dmc()[i];put(p,e.offset,2);put(p+2,e.address,2);p[4]=e.value;p[5]=e.stall;put(p+6,e.reserved,2);put(p+8,std::uint32_t(e.expansionLevel),4);}
    for(std::uint32_t i=0;i<h.expansionCount;++i,p+=8){const auto& e=view.expansions()[i];put(p,e.offset,2);put(p+2,e.nativePrefix,2);put(p+4,std::uint32_t(e.level),4);}
    written=bytes;return true;
}

/** @brief Проверяет complete wire/key/records до копирования в typed storage; Header записывается последним. */
bool decode(const std::uint8_t* input,std::size_t bytes,const Expected& expected,
            const Destination& d) noexcept {
    if(bytes<headerBytes||!range(input,bytes)||!d.header||get(input,4)!=magic||
       get(input+4,2)!=version||get(input+6,2)!=headerBytes||get(input+8,4)!=bytes)return false;
    for(unsigned i=70;i<headerBytes;++i)if(input[i])return false;
    Packet::Header h{};
    h.formatVersion=std::uint16_t(get(input+12,2));h.capabilities=std::uint16_t(get(input+14,2));
    h.generation=get(input+16,8);h.sequence=get(input+24,8);h.ppuFrame=get(input+32,8);
    h.baseCycle=get(input+40,8);h.endCycle=get(input+48,8);
    h.inputCount=std::uint32_t(get(input+56,4));h.dmcCount=std::uint32_t(get(input+60,4));h.expansionCount=std::uint32_t(get(input+64,4));
    h.region=nes::core::Region(input[68]);h.fault=nes::core::AudioFault(input[69]);
    const auto size=encodedSize(h);
    const std::size_t ni=std::size_t(h.inputCount)*4U,nd=std::size_t(h.dmcCount)*12U,ne=std::size_t(h.expansionCount)*8U;
    if(!size||size!=bytes||!matches(h,expected)||h.inputCount>d.inputCapacity||h.dmcCount>d.dmcCapacity||
       h.expansionCount>d.expansionCapacity||!range(d.header,sizeof(h))||!range(d.input,ni)||
       !range(d.dmc,nd)||!range(d.expansion,ne)||!disjoint(input,bytes,d.header,sizeof(h))||
       !disjoint(input,bytes,d.input,ni)||!disjoint(input,bytes,d.dmc,nd)||!disjoint(input,bytes,d.expansion,ne)||
       !disjoint(d.header,sizeof(h),d.input,ni)||!disjoint(d.header,sizeof(h),d.dmc,nd)||
       !disjoint(d.header,sizeof(h),d.expansion,ne)||!disjoint(d.input,ni,d.dmc,nd)||
       !disjoint(d.input,ni,d.expansion,ne)||!disjoint(d.dmc,nd,d.expansion,ne)||
       !disjoint(&d,sizeof(d),d.header,sizeof(h))||!disjoint(&d,sizeof(d),d.input,ni)||
       !disjoint(&d,sizeof(d),d.dmc,nd)||!disjoint(&d,sizeof(d),d.expansion,ne))return false;
    // Семантику проверяет renderer; фактический DMC mismatch может выявиться уже при replay.
    // Такой fault требует его прежнего reset; codec гарантирует лишь размер/identity/representation.
    const auto* p=input+headerBytes;
    for(std::uint32_t i=0;i<h.inputCount;++i,p+=4)d.input[i]={std::uint16_t(get(p,2)),p[2],p[3]};
    for(std::uint32_t i=0;i<h.dmcCount;++i,p+=12)d.dmc[i]={std::uint16_t(get(p,2)),std::uint16_t(get(p+2,2)),p[4],p[5],std::uint16_t(get(p+6,2)),signed32(p+8)};
    for(std::uint32_t i=0;i<h.expansionCount;++i,p+=8)d.expansion[i]={std::uint16_t(get(p,2)),std::uint16_t(get(p+2,2)),signed32(p+4)};
    *d.header=h;return true;
}
} // namespace nes::c6audio::wire
