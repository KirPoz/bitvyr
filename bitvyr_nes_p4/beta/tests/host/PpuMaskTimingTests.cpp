/** @file PpuMaskTimingTests.cpp @brief Проверяет фазу PPUMASK через публичные MMIO и PPUDATA. */
#include "Machine.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
unsigned checks = 0;

/**
 * @brief Останавливает fixture при нарушении наблюдаемого контракта PPU.
 * @param condition Ожидаемое условие текущей проверки.
 * @param message Диагностическое имя нарушенного контракта.
 */
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "PPUMASK FAIL %s\n", message);
        std::exit(1);
    }
}

#if !defined(NES_CORE_DIRECT_BUS) || !NES_CORE_DIRECT_BUS
/** @brief Независимая адресуемая память для полного generic PPU. */
struct VideoBus final : nes::core::PpuBus {
    std::array<std::uint8_t, 0x4000> memory{};
    /** @brief Возвращает байт точного адреса, выбранного PPU. */
    std::uint8_t ppuRead(std::uint16_t address) noexcept override { return memory[address & 0x3FFF]; }
    /** @brief Сохраняет маркер по адресу MMIO-доступа PPU. */
    void ppuWrite(std::uint16_t address, std::uint8_t value) noexcept override { memory[address & 0x3FFF] = value; }
};
#endif

/**
 * @brief Устанавливает v обычной парой записей PPUADDR без дополнительных tick.
 * @param ppu Заимствованный единолично исполняемый PPU текущего случая.
 * @param address Гостевой адрес PPU, младшие15 бит.
 */
template<class Chip>
void setAddress(Chip& ppu, unsigned address) {
    ppu.cpuWrite(6, static_cast<std::uint8_t>(address >> 8));
    ppu.cpuWrite(6, static_cast<std::uint8_t>(address));
}

/**
 * @brief Проверяет один переход rendering на границе вертикального инкремента256.
 * @details Разные маркеры CHR по0240/1241 различают пропущенный Y и реальный Y/X.
 * Первый PPUDATA загружает буфер, второй возвращает маркер первого адреса.
 * @param ppu Заимствованный PPU, сбрасываемый перед независимым случаем.
 * @param pal Выбор PAL, false задаёт NTSC.
 * @param grouped Продвижение интервала через tickDots вместо отдельных tick.
 * @param mask Разрешаемые BG/sprite биты PPUMASK.
 * @param disabling Переход из работающего rendering в forced blank.
 * @param writeDot Следующая точка строки14 в момент записи PPUMASK.
 * @param ticks Число PPU-точек между записью и наблюдением PPUDATA.
 * @param expected Маркер, ожидаемый по первому фактическому адресу PPUDATA.
 */
template<class Chip>
void boundary(Chip& ppu, bool pal, bool grouped, unsigned mask, bool disabling,
              unsigned writeDot, unsigned ticks, std::uint8_t expected) {
    ppu.reset(pal);
    setAddress(ppu, 0x0240); ppu.cpuWrite(7, 0xA5);
    setAddress(ppu, 0x1241); ppu.cpuWrite(7, 0x5A);
    if (disabling) ppu.cpuWrite(1, static_cast<std::uint8_t>(mask));
    unsigned guard = 0;
    while ((ppu.scanline() != 14 || ppu.dot() != writeDot) && guard < 20000) {
        ppu.tick();
        ++guard;
    }
    require(guard < 20000, "raster reaches requested next dot");
    setAddress(ppu, 0x0240);
    ppu.cpuWrite(1, disabling ? 0 : static_cast<std::uint8_t>(mask));
    if (grouped) ppu.tickDots(ticks);
    else for (unsigned tick = 0; tick < ticks; ++tick) ppu.tick();
    require(ppu.dot() == writeDot + ticks, "exact scalar/grouped interval");
    (void)ppu.cpuRead(7);
    const auto actual = ppu.cpuRead(7);
    if (actual != expected) {
        std::fprintf(stderr, "pal=%u grouped=%u mask=%u disabling=%u next_dot=%u ticks=%u actual=%u expected=%u\n",
                     unsigned(pal), unsigned(grouped), mask, unsigned(disabling), writeDot, ticks,
                     unsigned(actual), unsigned(expected));
    }
    require(actual == expected, "PPUDATA observes correct side of Y256");
}

/**
 * @brief Проверяет включение/выключение вокруг Y256 для обеих областей и способов тактирования.
 * @param ppu Заимствованный PPU, сбрасываемый отдельно для каждого случая.
 */
template<class Chip>
void renderingTransitions(Chip& ppu) {
    for (const bool pal : {false, true}) {
        for (const bool grouped : {false, true}) {
            for (const unsigned mask : {8U, 16U, 24U}) {
                // Три tick с254 применяют разрешение на256 и захватывают Y/X.
                boundary(ppu, pal, grouped, mask, false, 254, 3, 0x5A);
                // Два tick с255 сохраняют forced blank до завершения256.
                boundary(ppu, pal, grouped, mask, false, 255, 2, 0xA5);
                // Выключение имеет ту же фазу, поэтому ожидаемые стороны меняются.
                boundary(ppu, pal, grouped, mask, true, 254, 3, 0xA5);
                boundary(ppu, pal, grouped, mask, true, 255, 2, 0x5A);
            }
        }
    }
}
}

/** @brief Создаёт собственную CHR-RAM fixture и проверяет доступные в конфигурации PPU. */
int main() {
    std::vector<std::uint8_t> rom(16 + 16384, 0);
    rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1A; rom[4] = 1;
    rom[16] = 0x4C; rom[17] = 0; rom[18] = 0xC0;
    rom[16 + 0x3FFC] = 0; rom[16 + 0x3FFD] = 0xC0;
    auto machine = std::make_unique<nes::core::Machine>();
    require(machine->load(rom.data(), rom.size()) == nes::core::LoadResult::Ok, "own CHR-RAM fixture loads");
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    auto full = std::make_unique<nes::core::Ppu>(*machine);
#else
    VideoBus bus;
    auto full = std::make_unique<nes::core::Ppu>(bus);
#endif
    renderingTransitions(*full);
#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
    auto timing = std::make_unique<nes::core::TimingPpu>(*machine);
    renderingTransitions(*timing);
#endif
    std::printf("PPUMASK timing passed %u checks\n", checks);
}
