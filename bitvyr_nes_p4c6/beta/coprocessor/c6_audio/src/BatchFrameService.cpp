#include "BatchFrameService.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace nes::c6audio::service {
namespace {
/** @brief Проверяет непустой uintptr extent без переполнения, не проверяя произвольный адрес на доступность. */
bool range(const void*p,std::size_t bytes) noexcept {
    return bytes==0||(p&&bytes<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(p));
}
/** @brief Сверяет два уже ограниченных диапазона без pointer relational UB. */
bool apart(const void*a,std::size_t na,const void*b,std::size_t nb) noexcept {
    if(!na||!nb)return true;
    if(!range(a,na)||!range(b,nb))return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    return x+na<=y||y+nb<=x;
}
/** @brief Пишет числовые metadata/PCM вLE, не копируя native struct padding. */
void put(std::uint8_t*p,std::uint64_t value,unsigned bytes) noexcept {
    for(unsigned i=0;i<bytes;++i)p[i]=std::uint8_t(value>>(8U*i));
}
/** @brief Сравнивает actual source completion со всем исходным Raw key и дополнительным mapping. */
bool same(const Envelope&a,const Envelope&b) noexcept {
    return a.romEpoch==b.romEpoch&&a.inputFrame==b.inputFrame&&raw::equal(a.raw,b.raw);
}
}

/** @brief Заимствует7 непересекающихся extents; invalid backing оставляет service неготовым. */
BatchFrameService::BatchFrameService(std::uint8_t*bytes,std::size_t capacity,const wire::Destination&typed,core::AudioRenderer&renderer) noexcept
    :wire_(bytes),capacity_(capacity),typed_(typed),renderer_(renderer) {
    if(capacity<wire::headerBytes||capacity>maximumWireBytes||!range(bytes,capacity)||!typed.header||
       typed.inputCapacity>core::AudioPacket::inputCapacity||typed.dmcCapacity>core::AudioPacket::dmcCapacity||
       typed.expansionCapacity>core::AudioPacket::expansionCapacity)return;
    const void*p[]={bytes,typed.header,typed.input,typed.dmc,typed.expansion,&renderer,this};
    const std::size_t n[]={capacity,sizeof(*typed.header),typed.inputCapacity*sizeof(*typed.input),
        typed.dmcCapacity*sizeof(*typed.dmc),typed.expansionCapacity*sizeof(*typed.expansion),sizeof(renderer),sizeof(*this)};
    for(unsigned i=0;i<7;++i){if(!range(p[i],n[i]))return;for(unsigned j=0;j<i;++j)if(!apart(p[i],n[i],p[j],n[j]))return;}
    valid_=true;
}

/** @brief Принимает новую binding только из Unbound без удерживаемого source. */
Code BatchFrameService::bindJoined(const Binding&b) noexcept {
    if(!valid_)return Code::Capacity;
    if(state_!=State::Unbound||sourceHeld_)return Code::SourceHeld;
    if(!b.appGeneration||!b.romLease||!b.romEpoch||!b.session||!b.audioGeneration||!b.firstCommandSequence||
       (b.region!=core::Region::Ntsc&&b.region!=core::Region::Pal)||(b.audioCapabilities&~core::AudioPacket::externalDacQ16))return Code::Identity;
    if(b.firstCommandSequence>std::numeric_limits<std::uint32_t>::max()-16384U)return Code::Wrap;
    // После явного abandon тот же transport session требует новой монотонной App/ROM epoch.
    if(binding_.session==b.session&&b.appGeneration<=binding_.appGeneration&&b.romEpoch<=binding_.romEpoch)return Code::Identity;
    renderer_.reset();binding_=b;nextSequence_=b.firstCommandSequence;nextFrame_=audioSequence_=audioBase_=0;audioFrame_=1;
    rendererCalls_=0;timedOut_=false;state_=State::Idle;return Code::Ok;
}

/** @brief Сверяет полный envelope, opcode и текущую sequence без изменения владельца. */
bool BatchFrameService::command(const Envelope&key,Command operation)const noexcept {
    const auto&r=key.raw;const auto&o=owner_.key.raw;
    return key.romEpoch==binding_.romEpoch&&key.inputFrame==owner_.key.inputFrame&&key.inputFrame<=std::numeric_limits<std::uint32_t>::max()&&
        r.generation==binding_.appGeneration&&r.lease==binding_.romLease&&r.session==binding_.session&&
        r.frame==std::uint32_t(key.inputFrame)&&r.region==std::uint8_t(binding_.region)&&r.slot<raw::slotCount&&
        r.begin==o.begin&&r.end==o.end&&r.sequence==nextSequence_&&r.sequence<std::numeric_limits<std::uint32_t>::max()&&
        r.opcode==std::uint16_t(operation);
}

/** @brief Открывает ожидаемый frame после проверки timeline/capacity и сохраняет его ключ. */
Code BatchFrameService::begin(const Begin&request) noexcept {
    if(state_!=State::Idle)return Code::State;
    if(nextFrame_==std::numeric_limits<std::uint32_t>::max())return Code::Wrap;
    if(request.bytes<wire::headerBytes||request.bytes>capacity_)return Code::Capacity;
    const auto&k=request.key;const auto&r=k.raw;const auto&a=request.audio;
    if(k.inputFrame!=nextFrame_||k.inputFrame>std::numeric_limits<std::uint32_t>::max()||k.romEpoch!=binding_.romEpoch||
       r.generation!=binding_.appGeneration||r.lease!=binding_.romLease||r.session!=binding_.session||r.slot>=raw::slotCount||
       r.frame!=std::uint32_t(k.inputFrame)||r.region!=std::uint8_t(binding_.region)||r.sequence!=nextSequence_||
       r.opcode!=std::uint16_t(Command::Begin)||a.generation!=binding_.audioGeneration||a.sequence!=audioSequence_||
       a.ppuFrame!=audioFrame_||a.baseCycle!=audioBase_||a.region!=binding_.region||a.capabilities!=binding_.audioCapabilities||
       a.endCycle<=a.baseCycle||a.endCycle-a.baseCycle>=45000U||r.begin!=0||r.end!=a.endCycle-a.baseCycle)return Code::Identity;
    const auto upperCommands=request.bytes+core::Apu::sampleCapacity+3U;
    if(nextSequence_>std::numeric_limits<std::uint32_t>::max()-upperCommands)return Code::Wrap;
    owner_=request;received_=pcmOffset_=0;result_={};timedOut_=false;++nextSequence_;state_=State::Assembling;return Code::Ok;
}

/** @brief Копирует только следующий проверенный диапазон текущего immutable wire frame. */
Code BatchFrameService::data(const Envelope&key,std::size_t offset,const std::uint8_t*bytes,std::size_t count) noexcept {
    if(state_!=State::Assembling)return Code::State;
    if(!command(key,Command::Data))return Code::Identity;
    if(!count||count>maximumBodyBytes||offset!=received_)return Code::Order;
    if(count>owner_.bytes-received_||!range(bytes,count))return Code::Capacity;
    if(!apart(bytes,count,wire_,capacity_))return Code::Overlap;
    std::memcpy(wire_+received_,bytes,count);received_+=count;++nextSequence_;
    if(received_==owner_.bytes)state_=State::Complete;
    return Code::Ok;
}

/** @brief Проверяет принятый frame и публикует только завершённый PCM render. */
Code BatchFrameService::render(const Envelope&key) noexcept {
    if(state_!=State::Complete)return Code::State;
    if(!command(key,Command::Render))return Code::Identity;
    state_=State::Rendering;
    const bool okay=renderReady(wire_,owner_.bytes,owner_.audio,typed_,renderer_,result_);
    if(result_.generation)++rendererCalls_; // Stateless helper задаёт generation только после успешного decode перед actual render.
    ++nextSequence_;
    if(!okay||result_.samples>core::Apu::sampleCapacity||(!result_.pcm&&result_.samples)){
        state_=State::Rejected;return result_.generation?Code::DspFault:Code::WireRejected;
    }
    pcmOffset_=0;state_=State::Result;return Code::Ok;
}

/** @brief Отвергает пересечение caller output с внутренними заимствованными extents. */
bool BatchFrameService::separateOutput(const void*p,std::size_t n)const noexcept {
    return range(p,n)&&apart(p,n,this,sizeof(*this))&&apart(p,n,wire_,capacity_)&&apart(p,n,&renderer_,sizeof(renderer_))&&
        apart(p,n,typed_.header,sizeof(*typed_.header))&&apart(p,n,typed_.input,typed_.inputCapacity*sizeof(*typed_.input))&&
        apart(p,n,typed_.dmc,typed_.dmcCapacity*sizeof(*typed_.dmc))&&apart(p,n,typed_.expansion,typed_.expansionCapacity*sizeof(*typed_.expansion));
}

/** @brief Выдаёт bounded PCM prefix с прежним offset/key; source остаётся удерживаемым. */
PcmLease BatchFrameService::pcm(const Envelope&key,std::size_t offset,std::size_t wanted,std::uint8_t*output,std::size_t capacity) noexcept {
    PcmLease reply{};
    if(state_!=State::Result){reply.code=Code::State;return reply;}
    if(sourceHeld_){reply.code=Code::SourceHeld;return reply;}
    if(!command(key,Command::Pcm)){reply.code=Code::Identity;return reply;}
    if(offset!=pcmOffset_||!wanted||wanted>maximumPcmChunkSamples||offset>=result_.samples){reply.code=Code::Order;return reply;}
    const auto count=std::min(wanted,result_.samples-offset);const auto bytes=pcmHeaderBytes+count*2U;
    if(capacity<bytes){reply.code=Code::Capacity;return reply;}
    if(!separateOutput(output,bytes)){reply.code=Code::Overlap;return reply;}
    std::memset(output,0,pcmHeaderBytes);put(output,1,2);put(output+2,count,2);
    put(output+4,key.romEpoch,8);put(output+12,key.inputFrame,8);put(output+20,result_.samples,4);put(output+24,offset,4);
    put(output+28,offset+count==result_.samples?1U:0U,4);
    for(std::size_t i=0;i<count;++i)put(output+pcmHeaderBytes+i*2U,std::uint16_t(result_.pcm[offset+i]),2);
    source_=key;sourceHeld_=true;pcmOffset_+=count;++nextSequence_;
    reply={Code::Ok,key,output,bytes,std::uint32_t(offset),std::uint32_t(count),std::uint32_t(result_.samples)};return reply;
}

/** @brief Подтверждает release только совпавшего frame; сама команда не является внешним join. */
Code BatchFrameService::sourceReleased(const Envelope&key) noexcept {
    if(!sourceHeld_)return Code::State;
    if(!same(key,source_))return Code::Identity;
    sourceHeld_=false;return Code::Ok;
}

/** @brief Закрывает frame после прежнего полного consumption/release без пропуска sequence. */
Code BatchFrameService::consumed(const Envelope&key) noexcept {
    if(state_!=State::Result)return Code::State;
    if(sourceHeld_)return Code::SourceHeld;
    if(!command(key,Command::Consumed))return Code::Identity;
    if(pcmOffset_!=result_.samples)return Code::Order;
    if(nextFrame_==std::numeric_limits<std::uint32_t>::max()||audioSequence_==std::numeric_limits<std::uint64_t>::max()||audioFrame_==std::numeric_limits<std::uint64_t>::max())return Code::Wrap;
    ++nextSequence_;++nextFrame_;++audioSequence_;++audioFrame_;audioBase_=owner_.audio.endCycle;
    result_={};owner_={};received_=pcmOffset_=0;state_=State::Idle;return Code::Ok;
}

/** @details Все проверки находятся внутри уже удерживаемого admission; отказ не меняет ни один owner/DSP byte. */
Code BatchFrameService::unbindIdle(const IdleUnbindRequest&r,const IdleAdmissionLease&admission) noexcept {
    if(!admission.held())return Code::SourceHeld;
    if(!valid_)return Code::Capacity;
    if(sourceHeld_)return Code::SourceHeld;
    if(timedOut_||state_!=State::Idle)return Code::State;
    if(!sameIdleBinding(r.expectedBinding,binding_))return Code::Identity;
    if(r.nextCommandSequence!=nextSequence_||r.completedFrames!=nextFrame_||
       r.audioSequence!=audioSequence_||r.nextPpuFrame!=audioFrame_||r.finalAudioEndCycle!=audioBase_)return Code::Order;
    // Никто не читает PCM/typed wire; новый admission закрыт до control commit.
    // Binding/cursor сохраняются как история для прежнего bindJoined monotonic guard.
    renderer_.reset();state_=State::Unbound;owner_={};result_={};source_={};received_=pcmOffset_=0;
    return Code::Ok;
}

/** @brief Освобождает binding только по существующему joined предусловию, не по timeout. */
Code BatchFrameService::abandonJoined() noexcept {
    if(!valid_)return Code::Capacity;
    if(sourceHeld_||state_==State::Rendering)return Code::SourceHeld;
    renderer_.reset();state_=State::Unbound;owner_={};result_={};received_=pcmOffset_=0;timedOut_=false;return Code::Ok;
}

/** @brief Читает Main/service-owned поля без изменения state и без нового acquire права. */
Snapshot BatchFrameService::snapshot()const noexcept {
    return {state_,received_,owner_.bytes,pcmOffset_,state_==State::Result?result_.samples:0,nextSequence_,rendererCalls_,sourceHeld_,timedOut_,valid_,result_.status};
}

namespace {
/** @brief Читает ограниченные LE поля prefix/Header80 после проверки общей длины. */
std::uint64_t batchGet(const std::uint8_t*p,unsigned count) noexcept {
    std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t(p[i])<<(8U*i);return value;
}
/** @brief Сверяет immutable frame identity, оставляя командные opcode/sequence/slot отдельными. */
bool batchSameFrame(const Envelope&a,const Envelope&b) noexcept {
    const auto&x=a.raw;const auto&y=b.raw;
    return a.romEpoch==b.romEpoch&&a.inputFrame==b.inputFrame&&x.generation==y.generation&&x.lease==y.lease&&
        x.session==y.session&&x.frame==y.frame&&x.begin==y.begin&&x.end==y.end&&x.region==y.region;
}
}

PcmLease BatchFrameService::emitFast(const Envelope&key,std::size_t offset,std::size_t wanted,
    std::uint8_t*output,std::size_t capacity,bool previous) noexcept {
    PcmLease reply{};
    if(state_!=State::Result){reply.code=Code::State;return reply;}
    if(sourceHeld_){reply.code=Code::SourceHeld;return reply;}
    const auto opcode=key.raw.opcode;
    if((opcode!=batch::submitOpcode&&opcode!=batch::fetchOpcode)||!command(key,static_cast<Command>(opcode))){reply.code=Code::Identity;return reply;}
    const bool zeroFirst=opcode==batch::submitOpcode&&result_.samples==0&&offset==0;
    if(offset!=pcmOffset_||!wanted||wanted>maximumFastSamples||(!zeroFirst&&offset>=result_.samples)){
        reply.code=Code::Order;return reply;
    }
    const auto count=std::min(wanted,result_.samples-offset);const auto bytes=pcmHeaderBytes+count*2U;
    if(capacity<bytes){reply.code=Code::Capacity;return reply;}
    if(!separateOutput(output,bytes)||!apart(output,bytes,&key,sizeof(key))){reply.code=Code::Overlap;return reply;}
    std::memset(output,0,pcmHeaderBytes);put(output,1,2);put(output+2,count,2);
    put(output+4,key.romEpoch,8);put(output+12,key.inputFrame,8);put(output+20,result_.samples,4);put(output+24,offset,4);
    const auto flags=(offset+count==result_.samples?batch::lastPcmFlag:0U)|(previous?batch::previousConsumedAppliedFlag:0U);
    put(output+28,flags,4);
    for(std::size_t i=0;i<count;++i)put(output+pcmHeaderBytes+i*2U,std::uint16_t(result_.pcm[offset+i]),2);
    source_=key;sourceHeld_=true;pcmOffset_+=count;++nextSequence_;
    reply={Code::Ok,key,output,bytes,std::uint32_t(offset),std::uint32_t(count),std::uint32_t(result_.samples)};return reply;
}

PcmLease BatchFrameService::submit(const raw::Key&key,const std::uint8_t*body,std::size_t bytes,
    std::uint8_t*output,std::size_t capacity) noexcept {
    PcmLease reply{};
    if(!valid_){reply.code=Code::Capacity;return reply;}
    if(state_!=State::Idle&&state_!=State::Result){reply.code=Code::State;return reply;}
    if(sourceHeld_){reply.code=Code::SourceHeld;return reply;}
    if(timedOut_){reply.code=Code::State;return reply;}
    if(bytes<batch::submitPrefixBytes+wire::headerBytes||bytes>batch::submitPrefixBytes+maximumSubmitWireBytes||!range(body,bytes)){
        reply.code=Code::Capacity;return reply;
    }
    if(capacity<maximumFastBodyBytes){reply.code=Code::Capacity;return reply;}
    if(!separateOutput(body,bytes)||!separateOutput(&key,sizeof(key))||!separateOutput(output,maximumFastBodyBytes)||
       !apart(output,maximumFastBodyBytes,body,bytes)||!apart(output,maximumFastBodyBytes,&key,sizeof(key))){reply.code=Code::Overlap;return reply;}
    const auto flags=std::uint16_t(batchGet(body+2,2));const auto wireBytes=std::size_t(batchGet(body+4,2));
    const auto epoch=batchGet(body+8,8);const auto previousSequence=std::uint32_t(batchGet(body+16,4));
    if(batchGet(body,2)!=1||flags&~batch::previousConsumedFlag||batchGet(body+6,2)||
       wireBytes<wire::headerBytes||wireBytes>maximumSubmitWireBytes||bytes!=batch::submitPrefixBytes+wireBytes){reply.code=Code::WireRejected;return reply;}
    const bool previous=(flags&batch::previousConsumedFlag)!=0;
    if(state_==State::Idle){if(previous||previousSequence){reply.code=Code::Order;return reply;}}
    else{
        if(!previous||pcmOffset_!=result_.samples){reply.code=Code::Order;return reply;}
        const auto oldOpcode=source_.raw.opcode;
        if(!previousSequence||source_.raw.sequence!=previousSequence||
           (oldOpcode!=std::uint16_t(Command::Pcm)&&oldOpcode!=batch::submitOpcode&&oldOpcode!=batch::fetchOpcode)||
           !batchSameFrame(source_,owner_.key)||previousSequence==std::numeric_limits<std::uint32_t>::max()||
           nextSequence_!=previousSequence+1U){reply.code=Code::Identity;return reply;}
    }
    const auto advance=previous?1U:0U;
    if(nextFrame_>=std::numeric_limits<std::uint32_t>::max()-advance||
       audioSequence_>std::numeric_limits<std::uint64_t>::max()-advance||audioFrame_>std::numeric_limits<std::uint64_t>::max()-advance||
       nextSequence_>std::numeric_limits<std::uint32_t>::max()-(wireBytes+core::Apu::sampleCapacity+3U)){
        reply.code=Code::Wrap;return reply;
    }
    const auto newFrame=nextFrame_+advance,newAudioSequence=audioSequence_+advance,newAudioFrame=audioFrame_+advance;
    const auto base=previous?owner_.audio.endCycle:audioBase_;
    const auto*input=body+batch::submitPrefixBytes;const auto end=batchGet(input+48,8);
    if(epoch!=binding_.romEpoch||key.generation!=binding_.appGeneration||key.lease!=binding_.romLease||
       key.session!=binding_.session||key.frame!=newFrame||key.slot>=raw::slotCount||
       key.region!=std::uint8_t(binding_.region)||key.opcode!=batch::submitOpcode||key.sequence!=nextSequence_||
       end<=base||end-base>=45000U||key.begin!=0||key.end!=end-base){reply.code=Code::Identity;return reply;}
    const wire::Expected expected{binding_.audioGeneration,newAudioSequence,newAudioFrame,base,end,binding_.region,binding_.audioCapabilities};
    if(wireBytes>capacity_||!wire::decode(input,wireBytes,expected,typed_)){reply.code=Code::WireRejected;return reply;}
    // Все admission отказы закончены. Typed arrays validated; старый PCM уже явно consumed клиентом.
    if(previous){nextFrame_=newFrame;audioSequence_=newAudioSequence;audioFrame_=newAudioFrame;audioBase_=base;}
    const Envelope envelope{key,epoch,newFrame};owner_={envelope,expected,wireBytes};
    std::memcpy(wire_,input,wireBytes);received_=wireBytes;pcmOffset_=0;result_={};timedOut_=false;state_=State::Rendering;
    const bool okay=renderReady(wire_,owner_.bytes,owner_.audio,typed_,renderer_,result_);
    if(result_.generation)++rendererCalls_;
    if(!okay||result_.samples>core::Apu::sampleCapacity||(!result_.pcm&&result_.samples)){
        ++nextSequence_;state_=State::Rejected;reply.code=result_.generation?Code::DspFault:Code::WireRejected;return reply;
    }
    state_=State::Result;
    return emitFast(envelope,0,maximumFastSamples,output,capacity,previous);
}

PcmLease BatchFrameService::fetch(const Envelope&key,std::size_t offset,std::size_t wanted,
    std::uint8_t*output,std::size_t capacity) noexcept {
    if(key.raw.opcode!=batch::fetchOpcode){PcmLease reply{};reply.code=Code::Identity;return reply;}
    return emitFast(key,offset,wanted,output,capacity,false);
}

} // namespace nes::c6audio::service
