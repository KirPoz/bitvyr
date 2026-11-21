/**
 * @file PpuEmptySprite0SpanTests.cpp
 * @brief Настоящий Output/RGB/PCM и гостевой Sprite0 polling для пустых raster-интервалов.
 */
#include "Machine.h"
#include "apu/AudioRenderer.h"
#include "ppu/VideoRenderer.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using namespace nes::core;
unsigned checks=0;
std::FILE* output=nullptr;
/** @brief Останавливает fixture на первой точной строке нарушения контракта. */
[[noreturn]] void fail(const char* text,unsigned line){
    std::fprintf(stderr,"EMPTY_SPRITE0 line%u: %s\n",line,text);std::exit(1);
}
#define CHECK(value) do{++checks;if(!(value))fail(#value,__LINE__);}while(false)

/** @brief Положение/маска Sprite0 и независимое ожидаемое наличие хотя бы одного hit. */
struct Case {std::uint8_t x,mask,opaque;bool hit;};
/** @brief Малый assembler только оригинального тестового ROM; ветви проверяют относительный диапазон. */
struct Program {
    std::vector<std::uint8_t>& rom;
    unsigned pos=16;
    /** @brief Заимствует готовый iNES backing, не меняя его размер/адрес. */
    explicit Program(std::vector<std::uint8_t>& bytes):rom(bytes){}
    /** @brief Возвращает адрес следующего байта NROM CPU, начиная с $8000. */
    unsigned address()const{return 0x8000U+pos-16U;}
    /** @brief Добавляет байт с проверкой границы PRG. */
    void byte(unsigned value){CHECK(pos<16U+32768U);rom[pos++]=static_cast<std::uint8_t>(value);}
    /** @brief Добавляет opcode с восьмибитным operand. */
    void immediate(unsigned op,unsigned value){byte(op);byte(value);}
    /** @brief Добавляет opcode с little-endian шестнадцатибитным адресом. */
    void absolute(unsigned op,unsigned value){byte(op);byte(value);byte(value>>8U);}
    /** @brief Добавляет относительную ветвь; дальняя метка считается ошибкой теста. */
    void branch(unsigned op,unsigned target){
        const int delta=static_cast<int>(target)-static_cast<int>(address()+2U);
        CHECK(delta>=-128&&delta<=127);byte(op);byte(static_cast<std::uint8_t>(delta));
    }
    /** @brief Выполняет LDA immediate и настоящий STA absolute в программе гостя. */
    void set(unsigned target,unsigned value){immediate(0xa9,value);absolute(0x8d,target);}
};

/**
 * @brief Создаёт NROM с непрозрачным BG и единственным Sprite0 из независимой таблицы.
 * @param c X, clip и bitmap проверяемого спрайта; opaque задаёт восемь экранных битов.
 * @param tall false для 8x8, true для 8x16 с отдельным pattern bank1.
 * @return Собственные ROM-байты без заимствований чужих игр.
 * @details Гость считает итерации до первого BIT $2002 с V=1 и сохраняет timestamp
 * в RAM $10/$11, флаг в $12 и PHP в $13. Canonical A/B сравнивает точный guest cut,
 * всю RAM, каждый RGB/PCM байт, поэтому ранний/поздний hit меняет результат.
 */
std::vector<std::uint8_t> image(Case c,bool tall){
    std::vector<std::uint8_t> rom(16U+32768U+8192U,0);
    rom[0]='N';rom[1]='E';rom[2]='S';rom[3]=0x1a;rom[4]=2;rom[5]=1;
    Program p(rom);p.byte(0x78);p.byte(0xd8);p.immediate(0xa2,0xff);p.byte(0x9a);
    p.set(0x4017,0x40);p.set(0x2000,0);p.set(0x2001,0);
    // Все остальные OAM entries вне видимой строки; после цикла адрес снова0.
    p.set(0x2003,0);p.immediate(0xa2,0);p.immediate(0xa9,0xff);
    const auto oam=p.address();p.absolute(0x8d,0x2004);p.byte(0xe8);p.branch(0xd0,oam);
    p.set(0x2004,39);p.set(0x2004,1);p.set(0x2004,0);p.set(0x2004,c.x);
    p.set(0x2006,0x20);p.set(0x2006,0);p.immediate(0xa9,0);p.immediate(0xa2,0);p.immediate(0xa0,4);
    const auto names=p.address();p.absolute(0x8d,0x2007);p.byte(0xe8);p.branch(0xd0,names);p.byte(0x88);p.branch(0xd0,names);
    p.set(0x2006,0x3f);p.set(0x2006,0);
    for(unsigned i=0;i<32U;++i)p.set(0x2007,i==1U?0x16U:i==17U?0x30U:0x0fU);
    p.set(0x2006,0);p.set(0x2006,0);p.set(0x2005,0);p.set(0x2005,0);
    p.set(0x2000,tall?0x20U:0U);p.set(0x2001,c.mask);
    p.immediate(0xa2,0);p.immediate(0xa0,0);
    const auto poll=p.address();p.byte(0xe8);
    const auto skipHigh=p.address()+3U;p.branch(0xd0,skipHigh);p.byte(0xc8);
    p.absolute(0x2c,0x2002);p.branch(0x50,poll);
    p.immediate(0x86,0x10);p.immediate(0x84,0x11);p.byte(0x08);p.byte(0x68);p.immediate(0x85,0x13);
    p.immediate(0xa9,1);p.immediate(0x85,0x12);
    const auto end=p.address();p.absolute(0x4c,end);
    for(unsigned a=0x7ffa;a<0x8000U;a+=2U){rom[16U+a]=0;rom[16U+a+1U]=0x80;}
    const unsigned chr=16U+32768U;
    for(unsigned y=0;y<8U;++y){rom[chr+y]=0xff;rom[chr+16U+y]=c.opaque;}
    if(tall)for(unsigned y=0;y<8U;++y){rom[chr+0x1000U+y]=c.opaque;rom[chr+0x1010U+y]=c.opaque;}
    return rom;
}

/**
 * @brief Владеет настоящим Output producer и синхронным RGB/PCM consumer без SDK.
 * @details Machine уничтожается первой; ROM живёт во внешнем run до cancel/join.
 * Большие арены выделяются единожды до цикла. Между кадрами пакет явно release.
 */
struct Fixture {
    std::array<VideoPacket,2> packets;
    std::array<std::array<std::uint8_t,10240>,2> ram{};
    AudioPacket audio,retired;
    AudioRenderer sound;
    VideoStorage storage;
    VideoRenderer video{storage};
    std::array<std::uint16_t,256U*240U> rgb{};
    OutputFrameBinding binding{};OutputFrameLease lease{};OutputFrameTicket ticket{};OutputFrameCut cut{};
    const VideoPacket* sealed=nullptr;
    std::uint64_t events=0;
    Machine machine;
    /** @brief Привязывает все неалиасные диапазоны до первого Output JOB. */
    void init(const std::vector<std::uint8_t>& rom){
        CHECK(machine.load(rom.data(),rom.size(),Region::Ntsc)==LoadResult::Ok);
        machine.configureSplitAudio(&audio,true);
        binding.source={&machine,rom.data(),rom.size(),machine.colourTransportEpoch(),1,0,Region::Ntsc};
        binding.packets={&packets[0],&packets[1]};binding.snapshots={ram[0].data(),ram[1].data()};
        binding.protectedSpans[0]={&audio,sizeof(audio)};binding.protectedSpans[1]={&retired,sizeof(retired)};
        binding.protectedSpans[2]={rgb.data(),sizeof(rgb)};binding.protectedSpans[3]={&storage,sizeof(storage)};
        binding.protectedSpans[4]={&video,sizeof(video)};
        CHECK(machine.configureOutputFrames(binding,lease));video.setRgb565Output(rgb.data());
    }
    /** @brief Исполняет и потребляет один кадр с сохранением гостевых времени, RGB и PCM. */
    void frame(unsigned n){
        ticket={lease,n,n,machine.cpuCycles(),n&1U};
        CHECK(machine.beginOutputFrame(ticket));CHECK(machine.stepFrame()==RunStatus::FrameReady);
        CHECK(machine.sealOutputFrame(ticket,sealed,cut));CHECK(machine.copyOutputVisibleRam(ticket,ram[n&1U].data(),10240));
        events+=sealed->header().eventCount;
        CHECK(machine.retireAudioPacket(retired));CHECK(video.render(*sealed)==VideoRenderResult::Ok);
        CHECK(sound.render(retired)==AudioRenderResult::Ok);CHECK(!cut.cpu.jammed);
    }
};
/** @brief Записывает остановленные canonical bytes, если caller запросил внешний A/B файл. */
void bytes(const void* data,std::size_t size){if(output)CHECK(std::fwrite(data,1,size,output)==size);}
/** @brief Записывает фиксированный тип без padding окружающих C++-объектов. */
template<class T>void value(T data){bytes(&data,sizeof(data));}
/** @brief Сохраняет CPU/cut/RAM, каждый RGB и PCM; транспортные ops могут законно различаться. */
void dump(const Fixture& f,unsigned variant,unsigned frame){
    value(std::uint32_t(variant));value(std::uint32_t(frame));const auto& cpu=f.cut.cpu;
    value(cpu.pc);value(cpu.a);value(cpu.x);value(cpu.y);value(cpu.sp);value(cpu.p);value(cpu.cycles);value(std::uint8_t(cpu.jammed));
    value(f.cut.baseCycle);value(f.cut.endCycle);value(f.cut.ppuFrame);value(f.cut.scanline);value(f.cut.dot);value(f.cut.remainder);
    bytes(f.ram[frame&1U].data(),10240);bytes(f.rgb.data(),sizeof(f.rgb));
    value(std::uint64_t(f.sound.sampleCount()));bytes(f.sound.samples(),f.sound.sampleCount()*sizeof(std::int16_t));
}
/** @brief Проверяет independent hit expectation после шести кадров и канонический результат каждого кадра. */
void run(Case c,bool tall,unsigned variant){
    const auto rom=image(c,tall);auto f=std::make_unique<Fixture>();f->init(rom);
    for(unsigned frame=0;frame<6U;++frame){f->frame(frame);dump(*f,variant,frame);CHECK(f->packets[frame&1U].release());}
    const auto& ram=f->ram[1];CHECK((ram[0x12]!=0)==c.hit);
    if(c.hit){CHECK((ram[0x13]&0x40U)!=0);CHECK(ram[0x10]!=0||ram[0x11]!=0);}
    CHECK(f->machine.cancelOutputFrames(f->lease));
    // Не входит в canonical: transport может упаковать те же пиксели иначе.
    // Внешний control/ALL gate требует хотя бы одного сокращения на NROM,
    // подтверждая выполнение нового H8 path, а не только совпадение fallback.
    std::printf("EMPTY_SPRITE0_EVENTS variant=%u events=%llu\n",variant,static_cast<unsigned long long>(f->events));
}
}

/** @brief Запускает 8x8/8x16 и clip/x255 матрицу; успешный host тест не означает FPS платы. */
int main(int argc,char** argv){
    if(argc==3&&!std::strcmp(argv[1],"--canonical")){output=std::fopen(argv[2],"wb");CHECK(output);}
    else if(argc!=1)return 2;
    constexpr Case cases[]={{0,0x18,0xff,false},{0,0x1e,0xff,true},{7,0x18,0xff,true},
        {8,0x18,0xff,true},{80,0x1e,0xff,true},{248,0x1e,0xff,true},
        {254,0x1e,0x80,true},{254,0x1e,0x40,false},{255,0x1e,0xff,false}};
    unsigned variant=0;for(bool tall:{false,true})for(const auto c:cases)run(c,tall,variant++);
    if(output)CHECK(std::fclose(output)==0);
    std::printf("EMPTY_SPRITE0_OUTPUT_PASS cases=%u checks=%u\n",variant,checks);
}
