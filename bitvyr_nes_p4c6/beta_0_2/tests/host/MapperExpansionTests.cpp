#include "cartridge/Cartridge.h"
#include "Machine.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using nes::core::Cartridge;
using nes::core::LoadResult;
using nes::core::Mirroring;

namespace {
/** @brief Прерывает fixture на независимом нарушении bank/IRQ/state ожидания. */
void check(bool ok, const char* text, unsigned line) {
    if (!ok) { std::fprintf(stderr, "line%u: %s\n", line, text); std::exit(1); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)

/** @brief Собственный ROM: каждый8KiB PRG/1KiB CHR банк имеет отдельную byte-метку. */
std::vector<std::uint8_t> image(unsigned mapper, bool nes2=false, unsigned sub=0, bool chrRam=false) {
    std::vector<std::uint8_t> rom(16+512*1024+(chrRam?0:32*1024),0);
    std::memcpy(rom.data(),"NES\x1a",4);
    rom[4]=32; rom[5]=chrRam?0:4;
    rom[6]=static_cast<std::uint8_t>(mapper<<4); rom[7]=static_cast<std::uint8_t>((mapper&0xf0)|(nes2?8:0));
    if (nes2) { rom[8]=static_cast<std::uint8_t>(sub<<4); rom[10]=7; if(chrRam)rom[11]=7; }
    for(unsigned i=0;i<512*1024;++i)rom[16+i]=static_cast<std::uint8_t>(i/8192);
    if(!chrRam)for(unsigned i=0;i<32*1024;++i)rom[16+512*1024+i]=static_cast<std::uint8_t>(i/1024);
    return rom;
}
/** @brief Записывает FME-7 команду через отдельные select/data strobes. */
void fme(Cartridge& c,unsigned command,unsigned value) {
    c.cpuWrite(0x9fff,static_cast<std::uint8_t>(command),0);
    c.cpuWrite(0xbfff,static_cast<std::uint8_t>(value),1);
}
/** @brief Доказывает допуск только известных NES2 подвариантов, без CRC/name bypass. */
void headers() {
    for(unsigned mapper:{0U,1U,2U,3U,4U,5U,7U,69U,71U,85U,160U})
        CHECK(Cartridge::supportsTimingVideoMapper(static_cast<std::uint16_t>(mapper)));
    for(unsigned mapper:{6U,8U,68U,70U,84U,86U,159U,161U,65535U})
        CHECK(!Cartridge::supportsTimingVideoMapper(static_cast<std::uint16_t>(mapper)));
    CHECK(Cartridge::hasExpansionAudioMapper(5) && Cartridge::hasExpansionAudioMapper(69) &&
          Cartridge::hasExpansionAudioMapper(85) && !Cartridge::hasExpansionAudioMapper(4) &&
          !Cartridge::hasExpansionAudioMapper(160));
    Cartridge c;
    auto jp=image(85,true,2,true); jp[6]|=2; jp[10]=0x70; jp[15]=1;
    CHECK(c.load(jp.data(),jp.size())==LoadResult::Ok);
    CHECK(c.mapper()==85 && !c.legacyHeader());
    CHECK(c.cpuRead(0xe000)==63);
    for(unsigned sub=3;sub<16;++sub) {
        jp[8]=static_cast<std::uint8_t>(sub<<4);
        CHECK(c.load(jp.data(),jp.size())==LoadResult::UnsupportedFormat);
    }
    auto f=image(69,true);
    f[8]=0x10; CHECK(c.load(f.data(),f.size())==LoadResult::UnsupportedFormat);
    f[8]=0; f[10]=8; CHECK(c.load(f.data(),f.size())==LoadResult::UnsupportedFormat);
    f[10]=0; CHECK(c.load(f.data(),f.size())==LoadResult::Ok);
    fme(c,8,0xc0); c.cpuWrite(0x6000,0x73,0); CHECK(c.cpuRead(0x6000,0xa5)==0xa5);
    auto old=image(85); old[15]='X';
    CHECK(c.load(old.data(),old.size())==LoadResult::UnsupportedFormat);
    auto knownOther=image(1,true,1);
    CHECK(c.load(knownOther.data(),knownOther.size())==LoadResult::UnsupportedFormat);
}
/** @brief Сверяет AxROM submapper с байтом текущего PRG-банка и разводкой CIRAM. */
void axromSubmappers() {
    Cartridge c;
    for (unsigned sub = 0; sub <= 2; ++sub) {
        auto rom = image(7, true, sub, true);
        rom[4] = 8;
        rom[10] = 0;
        rom.resize(16 + 8 * 16384U);
        rom[16] = 0x11;
        rom[16 + 32768 + 1] = 0x02;
        CHECK(c.load(rom.data(), rom.size()) == LoadResult::Ok);
        c.cpuWrite(0x8000, 0x13, 0);
        CHECK(c.cpuRead(0x8002) == (sub == 2 ? 4 : 12));
        CHECK(c.mirroring() == Mirroring::Single1);
        c.cpuWrite(0x8001, 0x12, 0);
        CHECK(c.cpuRead(0x8002) == 8);
        CHECK(c.mirroring() == (sub == 2 ? Mirroring::Single0 : Mirroring::Single1));
        if (sub) {
            c.cpuWrite(0x6000, 0x75, 0);
            CHECK(c.cpuRead(0x6000, 0x52) == 0x52);
        }
        c.chrWrite(0x1fff, 0x29);
        CHECK(c.chrRead(0x1fff) == 0x29);
        rom[8] = 0x30;
        CHECK(c.load(rom.data(), rom.size()) == LoadResult::UnsupportedFormat);
    }
}

/** @brief Проверяет аппаратное декодирование Camerica, память и начальное состояние. */
void camericaBanks() {
    Cartridge c;
    auto rom = image(71, false, 0, true);
    rom[4] = 16;
    rom[6] |= 1;
    rom.resize(16 + 16 * 16384U);
    CHECK(c.load(rom.data(), rom.size()) == LoadResult::Ok);
    CHECK(c.cpuRead(0x8000) == 0 && c.cpuRead(0xc000) == 30);
    c.cpuWrite(0x8000, 0x1a, 0);
    CHECK(c.cpuRead(0x8000) == 0 && c.mirroring() == Mirroring::Vertical);
    c.cpuWrite(0xc000, 0xfa, 0);
    CHECK(c.cpuRead(0x8000) == 20 && c.cpuRead(0xbfff) == 21);
    CHECK(c.cpuRead(0xc000) == 30 && c.cpuRead(0xffff) == 31);
    c.cpuWrite(0xffff, 1, 0);
    CHECK(c.cpuRead(0x8000) == 2);
    c.cpuWrite(0x9000, 0x10, 0);
    CHECK(c.mirroring() == Mirroring::Single1 && c.cpuRead(0x8000) == 2);
    c.cpuWrite(0xa000, 3, 0);
    CHECK(c.mirroring() == Mirroring::Single1 && c.cpuRead(0x8000) == 2);
    c.cpuWrite(0x7fff, 0x75, 0);
    CHECK(c.cpuRead(0x7fff, 0x52) == 0x52);
    c.chrWrite(0x1fff, 0x29);
    CHECK(c.chrRead(0x1fff) == 0x29 && !c.irqLine());
    c.reset();
    CHECK(c.cpuRead(0x8000) == 0 && c.mirroring() == Mirroring::Vertical);

    rom[7] |= 8;
    rom[11] = 7;
    CHECK(c.load(rom.data(), rom.size()) == LoadResult::Ok);
    c.cpuWrite(0x9000, 0x10, 0);
    CHECK(c.mirroring() == Mirroring::Vertical);
    rom[8] = 0x10;
    CHECK(c.load(rom.data(), rom.size()) == LoadResult::UnsupportedFormat);
    rom[4] = 8;
    rom.resize(16 + 8 * 16384U);
    CHECK(c.load(rom.data(), rom.size()) == LoadResult::Ok);
    c.cpuWrite(0x8000, 0x10, 0);
    CHECK(c.mirroring() == Mirroring::Single1 && c.cpuRead(0x8000) == 0);
    c.cpuWrite(0x9fff, 0, 0);
    c.cpuWrite(0xc000, 0x1b, 0);
    CHECK(c.mirroring() == Mirroring::Single0 && c.cpuRead(0x8000) == 6);
    c.reset();
    CHECK(c.cpuRead(0x8000) == 0 && c.mirroring() == Mirroring::Single0);
    rom[8] = 0x20;
    CHECK(c.load(rom.data(), rom.size()) == LoadResult::UnsupportedFormat);
}

/** @brief Проверяет FME-7 ROM/RAM gate, все страницыCHR и physical CIRAM линии. */
void fmeBanks() {
    Cartridge c; auto rom=image(69);
    CHECK(c.load(rom.data(),rom.size())==LoadResult::Ok);
    CHECK(c.cpuRead(0xe000)==63);
    c.cpuWrite(0x8000,0xf9,0); CHECK(c.cpuRead(0x8000)==0);
    c.cpuWrite(0xa000,0xff,1); CHECK(c.cpuRead(0x8000)==63);
    fme(c,10,17); fme(c,11,31);
    CHECK(c.cpuRead(0xa000)==17 && c.cpuRead(0xc000)==31 && c.cpuRead(0xffff)==63);
    for(unsigned p=0;p<8;++p)fme(c,p,31-p);
    for(unsigned p=0;p<8;++p)CHECK(c.chrRead(static_cast<std::uint16_t>(p*1024+511))==31-p);
    c.chrWrite(0,0xaa); CHECK(c.chrRead(0)==31);
    fme(c,8,33); CHECK(c.cpuRead(0x6000)==33);
    c.cpuWrite(0x6000,0xee,0); CHECK(c.cpuRead(0x6000)==33);
    fme(c,8,0x43); CHECK(c.cpuRead(0x6000,0xa5)==0xa5);
    c.cpuWrite(0x6000,0xee,0);
    fme(c,8,0xc3); CHECK(c.cpuRead(0x6000)==0);
    c.cpuWrite(0x6000,0x87,0); CHECK(c.cpuRead(0x6000)==0x87);
    fme(c,8,0x40); c.cpuWrite(0x6000,0x99,0);
    fme(c,8,0xc0); CHECK(c.cpuRead(0x6000)==0x87);
    constexpr Mirroring expected[]{Mirroring::Vertical,Mirroring::Horizontal,Mirroring::Single0,Mirroring::Single1};
    for(unsigned i=0;i<4;++i){fme(c,12,i);CHECK(c.mirroring()==expected[i]);}
    c.reset(); CHECK(c.cpuRead(0x6000,0xa5)==0xa5 && !c.irqLine());
}
/** @brief Проверяет A3/A4 отдельно; противоположный бит действительно игнорируется платой. */
void vrcBanks() {
    for(unsigned sub=0;sub<=2;++sub) {
        Cartridge c; auto rom=image(85,sub!=0,sub);
        CHECK(c.load(rom.data(),rom.size())==LoadResult::Ok);
        const unsigned select=sub==1?8:16;
        c.cpuWrite(0x8000,3,0); c.cpuWrite(static_cast<std::uint16_t>(0x8000+select),7,0); c.cpuWrite(0x9000,11,0);
        CHECK(c.cpuRead(0x8000)==3 && c.cpuRead(0xa000)==7 && c.cpuRead(0xc000)==11 && c.cpuRead(0xe000)==63);
        for(unsigned p=0;p<8;++p)c.cpuWrite(static_cast<std::uint16_t>(0xa000+(p/2)*0x1000+(p%2)*select),static_cast<std::uint8_t>(p+9),0);
        for(unsigned p=0;p<8;++p)CHECK(c.chrRead(static_cast<std::uint16_t>(p*1024))==p+9);
        if(sub) {
            c.cpuWrite(static_cast<std::uint16_t>(0xa000+(sub==1?16:8)),27,0);
            CHECK(c.chrRead(0)==27 && c.chrRead(0x400)==10);
        } else {
            c.cpuWrite(0xa008,19,0); CHECK(c.chrRead(0x400)==19);
            c.cpuWrite(0xa010,20,0); CHECK(c.chrRead(0x400)==20);
        }
        CHECK(c.cpuRead(0x6000,0xa5)==0xa5);
        c.cpuWrite(0x6000,0x73,0); c.cpuWrite(0xe000,0x80,0);
        CHECK(c.cpuRead(0x6000)==0);
        c.cpuWrite(0x6000,0x87,0); CHECK(c.cpuRead(0x6000)==0x87);
        c.cpuWrite(0xe000,0,0); c.cpuWrite(0x6000,0x99,0);
        CHECK(c.cpuRead(0x6000,0xa5)==0xa5);
        c.cpuWrite(0xe000,0x80,0); CHECK(c.cpuRead(0x6000)==0x87);
        constexpr Mirroring expected[]{Mirroring::Vertical,Mirroring::Horizontal,Mirroring::Single0,Mirroring::Single1};
        for(unsigned i=0;i<4;++i){c.cpuWrite(0xe000,static_cast<std::uint8_t>(i),0);CHECK(c.mirroring()==expected[i]);}
        if(sub==1)CHECK(c.expansionSample()==0 && c.selectedExpansionOutputCpuCycles()==~0U);
    }
    Cartridge c; auto ram=image(85,true,2,true);
    CHECK(c.load(ram.data(),ram.size())==LoadResult::Ok);
    c.cpuWrite(0xa000,7,0); c.chrWrite(0,0x73);
    c.cpuWrite(0xa000,6,0); CHECK(c.chrRead(0)==0);
    c.cpuWrite(0xa000,15,0); CHECK(c.chrRead(0)==0x73);
}
/** @brief Проверяет underflow, отдельные enables и quiet grant строго до IRQ-фронта. */
void fmeIrq() {
    Cartridge c; auto rom=image(69); CHECK(c.load(rom.data(),rom.size())==LoadResult::Ok);
    fme(c,14,2); fme(c,15,0); fme(c,13,0x81);
    CHECK(c.cpuIrqQuietCycles()==2);
    c.cpuCyclesQuiet(2); CHECK(!c.irqLine() && c.cpuIrqQuietCycles()==0);
    c.cpuCycle(); CHECK(c.irqLine());
    fme(c,14,7); CHECK(c.irqLine()); // Счётчик не подтверждает IRQ.
    fme(c,13,1); CHECK(!c.irqLine() && c.cpuIrqQuietCycles()==~0U);
    c.cpuCyclesQuiet(1000); fme(c,13,0x81); CHECK(c.cpuIrqQuietCycles()==0xff07);
    fme(c,14,0); fme(c,15,0); fme(c,13,0x80);
    c.cpuCycle(); CHECK(!c.irqLine());
    fme(c,13,0x81); CHECK(c.cpuIrqQuietCycles()==65535);
    c.cpuCyclesQuiet(65535); CHECK(!c.irqLine() && c.cpuIrqQuietCycles()==0);
    c.cpuCycle(); CHECK(c.irqLine());
}
/** @brief Проверяет VRC cycle/scanline114,114,113 и ack без потери фазы. */
void vrcIrq() {
    Cartridge c; auto rom=image(85,true,2,true); CHECK(c.load(rom.data(),rom.size())==LoadResult::Ok);
    c.cpuWrite(0xe010,254,0); c.cpuWrite(0xf000,6,0);
    CHECK(c.cpuIrqQuietCycles()==1);
    c.cpuCyclesQuiet(1); CHECK(!c.irqLine() && c.cpuIrqQuietCycles()==0);
    c.cpuCycle(); CHECK(c.irqLine());
    c.cpuWrite(0xf010,0,0); CHECK(!c.irqLine() && c.cpuIrqQuietCycles()==~0U);
    c.cpuWrite(0xe010,255,0); c.cpuWrite(0xf000,3,0);
    for(unsigned period:{114U,114U,113U}) {
        CHECK(c.cpuIrqQuietCycles()==period-1);
        c.cpuCyclesQuiet(period-1); CHECK(!c.irqLine());
        c.cpuCycle(); CHECK(c.irqLine());
        c.cpuWrite(0xf010,0,0); CHECK(!c.irqLine());
    }
    c.cpuWrite(0xf000,3,0); c.cpuCyclesQuiet(40);
    CHECK(c.cpuIrqQuietCycles()==73);
    c.cpuWrite(0xf010,0,0); CHECK(c.cpuIrqQuietCycles()==73);
    c.cpuWrite(0xf000,0,0); c.cpuCyclesQuiet(1000); CHECK(!c.irqLine());
}
/** @brief Сопоставляет batch с serial на разных остатках и после ack pending IRQ. */
void batchEquivalence(unsigned mapper) {
    Cartridge bulk, serial; auto rom=image(mapper);
    CHECK(bulk.load(rom.data(),rom.size())==LoadResult::Ok);
    CHECK(serial.load(rom.data(),rom.size())==LoadResult::Ok);
    if(mapper==69) { fme(bulk,14,0x4d);fme(serial,14,0x4d);fme(bulk,13,0x81);fme(serial,13,0x81); }
    else { bulk.cpuWrite(0xe010,0xf1,0);serial.cpuWrite(0xe010,0xf1,0);bulk.cpuWrite(0xf000,3,0);serial.cpuWrite(0xf000,3,0); }
    std::uint32_t rng=17;
    for(unsigned i=0;i<300;++i) {
        rng=rng*1664525U+1013904223U;
        unsigned count=1+(rng%600);
        if(!bulk.irqLine() && count>bulk.cpuIrqQuietCycles())count=bulk.cpuIrqQuietCycles();
        bulk.cpuCyclesQuiet(count); for(unsigned n=0;n<count;++n)serial.cpuCycle();
        CHECK(bulk.irqLine()==serial.irqLine());
        CHECK(bulk.cpuIrqQuietCycles()==serial.cpuIrqQuietCycles());
        if(bulk.cpuIrqQuietCycles()==0){bulk.cpuCycle();serial.cpuCycle();CHECK(bulk.irqLine()==serial.irqLine());}
        if(i%7==0) {
            if(mapper==69){fme(bulk,13,0x81);fme(serial,13,0x81);}
            else {bulk.cpuWrite(0xf010,0,0);serial.cpuWrite(0xf010,0,0);}
            CHECK(bulk.cpuIrqQuietCycles()==serial.cpuIrqQuietCycles());
        }
    }
}
/** @brief Исполняет собственную6502-программу: IRQ обслуживается через настоящую Machine. */
void machineIrq(unsigned mapper) {
    auto rom=image(mapper);
    std::vector<std::uint8_t> program{0x78,0xa9,0x40,0x8d,0x17,0x40}; // SEI; inhibit APU frame IRQ.
    const auto store=[&](unsigned address,unsigned value) {
        program.insert(program.end(),{0xa9,static_cast<std::uint8_t>(value),0x8d,static_cast<std::uint8_t>(address),static_cast<std::uint8_t>(address>>8)});
    };
    if(mapper==69) {store(0x8000,14);store(0xa000,0xf4);store(0x8000,15);store(0xa000,1);store(0x8000,13);store(0xa000,0x81);}
    else {store(0xe010,0);store(0xf000,6);}
    program.push_back(0x58); // CLI.
    const auto loop=static_cast<std::uint16_t>(0xe000+program.size());
    program.insert(program.end(),{0xe6,0x00,0x4c,static_cast<std::uint8_t>(loop),static_cast<std::uint8_t>(loop>>8)});
    const std::size_t fixed=16+512*1024-8192;
    std::memcpy(rom.data()+fixed,program.data(),program.size());
    std::vector<std::uint8_t> handler{0xe6,0x01,0xa9,0};
    if(mapper==69)handler.insert(handler.end(),{0x8d,0x00,0xa0});
    else handler.insert(handler.end(),{0x8d,0x00,0xf0});
    handler.push_back(0x40); // RTI.
    std::memcpy(rom.data()+fixed+0x100,handler.data(),handler.size());
    rom[fixed+0x180]=0x40;
    rom[fixed+0x1ffa]=0x80;rom[fixed+0x1ffb]=0xe1;
    rom[fixed+0x1ffc]=0x00;rom[fixed+0x1ffd]=0xe0;
    rom[fixed+0x1ffe]=0x00;rom[fixed+0x1fff]=0xe1;
    auto machine=std::make_unique<nes::core::Machine>();
    CHECK(machine->load(rom.data(),rom.size())==LoadResult::Ok);
    for(unsigned i=0;i<2500;++i)machine->stepInstruction();
    CHECK(machine->debugRead(1)==1); CHECK(machine->debugRead(0)!=0);
    CHECK(machine->cpuState().pc>=loop && machine->cpuState().pc<loop+5);
}
}
/** @brief Проверяет mapper69/85 цифровые контракты в selected и scalar native domains. */
int main() {
    headers(); axromSubmappers(); camericaBanks(); fmeBanks(); vrcBanks(); fmeIrq(); vrcIrq();
    batchEquivalence(69); batchEquivalence(85); machineIrq(69); machineIrq(85);
    std::printf("Mapper7/69/71/85 banking/RAM/CHR/mirroring/IRQ/Machine passed; sizeof(Cartridge)=%zu\n",sizeof(Cartridge));
}

