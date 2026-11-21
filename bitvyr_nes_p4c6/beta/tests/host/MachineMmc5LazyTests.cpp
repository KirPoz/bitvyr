/**
 * @file MachineMmc5LazyTests.cpp
 * @brief Независимые directed-срезы lazy MMC5 и его обязательных барьеров.
 * @details Эталон исполняет отдельные tick и настоящие digital M2. PRIVATE
 * режим передаётся числом argv, а не consumer-макросом. Это не замер FPS.
 */
#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "ppu/VideoRenderer.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
unsigned checks=0,cases=0,activated=0;
/** @brief Прекращает процесс на первом различии с точной строкой теста. */
[[noreturn]] void fail(const char* expression,unsigned line){
    std::fprintf(stderr,"MMC5_LAZY line=%u check=%s\n",line,expression);std::exit(1);
}
#define CHECK(x) do{++checks;if(!(x))fail(#x,__LINE__);}while(false)
}

namespace nes::core {
/** @brief Меняет только artificial writer metadata для чистых capacity отказов. */
struct VideoSegmentWriterTestAccess {
    /** @brief Оставляет free legacy credits, не заполняя физическую arena мусором. */
    static void remaining(VideoPacket& p,unsigned free){
        CHECK(p.header().eventCount+free<=VideoPacket::eventCapacity);
        p.hot().legacyCount=VideoPacket::eventCapacity-p.header().eventCount-free;
    }
    /** @brief Вводит одну несовместимую запись перед pure admission. */
    static void reject(VideoPacket& p,unsigned kind){
        if(kind==0)p.hot().state=VideoPacket::State::Sealed;
        else if(kind==1)p.hot().header.protocol=VideoPacket::literalProtocol;
        else if(kind==2)p.hot().literalPrefixCount=1;
        else p.hot().header.fault=VideoFault::InvalidEvent;
    }
    /** @brief Возвращает искусственный отказ в допустимый state перед stop. */
    static void cleanup(VideoPacket& p){p.hot().state=VideoPacket::State::Recording;p.hot().literalPrefixCount=0;}
};

/** @brief Единственный test-friend; не добавляет production hooks или полей. */
struct MachineEventKernelTestAccess {
    /** @brief Дописывает числовой массив без padding и host pointers. */
    template<class T,std::size_t N>static void append(std::vector<std::uint64_t>& v,const std::array<T,N>& a){for(auto x:a)v.push_back(x);}
    /** @brief Задаёт synthetic video memory до begin/immutable seed. */
    static void initial(Machine& m,Mmc5& d,bool nameFF){
        for(unsigned i=0;i<m.nametables_.size();++i)m.nametables_[i]=nameFF?0xffU:std::uint8_t(i*37U+11U);
        for(unsigned i=0;i<d.exRam_.size();++i)d.exRam_[i]=std::uint8_t(i*13U+7U);
    }
    /** @brief Создаёт фазовый seed старым tick; последний CHR имеет настоящий адрес. */
    static void prepare(Machine& m,Mmc5& d,unsigned start,unsigned mode,unsigned scroll,unsigned fine,unsigned pixels,bool candidate){
        auto& p=m.timingPpu_;p.reset();p.bind(m.videoPacket_);p.scanline_=12;
        p.control_=0x10;p.mask_=0x1e;p.fineX_=std::uint8_t(fine);p.v_=p.t_=std::uint16_t(scroll);
        p.renderingEnabled_=p.renderingNext_=true;
        d.cpuWrite(0x2000,p.control_);d.cpuWrite(0x2001,p.mask_);d.cpuWrite(0x5104,mode);
        d.cpuWrite(0x5105,0x44);d.cpuWrite(0x5200,0);
        d.ppuReadStrobe(0x2000);d.ppuReadStrobe(0x2000);d.ppuReadStrobe(0x2000);d.ppuReadStrobe(0);
        for(unsigned i=0;i<32;++i){p.palette_[i]=std::uint8_t(i+1U);CHECK(m.videoPacket_->add(VideoOp::PaletteWrite,i,0,0,i+1U));}
        while(p.dot_<start)p.tick();
        CHECK(p.dot_==start&&p.spriteCacheValid_&&d.nameRepeat_==0&&d.lastPpuAddress_<0x2000U);
        p.pendingPixels_=pixels;p.pendingPixelX_=pixels?start-1U-pixels:0U;
        d.cpuIdle_=0;d.ppuReadSinceM2_=true;
        m.mmc5ReadQuantum_.active=candidate;m.mmc5ReadQuantum_.frameQualified=candidate;
        m.mmc5ReadQuantum_.deadline=m.cycles_+40000U;
    }
    /** @brief Исполняет candidate в scalar или GQ-origin; эталон всегда отдельные tick. */
    static void chunk(Machine& m,Mmc5& d,unsigned dots,bool gq,bool candidate){
        if(candidate){if(gq)m.timingPpu_.replayMmc5InplaceDots(dots);
            else{d.clockCpu(1U);m.timingPpu_.tickDots(dots);}}
        else for(unsigned i=0;i<dots;++i){if(i%3U==0U)d.clockCpu(1U);m.timingPpu_.tick();}
    }
    /** @brief Снимает наблюдаемые logical поля ДО materialize, включая реальные адреса. */
    static std::vector<std::uint64_t> logical(const Machine& m){const auto& p=m.timingPpu_;
        return {p.dot_,p.v_,p.t_,p.busAddress_,p.coarseXSignal_,p.scrollSignal_,p.nextTile_,p.nextAttribute_,
            p.status_,p.spriteCacheClocks_,p.pendingPixels_,p.pendingPixelX_,p.pendingBackgroundShifts_,
            p.nmiPending_,p.nmiDelay_,p.frameReady_};
    }
    /** @brief Снимает все mapper latches и backing, не материализуя candidate. */
    static std::vector<std::uint64_t> mapper(const Mmc5& d){
        std::vector<std::uint64_t> v={d.prgMode_,d.chrMode_,d.protect1_,d.protect2_,d.exMode_,d.ntMap_,d.fillTile_,d.fillAttribute_,
            d.upperChr_,d.splitControl_,d.splitScroll_,d.splitBank_,d.splitY_,d.irqCompare_,d.scanline_,d.multiplierA_,d.multiplierB_,
            d.ppuControl_,d.ppuMask_,d.lastChrSet_,d.nameRepeat_,d.cpuIdle_,d.tileAttribute_,d.tileSplitAttribute_,d.tileSplitY_,
            d.lastPpuAddress_,d.tileNtOffset_,d.irqEnabled_,d.irqPending_,d.inFrame_,d.ppuReadSinceM2_,d.tileExtended_,d.tileSplit_,d.splitSlMode_};
        append(v,d.exRam_);append(v,d.prgBanks_);append(v,d.chrBanks_);for(const auto& a:d.normalChrBases_)append(v,a);return v;
    }
    /** @brief Снимает полный PPU data endpoint после закрытия долга. */
    static std::vector<std::uint64_t> full(const Machine& m){auto v=logical(m);const auto& p=m.timingPpu_;
        v.insert(v.end(),{p.control_,p.mask_,p.oamAddress_,p.openBus_,p.readBuffer_,p.fineX_,p.writeToggle_,
            p.renderingEnabled_,p.renderingNext_,p.renderDelay_,p.pendingFill_,p.pendingPaletteIndex_,p.evaluateN_,p.evaluateM_,
            p.secondaryPosition_,p.evaluateLatch_,p.overflowFetches_,p.evaluationDone_,p.evaluationNextDot_,
            std::uint64_t(std::int64_t(p.scanline_)),p.frames_,p.pal_,p.odd_,p.nmiLine_,p.suppressVblank_,p.spriteCacheValid_,p.spriteZeroRow_});
        append(v,p.backgroundAtoms_);append(v,p.palette_);append(v,p.oam_);append(v,p.secondary_);
        append(v,p.secondaryIndex_);append(v,p.spriteRow_);append(v,p.spriteZeroMask_);
        for(const auto& s:p.sprites_)v.insert(v.end(),{s.low,s.high,s.x,s.attributes,s.index,s.valid});
        return v;
    }
    /** @brief Снимает архитектурный CPU/RAM срез; не исполняет debugRead. */
    static std::vector<std::uint64_t> cpu(const Machine& m){const auto& s=m.cpu_.state();
        std::vector<std::uint64_t> v={s.pc,s.a,s.x,s.y,s.sp,s.p,s.cycles,s.jammed,m.cycles_,m.openBus_,m.ppuRemainder_};append(v,m.ram_);return v;
    }
    /** @brief Материализует owner-local долг и затем старый Draw-prefix. */
    static void close(Machine& m){m.flushPpu();m.timingPpu_.flushPixels();}
    /** @brief Проверяет фактическую активацию, не выводя PRIVATE define в consumer. */
    static bool pending(const Machine& m){return m.timingPpu_.hasMmc5Lazy();}
    /** @brief Возвращает диагностические source counters, без значения времени хоста. */
    static std::array<unsigned,5> counts(const Machine& m){const auto& c=m.timingPpu_.mmc5LazyCounts_;return {c.chunks,c.dots,c.materializations,c.scalarChunks,c.gqChunks};}
    /** @brief Пробует чистый private admission, без скрытого fallback. */
    static bool admission(Machine& m,unsigned dots,bool first){return m.timingPpu_.tryAppendMmc5Lazy(dots,first);}
    /** @brief Читает настоящие horizon и GQ budget до материализации логического address. */
    static std::array<unsigned,2> budget(Machine& m){return {m.timingPpu_.mmc5InplaceActiveDots(),m.mmc5QuietCpuBudget(80,false)};}
    /** @brief Проверяет явные held CHR low/high на границе tileFF/fineY7. */
    static void heldFF(const Machine& m){const auto& p=m.timingPpu_;const unsigned phase=(p.dot_-1U)&7U;
        if(phase>=4U&&phase<=6U)CHECK(p.busAddress_==0x1ff7U);
        else if(phase==7U)CHECK(p.busAddress_==0x1fffU);
    }
    /** @brief Один уже наступивший Cart M2 до DMC callback или scalar PPU. */
    static void cart(Mmc5& d){d.clockCpu(1U);}
    /** @brief Настоящий CPU-bus read, включая vector observation и его M2. */
    static std::uint8_t read(Machine& m,std::uint16_t a){return m.read(a);}
    /** @brief Настоящий MMIO/mapper write, не raw mutation snapshot. */
    static void write(Machine& m,std::uint16_t a,std::uint8_t v){m.write(a,v);}
    /** @brief Настоящий split DMC callback после уже исполненного Cart M2. */
    static std::uint8_t dmc(Machine& m,std::uint16_t a){return m.dmcRead(a,m.cycles_);}
    /** @brief Вводит S0-кандидата либо исключённую область той же строки. */
    static void sprite(Machine& m,unsigned x){auto& p=m.timingPpu_;p.spriteZeroRow_=true;p.spriteZeroMask_.fill(0);p.spriteZeroMask_[x>>3U]=std::uint8_t(1U<<(x&7U));}
    /** @brief Создаёт один runtime reject до первого admission. */
    static void reject(Machine& m,Mmc5& d,unsigned kind){auto& p=m.timingPpu_;
        switch(kind){case 0:m.outputJob_=false;break;case 1:p.trackBusCycles_=true;break;
        case 2:p.trackBusAddresses_=true;break;case 3:p.renderDelay_=1;break;case 4:p.nmiDelay_=1;break;
        case 5:p.nmiPending_=true;break;case 6:p.frameReady_=true;break;case 7:p.pendingFill_=true;break;
        case 8:d.nameRepeat_=3;break;case 9:d.lastPpuAddress_=0x2345;break;case 10:d.inFrame_=false;break;
        case 11:d.irqEnabled_=d.irqPending_=true;break;case 12:p.dot_=249;break;case 13:p.fineX_=8;break;
        case 14:p.nextAttribute_=4;break;case 15:p.spriteCacheClocks_=256;break;default:CHECK(false);}
    }
    /** @brief Возвращает только fixture wiring перед штатным stop, без guest rewind. */
    static void stop(Machine& m){m.flushPpu();m.mmc5ReadQuantum_={};m.outputJob_=true;m.timingPpu_.packet_=m.videoPacket_;}
    /** @brief Разрешает настоящему stepFrame самому открыть и закрыть FrameScope. */
    static void realScope(Machine& m){m.mmc5ReadQuantum_={};}
};
} // namespace nes::core

namespace {
using namespace nes::core;
using Access=MachineEventKernelTestAccess;
using PacketAccess=VideoSegmentWriterTestAccess;
/** @brief Собственная NES2 MMC5 ROM; CHR различает fineY, page и каждый tile. */
std::vector<std::uint8_t> rom(bool jam=false){
    std::vector<std::uint8_t> r(16U+32768U+131072U,0xea);std::fill(r.begin(),r.begin()+16,0);
    r[0]='N';r[1]='E';r[2]='S';r[3]=0x1a;r[4]=2;r[5]=16;r[6]=0x50;r[7]=8;r[10]=7;
    r[16U+0x6000U]=jam?0x02:0x4c;r[16U+0x6001U]=0;r[16U+0x6002U]=0xe0;
    for(unsigned a:{0x7ffaU,0x7ffcU,0x7ffeU}){r[16U+a]=0;r[17U+a]=0xe0;}
    for(unsigned i=0;i<131072U;++i)r[16U+32768U+i]=std::uint8_t(i*29U+19U+(i>>10U)*37U);
    return r;
}
/** @brief Владеет неалиасными writer ресурсами до настоящего stop/cancel. */
struct Fixture {
    Mmc5 digital;Mmc5Audio expansion;std::array<VideoPacket,2> packets;
    std::array<std::array<std::uint8_t,10240>,2> ram{};AudioPacket audio;
    OutputFrameLease lease{};Machine machine;
    /** @brief Загружает только собственную ROM; начальный seed предшествует runtime памяти. */
    explicit Fixture(const std::vector<std::uint8_t>& image,bool nameFF=false){
        machine.configureMmc5(&digital,&expansion);CHECK(machine.load(image.data(),image.size(),Region::Ntsc)==LoadResult::Ok);
        machine.configureSplitAudio(&audio,true);OutputFrameBinding b{};
        b.source={&machine,image.data(),image.size(),machine.colourTransportEpoch(),1,0,Region::Ntsc};
        b.packets={&packets[0],&packets[1]};b.snapshots={ram[0].data(),ram[1].data()};
        b.protectedSpans[0]={&audio,sizeof(audio)};b.protectedSpans[1]={&digital,sizeof(digital)};b.protectedSpans[2]={&expansion,sizeof(expansion)};
        CHECK(machine.configureOutputFrames(b,lease));Access::initial(machine,digital,nameFF);
        OutputFrameTicket ticket{lease,0,0,machine.cpuCycles(),0};CHECK(machine.beginOutputFrame(ticket));
        for(unsigned i=0;i<4;++i)Access::write(machine,std::uint16_t(0x5114U+i),std::uint8_t(0x80U+i));
        Access::write(machine,0x5102,2);Access::write(machine,0x5103,1);
    }
    /** @brief Останавливает host-only writer после проверок даже при injected fault. */
    void stop(){Access::stop(machine);PacketAccess::cleanup(packets[0]);CHECK(machine.stopOutputWriter(lease));CHECK(machine.cancelOutputFrames(lease));}
};
/** @brief Сравнивает data endpoint после explicit close; сырые encodings могут различаться. */
void same(Fixture& a,Fixture& b,bool pixels=true){
    Access::close(a.machine);Access::close(b.machine);
    CHECK(!Access::pending(a.machine));CHECK(Access::full(a.machine)==Access::full(b.machine));
    CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));CHECK(Access::cpu(a.machine)==Access::cpu(b.machine));
    CHECK(a.packets[0].header().fault==b.packets[0].header().fault);
    CHECK(a.packets[0].legacyEventCount()==b.packets[0].legacyEventCount());
    if(pixels&&a.packets[0].header().fault==VideoFault::None){
        CHECK(a.packets[0].readRecording()&&b.packets[0].readRecording());
        auto sa=std::make_unique<VideoStorage>(),sb=std::make_unique<VideoStorage>();
        VideoRenderer va(*sa),vb(*sb);std::vector<std::uint16_t> ra(256U*240U),rb(256U*240U);
        va.setRgb565Output(ra.data());vb.setRgb565Output(rb.data());
        CHECK(va.observeVideo(a.packets[0]));CHECK(vb.observeVideo(b.packets[0]));CHECK(sa->indices==sb->indices);CHECK(ra==rb);
    }
    const auto& x=a.audio.header();const auto& y=b.audio.header();
    CHECK(x.inputCount==y.inputCount&&x.dmcCount==y.dmcCount&&x.expansionCount==y.expansionCount);
    CHECK(std::memcmp(a.audio.inputs(),b.audio.inputs(),x.inputCount*sizeof(AudioPacket::Input))==0);
    CHECK(std::memcmp(a.audio.dmc(),b.audio.dmc(),x.dmcCount*sizeof(AudioPacket::Dmc))==0);
    CHECK(std::memcmp(a.audio.expansions(),b.audio.expansions(),x.expansionCount*sizeof(AudioPacket::Expansion))==0);
}
/** @brief Не материализуя candidate, проверяет truthful digital projection каждого chunk. */
void logical(Fixture& a,Fixture& b){CHECK(Access::logical(a.machine)==Access::logical(b.machine));CHECK(Access::mapper(a.digital)==Access::mapper(b.digital));}
/** @brief A: все восемь фаз после adjacent scalar/GQ частей без промежуточного flush. */
void phases(const std::vector<std::uint8_t>& image,bool expected){
    for(unsigned mode:{0U,1U})for(unsigned v:{0x001dU,0x041fU,0x73fdU,0x7fffU})
    for(unsigned style=0;style<3;++style)for(unsigned fine:{0U,7U})for(unsigned pixels:{0U,8U}){
        auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,9,mode,v,fine,pixels,true);
        Access::prepare(b->machine,b->digital,9,mode,v,fine,pixels,false);
        for(unsigned i=0;i<8;++i){const bool gq=style==1U||(style==2U&&(i&1U));
            Access::chunk(a->machine,a->digital,3,gq,true);Access::chunk(b->machine,b->digital,3,gq,false);
            logical(*a,*b);CHECK(Access::pending(a->machine)==expected);
        }
        if(expected){
#if defined(NES_CORE_C1_COST_PROFILE)
            const auto c=Access::counts(a->machine);CHECK(c[0]==8U&&c[1]==24U&&c[2]==0U);
#endif
            ++activated;
        }
        same(*a,*b);Access::chunk(a->machine,a->digital,3,false,true);Access::chunk(b->machine,b->digital,3,false,false);
        same(*a,*b);a->stop();b->stop();++cases;
    }
}
/** @brief B: materialize после следующего Cart M2 обязан сохранить Pfalse/I0. */
void cartCuts(const std::vector<std::uint8_t>& image,bool expected){
    for(unsigned stop=1;stop<=8;++stop){auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,17,1,0x741e,0,8,true);Access::prepare(b->machine,b->digital,17,1,0x741e,0,8,false);
        for(unsigned i=0;i<stop;++i){Access::chunk(a->machine,a->digital,3,i&1U,true);Access::chunk(b->machine,b->digital,3,i&1U,false);}
        CHECK(Access::pending(a->machine)==expected);Access::cart(a->digital);Access::cart(b->digital);logical(*a,*b);
        same(*a,*b); // Сравнивает R/P/I сразу после flush, ДО следующего /RD.
        Access::chunk(a->machine,a->digital,3,false,true);Access::chunk(b->machine,b->digital,3,false,false);
        same(*a,*b);a->stop();b->stop();++cases;
    }
}
/** @brief C: реальные CPU/DMC/vector и PPU/mapper writes разрывают старую историю. */
void barriers(const std::vector<std::uint8_t>& image,bool expected){
    const std::array<std::uint16_t,19> writes={0x2000,0x2001,0x2005,0x2006,0x2007,0x5104,0x5105,0x5106,0x5107,
        0x5120,0x5128,0x5130,0x5200,0x5201,0x5202,0x5c00,0x4014,0x2003,0x2004};
    for(unsigned kind=0;kind<26;++kind){auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,17,kind&1U,0x741e,0,8,true);Access::prepare(b->machine,b->digital,17,kind&1U,0x741e,0,8,false);
        Access::chunk(a->machine,a->digital,3,false,true);Access::chunk(b->machine,b->digital,3,false,false);
        CHECK(Access::pending(a->machine)==expected);
        if(kind<4U){const std::uint16_t address=kind<2U?std::uint16_t(0x2002U+kind*5U):std::uint16_t(0xfffaU+kind-2U);
            CHECK(Access::read(a->machine,address)==Access::read(b->machine,address));}
        else if(kind<7U){Access::cart(a->digital);Access::cart(b->digital);logical(*a,*b);
            const auto address=std::uint16_t(kind==6U?0xc123U:0xfffaU+kind-4U);
            CHECK(Access::dmc(a->machine,address)==Access::dmc(b->machine,address));}
        else{Access::write(a->machine,writes[kind-7U],0x55);Access::write(b->machine,writes[kind-7U],0x55);}
        CHECK(!Access::pending(a->machine));same(*a,*b);
        Access::chunk(a->machine,a->digital,3,false,true);Access::chunk(b->machine,b->digital,3,false,false);
        same(*a,*b);a->stop();b->stop();++cases;
    }
}
/** @brief D: S0 вне отрезка разрешён, hit-кандидат и runtime guards дают старый путь. */
void domains(const std::vector<std::uint8_t>& image,bool expected){
    for(unsigned kind=0;kind<22;++kind){auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,9,0,0x041d,0,8,true);Access::prepare(b->machine,b->digital,9,0,0x041d,0,8,false);
        if(kind<16U){Access::reject(a->machine,a->digital,kind);Access::reject(b->machine,b->digital,kind);}
        else if(kind<20U){PacketAccess::reject(a->packets[0],kind-16U);PacketAccess::reject(b->packets[0],kind-16U);}
        else{Access::sprite(a->machine,kind==20U?150U:9U);Access::sprite(b->machine,kind==20U?150U:9U);}
        const auto before=Access::full(a->machine),map=Access::mapper(a->digital);const auto count=a->packets[0].header().eventCount;
        const auto fault=a->packets[0].header().fault;const auto state=a->packets[0].state();
        const auto credits=a->packets[0].legacyEventCount();
        const bool admitted=Access::admission(a->machine,3,true);
        CHECK(admitted==(expected&&kind==20U));
        if(!admitted){CHECK(Access::full(a->machine)==before);CHECK(Access::mapper(a->digital)==map);CHECK(a->packets[0].header().eventCount==count);
            CHECK(a->packets[0].header().fault==fault&&a->packets[0].state()==state&&a->packets[0].legacyEventCount()==credits);}
        else Access::chunk(b->machine,b->digital,3,true,false);
        same(*a,*b,false);a->stop();b->stop();++cases;
    }
}
/** @brief E: весь visible-horizon reserve отвергается чисто при0/1/недостатке credits. */
void capacity(const std::vector<std::uint8_t>& image,bool expected){
    constexpr unsigned needed=2U*(249U-9U)+16U;
    for(unsigned free:{0U,1U,needed-1U,needed}){auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,9,0,0x041d,0,8,true);Access::prepare(b->machine,b->digital,9,0,0x041d,0,8,false);
        PacketAccess::remaining(a->packets[0],free);PacketAccess::remaining(b->packets[0],free);
        const auto before=Access::full(a->machine),map=Access::mapper(a->digital);const auto count=a->packets[0].header().eventCount;
        const auto fault=a->packets[0].header().fault;const auto state=a->packets[0].state();
        const auto credits=a->packets[0].legacyEventCount();
        const bool admitted=Access::admission(a->machine,3,true);CHECK(admitted==(expected&&free==needed));
        if(admitted)Access::chunk(b->machine,b->digital,3,true,false);
        else{CHECK(Access::full(a->machine)==before);CHECK(Access::mapper(a->digital)==map);CHECK(a->packets[0].header().eventCount==count);
            CHECK(a->packets[0].header().fault==fault&&a->packets[0].state()==state&&a->packets[0].legacyEventCount()==credits);
            // Реальный caller после чистого false обязан поставить capacity fault
            // на прежней фазе, а не только вернуть одинаковый пустой endpoint.
            Access::chunk(a->machine,a->digital,3,true,true);Access::chunk(b->machine,b->digital,3,true,false);
        }
        same(*a,*b,false);a->stop();b->stop();++cases;
    }
}
/** @brief G: накопленный долг закрывается до249, затем старый путь пересекает255/256. */
void endpointFallbacks(const std::vector<std::uint8_t>& image,bool expected){
    for(unsigned start:{233U,241U})for(unsigned mode:{0U,1U}){
        auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,start,mode,0x7fff,0,8,true);
        Access::prepare(b->machine,b->digital,start,mode,0x7fff,0,8,false);
        for(unsigned i=0;i<8;++i){
            Access::chunk(a->machine,a->digital,3,i&1U,true);Access::chunk(b->machine,b->digital,3,i&1U,false);
            logical(*a,*b);CHECK(Access::pending(a->machine)==(expected&&start+3U*(i+1U)<=249U));
        }
        same(*a,*b);a->stop();b->stop();++cases;
    }
}
/** @brief H: tileFF/fineY7 удерживает1ff7/1fff и одинаковый next-GQ verdict на всех8 фазах. */
void heldHighCorners(const std::vector<std::uint8_t>& image,bool expected){
    for(unsigned mode:{0U,1U}){
        auto a=std::make_unique<Fixture>(image,true),b=std::make_unique<Fixture>(image,true);
        Access::prepare(a->machine,a->digital,9,mode,0x701d,0,8,true);
        Access::prepare(b->machine,b->digital,9,mode,0x701d,0,8,false);
        unsigned positive=0;
        for(unsigned i=0;i<8;++i){
            Access::chunk(a->machine,a->digital,3,i&1U,true);Access::chunk(b->machine,b->digital,3,i&1U,false);
            logical(*a,*b);Access::heldFF(a->machine);Access::heldFF(b->machine);
            const auto ca=Access::budget(a->machine),cb=Access::budget(b->machine);CHECK(ca==cb);
            CHECK(Access::pending(a->machine)==expected);if(ca[1])++positive;
        }
        CHECK(positive>0U);same(*a,*b);a->stop();b->stop();++cases;
    }
}
/** @brief F: настоящий CpuJammed выход FrameScope не оставляет долг владельца. */
void scopeJam(bool expected){const auto image=rom(true);auto a=std::make_unique<Fixture>(image);
    Access::prepare(a->machine,a->digital,9,0,0x041d,0,8,true);Access::realScope(a->machine);
    CHECK(a->machine.stepFrame()==RunStatus::CpuJammed);CHECK(!Access::pending(a->machine));
#if defined(NES_CORE_C1_COST_PROFILE)
    if(expected){const auto c=Access::counts(a->machine);CHECK(c[0]>0U&&c[2]>0U);}
#else
    (void)expected; // Счётчики выключены; закрытие долга всё равно проверено выше.
#endif
    a->stop();++cases;
}
} // namespace

/** @brief Запускает A–H; expected0/1 проверяет реальную PRIVATE активацию. */
int main(int argc,char** argv){CHECK(argc==2);CHECK(std::strcmp(argv[1],"0")==0||std::strcmp(argv[1],"1")==0);
    const bool expected=argv[1][0]=='1';const auto image=rom();
    phases(image,expected);cartCuts(image,expected);barriers(image,expected);domains(image,expected);capacity(image,expected);scopeJam(expected);endpointFallbacks(image,expected);heldHighCorners(image,expected);
    CHECK(activated==(expected?96U:0U));
    std::printf("MMC5_LAZY_DIRECTED_PASS cases=%u checks=%u activated=%u expected=%u\n",cases,checks,activated,unsigned(expected));
}
