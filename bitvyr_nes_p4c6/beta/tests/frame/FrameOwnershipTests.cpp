/** @file FrameOwnershipTests.cpp @brief Проверяет настоящий FramePipeline с SDK allocator mocks, без RTOS/платы. */
#include "FrameVideoPipeline.h"
#include "audio/c6/runtime/AudioPublished.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "esp_heap_caps.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <fstream>
#include "SyntheticImage.inl"
using namespace nes::core;
using nes::app::FrameVideoPipeline;
static unsigned assertions=0;
/**
 * @brief Проверяет одно условие сценария и учитывает его в общем счётчике assertions.
 * @details Заимствует строку what только на время вызова. При false печатает
 * диагностику в stderr и завершает процесс кодом 73; при true продолжает тест.
 * CRT assert и модальные окна Windows не используются.
 */
static void demand(bool okay,const char*what){++assertions;if(!okay){std::fprintf(stderr,"FAIL %s check=%u\n",what,assertions);std::exit(73);}}
/**
 * @brief Добавляет value как восемь байтов little-endian в собственный вектор результата.
 * @details out заимствуется с правом дописывания; существующие байты сохраняются.
 * Сериализация не зависит от endian и padding HOST-структур. Ошибки роста
 * std::vector не перехватываются и прерывают native-тест.
 */
static void word(std::vector<unsigned char>&out,std::uint64_t value){for(unsigned i=0;i<8;++i)out.push_back(static_cast<unsigned char>(value>>(i*8)));}
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
/**
 * @brief Считает 64-битный FNV-1a с fixture seed по занятым Input, Dmc и Expansion записям immutable audio view.
 * @details v и указатели его записей заимствуются до возврата; количество байтов
 * берётся из header counts, свободная ёмкость не читается. Вызывающий обеспечивает
 * действительный readable view; функция не проверяет lifetime и не удерживает view.
 */
static std::uint64_t tapeHash(const AudioPacketView&v){
    std::uint64_t h=1469598103934665603ULL;
    /** @brief Добавляет n заимствованных байтов p в hash h операциями XOR/умножения; p обязан быть читаемым для всего диапазона, при n=0 не разыменовывается. */
    const auto add=[&](const void*p,std::size_t n){const auto*b=static_cast<const unsigned char*>(p);for(std::size_t i=0;i<n;++i){h^=b[i];h*=1099511628211ULL;}};
    add(v.inputs(),v.header().inputCount*sizeof(AudioPacket::Input));add(v.dmc(),v.header().dmcCount*sizeof(AudioPacket::Dmc));
    add(v.expansions(),v.header().expansionCount*sizeof(AudioPacket::Expansion));return h;
}
#endif
/**
 * @brief Проверяет шесть кадров FrameVideoPipeline для одного mapper/region и дописывает RGB/CPU/RAM результат.
 * @details mapper и region выбирают synthetic ROM и временную шкалу; out принадлежит
 * вызывающему. ROM, Machine, AudioPacket и RGB остаются живы до destroyJoined;
 * каждый audio ticket проверяется до возврата заимствованного слота владельцу.
 * При negative намеренно требуется принятие повреждённого ticket, поэтому
 * корректный отказ pipeline должен завершить demand кодом 73. Прочие нарушения
 * тоже немедленно завершают тест; это синхронный HOST сценарий без RTOS proof.
 */
static void scenario(unsigned mapper,Region region,std::vector<unsigned char>&out,bool negative){
    auto rom=image(mapper,1);
    auto machine=std::make_unique<Machine>(LegacyPixelAllocator{+/** @brief Передаёт Machine результат malloc(n); успешный блок принадлежит Machine до освобождения парной функцией аллокатора. */ [](std::size_t n)noexcept{return static_cast<std::uint8_t*>(std::malloc(n));},+/** @brief Освобождает переданный Machine malloc-блок; nullptr допустим, после вызова p больше не принадлежит Machine. */ [](std::uint8_t*p)noexcept{std::free(p);}});
    Mmc5 digital;Mmc5Audio expansion;machine->configureMmc5(&digital,&expansion);
    auto active=std::make_unique<AudioPacket>();auto renderer=std::make_unique<AudioRenderer>();
    demand(machine->load(rom.data(),rom.size(),region)==LoadResult::Ok,"load");
    machine->configureSplitAudio(active.get(),true);static_cast<void>(machine->cartridge());
    auto*rgb=static_cast<std::uint16_t*>(heap_caps_aligned_alloc(128,122880,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
    demand(rgb!=nullptr,"rgb");
    auto*p=FrameVideoPipeline::createOutput(*machine,rom.data(),rom.size(),region,17,*active,rgb,61440);
    demand(p&&p->error()==FrameVideoPipeline::Error::None,"create");
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
    OutputFrameLease cold{};demand(p->outputLeaseAtCreate(cold),"cold_lease");
    demand(cold.rom==rom.data()&&cold.generation==17&&cold.romEpoch&&cold.transportEpoch,"cold_domains");
    FrameVideoPipeline::JoinedAudioTicket previous{},last{};
    AudioPacketView oldView{nullptr,nullptr,0,nullptr,0,nullptr,0};std::uint64_t oldHash=0;
    bool held=false;
#endif
    for(unsigned n=0;n<6;++n){
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
        demand(p->audioSlotAvailable(n),"slot_available_before_dispatch");
#endif
        demand(p->prepareJob(n,static_cast<std::uint8_t>(n*17),17),"prepare");
        machine->setController(0,static_cast<std::uint8_t>(n*17));
        demand(p->produce()==RunStatus::FrameReady,"produce");
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
        AudioPacketView view{nullptr,nullptr,0,nullptr,0,nullptr,0};FrameVideoPipeline::JoinedAudioTicket ticket{};
        demand(!p->borrowJoinedAudio(view,ticket),"worker_finished_not_joined");
#endif
        demand(p->joinedProducer(),"joined_producer"); // Caller здесь синхронно знает возврат produce; не RTOS proof.
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
        demand(!p->outputLeaseAtCreate(cold),"late_cold_getter_refused");
        demand(p->borrowJoinedAudio(view,ticket)&&view.readable(),"borrow_joined_audio");
        demand(ticket.output.slot==(n&1U)&&ticket.output.sequence==n&&ticket.output.inputFrame==n&&ticket.audio.ppuFrame==n+1&&ticket.appGeneration==17,"ticket_cut");
        demand(ticket.output.lease.romEpoch==cold.romEpoch&&ticket.output.lease.transportEpoch==cold.transportEpoch,"unchanged_lease");
        demand(n||ticket.output.baseCycle==7,"first_output_base7");demand(n||ticket.audio.baseCycle==0,"first_audio_base0");
        auto unchanged=ticket;auto duplicate=view;
        demand(!p->borrowJoinedAudio(duplicate,unchanged),"duplicate_borrow");
        demand(sameAudioHeader(ticket.audio,unchanged.audio),"rejected_output_unchanged");
        demand(!p->releaseJoinedAudio(ticket,false),"timeout_not_source_done");
        for(unsigned field=0;field<15;++field){
            auto bad=ticket;
            switch(field){
            case 0:++bad.appGeneration;break;case 1:++bad.output.lease.generation;break;
            case 2:++bad.output.lease.romEpoch;break;case 3:++bad.output.lease.transportEpoch;break;
            case 4:bad.output.lease.region=region==Region::Pal?Region::Ntsc:Region::Pal;break;
            case 5:++bad.output.sequence;break;case 6:++bad.output.inputFrame;break;case 7:++bad.output.baseCycle;break;
            case 8:bad.output.slot=2;break;case 9:++bad.audio.sequence;break;case 10:++bad.audio.baseCycle;break;
            case 11:++bad.audio.endCycle;break;case 12:++bad.audio.ppuFrame;break;
            case 13:bad.output.lease.rom+=1;break;case 14:++bad.output.lease.romBytes;break;
            }
            demand(!p->releaseJoinedAudio(bad,true),"stale_full_key_refused");
        }
        if(held){
            demand(tapeHash(oldView)==oldHash,"other_slot_production_preserves_audio");
            if(n>=2)demand(!p->releaseJoinedAudio(last,true),"stale_after_slot_reuse");
        }
#endif
        demand(p->prepareDsp(*renderer),"prepare_dsp_stage");
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
        demand(!renderer->valid()&&renderer->sampleCount()==0,"no_local_dsp");
        // Чётный N: C6 sourceDone может прийти до C0. Состояние Issued сохраняет video stage.
        if((n&1U)==0){demand(p->releaseJoinedAudio(ticket,true),"early_source_done");demand(!p->releaseJoinedAudio(ticket,true),"double_release_refused");auto again=view;auto againTicket=ticket;demand(!p->borrowJoinedAudio(again,againTicket),"no_reborrow_after_early_release");}
#endif
        demand(p->consumeColour(*renderer),"consume_colour");demand(p->accept(17),"accept");demand(p->refreshWitness(*renderer),"refresh_witness");
        const auto&s=p->snapshot();
        demand(s.frame==n+1&&s.inputFrame==n&&s.appGeneration==17,"snapshot_identity");
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
        demand(s.sampleCount==0&&s.pcmHash==0,"no_fake_pcm");
#else
        demand(s.sampleCount>0&&renderer->valid(),"real_local_pcm_control");
#endif
        word(out,mapper);word(out,static_cast<unsigned>(region));word(out,s.frame);word(out,s.cycle);word(out,s.ramHash);word(out,s.rgbHash);
        const auto&cpu=s.cpu;
        for(auto value:{std::uint64_t(cpu.pc),std::uint64_t(cpu.a),std::uint64_t(cpu.x),std::uint64_t(cpu.y),std::uint64_t(cpu.sp),std::uint64_t(cpu.p),std::uint64_t(cpu.cycles),std::uint64_t(cpu.jammed)})word(out,value);
        bool varied=false;for(unsigned pixel=1;pixel<61440;++pixel)varied|=rgb[pixel]!=rgb[0];demand(varied,"nonconstant_rgb");
        const auto*b=reinterpret_cast<const unsigned char*>(rgb);out.insert(out.end(),b,b+122880);
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
        if(held){
            demand(!p->audioSlotAvailable(n+1),"no_reuse_before_previous_source_done");
            demand(!FrameVideoPipeline::destroyJoined(p),"destroy_retains_audio_owner");
            demand(p->releaseJoinedAudio(previous,true),"previous_source_done");last=previous;held=false;
        }
        if(n&1U){previous=ticket;oldView=view;oldHash=tapeHash(view);held=true;}
        demand(p->audioSlotAvailable(n+1),"next_slot_reusable");
        if(negative){auto bad=ticket;++bad.appGeneration;demand(p->releaseJoinedAudio(bad,true),"negative_bad_key_rejected");}
#else
        static_cast<void>(negative);
#endif
    }
#if defined(NES_APP_C6_FRAME_VIDEO) && NES_APP_C6_FRAME_VIDEO
    if(held)demand(p->releaseJoinedAudio(previous,true),"final_source_done");
#endif
    demand(FrameVideoPipeline::destroyJoined(p),"destroy_after_all_releases");heap_caps_free(rgb);
}
/**
 * @brief Запускает четыре M0/M7 NTSC/PAL сценария и записывает полный детерминированный byte stream.
 * @details Требует argc=3: argv[1] задаёт выходной бинарный файл, argv[2] включает
 * negative только при точном совпадении строки. В обычном режиме возвращает 0
 * после успешной записи; неверные аргументы, отказ сценария или записи дают
 * exit(73) через demand. Локальный out владеет всеми сериализованными байтами.
 * Walltime, FPS и аппаратная работа DMA этим тестом не измеряются.
 */
int main(int argc,char**argv){
    demand(argc==3,"args");std::vector<unsigned char>out;const bool negative=std::strcmp(argv[2],"negative")==0;
    for(unsigned mapper:{0U,7U})for(auto region:{Region::Ntsc,Region::Pal})scenario(mapper,region,out,negative);
    std::ofstream f(argv[1],std::ios::binary);f.write(reinterpret_cast<const char*>(out.data()),static_cast<std::streamsize>(out.size()));demand(bool(f),"write");
    std::printf("PASS actual_frame scenarios=4 frames=24 checks=%u bytes=%zu\n",assertions,out.size());return 0;
}
