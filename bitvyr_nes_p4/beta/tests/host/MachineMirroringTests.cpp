#include "Machine.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;

/** @brief Останавливает fixture при нарушении независимого ожидаемого условия. */
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// Адресная проводка CIRAM описана независимо от Cartridge::Mirroring и
// реализации Machine; four-screen добавляет две собственные страницы.
// https://www.nesdev.org/wiki/PPU_memory_map
// https://www.nesdev.org/wiki/MMC1
// https://www.nesdev.org/wiki/MMC3
enum class Wiring { PpuA11, PpuA10, Ground, Vcc, FourScreen };

/**
 * @brief Независимые4KiB физических страниц CIRAM/four-screen и выбранная адресная проводка.
 * @details Ожидаемые aliases вычисляются побитно, без Cartridge::Mirroring и без вызова реализации Machine.
 */
struct ReferenceMemory {
    std::array<std::uint8_t, 4096> bytes{};
    Wiring wiring;

    /** @brief Вычисляет физическую CIRAM-проводку независимо от проверяемой Machine. */
    unsigned address(std::uint16_t ppuAddress) const {
        const unsigned low = ppuAddress & 0x3FF;
        switch (wiring) {
        case Wiring::PpuA11: return low | ((ppuAddress & 0x800) >> 1);
        case Wiring::PpuA10: return low | (ppuAddress & 0x400);
        case Wiring::Ground: return low;
        case Wiring::Vcc: return low | 0x400;
        case Wiring::FourScreen: return ppuAddress & 0xFFF;
        }
        std::abort();
    }
};

/**
 * @brief Ожидание одного буферизованного PPUDATA-read: PPU-адрес, байт и диагностическая фаза.
 * @details Значение вычислено независимой ReferenceMemory до исполнения тестовой программы.
 */
struct Observation {
    std::uint16_t ppuAddress;
    std::uint8_t value;
    std::string phase;
};

/**
 * @brief Строитель собственной CPU-программы проверки CIRAM и mapper-переключений.
 * @details Владеет code, эталонными physical pages и expected observations. Полученный ROM хранит внешний run до завершения Machine/load/reset.
 */
class Program {
public:
    /**
     * @brief Начинает программу с независимой проводкой initial, SEI/CLD и выключенным rendering/NMI.
     * @details Задаёт шаг PPUDATA1, чтобы последующие CPU-записи наблюдали точные байтовые aliases.
     */
    explicit Program(Wiring initial) : memory{{}, initial} {
        emit({0x78, 0xD8}); // SEI/CLD; каждый доступ выполняет настоящий CPU.
        store(0x2000, 0);  // NMI выключен, шаг PPUDATA равен одному байту.
        store(0x2001, 0);  // На время обращений к VRAM рендеринг выключен.
    }

    /** @brief Добавляет в тестовую программу настоящую CPU-запись по заданному адресу. */
    void store(std::uint16_t address, std::uint8_t value) {
        emit({0xA9, value, 0x8D, static_cast<std::uint8_t>(address),
              static_cast<std::uint8_t>(address >> 8)}); // LDA #value / STA abs
    }

    /** @brief Передаёт обе части адреса через регистр PPUADDR. */
    void setAddress(std::uint16_t address) {
        store(0x2006, static_cast<std::uint8_t>(address >> 8));
        store(0x2006, static_cast<std::uint8_t>(address));
    }

    /** @brief Сравнивает один read/write адрес через реальные CPU/PPU-регистры. */
    void probe(std::uint16_t address, const char* phase) {
        setAddress(address);
        const auto result = static_cast<std::uint16_t>(0x6000 + expected.size());
        emit({0xAD, 0x07, 0x20}); // Заполняется отложенный буфер чтения PPUDATA.
        // После$3EFF адрес переходит в palette RAM; чтение$3F00 обошло бы буфер.
        // Записи адреса сохраняют буфер; его читают через безопасное окно.
        if (address == 0x3EFF) setAddress(0x2000);
        emit({0xAD, 0x07, 0x20,
              0x8D, static_cast<std::uint8_t>(result), static_cast<std::uint8_t>(result >> 8)});
        expected.push_back({address, memory.bytes[memory.address(address)], phase});
    }

    /** @brief Проверяет все выбранные зеркальные окна и байтовые смещения. */
    void probeAll(const char* phase) {
        for (unsigned table = 0; table < 4; ++table) {
            for (const unsigned offset : offsets)
                probe(static_cast<std::uint16_t>(0x2000 + table * 1024 + offset), phase);
            // Зеркало$3000–$3EFF сохраняет и страницу, и байтовое смещение.
            probe(static_cast<std::uint16_t>(0x3155 + table * 1024), phase);
        }
        probe(0x3EFF, phase); // Последний байт зеркала nametable непосредственно перед palette RAM.
    }

    /** @brief Заполняет независимые страницы проверяемым байтовым рисунком. */
    void writePattern(unsigned phase) {
        for (unsigned table = 0; table < 4; ++table) {
            for (unsigned i = 0; i < offsets.size(); ++i) {
                const auto address = static_cast<std::uint16_t>(0x2000 + table * 1024 + offsets[i]);
                const auto value = static_cast<std::uint8_t>(0x13 + phase * 29 + table * 7 + i);
                setAddress(address);
                store(0x2007, value);
                memory.bytes[memory.address(address)] = value;
            }
        }
        // Запись идёт через верхнее зеркало, проверка — через нижнее окно.
        setAddress(0x3EFF);
        const auto value = static_cast<std::uint8_t>(0xA5 + phase);
        store(0x2007, value);
        memory.bytes[memory.address(0x3EFF)] = value;
        probe(0x2EFF, "write through $3000 mirror");
    }

    /** @brief Меняет mapper-проводку реальной CPU-записью без копирования RAM. */
    void change(unsigned mapper, unsigned mode, bool fourScreen = false) {
        Wiring next = memory.wiring;
        if (mapper == 1) {
            const unsigned control = 0x0C | mode; // Последнее окно16KiB остаётся фиксированным.
            for (unsigned bit = 0; bit < 4; ++bit)
                store(0x8000, static_cast<std::uint8_t>((control >> bit) & 1));
            probe(0x2400, "MMC1 four serial bits must not commit mirroring");
            store(0x8000, static_cast<std::uint8_t>((control >> 4) & 1));
            switch (mode) {
            case 0: next = Wiring::Ground; break;
            case 1: next = Wiring::Vcc; break;
            case 2: next = Wiring::PpuA10; break;
            case 3: next = Wiring::PpuA11; break;
            default: std::abort();
            }
        } else if (mapper == 7) {
            store(0x9FFF, static_cast<std::uint8_t>(mode ? 0x10 : 0));
            next = mode ? Wiring::Vcc : Wiring::Ground;
        } else if (mapper == 4) {
            store(0xA000, static_cast<std::uint8_t>(mode));
            next = fourScreen ? Wiring::FourScreen : ((mode & 1) ? Wiring::PpuA11 : Wiring::PpuA10);
        }
        // Переключение проводки не копирует и не очищает RAM.
        memory.wiring = next;
    }

    /**
     * @brief Формирует собственный iNES с PRG32KiB/CHR RAM и собранной CPU-программой.
     * @return Vector ROM с reset/IRQ/NMI vectors; caller удерживает backing до load, исполнения и повторного reset.
     */
    std::vector<std::uint8_t> image(unsigned mapper, std::uint8_t flags) {
        store(0x07FF, 0xC7);
        const auto stop = static_cast<std::uint16_t>(0xE000 + code.size());
        emit({0x4C, static_cast<std::uint8_t>(stop), static_cast<std::uint8_t>(stop >> 8)});
        require(code.size() <= 8192 - 6, "test program fits in the fixed $E000 bank");
        require(expected.size() < 8192, "test observations fit in PRG RAM");
        std::vector<std::uint8_t> rom(16 + 32768, 0xEA);
        for (unsigned i = 0; i < 16; ++i) rom[i] = 0;
        rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1A;
        rom[4] = 2; // Собственная iNES-программа PRG32KiB с CHR RAM8KiB.
        rom[6] = static_cast<std::uint8_t>((mapper << 4) | flags);
        rom[7] = static_cast<std::uint8_t>(mapper & 0xF0);
        rom[16 + 0x1FFF] = 0xFF; // Адрес записи AxROM выбран с учётом AND-конфликта шины.
        for (std::size_t i = 0; i < code.size(); ++i) rom[16 + 0x6000 + i] = code[i];
        for (unsigned vector = 0x7FFA; vector <= 0x7FFE; vector += 2) {
            rom[16 + vector] = 0; rom[16 + vector + 1] = 0xE0;
        }
        return rom;
    }

    ReferenceMemory memory;
    std::vector<Observation> expected;

private:
    /**
     * @brief Копирует последовательность opcode/operand bytes в собственный code vector.
     * @param bytes Временный initializer_list; заимствование заканчивается после insert.
     * @details Выделения vector допустимы только в host-builder до запуска Machine; скрытого ROM backing у ядра нет.
     */
    void emit(std::initializer_list<std::uint8_t> bytes) { code.insert(code.end(), bytes); }
    static constexpr std::array<unsigned, 4> offsets{0, 0x155, 0x3C0, 0x3FF};
    std::vector<std::uint8_t> code;
};

/** @brief Сравнивает результат программы с независимой таблицей ожидаемых байтов. */
void verify(nes::core::Machine& machine, const Program& program, const char* name) {
    for (unsigned instruction = 0; instruction < 20000 && machine.debugRead(0x07FF) != 0xC7; ++instruction) {
        machine.stepInstruction();
        require(!machine.cpuState().jammed, "mirroring program must not jam the CPU");
    }
    require(machine.debugRead(0x07FF) == 0xC7, "mirroring program reaches its completion marker");
    for (std::size_t i = 0; i < program.expected.size(); ++i) {
        const auto actual = machine.debugRead(static_cast<std::uint16_t>(0x6000 + i));
        const auto& expected = program.expected[i];
        if (actual != expected.value) {
            std::fprintf(stderr, "%s probe %zu, %s, PPU $%04X: expected $%02X, got $%02X\n",
                         name, i, expected.phase.c_str(), expected.ppuAddress, expected.value, actual);
            std::exit(1);
        }
        ++checks;
    }
}

/**
 * @brief Проверяет начальные и переключённые CIRAM aliases через реальную CPU-программу и повторный reset.
 * @details Program сначала строит независимые ожидания для каждой mode-фазы; Machine исполняет PPUDATA accesses до маркера завершения, затем сравнивается каждый байт.
 */
void run(nes::core::Machine& machine, const char* name, unsigned mapper,
         std::uint8_t flags, Wiring initial, std::initializer_list<unsigned> modes) {
    Program program(initial);
    program.probeAll("reset mapping and cleared VRAM");
    program.writePattern(0);
    program.probeAll("initial nametable aliases");
    unsigned phase = 1;
    for (const auto mode : modes) {
        program.change(mapper, mode, (flags & 8) != 0);
        program.probeAll("changed aliases retain old physical pages");
        program.writePattern(phase++);
        program.probeAll("writes through changed aliases");
    }
    if (mapper == 1) {
        program.store(0x8000, 0x80);
        program.probe(0x2000, "MMC1 serial reset preserves mirroring bits");
        program.probe(0x2400, "MMC1 serial reset preserves mirroring aliases");
    }
    auto rom = program.image(mapper, flags);
    require(machine.load(rom.data(), rom.size()) == nes::core::LoadResult::Ok, "original mirroring iNES image loads");
    verify(machine, program, name);
    machine.reset(); // После сброса картриджа прежняя выбранная страница не сохраняется.
    verify(machine, program, name);
    std::printf("Mirroring passed: %s, %zu buffered PPU probes per reset\n", name, program.expected.size());
}
} // namespace

/**
 * @brief Проверяет NROM horizontal/vertical/four-screen, AxROM, четыре режима MMC1 и MMC3 aliases.
 * @details Одна Machine повторно загружает собственные образы; это дополнительно проверяет отсутствие stale mapping после load/reset.
 */
int main() {
    nes::core::Machine machine; // Повторный владелец проверяет инвалидацию при load/reset.
    run(machine, "NROM four-screen", 0, 9, Wiring::FourScreen, {});
    run(machine, "NROM horizontal", 0, 0, Wiring::PpuA11, {});
    run(machine, "NROM vertical", 0, 1, Wiring::PpuA10, {});
    run(machine, "AxROM both single screens", 7, 0, Wiring::Ground, {1, 0, 1});
    run(machine, "MMC1 four modes and serial commit", 1, 1, Wiring::Ground, {1, 2, 3, 0, 2, 1});
    run(machine, "MMC3 vertical and horizontal", 4, 0, Wiring::PpuA11, {0, 1, 0xFE, 0xFF});
    run(machine, "MMC3 header vertical", 4, 1, Wiring::PpuA10, {1, 0});
    run(machine, "MMC3 four-screen ignores A000", 4, 8, Wiring::FourScreen, {0, 1, 0xFE, 0xFF});
    std::printf("Machine mirroring tests passed: %u checks\n", checks);
}
