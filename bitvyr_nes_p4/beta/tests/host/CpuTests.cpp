#include "cpu/Cpu6502.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using nes::core::Cpu6502;

namespace {

/**
 * @brief Один наблюдённый CPU-доступ:16-битный адрес, байт и направление read/write.
 * @details Журнал содержит реальные такты шины, включая dummy/RMW, а не декодированные инструкции.
 */
struct Trace {
    std::uint16_t address;
    std::uint8_t value;
    bool write;
};

/**
 * @brief Независимая CPU-шина с собственными64KiB памяти и журналом первых64 доступов.
 * @details count считает все обращения; cpu заимствован только для подачи IRQ/NMI на выбранном номере доступа. Память и trace не разделяются с другой Machine.
 */
struct Bus final : nes::core::CpuBus {
    std::array<std::uint8_t, 65536> memory{};
    std::array<Trace, 64> trace{};
    unsigned count = 0;
    Cpu6502* cpu = nullptr;
    int irqOnCycle = -1;
    int nmiOnCycle = -1;

    /** @brief Добавляет один реальный доступ в ограниченный журнал тестовой CPU-шины. */
    void append(std::uint16_t address, std::uint8_t value, bool write) noexcept {
        if (count < trace.size()) trace[count] = {address, value, write};
        ++count;
        if (cpu && static_cast<int>(count) == irqOnCycle) cpu->setIrqLine(true);
        if (cpu && static_cast<int>(count) == nmiOnCycle) cpu->requestNmi();
    }

    /**
     * @brief Записывает read address/value в trace, подаёт назначенные IRQ/NMI и возвращает memory[address].
     * @details Ни декодирование opcode, ни expected cycle sequence шиной не вычисляются.
     */
    std::uint8_t read(std::uint16_t address) noexcept override {
        append(address, memory[address], false);
        return memory[address];
    }

    /**
     * @brief Регистрирует настоящий write до изменения memory[address].
     * @details Такой порядок сохраняет отдельные old/new записи RMW и момент подачи test IRQ/NMI.
     */
    void write(std::uint16_t address, std::uint8_t value) noexcept override {
        append(address, value, true);
        memory[address] = value;
    }

    /** @brief Обнуляет только журнал теста, сохраняя память шины и состояние CPU. */
    void clearTrace() noexcept { count = 0; irqOnCycle = nmiOnCycle = -1; }
};

/**
 * @brief Завершает CPU-fixture кодом1 с текстом CHECK и номером исходной строки.
 * @details Не зависит от C assert/NDEBUG и не открывает GUI assertion dialog.
 */
[[noreturn]] void fail(const char* label, unsigned line) {
    std::fprintf(stderr, "CPU TEST FAILED line %u: %s\n", line, label);
    std::exit(1);
}
#define CHECK(condition) do { if (!(condition)) fail(#condition, __LINE__); } while (false)

/** @brief Формирует исходное состояние CPU с заданным PC для независимого случая. */
Cpu6502::State stateAt(std::uint16_t pc) {
    Cpu6502::State state;
    state.pc = pc;
    state.sp = 0xFD;
    state.p = 0x24;
    return state;
}

/** @brief Заполняет тестовую память программой до запуска CPU. */
void load(Bus& bus, Cpu6502& cpu, const std::uint8_t* code, unsigned length, std::uint16_t pc = 0x8000) {
    bus.memory.fill(0);
    bus.clearTrace();
    cpu.setIrqLine(false);
    cpu.setState(stateAt(pc));
    bus.cpu = &cpu;
    for (unsigned i = 0; i < length; ++i) bus.memory[static_cast<std::uint16_t>(pc + i)] = code[i];
}

/** @brief Сравнивает очередной адрес, направление и байт настоящего доступа CPU. */
void expectCycle(const Bus& bus, unsigned cycle, std::uint16_t address, bool write = false, int value = -1) {
    CHECK(cycle < bus.count);
    CHECK(bus.trace[cycle].address == address);
    CHECK(bus.trace[cycle].write == write);
    if (value >= 0) CHECK(bus.trace[cycle].value == value);
}

/** @brief Проверяет reset-вектор, dummy-доступы и порядок стековых операций. */
void resetAndStack(Bus& bus, Cpu6502& cpu) {
    bus.memory.fill(0);
    bus.clearTrace();
    bus.memory[0xFFFC] = 0x34;
    bus.memory[0xFFFD] = 0x92;
    cpu.setState(Cpu6502::State{});
    cpu.reset();
    CHECK(cpu.state().pc == 0x9234);
    CHECK(cpu.state().sp == 0xFD);
    CHECK(cpu.state().p == 0x24);
    CHECK(cpu.state().cycles == 7);
    CHECK(bus.count == 7);
    expectCycle(bus, 0, 0); expectCycle(bus, 1, 0);
    expectCycle(bus, 2, 0x100); expectCycle(bus, 3, 0x1FF); expectCycle(bus, 4, 0x1FE);
    expectCycle(bus, 5, 0xFFFC); expectCycle(bus, 6, 0xFFFD);
    for (unsigned i = 0; i < bus.count; ++i) CHECK(!bus.trace[i].write);

    const std::uint8_t stackCode[] = {0xA9,0x80,0x48,0xA9,0x00,0x68,0x08,0x28};
    load(bus, cpu, stackCode, sizeof(stackCode));
    CHECK(cpu.step() == 2); CHECK(cpu.step() == 3); CHECK(bus.memory[0x1FD] == 0x80);
    CHECK(cpu.step() == 2); CHECK(cpu.step() == 4); CHECK(cpu.state().a == 0x80);
    CHECK(cpu.state().sp == 0xFD); CHECK((cpu.state().p & 0x82) == 0x80);
    CHECK(cpu.step() == 3); CHECK((bus.memory[0x1FD] & 0x30) == 0x30);
    CHECK(cpu.step() == 4); CHECK((cpu.state().p & 0x30) == 0x20);
}

/** @brief Полностью перебирает арифметические входы/carry по независимому целочисленному эталону. */
void arithmeticExhaustive(Bus& bus, Cpu6502& cpu) {
    // Независимый целочисленный эталон перебирает входы и carry с установленным D,
    // проверяя именно отсутствие BCD-эффекта в NES.
    bus.memory.fill(0);
    auto state = stateAt(0x8000);
    for (unsigned operation = 0; operation < 2; ++operation) {
        bus.memory[0x8000] = operation == 0 ? 0x69 : 0xE9;
        for (unsigned a = 0; a < 256; ++a) {
            for (unsigned operand = 0; operand < 256; ++operand) {
                for (unsigned carry = 0; carry < 2; ++carry) {
                    state.a = static_cast<std::uint8_t>(a);
                    state.p = static_cast<std::uint8_t>(0x2C | carry);
                    cpu.setState(state);
                    bus.clearTrace();
                    bus.memory[0x8001] = static_cast<std::uint8_t>(operand);
                    CHECK(cpu.step() == 2);
                    const int integer = operation == 0 ? static_cast<int>(a + operand + carry) : static_cast<int>(a) - static_cast<int>(operand) - static_cast<int>(1 - carry);
                    const unsigned result = static_cast<unsigned>(integer) & 255U;
                    const bool expectedCarry = operation == 0 ? integer > 255 : integer >= 0;
                    const bool expectedOverflow = operation == 0 ? ((~(a ^ operand) & (a ^ result) & 128U) != 0) : (((a ^ operand) & (a ^ result) & 128U) != 0);
                    CHECK(cpu.state().a == result);
                    CHECK(((cpu.state().p & 1) != 0) == expectedCarry);
                    CHECK(((cpu.state().p & 2) != 0) == (result == 0));
                    CHECK(((cpu.state().p & 0x40) != 0) == expectedOverflow);
                    CHECK(((cpu.state().p & 0x80) != 0) == ((result & 128U) != 0));
                    CHECK((cpu.state().p & 0x2C) == 0x2C);
                    CHECK(cpu.state().pc == 0x8002);
                }
            }
        }
    }
}

/** @brief Проверяет порядок indexed/dummy-доступов при пересечении страниц. */
void indexedBusSequences(Bus& bus, Cpu6502& cpu) {
    const std::uint8_t readCode[] = {0xBD,0xFF,0x20};
    load(bus, cpu, readCode, sizeof(readCode));
    auto state = cpu.state(); state.x = 1; cpu.setState(state);
    bus.memory[0x2000] = 0x55; bus.memory[0x2100] = 0xAA;
    CHECK(cpu.step() == 5); CHECK(cpu.state().a == 0xAA);
    expectCycle(bus, 3, 0x2000); expectCycle(bus, 4, 0x2100);

    const std::uint8_t writeCode[] = {0x9D,0x01,0x20};
    load(bus, cpu, writeCode, sizeof(writeCode));
    state = cpu.state(); state.x = 1; state.a = 0x42; cpu.setState(state);
    CHECK(cpu.step() == 5);
    expectCycle(bus, 3, 0x2002); expectCycle(bus, 4, 0x2002, true, 0x42);

    const std::uint8_t indirect[] = {0xB1,0xFF};
    load(bus, cpu, indirect, sizeof(indirect));
    state = cpu.state(); state.y = 1; cpu.setState(state);
    bus.memory[0xFF] = 0xFF; bus.memory[0] = 0x20; bus.memory[0x100] = 0x70;
    bus.memory[0x2100] = 0x63;
    CHECK(cpu.step() == 6); CHECK(cpu.state().a == 0x63);
    expectCycle(bus, 2, 0xFF); expectCycle(bus, 3, 0); expectCycle(bus, 4, 0x2000); expectCycle(bus, 5, 0x2100);

    const std::uint8_t indirectX[] = {0xA1,0xFF};
    load(bus, cpu, indirectX, sizeof(indirectX));
    state = cpu.state(); state.x = 1; cpu.setState(state);
    bus.memory[0] = 0x34; bus.memory[1] = 0x12; bus.memory[0x1234] = 0x91;
    CHECK(cpu.step() == 6); CHECK(cpu.state().a == 0x91);
    expectCycle(bus, 2, 0xFF); expectCycle(bus, 3, 0); expectCycle(bus, 4, 1); expectCycle(bus, 5, 0x1234);

    const std::uint8_t zpIndexed[] = {0xB5,0xFE};
    load(bus, cpu, zpIndexed, sizeof(zpIndexed));
    state = cpu.state(); state.x = 3; cpu.setState(state); bus.memory[1] = 0x29;
    CHECK(cpu.step() == 4); CHECK(cpu.state().a == 0x29);
    expectCycle(bus, 2, 0xFE); expectCycle(bus, 3, 1);
}

/** @brief Проверяет обе записи RMW и точный порядок шинных тактов. */
void modifySequences(Bus& bus, Cpu6502& cpu) {
    const std::uint8_t code[] = {0x06,0x80};
    load(bus, cpu, code, sizeof(code));
    bus.memory[0x80] = 0x80;
    CHECK(cpu.step() == 5);
    expectCycle(bus, 2, 0x80, false, 0x80);
    expectCycle(bus, 3, 0x80, true, 0x80); expectCycle(bus, 4, 0x80, true, 0);
    CHECK((cpu.state().p & 0x83) == 3);

    const std::uint8_t rmwIndexed[] = {0xFE,0xFF,0x20};
    load(bus, cpu, rmwIndexed, sizeof(rmwIndexed));
    auto state = cpu.state(); state.x = 1; cpu.setState(state);
    bus.memory[0x2100] = 0x7F;
    CHECK(cpu.step() == 7);
    expectCycle(bus, 3, 0x2000); expectCycle(bus, 4, 0x2100, false, 0x7F);
    expectCycle(bus, 5, 0x2100, true, 0x7F); expectCycle(bus, 6, 0x2100, true, 0x80);
}

/** @brief Проверяет переходы, вызовы, возвраты и аппаратное поведение JMP indirect. */
void controlFlow(Bus& bus, Cpu6502& cpu) {
    const std::uint8_t branch[] = {0xD0,0x02};
    load(bus, cpu, branch, sizeof(branch), 0x80FD);
    CHECK(cpu.step() == 4); CHECK(cpu.state().pc == 0x8101);
    expectCycle(bus, 2, 0x80FF); expectCycle(bus, 3, 0x8001);
    auto state = stateAt(0x80FD); state.p |= 2; cpu.setState(state); bus.clearTrace();
    CHECK(cpu.step() == 2); CHECK(cpu.state().pc == 0x80FF);

    const std::uint8_t jump[] = {0x6C,0xFF,0x12};
    load(bus, cpu, jump, sizeof(jump));
    bus.memory[0x12FF] = 0x34; bus.memory[0x1200] = 0x56; bus.memory[0x1300] = 0x78;
    CHECK(cpu.step() == 5); CHECK(cpu.state().pc == 0x5634);
    expectCycle(bus, 3, 0x12FF); expectCycle(bus, 4, 0x1200);

    const std::uint8_t jsr[] = {0x20,0x34,0x92};
    load(bus, cpu, jsr, sizeof(jsr)); bus.memory[0x9234] = 0x60;
    CHECK(cpu.step() == 6); CHECK(cpu.state().pc == 0x9234); CHECK(cpu.state().sp == 0xFB);
    expectCycle(bus, 2, 0x1FD); expectCycle(bus, 3, 0x1FD, true, 0x80); expectCycle(bus, 4, 0x1FC, true, 0x02);
    expectCycle(bus, 5, 0x8002);
    bus.clearTrace();
    CHECK(cpu.step() == 6); CHECK(cpu.state().pc == 0x8003); CHECK(cpu.state().sp == 0xFD);
    expectCycle(bus, 5, 0x8002);
}

/** @brief Проверяет моменты IRQ/NMI, захват вектора и сохранение status на стеке. */
void interruptSequences(Bus& bus, Cpu6502& cpu) {
    const std::uint8_t code[] = {0x00,0xAA,0xEA};
    load(bus, cpu, code, sizeof(code));
    bus.memory[0xFFFE] = 0x34; bus.memory[0xFFFF] = 0x92; bus.memory[0x9234] = 0x40;
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9234);
    expectCycle(bus, 1, 0x8001); expectCycle(bus, 2, 0x1FD, true, 0x80); expectCycle(bus, 3, 0x1FC, true, 2);
    CHECK((bus.memory[0x1FB] & 0x30) == 0x30);
    CHECK((cpu.state().p & 0x30) == 0x20);
    CHECK(cpu.step() == 6); CHECK(cpu.state().pc == 0x8002);

    const std::uint8_t delayed[] = {0x58,0xEA,0xEA};
    load(bus, cpu, delayed, sizeof(delayed));
    bus.memory[0xFFFE] = 0x34; bus.memory[0xFFFF] = 0x92;
    cpu.setIrqLine(true);
    CHECK(cpu.step() == 2); CHECK(cpu.state().pc == 0x8001); // CLI
    CHECK(cpu.step() == 2); CHECK(cpu.state().pc == 0x8002); // IRQ разрешается после задержки на одну инструкцию.
    bus.clearTrace();
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9234);
    CHECK((bus.memory[0x1FB] & 0x10) == 0);
    expectCycle(bus, 0, 0x8002); expectCycle(bus, 1, 0x8002);

    const std::uint8_t sei[] = {0x78,0xEA};
    load(bus, cpu, sei, sizeof(sei));
    auto state = cpu.state(); state.p &= static_cast<std::uint8_t>(~4U); cpu.setState(state);
    bus.memory[0xFFFE] = 0x34; bus.memory[0xFFFF] = 0x92;
    bus.irqOnCycle = 1;
    CHECK(cpu.step() == 2); CHECK((cpu.state().p & 4) != 0);
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9234); // SEI не отменяет IRQ, уже замеченный при предыдущем опросе.

    const std::uint8_t nop[] = {0xEA};
    load(bus, cpu, nop, sizeof(nop));
    bus.memory[0xFFFA] = 0x78; bus.memory[0xFFFB] = 0x96;
    cpu.requestNmi();
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9678);
    CHECK((bus.memory[0x1FB] & 0x10) == 0);

    load(bus, cpu, code, sizeof(code));
    bus.memory[0xFFFE] = 0x34; bus.memory[0xFFFF] = 0x92;
    bus.memory[0xFFFA] = 0x78; bus.memory[0xFFFB] = 0x96;
    bus.nmiOnCycle = 5; // Подтверждённый фронт во время записи status предшествует чтению вектора.
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9678);
    CHECK((bus.memory[0x1FB] & 0x10) != 0);

    load(bus, cpu, code, sizeof(code));
    bus.memory[0xFFFE] = 0x34; bus.memory[0xFFFF] = 0x92; bus.memory[0x9234] = 0xEA;
    bus.memory[0xFFFA] = 0x78; bus.memory[0xFFFB] = 0x96;
    bus.nmiOnCycle = 6; // Фронт во время чтения вектора ожидает инструкции обработчика.
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9234);
    CHECK(cpu.step() == 2); CHECK(cpu.state().pc == 0x9235);
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9678);

    load(bus, cpu, nop, sizeof(nop));
    bus.memory[0x8001] = 0xEA;
    bus.memory[0xFFFA] = 0x78; bus.memory[0xFFFB] = 0x96;
    bus.nmiOnCycle = 2; // requestNmi передаёт уже квалифицированный фронт.
    CHECK(cpu.step() == 2); CHECK(cpu.state().pc == 0x8001);
    CHECK(cpu.step() == 7); CHECK(cpu.state().pc == 0x9678);
}

/** @brief Проверяет реализованные неофициальные NMOS-opcode по явным ожидаемым эффектам. */
void unofficial(Bus& bus, Cpu6502& cpu) {
    /**
     * @brief Независимая строка ожиданий неофициального opcode.
     * @details Задаёт начальные A/X/RAM/P и ожидаемые A/X/RAM/NVZC; числовые ожидания не вычисляются проверяемым decoder.
     */
    struct Case { std::uint8_t opcode, a, x, memory, p, expectedA, expectedX, expectedMemory, expectedFlags; };
    const Case cases[] = {
        {0x07,0x01,0,0x80,0x24,0x01,0,0x00,0x01}, // SLO
        {0x27,0xFF,0,0x80,0x25,0x01,0,0x01,0x01}, // RLA
        {0x47,0xFF,0,0x01,0x24,0xFF,0,0x00,0x81}, // SRE
        {0x67,0x40,0,0x01,0x24,0x41,0,0x00,0x00}, // RRA передаёт carry в ADC.
        {0xC7,0x10,0,0x11,0x24,0x10,0,0x10,0x03}, // DCP
        {0xE7,0x20,0,0x0F,0x25,0x10,0,0x10,0x01}, // ISC
        {0xA7,0,0,0x80,0x24,0x80,0x80,0x80,0x80}, // LAX
        {0x87,0x3C,0xF0,0,0x24,0x3C,0xF0,0x30,0x00}, // SAX
    };
    for (const auto& item : cases) {
        const std::uint8_t code[] = {item.opcode,0x40};
        load(bus, cpu, code, sizeof(code));
        auto state = cpu.state(); state.a = item.a; state.x = item.x; state.p = item.p; cpu.setState(state);
        bus.memory[0x40] = item.memory;
        const auto cycles = cpu.step();
        CHECK(cycles == (item.opcode == 0xA7 || item.opcode == 0x87 ? 3U : 5U));
        CHECK(cpu.state().a == item.expectedA); CHECK(cpu.state().x == item.expectedX);
        CHECK(bus.memory[0x40] == item.expectedMemory);
        CHECK((cpu.state().p & 0xC3) == item.expectedFlags);
    }
    const std::uint8_t arr[] = {0x6B,0xFF};
    load(bus, cpu, arr, sizeof(arr));
    auto state = cpu.state(); state.a = 0x80; state.p = 0x25; cpu.setState(state);
    CHECK(cpu.step() == 2); CHECK(cpu.state().a == 0xC0); CHECK((cpu.state().p & 0xC3) == 0xC1);
    const std::uint8_t axs[] = {0xCB,0x11};
    load(bus, cpu, axs, sizeof(axs));
    state = cpu.state(); state.a = 0xFF; state.x = 0x10; cpu.setState(state);
    CHECK(cpu.step() == 2); CHECK(cpu.state().x == 0xFF); CHECK((cpu.state().p & 0x83) == 0x80);

    const std::uint8_t nop[] = {0x1C,0xFF,0x20};
    load(bus, cpu, nop, sizeof(nop));
    state = cpu.state(); state.x = 1; cpu.setState(state);
    CHECK(cpu.step() == 5); CHECK(cpu.state().pc == 0x8003);
    expectCycle(bus, 3, 0x2000); expectCycle(bus, 4, 0x2100);
}

/** @brief Проходит таблицу opcode, проверяя завершение и ожидаемые классы инструкций. */
void decodeSmoke(Bus& bus, Cpu6502& cpu) {
    unsigned jammed = 0;
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const std::uint8_t code[] = {static_cast<std::uint8_t>(opcode),0,0};
        load(bus, cpu, code, sizeof(code));
        const auto cycles = cpu.step();
        CHECK(cycles >= 2 && cycles <= 8);
        CHECK(cycles == bus.count); CHECK(cpu.state().cycles == cycles);
        CHECK((cpu.state().p & 0x30) == 0x20);
        if (cpu.state().jammed) {
            ++jammed;
            CHECK(cpu.step() == 1);
            CHECK(cpu.state().jammed);
            cpu.reset(); CHECK(!cpu.state().jammed);
        }
    }
    CHECK(jammed == 12);
}

// Необязательная проверка по публичному аппаратному эталонному журналу nestest.
// Используются внешние тестовые данные, а не код другого эмулятора.
/** @brief Сравнивает внешний аппаратный журнал nestest; данные заимствуются только на время проверки. */
void nestestTrace(Bus& bus, Cpu6502& cpu, const char* romPath, const char* logPath) {
    std::FILE* rom = std::fopen(romPath, "rb");
    CHECK(rom != nullptr);
    std::array<std::uint8_t, 16> header{};
    CHECK(std::fread(header.data(), 1, header.size(), rom) == header.size());
    CHECK(std::memcmp(header.data(), "NES\x1A", 4) == 0 && header[4] == 1);
    bus.memory.fill(0);
    CHECK(std::fread(bus.memory.data() + 0x8000, 1, 16384, rom) == 16384);
    std::fclose(rom);
    std::memcpy(bus.memory.data() + 0xC000, bus.memory.data() + 0x8000, 16384);
    std::FILE* log = std::fopen(logPath, "rb");
    CHECK(log != nullptr);
    auto state = stateAt(0xC000); state.cycles = 7;
    cpu.setIrqLine(false); cpu.setState(state); bus.clearTrace();
    char line[512];
    unsigned lines = 0;
    while (std::fgets(line, sizeof(line), log)) {
        unsigned pc = 0, a = 0, x = 0, y = 0, p = 0, sp = 0;
        unsigned long long cycles = 0;
        const char* regs = std::strstr(line, "A:");
        const char* cycleText = std::strstr(line, "CYC:");
        CHECK(std::sscanf(line, "%x", &pc) == 1);
        CHECK(regs != nullptr && cycleText != nullptr);
        CHECK(std::sscanf(regs, "A:%x X:%x Y:%x P:%x SP:%x", &a, &x, &y, &p, &sp) == 5);
        CHECK(std::sscanf(cycleText, "CYC:%llu", &cycles) == 1);
        const auto& actual = cpu.state();
        if (actual.pc != pc || actual.a != a || actual.x != x || actual.y != y || actual.p != p || actual.sp != sp || actual.cycles != cycles) {
            std::fprintf(stderr, "nestest mismatch at line %u\nexpected: %sactual: PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%llu\n",
                lines + 1, line, actual.pc, actual.a, actual.x, actual.y, actual.p, actual.sp, static_cast<unsigned long long>(actual.cycles));
            fail("nestest golden CPU state", __LINE__);
        }
        ++lines;
        bus.clearTrace();
        const auto spent = cpu.step();
        CHECK(spent > 0 && !cpu.state().jammed);
    }
    std::fclose(log);
    CHECK(lines >= 8000);
    std::printf("nestest: %u golden instruction states and cycle counts matched\n", lines);
}

} // namespace

/**
 * @brief Проверяет reset/stack, полный ADC/SBC перебор, dummy/RMW/control-flow/IRQ и все256 opcode decode.
 * @details Только при двух заданных путях дополнительно сравнивает внешний nestest ROM/log; обычный CTest не подразумевает наличие этих данных.
 */
int main(int argc, char** argv) {
    static Bus bus;
    Cpu6502 cpu(bus);
    resetAndStack(bus,cpu);
    arithmeticExhaustive(bus,cpu);
    indexedBusSequences(bus,cpu);
    modifySequences(bus,cpu);
    controlFlow(bus,cpu);
    interruptSequences(bus,cpu);
    unofficial(bus,cpu);
    decodeSmoke(bus,cpu);
    std::puts("CPU tests passed: exhaustive ADC/SBC, bus cycles, addressing, stack, interrupts, illegal opcodes, all256 decode");
    if (argc == 3) nestestTrace(bus,cpu,argv[1],argv[2]);
    return 0;
}
