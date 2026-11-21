/** @file MachineCartRamQuantumTests.cpp @brief Точные PRG-RAM accesses и продолжение generic CPU-кванта. */
#include "Machine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace {
unsigned checks=0;
bool expectWindow=false;
std::FILE* canonical=nullptr;
/** @brief Завершает fixture с указанием нарушенного независимого контракта. */
[[noreturn]] void fail(const char* expression,unsigned line){std::fprintf(stderr,"PRG-RAM quantum line %u: %s\n",line,expression);std::exit(1);}
#define CHECK(condition) do{++checks;if(!(condition))fail(#condition,__LINE__);}while(false)
/** @brief Сохраняет явные значения без C++ padding для сравнения разных Core lanes. */
template<class T>void save(T value){if(canonical)CHECK(std::fwrite(&value,sizeof(value),1,canonical)==1);}
/** @brief Владеет синтетической ROM; все CPU-операции fixture исполняются из обычной RAM. */
std::vector<std::uint8_t> image(unsigned mapper){
    std::vector<std::uint8_t> r(16U+32768U+8192U,0);std::memcpy(r.data(),"NES\x1A",4);
    r[4]=2;r[5]=1;r[6]=std::uint8_t(mapper<<4U);r[7]=std::uint8_t(mapper&0xF0U);
    r[16U+0x7FFCU]=0;r[16U+0x7FFDU]=0x80;
    return r;
}
}

namespace nes::core {
/**
 * @brief Использует существующую friendship Machine без production hooks/counters.
 * @details Доступен только native fixture. Отбор реального budget выполняет сама
 * Machine на первом opcode read; тест не назначает произвольный будущий горизонт.
 */
struct MachineEventKernelTestAccess {
    /** @brief Устанавливает остановленный CPU на собственную программу RAM, сохраняя часы. */
    static void program(Machine& m,const std::vector<std::uint8_t>& code,std::uint8_t a=0){
        m.observeCpuReads();m.cpuReadBudget_=0;m.flushPpu();
        CHECK(code.size()<256U);std::copy(code.begin(),code.end(),m.ram_.begin()+0x200);
        auto state=m.cpu_.state();state.pc=0x200;state.a=a;state.p=0x24;state.jammed=false;m.cpu_.setState(state);
    }
    /** @brief Возвращает только долг уже исполненных M2 без материализации устройств. */
    static unsigned pending(const Machine& m){return m.pendingCpuReads_;}
    /** @brief Возвращает реальный остаток выданного budget, не создавая его. */
    static unsigned budget(const Machine& m){return m.cpuReadBudget_;}
    /** @brief Возвращает регистр A остановленного CPU без обычного observing getter. */
    static std::uint8_t a(const Machine& m){return m.cpu_.state().a;}
    /** @brief Читает открытую CPU-шину на уже исполненной границе инструкции. */
    static std::uint8_t openBus(const Machine& m){return m.openBus_;}
    /** @brief Материализует setup-инструкцию перед следующей независимой программой. */
    static void barrier(Machine& m){m.observeCpuReads();m.flushPpu();}
    /** @brief Проверяет retained activation до observing getter, затем создаёт canonical cut. */
    static void finish(Machine& m,unsigned mapper,unsigned scenario,bool eligible){
        if(eligible&&expectWindow){CHECK(pending(m)>0);CHECK(budget(m)>0);}
        if(eligible&&!expectWindow){CHECK(pending(m)==0);CHECK(budget(m)==0);}
        m.observeCpuReads();m.flushPpu();
        const auto& s=m.cpu_.state();save(mapper);save(scenario);save(s.pc);save(s.a);save(s.x);save(s.y);save(s.sp);save(s.p);save(s.cycles);save(std::uint8_t(s.jammed));
        save(m.cpuCycles());save(m.openBus_);save(std::uint8_t(m.cartridge_.irqLine()));
        for(const auto b:m.ram_)save(b);
        for(unsigned address=0x6000;address<0x8000;++address)save(m.cartridge_.cpuRead(std::uint16_t(address),m.openBus_));
        const auto& p=m.timingPpu_;save(p.scanline_);save(p.dot_);save(p.frames_);save(p.control_);save(p.mask_);save(p.status_);save(p.v_);save(p.t_);save(p.openBus_);save(p.busAddress_);save(m.ppuRemainder_);
        save(std::uint64_t(m.sampleCount()));for(unsigned i=0;i<m.sampleCount();++i)save(m.samples()[i]);
    }
};
}

namespace {
using namespace nes::core;
using Access=MachineEventKernelTestAccess;
/** @brief Создаёт одну независимую Machine с живой ROM до конца fixture. */
std::unique_ptr<Machine> machine(const std::vector<std::uint8_t>& rom,Region region=Region::Ntsc){auto m=std::make_unique<Machine>();CHECK(m->load(rom.data(),rom.size(),region)==LoadResult::Ok);return m;}
/** @brief Настраивает память/устройство настоящей STA-инструкцией, не несуществующим debug write API. */
void poke(Machine& m,std::uint16_t address,std::uint8_t value){Access::program(m,{0x8D,std::uint8_t(address),std::uint8_t(address>>8)},value);m.stepInstruction();Access::barrier(m);}
/** @brief Выполняет реальные STA-записи серийного MMC1 с исходными межинструкционными clocks. */
void mmc1Register(Machine& m,std::uint16_t address,std::uint8_t value){for(unsigned bit=0;bit<5;++bit){Access::program(m,{0x8D,std::uint8_t(address),std::uint8_t(address>>8)},std::uint8_t((value>>bit)&1U));m.stepInstruction();}}
/** @brief Проверяет независимые значения RAM/open bus, indexed crossing, RMW и реальные stores. */
void dataAccesses(const std::vector<std::uint8_t>& rom,unsigned mapper,Region region){
    auto m=machine(rom,region);poke(*m,0x6000,0x39);poke(*m,0x6100,0x52);
    Access::program(*m,{0xAD,0x00,0x60});const auto before=m->cpuCycles();m->stepInstruction();
    CHECK(m->cpuCycles()==before+4U);CHECK(Access::a(*m)==0x39);CHECK(Access::openBus(*m)==0x39);Access::finish(*m,mapper,0,true);
    Access::program(*m,{0x8D,0x01,0x60},0xA5);m->stepInstruction();CHECK(Access::openBus(*m)==0xA5);Access::finish(*m,mapper,1,true);CHECK(m->debugRead(0x6001)==0xA5);
    Access::program(*m,{0xEE,0x00,0x60});m->stepInstruction();CHECK(Access::openBus(*m)==0x3A);Access::finish(*m,mapper,2,true);CHECK(m->debugRead(0x6000)==0x3A);
    Access::program(*m,{0xA2,0x01,0xBD,0xFF,0x60});m->stepInstruction();const auto indexed=m->cpuCycles();m->stepInstruction();CHECK(m->cpuCycles()==indexed+5U);CHECK(Access::a(*m)==0x52);Access::finish(*m,mapper,3,true);
    // Следующий MMIO — настоящий барьер; deferred байты и последний old/new уже видны.
    Access::program(*m,{0xAD,0x00,0x60,0x8D,0x00,0x20});m->stepInstruction();m->stepInstruction();CHECK(m->debugRead(0x6000)==0x3A);Access::finish(*m,mapper,4,false);
}
/** @brief Проверяет MMC1 disable/open bus и MMC3 enable/write protection без кеша разрешений. */
void protection(unsigned mapper){
    const auto rom=image(mapper);auto m=machine(rom);poke(*m,0x6000,0x47);
    if(mapper==1)mmc1Register(*m,0xE000,0x10);else poke(*m,0xA001,0x00);
    Access::program(*m,{0xAD,0x00,0x60});m->stepInstruction();CHECK(Access::a(*m)==0x60);Access::finish(*m,mapper,10,true);
    Access::program(*m,{0x8D,0x00,0x60},0xA9);m->stepInstruction();Access::finish(*m,mapper,11,true);
    if(mapper==1)mmc1Register(*m,0xE000,0x00);else poke(*m,0xA001,0x80);
    CHECK(m->debugRead(0x6000)==0x47);
    if(mapper==4){poke(*m,0xA001,0xC0);Access::program(*m,{0xEE,0x00,0x60});m->stepInstruction();Access::finish(*m,mapper,12,true);poke(*m,0xA001,0x80);CHECK(m->debugRead(0x6000)==0x47);}
}
/** @brief Подтверждает неизменность фильтра consecutive MMC1 writes после retained RAM M2. */
void mmc1WriteCycle(){
    const auto rom=image(1);auto m=machine(rom);poke(*m,0x6000,0x22);
    Access::program(*m,{0xEE,0x00,0x60,0xEE,0x00,0xE0});m->stepInstruction();m->stepInstruction();
    // ROM $E000 содержит0; INC даёт old0/new1, новый adjacent write должен быть проигнорирован.
    mmc1Register(*m,0xE000,0x00);CHECK(m->debugRead(0x6000)==0x23);Access::finish(*m,1,20,false);
}
/** @brief Observer получает прежние eager границы вместо нового удержанного cartridge окна. */
struct Observer final:VideoObserver {
    /** @brief Принимает prefix без обращения к изменяемой Machine. */
    bool observeVideo(const VideoPacket&)noexcept override{return true;}
    /** @brief Тест не заимствует собственный framebuffer наблюдателя. */
    const std::uint8_t* indexedVideo()const noexcept override{return nullptr;}
    /** @brief Возвращает нейтральный emphasis для любого запрошенного ряда. */
    std::uint8_t videoEmphasis(unsigned)const noexcept override{return 0;}
};
/** @brief Доказывает отсутствие нового пути при подключённом синхронном видеонаблюдателе. */
void observed(){const auto rom=image(0);auto m=machine(rom);Observer observer;m->configureSplitVideo(nullptr,&observer);poke(*m,0x6000,0x57);Access::program(*m,{0xAD,0x00,0x60});m->stepInstruction();CHECK(Access::a(*m)==0x57);CHECK(Access::pending(*m)==0);Access::finish(*m,0,30,false);m->configureSplitVideo(nullptr,nullptr);}
/** @brief Проверяет actual Legacy160 RAM/PRG окно и два write-IRQ strobes RMW на пользовательской ROM. */
void legacy(const char* path){
    if(!path){std::puts("Legacy160 directed positive fixture skipped: pass exact local Aladdin NES as third argument");return;}
    std::ifstream input(path,std::ios::binary);CHECK(input);const std::vector<std::uint8_t> rom((std::istreambuf_iterator<char>(input)),{});
    dataAccesses(rom,160,Region::Ntsc);auto m=machine(rom);poke(*m,0x6000,0x31);poke(*m,0xD000,0x80);
    const auto expected=m->debugRead(0x6000);Access::program(*m,{0xAD,0x00,0x60});m->stepInstruction();CHECK(Access::a(*m)==expected);Access::finish(*m,160,40,true);
    Access::program(*m,{0x8D,0x00,0x60},0xEE);m->stepInstruction();Access::finish(*m,160,41,true);poke(*m,0xD000,0);CHECK(m->debugRead(0x6000)==0x31);
    poke(*m,0xC001,0x47);poke(*m,0xC004,6);poke(*m,0xC005,255);poke(*m,0xC003,0);
    Access::program(*m,{0xEE,0x00,0x60});m->stepInstruction();CHECK(m->cartridge().irqLine());CHECK(m->debugRead(0x6000)==0x32);Access::finish(*m,160,42,false);
}
}

/** @brief Запускает независимые semantic fixtures и optional canonical A/B dump; host FPS не измеряет. */
int main(int argc,char** argv){
    expectWindow=argc>1&&std::strtoul(argv[1],nullptr,10)!=0;
    if(argc>2){canonical=std::fopen(argv[2],"wb");CHECK(canonical);}
    for(const auto mapper:{0U,1U,2U,3U,4U,7U})for(const auto region:{Region::Ntsc,Region::Pal})dataAccesses(image(mapper),mapper,region);
    protection(1);protection(4);mmc1WriteCycle();observed();legacy(argc>3?argv[3]:nullptr);
    if(canonical)CHECK(std::fclose(canonical)==0);
    std::printf("PRG-RAM quantum passed: expected=%u checks=%u\n",unsigned(expectWindow),checks);
}
