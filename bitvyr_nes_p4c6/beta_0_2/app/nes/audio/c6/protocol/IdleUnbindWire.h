#pragma once
#include "IdleUnbind.h"
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nes::c6audio::lifecycle {
namespace raw=::nes::c6raw;
/** @brief PRIVATE предложение команды; actual ae1c Raw dispatcher её ещё не регистрирует. */
inline constexpr std::uint16_t unbindOpcode=0x47;
/** @brief Отдельный HELLO capability checked UNBIND; legacy ae1c1ff его не объявляет. */
inline constexpr std::uint32_t capability=0x200U;
inline constexpr std::size_t requestBytes=88,commonBytes=32,snapshotBytes=40,ackBytes=160;

/** @brief Читает проверенный LE span без native struct layout и требований к выравниванию. */
inline std::uint64_t get(const std::uint8_t*p,unsigned bytes) noexcept {
    std::uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=std::uint64_t(p[i])<<(8U*i);return value;
}
/** @brief Пишет проверенное LE поле; caller владеет всем span до возврата. */
inline void put(std::uint8_t*p,std::uint64_t value,unsigned bytes) noexcept {
    for(unsigned i=0;i<bytes;++i)p[i]=std::uint8_t(value>>(8U*i));
}
/** @brief Проверяет отдельные доступные extents численно, исключая wrap и alias. */
inline bool apart(const void*a,std::size_t an,const void*b,std::size_t bn) noexcept {
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    if(!a||!b||an>std::numeric_limits<std::uintptr_t>::max()-x||bn>std::numeric_limits<std::uintptr_t>::max()-y)return false;
    return x<=y?y-x>=an:x-y>=bn;
}
/** @brief Проверяет допустимый cold binding; opaque64 keys не усечены до Raw32. */
inline bool validBinding(const service::Binding&b) noexcept {
    return b.appGeneration&&b.romLease&&b.romEpoch&&b.session&&b.audioGeneration&&b.firstCommandSequence==1&&
        (b.region==core::Region::Ntsc||b.region==core::Region::Pal)&&!(b.audioCapabilities&~core::AudioPacket::externalDacQ16);
}
/** @brief Проверяет непротиворечивый конечный cursor; actual service отдельно сверяет все значения. */
inline bool valid(const service::IdleUnbindRequest&r) noexcept {
    const auto&b=r.expectedBinding;
    return validBinding(b)&&r.completedFrames<=std::numeric_limits<std::uint32_t>::max()&&
        r.audioSequence==r.completedFrames&&r.nextPpuFrame==r.completedFrames+1U&&
        r.nextCommandSequence>=b.firstCommandSequence&&r.nextCommandSequence<std::numeric_limits<std::uint32_t>::max()&&
        (r.completedFrames?(r.finalAudioEndCycle&&r.nextCommandSequence>b.firstCommandSequence):(!r.finalAudioEndCycle&&r.nextCommandSequence==b.firstCommandSequence));
}
/** @brief Сверяет все binding/cursor поля без padding и native memcmp. */
inline bool same(const service::IdleUnbindRequest&a,const service::IdleUnbindRequest&b) noexcept {
    return service::sameIdleBinding(a.expectedBinding,b.expectedBinding)&&a.completedFrames==b.completedFrames&&
        a.audioSequence==b.audioSequence&&a.nextPpuFrame==b.nextPpuFrame&&a.finalAudioEndCycle==b.finalAudioEndCycle&&a.nextCommandSequence==b.nextCommandSequence;
}
/** @brief Кодирует ровно88Б version1; false не изменяет caller output. */
inline bool encodeRequest(const service::IdleUnbindRequest&r,std::uint8_t*out,std::size_t capacity) noexcept {
    if(!out||capacity<requestBytes||!valid(r)||!apart(out,requestBytes,&r,sizeof(r)))return false;
    const auto&b=r.expectedBinding;
    put(out,1,2);put(out+2,0,2);put(out+4,b.appGeneration,8);put(out+12,b.romLease,8);put(out+20,b.romEpoch,8);
    put(out+28,b.session,8);put(out+36,b.audioGeneration,8);put(out+44,b.firstCommandSequence,4);
    out[48]=std::uint8_t(b.region);out[49]=0;put(out+50,b.audioCapabilities,2);put(out+52,r.nextCommandSequence,4);
    put(out+56,r.completedFrames,8);put(out+64,r.audioSequence,8);put(out+72,r.nextPpuFrame,8);put(out+80,r.finalAudioEndCycle,8);return true;
}
/** @brief Декодирует88Б в отдельный temporary; false сохраняет caller request. */
inline bool decodeRequest(const std::uint8_t*p,std::size_t bytes,service::IdleUnbindRequest&out) noexcept {
    if(!p||bytes!=requestBytes||get(p,2)!=1||get(p+2,2)||p[49]||p[48]>1)return false;
    service::IdleUnbindRequest value{};
    value.expectedBinding={get(p+4,8),get(p+12,8),get(p+20,8),get(p+28,8),get(p+36,8),std::uint32_t(get(p+44,4)),core::Region(p[48]),std::uint16_t(get(p+50,2))};
    value.nextCommandSequence=std::uint32_t(get(p+52,4));value.completedFrames=get(p+56,8);value.audioSequence=get(p+64,8);value.nextPpuFrame=get(p+72,8);value.finalAudioEndCycle=get(p+80,8);
    if(!valid(value))return false;
    out=value;return true;
}
/** @brief Строит control key старого потока; final frame32 ограничен valid(request). */
inline raw::Key key(const service::IdleUnbindRequest&r,std::uint8_t slot) noexcept {
    const auto&b=r.expectedBinding;return {b.appGeneration,b.romLease,b.session,std::uint32_t(r.completedFrames?r.completedFrames-1U:0),r.nextCommandSequence,0,0,unbindOpcode,slot,std::uint8_t(b.region)};
}
/** @brief Проверяет outer key вместе с полным request; opcode15 не получает reset полномочий. */
inline bool matchesKey(const raw::Key&k,const service::IdleUnbindRequest&r) noexcept {
    return valid(r)&&k.slot<raw::slotCount&&raw::equal(k,key(r,k.slot));
}
/** @brief Проверяет только actual Unbound snapshot после checked reset, не создавая completion. */
inline bool validSnapshot(const service::Snapshot&s,const service::IdleUnbindRequest&r) noexcept {
    return s.state==service::State::Unbound&&!s.sourceHeld&&!s.timedOut&&s.storageValid&&
        !s.received&&!s.expected&&!s.pcmOffset&&!s.samples&&s.nextSequence==r.nextCommandSequence&&s.rendererCalls==r.completedFrames&&
        s.rendererStatus==core::AudioRenderResult::InvalidPacket;
}
/**
 * @brief Кодирует ACK160: прежний common32, exact echo88, snapshot40.
 * @param common Восемь host-independent32 полей прежнего Raw/audio common prefix.
 * @pre Caller уже удержал actual Raw admission через reset и владеет отдельным output.
 * ACK не доказывает прекращение чтения его источника C6 driver; это отдельный adapter seam.
 */
inline bool encodeAck(const std::uint32_t(&common)[8],const service::IdleUnbindRequest&r,const service::Snapshot&s,std::uint8_t*out,std::size_t capacity) noexcept {
    if(!out||capacity<ackBytes||common[0]||!common[2]||!valid(r)||!validSnapshot(s,r)||
       !apart(out,ackBytes,&r,sizeof(r))||!apart(out,ackBytes,&s,sizeof(s))||!apart(out,ackBytes,common,sizeof(common)))return false;
    for(unsigned i=0;i<8;++i)put(out+4U*i,common[i],4);
    if(!encodeRequest(r,out+commonBytes,requestBytes))return false;
    auto*p=out+commonBytes+requestBytes;
    put(p,1,2);p[2]=std::uint8_t(s.state);p[3]=4;put(p+4,s.nextSequence,4);put(p+8,s.rendererCalls,4);
    put(p+12,0,4);put(p+16,0,4);put(p+20,0,4);put(p+24,0,4);put(p+28,std::uint8_t(s.rendererStatus),4);put(p+32,0,4);put(p+36,0,4);return true;
}
/** @brief Проверяет ACK160 against exact pending request; wireCRC/common/fullRawkey проверяет прежний transport/client. */
inline bool decodeAck(const std::uint8_t*p,std::size_t bytes,const service::IdleUnbindRequest&expected) noexcept {
    if(!p||bytes!=ackBytes||get(p,4)||!get(p+8,4))return false;
    service::IdleUnbindRequest echoed{};if(!decodeRequest(p+commonBytes,requestBytes,echoed)||!same(echoed,expected))return false;
    p+=commonBytes+requestBytes;
    return get(p,2)==1&&p[2]==std::uint8_t(service::State::Unbound)&&p[3]==4&&get(p+4,4)==expected.nextCommandSequence&&get(p+8,4)==expected.completedFrames&&
        !get(p+12,4)&&!get(p+16,4)&&!get(p+20,4)&&!get(p+24,4)&&get(p+28,4)==std::uint8_t(core::AudioRenderResult::InvalidPacket)&&!get(p+32,4)&&!get(p+36,4);
}
} // пространство имён nes::c6audio::lifecycle
