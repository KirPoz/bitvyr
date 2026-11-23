#include "RawAsyncAdapter.h"
#include <cstring>
#include <limits>

namespace nes::c6audio::async {namespace {
/** @brief Читает LE32 уже bounded body, не native struct. */
std::uint32_t get(const std::uint8_t*p)noexcept{return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
/** @brief Проверяет caller byte extent и alias с изменяемым adapter metadata. */
bool separate(const void*p,std::size_t n,const void*object,std::size_t objectBytes)noexcept{
    if(!n)return true;
    const auto x=reinterpret_cast<std::uintptr_t>(p),y=reinterpret_cast<std::uintptr_t>(object);
    return p&&n<=std::numeric_limits<std::uintptr_t>::max()-x&&(x+n<=y||y+objectBytes<=x);
}
}
/** @brief Legacy и checked-warm capability sets задаются точно; bit сам по себе не квалифицирует firmware. */
bool RawAsyncAdapter::validProfile(const RawProfile&profile)noexcept{
    const auto&k=profile.hello;bool nonzero=false;for(auto b:profile.elf)nonzero|=b!=0;
    const auto known=raw::helloCapabilities|lifecycle::capability;
    return nonzero&&k.generation&&k.lease&&k.session&&!k.frame&&!k.sequence&&!k.begin&&!k.end&&!k.opcode&&k.slot<raw::slotCount&&k.region<=1&&
        (profile.expectedCapabilities==raw::helloCapabilities||profile.expectedCapabilities==known)&&
        (!profile.warmIdleUnbind||(profile.expectedCapabilities&lifecycle::capability));
}
/** @brief Проверяет immutable boot binding и ненулевой ELF до HELLO; sticky poison не отменяет владельцев. */
bool RawAsyncAdapter::beginHello()noexcept{
    const auto&k=profile_.hello;
    if(qualified_||!validProfile(profile_)){poisoned_=true;return false;}
    return submit(k,nullptr,0,true);
}
/** @brief Main допускает один bounded request того же binding; Channel хранит источник до actual sourceDone. */
bool RawAsyncAdapter::submit(const raw::Key&k,const std::uint8_t*p,std::size_t n,bool isHello)noexcept{
    const auto&b=profile_.hello;
    if(poisoned_||pending_||channel_.occupied()||!clock_||k.generation!=b.generation||k.lease!=b.lease||k.session!=b.session||k.region!=b.region||
       k.slot>=raw::slotCount||n>raw::requestPayloadBytes||!separate(p,n,this,sizeof(*this))||
       (!isHello&&(!qualified_||!(k.opcode==12||k.opcode==13||(k.opcode>=0x40&&k.opcode<=0x46)||k.opcode==lifecycle::unbindOpcode)))){poisoned_=true;return false;}
    if(!isHello&&k.opcode==lifecycle::unbindOpcode){
        service::IdleUnbindRequest expected{};
        if(!warmQualified()||!lifecycle::decodeRequest(p,n,expected)||!lifecycle::matchesKey(k,expected)){poisoned_=true;return false;}
        // Пока Channel держит собственный TX, прежний reply buffer хранит exact request88.
        // Ни нового hot buffer, ни заимствованного caller pointer здесь нет.
        std::memcpy(reply_.data(),p,lifecycle::requestBytes);
    }
    unbindVerified_=false;
    request_=k;helloPending_=isHello;started_=clock_();
    if(!channel_.submit(k,p,n)){poisoned_=true;return false;}
    pending_=true;return true;
}
/** @brief StartFn проверяет context и делегирует admission; false не отменяет ранее принятый TX. */
bool RawAsyncAdapter::start(void*context,const raw::Key&key,const std::uint8_t*p,std::size_t n)noexcept{
    return context&&static_cast<RawAsyncAdapter*>(context)->submit(key,p,n,false);
}
/** @brief Сверяет exact100-byte HELLO, ELF, capability и directional MTU без native ABI structs. */
bool RawAsyncAdapter::verifyHello(const raw::View&v)noexcept{
    if(v.size!=raw::helloBodyBytes||v.kind!=2||v.flags||!v.payload)return false;
    const auto*p=v.payload;
    if(get(p)||get(p+8)!=160000000U||get(p+12)!=1||get(p+16)!=profile_.expectedCapabilities||std::memcmp(p+20,profile_.elf.data(),32)||get(p+52)||
       get(p+56)!=2||get(p+60)!=160000000U||get(p+80)!=raw::requestPacketBytes||get(p+84)!=raw::slotCount||
       get(p+92)!=raw::replyPacketBytes||get(p+96)!=raw::directionalProfile||!get(p+64)||!get(p+68)||!get(p+72)||!get(p+76))return false;
    hello_={get(p+64),get(p+68),get(p+72),get(p+76),get(p+60)};return true;
}
/** @brief Main ждёт matching RX и TXdone, копирует stable reply до release; clock читается лишь на Complete. */
TransportState RawAsyncAdapter::poll(void*context,ReplyView&out)noexcept{
    if(!context)return TransportState::Failed;
    auto&self=*static_cast<RawAsyncAdapter*>(context);
    if(self.poisoned_||!self.pending_)return TransportState::Failed;
    raw::View view{};if(!self.channel_.poll(self.request_.slot,view))return TransportState::Pending;
    if(!raw::equal(view.key,self.request_)||view.size>raw::payloadBytes){self.poisoned_=true;return TransportState::Failed;}
    const bool unbind=self.request_.opcode==lifecycle::unbindOpcode;
    if(unbind){
        service::IdleUnbindRequest expected{};
        if(!self.warmQualified()||!lifecycle::decodeRequest(self.reply_.data(),lifecycle::requestBytes,expected)||
           !lifecycle::matchesKey(self.request_,expected)||!lifecycle::decodeAck(view.payload,view.size,expected)){
            self.poisoned_=true;return TransportState::Failed;
        }
    }
    const auto total=raw::headerBytes+view.size;
    // Channel владеет полным header+payload и уже проверил CRC; копия остаётся стабильной после release.
    std::memcpy(self.reply_.data(),view.payload-raw::headerBytes,total);raw::View stable{};
    if(!raw::decode(self.reply_.data(),total,stable)||!self.channel_.release(self.request_.slot)){self.poisoned_=true;return TransportState::Failed;}
    self.pending_=false;const auto ended=self.clock_();
    if(ended<self.started_||(self.helloPending_&&!self.verifyHello(stable))){self.poisoned_=true;return TransportState::Failed;}
    if(self.helloPending_)self.qualified_=true;
    self.unbindVerified_=unbind;
    out={stable,ended-self.started_};return TransportState::Complete;
}
/** @brief Повторно сверяет stable cached ACK; новый key не получает чужие OTA/read/write полномочия. */
bool RawAsyncAdapter::canWarmRebind(const raw::Key&nextHello)const noexcept{
    if(!unboundAckVerified()||nextHello.session==profile_.hello.session)return false;
    auto next=profile_;next.hello=nextHello;if(!validProfile(next))return false;
    raw::View reply{};service::IdleUnbindRequest expected{};
    return request_.opcode==lifecycle::unbindOpcode&&raw::decode(reply_.data(),raw::headerBytes+lifecycle::ackBytes,reply)&&
        reply.kind==2&&!reply.flags&&raw::equal(reply.key,request_)&&reply.size==lifecycle::ackBytes&&
        lifecycle::decodeRequest(reply.payload+lifecycle::commonBytes,lifecycle::requestBytes,expected)&&
        lifecycle::matchesKey(request_,expected)&&lifecycle::decodeAck(reply.payload,reply.size,expected);
}
/** @brief Сохраняет image/caps/clock/Channel и все boot-long адреса; новый HELLO ещё не отправляется. */
bool RawAsyncAdapter::reprofileWarm(const raw::Key&nextHello)noexcept{
    if(!canWarmRebind(nextHello))return false;
    profile_.hello=nextHello;request_={};hello_={};started_=0;qualified_=helloPending_=unbindVerified_=false;return true;
}
/** @brief Poison сохраняет pending slot и источник до известного recovery; retry/cancel/release не выполняет. */
void RawAsyncAdapter::noteTimeout()noexcept{
    poisoned_=true;if(pending_)channel_.timeout(request_.slot);
}
} // namespace nes::c6audio::async
