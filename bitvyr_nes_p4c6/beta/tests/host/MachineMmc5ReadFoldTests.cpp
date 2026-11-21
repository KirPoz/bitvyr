/**
 * @file MachineMmc5ReadFoldTests.cpp
 * @brief Направленный oracle частного trusted MMC5 read-fold; собственные ROM.
 * @details Один host-поток. Runtime expected0/1 не зависит от PRIVATE consumer macro.
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
unsigned checks=0,positive=0,pureRejects=0,sourceRejects=0,dynamicRejects=0,actualCases=0,futureCases=0;
std::FILE* canonical=nullptr;
/** @brief Прекращает тест при первом расхождении, сохраняя точный номер проверки. */
[[noreturn]] void fail(const char* text,unsigned line){std::fprintf(stderr,"READ_FOLD line=%u check=%s\n",line,text);std::exit(1);}
#define CHECK(x) do{++checks;if(!(x))fail(#x,__LINE__);}while(false)
/** @brief Копирует representation для same-object const-call проверки, без толкования padding. */
template<class T>std::vector<unsigned char> bytes(const T& object){const auto* p=reinterpret_cast<const unsigned char*>(&object);return {p,p+sizeof(object)};}
/** @brief Дописывает числовые поля без указателей/выравнивания в необязательный canonical файл. */
void dump(const std::vector<std::uint64_t>& fields){if(!canonical)return;const auto count=std::uint32_t(fields.size());
    CHECK(std::fwrite(&count,sizeof(count),1,canonical)==1);CHECK(std::fwrite(fields.data(),sizeof(fields[0]),fields.size(),canonical)==fields.size());}
}

namespace nes::core {
/** @brief Использует существующую CPU friendship только для независимого host oracle. */
struct CpuQuietMmioTestAccess {
    /** @brief Задаёт synthetic addressing latch до начала проверяемого исполнения. */
    static void base(Cpu6502& cpu,std::uint16_t value){cpu.baseAddress_=value;}
    /** @brief Снимает архитектурно значимые private interrupt/address поля. */
    static std::vector<std::uint64_t> fields(const Cpu6502& c){return {c.baseAddress_,c.nmiPending_,c.nmiReady_,c.irqLine_,c.irqPending_,c.irqAtLastCycleStart_,c.nmiAtLastCycleStart_,c.inStep_};}
    /** @brief Вводит CPU event до старого admission, не вызывая unchecked helper. */
    static void reject(Cpu6502& c,unsigned kind){switch(kind){case 0:c.nmiPending_=true;break;case 1:c.nmiReady_=true;break;
        case 2:c.irqPending_=true;break;case 3:c.irqLine_=true;break;case 4:c.inStep_=true;break;default:CHECK(false);}}
};
/** @brief Доступ к private owner только у остановленного однопоточного fixture. */
struct MachineEventKernelTestAccess {
    /** @brief Заполняет начальный NT fixture ДО beginOutputFrame, без изменения guest clock. */
    static void initialVideoMemory(Machine& m){for(unsigned i=0;i<m.nametables_.size();++i)m.nametables_[i]=std::uint8_t(i*37U+11U);}

    /** @brief Дописывает реальные элементы массива, исключая padding и host pointers. */
    template<class T,std::size_t N>static void append(std::vector<std::uint64_t>& r,const std::array<T,N>& a){for(auto v:a)r.push_back(v);}
    /** @brief Делает настоящий шинный доступ с исходными M2/MMIO/mapper effects. */
    static void write(Machine& m,std::uint16_t a,std::uint8_t v){m.write(a,v);}
    /** @brief Ставит независимый scalar PPU prefix, затем CPU/RAM cut до grant. */
    static void prepare(Machine& m,Mmc5& d,std::uint16_t pc,std::uint16_t base,unsigned x,unsigned value){
        m.flushCpuReads();m.flushPpu();m.cpuReadBudget_=0;auto& p=m.timingPpu_;p.reset();p.bind(m.videoPacket_);
        p.scanline_=12;p.control_=0x10;p.mask_=0x1e;p.renderingEnabled_=p.renderingNext_=true;p.v_=p.t_=0x041d;
        for(unsigned i=0;i<p.palette_.size();++i){p.palette_[i]=std::uint8_t(i+1U);CHECK(m.videoPacket_->add(VideoOp::PaletteWrite,i,0,0,i+1U));}
        // Initial synthetic NT задана до begin/seed; дальнейшие stores остаются реальными.
        d.cpuWrite(0x2000,p.control_);d.cpuWrite(0x2001,p.mask_);d.cpuWrite(0x5104,0);d.cpuWrite(0x5105,0x44);d.cpuWrite(0x5200,0);
        d.ppuReadStrobe(0x2000);d.ppuReadStrobe(0x2000);d.ppuReadStrobe(0x2000);d.ppuReadStrobe(0);while(p.dot_<41U)p.tick();
        d.cpuIdle_=0;d.ppuReadSinceM2_=true;const auto address=std::uint16_t(base+x);
        if(address<0x2000U)m.ram_[address&2047U]=std::uint8_t(value);else if(address>=0x6000U&&address<0x8000U)d.cpuWrite(address,std::uint8_t(value));
        auto s=m.cpu_.state();s.pc=pc;s.a=std::uint8_t(value);s.x=std::uint8_t(x);s.y=0x6a;s.sp=0xc7;s.p=std::uint8_t(0x25U|(value&128U));
        s.cycles=m.cpuCycles();s.jammed=false;m.cpu_.setState(s);CpuQuietMmioTestAccess::base(m.cpu_,base);m.mmc5ReadQuantum_={};m.pendingCpuReads_=m.cpuReadBudget_=0;
    }
    /** @brief Создаёт только valid borrowed context для прямых pure range/fixed-point tests. */
    static void certificate(Machine& m,Mmc5& d){CHECK(m.mmc5QuietFramePrerequisites());m.mmc5ReadQuantum_.active=true;
        m.mmc5ReadQuantum_.frameQualified=true;m.mmc5ReadQuantum_.deadline=m.cpuCycles()+1000U;m.mmc5ReadQuantum_.ram6000=d.cpu6000Page();m.cpuReadBudget_=80;}
    /** @brief Получает final branch dummy через текущий execution view, без read-strobe. */
    static std::uint8_t open(Machine& m,std::uint16_t pc,std::uint16_t branch){const auto after=std::uint16_t(branch+2U);
        const auto dummy=std::uint16_t((after&0xff00U)!=(pc&0xff00U)?(after&0xff00U)|(pc&255U):after);return m.quietCpuReadPages()[(dummy>>13U)&3U][dummy&8191U];}
    /** @brief Упаковывает scalar witness; никакой адрес CPU State в helper не передаётся. */
    static unsigned fold(Machine& m,const Cpu6502::State& s,std::uint16_t base,std::uint8_t open,std::uint16_t branch,unsigned bc,unsigned remaining,unsigned badPointer=0){
        const auto pcAx=std::uint32_t(s.pc)|(std::uint32_t(s.a)<<16U)|(std::uint32_t(s.x)<<24U);
        const auto baseOpenP=(std::uint32_t(base)<<16U)|(std::uint32_t(open)<<8U)|s.p;
        const auto info=std::uint32_t(branch)|(bc<<16U)|(std::uint32_t(s.jammed)<<24U);
        return m.mmc5LocalReadFold(pcAx,baseOpenP,info,remaining,badPointer==1?nullptr:m.ram_.data(),
            badPointer==2?nullptr:m.mmc5ReadQuantum_.ram6000,badPointer==3?nullptr:m.quietCpuReadPages());}
    /** @brief Проверяет once-frame source boundary, не изменяя state/certificates. */
    static bool source(Machine& m){return m.mmc5ReadFoldFrameSource();}
    /** @brief Исполняет actual entry со старым admission и FrameScope; guard cases должны вернуть0. */
    static unsigned run(Machine& m,unsigned budget){Machine::Mmc5ReadQuantumFrameScope scope(m,m.cpuCycles()+1000U);return m.runMmc5QuietBlock(budget);}
    /** @brief Независимый обычный interpreter; не создаёт FrameScope или новый grant. */
    static unsigned step(Machine& m){return m.cpu_.step();}
    /** @brief Материализует только уже завершённые CPU/PPU долги. */
    static void flush(Machine& m){m.flushCpuReads();m.flushPpu();}
    /** @brief Снимает synthetic certificate перед штатным stop/cancel, без гостевых clocks. */
    static void clear(Machine& m){m.mmc5ReadQuantum_={};m.pendingCpuReads_=m.cpuReadBudget_=0;}
    /** @brief Делает первый LDA наблюдаемым, изменяя A/N перед началом actual исполнения. */
    static void initial(Machine& m){auto s=m.cpu_.state();s.a^=0x5aU;s.p^=128U;m.cpu_.setState(s);}
    /** @brief Меняет data единственным владельцем после grant, чтобы закончить polling loop. */
    static void zero(Machine& m,Mmc5& d,std::uint16_t a){if(a<0x2000U)m.ram_[a&2047U]=0;else d.cpuWrite(a,0);}
    /** @brief Даёт настоящее NMI после закрытого grant, сохраняя прежнюю CPU edge-модель. */
    static void nmi(Machine& m){m.cpu_.requestNmi();}
    /** @brief Настраивает читаемость/защиту либо PRG-RAM code через настоящий mapper decoder. */
    static void mapping(Machine& m,Mmc5& d,unsigned kind){if(kind==0){m.cartridge_.cpuWrite(0x5102,0,m.cpuCycles());m.cartridge_.cpuWrite(0x5103,0,m.cpuCycles());}
        else if(kind==1){m.cartridge_.cpuWrite(0x5114,0,m.cpuCycles());}else{m.cartridge_.cpuWrite(0x5113,4,m.cpuCycles());}
        m.mmc5ReadQuantum_.ram6000=d.cpu6000Page();}
    /** @brief Вводит source identity corruption до реального FrameScope; pure helper не вызывается. */
    static void sourceBad(Machine& m,unsigned kind){switch(kind){case 0:m.outputTicket_.lease.generation++;break;case 1:m.outputTicket_.lease.romEpoch++;break;
        case 2:m.outputTicket_.lease.transportEpoch++;break;case 3:m.outputTicket_.lease.rom++;break;case 4:m.outputTicket_.lease.romBytes--;break;
        case 5:m.outputTicket_.lease.producer=nullptr;break;case 6:m.outputLease_.region=Region::Pal;break;case 7:m.outputTicket_.slot=2;break;
        case 8:m.outputTicket_.sequence++;break;case 9:m.outputLease_.rom++;m.outputTicket_.lease=m.outputLease_;break;
        case 10:m.outputLease_.romBytes--;m.outputTicket_.lease=m.outputLease_;break;case 11:m.outputLease_.romBytes=~std::size_t{0};m.outputTicket_.lease=m.outputLease_;break;
        case 12:m.outputLease_.rom=nullptr;m.outputTicket_.lease=m.outputLease_;break;case 13:m.timingPpu_.packet_=m.outputBinding_.packets[1];break;
        case 14:m.outputTicket_.sequence=1;m.videoSequence_=1;const_cast<VideoPacket::Header&>(m.videoPacket_->header()).sequence=1;break;default:CHECK(false);}}
    /** @brief Вводит dynamic event до old admission, а не после доверенной квалификации. */
    static void dynamicBad(Machine& m,Mmc5& d,unsigned kind){switch(kind){case 0:m.outputOwner_=false;break;case 1:m.outputJob_=false;break;
        case 2:m.outputInitializing_=true;break;case 3:m.outputSealed_=true;break;case 4:m.dmaPending_=true;break;case 5:m.dmcStalls_=1;break;
        case 6:m.pendingPpuDots_=1;break;case 7:m.deferPpuAllowed_=true;break;case 8:m.videoPacket_->fault(VideoFault::Capacity);break;
        case 9:const_cast<AudioPacket::Header&>(m.audioPacket_->header()).fault=AudioFault::DmcOrder;break;case 10:m.timingPpu_.nmiPending_=true;break;
        case 11:m.timingPpu_.frameReady_=true;break;case 12:m.timingApu_.advanceCpuCycles(1U);break;
        case 13:{static TimingPpu::Capture capture{};m.timingPpu_.capture_=&capture;break;}case 14:m.videoObservableOnly_=true;break;
        case 15:d.irqPending_=d.irqEnabled_=true;break;case 16:m.cartridge_.cpuWrite(0x5010,1,m.cpuCycles());break;default:CHECK(false);}}
    /** @brief Устанавливает private CPU event перед неизменённым old admission. */
    static void cpuBad(Machine& m,unsigned kind){CpuQuietMmioTestAccess::reject(m.cpu_,kind);}
    /** @brief Снимает CPU/RAM и detector/PPU поля, определяющие следующее наблюдаемое чтение. */
    static std::vector<std::uint64_t> fields(const Machine& m,const Mmc5& d){const auto& s=m.cpu_.state();const auto& p=m.timingPpu_;
        auto r=CpuQuietMmioTestAccess::fields(m.cpu_);r.insert(r.end(),{s.pc,s.a,s.x,s.y,s.sp,s.p,s.cycles,s.jammed,m.cpuCycles(),m.openBus_,m.pendingCpuReads_,m.cpuReadBudget_,
            m.dmaPending_,m.dmcStalls_,m.pendingPpuDots_,m.deferPpuAllowed_,p.dot_,std::uint64_t(std::int64_t(p.scanline_)),p.frames_,p.control_,p.mask_,p.status_,
            p.v_,p.t_,p.openBus_,p.readBuffer_,p.busAddress_,p.oamAddress_,p.fineX_,p.writeToggle_,p.nmiPending_,p.nmiLine_,p.frameReady_,
            d.prgMode_,d.chrMode_,d.protect1_,d.protect2_,d.lastPpuAddress_,d.nameRepeat_,d.cpuIdle_,d.ppuReadSinceM2_,d.inFrame_,d.scanline_,d.irqPending_,d.irqEnabled_,d.splitY_,d.tileExtended_,d.tileSplit_});
        append(r,m.ram_);for(unsigned a=0x6000;a<0x8000;++a)r.push_back(d.cpuPeek(std::uint16_t(a),m.openBus_));append(r,d.exRam_);append(r,d.prgBanks_);append(r,d.chrBanks_);
        append(r,p.palette_);append(r,p.oam_);append(r,p.secondary_);append(r,p.secondaryIndex_);append(r,p.spriteRow_);return r;}
    /** @brief Завершает Recording draw-prefix перед независимым canonical observer. */
    static void pixels(Machine& m){m.timingPpu_.flushPixels();}
};
}

namespace {
using namespace nes::core;using Access=MachineEventKernelTestAccess;
/** @brief Параметры собственной ROM; PC не является условием production оптимизации. */
struct Shape{std::uint16_t pc,base;unsigned x,value;bool zp;};
/** @brief Владеет однопоточными host ресурсами. ROM заимствована до конца fixture. */
struct Fixture{Mmc5 digital;Mmc5Audio expansion;std::array<VideoPacket,2> packets;std::array<std::array<std::uint8_t,10240>,2> ram{};AudioPacket audio;OutputFrameLease lease{};Machine machine;
    /** @brief Привязывает полный public Output протокол без внешнего Worker/consumer. */
    explicit Fixture(const std::vector<std::uint8_t>& rom){machine.configureMmc5(&digital,&expansion);CHECK(machine.load(rom.data(),rom.size(),Region::Ntsc)==LoadResult::Ok);machine.configureSplitAudio(&audio,true);
        OutputFrameBinding b{};b.source={&machine,rom.data(),rom.size(),machine.colourTransportEpoch(),1,0,Region::Ntsc};b.packets={&packets[0],&packets[1]};b.snapshots={ram[0].data(),ram[1].data()};
        b.protectedSpans[0]={&audio,sizeof(audio)};b.protectedSpans[1]={&digital,sizeof(digital)};b.protectedSpans[2]={&expansion,sizeof(expansion)};CHECK(machine.configureOutputFrames(b,lease));Access::initialVideoMemory(machine);
        CHECK(machine.beginOutputFrame({lease,0,0,machine.cpuCycles(),0}));for(unsigned i=0;i<4;++i)Access::write(machine,std::uint16_t(0x5114U+i),std::uint8_t(0x80U+i));Access::write(machine,0x5102,2);Access::write(machine,0x5103,1);}
    /** @brief Закрывает valid fixture после синхронного completion; corruption fixtures не публикуются. */
    void stop(){Access::clear(machine);CHECK(machine.stopOutputWriter(lease));CHECK(machine.cancelOutputFrames(lease));}
};
/** @brief Создаёт собственный32KiB PRG/8KiB CHR, loop и настоящий MMIO/NMI continuation. */
std::vector<std::uint8_t> rom(const Shape& s){std::vector<std::uint8_t> r(16+32768+8192,0xea);std::fill(r.begin(),r.begin()+16,0);
    r[0]='N';r[1]='E';r[2]='S';r[3]=0x1a;r[4]=2;r[5]=1;r[6]=0x50;r[7]=8;r[10]=7;const unsigned len=s.zp?2U:3U,p=16U+s.pc-0x8000U;
    r[p]=s.zp?0xa5:0xbd;r[p+1]=std::uint8_t(s.base);if(!s.zp){r[p+2]=std::uint8_t(s.base>>8U);}r[p+len]=0xd0;r[p+len+1]=std::uint8_t(-int(len)-2);
    const std::array<std::uint8_t,10> tail={{0xad,2,0x20,0xad,7,0x20,0xea,0xea,0xea,0xea}};
    if(p+len+2U+tail.size()<16U+32768U){std::memcpy(r.data()+p+len+2U,tail.data(),tail.size());}const auto a=std::uint16_t(s.base+s.x);
    if(a>=0x8000U&&a<0xfffaU){r[16U+a-0x8000U]=std::uint8_t(s.value);}r[16U+0x6100U]=0xa9;r[16U+0x6101U]=0x42;r[16U+0x6102U]=0x40;
    for(unsigned a0:{0x7ffaU,0x7ffeU}){r[16+a0]=0;r[16+a0+1]=0xe1;}r[16U+0x7ffcU]=0;r[16U+0x7ffdU]=0xe0;
    for(unsigned i=0;i<8192;++i){r[16U+32768U+i]=std::uint8_t(i*29U+19U);}return r;}
/** @brief Вычисляет реальные body/branch clocks для assertions, не продвигая guest. */
unsigned period(const Shape& s){const auto branch=std::uint16_t(s.pc+(s.zp?2U:3U)),a=std::uint16_t(s.base+s.x);return(s.zp?3U:4U+unsigned((s.base&0xff00U)!=(a&0xff00U)))+3U+unsigned(((branch+2U)&0xff00U)!=(s.pc&0xff00U));}
/** @brief Вызывает pure helper на valid certificate и проверяет неизменность всех объектов. */
unsigned pure(Fixture& f,const Cpu6502::State& s,std::uint16_t base,std::uint8_t open,std::uint16_t branch,unsigned bc,unsigned remaining,unsigned badPointer=0){
    const auto m=bytes(f.machine),d=bytes(f.digital),a=bytes(f.audio),p=bytes(f.packets);std::vector<VideoEvent> arena(f.packets[0].events(),f.packets[0].events()+VideoPacket::eventCapacity);
    const auto n=Access::fold(f.machine,s,base,open,branch,bc,remaining,badPointer);CHECK(m==bytes(f.machine));CHECK(d==bytes(f.digital));CHECK(a==bytes(f.audio));CHECK(p==bytes(f.packets));
    CHECK(std::memcmp(arena.data(),f.packets[0].events(),arena.size()*sizeof(VideoEvent))==0);return n;}
/** @brief Сравнивает точные CPU/latch/RAM и audio records после прежнего flush. */
void equal(Fixture& a,Fixture& b){Access::flush(a.machine);Access::flush(b.machine);CHECK(Access::fields(a.machine,a.digital)==Access::fields(b.machine,b.digital));
    const auto& x=a.audio.header();const auto& y=b.audio.header();CHECK(x.baseCycle==y.baseCycle&&x.inputCount==y.inputCount&&x.dmcCount==y.dmcCount&&x.expansionCount==y.expansionCount);
    CHECK(std::memcmp(a.audio.inputs(),b.audio.inputs(),x.inputCount*sizeof(AudioPacket::Input))==0);CHECK(std::memcmp(a.audio.dmc(),b.audio.dmc(),x.dmcCount*sizeof(AudioPacket::Dmc))==0);
    CHECK(std::memcmp(a.audio.expansions(),b.audio.expansions(),x.expansionCount*sizeof(AudioPacket::Expansion))==0);}
/** @brief Сравнивает decoded индексы/RGB и сохраняет переносимый endpoint fingerprint. */
void video(Fixture& a,Fixture& b){Access::pixels(a.machine);Access::pixels(b.machine);CHECK(a.packets[0].readRecording());CHECK(b.packets[0].readRecording());
    auto sa=std::make_unique<VideoStorage>(),sb=std::make_unique<VideoStorage>();VideoRenderer va(*sa),vb(*sb);std::vector<std::uint16_t> ra(256U*240U),rb(256U*240U);
    va.setRgb565Output(ra.data());vb.setRgb565Output(rb.data());CHECK(va.observeVideo(a.packets[0]));CHECK(vb.observeVideo(b.packets[0]));CHECK(sa->indices==sb->indices);CHECK(ra==rb);
    CHECK(std::any_of(sa->indices.begin(),sa->indices.end(),/** @brief Возвращает true для ненулевого decoded pixel index. Используется any_of как независимый свидетель непустого рисунка; пиксель передан по значению, память renderer не изменяется. */ [](std::uint8_t v){return v!=0;}));std::vector<std::uint64_t> r;for(auto v:sa->indices)r.push_back(v);for(auto v:ra)r.push_back(v);dump(r);
    dump(Access::fields(a.machine,a.digital));const auto& h=a.audio.header();dump({h.baseCycle,h.inputCount,h.dmcCount,h.expansionCount});
    for(unsigned i=0;i<h.inputCount;++i){const auto& v=a.audio.inputs()[i];dump({v.offset,v.operation,v.value});}for(unsigned i=0;i<h.dmcCount;++i){const auto& v=a.audio.dmc()[i];dump({v.offset,v.address,v.value,v.stall});}
    for(unsigned i=0;i<h.expansionCount;++i){const auto& v=a.audio.expansions()[i];dump({v.offset,v.nativePrefix,std::uint64_t(std::int64_t(v.level))});}}
/** @brief Проверяет positive windows, min4/remaining36 bounds и защищённую читаемую WRAM. */
void positives(int enabled){const std::array<Shape,10> shapes={{{0xe000,0xe2,0,0x11,true},{0xe000,0xe2,0,0x80,true},{0xe000,0x6000,0xc1,0x5a,false},
    {0xe000,0x60ff,1,0x80,false},{0xe000,0x18e1,1,0xff,false},{0xe000,0xfff9,7,0x11,false},{0x80fc,0xe2,0,0x11,true},
    {0x80fb,0x60ff,1,0x80,false},{0xe000,0xc040,0,0x5a,false},{0xe000,0xe040,0,0x5a,false}}};
    for(const auto& s:shapes){const auto image=rom(s);auto f=std::make_unique<Fixture>(image);Access::prepare(f->machine,f->digital,s.pc,s.base,s.x,s.value);CHECK(Access::source(f->machine)==bool(enabled));Access::certificate(f->machine,f->digital);
        const auto state=f->machine.cpuState();const auto branch=std::uint16_t(s.pc+(s.zp?2U:3U));const unsigned bc=3U+unsigned(((branch+2U)&0xff00U)!=(s.pc&0xff00U));const auto open=Access::open(f->machine,s.pc,branch);const unsigned p=period(s);
        for(unsigned remaining:{35U,36U,4U*p-1U,4U*p,4U*p+1U,80U,81U}){const unsigned expected=enabled&&remaining>=36U&&remaining<=80U&&remaining/p>=4U?(remaining/p)*p:0;
            CHECK(pure(*f,state,s.base,open,branch,bc,remaining)==expected);++positive;}Access::mapping(f->machine,f->digital,0);CHECK(pure(*f,state,s.base,open,branch,bc,80U)==(enabled?(80U/p)*p:0));++positive;f->stop();}}
/** @brief Проверяет только local pure-contract отказы на действительном source/owner сертификате. */
void pureBad(){const Shape good{0x8000,0x6000,0xc1,0x5a,false};
    for(unsigned kind=0;kind<12U;++kind){auto image=rom(good);if(kind==10)image[16]=0xae;if(kind==11)image[16+4]=0;
        auto f=std::make_unique<Fixture>(image);Access::prepare(f->machine,f->digital,good.pc,good.base,good.x,good.value);Access::certificate(f->machine,f->digital);
        auto state=f->machine.cpuState();auto base=good.base;auto open=Access::open(f->machine,good.pc,good.pc+3U);unsigned bc=3,remaining=42,pointer=0;
        switch(kind){case 0:state.a^=1;break;case 1:state.p|=2;break;case 2:state.p^=128;break;case 3:base++;break;case 4:open^=1;break;case 5:bc=4;break;
        case 6:pointer=1;break;case 7:pointer=2;break;case 8:pointer=3;break;case 9:state.jammed=true;break;default:break;}
        CHECK(pure(*f,state,base,open,good.pc+3U,bc,remaining,pointer)==0);++pureRejects;f->stop();}
    const std::array<Shape,8> bad={{{0xe000,0x1fff,1,0x11,false},{0xe000,0x5fff,1,0x11,false},{0xe000,0xfff9,1,0x11,false},{0xe000,0xfffa,1,0x11,false},
        {0xe000,0x9fff,1,0x11,false},{0x9ffd,0xe2,0,0x11,false},{0xfff7,0xe2,0,0x11,true},{0xe000,0xe2,0,0,true}}};
    for(const auto& s:bad){const auto image=rom(s);auto f=std::make_unique<Fixture>(image);Access::prepare(f->machine,f->digital,s.pc,s.base,s.x,s.value);Access::certificate(f->machine,f->digital);
        const auto branch=std::uint16_t(s.pc+(s.zp?2U:3U));CHECK(pure(*f,f->machine.cpuState(),s.base,Access::open(f->machine,s.pc,branch),branch,3U,72U)==0);++pureRejects;f->stop();}
    for(unsigned kind=1;kind<=2;++kind){const auto image=rom(good);auto f=std::make_unique<Fixture>(image);Access::prepare(f->machine,f->digital,good.pc,good.base,good.x,good.value);Access::certificate(f->machine,f->digital);
        const auto state=f->machine.cpuState();const auto open=Access::open(f->machine,good.pc,good.pc+3U);Access::mapping(f->machine,f->digital,kind);
        CHECK(pure(*f,state,good.base,open,good.pc+3U,3U,42U)==0);++pureRejects;f->stop();}}
/** @brief Проверяет source перед Scope, а события — старым actual admission; helper не вызывается. */
void boundaries(int enabled){const Shape s{0xe000,0x6000,0xc1,0x5a,false};
    for(unsigned kind=0;kind<15U;++kind){const auto image=rom(s);auto f=std::make_unique<Fixture>(image);Access::prepare(f->machine,f->digital,s.pc,s.base,s.x,s.value);Access::sourceBad(f->machine,kind);
        const auto before=bytes(f->machine);CHECK(!Access::source(f->machine));CHECK(before==bytes(f->machine));if(enabled){CHECK(Access::run(f->machine,49U)==0);CHECK(before==bytes(f->machine));}++sourceRejects;}
    for(unsigned kind=0;kind<22U;++kind){const auto image=rom(s);auto f=std::make_unique<Fixture>(image);Access::prepare(f->machine,f->digital,s.pc,s.base,s.x,s.value);
        if(kind<17U)Access::dynamicBad(f->machine,f->digital,kind);else Access::cpuBad(f->machine,kind-17U);const auto before=bytes(f->machine),audio=bytes(f->audio),digital=bytes(f->digital);
        CHECK(Access::run(f->machine,49U)==0);CHECK(before==bytes(f->machine));CHECK(audio==bytes(f->audio));CHECK(digital==bytes(f->digital));++dynamicRejects;}}
/** @brief Сравнивает первый real pair, bounded folded suffix и будущие MMIO/NMI/DMC edges. */
void execution(){const std::array<Shape,7> shapes={{{0xe000,0xe2,0,0x11,true},{0xe000,0x6000,0xc1,0x5a,false},{0xe000,0x60ff,1,0x80,false},
    {0xe000,0x18e1,1,0xff,false},{0x80fc,0xe2,0,0x11,true},{0x80fb,0x60ff,1,0x80,false},{0x9ffd,0xe2,0,0x11,false}}};
    for(const auto& s:shapes)for(unsigned future=0;future<3U;++future){const auto image=rom(s);auto a=std::make_unique<Fixture>(image),b=std::make_unique<Fixture>(image);
        Access::prepare(a->machine,a->digital,s.pc,s.base,s.x,s.value);Access::prepare(b->machine,b->digital,s.pc,s.base,s.x,s.value);Access::initial(a->machine);Access::initial(b->machine);
        const unsigned p=period(s),n=(36U+p-1U)/p,budget=(n+1U)*p;
        CHECK(Access::run(a->machine,budget)==budget);
        for(unsigned i=0;i<2U*(n+1U);++i){Access::step(b->machine);}equal(*a,*b);++actualCases;const auto address=std::uint16_t(s.base+s.x);Access::zero(a->machine,a->digital,address);Access::zero(b->machine,b->digital,address);
        for(unsigned i=0;i<4;++i){CHECK(Access::step(a->machine)==Access::step(b->machine));equal(*a,*b);}
        if(future==0){Access::write(a->machine,0x5113,0);Access::write(b->machine,0x5113,0);Access::write(a->machine,0x5127,7);Access::write(b->machine,0x5127,7);}
        else if(future==1){Access::nmi(a->machine);Access::nmi(b->machine);for(unsigned i=0;i<5;++i){CHECK(Access::step(a->machine)==Access::step(b->machine));}}
        else{for(const auto& w:std::array<std::pair<std::uint16_t,std::uint8_t>,5>{{{0x4010,0x8f},{0x4011,0x20},{0x4012,0},{0x4013,0},{0x4015,0x10}}}){Access::write(a->machine,w.first,w.second);Access::write(b->machine,w.first,w.second);}
            for(unsigned i=0;i<40;++i){CHECK(Access::step(a->machine)==Access::step(b->machine));}CHECK(a->audio.header().dmcCount>0);}
        equal(*a,*b);video(*a,*b);++futureCases;a->stop();b->stop();}}
}

/** @brief Запускает один permanent TU: expected0/1, необязательный canonical path. */
int main(int argc,char** argv){CHECK(argc>=2);const int enabled=std::atoi(argv[1]);CHECK(enabled==0||enabled==1);
    CHECK(argc==2||argc==3);if(argc==3){canonical=std::fopen(argv[2],"wb");CHECK(canonical);}
    positives(enabled);pureBad();boundaries(enabled);execution();if(canonical){CHECK(std::fclose(canonical)==0);}
    std::printf("MMC5_READ_FOLD05_PASS positive=%u pure_reject=%u source_reject=%u dynamic_reject=%u actual=%u future=%u checks=%u\n",positive,pureRejects,sourceRejects,dynamicRejects,actualCases,futureCases,checks);return 0;}
