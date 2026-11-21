#include "Machine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using namespace nes::core;

/**
 * @brief Сообщает первое расхождение PCM/CPU с region и номером гостевого кадра.
 * @details Немедленно завершает fixture кодом1; не исправляет число samples или данные PCM.
 */
[[noreturn]] void fail(const char* reason, Region region, unsigned frame) {
    std::fprintf(stderr, "FAIL frame/PCM ownership: %s region=%s frame=%u\n",
                 reason, region == Region::Pal ? "PAL" : "NTSC", frame);
    std::exit(1);
}

/** @brief Создаёт собственный iNES для чувствительных sample-фаз на границах кадров. */
std::vector<std::uint8_t> originalAudioRom() {
    // Собственный iNES с удерживаемым pulse и трёхтактным циклом позволяет
    // проверять разные sample-фазы хвоста инструкции на границе кадра.
    std::vector<std::uint8_t> rom(16 + 32768 + 8192, 0);
    rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1A;
    rom[4] = 2; rom[5] = 1;
    const std::uint8_t program[]{
        0xA9, 0x0F, 0x8D, 0x15, 0x40, // Включаются length-счётчики каналов.
        0xA9, 0xBF, 0x8D, 0x00, 0x40, // Фиксированная громкость pulse при остановленном length.
        0xA9, 0x40, 0x8D, 0x02, 0x40,
        0xA9, 0xF8, 0x8D, 0x03, 0x40,
        0xA9, 0xC0, 0x8D, 0x17, 0x40, // Пятишаговый sequencer с запрещённым IRQ.
        0x4C, 0x19, 0x80,
    };
    std::memcpy(rom.data() + 16, program, sizeof(program));
    rom[16 + 0x7FFC] = 0x00; rom[16 + 0x7FFD] = 0x80;
    return rom;
}

/** @brief Сравнивает полный PCM при немедленном и отложенном чтении кадрового результата. */
void consecutiveFrames(Region region, const std::vector<std::uint8_t>& rom) {
    auto observed = std::make_unique<Machine>();
    auto unobserved = std::make_unique<Machine>();
    if (observed->load(rom.data(), rom.size(), region) != LoadResult::Ok ||
        unobserved->load(rom.data(), rom.size(), region) != LoadResult::Ok)
        fail("synthetic ROM load", region, 0);

    bool nonzeroAudio = false;
    constexpr unsigned frameCount = 1110; // Включая чувствительную PAL-границу кадра999.
    for (unsigned frame = 1; frame <= frameCount; ++frame) {
        if (observed->stepFrame() != RunStatus::FrameReady ||
            unobserved->stepFrame() != RunStatus::FrameReady)
            fail("frame did not complete", region, frame);

        // Эта Machine всегда принимает полный PCM предыдущего кадра до следующего
        // stepFrame; у независимой Machine getters не вызываются37 кадров подряд.
        const auto expectedCount = observed->sampleCount();
        if (!expectedCount || expectedCount > Apu::sampleCapacity)
            fail("reference PCM count", region, frame);
        const auto* expected = observed->samples();
        for (std::size_t i = 0; i < expectedCount; ++i)
            nonzeroAudio = nonzeroAudio || expected[i] != 0;

        if (frame % 37 != 0) continue;
        const auto actualCount = unobserved->sampleCount();
        if (actualCount != expectedCount) {
            std::fprintf(stderr, "PCM samples observed=%zu unobserved=%zu\n", expectedCount, actualCount);
            fail("old-frame samples leaked into the new frame", region, frame);
        }
        if (std::memcmp(expected, unobserved->samples(), expectedCount * sizeof(std::int16_t)))
            fail("current-frame PCM bytes depend on previous getters", region, frame);
        const auto& a = observed->cpuState();
        const auto& b = unobserved->cpuState();
        if (a.pc != b.pc || a.a != b.a || a.x != b.x || a.y != b.y ||
            a.sp != b.sp || a.p != b.p || a.cycles != b.cycles || a.jammed != b.jammed ||
            observed->cpuCycles() != unobserved->cpuCycles() || observed->frames() != unobserved->frames())
            fail("CPU/frame state depends on previous getters", region, frame);
    }
    if (!nonzeroAudio) fail("fixture produced only silence", region, frameCount);
}
} // namespace

/**
 * @brief Сравнивает2220 кадров NTSC/PAL у Machine с чтением PCM каждый кадр и раз в37 кадров.
 * @details Проверяются каждый PCM-байт, sample count, CPU/frame state и ненулевой звук собственной pulse-программы.
 */
int main() {
    const auto rom = originalAudioRom();
    consecutiveFrames(nes::core::Region::Ntsc, rom);
    consecutiveFrames(nes::core::Region::Pal, rom);
    std::puts("Frame/PCM ownership passed:2220 NTSC/PAL frames, getters every frame versus every37.");
}
