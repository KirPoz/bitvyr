/**
 * @file Mmc5AudioTests.cpp
 * @brief Направленные black-box проверки MMC5 audio без ROM, SDK и устройства.
 */
#include "cartridge/Mmc5Audio.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nes::core {
/** @brief Независимая тестовая политика восьми фаз и доступ к cache-slot без clock. */
struct Mmc5AudioCacheTestAccess {
    /** @brief Вычисляет mean по восьми аппаратным duty-битам; cache/helper не использует. */
    static std::int32_t expected(const Mmc5Audio& audio) noexcept {
        constexpr unsigned waveform[4]={0x02,0x06,0x1e,0xf9};
        const auto raw=audio.levels();const unsigned level[2]={raw.pulse1,raw.pulse2};
        std::uint32_t sum=0;
        for(unsigned i=0;i<2;++i){const auto&p=audio.pulses_[i];
            if(p.period<=1U&&p.length){const unsigned volume=(p.control&0x10U)?p.control&15U:p.decay;
                for(unsigned phase=0;phase<8;++phase)if(waveform[p.control>>6U]&(1U<<phase))sum+=volume;
            }else sum+=8U*level[i];
        }
        return -static_cast<std::int32_t>((653U*sum)/8U)-296*raw.pcm;
    }
    /** @brief Возвращает только тесту slot для обратимого negative control активации. */
    static std::int32_t& slot(Mmc5Audio& audio) noexcept {return audio.cachedSampleQ16_;}
    /** @brief Подтверждает настоящий старый timer debt после записи короткого period. */
    static bool oldLongTimer(const Mmc5Audio& audio) noexcept {return audio.pulses_[0].timer>audio.pulses_[0].period;}
};
} // namespace nes::core

namespace {
using Audio = nes::core::Mmc5Audio;
unsigned checks = 0;
unsigned groups = 0;

/** @brief Останавливает HOST-тест на первом нарушенном публичном контракте. */
void require(bool value, const char* text) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "MMC5_AUDIO_FAIL group=%u check=%u %s\n", groups, checks, text);
        std::exit(1);
    }
}

/** @brief Читает только управляемые length bits; это реальный status strobe. */
unsigned status(Audio& audio) { return audio.cpuRead(0x5015, 0) & 3U; }

/** @brief Сравнивает три DAC, IRQ и будущую event-границу без consume-чтений. */
void same(const Audio& a, const Audio& b) {
    const auto x = a.levels();
    const auto y = b.levels();
    require(x.pulse1 == y.pulse1 && x.pulse2 == y.pulse2 && x.pcm == y.pcm, "DAC equality");
    require(a.sample() == b.sample(), "mixer level equality");
    require(a.irqLine() == b.irqLine(), "IRQ equality");
    require(a.nextEventCpuCycles() == b.nextEventCpuCycles(), "next edge equality");
}

/** @brief Настраивает один audible pulse, сохраняя свободу timer/envelope теста. */
void start(Audio& a, unsigned channel, std::uint8_t control, unsigned period, unsigned length = 1) {
    const auto base = static_cast<std::uint16_t>(0x5000U + 4U * channel);
    a.cpuWrite(0x5015, 3);
    a.cpuWrite(base, control);
    a.cpuWrite(static_cast<std::uint16_t>(base + 2), static_cast<std::uint8_t>(period));
    a.cpuWrite(static_cast<std::uint16_t>(base + 3),
               static_cast<std::uint8_t>((length << 3U) | (period >> 8U)));
}

/** @brief Cold/reset не смешиваются с write-mode, ревизией либо open bus. */
void resetAndDecode() {
    ++groups;
    Audio a;
    require(a.sample() == 0 && status(a) == 0 && !a.irqLine(), "defined cold state");
    require(a.nextEventCpuCycles() == 2, "cold timer half-phase");
    require(a.cpuRead(0x5010, 0xFF) == 0x7E, "letterless revision and undriven bits");
    require(a.cpuRead(0x5015, 0xFF) == 0xFC, "length status preserves undriven high bits");
    a.cpuWrite(0x5010, 1);
    require(a.cpuRead(0x5010, 0) == 0, "read bit0 is revision, not mode");
    a.reset(true);
    require(a.cpuRead(0x5010, 0x22) == 0x23, "MMC5A revision bit");
    for (const auto address : {0x5000, 0x5007, 0x5011, 0x5014, 0x5016, 0xD010}) {
        require(a.cpuRead(static_cast<std::uint16_t>(address), 0xA5) == 0xA5, "write-only/unmapped read is bus");
    }
    a.clockCpu(0);
    require(a.nextEventCpuCycles() == 2, "zero span has no effect");
}

/** @brief Enable сам не загружает length; disable немедленно очищает его. */
void enablesAndLengths() {
    ++groups;
    Audio a;
    a.cpuWrite(0x5003, 0x08);
    require(status(a) == 0, "disabled high write does not load length");
    a.cpuWrite(0x5015, 1);
    require(status(a) == 0, "enable without reload remains silent");
    a.cpuWrite(0x5003, 0x08);
    require(status(a) == 1, "enabled high write loads channel1");
    a.cpuWrite(0x5007, 0x08);
    require(status(a) == 1, "disabled channel2 independent");
    a.cpuWrite(0x5015, 3);
    a.cpuWrite(0x5007, 0x08);
    require(status(a) == 3, "both channels active");
    a.cpuWrite(0x5015, 2);
    require(status(a) == 2, "disable only clears selected length");
    a.cpuWrite(0x5015, 3);
    require(status(a) == 2, "reenable does not resurrect length");
    a.cpuWrite(0x5015, 0xFC);
    require(status(a) == 0, "unimplemented enable bits ignored");

    // Ожидаемые durations взяты из аппаратной таблицы, не из helper устройства.
    constexpr unsigned lengths[32] = {
        10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
        12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
    };
    for (unsigned index = 0; index != 32; ++index) {
        a.reset();
        start(a, 0, 0xDF, 0, index);
        a.clockCpu((lengths[index] - 1U) * Audio::framePeriodCpu);
        require(status(a) == 1, "length active one autonomous frame before expiration");
        a.clockCpu(Audio::framePeriodCpu - 1U);
        require(status(a) == 1, "length edge is not early");
        a.clockCpu();
        require(status(a) == 0, "length expires on exact autonomous frame");
    }
    a.reset();
    start(a, 0, 0xFF, 0, 3); // Length остановлен после загрузки2.
    a.clockCpu(25U * Audio::framePeriodCpu);
    require(status(a) == 1, "halt retains length");
    a.cpuWrite(0x5000, 0xDF);
    a.clockCpu(Audio::framePeriodCpu);
    require(status(a) == 1, "unhalt consumes first length clock");
    a.clockCpu(Audio::framePeriodCpu);
    require(status(a) == 0, "unhalt consumes second length clock");
}

/** @brief Timer/duty проверяются по вручную заданным waveform и CPU periods. */
void pulseTimers() {
    ++groups;
    constexpr std::uint8_t waves[4][8] = {
        {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0},
        {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1}
    };
    for (unsigned duty = 0; duty != 4; ++duty) {
        Audio a;
        start(a, 0, static_cast<std::uint8_t>((duty << 6U) | 0x3FU), 0);
        require(a.levels().pulse1 == 15U * waves[duty][0], "duty starts at phase0");
        a.clockCpu();
        require(a.levels().pulse1 == 15U * waves[duty][0], "pulse waits second CPU tick");
        a.clockCpu();
        require(a.levels().pulse1 == 15U * waves[duty][1], "timer0 is audible without native t<8 mute");
        for (unsigned phase = 2; phase != 17; ++phase) {
            a.clockCpu(2);
            require(a.levels().pulse1 == 15U * waves[duty][phase & 7U], "exact duty waveform across wraps");
        }
    }
    Audio a;
    start(a, 0, 0x3F, 7);
    a.clockCpu(2); // Исходный нулевой счётчик истекает; новая перезагрузка имеет period7.
    require(a.levels().pulse1 == 15, "initial timer event");
    a.clockCpu(15);
    require(a.levels().pulse1 == 15, "t7 period is16 CPU clocks, not8");
    a.clockCpu();
    require(a.levels().pulse1 == 0, "t7 next sequence edge");
    a.reset();
    start(a, 0, 0x3F, 2047);
    a.clockCpu(2);
    a.clockCpu(4095);
    require(a.levels().pulse1 == 15, "11-bit maximum holds phase4095 CPU clocks");
    a.clockCpu();
    require(a.levels().pulse1 == 0, "11-bit maximum period4096 CPU clocks");
}

/** @brief High перезапускает waveform/envelope, но сохраняет timer residual. */
void timerWritesAndIgnoredSweep() {
    ++groups;
    Audio a, b;
    start(a, 0, 0x3F, 9);
    start(b, 0, 0x3F, 9);
    a.clockCpu(10); b.clockCpu(10); // Начальные phase1 и timer5.
    a.cpuWrite(0x5003, 0x08);
    require(a.levels().pulse1 == 0, "high write resets phase0 immediately");
    a.clockCpu(11);
    require(a.levels().pulse1 == 0, "high write keeps remaining timer until edge");
    a.clockCpu();
    require(a.levels().pulse1 == 15, "high write did not reset divider");
    b.cpuWrite(0x5001, 0xFF);
    b.cpuWrite(0x5005, 0xFF);
    b.cpuWrite(0x5008, 0xFF);
    b.cpuWrite(0x4017, 0x80);
    Audio plain;
    start(plain, 0, 0x3F, 9);
    plain.clockCpu(10);
    for (unsigned i = 0; i != 500; ++i) {
        b.clockCpu(); plain.clockCpu(); same(b, plain);
    }
    // Запись low не сбрасывает фазу и не меняет работающий divider.
    a.reset(); start(a, 0, 0x3F, 9); a.clockCpu(2);
    a.cpuWrite(0x5002, 0);
    a.clockCpu(19);
    require(a.levels().pulse1 == 15, "low write does not force early edge");
    a.clockCpu();
    require(a.levels().pulse1 == 0, "old running timer reaches edge before period0 reload");
}

/** @brief Envelope clocks независимы от APU4017 и от выбранного bulk-разбиения. */
void envelopesAndClockBoundaries() {
    ++groups;
    Audio a;
    start(a, 0, 0xC0, 0);
    require(a.levels().pulse1 == 0, "start flag is deferred until frame clock");
    a.clockCpu(Audio::framePeriodCpu - 1);
    a.cpuWrite(0x4017, 0x80);
    require(a.levels().pulse1 == 0, "native frame write does not clock MMC5 envelope");
    a.clockCpu();
    require(a.levels().pulse1 == 15, "first frame initializes decay15");
    for (unsigned value = 14; value != 0; --value) {
        a.clockCpu(Audio::framePeriodCpu);
        require(a.levels().pulse1 == value, "envelope0 decreases every autonomous clock");
    }
    a.clockCpu(Audio::framePeriodCpu);
    require(a.levels().pulse1 == 0, "nonloop envelope reaches0");
    a.clockCpu(Audio::framePeriodCpu);
    require(a.levels().pulse1 == 0, "nonloop envelope stays0");

    a.reset(); start(a, 0, 0xE0, 0);
    a.clockCpu(16 * Audio::framePeriodCpu);
    require(a.levels().pulse1 == 0, "loop envelope reaches0 before wrapping");
    a.clockCpu(Audio::framePeriodCpu);
    require(a.levels().pulse1 == 15, "loop envelope wraps on following clock");

    a.reset(); start(a, 0, 0xD2, 0);
    a.clockCpu(4 * Audio::framePeriodCpu);
    require(a.levels().pulse1 == 2, "constant volume overrides changing decay");
    a.cpuWrite(0x5000, 0xC2);
    require(a.levels().pulse1 == 14, "envelope advances behind constant volume selector");
    a.cpuWrite(0x5003, 0x08);
    require(a.levels().pulse1 == 14, "high write only queues envelope restart");
    a.clockCpu(Audio::framePeriodCpu);
    require(a.levels().pulse1 == 15, "restart takes effect at next frame edge");

    a.reset(); start(a, 0, 0xDF, 0, 3);
    a.clockCpu(Audio::framePeriodCpu - 1);
    require(status(a) == 1, "length before first frame");
    a.clockCpu();
    require(status(a) == 1, "length2 after exactly one frame");
    a.clockCpu(Audio::framePeriodCpu - 1);
    require(status(a) == 1, "no spurious120Hz half-step");
    a.clockCpu();
    require(status(a) == 0, "length2 expires after two240-rate clocks");
}

/** @brief PCM write-mode проверяет весь DAC-range, zero hold и IRQ-trip gating. */
void pcmWriteAndIrq() {
    ++groups;
    Audio a;
    a.cpuWrite(0x5010, 0x80);
    for (unsigned value = 1; value != 256; ++value) {
        a.cpuWrite(0x5011, static_cast<std::uint8_t>(value));
        require(a.levels().pcm == value && !a.irqLine(), "all8 DAC bits retained");
        a.cpuWrite(0x5011, 0);
        require(a.levels().pcm == value && a.irqLine(), "zero holds DAC and trips IRQ");
        require(a.cpuRead(0x5010, 0x22) == 0xA2 && !a.irqLine(), "5010 read reports then acknowledges");
    }
    a.cpuWrite(0x5011, 0);
    a.cpuWrite(0x5010, 0);
    require(!a.irqLine(), "disable masks pending trip");
    a.cpuWrite(0x5010, 0x80);
    require(a.irqLine(), "control write does not acknowledge trip");
    a.cpuWrite(0x5011, 0x11);
    require(!a.irqLine(), "nonzero event clears old trip without status read");
    a.cpuWrite(0x5010, 0);
    a.cpuWrite(0x5011, 0);
    a.cpuWrite(0x5010, 0x80);
    require(a.irqLine(), "trip recorded while IRQ enable was0");
    a.cpuWrite(0x5015, 0);
    require(a.irqLine() && a.levels().pcm == 0x11, "pulse enable does not control PCM");
    (void)a.sample(); (void)a.levels();
    require(a.irqLine(), "pure sample getter never acknowledges");
}

/** @brief Read-mode наблюдает точный диапазон CPU, включая opcode/dummy-подобные strobes. */
void pcmReadStrobes() {
    ++groups;
    Audio a(true);
    for (unsigned address = 0; address != 65536; ++address) {
        a.cpuWrite(0x5010, 0x80);
        a.cpuWrite(0x5011, 0x33);
        a.cpuWrite(0x5010, 0x81);
        const auto value = a.cpuRead(static_cast<std::uint16_t>(address), 0xA5);
        const bool prg = address >= 0x8000 && address <= 0xBFFF;
        require(a.levels().pcm == (prg ? 0xA5 : 0x33), "full16-bit read range decode");
        if (address != 0x5010 && address != 0x5015)
            require(value == 0xA5, "PRG or undriven bus value not replaced by audio");
    }
    a.cpuWrite(0x5011, 0);
    require(!a.irqLine() && a.levels().pcm == 0x33, "direct write ignored in read-mode including zero");
    require(a.cpuRead(0x8000, 0) == 0 && a.irqLine(), "readzero triggers PCM interrupt");
    require(a.levels().pcm == 0x33, "readzero keeps previous DAC");
    require((a.cpuRead(0x5010, 0) & 0x81U) == 0x81U, "MMC5A reports IRQ and revision");
    require(!a.irqLine(), "read acknowledges once");
    (void)a.cpuRead(0xBFFF, 0);
    require(a.irqLine(), "last valid CPU byte also trips");
    (void)a.cpuRead(0x8001, 0x66);
    require(!a.irqLine() && a.levels().pcm == 0x66, "subsequent opcode/dummy read clears trip and changes DAC");
    a.cpuWrite(0x5010, 0x80);
    (void)a.cpuRead(0x8000, 0);
    require(!a.irqLine() && a.levels().pcm == 0x66, "write-mode never snoops PRG");
}

/** @brief Reset detection сохраняет DAC/pulse/timer; cold reset очищает сессию. */
void resetDetectionAndMixing() {
    ++groups;
    Audio a;
    start(a, 0, 0xFF, 0);
    start(a, 1, 0xFF, 0);
    a.cpuWrite(0x5010, 0x80);
    a.cpuWrite(0x5011, 255);
    require(a.sample() == -95070, "nominal inverted full-scale sum without clipping");
    a.clockCpu(3);
    const auto before = a.levels();
    const auto edge = a.nextEventCpuCycles();
    a.cpuWrite(0x5011, 0);
    a.detectedReset();
    const auto after = a.levels();
    require(!a.irqLine() && status(a) == 3, "detected reset disables PCM IRQ, preserves lengths");
    require(before.pulse1 == after.pulse1 && before.pulse2 == after.pulse2 && after.pcm == 255,
            "detected reset retains DAC values");
    require(a.nextEventCpuCycles() == edge, "detected reset does not restart dividers");
    a.cpuWrite(0x5010, 0x80);
    require(a.irqLine(), "trip latch retained while reset control masks it");
    a.reset(true);
    require(a.sample() == 0 && status(a) == 0 && !a.irqLine(), "new session clears old sound");
    require(a.cpuRead(0x5010, 0) == 1, "cold revision selected explicitly");
}

/** @brief Batched clock сравнивается с единичными clocks после каждого bus event. */
void batchedClockAndEventFence() {
    ++groups;
    Audio span, scalar;
    start(span, 0, 0xE3, 13); start(scalar, 0, 0xE3, 13);
    start(span, 1, 0x9A, 247); start(scalar, 1, 0x9A, 247);
    std::uint32_t random = 0xA51437U;
    for (unsigned pass = 0; pass != 1200; ++pass) {
        random = random * 1664525U + 1013904223U;
        const auto count = random % 12000U;
        span.clockCpu(count);
        for (std::uint32_t i = 0; i != count; ++i) scalar.clockCpu();
        same(span, scalar);
        require(status(span) == status(scalar), "length equality after span");
        if ((pass % 7U) == 0) {
            const auto address = static_cast<std::uint16_t>(0x5000U + ((pass / 7U) & 7U));
            const auto value = static_cast<std::uint8_t>(random >> 18U);
            span.cpuWrite(address, value); scalar.cpuWrite(address, value);
        }
        if ((pass % 23U) == 0) {
            span.cpuWrite(0x5015, 3); scalar.cpuWrite(0x5015, 3);
        }
        same(span, scalar);
    }
    Audio event, single;
    start(event, 0, 0x3F, 37); start(single, 0, 0x3F, 37);
    for (unsigned i = 0; i != 2000; ++i) {
        const auto next = event.nextEventCpuCycles();
        require(next >= 1 && next <= Audio::framePeriodCpu, "strictly positive event horizon");
        const auto before = event.sample();
        event.clockCpu(next - 1);
        require(event.sample() == before, "no DAC change before advertised internal edge");
        event.clockCpu();
        for (std::uint32_t j = 0; j != next; ++j) single.clockCpu();
        same(event, single);
    }
}

/**
 * @brief Проверяет duty-mean независимым средним исходного raw за целый период.
 * @details Constant-volume/halt исключают frame/envelope из короткого окна.
 * Ни новый portable getter, ни его deadline не участвуют в эталонном среднем.
 */
void portableDutyMeans() {
    ++groups;
    constexpr unsigned on[4] = {1, 2, 4, 6};
    for (unsigned period : {0U, 1U}) for (unsigned duty = 0; duty != 4; ++duty)
        for (unsigned volume = 0; volume != 16; ++volume) {
            Audio a;
            start(a, 0, static_cast<std::uint8_t>((duty << 6U) | 0x30U | volume), period);
            a.clockCpu(2); // Первый divider edge завершён, теперь полный reload period.
            for (unsigned pcm : {0U, 1U, 127U, 255U}) {
                a.cpuWrite(0x5011, static_cast<std::uint8_t>(pcm));
                a.clockCpu(); // Чередуются pending/nonpending и обе M2 half-phases.
                const auto expected = -static_cast<std::int32_t>(653U * volume * on[duty] / 8U) -
                                      static_cast<std::int32_t>(296U * pcm);
                require(a.portableSample() == expected, "portable rational duty mean and exact PCM");
                Audio cycle = a;
                std::int64_t rawSum = 0;
                const unsigned clocks = 16U * (period + 1U);
                for (unsigned tick = 0; tick != clocks; ++tick) { cycle.clockCpu(); rawSum += cycle.sample(); }
                require(a.portableSample() == rawSum / clocks, "portable equals independent complete raw waveform mean");
                std::array<unsigned char, sizeof(Audio)> before{};
                std::memcpy(before.data(), &a, sizeof(a));
                (void)a.portableSample(); (void)a.nextPortableOutputCpuCycles();
                require(std::memcmp(before.data(), &a, sizeof(a)) == 0, "portable getters preserve complete object representation");
            }
        }
    Audio mixed;
    start(mixed, 0, 0xB1, 0); start(mixed, 1, 0xB1, 1); mixed.clockCpu(2);
    require(mixed.portableSample() == -653, "two half-unit channels round once after summing gain");
    Audio half;
    start(half, 0, 0xB1, 1);
    require(half.portableSample() == -326, "gain precedes division: volume1 half duty is not zero");
    for (unsigned period : {2U, 15U, 2047U}) {
        Audio raw;
        start(raw, 0, 0xBF, period);
        for (unsigned clock = 0; clock != 100; ++clock) {
            require(raw.portableSample() == raw.sample(), "audible noncarrier pulse stays exact");
            require(raw.nextPortableOutputCpuCycles() == raw.nextOutputCpuCycles(), "noncarrier deadline fast fallback");
            raw.clockCpu();
        }
    }
}

/** @brief Сравнивает raw цифровые статусы и будущие сроки после разных разбиений. */
void portableSame(Audio& span, Audio& scalar) {
    same(span, scalar);
    require(status(span) == status(scalar), "portable grouping retains raw lengths");
    require(span.nextOutputCpuCycles() == scalar.nextOutputCpuCycles(), "raw output deadline retained");
    require(span.portableSample() == scalar.portableSample(), "portable output at exact cut");
    require(span.nextPortableOutputCpuCycles() == scalar.nextPortableOutputCpuCycles(), "portable deadline at exact cut");
}

/**
 * @brief Проверяет обещанный тихий интервал каждым исходным единичным clock.
 * @param clocks Новый bulk-span; может пересечь horizon для проверки large clock.
 * @details До объявленной границы scalar output обязан быть постоянным.
 */
void portableAdvance(Audio& span, Audio& scalar, unsigned clocks) {
    const auto horizon = span.nextPortableOutputCpuCycles();
    require(horizon && horizon <= Audio::framePeriodCpu, "positive bounded portable horizon including pending");
    const auto before = span.portableSample();
    span.clockCpu(clocks);
    for (unsigned n = 0; n != clocks; ++n) {
        scalar.clockCpu();
        if (n + 1U < horizon) require(scalar.portableSample() == before, "no portable edge before advertised horizon");
    }
    portableSame(span, scalar);
}

/** @brief Содержательные mixed/envelope/MMIO случаи без декартова миллиарда шагов. */
void portableHorizonsAndBusEdges() {
    ++groups;
    constexpr unsigned periods[8][2] = {{0,1},{1,2},{0,15},{2,1},{15,2047},{1,0},{2047,1},{0,1}};
    for (unsigned scenario = 0; scenario != 8; ++scenario) {
        Audio span, scalar;
        for (auto* a : {&span, &scalar}) {
            start(*a, 0, static_cast<std::uint8_t>(scenario == 5 ? 0xA2 : 0xB1), periods[scenario][0]);
            start(*a, 1, static_cast<std::uint8_t>(scenario == 5 ? 0x40 : 0x7F), periods[scenario][1], 3);
            a->cpuWrite(0x5010, 0x80); a->cpuWrite(0x5011, 0x71);
        }
        portableAdvance(span, scalar, 2);
        if (scenario == 6) {
            // Новый short period сохраняет прежний длинный timer residual.
            for (auto* a : {&span, &scalar}) { a->cpuWrite(0x5002, 1); a->cpuWrite(0x5003, 0x08); }
        }
        for (unsigned pass = 0; pass != 40; ++pass) {
            const auto edge = span.nextPortableOutputCpuCycles();
            const unsigned clocks = pass % 4U == 0 ? edge - 1U : pass % 4U == 1 ? edge :
                                    pass % 4U == 2 ? edge + 1U : 12000U + pass * 31U;
            portableAdvance(span, scalar, clocks);
            if (scenario == 5 && pass < 8) continue; // Настоящие envelope/length frame edges.
            const std::uint16_t addresses[]{0x5000,0x5002,0x5003,0x5004,0x5006,0x5007,0x5015,0x5011};
            const auto address = addresses[pass & 7U];
            const auto value = static_cast<std::uint8_t>(address == 0x5015 ? ((pass >> 3U) & 3U) :
                address == 0x5002 || address == 0x5006 ? pass % 3U :
                address == 0x5003 || address == 0x5007 ? 0x08U : 0xB0U | (pass & 15U));
            span.cpuWrite(address, value); scalar.cpuWrite(address, value);
            portableSame(span, scalar);
            if ((pass % 5U) == 0) {
                const auto control = static_cast<std::uint8_t>((pass & 1U) ? 0x81 : 0x80);
                span.cpuWrite(0x5010, control); scalar.cpuWrite(0x5010, control);
                const auto data = static_cast<std::uint8_t>((pass & 2U) ? 0 : 0x77);
                require(span.cpuRead(0x8000, data) == scalar.cpuRead(0x8000, data), "exact PCM read-strobe byte");
                portableSame(span, scalar);
                require(span.cpuRead(0x5010, 0x22) == scalar.cpuRead(0x5010, 0x22), "exact PCM IRQ acknowledge");
            }
        }
        for (unsigned future = 0; future != 128; ++future) portableAdvance(span, scalar, 1);
    }
}

/** @brief Крупный прошедший span сравнивается с настоящими единичными clocks, без скрытых MMIO. */
void portableLargeSpan() {
    ++groups;
    Audio span, scalar;
    for (auto* a : {&span, &scalar}) {
        start(*a, 0, 0xBF, 1); start(*a, 1, 0x31, 0);
        a->cpuWrite(0x5010, 0x80); a->cpuWrite(0x5011, 255); a->cpuWrite(0x5011, 0);
    }
    portableAdvance(span, scalar, 1000001);
    require(span.irqLine() && scalar.irqLine(), "portable large span does not clear PCM IRQ");
    span.detectedReset(); scalar.detectedReset(); portableSame(span, scalar);
    span.reset(true); scalar.reset(true); portableSame(span, scalar);
}
/** @brief Проверяет invalidation отдельным восьмифазным oracle, а не вторым cache.
 * @param declared Передал ли CTest ожидаемую private policy явно.
 * @param expectedCache Ожидаемая активация selected-cache; не PUBLIC define теста.
 * @details Обратимая порча slot обязана менять только portable getter при ON.
 * Восстановление выполняется до любого следующего clock или MMIO; это не guest API.
 */
void selectedCacheInvalidation(bool declared,bool expectedCache) {
    ++groups;using Access=nes::core::Mmc5AudioCacheTestAccess;
    /**
     * @brief Сравнивает portable DAC с независимым восьмифазным oracle, затем проверяет неизменность RAW getter.
     * @details Читает const audio без clock/MMIO; намеренная порча selected-cache slot снаружи обязана обнаруживаться только выбранной portable policy.
     */
    const auto observe=[](const Audio& a){
        require(a.portableSample()==Access::expected(a),"independent eight-phase selected cache invalidation");
        const auto raw=a.levels();
        require(a.sample()==-653*(static_cast<int>(raw.pulse1)+raw.pulse2)-296*raw.pcm,"selected policy preserves RAW sample getter");
    };
    Audio a;observe(a);start(a,0,0x9f,1,3);start(a,1,0xb1,0,3);observe(a);
    if(declared){const auto saved=Access::slot(a);const auto raw=a.sample();Access::slot(a)^=1;
        require((a.portableSample()!=Access::expected(a))==expectedCache,"private selected cache activation and corruption negative control");
        if(expectedCache)require(a.sample()==raw,"corrupt selected slot cannot alter RAW API");
        Access::slot(a)=saved;observe(a);
    }
    for(unsigned period:{2U,1U,0U,31U,1U}){a.cpuWrite(0x5002,static_cast<std::uint8_t>(period));observe(a);
        for(unsigned n=0;n<67;++n){a.clockCpu();observe(a);}}
    // Новый длинный divider начинается с cold timer0, не с предыдущего short debt.
    a.reset();start(a,0,0x9f,127,3);a.clockCpu(2);a.cpuWrite(0x5002,1);
    require(Access::oldLongTimer(a),"long timer remains after period-only short write");observe(a);
    for(unsigned n=0;n<260;++n){a.clockCpu();observe(a);}
    for(unsigned duty=0;duty<4;++duty)for(unsigned volume:{0U,1U,15U}){
        a.cpuWrite(0x5000,static_cast<std::uint8_t>((duty<<6U)|0x10U|volume));observe(a);
        a.clockCpu(3);observe(a);
    }
    a.cpuWrite(0x5000,2);a.cpuWrite(0x5003,0);a.cpuWrite(0x5004,0x20);a.cpuWrite(0x5007,0);observe(a);
    for(unsigned frame=0;frame<25;++frame){a.clockCpu(Audio::framePeriodCpu-1);observe(a);a.clockCpu();observe(a);}
    for(unsigned enabled:{0U,3U,1U,2U,3U}){a.cpuWrite(0x5015,static_cast<std::uint8_t>(enabled));observe(a);
        a.cpuWrite(0x5003,0x08);observe(a);a.cpuWrite(0x5007,0x08);observe(a);}
    for(unsigned pcm:{0U,1U,255U,0U,12U,12U}){a.cpuWrite(0x5011,static_cast<std::uint8_t>(pcm));observe(a);}
    a.cpuWrite(0x5010,0x81);observe(a);
    for(unsigned value:{0U,1U,0U,255U}){(void)a.cpuRead(0x8000,static_cast<std::uint8_t>(value));observe(a);(void)a.cpuRead(0x5010,0xa0);observe(a);}
    a.detectedReset();observe(a);a.reset(true);observe(a);
}
} // namespace

/** @brief Исполняет только переносимые chip-contract проверки, не игры/FPS. */
int main(int argc,char**argv) {
    const bool declared=argc==3;
    require(argc==1||(declared&&std::strcmp(argv[1],"--selected-cache")==0&&
        (std::strcmp(argv[2],"0")==0||std::strcmp(argv[2],"1")==0)),"optional expected private cache policy argument");
    const bool expectedCache=declared&&std::strcmp(argv[2],"1")==0;
    resetAndDecode();
    enablesAndLengths();
    pulseTimers();
    timerWritesAndIgnoredSweep();
    envelopesAndClockBoundaries();
    pcmWriteAndIrq();
    pcmReadStrobes();
    resetDetectionAndMixing();
    batchedClockAndEventFence();
    portableDutyMeans();
    portableHorizonsAndBusEdges();
    portableLargeSpan();
    selectedCacheInvalidation(declared,expectedCache);
    std::printf("MMC5_AUDIO_PASS groups=%u checks=%u\n", groups, checks);
    return 0;
}
