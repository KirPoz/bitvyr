#include "cartridge/Legacy160.h"
#include "cpu/Cpu6502.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

/**
 * @brief Выводит нарушенное условие Legacy160 и строку, затем возвращает ошибку процессом.
 * @details Никакой работы с платой или повторного чтения ROM при отказе нет.
 */
[[noreturn]] void fail(const char* text, unsigned line) {
    std::fprintf(stderr, "Legacy160 test failed line %u: %s\n", line, text);
    std::exit(1);
}
#define CHECK(condition) do { if (!(condition)) fail(#condition,__LINE__); } while (false)

/** @brief Проверяет PRG/CHR-декодирование Legacy160 по независимым меткам backing. */
void banks() {
    nes::core::Legacy160 mapper;
    mapper.reset(524288, 524288);
    CHECK(mapper.prgOffset(0x8000) == 0x78000);
    CHECK(mapper.prgOffset(0xFFFF) == 0x7FFFF);
    mapper.write(0xD000,0x1E);
    for (unsigned i=0;i<4;++i) mapper.write(static_cast<std::uint16_t>(0x8000+i),static_cast<std::uint8_t>(12+i));
    CHECK(mapper.prgOffset(0x8000) == 12*8192);
    CHECK(mapper.prgOffset(0xA123) == 13*8192+0x123);
    CHECK(mapper.prgOffset(0xC000) == 14*8192);
    CHECK(mapper.prgOffset(0xFFFF) == 15*8192+8191);
    mapper.write(0x8802,0); CHECK(mapper.prgOffset(0xC000) == 14*8192); // Установленный A11 запрещает запись выбора PRG.
    mapper.write(0xD000,0x1A); CHECK(mapper.prgOffset(0xE000) == 63*8192);
    mapper.write(0xD000,0x9E); CHECK(mapper.mapsPrgAt6000()); CHECK(mapper.prgOffset(0x6000) == 15*8192);
    mapper.write(0xD000,0x15);
    CHECK(mapper.prgOffset(0x8000) == 13*16384);
    CHECK(mapper.prgOffset(0xC000) == 15*16384);
    mapper.write(0xD000,0x17); mapper.write(0x8000,0x10);
    CHECK(mapper.prgOffset(0x8000) == 4*8192); // Семь бит номера банка разворачиваются по заданной проводке.

    mapper.write(0xD000,0x1E);
    for (unsigned i=0;i<8;++i) mapper.write(static_cast<std::uint16_t>(0x9000+i),static_cast<std::uint8_t>(8+i));
    CHECK(mapper.chrOffset(0x0000) == 8*1024);
    CHECK(mapper.chrOffset(0x1FFF) == 15*1024+1023);
    mapper.write(0x9800,0); CHECK(mapper.chrOffset(0) == 8*1024); // Установленный A11 запрещает запись выбора CHR.
    mapper.write(0xA000,1); // Старший бит банка требует внешнего CHR объёмом512KiB.
    CHECK(mapper.chrOffset(0) == 8*1024);
    mapper.write(0xD003,0x20); CHECK(mapper.chrOffset(0) == 264*1024);
    mapper.write(0xD002,0x40); CHECK(mapper.chrWriteEnabled());
    for (unsigned i=0;i<4;++i) { mapper.write(0xD001,static_cast<std::uint8_t>(i)); CHECK(mapper.mirrorMode()==i); }

    mapper.reset(262144,262144);
    mapper.write(0xD000,0x0E); // Режим CHR4KiB с переключаемым PRG.
    mapper.write(0xD003,0x80); // Защёлки изменяются по MMC4-подобному протоколу.
    mapper.write(0x9000,1); mapper.write(0x9002,2);
    CHECK(mapper.chrOffset(0x123) == 4096+0x123);
    mapper.ppuAddress(0xFE8); CHECK(mapper.ppuRead());
    CHECK(mapper.chrOffset(0x123) == 8192+0x123);
    CHECK(!mapper.ppuRead()); // Повторное чтение уже выбранной защёлки не инвалидирует кэш.
    mapper.ppuAddress(0xFD8); CHECK(mapper.ppuRead());
    CHECK(mapper.chrOffset(0x123) == 4096+0x123);
    CHECK(!mapper.ppuRead());
    mapper.ppuAddress(0x2FE8); CHECK(!mapper.ppuRead()); // Чтение nametable не изменяет CHR-защёлки.
    CHECK(mapper.chrOffset(0x123) == 4096+0x123);
}

/** @brief Проверяет адресное декодирование регистров и IRQ-предделитель Legacy160. */
void interruptsAndRegisters() {
    nes::core::Legacy160 mapper;
    mapper.reset(262144,262144);
    mapper.write(0xC001,0x85); // A12 управляет трёхбитным предделителем и уменьшением счётчика.
    mapper.write(0xC004,3); mapper.write(0xC005,1); mapper.write(0xC003,0);
    for (unsigned edge=0;edge<12;++edge) {
        mapper.ppuAddress(0); mapper.ppuAddress(0x1000);
        CHECK(mapper.irqLine() == (edge == 11));
    }
    mapper.write(0xC002,0); CHECK(!mapper.irqLine());
    mapper.write(0xC001,0x40); mapper.write(0xC004,0xFE); mapper.write(0xC005,0xFF); mapper.write(0xC000,1);
    mapper.cpuCycle(); CHECK(!mapper.irqLine()); mapper.cpuCycle(); CHECK(mapper.irqLine());
    mapper.write(0xC002,0); mapper.write(0xC001,0x47); mapper.write(0xC004,6); mapper.write(0xC005,0xFF); mapper.write(0xC003,0);
    mapper.cpuWriteStrobe(); CHECK(!mapper.irqLine()); mapper.cpuWriteStrobe(); CHECK(mapper.irqLine());
    mapper.write(0xC002,0); mapper.write(0xC001,0x46); mapper.write(0xC004,6); mapper.write(0xC005,0xFF); mapper.write(0xC003,0);
    mapper.ppuAddress(0); mapper.ppuAddress(0x1000); CHECK(!mapper.irqLine());
    mapper.ppuRead(); CHECK(!mapper.irqLine()); mapper.ppuRead(); CHECK(mapper.irqLine());
    mapper.write(0xC002,0); mapper.write(0xC001,0x40); mapper.write(0xC006,0xAA);
    mapper.write(0xC004,0x54); mapper.write(0xC005,0x55); mapper.write(0xC003,0);
    mapper.cpuCycle(); CHECK(!mapper.irqLine()); mapper.cpuCycle(); CHECK(mapper.irqLine()); // XOR задаёт исходные FE/FF.
    mapper.write(0xC002,0);
    mapper.write(0x5800,0x68); mapper.write(0x5801,0x37);
    for(unsigned i=0;i<8;++i) mapper.cpuCycle();
    CHECK(mapper.cpuRead(0x5800,0)==0x58 && mapper.cpuRead(0x5801,0)==0x16);
    mapper.write(0x5802,250); mapper.write(0x5802,10); CHECK(mapper.cpuRead(0x5802,0)==4);
    mapper.write(0x5803,0x5A); CHECK(mapper.cpuRead(0x5802,0)==0 && mapper.cpuRead(0x5803,0)==0x5A);
    CHECK(mapper.cpuRead(0x5000,0xFF)==0x3F);
    CHECK(mapper.cpuRead(0x5001,0xA5)==0xA5);
}

// Это проверка банков ROM и протокола. Искусственный PPU с постоянным vblank
// не является проверкой gameplay, видео, звука или временной модели.
/**
 * @brief Минимальная CPU-шина банкового ROM-протокола с собственными PRG и2KiB RAM.
 * @details Каждый read/write продвигает настоящий Legacy160 CPU-cycle. PPUSTATUS искусственно всегда vblank; поэтому этот fixture не проверяет gameplay, рендеринг или PPU timing.
 */
struct BootBus final : nes::core::CpuBus {
    nes::core::Legacy160 mapper;
    std::vector<std::uint8_t> prg;
    std::array<std::uint8_t,2048> ram{};
    std::array<unsigned,4> prgWrites{};
    std::array<unsigned,8> chrWrites{};
    unsigned modeWrites=0, irqWrites=0, jumperReads=0;
    std::uint8_t mode=0,irqMode=0;
    /**
     * @brief Выполняет mapper.cpuCycle и читает RAM, выбранный PRG либо регистр Legacy160.
     * @details PPUSTATUS возвращает искусственный$80, а jumper-read считается отдельно: функция проверяет банковый boot-протокол, не PPU-время.
     */
    std::uint8_t read(std::uint16_t address) noexcept override {
        mapper.cpuCycle();
        if (address<0x2000) return ram[address&2047];
        if (address>=0x8000) return prg[mapper.prgOffset(address)];
        if (address>=0x2000 && address<0x4000) return (address&7)==2 ? 0x80 : 0;
        if (address==0x5000) ++jumperReads;
        return mapper.cpuRead(address,0);
    }
    /**
     * @brief На настоящем test CPU-cycle подаёт write-strobe Legacy160 и учитывает PRG/CHR/mode/IRQ записи.
     * @details RAM принадлежит BootBus; mapper получает исходные address/value без вычисления ожидаемого результата.
     */
    void write(std::uint16_t address,std::uint8_t value) noexcept override {
        mapper.cpuCycle(); mapper.cpuWriteStrobe();
        if (address<0x2000) ram[address&2047]=value;
        if (address>=0x8000 && address<=0x8003) ++prgWrites[address&3];
        if (address>=0x9000 && address<=0x9007) ++chrWrites[address&7];
        if (address==0xD000) { ++modeWrites; mode=value; }
        if (address==0xC001) { ++irqWrites; irqMode=value; }
        mapper.write(address,value);
    }
};

/** @brief Проверяет банковый протокол реального ROM отдельно от временной модели PPU. */
void actualRom(const char* path) {
    std::FILE* input=std::fopen(path,"rb"); CHECK(input!=nullptr);
    std::array<std::uint8_t,16> header{}; CHECK(std::fread(header.data(),1,16,input)==16);
    CHECK(header[4]==16 && header[5]==32 && header[7]==0xA0);
    BootBus bus; bus.prg.resize(262144); CHECK(std::fread(bus.prg.data(),1,bus.prg.size(),input)==bus.prg.size()); std::fclose(input);
    bus.mapper.reset(bus.prg.size(),262144);
    nes::core::Cpu6502 cpu(bus); cpu.reset(); CHECK(cpu.state().pc==0xE0EF);
    for(unsigned i=0;i<100000;++i) { const auto spent=cpu.step(); CHECK(spent>0 && !cpu.state().jammed); }
    CHECK(bus.modeWrites>0 && bus.mode==0x1E);
    CHECK(bus.irqWrites>0 && bus.irqMode==0x85);
    CHECK(bus.prgWrites[0]>0 && bus.prgWrites[1]>0 && bus.prgWrites[2]>0 && bus.prgWrites[3]>0);
    CHECK(bus.jumperReads>0);
    std::printf("Aladdin protocol probe: 100000 own-CPU instructions, PC=%04X, mode=%02X IRQmode=%02X, PRG writes=%u/%u/%u/%u jumper reads=%u\n",
        cpu.state().pc,bus.mode,bus.irqMode,bus.prgWrites[0],bus.prgWrites[1],bus.prgWrites[2],bus.prgWrites[3],bus.jumperReads);
}

} // namespace

/**
 * @brief Проверяет банки, register masks, CHR latches, multiplier и источники IRQ Legacy160.
 * @details При одном заданном ROM-пути дополнительно выполняет100000 инструкций банкового boot-probe; это не gameplay/video/audio qualification.
 */
int main(int argc,char** argv) {
    banks(); interruptsAndRegisters();
    std::puts("Legacy160 tests passed: banks/register masks/CHR latches/multiplier/allIRQclock sources");
    if(argc==2) actualRom(argv[1]);
}
