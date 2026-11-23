#pragma once
#include "AudioStageToken.h"
#include "apu/AudioRenderer.h"
#include "../bridge/ThinBridge.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nes::app::coupled_dsp {
/**
 * @brief Единственный одноразовый loan существующих packet/renderer/PCM между Main и Worker.
 * @details Main offer допустим только после настоящего join; опубликованный start передаёт все
 * поля Worker. Main снова читает их только после настоящего done. Здесь нет RTOS/heap/часов.
 */
class Loan final {
public:
    /** @brief Этапы не являются отдельными completion: до WorkerDone ресурсами нельзя распоряжаться. */
    enum class Phase : std::uint8_t {Idle,Offered,Published,Rendered,Acknowledged,Finalized,Accepted,Poisoned};
    /**
     * @brief На joined Main предлагает существующие непересекающиеся буферы для следующего input frame.
     * @param primary Единственная Machine, которая после start принадлежит Worker.
     * @param packet Существующий retired packet; новый AudioPacket не создаётся.
     * @param renderer Существующий renderer; Main не наблюдает его до done.
     * @param pcm Существующий Snapshot PCM; Worker делает прежнюю единственную копию.
     * @param capacity Ёмкость pcm в mono16 отсчётах.
     * @param token Стабильный token в EmulationWorkerState, живущий до joined cleanup.
     * @param lease Неизменные ROM/generation/epoch/region и producer.
     * @param session Текущая ненулевая bridge session.
     * @param inputFrame Нулевой индекс предстоящего input; primary.frames() ещё равен ему.
     */
    bool offer(core::Machine& primary,core::AudioPacket& packet,core::AudioRenderer& renderer,
               std::int16_t* pcm,std::size_t capacity,core::AudioStageToken& token,
               const core::ColourLease& lease,std::uint32_t session,std::uint64_t inputFrame) noexcept {
        if((phase_!=Phase::Idle&&phase_!=Phase::Accepted)||token.valid()||!session||!lease.generation||
           lease.producer!=&primary||lease.epoch!=primary.colourTransportEpoch()||
           lease.region!=primary.region()||primary.frames()!=inputFrame||inputFrame==~std::uint64_t{0}||
           !pcm||!capacity||capacity>core::Apu::sampleCapacity||
           reinterpret_cast<std::uintptr_t>(pcm)%alignof(std::int16_t))return false;
        const auto bytes=capacity*sizeof(*pcm);
        if(overlaps(pcm,bytes,&packet,sizeof(packet))||overlaps(pcm,bytes,&renderer,sizeof(renderer))||
           overlaps(pcm,bytes,&token,sizeof(token))||overlaps(pcm,bytes,&primary,sizeof(primary))||
           overlaps(pcm,bytes,this,sizeof(*this))||overlaps(pcm,bytes,lease.rom,lease.romBytes))return false;
        primary_=&primary;packet_=&packet;renderer_=&renderer;pcm_=pcm;capacity_=capacity;token_=&token;
        lease_=lease;session_=session;input_=inputFrame;audioGeneration_=primary.audioGeneration();
        ticket_={};cut_={};sequence_=0;samples_=0;phase_=Phase::Offered;return true;
    }
    /**
     * @brief На Worker после настоящей commit publication подготавливает и рендерит точный PCM.
     * @param cut Завершённый CPU/PPU cut того же кадра, который уже поставлен C0 в очередь.
     * @param sequence Post-prefix sequence опубликованного Commit.
     * @param ticket Квитанция именно успешного Producer.commit, не предсказанный serial.
     * @return false отравляет loan без cancellation/free и без Main retry.
     */
    bool renderAfterPublication(const core::ColourCut& cut,std::uint64_t sequence,
                                bridge::Ticket ticket) noexcept {
        if(phase_!=Phase::Offered||ticket.session!=session_||!ticket.serial||cut.frame!=input_+1U)
            return fail();
        ticket_=ticket;cut_=cut;sequence_=sequence;phase_=Phase::Published;
        if(!primary_->stageAudioPacketForThinCommit(*packet_,*token_))return fail();
        const auto& h=token_->header();
        if(!token_->valid()||h.generation!=audioGeneration_||h.region!=lease_.region||
           h.ppuFrame!=cut.frame||h.endCycle!=cut.cycle)return fail();
        if(renderer_->render(*packet_)!=core::AudioRenderResult::Ok||!rendererMatches(h))return fail();
        samples_=renderer_->sampleCount();
        if(samples_>capacity_)return fail();
        // Та же единственная Snapshot PCM copy, которая прежде выполнялась Main после join.
        std::memcpy(pcm_,renderer_->samples(),samples_*sizeof(*pcm_));
        phase_=Phase::Rendered;return true;
    }
    /**
     * @brief Worker фиксирует настоящий matching receipt после DSP, не имитируя WorkerDone.
     * @param ticket Только Producer.receipt==Ready с исходной publication identity.
     */
    bool acknowledged(bridge::Ticket ticket) noexcept {
        if(phase_!=Phase::Rendered||ticket.session!=ticket_.session||ticket.position!=ticket_.position||
           ticket.serial!=ticket_.serial)return fail();
        phase_=Phase::Acknowledged;return true;
    }
    /**
     * @brief После Machine.stepFrame на Worker завершает staged interval без второй копии.
     * @param primary Та же Machine после ACK/ThinReady; ordinary retire guard не обходится.
     */
    bool finalize(core::Machine& primary,std::uint64_t sequence) noexcept {
        if(phase_!=Phase::Acknowledged||sequence_!=sequence||primary_!=&primary||!matchesPrimary(primary)||
           !primary.finalizeAudioPacketForThinCommit(*token_)||token_->valid()||!token_->completed())return fail();
        phase_=Phase::Finalized;return true;
    }
    /**
     * @brief Joined Main принимает уже готовый PCM, не вызывая Renderer второй раз.
     * @param primary Тот же присоединённый producer; чтение живой Machine запрещено.
     * @param lease Текущая полная ROM/generation/epoch/region привязка.
     * @param session Текущая bridge session после true done.
     * @param inputFrame Ожидаемый индекс принятого input.
     * @param samples Получает количество готовых mono16 отсчётов только при true.
     */
    bool acceptJoined(core::Machine& primary,const core::ColourLease& lease,std::uint32_t session,
                      std::uint64_t inputFrame,std::uint64_t sequence,std::uint32_t& samples) noexcept {
        if(phase_!=Phase::Finalized||sequence!=sequence_||primary_!=&primary||session!=session_||inputFrame!=input_||
           !core::sameColourLease(lease,lease_)||!matchesPrimary(primary)||!token_->completed()||
           token_->valid()||!rendererMatches(token_->header())||samples_>capacity_)return fail();
        samples=samples_;phase_=Phase::Accepted;return true;
    }
    /**
     * @brief На присоединённом Main закрывает завершённый либо отравленный loan перед reset/detach.
     * @param primary Живая Machine при настоящем WorkerDone и между C0 callbacks.
     * @details Ни timeout, ни fault сами по себе не разрешают этот вызов. PCM/packet/renderer не освобождаются.
     */
    bool abandonJoined(core::Machine& primary)noexcept{
        if(phase_==Phase::Idle)return true;
        if(primary_!=&primary||!token_)return false;
        if(token_->valid()&&!primary.abandonAudioPacketForThinCommitJoined(*token_))return false;
        if(!token_->resetJoined())return false;
        primary_=nullptr;packet_=nullptr;renderer_=nullptr;token_=nullptr;pcm_=nullptr;
        phase_=Phase::Idle;return true;
    }
    /** @brief Числовой этап читается Worker либо joined Main; это не атомарный completion. */
    Phase phase()const noexcept{return phase_;}
    /** @brief Не освобождает заимствования: ошибки сохраняются до настоящего join/reset. */
    bool poison()noexcept{return fail();}
private:
    /** @brief Проверяет полуоткрытые диапазоны без переполнения uintptr; invalid трактуется как alias. */
    static bool overlaps(const void* a,std::size_t an,const void* b,std::size_t bn)noexcept{
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
        if(!a||!b||!an||!bn||an>~std::uintptr_t{0}-x||bn>~std::uintptr_t{0}-y)return true;
        return x<y+bn&&y<x+an;
    }
    /** @brief Сопоставляет renderer с закрытым immutable заголовком, не читая primary packet. */
    bool rendererMatches(const core::AudioPacket::Header& h)const noexcept{
        return renderer_->valid()&&renderer_->generation()==h.generation&&renderer_->sequence()==h.sequence&&
            renderer_->ppuFrame()==h.ppuFrame&&renderer_->cycles()==h.endCycle;
    }
    /** @brief Worker либо joined Main сверяет owner/epoch/region и точный завершённый cut. */
    bool matchesPrimary(const core::Machine& primary)const noexcept{
        return lease_.producer==&primary&&lease_.epoch==primary.colourTransportEpoch()&&
            lease_.region==primary.region()&&primary.audioGeneration()==audioGeneration_&&
            primary.frames()==cut_.frame&&primary.cpuCycles()==cut_.cycle;
    }
    /** @brief Защёлкивает отказ без сброса token, renderer, ROM, packet или PCM storage. */
    bool fail()noexcept{phase_=Phase::Poisoned;return false;}
    core::Machine*primary_=nullptr;core::AudioPacket*packet_=nullptr;core::AudioRenderer*renderer_=nullptr;
    core::AudioStageToken*token_=nullptr;std::int16_t*pcm_=nullptr;std::size_t capacity_=0;
    core::ColourLease lease_{};bridge::Ticket ticket_{};core::ColourCut cut_{};
    std::uint64_t input_=0,audioGeneration_=0,sequence_=0;std::uint32_t session_=0,samples_=0;
    Phase phase_=Phase::Idle;
};
} // namespace nes::app::coupled_dsp
