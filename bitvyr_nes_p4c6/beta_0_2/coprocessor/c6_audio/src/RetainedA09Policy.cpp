#include "RetainedA09Policy.h"
#include "RetainedA09Expected.h"
#include <array>
#include <cstring>
#include <limits>

namespace nes::c6rollback {namespace {
/** @brief LE32 из уже bounded metadata, без выравнивания/packed native cast. */
std::uint32_t get(const std::uint8_t*p)noexcept{return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
/** @brief Записывает публичное LE32 поле успеха, не экспортируя arbitrary memory. */
void put(std::uint8_t*p,std::uint32_t v)noexcept{for(unsigned i=0;i<4;++i)p[i]=std::uint8_t(v>>(8U*i));}
/** @brief Проверяет доступный extent без uintptr wrap; физическую доступность доказывает caller. */
bool range(const void*p,std::size_t n)noexcept{return p&&n<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(p);}
/** @brief Не допускает mutation через alias output/written/request до любого callback. */
bool apart(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
    if(!range(a,an)||!range(b,bn))return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);return x+an<=y||y+bn<=x;
}
/** @brief Снимок fixed runningOTA1/nextOTA0 и security/resource/public hash guard. */
bool fixed(const std::uint8_t*m,std::uint32_t boot,bool after)noexcept{
    if(get(m)!=1||get(m+4)!=7||get(m+8)!=0x190000U||get(m+12)!=0x180000U||
       get(m+16)!=0x10000U||get(m+20)!=0x180000U||get(m+24)!=boot||get(m+28)!=0x180000U||
       get(m+32)!=0x400000U||get(m+36)||get(m+40)!=2||get(m+44)!=160000000U||
       std::memcmp(m+80,expected::boot.data(),32)||std::memcmp(m+112,expected::table.data(),32)||
       std::memcmp(m+176,expected::slot0.data(),32))return false;
    bool self=false;for(unsigned i=0;i<32;++i)self|=m[48+i]!=0;if(!self)return false;
    const auto first=get(m+304),second=get(m+316),maximum=first>second?first:second;
    if(!first||!second||maximum>std::numeric_limits<std::uint32_t>::max()-2U||
       get(m+308)!=0xffffffffU||get(m+320)!=0xffffffffU||get(m+312)!=1||get(m+324)!=1)return false;
    // При двух app слотах (seq-1)%2: bootOTA1 имеет even seq, bootOTA0 — odd.
    return bool(maximum&1U)==after;
}
}
/** @brief Проверяет точный retained request по статическим ожидаемым полям, без reset side effects. */
bool validRequest(const std::uint8_t*p,std::size_t n)noexcept{
    if(n!=requestBytes||!range(p,n)||p[0]!=1||p[1]||p[2]||p[3])return false;
    bool sha=false;for(unsigned i=4;i<requestBytes;++i)sha|=p[i]!=0;return sha;
}
Code select(const std::uint8_t*request,std::size_t requestSize,std::uint8_t*output,
    std::size_t capacity,std::size_t&written,Hooks hooks)noexcept{
    if(!validRequest(request,requestSize)||capacity<replyBytes||!range(output,capacity)||!hooks.read||!hooks.select||
       !apart(request,requestSize,output,capacity)||!apart(&written,sizeof(written),request,requestSize)||
       !apart(&written,sizeof(written),output,capacity))return Code::BadRequest;
    std::array<std::uint8_t,metadataBytes>meta{};
    if(!hooks.read(hooks.context,meta.data(),meta.size()))return Code::Metadata;
    if(!fixed(meta.data(),0x190000U,false)||std::memcmp(request+4,meta.data()+144,32))return Code::Guard;
    // Запоминается только собственный image/slot1 identity; после setboot могут меняться лишь OTAdata/boot.
    std::array<std::uint8_t,64>current{};std::memcpy(current.data(),meta.data()+48,32);std::memcpy(current.data()+32,meta.data()+208,32);
    if(!hooks.select(hooks.context))return Code::SetBoot;
    if(!hooks.read(hooks.context,meta.data(),meta.size())||!fixed(meta.data(),0x10000U,true)||
       !std::memcmp(request+4,meta.data()+144,32)||std::memcmp(current.data(),meta.data()+48,32)||
       std::memcmp(current.data()+32,meta.data()+208,32))return Code::PostVerify;
    std::array<std::uint8_t,replyBytes>reply{};put(reply.data(),1);put(reply.data()+4,0x10000U);std::memcpy(reply.data()+8,meta.data()+144,32);
    std::memcpy(output,reply.data(),reply.size());written=reply.size();return Code::Ok;
}
}
