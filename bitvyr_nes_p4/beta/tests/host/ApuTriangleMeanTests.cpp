/**
 * @file ApuTriangleMeanTests.cpp
 * @brief Направленные регрессии переносного triangle: шесть семантических групп.
 * @details Эталон исполняет каждый CPU-такт без deadline/entry/advanceSkipped.
 * Общие старые primitives: frame/timer/DMC, raw mixer(false), FIR(false), emitSample.
 * Независимы: расписание, среднее32 raw-фаз, eligibility и выбор dense kernel.
 * Это проверка контракта оптимизации, а не второй эталон формул DAC/FIR и не FPS.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <vector>

// Только native fixture открывает состояние для проверки границ и FIR-хвоста.
// Рабочие заголовки, layout и PRIVATE-флаги Core не изменяются.
#define private public
#include "apu/Apu.h"
#undef private
#include "apu/AudioRenderer.h"

#if defined(NES_CORE_PORTABLE_NATIVE_TRIANGLE_MEAN)
#error "The Core triangle policy must remain PRIVATE and not reach its test consumer"
#endif

namespace {
using namespace nes::core;
unsigned checks = 0;

/** @brief Останавливает тест на первой нарушенной семантической границе. */
[[noreturn]] void fail(const char* expression, unsigned line) {
    std::fprintf(stderr, "triangle-mean line=%u expression=%s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) do { ++checks; if (!(expression)) fail(#expression, __LINE__); } while (false)

/** @brief Возвращает детерминированные DMC-байты и сохраняет порядок реальных обращений. */
struct Bus final : ApuBus {
    Apu* owner = nullptr;
    std::vector<std::tuple<unsigned, std::uint64_t, unsigned>> trace;
    /** @brief Фиксирует гостевой такт и адрес; чтение не исполняет новый APU-такт. */
    std::uint8_t dmcRead(std::uint16_t address) noexcept override {
        trace.emplace_back(0, owner->cycles_ + owner->pendingCycles_, address);
        return static_cast<std::uint8_t>((address * 37U) ^ 0xa5U);
    }
    /** @brief Фиксирует украденные clocks без рекурсивного продвижения устройства. */
    void dmcStall(unsigned count) noexcept override {
        trace.emplace_back(1, owner->cycles_ + owner->pendingCycles_, count);
    }
};

/** @brief Сериализует цифровой logical cut без padding, материализуя pending лишь в копии. */
std::vector<std::uint64_t> digital(const Apu& source) {
    auto a = source;
    a.synchronize();
    std::vector<std::uint64_t> out;
    for (const auto& p : a.pulses_) {
        out.insert(out.end(), {p.envelope.control, p.envelope.divider, p.envelope.decay,
            p.envelope.start, p.period, p.timer, p.sweep, p.sweepDivider, p.sequence,
            p.length, p.enabled, p.sweepReload});
    }
    const auto& t = a.triangle_;
    out.insert(out.end(), {t.period, t.timer, t.control, t.linear, t.length,
        t.sequence, t.enabled, t.reload});
    const auto& n = a.noise_;
    out.insert(out.end(), {n.envelope.control, n.envelope.divider, n.envelope.decay,
        n.envelope.start, n.timer, n.shift, n.control, n.length, n.enabled});
    const auto& d = a.dmc_;
    out.insert(out.end(), {d.timer, d.address, d.remaining, d.control, d.start,
        d.length, d.output, d.buffer, d.shift, d.bits, d.bufferEmpty, d.silent, d.irq});
    out.insert(out.end(), {a.cycles_, a.frameCycle_, a.samplePhase_, a.frameResetDelay_,
        a.pal_, a.fiveStep_, a.irqInhibit_, a.frameIrq_, a.totalSamples_,
        a.droppedSamples_, a.sampleCount_});
    return out;
}

/** @brief Определяет допустимость среднего непосредственно из цифрового состояния. */
bool oracleEligible(const Apu& a) {
    const auto& t = a.triangle_;
    return t.period < 2 && t.length != 0 && t.linear != 0 && t.timer <= t.period;
}

/**
 * @brief Усредняет32 настоящие raw DAC-фазы после нелинейного смешивания.
 * @details Raw-уровень triangle0 служит целочисленным смещением; оставшиеся
 * неотрицательные TND-разности суммируются в uint64. Ни mixerOutput(true),
 * ни triangleMeanEligible не вызываются. Старые pulse/noise/DMC/DAC общие.
 */
std::int32_t oracleMix(Apu& a, bool portable) {
    if (!portable || !oracleEligible(a)) return a.mixerOutput(false);
    const auto saved = a.triangle_.sequence;
    a.triangle_.sequence = 15; // Raw DAC0, а не средний аргумент нелинейной функции.
    const auto offset = a.mixerOutput(false);
    std::uint64_t sum = 0;
    for (unsigned phase = 0; phase < 32; ++phase) {
        a.triangle_.sequence = static_cast<std::uint8_t>(phase);
        const auto value = a.mixerOutput(false);
        CHECK(value >= offset);
        sum += static_cast<unsigned>(value - offset);
    }
    a.triangle_.sequence = saved;
    return offset + static_cast<std::int32_t>((sum + 16U) / 32U);
}

/**
 * @brief Исполняет literal1M2: clocks, затем sample, затем новый DAC/FIR-вклад.
 * @param denseKernel true выбирает прежний dense FIR при допустимом среднем;
 * false нужен только отрицательному контролю ошибочного rank4 выбора.
 * @details При dense kernel period временно равен2 только внутри старого
 * depositDelta(false), затем немедленно восстановлен. Это test-only выбор
 * фильтра, без clocks/mixer и без изменения гостевого состояния. Сам FIR
 * не копируется. Digital-before/after требует неизменных timer/phase/DMC.
 */
void scalar(Apu& a, unsigned count, bool portable, bool denseKernel = true) {
    for (unsigned i = 0; i < count; ++i) {
        CHECK(a.pendingCycles_ == 0);
        ++a.cycles_;
        a.frameCounter();
        a.fetchDmc();
        a.clockTimers();
        a.samplePhase_ += Apu::sampleRate;
        if (a.samplePhase_ >= a.cpuRate_) {
            a.samplePhase_ -= a.cpuRate_;
            a.emitSample(); // sample предшествует deposit даже при совпавших границах.
        }
        const auto mixed = oracleMix(a, portable);
        if (mixed != a.lastMix_) {
            const auto before = digital(a);
            const auto period = a.triangle_.period;
            if (portable && oracleEligible(a) && denseKernel) a.triangle_.period = 2;
            a.depositDelta(mixed - a.lastMix_, false);
            a.triangle_.period = period;
            CHECK(digital(a) == before);
            a.lastMix_ = mixed;
        }
    }
}

/** @brief Продвигает настоящий renderer helper либо его документированный fallback. */
void rendererAdvance(Apu& a, unsigned count, bool portable) {
#if defined(NES_CORE_APU_TRIANGLE_SEGMENTS) && NES_CORE_APU_TRIANGLE_SEGMENTS
    a.advanceRendererCpuCycles(count, portable);
#else
    a.advanceCpuCycles(count, portable);
#endif
}

/** @brief Подготавливает одинаковый допустимый channel cut, не скрывая tick в setup. */
void setup(Apu& a, Bus& bus, unsigned period, unsigned timer) {
    a.reset();
    bus.owner = &a;
    auto& t = a.triangle_;
    t.period = static_cast<std::uint16_t>(period);
    t.timer = static_cast<std::uint16_t>(timer);
    t.sequence = 15; t.length = 9; t.linear = 9; t.control = 0x89; t.enabled = true;
    auto& p = a.pulses_[0];
    p.period = 17; p.timer = 5; p.length = 9; p.enabled = true; p.envelope.control = 0x3f;
    a.noise_.length = 9; a.noise_.control = 2; a.noise_.timer = 9;
    a.noise_.envelope.control = 0x1b; a.noise_.enabled = true; a.dmc_.output = 37;
}

/** @brief Владеет raw digital, оптимизированным mean и независимым literal mean. */
struct Trio {
    Bus rb, mb, sb;
    Apu raw{rb}, mean{mb}, reference{sb};
    /** @brief Устанавливает три одинаковых channel cuts и правильных владельцев шин. */
    Trio(unsigned period, unsigned timer) {
        setup(raw, rb, period, timer); setup(mean, mb, period, timer);
        setup(reference, sb, period, timer);
    }
    /** @brief Применяет одну настоящую MMIO-запись во всех трёх экземплярах. */
    void write(std::uint16_t address, std::uint8_t value) {
        raw.write(address, value); mean.write(address, value); reference.write(address, value);
    }
    /** @brief Сопоставляет external DAC с прежним setter; возвращает фактический quiet admission. */
    bool expansion(std::int32_t level) {
        raw.setExpansionInput(level);
        const bool accepted = mean.trySetExpansionInputQuiet(level, true);
        if (!accepted) mean.setExpansionInput(level);
        reference.setExpansionInput(level);
        return accepted;
    }
    /** @brief Требует digital/DMC parity и каждого PCM-отсчёта mean против literal. */
    void compare() const {
        CHECK(digital(raw) == digital(mean)); CHECK(digital(mean) == digital(reference));
        CHECK(rb.trace == mb.trace); CHECK(mb.trace == sb.trace);
        CHECK(mean.sampleCount_ == reference.sampleCount_);
        CHECK(std::equal(mean.samples_.begin(), mean.samples_.begin() + mean.sampleCount_, reference.samples_.begin()));
    }
    /** @brief Доводит три реализации до одного logical cut и сразу сравнивает результат. */
    void advance(unsigned count) {
        rendererAdvance(raw, count, false); rendererAdvance(mean, count, true);
        scalar(reference, count, true); compare();
    }
};

/** @brief Группа1: nonlinear mean32, реальный held DAC и отсутствие mean при period2. */
void nonlinearAndHeld() {
    Bus b; Apu a(b); setup(a, b, 0, 0);
    const auto expected = oracleMix(a, true);
    CHECK(a.mixerOutput(true) == expected);
    a.triangle_.sequence = 7; const auto upper = a.mixerOutput(false);
    a.triangle_.sequence = 8; const auto lower = a.mixerOutput(false);
    CHECK(expected != (upper + lower + 1) / 2); // E[f(T)] отличается от f около7.5.
    for (unsigned phase : {0U, 7U, 15U, 16U, 31U}) {
        a.triangle_.sequence = static_cast<std::uint8_t>(phase);
        CHECK(a.mixerOutput(true) == expected);
    }
    a.triangle_.length = 0; CHECK(a.mixerOutput(true) == a.mixerOutput(false));
    a.triangle_.length = 9; a.triangle_.linear = 0;
    CHECK(a.mixerOutput(true) == a.mixerOutput(false));
    a.triangle_.linear = 9; a.triangle_.period = 2;
    CHECK(a.mixerOutput(true) == a.mixerOutput(false));
}

/** @brief Группа2: $400A/B сохраняют старый timer100, entry и pending cuts1..7 точны. */
void oldTimerEntry() {
    for (unsigned period : {0U, 1U}) {
        Trio t(100, 100);
        t.write(0x400a, static_cast<std::uint8_t>(period)); t.write(0x400b, 0x08);
        CHECK(t.mean.triangle_.timer == 100);
        const unsigned entry = 100 - period;
        t.advance(entry - 1); CHECK(!oracleEligible(t.reference));
        const auto sequence = t.reference.triangle_.sequence;
        t.advance(1); CHECK(oracleEligible(t.reference));
        CHECK(t.reference.triangle_.sequence == sequence); // Entry — decrement, не waveform edge.
        for (unsigned count : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 99U, 1024U}) t.advance(count);
    }
}

/** @brief Группа3: sample-before-deposit, entry/frame/DMC/reset и quiet external DAC. */
void boundaryTies(bool quietExpected) {
    for (unsigned tie : {1U, 2U, 4U, 8U}) {
        Trio t(1, 100);
        for (Apu* a : {&t.raw, &t.mean, &t.reference}) {
            a->samplePhase_ = static_cast<unsigned>((a->cpuRate_ - (99ULL * Apu::sampleRate) % a->cpuRate_) % a->cpuRate_);
            if (tie == 2) { a->frameCycle_ = 7457 - 99; a->triangle_.control = 0; a->triangle_.linear = 1; }
            if (tie == 4) {
                a->dmc_.control = 0x80; a->dmc_.timer = 98; a->dmc_.bufferEmpty = false;
                a->dmc_.buffer = 0xa5; a->dmc_.remaining = 3; a->dmc_.bits = 1; a->dmc_.silent = false;
            }
            if (tie == 8) a->write(0x4017, 0x80);
        }
        t.advance(98); t.advance(1); t.advance(1); t.advance(39); t.advance(1024);
    }
    bool admitted = false;
    for (unsigned period : {0U, 1U}) {
        const unsigned entry = 100 - period;
        for (unsigned cut : {entry - 1, entry + 1}) {
            Trio t(period, 100); t.advance(cut);
            for (std::int32_t level : {-95070, 0, -17000}) {
                admitted = t.expansion(level) || admitted;
                t.advance(1); t.advance(7); t.advance(39);
            }
        }
    }
    CHECK(admitted == quietExpected);
}

/** @brief Группа4: старый rank4-хвост живёт15/16/17 samples; wrong-kernel control чувствителен. */
void filterCarryAndWrongKernel() {
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    Bus bus; Apu seed(bus); setup(seed, bus, 0, 0);
    rendererAdvance(seed, 500, false); seed.synchronize(); CHECK(seed.momentCountdown_ == 16);
    Apu actual = seed, reference = seed;
    const auto initial = actual.totalSamples_;
    for (unsigned stop : {15U, 16U, 17U}) {
        while (actual.totalSamples_ - initial < stop) {
            rendererAdvance(actual, 1, true); scalar(reference, 1, true);
        }
        CHECK(digital(actual) == digital(reference));
        CHECK(std::equal(actual.samples_.begin(), actual.samples_.begin() + actual.sampleCount_, reference.samples_.begin()));
        CHECK(actual.momentCountdown_ == (stop == 15 ? 1U : 0U));
    }
    Bus goodBus, wrongBus;
    Apu good(goodBus), wrong(wrongBus); setup(good, goodBus, 0, 0); setup(wrong, wrongBus, 0, 0);
    good.samplePhase_ = wrong.samplePhase_ = good.cpuRate_ / 3;
    scalar(good, 1600, true, true); scalar(wrong, 1600, true, false);
    CHECK(good.sampleCount_ == wrong.sampleCount_);
    CHECK(!std::equal(good.samples_.begin(), good.samples_.begin() + good.sampleCount_, wrong.samples_.begin()));
#endif
}

/** @brief Группа5: period0/1↔audible, disable/re-enable length и настоящий linear-clock. */
void registerTransitions() {
    Trio t(0, 0); t.advance(111);
    for (unsigned p : {1U, 9U, 0U, 1U}) {
        t.write(0x400a, static_cast<std::uint8_t>(p)); t.advance(107);
    }
    t.write(0x4015, 0); t.advance(39); CHECK(t.reference.triangle_.length == 0);
    t.write(0x4015, 4); t.write(0x400b, 0x08); t.advance(39);
    CHECK(t.reference.triangle_.length != 0);
    t.write(0x4008, 0); t.advance(7457); CHECK(t.reference.triangle_.linear == 0);
    t.write(0x4008, 0x89); t.write(0x400b, 0x08); t.advance(7457);
    CHECK(t.reference.triangle_.linear != 0);
}

/** @brief Группа6: defaultfalse остаётся raw, AudioRenderer следует PRIVATE0/1 без macro leak. */
void rawDefaultAndRenderer(bool portableExpected) {
    Bus ab, bb, cb; Apu a(ab), b(bb), c(cb);
    setup(a, ab, 0, 0); setup(b, bb, 0, 0); setup(c, cb, 0, 0);
    a.advanceCpuCycles(700); b.advanceCpuCycles(700, false); scalar(c, 700, false);
#if defined(NES_CORE_APU_TRIANGLE_SEGMENTS) && NES_CORE_APU_TRIANGLE_SEGMENTS
    a.advanceRendererCpuCycles(300); b.advanceRendererCpuCycles(300, false);
#else
    a.advanceCpuCycles(300); b.advanceCpuCycles(300, false);
#endif
    scalar(c, 300, false);
    CHECK(digital(a) == digital(b)); CHECK(digital(b) == digital(c));
    CHECK(std::equal(a.samples_.begin(), a.samples_.begin() + a.sampleCount_, c.samples_.begin()));
    CHECK(std::equal(b.samples_.begin(), b.samples_.begin() + b.sampleCount_, c.samples_.begin()));

    auto tape = std::make_unique<AudioPacket>(); auto sealed = std::make_unique<AudioPacket>();
    auto actual = std::make_unique<AudioRenderer>(); Bus rb; Apu reference(rb); rb.owner = &reference;
    tape->begin(1, 0, 0, Region::Ntsc);
    const std::array<AudioPacket::Input, 4> writes{{{0,0x15,4},{0,0x08,0xbf},{0,0x0a,0},{0,0x0b,0x08}}};
    for (const auto& input : writes) {
        CHECK(tape->addInput(0, input.operation, input.value));
        reference.write(static_cast<std::uint16_t>(0x4000U + input.operation), input.value);
    }
    CHECK(tape->retireTo(*sealed, 9000, 1)); scalar(reference, 9000, portableExpected);
    CHECK(actual->render(*sealed) == AudioRenderResult::Ok);
    CHECK(actual->cycles() == 9000 && actual->sampleCount() == reference.sampleCount_);
    CHECK(std::equal(reference.samples_.begin(), reference.samples_.begin() + reference.sampleCount_, actual->samples()));
}
} // namespace

/** @brief Запускает шесть групп; аргументы0/1 задают PRIVATE Mean и expansion-quiet linked Core. */
int main(int argc, char** argv) {
    CHECK(argc == 3);
    for (unsigned i : {1U, 2U}) { CHECK(argv[i][0] == '0' || argv[i][0] == '1'); CHECK(argv[i][1] == '\0'); }
    nonlinearAndHeld(); oldTimerEntry(); boundaryTies(argv[2][0] == '1'); filterCarryAndWrongKernel();
    registerTransitions(); rawDefaultAndRenderer(argv[1][0] == '1');
    std::printf("triangle_mean groups=6 checks=%u host_FPS_claim=false\n", checks);
}
