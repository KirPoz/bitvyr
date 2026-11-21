#include "ppu/Ppu.h"
#include "apu/Apu.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
unsigned checks = 0;
/** @brief Останавливает fixture при нарушении независимого ожидаемого условия. */
void require(bool condition, const char* what) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

/**
 * @brief Собственные16KiB PPU-памяти и независимый наблюдатель dots/A12.
 * @details ppuCycle считает реальное время, а ppuAddress принимает A12 только после восьми низких dots. Счётчики не управляют самой моделью PPU.
 */
struct VideoBus final : nes::core::PpuBus {
    std::array<std::uint8_t, 0x4000> memory{};
    std::uint64_t dots = 0;
    unsigned lowDots = 0, filteredEdges = 0;
    bool a12 = false;
    /** @brief Читает memory[address&$3FFF] без изменения dots/A12; timing отдельно учитывает ppuCycle. */
    std::uint8_t ppuRead(std::uint16_t address) noexcept override { return memory[address & 0x3FFF]; }
    /** @brief Записывает байт независимой тестовой PPU-шины на исходной границе события. */
    void ppuWrite(std::uint16_t address, std::uint8_t value) noexcept override { memory[address & 0x3FFF] = value; }
    /** @brief Фиксирует реальный номер PPU-такта для сравнения порядка событий. */
    void ppuCycle() noexcept override { ++dots; if (!a12 && lowDots < 1000) ++lowDots; }
    /** @brief Фиксирует адресный фронт PPU отдельно от чтения данных. */
    void ppuAddress(std::uint16_t address) noexcept override {
        const bool next = (address & 0x1000) != 0;
        if (!a12 && next && lowDots >= 8) ++filteredEdges;
        if (next) lowDots = 0;
        a12 = next;
    }
};

/**
 * @brief DMC-шина с журналом запрошенных адресов и суммой украденных CPU-тактов.
 * @details Возвращает фиксированный$AA; это oracle routing/sample timing, а не ROM картриджа.
 */
struct AudioBus final : nes::core::ApuBus {
    unsigned stalls = 0;
    std::vector<std::uint16_t> reads;
    /** @brief Записывает DMC-адрес в журнал и возвращает постоянный$AA для независимой timing-проверки. */
    std::uint8_t dmcRead(std::uint16_t address) noexcept override {
        reads.push_back(address);
        return 0xAA;
    }
    /** @brief Учитывает украденные DMC-такты без рекурсивного продвижения APU. */
    void dmcStall(unsigned cycles) noexcept override { stalls += cycles; }
};

/** @brief Передаёт обе части адреса через регистр PPUADDR. */
void setAddress(nes::core::Ppu& ppu, unsigned address) {
    ppu.cpuWrite(6, static_cast<std::uint8_t>(address >> 8));
    ppu.cpuWrite(6, static_cast<std::uint8_t>(address));
}
/** @brief Записывает палитру через настоящий адресный/данный интерфейс PPU. */
void palette(nes::core::Ppu& ppu, unsigned offset, std::uint8_t value) {
    setAddress(ppu, 0x3F00 + offset);
    ppu.cpuWrite(7, value);
}
/** @brief Продвигает тест до следующей реальной границы PPU-кадра. */
void waitFrame(nes::core::Ppu& ppu) {
    ppu.clearFrameReady();
    for (unsigned i = 0; i < 107000; ++i) {
        ppu.tick();
        if (ppu.frameReady()) return;
    }
    require(false, "PPU must produce a frame within one PAL frame");
}

/** @brief Проверяет PPUDATA/status/scroll/address/OAM и их побочные эффекты. */
void ppuRegisters() {
    VideoBus bus;
    nes::core::Ppu ppu(bus);
    palette(ppu, 0, 0x25);
    setAddress(ppu, 0x3F10);
    require((ppu.cpuRead(7) & 0x3F) == 0x25, "universal sprite/background palette mirrors");
    palette(ppu, 0x14, 0x61);
    setAddress(ppu, 0x3F04);
    require((ppu.cpuRead(7) & 0x3F) == 0x21, "palette stores six bits and mirrors 3F14");
    bus.memory[0x2000] = 0x77;
    bus.memory[0x2001] = 0x55;
    setAddress(ppu, 0x2000);
    (void)ppu.cpuRead(7);
    require(ppu.cpuRead(7) == 0x77, "PPUDATA read buffer delays nametable reads");
    require(ppu.cpuRead(7) == 0x55, "PPUDATA increments address");
    ppu.cpuWrite(0, 4);
    setAddress(ppu, 0x2100);
    ppu.cpuWrite(7, 0x12);
    ppu.cpuWrite(7, 0x34);
    require(bus.memory[0x2100] == 0x12 && bus.memory[0x2120] == 0x34, "PPUCTRL selects 32-byte VRAM increment");
    ppu.cpuWrite(3, 0xFE);
    ppu.dmaWrite(0xFF);
    ppu.dmaWrite(0x12);
    ppu.dmaWrite(0x34);
    ppu.cpuWrite(3, 0xFE);
    require(ppu.cpuRead(4) == 0xE3, "OAM attribute unused bits are clear");
    ppu.cpuWrite(3, 0xFF);
    require(ppu.cpuRead(4) == 0x12, "OAM DMA writes last byte");
    ppu.cpuWrite(3, 0);
    require(ppu.cpuRead(4) == 0x34, "OAM DMA wraps at 256 bytes");
    ppu.reset();
    ppu.cpuWrite(0, 0x80);
    waitFrame(ppu);
    ppu.tick(); ppu.tick();
    require(ppu.takeNmi(), "vblank generates enabled NMI edge");
    require(!ppu.takeNmi(), "NMI is consumed once");
    require((ppu.cpuRead(2) & 0x80) != 0, "PPUSTATUS exposes vblank");
    require((ppu.cpuRead(2) & 0x80) == 0, "PPUSTATUS read clears vblank");
    ppu.cpuWrite(0, 0);
    waitFrame(ppu);
    ppu.cpuWrite(0, 0x80);
    ppu.tick(); ppu.tick();
    require(ppu.takeNmi(), "enabling NMI during vblank generates an edge");
}

/** @brief Проверяет кадровые границы, рендеринг и NMI на реальных PPU-тактах. */
void ppuTimingAndRendering() {
    VideoBus bus;
    nes::core::Ppu ppu(bus);
    for (unsigned row = 0; row < 8; ++row) { bus.memory[row] = 0xFF; bus.memory[16 + row] = 0xFF; }
    palette(ppu, 0, 0x0F);
    palette(ppu, 1, 0x21);
    palette(ppu, 0x11, 0x31);
    ppu.cpuWrite(3, 0);
    ppu.dmaWrite(20); ppu.dmaWrite(1); ppu.dmaWrite(0); ppu.dmaWrite(16);
    ppu.cpuWrite(0, 0);
    ppu.cpuWrite(5, 0); ppu.cpuWrite(5, 0);
    ppu.cpuWrite(1, 0x1E);
    waitFrame(ppu);
    const auto* pixels = ppu.framebuffer();
    require(pixels[0] == 0x21 && pixels[255] == 0x21, "background pipeline covers first visible scanline");
    require(pixels[20 * 256 + 16] == 0x21, "sprite Y coordinate has one-line offset");
    require(pixels[21 * 256 + 16] == 0x31 && pixels[21 * 256 + 23] == 0x31, "sprite pattern covers eight pixels");
    require(pixels[21 * 256 + 24] == 0x21, "sprite shift register expires after eight pixels");
    require((ppu.cpuRead(2) & 0x40) != 0, "opaque sprite zero overlapping background sets hit");
    const auto start = bus.dots;
    waitFrame(ppu);
    const auto middle = bus.dots;
    waitFrame(ppu);
    const auto end = bus.dots;
    require((middle - start == 89341 && end - middle == 89342) || (middle - start == 89342 && end - middle == 89341), "NTSC skips exactly one rendered dot on alternate frames");
    ppu.reset(true);
    ppu.cpuWrite(1, 8);
    waitFrame(ppu);
    const auto palStart = bus.dots;
    waitFrame(ppu);
    require(bus.dots - palStart == 106392, "PAL has 312 scanlines and no odd-frame skip");

    ppu.reset();
    ppu.cpuWrite(3, 0);
    for (unsigned sprite = 0; sprite < 9; ++sprite) {
        ppu.dmaWrite(20); ppu.dmaWrite(1); ppu.dmaWrite(0); ppu.dmaWrite(static_cast<std::uint8_t>(sprite * 8));
    }
    ppu.cpuWrite(1, 0x18);
    waitFrame(ppu);
    require((ppu.cpuRead(2) & 0x20) != 0, "ninth in-range sprite sets overflow");

    ppu.reset();
    bus.filteredEdges = 0;
    bus.lowDots = 0;
    bus.a12 = false;
    ppu.cpuWrite(0, 8);
    ppu.cpuWrite(1, 0x18);
    waitFrame(ppu);
    require(bus.filteredEdges == 241, "8-dot A12 filter sees one MMC3 edge on each rendering scanline");
    ppu.reset(); bus.filteredEdges = 0; bus.lowDots = 0; bus.a12 = false;
    ppu.cpuWrite(0, 0x10); ppu.cpuWrite(1, 0x18);
    waitFrame(ppu);
    require(bus.filteredEdges == 241, "MMC3 reversed pattern tables do not double-clock at scanline boundary");
    ppu.reset(); bus.filteredEdges = 0; bus.lowDots = 0; bus.a12 = false;
    ppu.cpuWrite(0, 0x18); ppu.cpuWrite(1, 0x18);
    waitFrame(ppu);
    require(bus.filteredEdges == 0, "MMC3 filter rejects short nametable gaps with both pattern tables high");
}

/** @brief Продвигает заданное число тактов тестовой APU, сохраняя независимую шину. */
void clock(nes::core::Apu& apu, unsigned cycles) {
    for (unsigned i = 0; i < cycles; ++i) {
        apu.tick();
        if (apu.sampleCount() == nes::core::Apu::sampleCapacity) apu.clearSamples();
    }
}

/** @brief Проверяет frame sequencer, length/envelope и IRQ по точным тактам. */
void apuRegistersAndTiming() {
    AudioBus bus;
    nes::core::Apu apu(bus);
    clock(apu, 10000);
    require(apu.readStatus() == 0, "APU reset disables all length counters");
    for (std::size_t i = 0; i < apu.sampleCount(); ++i) require(apu.samples()[i] == 0, "APU reset produces silent PCM");
    apu.reset();
    apu.write(0x4015, 15);
    apu.write(0x4000, 0x1F); apu.write(0x4004, 0x1F);
    apu.write(0x4008, 1); apu.write(0x400C, 0x1F);
    apu.write(0x4003, 0); apu.write(0x4007, 0); apu.write(0x400B, 0); apu.write(0x400F, 0);
    require((apu.readStatus() & 15) == 15, "enabled channel length registers load");
    clock(apu, 14912);
    require(apu.debug().length[0] == 10, "length counters wait for half-frame clock");
    clock(apu, 1);
    const auto state = apu.debug();
    require(state.length[0] == 9 && state.length[1] == 9 && state.length[2] == 9 && state.length[3] == 9, "half frame clocks all four length counters");
    clock(apu, 14915);
    require(apu.irqLine() && (apu.readStatus() & 0x40), "four-step frame IRQ begins at cycle 29828");
    require(!apu.irqLine(), "status read acknowledges frame IRQ");
    clock(apu, 1);
    require(apu.irqLine(), "frame IRQ reasserts on following frame terminal cycle");
    apu.write(0x4017, 0x40);
    require(!apu.irqLine(), "IRQ inhibit clears frame IRQ immediately");
    apu.write(0x4015, 0);
    require((apu.readStatus() & 15) == 0, "disable writes clear length counters");

    apu.reset();
    apu.write(0x4015, 3);
    for (const unsigned base : {0x4000U, 0x4004U}) {
        apu.write(base, 0x3F); apu.write(base + 1, 0x89); apu.write(base + 2, 100); apu.write(base + 3, 0);
    }
    clock(apu, 14913);
    require(apu.debug().pulsePeriod[0] == 49 && apu.debug().pulsePeriod[1] == 50, "pulse sweeps use different negate carry wiring");
    apu.reset();
    apu.write(0x4015, 1); apu.write(0x4003, 0);
    apu.write(0x4017, 0xC0);
    clock(apu, 3);
    require(apu.debug().length[0] == 10, "frame counter reset is delayed after even-cycle write");
    clock(apu, 1);
    require(apu.debug().length[0] == 9, "five-step reset clocks half frame after four CPU cycles");
    clock(apu, 100000);
    require(!apu.irqLine(), "five-step sequence never raises frame IRQ");
}

/** @brief Проверяет DMC-запросы/остановки и количество выходных PCM-отсчётов. */
void dmcAndSampleRate() {
    AudioBus bus;
    nes::core::Apu apu(bus);
    apu.write(0x4010, 0x8F); apu.write(0x4012, 0xFF); apu.write(0x4013, 0);
    apu.write(0x4015, 0x10);
    apu.tick();
    require(bus.reads.size() == 1 && bus.reads[0] == 0xFFC0 && bus.stalls == 4, "DMC fetch maps start address and requests CPU stalls");
    require((apu.readStatus() & 0x90) == 0x80, "DMC IRQ signals completion of final byte fetch");
    require(apu.irqLine(), "status read preserves DMC IRQ");
    apu.write(0x4015, 0);
    require(!apu.irqLine(), "4015 acknowledges DMC IRQ");
    apu.reset(); bus.reads.clear(); bus.stalls = 0;
    apu.write(0x4010, 0xCF); apu.write(0x4012, 0xFF); apu.write(0x4013, 4);
    apu.write(0x4015, 0x10);
    clock(apu, 30000);
    require(bus.reads.size() >= 65 && bus.reads[63] == 0xFFFF && bus.reads[64] == 0x8000, "DMC sample fetch wraps FFFF to 8000");
    require(!apu.debug().dmcIrq, "DMC looping suppresses sample-completion IRQ");
    apu.reset();
    clock(apu, 1789773);
    require(apu.totalSamples() == 48000 && apu.droppedSamples() == 0, "NTSC resampler produces exact 48kHz without dropped samples");
    apu.reset(true);
    clock(apu, 1662607);
    require(apu.totalSamples() == 48000 && apu.droppedSamples() == 0, "PAL resampler produces exact 48kHz without dropped samples");
}

/** @brief Создаёт отдельную треугольную волну для проверки периода и спектра. */
std::vector<std::int16_t> triangleWave(nes::core::Apu& apu, unsigned period) {
    apu.reset();
    apu.write(0x4015, 4); apu.write(0x4008, 0xFF);
    apu.write(0x400A, static_cast<std::uint8_t>(period));
    apu.write(0x400B, static_cast<std::uint8_t>(period >> 8));
    apu.write(0x4017, 0xC0);
    std::vector<std::int16_t> result;
    for (unsigned i = 0; i < 178977; ++i) {
        apu.tick();
        if (apu.sampleCount() >= 1000) {
            result.insert(result.end(), apu.samples(), apu.samples() + apu.sampleCount());
            apu.clearSamples();
        }
    }
    result.insert(result.end(), apu.samples(), apu.samples() + apu.sampleCount());
    return result;
}
/** @brief Вычисляет среднеквадратичный уровень готового тестового PCM. */
double rms(const std::vector<std::int16_t>& samples) {
    double sum = 0.0;
    for (std::size_t i = 1000; i < samples.size(); ++i) sum += static_cast<double>(samples[i]) * samples[i];
    return std::sqrt(sum / (samples.size() - 1000));
}
/** @brief Проверяет частоту формы волны и DC/AC свойства независимого PCM. */
void audioSpectrum() {
    AudioBus bus;
    nes::core::Apu apu(bus);
    const auto audible = triangleWave(apu, 127);
    const auto ultrasonic = triangleWave(apu, 0);
    const double audibleRms = rms(audible), ultrasonicRms = rms(ultrasonic);
    require(audibleRms > 100.0, "triangle generates audible non-silent PCM");
    require(ultrasonicRms < audibleRms * 0.15, "bandlimited resampling suppresses ultrasonic triangle aliasing");
    // Фильтры и нелинейный DAC создают лишние переходы через ноль. Определяется
    // период всей формы волны, а не частота каждого перехода.
    unsigned bestLag = 0;
    double bestCorrelation = -1.0e30;
    for (unsigned lag = 80; lag <= 140; ++lag) {
        double correlation = 0.0;
        for (std::size_t i = 1000 + lag; i < audible.size(); ++i)
            correlation += static_cast<double>(audible[i]) * audible[i - lag];
        correlation /= audible.size() - 1000 - lag;
        if (correlation > bestCorrelation) { bestCorrelation = correlation; bestLag = lag; }
    }
    const double frequency = 48000.0 / bestLag;
    std::printf("Audio check: triangle %.1fHz, RMS %.1f, ultrasonic RMS %.2f\n", frequency, audibleRms, ultrasonicRms);
    require(frequency > 420.0 && frequency < 455.0, "triangle timer produces expected approximately 437Hz pitch");
}
}  // namespace

/**
 * @brief Проверяет PPU registers/frame/NMI, APU frame IRQ, DMC stalls/sample rate и форму triangle PCM.
 * @details Память VideoBus и ответы AudioBus независимы от проверяемых моделей; спектральные проверки не заменяют физический I2S-тест.
 */
int main() {
    ppuRegisters();
    ppuTimingAndRendering();
    apuRegistersAndTiming();
    dmcAndSampleRate();
    audioSpectrum();
    std::printf("PPU/APU tests passed: %u checks\n", checks);
}
