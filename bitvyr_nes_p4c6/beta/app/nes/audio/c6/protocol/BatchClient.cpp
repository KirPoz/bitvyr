#include "BatchClient.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace nes::c6audio::async {namespace {
/** @brief Читает bounded LE поле без alignment предположений. */
std::uint64_t get(const std::uint8_t*p,unsigned n)noexcept{std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8U*i);return v;}
/** @brief Пишет bounded LE поле без копирования native padding. */
void put(std::uint8_t*p,std::uint64_t v,unsigned n)noexcept{for(unsigned i=0;i<n;++i)p[i]=std::uint8_t(v>>(8U*i));}
/** @brief Проверяет uintptr extent; доступность памяти обеспечивает caller. */
bool range(const void*p,std::size_t n)noexcept{return !n||(p&&n<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(p));}
/** @brief Проверяет непересечение независимых extents без pointer-order UB. */
bool apart(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
    if(!an||!bn)return true;
    if(!range(a,an)||!range(b,bn))return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);return x+an<=y||y+bn<=x;
}
/** @brief Отказывает до переполнения telemetry64. */
bool add(std::uint64_t&v,std::uint64_t n)noexcept{if(v>std::numeric_limits<std::uint64_t>::max()-n)return false;v+=n;return true;}
/** @brief Восстанавливает PCM16 без implementation-defined unsigned narrowing. */
std::int16_t signed16(const std::uint8_t*p)noexcept{const auto v=get(p,2);return v<32768U?std::int16_t(v):std::int16_t(-1-std::int32_t(65535U-v));}
}
/** @brief Сохраняет первую ошибку и Poisoned; чужую память не освобождает и remote cancel не отправляет. */
bool BatchClient::fail(Error e)noexcept{if(error_==Error::None)error_=e;phase_=Phase::Poisoned;return false;}
/** @brief Строит полный wire key immutable binding; frame/duration уже ограничены вызывающим методом. */
raw::Key BatchClient::key(std::uint16_t op,std::uint32_t seq,std::uint64_t frame,std::uint32_t duration)const noexcept{
    return {binding_.appGeneration,binding_.romLease,binding_.session,std::uint32_t(frame),seq,0,duration,op,slot_,std::uint8_t(binding_.region)};
}
/** @brief Однократный start защищён от reentry и переполнения счётчика; request хранится до completion. */
bool BatchClient::start(raw::Key k,std::size_t n)noexcept{
    if(pending_||inside_||error_!=Error::None||!start_||!poll_||n>body_.size()||stats_.calls==std::numeric_limits<std::uint32_t>::max())return fail(Error::State);
    request_=k;++stats_.calls;inside_=true;const bool accepted=start_(context_,request_,body_.data(),n);inside_=false;
    if(error_!=Error::None)return false;
    if(!accepted)return fail(Error::Exchange);
    pending_=true;return true;
}
/** @brief До чтения body проверяет reply key/extent/alias/status; telemetry складывается с overflow guard. */
bool BatchClient::common(const ReplyView&r)noexcept{
    const auto&v=r.raw;
    if(!raw::equal(request_,v.key)||v.kind!=2||v.flags||v.size<12||v.size>raw::payloadBytes||!range(v.payload,v.size)||
       !apart(v.payload,v.size,this,sizeof(*this))||(!inputReleased_&&!apart(v.payload,v.size,frame_.wire,frame_.bytes))||
       !apart(v.payload,v.size,pcm_,capacity_*sizeof(*pcm_)))return fail(Error::Reply);
    if(!add(stats_.hostElapsedUs,r.host_elapsed_us))return fail(Error::Overflow);
    stats_.remoteStatus=std::uint32_t(get(v.payload,4));if(stats_.remoteStatus)return fail(Error::Remote);
    if(v.size<32||!get(v.payload+8,4)||(cold_.serverHz&&get(v.payload+8,4)!=cold_.serverHz))return fail(Error::Reply);
    if(!add(stats_.serverCycles,get(v.payload+4,4))||!add(stats_.rxCycles,get(v.payload+12,4))||
       !add(stats_.operationCycles,get(v.payload+16,4))||!add(stats_.previousEncodeCycles,get(v.payload+20,4))||
       !add(stats_.previousQueueCycles,get(v.payload+24,4))||!add(stats_.previousSourceCycles,get(v.payload+28,4)))return fail(Error::Overflow);
    return true;
}
/** @brief Готовит version/ROMepoch/input envelope в собственном массиве; native padding не передаёт. */
void BatchClient::envelope(std::uint64_t input)noexcept{std::fill_n(body_.data(),20,0);put(body_.data(),1,2);put(body_.data()+4,binding_.romEpoch,8);put(body_.data()+12,input,8);}
/** @brief Сверяет exact72-byte service snapshot с ожидаемыми state/sequence/extents и renderer result. */
bool BatchClient::snapshot(const ReplyView&r,service::State state,std::size_t received,std::size_t expected,std::size_t offset,std::size_t samples,std::uint32_t calls)noexcept{
    if(r.raw.size!=72)return fail(Error::Snapshot);
    const auto*p=r.raw.payload+32;
    if(get(p,2)!=1||p[2]!=std::uint8_t(state)||p[3]!=4||get(p+4,4)!=sequence_||get(p+8,4)!=calls||get(p+12,4)!=received||
       get(p+16,4)!=expected||get(p+20,4)!=offset||get(p+24,4)!=samples||
       get(p+28,4)!=std::uint32_t(state==service::State::Result?core::AudioRenderResult::Ok:core::AudioRenderResult::InvalidPacket))return fail(Error::Snapshot);
    return true;
}
/** @brief Разрешает INIT лишь Empty owner с согласованным binding/backend; источники не заимствует заранее. */
bool BatchClient::beginCold(const service::Binding&b)noexcept{
    if(inside_||pending_||phase_!=Phase::Empty||error_!=Error::None)return fail(Error::State);
    if((backend_!=Backend::Legacy&&backend_!=Backend::Batch)||!b.appGeneration||!b.romLease||!b.romEpoch||!b.session||!b.audioGeneration||b.firstCommandSequence!=1||
       (b.region!=core::Region::Ntsc&&b.region!=core::Region::Pal)||(b.audioCapabilities&~core::AudioPacket::externalDacQ16))return fail(Error::Input);
    binding_=b;completion_=Completion::None;phase_=Phase::Init;return prepare();
}
/** @brief После cold INIT переводит client в BIND; pending/reentry закрывает дальнейшую работу. */
bool BatchClient::beginBind()noexcept{
    if(inside_||pending_||phase_!=Phase::Cold||error_!=Error::None)return fail(Error::State);
    completion_=Completion::None;phase_=Phase::Bind;return prepare();
}
/** @brief Проверяет следующий full key и disjoint caller wire/PCM; borrow хранится до actual input release. */
bool BatchClient::beginFrame(const FrameInput&f,std::int16_t*pcm,std::size_t capacity)noexcept{
    if(inside_||!bound())return fail(Error::State);
    const auto&a=f.audio;
    if(f.inputFrame!=nextInput_||f.inputFrame>=std::numeric_limits<std::uint32_t>::max()||a.generation!=binding_.audioGeneration||
       a.sequence!=nextAudio_||a.ppuFrame!=nextPpu_||a.baseCycle!=nextBase_||a.endCycle<=a.baseCycle||a.endCycle-a.baseCycle>=45000||
       a.region!=binding_.region||a.capabilities!=binding_.audioCapabilities||f.bytes<wire::headerBytes||f.bytes>service::FrameService::maximumWireBytes||
       !range(f.wire,f.bytes)||!capacity||capacity>core::Apu::sampleCapacity||capacity>std::numeric_limits<std::size_t>::max()/sizeof(*pcm)||!range(pcm,capacity*sizeof(*pcm))||reinterpret_cast<std::uintptr_t>(pcm)%alignof(std::int16_t)||
       !apart(f.wire,f.bytes,this,sizeof(*this))||!apart(pcm,capacity*sizeof(*pcm),this,sizeof(*this))||
       !apart(f.wire,f.bytes,pcm,capacity*sizeof(*pcm))||!apart(&f,sizeof(f),this,sizeof(*this))||!apart(&f,sizeof(f),pcm,capacity*sizeof(*pcm)))return fail(Error::Input);
    if(sequence_>std::numeric_limits<std::uint32_t>::max()-(f.bytes+core::Apu::sampleCapacity+3U)||
       stats_.rendererCalls==std::numeric_limits<std::uint32_t>::max()||stats_.frames==std::numeric_limits<std::uint32_t>::max()||
       nextAudio_==std::numeric_limits<std::uint64_t>::max()||nextPpu_==std::numeric_limits<std::uint64_t>::max())return fail(Error::Overflow);
    frame_=f;pcm_=pcm;capacity_=capacity;received_=offset_=sampleCount_=requestCount_=0;inputReleased_=false;
    completion_=Completion::None;fastFrame_=backend_==Backend::Batch&&f.bytes<=maximumSubmitWireBytes;
    submitAcknowledgesPrevious_=remoteHeld_&&fastFrame_;
    phase_=remoteHeld_&&!fastFrame_?Phase::PriorConsumed:(fastFrame_?Phase::Submit:Phase::Begin);
    return prepare();
}
/** @brief Подтверждает последний remote Result; локальный PCM ready сам не означает remote Idle. */
bool BatchClient::beginDrain()noexcept{
    if(inside_||!bound())return fail(Error::State);
    completion_=Completion::None;
    if(!remoteHeld_){completion_=Completion::Drain;return true;}
    phase_=Phase::FinalConsumed;return prepare();
}
/** @details Старые immutable keys/cursors остаются действительными до точного ACK; false не пишет caller output. */
bool BatchClient::idleUnbindRequest(service::IdleUnbindRequest&out)const noexcept{
    if(inside_||!remoteIdle()||pcm_||capacity_||frame_.wire||nextInput_!=stats_.frames||nextAudio_!=nextInput_||nextPpu_!=nextInput_+1U)return false;
    const service::IdleUnbindRequest value{binding_,nextInput_,nextAudio_,nextPpu_,nextBase_,sequence_};
    if(!lifecycle::valid(value))return false;
    out=value;return true;
}
/** @details Packet использует прежний body_; отказ до start не отменяет ни одного старого владельца. */
bool BatchClient::beginUnbind(const service::IdleUnbindRequest&expected)noexcept{
    service::IdleUnbindRequest actual{};
    if(!idleUnbindRequest(actual)||!lifecycle::same(actual,expected)||!lifecycle::encodeRequest(actual,body_.data(),body_.size()))return false;
    completion_=Completion::None;phase_=Phase::Unbind;
    return start(lifecycle::key(actual,slot_),lifecycle::requestBytes);
}
/** @details Все guards предшествуют сбросу stream metadata; повторный INIT и сброс sticky fault запрещены. */
bool BatchClient::rebindEmpty(const service::Binding&next)noexcept{
    if(inside_||!unbound()||remoteHeld_||pcm_||capacity_||frame_.wire||!lifecycle::validBinding(next)||
       next.appGeneration<=binding_.appGeneration||next.romLease==binding_.romLease||next.romEpoch<=binding_.romEpoch||
       next.session==binding_.session||next.audioGeneration==binding_.audioGeneration)return false;
    binding_=next;frame_={};previous_={};pcm_=nullptr;capacity_=received_=offset_=sampleCount_=requestCount_=0;
    nextInput_=nextAudio_=nextBase_=0;nextPpu_=1;sequence_=next.firstCommandSequence;slot_=0;request_={};stats_={};
    inputReleased_=remoteHeld_=fastFrame_=submitAcknowledgesPrevious_=false;completion_=Completion::None;phase_=Phase::Cold;return true;
}
/** @brief Формирует один bounded запрос текущей Legacy/Batch фазы без второго RPC внутри шага. */
bool BatchClient::prepare()noexcept{
    if(error_!=Error::None)return false;
    if(phase_==Phase::Init){put(body_.data(),1,4);return start(key(12,0,0,0),4);}
    if(phase_==Phase::Bind){std::fill_n(body_.data(),32,0);body_[0]=1;body_[1]=std::uint8_t(binding_.region);put(body_.data()+2,binding_.audioCapabilities,2);
        put(body_.data()+4,binding_.romEpoch,8);put(body_.data()+12,binding_.audioGeneration,8);put(body_.data()+20,1,4);return start(key(13,0,0,0),32);}
    if(phase_==Phase::PriorConsumed||phase_==Phase::FinalConsumed){envelope(previous_.inputFrame);
        return start(key(0x44,sequence_,previous_.inputFrame,std::uint32_t(previous_.audio.endCycle-previous_.audio.baseCycle)),20);}
    const auto duration=std::uint32_t(frame_.audio.endCycle-frame_.audio.baseCycle);
    if(phase_==Phase::Submit){std::fill_n(body_.data(),20,0);put(body_.data(),1,2);put(body_.data()+2,submitAcknowledgesPrevious_?1U:0U,2);
        put(body_.data()+4,frame_.bytes,2);put(body_.data()+8,binding_.romEpoch,8);put(body_.data()+16,submitAcknowledgesPrevious_?previous_.lastPcmSequence:0U,4);
        std::memcpy(body_.data()+submitPrefixBytes,frame_.wire,frame_.bytes);return start(key(batch::submitOpcode,sequence_,frame_.inputFrame,duration),submitPrefixBytes+frame_.bytes);}
    envelope(frame_.inputFrame);
    if(phase_==Phase::Begin){const auto&a=frame_.audio;std::fill_n(body_.data()+20,52,0);put(body_.data()+20,a.generation,8);put(body_.data()+28,a.sequence,8);
        put(body_.data()+36,a.ppuFrame,8);put(body_.data()+44,a.baseCycle,8);put(body_.data()+52,a.endCycle,8);body_[60]=std::uint8_t(a.region);
        put(body_.data()+62,a.capabilities,2);put(body_.data()+68,frame_.bytes,4);return start(key(0x40,sequence_,frame_.inputFrame,duration),72);}
    if(phase_==Phase::Data){requestCount_=std::min<std::size_t>(service::FrameService::maximumBodyBytes,frame_.bytes-received_);
        put(body_.data()+20,received_,4);put(body_.data()+24,requestCount_,2);put(body_.data()+26,0,2);std::memcpy(body_.data()+28,frame_.wire+received_,requestCount_);
        return start(key(0x41,sequence_,frame_.inputFrame,duration),28+requestCount_);}
    if(phase_==Phase::Render)return start(key(0x42,sequence_,frame_.inputFrame,duration),20);
    if(phase_==Phase::Pcm||phase_==Phase::Fetch){const auto maximum=phase_==Phase::Pcm?service::FrameService::maximumPcmChunkSamples:maximumBatchPcmSamples;
        requestCount_=std::min<std::size_t>(maximum,sampleCount_-offset_);put(body_.data()+20,offset_,4);put(body_.data()+24,requestCount_,2);put(body_.data()+26,0,2);
        return start(key(phase_==Phase::Pcm?0x43:batch::fetchOpcode,sequence_,frame_.inputFrame,duration),28);}
    if(phase_==Phase::Consumed)return start(key(0x44,sequence_,frame_.inputFrame,duration),20);
    return fail(Error::State);
}
/** @brief Проверяет полный PCM/count и обновляет sequence после всех chunks; remote held учитывается отдельно. */
bool BatchClient::finishPcm()noexcept{
    if(stats_.frames==std::numeric_limits<std::uint32_t>::max())return fail(Error::Overflow);
    previous_={frame_.audio,frame_.inputFrame,request_.sequence};remoteHeld_=true;
    nextInput_=frame_.inputFrame+1U;nextAudio_=frame_.audio.sequence+1U;nextPpu_=frame_.audio.ppuFrame+1U;nextBase_=frame_.audio.endCycle;
    ++stats_.frames;completion_=Completion::Frame;pcm_=nullptr;capacity_=0;
    phase_=fastFrame_?Phase::Bound:Phase::Consumed;return true;
}
/** @brief Проверяет extents/offset до записи caller span и восстанавливает signed16 из LE без narrowing UB. */
bool BatchClient::acceptPcm(const ReplyView&r,bool firstBatch)noexcept{
    if(r.raw.size<64)return fail(Error::Reply);
    const auto*p=r.raw.payload+32;const auto count=std::size_t(get(p+2,2)),total=std::size_t(get(p+20,4));const auto flags=get(p+28,4);
#if defined(NES_EXPERIMENTAL_BATCH_FETCH46) && NES_EXPERIMENTAL_BATCH_FETCH46
    // Форма PCM определяется ожидаемым opcode; fastFrame_ сохраняет отдельный lifecycle inline SUBMIT.
    const auto maximum=(request_.opcode==batch::submitOpcode||request_.opcode==batch::fetchOpcode)?
        maximumBatchPcmSamples:service::FrameService::maximumPcmChunkSamples;
#else
    const auto maximum=fastFrame_?maximumBatchPcmSamples:service::FrameService::maximumPcmChunkSamples;
#endif
    if(total>capacity_||total>core::Apu::sampleCapacity||get(p,2)!=1||get(p+4,8)!=binding_.romEpoch||get(p+12,8)!=frame_.inputFrame||
       get(p+24,4)!=offset_||offset_>total||count!=std::min(maximum,total-offset_)||(!firstBatch&&total!=sampleCount_)||
       r.raw.size!=64+2*count||flags!=(std::uint64_t(offset_+count==total?1U:0U)|(firstBatch&&submitAcknowledgesPrevious_?2U:0U)))return fail(Error::Reply);
    if(!total&&!firstBatch)return fail(Error::Reply);
    sampleCount_=total;
    for(std::size_t i=0;i<count;++i)pcm_[offset_+i]=signed16(p+32+2*i);
    offset_+=count;advance();
    if(stats_.pcmChunks==std::numeric_limits<std::uint32_t>::max())return fail(Error::Overflow);
    ++stats_.pcmChunks;
    if(firstBatch){inputReleased_=true;frame_.wire=nullptr;received_=frame_.bytes;
        if(stats_.rendererCalls==std::numeric_limits<std::uint32_t>::max())return fail(Error::Overflow);
        ++stats_.rendererCalls;submitAcknowledgesPrevious_=false;}
    if(offset_==sampleCount_)return finishPcm();
#if defined(NES_EXPERIMENTAL_BATCH_FETCH46) && NES_EXPERIMENTAL_BATCH_FETCH46
    // Большой DATA/Render кадр продолжает FETCH, но finishPcm всё ещё требует явный CONSUMED.
    phase_=(request_.opcode==batch::fetchOpcode||fastFrame_)?Phase::Fetch:Phase::Pcm;return true;
#else
    phase_=fastFrame_?Phase::Fetch:Phase::Pcm;return true;
#endif
}
/** @brief Разбирает ответ только ожидаемой фазы; snapshot/consumed не заменяют Worker join. */
bool BatchClient::accept(const ReplyView&r)noexcept{
    if(!common(r))return false;
    if(phase_==Phase::Unbind){
        service::IdleUnbindRequest expected{};
        if(!lifecycle::decodeRequest(body_.data(),lifecycle::requestBytes,expected)||!lifecycle::matchesKey(request_,expected)||
           !lifecycle::decodeAck(r.raw.payload,r.raw.size,expected))return fail(Error::Reply);
        phase_=Phase::Unbound;completion_=Completion::Unbind;return true;
    }
    const auto*p=r.raw.payload+32;
    if(phase_==Phase::Init){if(r.raw.size!=104)return fail(Error::Cold);
        constexpr std::uint32_t golden[]={31U,913123373U,32768U,3330297490U,8192U,2602333213U};
        for(unsigned i=0;i<6;++i)if(get(p+36+4*i,4)!=golden[i])return fail(Error::Cold);
        if(get(p,4)!=1||get(p+64,4)!=core::Apu::sampleCapacity||get(p+68,4)!=core::Apu::sampleRate||get(p+32,4)!=service::FrameService::maximumWireBytes||
           !get(p+12,4)||!get(p+16,4)||!get(p+20,4)||!get(p+24,4)||!get(p+28,4)||!get(p+60,4))return fail(Error::Cold);
        cold_={std::uint32_t(get(p+4,4)),std::uint32_t(get(p+8,4)),std::uint32_t(get(p+12,4)),std::uint32_t(get(p+16,4)),std::uint32_t(get(p+20,4)),
            std::uint32_t(get(p+24,4)),std::uint32_t(get(p+28,4)),std::uint32_t(get(p+32,4)),std::uint32_t(get(p+60,4)),std::uint32_t(get(r.raw.payload+8,4))};
        slot_^=1U;phase_=Phase::Cold;completion_=Completion::Init;return true;}
    if(phase_==Phase::Bind){if(!snapshot(r,service::State::Idle,0,0,0,0,0))return false;slot_^=1U;phase_=Phase::Bound;completion_=Completion::Bind;return true;}
    if(phase_==Phase::Submit)return acceptPcm(r,true);
    if(phase_==Phase::Pcm||phase_==Phase::Fetch)return acceptPcm(r,false);
    if(phase_==Phase::PriorConsumed||phase_==Phase::FinalConsumed||phase_==Phase::Consumed){const auto previousPhase=phase_;advance();
        if(!snapshot(r,service::State::Idle,0,0,0,0,stats_.rendererCalls))return false;
        remoteHeld_=false;previous_={};
        if(previousPhase==Phase::PriorConsumed){phase_=Phase::Begin;return true;}
        phase_=Phase::Bound;if(previousPhase==Phase::FinalConsumed)completion_=Completion::Drain;return true;}
    if(phase_==Phase::Begin){advance();if(!snapshot(r,service::State::Assembling,0,frame_.bytes,0,0,stats_.rendererCalls))return false;phase_=Phase::Data;return true;}
    if(phase_==Phase::Data){received_+=requestCount_;advance();const auto state=received_==frame_.bytes?service::State::Complete:service::State::Assembling;
        if(!snapshot(r,state,received_,frame_.bytes,0,0,stats_.rendererCalls))return false;
        if(received_==frame_.bytes){inputReleased_=true;frame_.wire=nullptr;phase_=Phase::Render;}return true;}
    if(phase_==Phase::Render){if(r.raw.size!=72)return fail(Error::Snapshot);sampleCount_=std::size_t(get(p+24,4));
        if(sampleCount_>capacity_||stats_.rendererCalls==std::numeric_limits<std::uint32_t>::max())return fail(Error::Capacity);
        ++stats_.rendererCalls;advance();if(!snapshot(r,service::State::Result,received_,frame_.bytes,0,sampleCount_,stats_.rendererCalls))return false;
        if(!sampleCount_)return finishPcm();
#if defined(NES_EXPERIMENTAL_BATCH_FETCH46) && NES_EXPERIMENTAL_BATCH_FETCH46
        // Batch contract уже требует поддержанные45/46; Legacy без этой capability остаётся на43.
        phase_=backend_==Backend::Batch?Phase::Fetch:Phase::Pcm;return true;}
#else
        phase_=Phase::Pcm;return true;}
#endif
    return fail(Error::State);
}
/** @brief Один Main шаг содержит не более одного poll/start; Pending не timeout и сохраняет borrows. */
Progress BatchClient::poll()noexcept{
    if(inside_){fail(Error::State);return Progress::Poisoned;}
    if(error_!=Error::None)return Progress::Poisoned;
    if(!pending_)return completion_==Completion::None?Progress::Idle:Progress::Complete;
    ReplyView reply{};inside_=true;const auto result=poll_(context_,reply);inside_=false;
    if(error_!=Error::None)return Progress::Poisoned;
    if(result==TransportState::Pending)return Progress::Pending;
    if(result!=TransportState::Complete){fail(Error::Exchange);return Progress::Poisoned;}
    pending_=false;if(!accept(reply))return Progress::Poisoned;
    if(phase_==Phase::Bound||phase_==Phase::Cold||phase_==Phase::Unbound)return Progress::Complete;
    return prepare()?Progress::Pending:Progress::Poisoned;
}
} // namespace nes::c6audio::async
