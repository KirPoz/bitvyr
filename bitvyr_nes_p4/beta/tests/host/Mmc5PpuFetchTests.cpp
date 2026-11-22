/**
 * @file Mmc5PpuFetchTests.cpp
 * @brief Проверяет generic legacy-PPU fetch-hook, его фазу/колонку и совместимость default bus. Не является эталоном MMC5.
 */
#include "ppu/Ppu.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
#error "Mmc5PpuFetchTests uses the portable virtual PpuBus; actual Machine integration is a separate test"
#endif

namespace {
using nes::core::Ppu;
using nes::core::PpuBus;
using nes::core::PpuFetchKind;
unsigned cases = 0;
std::uint64_t checks = 0;

/** @brief Прерывает HOST-тест при нарушении независимо заданного фазового контракта. */
void require(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "MMC5_PPU_FETCH_FAIL case=%u checks=%llu message=%s\n",
                     cases, static_cast<unsigned long long>(checks), message);
        std::exit(1);
    }
}

/** @brief Один наблюдённый старый bus callback; без fetch-метаданных. */
struct Wire {
    unsigned kind = 0;
    int line = 0;
    unsigned dot = 0, address = 0, value = 0;
    /** @brief Сравнивает полный порядок, момент, адрес и значение старого callback. */
    bool same(const Wire& other) const noexcept {
        return kind == other.kind && line == other.line && dot == other.dot &&
               address == other.address && value == other.value;
    }
};

/** @brief Старый bus без override нового hook; ограниченный trace принадлежит тесту. */
struct LegacyBus : PpuBus {
    const Ppu* ppu = nullptr;
    std::array<std::uint8_t, 0x4000> memory{};
    std::vector<Wire> wires;
    /** @brief Резервирует фиксированный верхний предел trace до начала PPU-шага. */
    LegacyBus() { wires.reserve(20000); }
    /** @brief Записывает callback, сохраняя явный предел памяти теста. */
    void record(unsigned kind, unsigned address = 0, unsigned value = 0) noexcept {
        require(wires.size() < 20000, "wire trace bound");
        wires.push_back({kind, ppu ? ppu->scanline() : -2, ppu ? ppu->dot() : 0U, address, value});
    }
    /** @brief Читает ровно один байт и фиксирует старый read callback. */
    std::uint8_t ppuRead(std::uint16_t address) noexcept override {
        const auto value = memory[address & 0x3FFFU];
        record(2, address, value);
        return value;
    }
    /** @brief Изменяет только память тестовой шины и фиксирует старый write callback. */
    void ppuWrite(std::uint16_t address, std::uint8_t value) noexcept override {
        memory[address & 0x3FFFU] = value;
        record(3, address, value);
    }
    /** @brief Сохраняет отдельный адресный drive без добавления чтения. */
    void ppuAddress(std::uint16_t address) noexcept override { record(1, address); }
    /** @brief Сохраняет отдельную PPU-точку без изменения адреса или данных. */
    void ppuCycle() noexcept override { record(0); }
};

/** @brief Метаданные одного фактически вызванного fetch; физический read отдельно остаётся в wires. */
struct Fetch {
    PpuFetchKind kind = PpuFetchKind::Cpu;
    int line = 0;
    unsigned dot = 0, address = 0, tileX = 0;
};

/** @brief Обогащённый bus: либо exact default delegation, либо независимые фазовые данные для проверки использования результата. */
struct FetchBus final : LegacyBus {
    std::vector<Fetch> fetches;
    bool phaseValues = false;
    /** @brief Резервирует bounded список fetch до вызовов PPU. */
    FetchBus() { fetches.reserve(2000); }
    /** @brief Наблюдает фазу и возвращает байт; тестовая phaseValues не моделирует регистры или IRQ MMC5. */
    std::uint8_t ppuReadFetch(std::uint16_t address, PpuFetchKind kind, unsigned tileX) noexcept override {
        require(fetches.size() < 2000, "fetch trace bound");
        fetches.push_back({kind, ppu->scanline(), ppu->dot(), address, tileX});
        auto value = LegacyBus::ppuRead(address);
        if (phaseValues && kind == PpuFetchKind::BackgroundAttribute) value = 0xFF;
        if (phaseValues && kind == PpuFetchKind::BackgroundPattern) value = (address & 8U) ? 0 : 0xFF;
        wires.back().value = value;
        return value;
    }
};

/** @brief Проверяет все 170 reads полной активной строки по независимому расписанию PPU. */
void checkLine(const FetchBus& bus, int line) {
    std::array<const Fetch*, 341> byDot{};
    unsigned reads = 0;
    for (const auto& f : bus.fetches) if (f.line == line) {
        require(f.dot < byDot.size() && byDot[f.dot] == nullptr, "one fetch at a read dot");
        byDot[f.dot] = &f;
        ++reads;
    }
    require(reads == 170, "complete line read count incl empty sprites and tail dummy");
    for (unsigned tile = 0; tile < 34; ++tile) {
        const unsigned start = tile < 32 ? 1U + tile * 8U : 321U + (tile - 32U) * 8U;
        const unsigned x = tile < 32 ? tile + 2U : tile - 32U;
        for (unsigned phase = 0; phase < 4; ++phase) {
            const auto* f = byDot[start + 1U + 2U * phase];
            require(f != nullptr, "all four BG reads");
            const auto kind = phase == 0 ? PpuFetchKind::BackgroundName :
                              phase == 1 ? PpuFetchKind::BackgroundAttribute : PpuFetchKind::BackgroundPattern;
            require(f->kind == kind && f->tileX == x, "BG kind and future tile column");
            require(phase < 2 ? f->address >= 0x2000U && f->address < 0x3000U : f->address < 0x2000U,
                    "BG name/attribute vs pattern address");
        }
    }
    for (unsigned slot = 0; slot < 8; ++slot) for (unsigned phase = 0; phase < 4; ++phase) {
        const auto* f = byDot[258U + slot * 8U + phase * 2U];
        require(f != nullptr && f->tileX == 0, "all four reads in every sprite slot");
        require(f->kind == (phase < 2 ? PpuFetchKind::DummyName : PpuFetchKind::SpritePattern),
                "sprite garbage is not a BG tile or attribute");
    }
    for (const unsigned dot : {338U, 340U}) {
        const auto* f = byDot[dot];
        require(f && f->kind == PpuFetchKind::DummyName && f->tileX == 0, "end-line dummy reads");
    }
}

/** @brief Сравнивает новый metadata-hook со старым bus при неизменных данных, NTSC/PAL и разных размерах продвижения. */
void delegation(bool pal, unsigned group) {
    ++cases;
    LegacyBus oldBus;
    FetchBus fetchBus;
    for (unsigned i = 0; i < oldBus.memory.size(); ++i)
        oldBus.memory[i] = fetchBus.memory[i] = static_cast<std::uint8_t>(i * 73U + (i >> 5U));
    Ppu oldPpu(oldBus), fetchPpu(fetchBus);
    oldBus.ppu = &oldPpu; fetchBus.ppu = &fetchPpu;
    oldPpu.reset(pal); fetchPpu.reset(pal);
    oldPpu.cpuWrite(1, 0x18); fetchPpu.cpuWrite(1, 0x18);
    for (unsigned done = 0; done < 3U * 341U;) {
        const unsigned n = group < 3U * 341U - done ? group : 3U * 341U - done;
        for (unsigned i = 0; i < n; ++i) oldPpu.tick();
        fetchPpu.tickDots(n);
        done += n;
    }
    require(oldBus.wires.size() == fetchBus.wires.size(), "default delegate callback count");
    for (std::size_t i = 0; i < oldBus.wires.size(); ++i)
        require(oldBus.wires[i].same(fetchBus.wires[i]), "default delegate ordered cycle/address/read/write");
    require(std::memcmp(oldPpu.framebuffer(), fetchPpu.framebuffer(), Ppu::width * Ppu::height) == 0,
            "default delegate every indexed pixel");
    require(oldPpu.cpuRead(2) == fetchPpu.cpuRead(2), "default delegate STATUS effects");
    checkLine(fetchBus, 0); checkLine(fetchBus, 1);
}

/** @brief Устанавливает адрес PPUDATA через реальный публичный регистровый путь. */
void address(Ppu& ppu, unsigned value) {
    ppu.cpuWrite(6, static_cast<std::uint8_t>(value >> 8U));
    ppu.cpuWrite(6, static_cast<std::uint8_t>(value));
}

/** @brief Проверяет, что CPU2007 не выдаёт себя за BG fetch, включая внешнее buffer-refill при чтении палитры. */
void cpuContext() {
    ++cases;
    FetchBus bus; Ppu ppu(bus); bus.ppu = &ppu;
    address(ppu, 0x2000); (void)ppu.cpuRead(7);
    address(ppu, 0x3F00); (void)ppu.cpuRead(7);
    unsigned reads = 0;
    for (const auto& w : bus.wires) if (w.kind == 2) {
        require(w.address == (reads ? 0x2F00U : 0x2000U), "CPU2007 actual external addresses");
        ++reads;
    }
    require(reads == 2 && bus.fetches.empty(), "CPU2007 remains the untyped CPU-context bus path");
}

/** @brief Проверяет, что PPU использует именно возвращённые hook-байты атрибута/CHR, а не повторно читает прежнюю память. */
void phaseResult() {
    ++cases;
    FetchBus bus; Ppu ppu(bus); bus.ppu = &ppu; bus.phaseValues = true;
    address(ppu, 0x3F0D); ppu.cpuWrite(7, 0x25);
    address(ppu, 0); ppu.cpuWrite(0, 0); ppu.cpuWrite(1, 0x0A);
    for (unsigned i = 0; i < 3U * 341U; ++i) ppu.tick();
    const auto* pixels = ppu.framebuffer();
    for (unsigned x = 16; x < 240; ++x) require(pixels[Ppu::width + x] == 0x25, "hook byte reaches background shifters and palette");
}
}

/** @brief Запускает только portable PPU-bus contract tests; Machine/MMC5/SDK остаются отдельными gates. */
int main() {
    for (const bool pal : {false, true}) for (const unsigned group : {1U, 3U, 8U, 24U}) delegation(pal, group);
    cpuContext(); phaseResult();
    std::printf("MMC5_PPU_FETCH_PASS cases=%u checks=%llu actual_mapper=0 machine=0 sdk=0\n",
                cases, static_cast<unsigned long long>(checks));
    return 0;
}
