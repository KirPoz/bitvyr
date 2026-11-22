#include "Machine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

/** @brief Останавливает fixture при нарушении независимого ожидаемого условия. */
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

/** @brief Проверяет, что OAM DMA останавливает именно read-такт CPU и сохраняет переданные байты. */
void dmaStartsOnRead(bool alternateParity) {
    // Собственная короткая программа заполняет CPU-страницу$02
    // индексами байтов, запрашивает OAM DMA и читает две позиции OAM.
    std::vector<std::uint8_t> program{
        0xA2, 0x00,                         // LDX #0
        0x8A,                               // TXA
        0x9D, 0x00, 0x02,                   // STA $0200,X
        0xE8,                               // INX
        0xD0, 0xF9,                         // BNE возвращается в цикл.
    };
    if (alternateParity) { program.push_back(0x24); program.push_back(0x00); } // BIT$00 исполняет три такта.
    program.push_back(0xA9); program.push_back(0x02); // LDA #$02
    const auto dmaInstruction = static_cast<std::uint16_t>(0xC000 + program.size());
    const std::uint8_t tail[]{
        0x8D, 0x14, 0x40,                   // STA $4014
        0xEA,                               // Чтение opcode NOP останавливается DMA.
        0xA9, 0x80, 0x8D, 0x03, 0x20,       // LDA #$80 / STA $2003
        0xAD, 0x04, 0x20, 0x85, 0x10,       // LDA $2004 / STA $10
        0xA9, 0xFE, 0x8D, 0x03, 0x20,
        0xAD, 0x04, 0x20, 0x85, 0x11,
    };
    program.insert(program.end(), std::begin(tail), std::end(tail));
    std::vector<std::uint8_t> rom(16 + 16384 + 8192, 0);
    rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1A;
    rom[4] = rom[5] = 1;
    for (std::size_t i = 0; i < program.size(); ++i) rom[16 + i] = program[i];
    rom[16 + 0x3FFC] = 0; rom[16 + 0x3FFD] = 0xC0;
    nes::core::Machine machine;
    machine.resetCpuReadWindow();
    require(machine.load(rom.data(), rom.size()) == nes::core::LoadResult::Ok, "original DMA test ROM loads");
    for (unsigned i = 0; i < 2000 && machine.cpuState().pc != dmaInstruction; ++i) machine.stepInstruction();
    require(machine.cpuState().pc == dmaInstruction, "DMA fixture reaches its transfer instruction");
    const auto beforeWrite = machine.cpuCycles();
    machine.stepInstruction();
    require(machine.cpuCycles() - beforeWrite == 4, "writing OAMDMA takes four instruction cycles, without halting a write");
    require(machine.cpuState().pc == dmaInstruction + 3, "CPU advances to instruction whose read will be halted");
    const auto beforeRead = machine.cpuCycles();
    const auto beforeCpu = machine.cpuState().cycles;
    const auto expected = (beforeRead & 1) ? 516U : 515U;
    machine.stepInstruction();
    require(machine.cpuCycles() - beforeRead == expected, "following NOP read includes 513/514 physical DMA cycles");
    require(machine.cpuState().cycles - beforeCpu == 2, "DMA preserves NOP's two CPU bus requests");
    require(machine.cpuState().pc == dmaInstruction + 4, "CPU completes the instruction it began before DMA halt");
    for (unsigned i = 0; i < 8; ++i) machine.stepInstruction();
    require(machine.debugRead(0x10) == 0x80, "DMA copies CPU RAM into OAM");
    require(machine.debugRead(0x11) == 0xE2, "DMA applies OAM attribute storage bits");
}
}  // namespace

/**
 * @brief Проверяет read-start OAM DMA в обеих CPU parity, включая фактические stalls и два OAM-байта.
 * @details Используется собственная короткая iNES-программа; injected host-clock PROFILE не требуется.
 */
int main() {
    dmaStartsOnRead(false);
    dmaStartsOnRead(true);
    std::puts("Machine DMA timing passed for both CPU phase parities.");
}
