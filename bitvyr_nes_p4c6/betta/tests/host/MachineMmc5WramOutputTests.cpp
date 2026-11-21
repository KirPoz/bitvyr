/**
 * @file MachineMmc5WramOutputTests.cpp
 * @brief Собственная MMC5 ROM проверяет WRAM и admission внутри настоящего Output writer.
 * @details Один executable проверяет независимые известные результаты программы.
 * Необязательный файл содержит canonical CPU/RAM/cuts/RGB/PCM для точного
 * сравнения разных executable: baseline, WRAM и frame-admission. Это не FPS.
 */
#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "apu/AudioRenderer.h"
#include "ppu/VideoRenderer.h"
#include "cpu/RomWindow.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using namespace nes::core;
namespace {
unsigned checks=0;
std::FILE* output=nullptr;
/** @brief Останавливает fixture с исходной строкой нарушенного наблюдаемого контракта. */
[[noreturn]] void fail(const char* text,unsigned line){std::fprintf(stderr,"MMC5 WRAM line %u: %s\n",line,text);std::exit(1);}
#define CHECK(value) do{++checks;if(!(value))fail(#value,__LINE__);}while(false)
/**
 * @brief Минимальный assembler только собственных коротких тестовых программ.
 * @details Все allocations происходят до запуска Machine; fixups используют
 * абсолютные 16-битные CPU-адреса. Сгенерированные ROM не заимствуют чужой код.
 */
struct Program {
    std::vector<std::uint8_t> code;
    /** @brief Публикует очередной opcode/operand байт до начала исполнения. */
    void byte(unsigned value){code.push_back(static_cast<std::uint8_t>(value));}
    /** @brief Добавляет opcode с одним байтом immediate/zero-page operand. */
    void pair(unsigned op,unsigned value){byte(op);byte(value);}
    /** @brief Добавляет opcode с little-endian абсолютным адресом. */
    void absolute(unsigned op,unsigned address){byte(op);byte(address);byte(address>>8U);}
    /** @brief Записывает константу через настоящие LDA/STA, включая все MMIO-барьеры. */
    void set(unsigned address,unsigned value){pair(0xa9,value);absolute(0x8d,address);}
    /** @brief Возвращает текущий адрес кода, размещаемого в последнем окне $E000. */
    unsigned pc()const noexcept{return 0xe000U+static_cast<unsigned>(code.size());}
};
/**
 * @brief Собирает шесть сценариев WRAM/protection/banking/IRQ/DMC/self-modification.
 * @param variant 0 — обычные read/store/RMW, 1 — protection/banks/open bus,
 * 2 — NMI/MMC5/APU IRQ и DMC, 3 — изменяемый WRAM-код, 4 — динамика MMIO,
 * 5 — физически отсутствующая RAM с открытой шиной.
 * @return Владеющие байты NES2 ROM с 8 KiB SRAM, кроме явного no-RAM сценария.
 */
std::vector<std::uint8_t> image(unsigned variant){
    std::vector<std::uint8_t> rom(16+32768+8192,0);
    rom[0]='N';rom[1]='E';rom[2]='S';rom[3]=0x1a;rom[4]=2;rom[5]=1;
    rom[6]=0x50;rom[7]=0x08;rom[10]=variant==5?0:7;
    Program p;p.byte(0x78);p.byte(0xd8);p.set(0x4017,0x40);
    p.set(0x5114,0x80);p.set(0x5115,0x81);p.set(0x5116,0x82);p.set(0x5117,0x83);
    p.set(0x5102,2);p.set(0x5103,1);p.set(0x5113,0);
    p.set(0x4015,1);p.set(0x4000,0xbf);p.set(0x4002,0x40);p.set(0x4003,0xf8);
    p.set(0x2000,variant==2?0x80:0);p.set(0x2001,0x18);
    if(variant==2){
        p.set(0x5203,120);p.set(0x5204,0x80);p.set(0x4010,0x8f);
        p.set(0x4012,0);p.set(0x4013,1);p.set(0x4015,0x11);p.byte(0x58);
    }
    if(variant==3){p.set(0x6100,0xa9);p.set(0x6102,0x60);}
    const unsigned loop=p.pc();
    if(variant==1){
        p.set(0x5113,0);p.set(0x6000,0x39);p.set(0x5103,0);p.set(0x6000,0xe7);
        p.absolute(0xad,0x6000);p.pair(0x85,0x20); // Защита сохраняет39.
        // У8KiB платы bank1 зеркалит bank0, а bit2 выбирает неподключённый CE.
        p.set(0x5103,1);p.set(0x5113,1);p.set(0x6000,0x77);
        p.absolute(0xad,0x6000);p.pair(0x85,0x21);
        p.set(0x5113,4);p.set(0x6000,0x11);p.absolute(0xad,0x6000);p.pair(0x85,0x22);
        p.set(0x5113,0);p.absolute(0xad,0x6000);p.pair(0x85,0x23);
        p.set(0x5102,0);p.set(0x6000,0xfe);p.absolute(0xad,0x6000);p.pair(0x85,0x24);
        p.set(0x5102,2);
    }else if(variant==5){
        p.set(0x6000,0xfe);p.absolute(0xad,0x6000);p.pair(0x85,0x22);
        p.absolute(0xee,0x6000);p.absolute(0xad,0x6000);p.pair(0x85,0x23);
    }else{
        // На WRAM RMW должны остаться два M2 записи; $60FF,X даёт настоящий
        // page-cross dummy, $7FFF,X пересекает WRAM/PRG без смешения backing.
        p.absolute(0xee,0x6000);p.absolute(0xad,0x6000);p.pair(0x49,0x55);p.absolute(0x8d,0x6001);
        p.pair(0xa2,1);p.absolute(0xbd,0x60ff);p.absolute(0x9d,0x61ff);
        p.absolute(0x1e,variant==3?0x62ff:0x60ff);p.absolute(0xbd,0x7fff);p.pair(0x85,0x25);
        if(variant==3){
            p.absolute(0xad,0x6000);p.absolute(0x8d,0x6101);p.absolute(0x20,0x6100);
            p.pair(0x85,0x26); // RTS возвращает изменённый immediate из WRAM.
        }
        if(variant==4){
            p.set(0x5010,1);p.absolute(0xad,0x8000);p.set(0x5010,0);
            p.set(0x2001,0);p.byte(0xea);p.set(0x2001,0x18);
            p.set(0x5113,1);p.set(0x6002,0x69);p.set(0x5113,0);
        }
    }
    p.pair(0xe6,0x27);p.byte(0xea);p.byte(0xea);p.byte(0xea);p.absolute(0x4c,loop);
    CHECK(p.code.size()<0x600);
    std::memcpy(rom.data()+16+0x6000,p.code.data(),p.code.size());
    const std::uint8_t nmi[]={0xe6,0x10,0x40};
    const std::uint8_t irq[]={0x48,0xad,0x04,0x52,0xad,0x15,0x40,0xa9,0x11,0x8d,0x15,0x40,0xe6,0x11,0x68,0x40};
    std::memcpy(rom.data()+16+0x6600,nmi,sizeof(nmi));std::memcpy(rom.data()+16+0x6610,irq,sizeof(irq));
    rom[16+0x7ffa]=0;rom[16+0x7ffb]=0xe6;
    rom[16+0x7ffc]=0;rom[16+0x7ffd]=0xe0;
    rom[16+0x7ffe]=0x10;rom[16+0x7fff]=0xe6;
    for(unsigned i=0;i<8192;++i)rom[16+32768+i]=static_cast<std::uint8_t>((i&8)?0xaa:0x55);
    return rom;
}
/**
 * @brief Владеет independent Output producer/consumer до отмены joined аренды.
 * @details Machine уничтожается первой; ROM и пакеты живут до всех кадров и cancel.
 */
struct Fixture {
    Mmc5 digital;Mmc5Audio expansion;
#if defined(NES_CORE_VIDEO_HOT_METADATA) && NES_CORE_VIDEO_HOT_METADATA
    std::array<VideoPacket::HotState,2> hotStates{}; // Живут дольше пакетов при destructor unwind.
#endif
    std::array<VideoPacket,2> packets;
    std::array<std::array<std::uint8_t,10240>,2> ram{};
    AudioPacket audio,retired;AudioRenderer sound;
    VideoStorage videoStorage;VideoRenderer video{videoStorage};
    std::array<std::uint16_t,256*240> rgb{};
    OutputFrameBinding binding{};OutputFrameLease lease{};OutputFrameTicket ticket{};OutputFrameCut cut{};
    const VideoPacket* sealed=nullptr;std::uint64_t dmc=0;
    Machine machine;
    /** @brief Привязывает заимствованные ROM/Output/audio ranges до первого frame. */
    void init(const std::vector<std::uint8_t>& rom){
        machine.configureMmc5(&digital,&expansion);CHECK(machine.load(rom.data(),rom.size(),Region::Ntsc)==LoadResult::Ok);
        
        machine.configureSplitAudio(&audio,true);
        
        binding.source={&machine,rom.data(),rom.size(),machine.colourTransportEpoch(),1,0,Region::Ntsc};
        binding.packets={&packets[0],&packets[1]};binding.snapshots={ram[0].data(),ram[1].data()};
        binding.protectedSpans[0]={&audio,sizeof(audio)};binding.protectedSpans[1]={&retired,sizeof(retired)};
        binding.protectedSpans[2]={&digital,sizeof(digital)};binding.protectedSpans[3]={&expansion,sizeof(expansion)};
        binding.protectedSpans[4]={rgb.data(),sizeof(rgb)};binding.protectedSpans[5]={&videoStorage,sizeof(videoStorage)};
        binding.protectedSpans[6]={&video,sizeof(video)};
#if defined(NES_CORE_VIDEO_HOT_METADATA) && NES_CORE_VIDEO_HOT_METADATA
        CHECK(packets[0].bindHotState(&hotStates[0]));CHECK(packets[1].bindHotState(&hotStates[1]));
        OutputFrameLease unregistered{};
        CHECK(!machine.configureOutputFrames(binding,unregistered));CHECK(machine.cpuCycles()==7);
        binding.protectedSpans[7]={hotStates.data(),sizeof(hotStates)};
#endif
        
        CHECK(machine.configureOutputFrames(binding,lease));video.setRgb565Output(rgb.data());
    }
    /** @brief Исполняет один guest frame и синхронно потребляет RGB/PCM после Worker return. */
    void frame(unsigned n){
        machine.setController(0,static_cast<std::uint8_t>(n));ticket={lease,n,n,machine.cpuCycles(),n&1U};
        CHECK(machine.beginOutputFrame(ticket));CHECK(machine.stepFrame()==RunStatus::FrameReady);
        CHECK(machine.sealOutputFrame(ticket,sealed,cut));CHECK(machine.copyOutputVisibleRam(ticket,ram[n&1U].data(),10240));
        CHECK(machine.retireAudioPacket(retired));CHECK(video.render(*sealed)==VideoRenderResult::Ok);
        CHECK(sound.render(retired)==AudioRenderResult::Ok);dmc+=retired.header().dmcCount;
        
    }
};
/** @brief Записывает ровно заданные уже остановленные canonical байты; без padding объектов. */
void bytes(const void* data,std::size_t size){if(output)CHECK(std::fwrite(data,1,size,output)==size);}
/** @brief Записывает одно поле фиксированного типа; endian общий у сравниваемых host executable. */
template<class T>void value(T data){bytes(&data,sizeof(data));}
/** @brief Сериализует наблюдаемое состояние и каждый RGB/PCM байт для external A/B comparator. */
void dump(const Fixture& f,unsigned variant,unsigned frame){
    value(std::uint32_t(variant));value(std::uint32_t(frame));const auto& s=f.cut.cpu;
    value(s.pc);value(s.a);value(s.x);value(s.y);value(s.sp);value(s.p);value(s.cycles);value(std::uint8_t(s.jammed));
    value(f.cut.baseCycle);value(f.cut.endCycle);value(f.cut.ppuFrame);value(f.cut.scanline);value(f.cut.dot);value(f.cut.remainder);
    bytes(f.ram[frame&1U].data(),10240);bytes(f.rgb.data(),sizeof(f.rgb));
    value(std::uint64_t(f.sound.sampleCount()));bytes(f.sound.samples(),f.sound.sampleCount()*sizeof(std::int16_t));
    const auto& a=f.retired.header();value(a.inputCount);value(a.dmcCount);value(a.expansionCount);
    value(a.baseCycle);value(a.endCycle);value(a.ppuFrame);
    // Поля DMC доказывают guest sample timestamps; никакой memcmp padding.
    for(unsigned i=0;i<a.dmcCount;++i){const auto& d=f.retired.dmc()[i];value(d.offset);value(d.address);value(d.value);value(d.stall);}
}
/**
 * @brief Проверяет восемь полных кадров сценария с независимыми semantic assertions.
 * @details Canonical output сравнивают между разными core configs, а не между
 * двумя одинаковыми Machine. Known-value assertions не зависят от fast decoder.
 */
void run(unsigned variant){
    const auto rom=image(variant);auto f=std::make_unique<Fixture>();f->init(rom);
    for(unsigned frame=0;frame<8;++frame){
        f->frame(frame);dump(*f,variant,frame);CHECK(!f->cut.cpu.jammed);
        if(frame){const auto& ram=f->ram[frame&1U];
            if(variant==1)CHECK(ram[0x20]==0x39&&ram[0x21]==0x77&&ram[0x22]==0x60&&ram[0x23]==0x77&&ram[0x24]==0x77);
            if(variant==5)CHECK(ram[0x22]==0x60&&ram[0x23]==0x60);
        }
        CHECK(f->packets[frame&1U].release());
    }
    const auto& ram=f->ram[1];
    if(variant==2){CHECK(ram[0x10]>0);CHECK(ram[0x11]>0);CHECK(f->dmc>0);}
    
    CHECK(f->machine.cancelOutputFrames(f->lease));
    
    std::printf("MMC5 WRAM variant=%u frames=8 NMI=%u IRQ=%u DMC=%llu exact-state output\n",variant,unsigned(ram[0x10]),unsigned(ram[0x11]),static_cast<unsigned long long>(f->dmc));
}
}
/** @brief Выполняет semantic проверки и при запросе пишет external differential canonical файл. */
int main(int argc,char** argv){
    CHECK(argc==1||argc==2);if(argc==2){output=std::fopen(argv[1],"wb");CHECK(output);}
    for(unsigned variant=0;variant<6;++variant)run(variant);
    if(output)CHECK(std::fclose(output)==0);
    std::printf("MMC5 WRAM Output: %u checks passed\n",checks);
}
