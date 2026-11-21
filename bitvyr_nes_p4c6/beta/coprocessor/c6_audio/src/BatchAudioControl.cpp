#include "BatchAudioControl.h"
#include <new>
#include <limits>

namespace nes::c6audio::adapter {namespace {
/** @brief Читает ограниченное LE поле после проверки общей длины команды. */
std::uint64_t get(const std::uint8_t*p,unsigned n) noexcept {std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8U*i);return v;}
/** @brief Пишет числовые metadata без требований к выравниванию host/C6. */
void put(std::uint8_t*p,std::uint64_t v,unsigned n) noexcept {for(unsigned i=0;i<n;++i)p[i]=std::uint8_t(v>>(8U*i));}
/** @brief Проверяет caller extents до INIT/DTO мутаций, не проверяя физическую доступность. */
bool apart(const void*a,std::size_t an,const void*b,std::size_t bn) noexcept {
    if(!an||!bn)return true;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    if(!a||!b||an>std::numeric_limits<std::uintptr_t>::max()-x||bn>std::numeric_limits<std::uintptr_t>::max()-y)return false;
    return x+an<=y||y+bn<=x;
}
/** @brief Проверяет version/reserved; key остаётся отдельным immutable Raw identity. */
bool envelope(const raw::Key&key,const std::uint8_t*p,std::size_t n,service::Envelope&out) noexcept {
    if(!p||n<20||get(p,2)!=1||get(p+2,2))return false;
    out={key,get(p+4,8),get(p+12,8)};return true;
}
}
core::AudioRenderer*BatchAudioControl::renderer() noexcept {return std::launder(reinterpret_cast<core::AudioRenderer*>(rendererStorage_.data()));}
service::BatchFrameService*BatchAudioControl::service() noexcept {return std::launder(reinterpret_cast<service::BatchFrameService*>(serviceStorage_.data()));}
/** @brief Проверяет session до HELLO; не сбрасывает уже связанный renderer. */
bool BatchAudioControl::permitsHello(std::uint64_t session)const noexcept {return !bound_||session==session_;}
/** @brief Передаёт full-key release service; не подменяет его локальным finished. */
bool BatchAudioControl::sourceReleased(const service::Envelope&key) noexcept {
    if(!initialized_||fault_||service()->sourceReleased(key)!=service::Code::Ok){fault_=true;return false;}return true;
}
/** @brief Кодирует bounded snapshot владельца в caller buffer; capacity измеряется в байтах. */
std::size_t BatchAudioControl::snapshot(std::uint8_t*out,std::size_t capacity) noexcept {
    if(capacity<40)return 0;
    const auto s=service()->snapshot();
    put(out,1,2);out[2]=std::uint8_t(s.state);out[3]=std::uint8_t(s.sourceHeld)|(std::uint8_t(s.timedOut)<<1U)|(std::uint8_t(s.storageValid)<<2U);
    put(out+4,s.nextSequence,4);put(out+8,s.rendererCalls,4);put(out+12,s.received,4);put(out+16,s.expected,4);
    put(out+20,s.pcmOffset,4);put(out+24,s.samples,4);put(out+28,std::uint8_t(s.rendererStatus),4);
    put(out+32,assemblyCycles_,4);put(out+36,renderCycles_,4);return 40;
}
/** @details Lease уничтожается после bound_=false; admission нельзя открыть между service reset и control commit. */
service::Code BatchAudioControl::unbindIdle(const service::IdleUnbindRequest&r,const service::IdleAdmissionBarrier&barrier) noexcept {
    if(!initialized_||!bound_||fault_)return service::Code::State;
    if(r.expectedBinding.session!=session_)return service::Code::Identity;
    const auto admission=service::IdleAdmissionLease::acquire(barrier);
    if(!admission.held())return service::Code::SourceHeld;
    const auto code=service()->unbindIdle(r,admission);
    if(code==service::Code::Ok){
        // Query14/ACK нового Unbound потока не переносит diagnostic tail старого кадра.
        assemblyCycles_=renderCycles_=0;bound_=false;
    }
    return code;
}

/** @brief Разбирает одну bounded команду и сохраняет прежние state/key/fault проверки. */
Reply BatchAudioControl::process(const raw::Key&key,const std::uint8_t*p,std::size_t n,std::uint8_t*out,std::size_t cap,Query clock,Query stack) noexcept {
    Reply reply{};reply.status=100;
    if(fault_||!out||!clock||!stack||(!p&&n)||n>1336||cap>1696||
       !apart(out,cap,this,sizeof(*this))||!apart(p,n,this,sizeof(*this))||!apart(out,cap,p,n))return reply;
    if(key.opcode==12){
        if(initialized_||n!=4||get(p,4)!=1||key.frame||key.sequence||key.begin||key.end||key.region>1||cap<72)return reply;
        const auto beforeStack=stack(),begin=clock();
        ::new(static_cast<void*>(rendererStorage_.data()))core::AudioRenderer;
        const auto constructed=clock();std::uint32_t tables[6]{};renderer()->diagnosticTableCrcs(tables);
        const auto crcEnd=clock();::new(static_cast<void*>(serviceStorage_.data()))service::BatchFrameService(wire_.data(),wire_.size(),typed_.destination(),*renderer());
        initialized_=true;
        put(out,1,4);put(out+4,constructed-begin,4);put(out+8,crcEnd-constructed,4);
        put(out+12,beforeStack,4);put(out+16,stack(),4);put(out+20,sizeof(core::AudioRenderer),4);
        put(out+24,sizeof(service::BatchFrameService),4);put(out+28,sizeof(typed_),4);put(out+32,wire_.size(),4);
        for(unsigned i=0;i<6;++i)put(out+36+4*i,tables[i],4);
        put(out+60,sizeof(*this),4);put(out+64,core::Apu::sampleCapacity,4);put(out+68,core::Apu::sampleRate,4);
        reply.status=0;reply.bytes=72;return reply;
    }
    if(!initialized_)return reply;
    if(key.opcode==14&&n==0&&cap>=40&&(!bound_||key.session==session_)){
        reply.status=0;reply.bytes=snapshot(out,cap);return reply;
    }
    if(key.opcode==13){
        if(bound_||n!=32||p[0]!=1||p[1]>1||p[1]!=key.region||get(p+24,8)||key.frame||key.sequence||key.begin||key.end||cap<40)return reply;
        const service::Binding b{key.generation,key.lease,get(p+4,8),key.session,get(p+12,8),std::uint32_t(get(p+20,4)),core::Region(p[1]),std::uint16_t(get(p+2,2))};
        const auto code=service()->bindJoined(b);reply.status=code==service::Code::Ok?0U:100U+std::uint32_t(code);
        if(!reply.status){bound_=true;session_=key.session;reply.bytes=snapshot(out,cap);}return reply;
    }
    if(key.opcode==batch::submitOpcode){
        if(!bound_||key.session!=session_)return reply;
        const auto started=clock();const auto lease=service()->submit(key,p,n,out,cap);reply.operationCycles=clock()-started;
        reply.status=lease.code==service::Code::Ok?0U:100U+std::uint32_t(lease.code);
        if(!reply.status){assemblyCycles_=0;renderCycles_=reply.operationCycles;reply.bytes=lease.bytes;reply.pcmSource=true;reply.source=lease.key;}
        return reply;
    }
    service::Envelope e{};if(!bound_||key.session!=session_||!envelope(key,p,n,e)||cap<40)return reply;
    service::Code code=service::Code::Identity;
    if(key.opcode==std::uint16_t(service::Command::Begin)&&n==72){
        const auto a=p+20;if(a[40]>1||a[41]||get(a+44,4))return reply;
        const wire::Expected expected{get(a,8),get(a+8,8),get(a+16,8),get(a+24,8),get(a+32,8),core::Region(a[40]),std::uint16_t(get(a+42,2))};
        code=service()->begin({e,expected,std::size_t(get(p+68,4))});if(code==service::Code::Ok)assemblyCycles_=renderCycles_=0;
    }else if(key.opcode==std::uint16_t(service::Command::Data)&&n>=28&&get(p+26,2)==0&&n==28+get(p+24,2)){
        const auto started=clock();code=service()->data(e,std::size_t(get(p+20,4)),p+28,std::size_t(get(p+24,2)));
        reply.operationCycles=clock()-started;if(code==service::Code::Ok)assemblyCycles_+=reply.operationCycles;
    }else if(key.opcode==std::uint16_t(service::Command::Render)&&n==20){
        const auto started=clock();code=service()->render(e);reply.operationCycles=clock()-started;renderCycles_=reply.operationCycles;
    }else if(key.opcode==std::uint16_t(service::Command::Pcm)&&n==28&&get(p+26,2)==0){
        const auto lease=service()->pcm(e,std::size_t(get(p+20,4)),std::size_t(get(p+24,2)),out,cap);
        reply.status=lease.code==service::Code::Ok?0U:100U+std::uint32_t(lease.code);
        if(!reply.status){reply.bytes=lease.bytes;reply.pcmSource=true;reply.source=lease.key;}return reply;
    }else if(key.opcode==batch::fetchOpcode&&n==28&&get(p+26,2)==0){
        const auto lease=service()->fetch(e,std::size_t(get(p+20,4)),std::size_t(get(p+24,2)),out,cap);
        reply.status=lease.code==service::Code::Ok?0U:100U+std::uint32_t(lease.code);
        if(!reply.status){reply.bytes=lease.bytes;reply.pcmSource=true;reply.source=lease.key;}return reply;
    }else if(key.opcode==std::uint16_t(service::Command::Consumed)&&n==20){
        code=service()->consumed(e);
    }
    reply.status=code==service::Code::Ok?0U:100U+std::uint32_t(code);if(!reply.status)reply.bytes=snapshot(out,cap);return reply;
}
} // namespace nes::c6audio::adapter
