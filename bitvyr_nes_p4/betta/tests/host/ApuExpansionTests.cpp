/**
 * @file ApuExpansionTests.cpp
 * @brief Внешний DAC: быстрый APU против принудительно скалярного event-path.
 */
#include "apu/Apu.h"
#include "apu/AudioRenderer.h"
#include "cartridge/Mmc5Audio.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {
unsigned checks = 0, cases = 0;
unsigned long long comparedPcm = 0, inputEdges = 0, wideInputEdges = 0;
unsigned long long quietAccepted = 0;

/** @brief Фиксирует первое расхождение без ослабления PCM/clock-проверок. */
void require(bool condition, const char* text) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "APU_EXPANSION_FAIL case=%u check=%u %s\n", cases, checks, text);
        std::exit(1);
    }
}

/** @brief Детерминированные настоящие DMC-ответы; bus не clock-ает APU повторно. */
struct Bus final : nes::core::ApuBus {
    std::array<std::uint16_t, 4096> addresses{};
    unsigned reads = 0, stalls = 0;
    /** @brief Возвращает байт отображённой выборки, сохраняя порядок адресов. */
    std::uint8_t dmcRead(std::uint16_t address) noexcept override {
        require(reads < addresses.size(), "bounded DMC trace");
        addresses[reads++] = address;
        return static_cast<std::uint8_t>((address * 37U) ^ (address >> 3U));
    }
    /** @brief Только учитывает запрос stall, не добавляя скрытых тестовых clocks. */
    void dmcStall(unsigned count) noexcept override { stalls += count; }
};

/** @brief Сравнивает опубликованный PCM, IRQ, виртуализированные clocks и DMC. */
void compare(nes::core::Apu& a, nes::core::Apu& b, const Bus& x, const Bus& y) {
    require(a.sampleCount() == b.sampleCount(), "sample count");
    for (std::size_t i = 0; i != a.sampleCount(); ++i) {
        require(a.samples()[i] == b.samples()[i], "every signed PCM sample");
        ++comparedPcm;
    }
    require(a.totalSamples() == b.totalSamples(), "total sample clocks");
    require(a.droppedSamples() == 0 && b.droppedSamples() == 0, "no hidden dropped PCM");
    require(a.irqLine() == b.irqLine(), "IRQ level");
    const auto p = a.debug(), q = b.debug();
    require(p.pulsePeriod == q.pulsePeriod && p.length == q.length, "pulse periods and all lengths");
    require(p.dmcAddress == q.dmcAddress && p.dmcRemaining == q.dmcRemaining && p.dmcOutput == q.dmcOutput,
            "DMC address/count/DAC");
    require(p.noiseShift == q.noiseShift && p.triangleLinear == q.triangleLinear, "noise and triangle");
    require(p.frameCycle == q.frameCycle && p.frameIrq == q.frameIrq && p.dmcIrq == q.dmcIrq, "frame phase/IRQs");
    require(x.reads == y.reads && x.stalls == y.stalls, "DMC counts/stalls");
    for (unsigned i = 0; i != x.reads; ++i) require(x.addresses[i] == y.addresses[i], "ordered DMC addresses");
    a.clearSamples(); b.clearSamples();
}

/** @brief Одинаково включает все native каналы, не используя expansion для clock. */
void setupNative(nes::core::Apu& a) {
    a.write(0x4015, 0x1F);
    a.write(0x4000, 0xBF); a.write(0x4002, 23); a.write(0x4003, 0x18);
    a.write(0x4004, 0x77); a.write(0x4006, 61); a.write(0x4007, 0x20);
    a.write(0x4008, 0xBF); a.write(0x400A, 5); a.write(0x400B, 0x18);
    a.write(0x400C, 0x3B); a.write(0x400E, 3); a.write(0x400F, 0x18);
    a.write(0x4010, 0x8F); a.write(0x4011, 0x45);
    a.write(0x4012, 0x20); a.write(0x4013, 0x07); a.write(0x4015, 0x1F);
}

/**
 * @brief Исполняет реальный MMC5 clock/DAC и два независимых APU event schedules.
 * @details Reference перед каждым tick вызывает старый Apu::write4018:
 * неизвестный регистр не меняет канал, но synchronize+eventCountdown1 заставляют
 * исходное scalar тело tickEvent исполниться на каждом такте. Reference не
 * полагается на пробуждение из нового setter и ловит исходную потерю DAC edges.
 */
void run(bool pal, bool native, bool expansion, unsigned seed) {
    ++cases;
    Bus firstBus, referenceBus;
    nes::core::Apu actual(firstBus), reference(referenceBus);
    actual.reset(pal); reference.reset(pal);
    if (native) { setupNative(actual); setupNative(reference); }
    nes::core::Mmc5Audio chip;
    chip.cpuWrite(0x5015, 3);
    chip.cpuWrite(0x5000, 0xFF); chip.cpuWrite(0x5002, 0); chip.cpuWrite(0x5003, 0x08);
    chip.cpuWrite(0x5004, 0x7B); chip.cpuWrite(0x5006, 7); chip.cpuWrite(0x5007, 0x08);
    chip.cpuWrite(0x5011, 255);
    std::int32_t previousInput = 0;
    bool nonzeroPcm = false;
    std::uint32_t random = seed;
    for (unsigned cycle = 0; cycle != 180000; ++cycle) {
        chip.clockCpu();
        const auto input = expansion ? chip.sample() : 0;
        const auto difference = input - previousInput;
        if (difference) ++inputEdges;
        if (difference > 65535 || difference < -65535) ++wideInputEdges;
        previousInput = input;
        const auto oldPhase = actual.debug().frameCycle;
        const auto oldSamples = actual.totalSamples();
        actual.setExpansionInput(input);
        require(actual.debug().frameCycle == oldPhase && actual.totalSamples() == oldSamples,
                "setter does not advance a new CPU clock/sample");
        reference.setExpansionInput(input);
        reference.write(0x4018, 0); // Независимый тестовый эталон принудительно исполняет прежнее скалярное расписание.
        actual.tick(); reference.tick();
        for (std::size_t i = 0; i != actual.sampleCount(); ++i) nonzeroPcm |= actual.samples()[i] != 0;
        if ((cycle % 113U) == 112U) compare(actual, reference, firstBus, referenceBus);

        // Эффекты шины следуют за этим CPU-тактом, как регистровый I/O настоящей Machine.
        random = random * 1664525U + 1013904223U;
        if ((cycle % 3U) == 0) chip.cpuWrite(0x5011, (cycle & 1U) ? 1 : 255);
        if ((cycle % 199U) == 0) chip.cpuWrite(0x5002, static_cast<std::uint8_t>(random >> 24U));
        if ((cycle % 503U) == 0) chip.cpuWrite(0x5003, static_cast<std::uint8_t>(0x08U | ((random >> 19U) & 7U)));
        if (native && (cycle % 383U) == 0) {
            const auto value = static_cast<std::uint8_t>(random >> 24U);
            actual.write(0x4011, value); reference.write(0x4011, value);
        }
        if ((cycle % 29989U) == 0) require(actual.readStatus() == reference.readStatus(), "matched real IRQ acknowledge");
    }
    compare(actual, reference, firstBus, referenceBus);
    // Native reset может давать HPF-transient от удерживаемого triangle DAC;
    // zero-input control проверяется на everyPCM parity, а не на invented silence.
    if (native || expansion) require(nonzeroPcm, "positive audible PCM witness");
}

/**
 * @brief Сравнивает отдельный quiet-вход с независимым старым scalar tick.
 * @details Таймеры0/1/46, DMC и sample/frame edges возникают настоящими clocks.
 * Same-U запись здесь явно исключает новый вход, как требует его контракт.
 */
void quietInput(bool pal, unsigned period, bool dmc) {
    ++cases;
    Bus actualBus, oldBus;
    nes::core::Apu actual(actualBus), old(oldBus);
    actual.reset(pal); old.reset(pal);
    for (auto* a : {&actual, &old}) {
        setupNative(*a);
        a->write(0x400A, static_cast<std::uint8_t>(period));
        if (!dmc) a->write(0x4015, 0x0F);
    }
    for (unsigned cycle = 0; cycle != 40000; ++cycle) {
        const bool nativeAtStamp = (cycle % 173U) == 0;
        if (nativeAtStamp) {
            const auto value = static_cast<std::uint8_t>(cycle >> 3U);
            actual.write(0x4011, value); old.write(0x4011, value);
        }
        const std::int32_t level = (cycle & 4U) ? -95070 : -653;
        const auto oldSamples = actual.totalSamples();
        bool accepted = false;
        if (!nativeAtStamp) accepted = actual.trySetExpansionInputQuiet(level);
        if (accepted) ++quietAccepted;
        else actual.setExpansionInput(level);
        require(actual.totalSamples() == oldSamples, "quiet setter does not emit or advance sample time");
        old.setExpansionInput(level);
        old.write(0x4018, 0); // Старый независимый scalar reference, без quiet API.
        actual.tick(); old.tick();
        if ((cycle % 113U) == 112U) compare(actual, old, actualBus, oldBus);
    }
    compare(actual, old, actualBus, oldBus);
}

/** @brief Отказ не материализует даже отложенные таймеры и не пишет FIR-память. */
void quietFailure(bool pal, bool featureOff) {
    ++cases;
    Bus bus;
    nes::core::Apu actual(bus); actual.reset(pal);
    std::array<unsigned char, sizeof(nes::core::Apu)> before{};
    /**
     * @brief Проверяет чистый отказ quiet-setter на заданном DAC-уровне.
     * @param level Новый внешний DAC в исходных целочисленных единицах APU.
     * @details До вызова копируется полное представление actual; false обязан сохранить каждый байт, включая FIR и отложенные таймеры.
     */
    const auto rejected = [&](std::int32_t level) {
        std::memcpy(before.data(), &actual, sizeof(actual));
        require(!actual.trySetExpansionInputQuiet(level), "quiet guard returns false");
        require(std::memcmp(before.data(), &actual, sizeof(actual)) == 0, "quiet rejection preserves complete object representation");
    };
    rejected(-653); // Cold eventCountdown1 требует исходного tick.
    actual.tick();
    rejected(0); // Неизменившийся вход ничего не материализует.
    if (featureOff) rejected(-653);
    actual.write(0x4017, 0x80);
    rejected(-95070); // Pending frame reset остаётся на прежнем пути.
}

/**
 * @brief Строит настоящее immutable tape с Input/повторами при одинаковом U.
 * @details Reference повторяет только старые public операции и literal clocks.
 * Несколько последовательных пакетов проверяют E@end и MMIO следующего base.
 */
void rendererTies(bool pal, unsigned initialPeriod) {
    ++cases;
    using namespace nes::core;
    auto active = std::make_unique<AudioPacket>();
    auto sealed = std::make_unique<AudioPacket>();
    auto actual = std::make_unique<AudioRenderer>();
    Bus oldBus;
    Apu old(oldBus); old.reset(pal);
    std::uint64_t oldCycle = 0;
    /**
     * @brief Доводит независимый старый Apu до абсолютного CPU-такта target.
     * @details Исполняет только отдельные tick и увеличивает oldCycle; не использует quiet API проверяемого renderer.
     */
    const auto oldAdvance = [&](std::uint64_t target) {
        while (oldCycle < target) { old.tick(); ++oldCycle; }
    };
    constexpr unsigned span = 4096;
    for (unsigned sequence = 0; sequence != 24; ++sequence) {
        const auto base = std::uint64_t(sequence) * span;
        active->begin(1, sequence, base, pal ? Region::Pal : Region::Ntsc, true);
        require(active->addInput(base, AudioPacket::clearSamplesOperation, 0), "packet sample-clear append");
        require(active->addInput(base, 0x0A, static_cast<std::uint8_t>((sequence & 1U) ? 0 : initialPeriod)),
                "next-packet boundary MMIO append");
        if (!sequence) {
            const std::array<AudioPacket::Input, 8> setup{{
                {0,0x15,0x0F},{0,0x00,0xBF},{0,0x02,23},{0,0x03,0x18},
                {0,0x08,0xBF},{0,0x0A,static_cast<std::uint8_t>(initialPeriod)},
                {0,0x0B,0x18},{0,0x17,0xC0}}};
            for (const auto& in : setup) require(active->addInput(base, in.operation, in.value), "native setup append");
        }
        for (unsigned offset = 1; offset <= span; ++offset) {
            const auto stamp = base + offset;
            if ((offset % 97U) == 0)
                require(active->addInput(stamp, 0x0A, static_cast<std::uint8_t>((offset / 97U) % 3U)),
                        "native before same-U external append");
            if ((offset & 3U) == 0 || (offset % 149U) == 0 || (offset % 251U) == 0)
                require(active->addExpansion(stamp, (offset & 4U) ? -95070 : -653), "external append");
            if ((offset % 149U) == 0)
                require(active->addInput(stamp, 0x08, static_cast<std::uint8_t>(0x80U | ((offset >> 5U) & 0x7FU))),
                        "native after same-U external append");
            if ((offset % 251U) == 0)
                require(active->addExpansion(stamp, -30000), "repeated same-U external append");
        }
        require(active->retireTo(*sealed, base + span, sequence + 1U), "packet retire");
        unsigned native = 0;
        /**
         * @brief Повторяет один native Input на его точном base+offset в scalar oracle.
         * @details Сначала доводит старый Apu до stamp, затем очищает samples либо пишет регистр; порядок одинакового U задаёт nativePrefix expansion-события.
         */
        const auto apply = [&](const AudioPacket::Input& in) {
            oldAdvance(base + in.offset);
            if (in.operation == AudioPacket::clearSamplesOperation) old.clearSamples();
            else old.write(static_cast<std::uint16_t>(0x4000U + in.operation), in.value);
        };
        for (unsigned i = 0; i < sealed->header().expansionCount; ++i) {
            const auto& e = sealed->expansions()[i];
            while (native < e.nativePrefix) apply(sealed->inputs()[native++]);
            oldAdvance(base + e.offset); old.setExpansionInput(e.level);
        }
        while (native < sealed->header().inputCount) apply(sealed->inputs()[native++]);
        oldAdvance(base + span);
        require(actual->render(*sealed) == AudioRenderResult::Ok, "renderer same-U packet accepted");
        require(actual->cycles() == oldCycle && actual->sampleCount() == old.sampleCount(), "packet cycle/sample parity");
        require(std::memcmp(actual->samples(), old.samples(), old.sampleCount() * sizeof(std::int16_t)) == 0,
                "every packet PCM byte equals old scalar replay");
        comparedPcm += old.sampleCount();
    }
}
} // namespace

/** @brief Проверяет NTSC/PAL, native/no-native и отсутствие expansion по одному clock. */
int main(int argc, char** argv) {
    int quietExpected = -1;
    if (argc == 3 && std::strcmp(argv[1], "--expansion-quiet") == 0 &&
        (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0))
        quietExpected = argv[2][0] - '0';
    else require(argc == 1, "optional CLI: --expansion-quiet 0|1");
    for (const bool pal : {false, true}) {
        quietFailure(pal, quietExpected == 0);
        run(pal, false, false, 0x1A2BU);
        run(pal, true, false, 0x4513U);
        run(pal, false, true, 0xE571U);
        run(pal, true, true, 0x9147U);
        for (const unsigned period : {0U, 1U, 46U}) {
            quietInput(pal, period, false);
            quietInput(pal, period, true);
            rendererTies(pal, period);
        }
    }
    require(inputEdges > 100000 && wideInputEdges > 1000, "real rapid and wide MMC5 DAC changes exercised");
    if (quietExpected >= 0) require(quietExpected ? quietAccepted > 1000 : quietAccepted == 0,
                                   "actual quiet feature activation matches expected CLI");
    std::printf("APU_EXPANSION_PASS cases=%u checks=%u pcm=%llu input_edges=%llu wide_edges=%llu quiet_accepted=%llu\n",
                cases, checks, comparedPcm, inputEdges, wideInputEdges, quietAccepted);
    return 0;
}
