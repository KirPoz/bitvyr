#include "CoupledVideoPipeline.h"
#include "CoupledDspLoan.h"
#include "../../audio/c6/runtime/RemoteLoan.h"
#include "../bridge/ThinBridge.h"
#include "CoupledHotRaster.h"
#include "ppu/PpuColourReplay.h"
#include "ppu/PpuRgbOutput.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include <array>
#include <new>
#include <algorithm>
#include <cstdio>
// PRIVATE App-only ёмкость: default8 сохраняет прежний transport,16 требует отдельной приёмки.
static_assert(8==8||8==16,
              "COUPLED_RING_SLOTS must be8 or16");

namespace nes::app {
namespace coupled_cost {
}
using namespace coupled_cost;
namespace {


/**
 * @brief Возвращает пару функций выделения/освобождения индексированного изображения в PSRAM.
 * @details Аллокатор передаётся отдельному C0 Ppu и не владеет Board RGB.
 */

/**
 * @brief Проверяет принадлежность обоих концов ненулевого диапазона PSRAM.
 * @details Это локальная проверка адресного диапазона, а не реестр всех живых аллокаций.

 * @param[in] p Начало живой аллокации, принадлежность концов которой проверяется как PSRAM.
 * @param[in] n Размер проверяемого диапазона в байтах; extent/lifetime обеспечивает caller.
 */
bool external(const void*p,std::size_t n)noexcept{return p&&n&&esp_ptr_external_ram(p)&&esp_ptr_external_ram(static_cast<const std::uint8_t*>(p)+n-1);}
}

/**
 * @brief Рабочий владелец coupled-сеанса: очередь bridge, отдельный C0 PPU/MMC5/audio actor и Main/Worker состояние.
 * @details Factory владеет INTERNAL Control и PSRAM Impl/RGB до joined teardown; ROM и внешние RGB остаются у caller, Worker передаёт события в bridge, Main исполняет actor.
 */
struct CoupledVideoPipeline::Impl final {
    static constexpr unsigned slotsCount=8,eventsCount=64;
    bridge::Control* control;std::uint16_t* rgb;std::uint16_t*ownedRgb;
    const std::uint16_t*acceptedRgb=nullptr;
    platform::GameSourcePool rgbPool{};platform::GameSourceTicket acceptedTicket{};
    std::uint64_t rgbBeginCycle=7,rgbSequence=0;unsigned rgbSlot=0;bool rgbDouble=false;
    // Отдельные цифровая модель и expansion DAC принадлежат только Main actor.
    // Они не читают изменяемые primary MMC5/WRAM и живут до joined teardown.
    core::Machine shadow{coupled_hot::noPixels()};coupled_hot::Owner hot;core::PpuColourReplay actor{shadow};
    core::ThinReplaySink receiver{actor};


    std::array<bridge::Slot,slotsCount> slots{};std::array<core::ColourEvent,slotsCount*eventsCount> queued{};
    bridge::Reply reply{};std::array<core::ColourEvent,eventsCount> scratch{};
    bridge::Channel bridge;bridge::Producer producer;bridge::Consumer consumer;bridge::Sink adapter;
    
    /**
     * @brief Локальные метки начала и конца одного Worker held-loop, передаваемые только в synchronous stage.
     * @details Метки измеряют host wait и не подтверждают ACK, завершение C0 или возврат владения ресурсами.
     */
    /** @brief Только stack Worker во время одного synchronous stage; не публикуется Main из callback. */
    struct HeldWaitMarks final {std::int64_t begin=0,end=0;std::uint32_t entries=0,exits=0;};
    /**
     * @brief Снимает ровно enter/exit host timestamps вокруг уже существующего held-loop.
     * @details Не трогает Core/publication/Raw и не вызывается внутри polling iteration.
     * Exit после timeout означает конец измеряемого ожидания, а не освобождение владельца.
     */
    static void markHeldWait(void*context,bool entering)noexcept{
        auto&marks=*static_cast<HeldWaitMarks*>(context);const auto now=esp_timer_get_time();
        if(entering){marks.begin=now;++marks.entries;}else{marks.end=now;++marks.exits;}
    }
    /** @brief Заимствует стабильный Impl до joined teardown: передаёт begin/read/commit с ожиданием ACK, prefix копирует в bridge и учитывает host-время; C0 actor из Worker не исполняет. */ struct Timed final:core::ThinPpuSink {
        Impl& state;
                /**
                 * @brief Привязывает измеряющий адаптер к неизменному Impl.
                 * @details Он не приобретает владение producer или C0 актором.
                
                 * @param[in] s Неподвижный Impl, чьи adapter и Times заимствует обёртка до joined teardown.
                 */
                explicit Timed(Impl&s)noexcept:state(s){}
        
        /**
         * @brief Передаёт начальную lease/cut в bridge и ожидает настоящий begin ACK согласно текущему wait-контексту.
        
         * @param[in] l Неизменный producer/ROM/region/generation/epoch lease начального bind.
         * @param[in] c Уже завершённая абсолютная CPU-cycle/raster/phase граница reset7.
         */
        bool begin(const core::ColourLease&l,const core::ColourCut&c)noexcept override{
            std::printf("NES_COUPLED_SETUP stage=sink_begin cycle=%llu frame=%llu row=%d dot=%u phase=%u region=%u epoch=%llu generation=%llu mode=%u\n",
                static_cast<unsigned long long>(c.cycle),static_cast<unsigned long long>(c.frame),int(c.scanline),unsigned(c.dot),unsigned(c.phase),unsigned(l.region),
                static_cast<unsigned long long>(l.epoch),static_cast<unsigned long long>(l.generation),unsigned(state.thin.mode()));
            const bool ok=state.adapter.begin(l,c);
            std::printf("NES_COUPLED_SETUP stage=sink_begin_result ok=%u bridge_fault=%u thin_fault=%u receiver_fault=%u actor_error=%u\n",
                unsigned(ok),unsigned(state.bridge.fault()),unsigned(state.thin.fault()),unsigned(state.receiver.fault()),unsigned(state.actor.error()));
            return ok;
        }
        
        /**
         * @brief Передаёт завершённый префикс в копирующую очередь bridge и накапливает время вызова.
         * @details Не означает завершения C0: обычный префикс может остаться в очереди.
        
         * @param[in] p Завершённый prefix с actual CPU-cut и caller events, которые bridge копирует до возврата.
         */
        bool prefix(const core::ThinPrefix&p)noexcept override{const auto b=esp_timer_get_time();bool ok;
            ok=state.adapter.prefix(p);
            const auto end=esp_timer_get_time();state.times.prefix+=end-b;++state.times.prefixes;
            return ok;}
        
        /**
         * @brief Отправляет один настоящий запрос PPU-чтения и ждёт ответ с проверенной идентичностью.
         * @details Учитывает число, суммарное и максимальное время ожидания.
        
         * @param[in] q Once-only запрос с lease/cut/post-prefix sequence/request-id; не предсказанное значение.
         * @param[out] r Авторитетный C0 read-byte и та же identity; результат действителен только при true.
         */
        bool read(const core::ThinReadRequest&q,core::ThinReadReply&r)noexcept override{const auto b=esp_timer_get_time();bool ok;
            ok=state.adapter.read(q,r);
            const auto elapsed=std::uint64_t(esp_timer_get_time()-b);state.times.read+=elapsed;state.times.maxRead=std::max(state.times.maxRead,elapsed);++state.times.reads;return ok;}
        
        /**
         * @brief Передаёт окончательный cut и sequence и ждёт действительный commit ACK.
         * @details ACK не передаёт App право игнорировать отдельное владение PPA.
        
         * @param[in] l Неизменный lease сеанса.
         * @param[in] c Окончательная завершённая CPU-cycle/raster/phase граница кадра.
         * @param[in] s Sequence после последнего prefix; commit ACK относится именно к нему.
         */
        bool commit(const core::ColourLease&l,const core::ColourCut&c,std::uint64_t s)noexcept override{
            const auto begin=esp_timer_get_time();bool okay;
            if(state.dspEnabled){
                // Сначала реальная публикация, чтобы C0 мог закончить tail параллельно DSP.
                bridge::Ticket ticket{};bool published=false;
                for(;;){const auto result=state.producer.commit(l,c,s,ticket);
                    if(result==bridge::Result::Published){published=true;
                        break;}
                    if(result!=bridge::Result::Full||!Impl::wait(&state))break;
                }
                okay=published;
                if(okay){
                    const auto beforeDsp=state.producer.receipt(ticket);
                    if(beforeDsp==bridge::Result::Ready)++state.times.ackReadyBeforeDsp;
                    okay=beforeDsp==bridge::Result::Ready||beforeDsp==bridge::Result::Pending;
                    if(okay){
                        const auto dspBegin=esp_timer_get_time();
                        HeldWaitMarks heldMarks{};
                        okay=state.remoteEnabled?state.remoteLoan.stageAndPublish(c,s,ticket,Impl::markHeldWait,&heldMarks):state.dspLoan.renderAfterPublication(c,s,ticket);
                        const auto dspEnd=esp_timer_get_time();
                        if(state.remoteEnabled){
                            const auto stageUs=std::uint64_t(dspEnd-dspBegin);state.times.remoteStage+=stageUs;
                            state.times.remoteStageMax=std::max(state.times.remoteStageMax,stageUs);++state.times.remoteStageFrames;
                            state.times.remoteHeldEntries+=heldMarks.entries;state.times.remoteHeldExits+=heldMarks.exits;
                            if(heldMarks.entries||heldMarks.exits){
                                if(heldMarks.entries==1&&heldMarks.exits==1&&heldMarks.begin>=dspBegin&&heldMarks.end>=heldMarks.begin&&heldMarks.end<=dspEnd){
                                    const auto heldUs=std::uint64_t(heldMarks.end-heldMarks.begin);state.times.remoteHeld+=heldUs;
                                    state.times.remoteHeldMax=std::max(state.times.remoteHeldMax,heldUs);
                                }else ++state.times.remoteClockErrors;
                            }
                        }else
                        state.times.workerDsp+=dspEnd-dspBegin;
                        if(okay&&!state.remoteEnabled)++state.times.dspFrames;
                    }
                }
                if(okay){for(;;){const auto result=state.producer.receipt(ticket);
                    if(result==bridge::Result::Ready){
                        okay=state.remoteEnabled?state.remoteLoan.acknowledged(ticket):state.dspLoan.acknowledged(ticket);
                        break;}
                    if(result!=bridge::Result::Pending||!Impl::wait(&state)){okay=false;break;}
                }}
                if(!okay){
                    if(state.remoteEnabled){state.remoteLoan.poison();}else{state.dspLoan.poison();}
                    state.producer.stop(bridge::Fault::Receiver);}
            }else okay=state.adapter.commit(l,c,s);
            state.times.commit+=esp_timer_get_time()-begin;return okay;
        }
        
        /**
         * @brief Помечает транспорт неисправным через адаптер.
         * @details Не вызывает C0 актор из Worker.
        
         * @param[in] f Sticky ColourFault текущей session; invalidation не запускает C0 из Worker.
         */
        void invalidate(core::ColourFault f)noexcept override{state.adapter.invalidate(f);}
    } timed{*this};
    core::ThinPpuChannel thin{timed,scratch.data(),eventsCount};
    /** @brief Выбирает единственный RGB context после admission, без скрытого fallback. */
    core::PpuRgbOutput& selectedOutput()noexcept{
        return hot.value->output;
    }
    /** @brief Выбирает caller-owned pair table, живущую до joined teardown. */
    auto& selectedTable()noexcept{
        return hot.value->table;
    }
    /** @brief Возвращает выбранную C0 цифровую модель, не primary mapper. */
    core::Mmc5& selectedMapper()noexcept{
        return hot.value->mapper;
    }
    /** @brief Возвращает отдельный C0 DAC, который Worker не исполняет. */
    core::Mmc5Audio& selectedAudio()noexcept{
        return hot.value->audio;
    }
    core::ColourLease lease{};Times times{};
    // Только metadata; packet/renderer/PCM/token уже существуют в WorkerState.
    coupled_dsp::Loan dspLoan;bool dspEnabled=false;
    c6_async::RemoteLoan remoteLoan{};bool remoteEnabled=false;
    std::int64_t deadline=0;bool bootstrap=true,accepted=false;
    
    /**
     * @brief Создаёт связанный набор bridge endpoints и C0 receiver над уже выделенными control/RGB.
     * @details До публикации Worker bootstrap остаётся Main-ролью.
    
     * @param[in] c Уже выделенный выровненный INTERNAL bridge control, живущий до обоих park/join.
     * @param[in] r PSRAM staging не менее122880 байтов RGB565, удерживаемый до joined teardown.
     */
    Impl(bridge::Control*c,std::uint16_t*r)noexcept:control(c),rgb(r),ownedRgb(r),
        bridge({c,slots.data(),slotsCount,queued.data(),eventsCount,&reply}),producer(bridge),consumer(bridge,
        receiver
        ),adapter(producer,wait,this){}
    
    /**
     * @brief Во время idle-bootstrap на Main прокачивает C0 receiver; после handoff лишь проверяет fault/deadline.
     * @details Worker никогда не исполняет C0 callback через этот путь.
    
     * @param[in] context Указатель на стабильный Impl; bootstrap pump разрешён только Main до handoff Worker.
     */
    static bool wait(void*context)noexcept{
        auto&s=*static_cast<Impl*>(context);
        // Bootstrap выполняется на idle Main. Неизменная роль меняется ДО
        // первого start Worker и никогда не меняется при заимствованном задании.
        if(s.bootstrap){const auto r=s.consumer.pump();if(r!=bridge::Result::Ready&&r!=bridge::Result::Empty)return false;}
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return s.bridge.fault()==bridge::Fault::None&&esp_timer_get_time()<s.deadline;
    }
    
    /**
     * @brief Устанавливает sticky abort текущей transport session.
     * @details Во время задания разрешены только атомарные изменения остановки; освобождения здесь нет.
     */
    void stop()noexcept{static_cast<void>(bridge.abort(bridge.session(),bridge::Fault::Cancelled));}
    
    /**
     * @brief На Main между pump-вызовами паркует consumer, чтобы тот сам инвалидировал C0 актор после активного callback.
     */
    void parkMain()noexcept{static_cast<void>(consumer.park());}
};

/**
 * @brief На joined/reset7 Main выделяет выровненные control/Impl и PSRAM staging, выполняет debt barrier, привязывает bridge, thin Signals и RGB.
 * @details При отказе сохраняет порядок detach/park до освобождения; ROM должен оставаться живым.

 * @param[in] m Joined producer Machine на reset7, с нужным split audio и drained debt.
 * @param[in] rom Неизменное полное ROM image, живое до detach/обоих park.
 * @param[in] bytes Размер этого ROM image в байтах.
 * @param[in] region Регион NTSC/PAL, совпадающий с producer и C0.
 * @param[in] generation Ненулевое App generation, записываемое в неизменный transport lease.
 */
#if defined(NES_DIAGNOSTIC_MMC5_INDEX_MEMORY)
void CoupledVideoPipeline::setIndexedMemoryProbe(bool enableHp)noexcept{
    coupled_hot::indexedInHpSram=enableHp;
}
/** @brief Резервирует управляющий блок во внутренней памяти и долговечные графические буферы в PSRAM, затем связывает их с текущим поколением игры. */
#endif
CoupledVideoPipeline* CoupledVideoPipeline::create(core::Machine&m,const std::uint8_t*rom,std::size_t bytes,core::Region region,std::uint64_t generation,std::uint16_t*alternate)noexcept{
    auto*c=static_cast<bridge::Control*>(heap_caps_aligned_calloc(alignof(bridge::Control),1,sizeof(bridge::Control),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    auto*rgb=static_cast<std::uint16_t*>(heap_caps_malloc(122880,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    void*memory=heap_caps_aligned_alloc(alignof(Impl),sizeof(Impl),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    void*handle=heap_caps_malloc(sizeof(CoupledVideoPipeline),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    std::printf("NES_COUPLED_RING_CONFIG slots=%u events_per_slot=%u impl_bytes=%u slot_bytes=%u event_bytes=%u reply_bytes=%u\n",
        unsigned(Impl::slotsCount),unsigned(Impl::eventsCount),unsigned(sizeof(Impl)),unsigned(sizeof(bridge::Slot)),
        unsigned(sizeof(core::ColourEvent)),unsigned(sizeof(bridge::Reply)));
    const bool controlInternal=c&&esp_ptr_internal(c),rgbExternal=external(rgb,122880),stateExternal=external(memory,sizeof(Impl)),handleExternal=external(handle,sizeof(CoupledVideoPipeline));
    const bool stateAligned=memory&&reinterpret_cast<std::uintptr_t>(memory)%alignof(Impl)==0;
    std::printf("NES_COUPLED_SETUP stage=allocation control=%p rgb=%p state=%p handle=%p control_internal=%u rgb_external=%u state_external=%u handle_external=%u state_bytes=%u control_align=%u state_align=%u state_remainder=%u\n",
        static_cast<void*>(c),static_cast<void*>(rgb),memory,handle,unsigned(controlInternal),unsigned(rgbExternal),unsigned(stateExternal),unsigned(handleExternal),unsigned(sizeof(Impl)),unsigned(alignof(bridge::Control)),unsigned(alignof(Impl)),unsigned(reinterpret_cast<std::uintptr_t>(memory)%alignof(Impl)));
    if(!c||!rgb||!memory||!handle||!controlInternal||!rgbExternal||!stateExternal||!handleExternal||!stateAligned){heap_caps_free(handle);heap_caps_free(memory);heap_caps_free(rgb);heap_caps_free(c);return nullptr;}
    ::new(c)bridge::Control{};auto*s=::new(memory)Impl(c,rgb);auto*result=::new(handle)CoupledVideoPipeline(s);
    // До actor.begin/load C0 получает собственный MMC5; M4 продолжает прежний путь.
    const auto& cartridge=m.cartridge(); // Та же reset7-граница materialization CPU/PPU debt.
    std::printf("NES_COUPLED_SETUP stage=constructed cpu_cycles=%llu frame=%llu region=%u mapper=%u observable=%u split_video=%u split_audio=%u shadow_pixels=%u core_video_fault=%u core_audio_fault=%u slots=%p slot_remainder=%u reply=%p reply_remainder=%u\n",
        static_cast<unsigned long long>(m.cpuCycles()),static_cast<unsigned long long>(m.frames()),unsigned(m.region()),unsigned(cartridge.mapper()),
        unsigned(m.observableVideoEnabled()),unsigned(m.splitVideoEnabled()),unsigned(m.splitAudio()),unsigned(s->shadow.legacyVideoReady()),unsigned(m.videoFault()),unsigned(m.audioFault()),static_cast<void*>(s->slots.data()),unsigned(reinterpret_cast<std::uintptr_t>(s->slots.data())%alignof(bridge::Slot)),static_cast<void*>(&s->reply),unsigned(reinterpret_cast<std::uintptr_t>(&s->reply)%alignof(bridge::Reply)));
    s->lease={&m,rom,bytes,m.colourTransportEpoch(),generation,region};
    // До внешнего bind нет attached endpoints/Worker loans; отказ полностью локален Main.
    bool indexedRow=false;
#if defined(NES_DIAGNOSTIC_MMC5_INDEX_MEMORY)
    indexedRow=coupled_hot::indexedInHpSram&&cartridge.mapper()==5U;
#endif
    const bool hotReady=s->hot.create(s->shadow,s,sizeof(Impl),m,s->lease,rgb,indexedRow);
    const bool externalBound=hotReady&&(indexedRow?s->actor.bindExternalRgbOnlyPpu(s->hot.value->ppu):
                                                  s->actor.bindExternalPpu(s->hot.value->ppu));
    s->hot.report(externalBound,indexedRow,generation);
    if(!externalBound){
        s->~Impl();c->~Control();heap_caps_free(c);heap_caps_free(rgb);heap_caps_free(s);result->~CoupledVideoPipeline();heap_caps_free(result);return nullptr;
    }
    const core::ColourCut cut{7,0,-1,std::uint16_t(region==core::Region::Pal?22:21),std::uint8_t(region==core::Region::Pal?2:0)};
    const bridge::Span extra[]={{&m,sizeof(m)},{&s->shadow,sizeof(s->shadow)},{&s->actor,sizeof(s->actor)},{&s->receiver,sizeof(s->receiver)},
        {&s->producer,sizeof(s->producer)},{&s->consumer,sizeof(s->consumer)},{&s->adapter,sizeof(s->adapter)},{&s->thin,sizeof(s->thin)},
        {&s->timed,sizeof(s->timed)},{s->scratch.data(),sizeof(s->scratch)},{s->selectedTable().data(),sizeof(s->selectedTable())},{&s->selectedOutput(),sizeof(s->selectedOutput())},{rgb,122880},
        {&s->selectedMapper(),sizeof(core::Mmc5)},{&s->selectedAudio(),sizeof(core::Mmc5Audio)}
        ,{s->hot.value,sizeof(coupled_hot::HotRaster)},{s->hot.value->ppu.indexedStorageIdentity(),s->hot.value->ppu.indexedStorageBytes()}
        ,{alternate,122880}
    };
    s->deadline=esp_timer_get_time()+1500000;
    const auto initialized=s->bridge.initialize(s->lease,cut,extra,sizeof(extra)/sizeof(extra[0])-(alternate?0U:1U));
    std::printf("NES_COUPLED_SETUP stage=bridge_initialize result=%u bridge_fault=%u region=%u cycle=%llu row=%d dot=%u phase=%u\n",
        unsigned(initialized),unsigned(s->bridge.fault()),unsigned(region),static_cast<unsigned long long>(cut.cycle),int(cut.scanline),unsigned(cut.dot),unsigned(cut.phase));
    auto producer=bridge::Result::Stopped,consumer=bridge::Result::Stopped;bool configured=false,bound=false;
    if(initialized==bridge::Result::Ready){producer=s->producer.attach();std::printf("NES_COUPLED_SETUP stage=producer_attach result=%u bridge_fault=%u\n",unsigned(producer),unsigned(s->bridge.fault()));}
    if(initialized==bridge::Result::Ready&&producer==bridge::Result::Ready){consumer=s->consumer.attach();std::printf("NES_COUPLED_SETUP stage=consumer_attach result=%u bridge_fault=%u\n",unsigned(consumer),unsigned(s->bridge.fault()));}
    if(initialized==bridge::Result::Ready&&producer==bridge::Result::Ready&&consumer==bridge::Result::Ready){
        // Legacy160 имеет FD/FE-защёлки и /RD IRQ: сохраняем полный Timing PPU внутри того же Thin/C6 протокола.
        const auto mode=cartridge.mapper()==160U?core::ThinMode::Oracle:core::ThinMode::Signals;
        configured=m.configureThinPpu(&s->thin,s->lease,mode);
        std::printf("NES_COUPLED_SETUP stage=configure_thin ok=%u bridge_fault=%u thin_fault=%u actor_error=%u core_video_fault=%u core_audio_fault=%u mode=%u\n",
            unsigned(configured),unsigned(s->bridge.fault()),unsigned(s->thin.fault()),unsigned(s->actor.error()),unsigned(m.videoFault()),unsigned(m.audioFault()),unsigned(s->thin.mode()));
        if(configured){bound=s->selectedOutput().bind(s->actor,rgb,core::PpuRgbOutput::pixelCount,s->selectedTable().data(),s->selectedTable().size());
            std::printf("NES_COUPLED_SETUP stage=rgb_bind ok=%u bridge_fault=%u thin_fault=%u actor_error=%u rgb_error=%u\n",
                unsigned(bound),unsigned(s->bridge.fault()),unsigned(s->thin.fault()),unsigned(s->actor.error()),unsigned(s->selectedOutput().error()));}
    }
    if(bound&&alternate){
        bound=s->selectedOutput().registerAlternate(s->actor,alternate,core::PpuRgbOutput::pixelCount)&&
            s->rgbPool.configure(rgb,alternate)&&s->rgbPool.beginWrite(0);
        s->rgbDouble=bound;
    }
    bool ready=initialized==bridge::Result::Ready&&producer==bridge::Result::Ready&&consumer==bridge::Result::Ready&&configured&&bound;
    if(!ready){
        // Даже отказ bind уже мог передать channel storage в Machine.
        m.reset();static_cast<void>(m.cartridge());static_cast<void>(m.configureThinPpu(nullptr));
        destroyJoined(result);return nullptr;
    }
    // PREACK loan зависит от настоящего bound commit, а не наличия MMC5 expansion DAC.
    s->dspEnabled=m.thinPpuBound();
    s->bootstrap=false;s->times={};return result;
}

/**
 * @brief Уничтожает контекст после реального завершения Worker и явного detach producer.
 * @details Main последовательно паркует endpoints; неизвестное владение нельзя заменять истечением timeout.

 * @param[in] p Фабричный контекст после настоящего Worker done и producer detach; nullptr допустим.
 */
bool CoupledVideoPipeline::destroyJoined(CoupledVideoPipeline*p)noexcept{
    if(!p)return true;
    auto*s=p->state_;if(s->rgbDouble&&!s->rgbPool.canDestroy())return false;s->stop();s->parkMain();static_cast<void>(s->producer.park());
    auto*c=s->control;auto*rgb=s->ownedRgb;s->~Impl();c->~Control();heap_caps_free(c);heap_caps_free(rgb);heap_caps_free(s);p->~CoupledVideoPipeline();heap_caps_free(p);return true;
}

/**
 * @brief На joined Main проверяет producer, ROM, размер, регион, поколение и актуальную transport epoch.
 * @details Не предназначен для чтения producer во время его работы.

 * @param[in] m Joined producer Machine, чья актуальная epoch читается в этом вызове.
 * @param[in] r Адрес того же удерживаемого ROM image.
 * @param[in] n Полный размер ROM image в байтах.
 * @param[in] region Неизменный регион текущего сеанса.
 * @param[in] generation Ожидаемое App generation; номер кадра сюда не передаётся.
 */
bool CoupledVideoPipeline::matches(const core::Machine&m,const std::uint8_t*r,std::size_t n,core::Region region,std::uint64_t generation)const noexcept{
    const auto&l=state_->lease;return l.producer==&m&&l.epoch==m.colourTransportEpoch()&&l.rom==r&&l.romBytes==n&&l.region==region&&l.generation==generation;
}

/**
 * @brief До публикации start сбрасывает принятие/счётчики и задаёт крайний срок задания.
 * @details Все обычные поля в этот момент принадлежат Main.
 */
void CoupledVideoPipeline::startJob()noexcept{state_->accepted=false;state_->acceptedRgb=nullptr;state_->acceptedTicket={};state_->times={};state_->deadline=esp_timer_get_time()+1500000;
}

/** @brief Неизменный admit не читает занятую primary Machine. */
bool CoupledVideoPipeline::preAckDspEnabled()const noexcept{
    return state_->dspEnabled;
}
/** @details Main вызывает только до start semaphore и после истинного previous done. */
bool CoupledVideoPipeline::offerPreAckDsp(core::Machine&m,core::AudioPacket&p,core::AudioRenderer&r,std::int16_t*pcm,std::size_t n,core::AudioStageToken&t,std::uint64_t input)noexcept{
    auto&s=*state_;return s.dspEnabled&&s.dspLoan.offer(m,p,r,pcm,n,t,s.lease,s.bridge.session(),input);
}
/** @details Прежний audioReady guard уже установлен внутри вернувшегося stepFrame. */
bool CoupledVideoPipeline::finalizePreAckDsp(core::Machine&m)noexcept{
    auto&s=*state_;
    if(s.remoteEnabled)return s.remoteLoan.finalize(m,s.thin.counters().prefixes);
    return !s.dspEnabled||s.dspLoan.finalize(m,s.thin.counters().prefixes);
}
/** @details После настоящего done Main не рендерит и не копирует PCM повторно. */
bool CoupledVideoPipeline::acceptPreAckDspJoined(core::Machine&m,std::uint64_t input,std::uint32_t&samples)noexcept{
    auto&s=*state_;return s.dspEnabled&&s.dspLoan.acceptJoined(m,s.lease,s.bridge.session(),input,s.thin.counters().prefixes,samples);
}
/** @details Fault не равен join: caller сохраняет всё до настоящего завершения Worker. */
bool CoupledVideoPipeline::abandonPreAckDspJoined(core::Machine&m)noexcept{
    auto&s=*state_;
    if(s.remoteEnabled)return s.remoteLoan.abandonJoined(m);
    return !s.dspEnabled||s.dspLoan.abandonJoined(m);
}

/** @details Joined Main предлагает прежний retired packet; новый AudioPacket/renderer не создаётся. */
bool CoupledVideoPipeline::offerRemoteAudio(core::Machine&m,core::AudioPacket&p,core::AudioStageToken&t,
    ::nes::c6audio::async::AudioPublished&publication,const ::nes::c6audio::async::PublishedKey&key)noexcept{
    auto&s=*state_;if(!s.dspEnabled||!s.remoteLoan.offer(m,p,t,publication,s.lease,s.bridge.session(),key,Impl::wait,&s))return false;
    s.remoteEnabled=true;return true;
}
/** @details Caller уже получил done и проверил result/generation/ROM/cut, PCM N может быть pending. */
bool CoupledVideoPipeline::acceptRemoteAudioJoined(core::Machine&m,std::uint64_t input,::nes::c6audio::async::JoinedFrame&joined)noexcept{
    auto&s=*state_;return s.remoteEnabled&&s.remoteLoan.acceptJoined(m,s.lease,s.bridge.session(),input,s.thin.counters().prefixes,joined);
}

/**
 * @brief На Main обслуживает очередной доступный запрос receiver и накапливает его wall time.
 * @details Producer Machine не наблюдает; опциональный proven Empty не вызывает bridge/timer, Ready измеряется прежним путём.
 */
coupled_done::VideoResult CoupledVideoPipeline::pump()noexcept{
    auto&s=*state_;
    const auto begin=esp_timer_get_time();
    const auto result=s.consumer.pump();
    if(result==bridge::Result::Ready){
        s.times.consumer+=esp_timer_get_time()-begin;
        return coupled_done::VideoResult::Processed;
    }
    return result==bridge::Result::Empty?coupled_done::VideoResult::Empty:coupled_done::VideoResult::Fault;
}

/**
 * @brief Сравнивает монотонное время с deadline задания.
 * @details Не отменяет Worker и не доказывает park.
 */
bool CoupledVideoPipeline::expired()const noexcept{return esp_timer_get_time()>=state_->deadline;}

/**
 * @brief Устанавливает sticky abort текущей transport session.
 * @details Во время задания разрешены только атомарные изменения остановки; освобождения здесь нет.
 */
void CoupledVideoPipeline::stop()noexcept{state_->stop();}

/**
 * @brief На Main между pump-вызовами паркует consumer, чтобы тот сам инвалидировал C0 актор после активного callback.
 */
void CoupledVideoPipeline::parkMain()noexcept{state_->parkMain();}

/**
 * @brief На Worker проверяет transport/thin faults и его собственный producer Thin binding перед гостевой работой CPU1.
 * @details Не читает изменяемые Main lifecycle-флаги.
 */
bool CoupledVideoPipeline::workerCanRun()const noexcept{return state_->bridge.fault()==bridge::Fault::None&&state_->thin.fault()==core::ColourFault::None&&state_->lease.producer->thinPpuBound();}

/**
 * @brief На Worker проверяет его итоговый endpoint без чтения Main lifecycle.
 * @details Настоящий done публикуется внешним worker-протоколом App.
 */


bool CoupledVideoPipeline::workerFinished()noexcept{auto&s=*state_;const bool ok=s.bridge.fault()==bridge::Fault::None&&s.thin.outputReady();if(!ok){s.producer.stop();static_cast<void>(s.producer.park());}return ok;}

/**
 * @brief На joined Main сверяет завершение обоих акторов, cut/frame и полный clock/APU witness.
 * @details Ошибка запрещает принятие RGB и связанного audio.

 * @param[in] cycle Абсолютное число завершённых CPU cycles производителя, не число PPU dots.
 * @param[in] frame Число завершённых producer кадров, сверяемое с thin commit counter.
 */
bool CoupledVideoPipeline::jointReady(std::uint64_t cycle,std::uint64_t frame)noexcept{auto&s=*state_;
    const bool okay=s.bridge.fault()==bridge::Fault::None&&s.thin.outputReady()&&s.receiver.ready()&&s.actor.error()==core::PpuColourReplay::Error::None&&s.actor.cycle()==cycle&&s.actor.checkWitness()&&frame==s.thin.counters().commits;
    return okay;
}

/**
 * @brief При joined RGB-границе подтверждает согласованный вывод.
 * @details Вызывающая сторона заранее завершает владение PPA и проверяет реальные DSP/аудио-свидетели.
 */
bool CoupledVideoPipeline::accept()noexcept{
    auto&s=*state_;s.accepted=s.bridge.fault()==bridge::Fault::None&&s.selectedOutput().pixels()==s.rgb;
    if(!s.accepted)return false;
    s.acceptedRgb=s.rgb;
    if(s.rgbDouble){
        const auto frame=s.thin.counters().commits;
        s.acceptedTicket={s.rgb,s.lease.generation,s.lease.epoch,++s.rgbSequence,frame-1,frame,s.rgbBeginCycle,s.actor.cycle(),std::uint8_t(s.rgbSlot),std::uint8_t(s.lease.region)};
        if(!s.rgbPool.seal(s.acceptedTicket)){s.accepted=false;s.acceptedRgb=nullptr;return false;}
    }
    return true;
}

/**
 * @brief Возвращает заимствованный staging RGB только после успешного принятия.
 * @details Указатель не даёт права обходить joined/PPA границы.
 */
const std::uint16_t* CoupledVideoPipeline::pixels()const noexcept{return state_->accepted?state_->acceptedRgb:nullptr;}
/** @brief Не читает Machine/Worker; конфигурация фиксирована до первого start. */
bool CoupledVideoPipeline::rgbPoolEnabled()const noexcept{return state_->rgbDouble;}
/** @brief Отделяет постоянное право освобождения A от чередующегося назначения. */
const std::uint16_t*CoupledVideoPipeline::ownedRgb()const noexcept{return state_->ownedRgb;}
/** @brief Копирует только принятый Main ticket без публикации или смены credit. */
platform::GameSourceTicket CoupledVideoPipeline::acceptedSource()const noexcept{return state_->accepted?state_->acceptedTicket:platform::GameSourceTicket{};}
/** @brief Резервирует credit до единственного Board begin. */
bool CoupledVideoPipeline::borrowSource(const platform::GameSourceTicket&t)noexcept{return state_->rgbDouble&&state_->accepted&&platform::sameGameSource(t,state_->acceptedTicket)&&state_->rgbPool.borrow(t);}
/** @brief Фиксирует успешное принятие hardware reader без освобождения источника. */
bool CoupledVideoPipeline::confirmSource(const platform::GameSourceTicket&t)noexcept{return state_->rgbDouble&&state_->rgbPool.confirmBorrow(t);}
/** @brief Возвращает Ready только при непринятом Board begin; Reading отменить нельзя. */
bool CoupledVideoPipeline::cancelSource(const platform::GameSourceTicket&t)noexcept{return state_->rgbDouble&&state_->rgbPool.cancelBorrow(t);}
/** @brief Возвращает source credit после точного Board completion с тем же ticket. */
bool CoupledVideoPipeline::completeSource(const platform::GameSourceTicket&t)noexcept{return state_->rgbDouble&&state_->rgbPool.complete(t);}
/** @brief Переставляет только свободное назначение; принятый pointer/key остаётся прежним до startJob. */
bool CoupledVideoPipeline::prepareNextRgb(const platform::GameSourceTicket&t)noexcept{
    auto&s=*state_;if(!s.rgbDouble||!s.accepted||!platform::sameGameSource(t,s.acceptedTicket)||
        s.rgbPool.state(t.slot)!=platform::GameSourcePool::State::Reading)return false;
    const unsigned next=t.slot^1U;auto*destination=s.rgbPool.source(next);
    if(!s.rgbPool.beginWrite(next)||!s.selectedOutput().retarget(s.actor,destination,t.endCycle,t.frame))return false;
    s.rgb=destination;s.rgbSlot=next;s.rgbBeginCycle=t.endCycle;return true;
}

/**
 * @brief Возвращает Main-снимок измерений текущего задания.
 * @details Поля перекрываются с работой другого ядра и не складываются в FPS.
 */
const CoupledVideoPipeline::Times& CoupledVideoPipeline::times()const noexcept{return state_->times;}
}
