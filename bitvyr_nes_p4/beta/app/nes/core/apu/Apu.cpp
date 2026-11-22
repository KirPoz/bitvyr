/**
 * @file Apu.cpp
 * @brief Полный APU, точное расписание DAC-событий и PCM-ресэмплинг.
 * @ingroup nes_apu
 */
#include "Apu.h"

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS && !(defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU)
#include "Machine.h"
#endif

#include <cmath>
#include <algorithm>

namespace nes::core {
namespace {
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
#include "HybridRank4Tables.inl"

#endif
constexpr std::uint8_t lengths[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};
constexpr std::uint16_t noisePeriods[2][16] = {
    {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068},
    {4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778}
};
constexpr std::uint16_t dmcPeriods[2][16] = {
    {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54},
    {398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50}
};
constexpr std::uint8_t duties[4] = {0x02, 0x06, 0x1E, 0xF9};
constexpr double pi = 3.1415926535897932384626433832795;
}  // пространство имён

/**
 * @brief Создаёт звуковой владелец с заимствованным bus-контрактом.
 * @details Состояние каналов сбрасывается, а дальнейшие DMC-чтения/IRQ подчиняются выбранному полному либо временному тракту.
 * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
 */
Apu::Apu(Bus& bus) noexcept : bus_(bus), tables_(sharedTables()) { reset(); }

/**
 * @brief Предоставляет общие неизменяемые таблицы синтеза APU.
 * @details Таблицы создаются один раз и используются всеми экземплярами; вызов не продвигает гостевые такты и не выделяет покадровое хранилище.
 * @return Ссылка на таблицы со статическим сроком жизни.
 */
const Apu::Tables& Apu::sharedTables() noexcept {
    // Создать сразу в единственном конечном хранилище. Стандартная инициализация C++
    // публикует неизменяемые таблицы до возврата любого конкурентного конструктора.
    static const Tables tables;
    return tables;
}

/**
 * @brief Вычисляет неизменяемые коэффициенты смешивания и реконструкции APU.
 * @details Таблицы разделяются через sharedTables; расчёт не зависит от исполнения конкретного ROM.
 */
Apu::Tables::Tables() noexcept {
    pulse[0] = 0;
    for (unsigned p = 1; p < pulse.size(); ++p)
        pulse[p] = static_cast<std::uint16_t>(65535.0 * 95.88 / (8128.0 / p + 100.0) + 0.5);
    for (unsigned triangle = 0; triangle < 16; ++triangle) {
        for (unsigned noise = 0; noise < 16; ++noise) {
            for (unsigned dmc = 0; dmc < 128; ++dmc) {
                const double sum = triangle / 8227.0 + noise / 12241.0 + dmc / 22638.0;
                const double mixed = sum == 0.0 ? 0.0 : 159.79 / (1.0 / sum + 100.0);
                tnd[(noise << 11) | (dmc << 4) | triangle] = static_cast<std::uint16_t>(mixed * 65535.0 + 0.5);
            }
        }
    }
    // Импульс с дробной задержкой и оконным sinc переводит переходы DAC
    // в отсчёты 48 kHz. Интегрирование восстанавливает ступеньки без алиасинга
    // ближайшего отсчёта. Задержка impulseSize/2 отсчётов не зависит от темпа хоста.
    for (unsigned phase = 0; phase < impulsePhases; ++phase) {
        double coefficients[impulseSize]{};
        double sum = 0.0;
        const double center = impulseSize / 2.0 - 1.0 + static_cast<double>(phase) / impulsePhases;
        for (unsigned tap = 0; tap < impulseSize; ++tap) {
            const double x = static_cast<double>(tap) - center;
            const double normalized = x / (impulseSize / 2.0);
            const double window = 0.42 + 0.5 * std::cos(pi * normalized) + 0.08 * std::cos(2.0 * pi * normalized);
            const double sinc = std::abs(x) < 1.0e-12 ? 0.8 : std::sin(0.8 * pi * x) / (pi * x);
            coefficients[tap] = window * sinc;
            sum += coefficients[tap];
        }
        int quantizedSum = 0;
        for (unsigned tap = 0; tap < impulseSize; ++tap) {
            const double coefficient = coefficients[tap] * 32768.0 / sum;
            const int quantized = static_cast<int>(coefficient >= 0.0 ? coefficient + 0.5 : coefficient - 0.5);
            impulse[phase][tap] = static_cast<std::int16_t>(quantized);
            quantizedSum += quantized;
        }
        impulse[phase][impulseSize / 2] = static_cast<std::int16_t>(impulse[phase][impulseSize / 2] + 32768 - quantizedSum);
    }
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 */
void Apu::reset(bool pal) noexcept {
    expansionLevel_=0;
    pulses_.fill(Pulse{});
    triangle_ = Triangle{};
    noise_ = Noise{};
    dmc_ = Dmc{};
    pendingImpulse_.fill(0);
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    momentHistory_ = {}; momentCountdown_ = 0;
#endif
    samples_.fill(0);
    cycles_ = totalSamples_ = droppedSamples_ = 0;
    sampleCount_ = 0;
    frameCycle_ = samplePhase_ = lastMix_ = 0;
    integrator_ = 0;
    impulsePosition_ = frameResetDelay_ = 0;
    pendingCycles_ = 0;
    eventCountdown_ = 1;
    pal_ = pal;
    cpuRate_ = pal ? 1662607 : 1789773;
    fiveStep_ = irqInhibit_ = frameIrq_ = false;
    highPass90_ = highPass440_ = lowPass14000_ = previousInput_ = previousHighPass90_ = 0;
}

/**
 * @brief Выполняет один шаг часов соответствующего устройства.
 * @details Единица времени определяется классом владельца; метод изменяет внутренние делители и события, а не системное время ESP-IDF.
 */
void Apu::Envelope::clock() noexcept {
    if (start) { start = false; decay = 15; divider = control & 15; }
    else if (divider) --divider;
    else {
        divider = control & 15;
        if (decay) --decay;
        else if (control & 0x20) decay = 15;
    }
}

/**
 * @brief Обслуживает четвертькадровые envelope и linear-counter события.
 * @details Меняет состояние каналов, но не означает выдачу готового блока PCM.
 */
void Apu::quarterFrame() noexcept {
    for (auto& pulse : pulses_) pulse.envelope.clock();
    noise_.envelope.clock();
    if (triangle_.reload) triangle_.linear = triangle_.control & 0x7F;
    else if (triangle_.linear) --triangle_.linear;
    if (!(triangle_.control & 0x80)) triangle_.reload = false;
}

/**
 * @brief Вычисляет целевой период pulse sweep с особенностью отрицания канала.
 * @details Результат используется проверкой допустимости, а не немедленной записью нового периода.
 * @param channel Индекс pulse-канала 0 или 1; определяет разницу negate carry в sweep.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Apu::sweepTarget(unsigned channel) const noexcept {
    const auto& pulse = pulses_[channel];
    const int change = pulse.period >> (pulse.sweep & 7);
    int target = pulse.period;
    if (pulse.sweep & 8) target -= change + (channel == 0 ? 1 : 0);
    else target += change;
    return target < 0 ? 0 : static_cast<unsigned>(target);
}

/**
 * @brief Обслуживает полукадровые length/sweep события каналов.
 * @details Обновление делителей и запрещённых sweep-целей выполняется в исходном порядке.
 */
void Apu::halfFrame() noexcept {
    for (unsigned channel = 0; channel < 2; ++channel) {
        auto& pulse = pulses_[channel];
        if (pulse.length && !(pulse.envelope.control & 0x20)) --pulse.length;
        const unsigned target = sweepTarget(channel);
        if (pulse.sweepDivider == 0 && (pulse.sweep & 0x80) && (pulse.sweep & 7) && pulse.period >= 8 && target <= 0x7FF)
            pulse.period = static_cast<std::uint16_t>(target);
        if (pulse.sweepDivider == 0 || pulse.sweepReload) {
            pulse.sweepDivider = (pulse.sweep >> 4) & 7;
            pulse.sweepReload = false;
        } else --pulse.sweepDivider;
    }
    if (triangle_.length && !(triangle_.control & 0x80)) --triangle_.length;
    if (noise_.length && !(noise_.envelope.control & 0x20)) --noise_.length;
}

/**
 * @brief Продвигает последовательность quarter/half-frame событий APU.
 * @details Сохраняет NTSC/PAL-расписание, режим четырёх/пяти шагов и семантику IRQ.
 */
void Apu::frameCounter() noexcept {
    if (frameResetDelay_) {
        if (--frameResetDelay_ == 0) {
            frameCycle_ = 0;
            if (fiveStep_) { quarterFrame(); halfFrame(); }
            return;
        }
    }
    ++frameCycle_;
    const unsigned first = pal_ ? 8313 : 7457;
    const unsigned second = pal_ ? 16627 : 14913;
    const unsigned third = pal_ ? 24939 : 22371;
    const unsigned fourth = pal_ ? 33253 : 29829;
    const unsigned fifth = pal_ ? 41565 : 37281;
    if (frameCycle_ == first || frameCycle_ == third) quarterFrame();
    else if (frameCycle_ == second) { quarterFrame(); halfFrame(); }
    if (fiveStep_) {
        if (frameCycle_ == fifth) { quarterFrame(); halfFrame(); }
        if (frameCycle_ >= fifth + 1) frameCycle_ = 0;
    } else {
        if (frameCycle_ >= fourth - 1 && frameCycle_ <= fourth + 1 && !irqInhibit_) frameIrq_ = true;
        if (frameCycle_ == fourth) { quarterFrame(); halfFrame(); }
        if (frameCycle_ >= fourth + 1) frameCycle_ = 0;
    }
}

/**
 * @brief Запрашивает либо перезапускает фактическую выборку байта DMC.
 * @details Чтение и украденные CPU-такты согласуются через bus-контракт; значение не генерируется по памяти renderer произвольно.
 */
void Apu::restartDmc() noexcept {
    dmc_.address = 0xC000 | (static_cast<unsigned>(dmc_.start) << 6);
    dmc_.remaining = static_cast<std::uint16_t>(static_cast<unsigned>(dmc_.length) * 16 + 1);
}

/**
 * @brief Запрашивает либо перезапускает фактическую выборку байта DMC.
 * @details Чтение и украденные CPU-такты согласуются через bus-контракт; значение не генерируется по памяти renderer произвольно.
 */
void Apu::fetchDmc() noexcept {
    if (!dmc_.bufferEmpty || !dmc_.remaining) return;
    dmc_.buffer = bus_.dmcRead(dmc_.address);
    bus_.dmcStall(4);
    dmc_.bufferEmpty = false;
    if (++dmc_.address == 0) dmc_.address = 0x8000;
    if (--dmc_.remaining == 0) {
        if (dmc_.control & 0x40) restartDmc();
        else if (dmc_.control & 0x80) dmc_.irq = true;
    }
}

/**
 * @brief Продвигает внутренние таймеры звуковых каналов.
 * @details Чередование CPU-фазы pulse/noise и каждый такт triangle/DMC сохраняются.
 */
void Apu::clockTimers() noexcept {
    if ((cycles_ & 1) == 0) {
        for (auto& pulse : pulses_) {
            if (pulse.timer) --pulse.timer;
            else { pulse.timer = pulse.period; pulse.sequence = (pulse.sequence + 1) & 7; }
        }
    }
    if (triangle_.timer) --triangle_.timer;
    else {
        triangle_.timer = triangle_.period;
        if (triangle_.length && triangle_.linear) triangle_.sequence = (triangle_.sequence + 1) & 31;
    }
    if (noise_.timer) --noise_.timer;
    else {
        noise_.timer = noisePeriods[pal_ ? 1 : 0][noise_.control & 15] - 1;
        const unsigned tap = (noise_.control & 0x80) ? 6 : 1;
        const unsigned feedback = (noise_.shift ^ (noise_.shift >> tap)) & 1;
        noise_.shift = static_cast<std::uint16_t>((noise_.shift >> 1) | (feedback << 14));
    }
    if (dmc_.timer) --dmc_.timer;
    else {
        dmc_.timer = dmcPeriods[pal_ ? 1 : 0][dmc_.control & 15] - 1;
        if (!dmc_.silent) {
            if ((dmc_.shift & 1) && dmc_.output <= 125) dmc_.output += 2;
            else if (!(dmc_.shift & 1) && dmc_.output >= 2) dmc_.output -= 2;
        }
        dmc_.shift >>= 1;
        if (--dmc_.bits == 0) {
            dmc_.bits = 8;
            dmc_.silent = dmc_.bufferEmpty;
            if (!dmc_.bufferEmpty) { dmc_.shift = dmc_.buffer; dmc_.bufferEmpty = true; }
        }
    }
}

/**
 * @brief Вычисляет текущий уровень pulse-канала из его таймера, duty и envelope.
 * @details Запрещённые длина и sweep-цель дают тот же заглушённый уровень; метод не читает CPU-память.
 * @param channel Индекс pulse-канала 0 или 1; определяет разницу negate carry в sweep.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Apu::pulseOutput(unsigned channel) const noexcept {
    const auto& pulse = pulses_[channel];
    if (!pulse.length || pulse.period < 8 || sweepTarget(channel) > 0x7FF) return 0;
    if (!(duties[pulse.envelope.control >> 6] & (1U << pulse.sequence))) return 0;
    return pulse.envelope.output();
}

/**
 * @brief Смешивает текущие уровни NES-каналов по используемым нелинейным таблицам.
 * @details Это модель цифрового звукового тракта; аппаратный DAC платы и громкость находятся вне ядра.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
std::int32_t Apu::mixerOutput(bool triangleMean) const noexcept {
    const unsigned triangle = triangle_.sequence < 16 ? 15 - triangle_.sequence : triangle_.sequence - 16;
    const unsigned noise = noise_.length && !(noise_.shift & 1) ? noise_.envelope.output() : 0;
    const auto* column = tables_.tnd.data() + (noise << 11U) + (unsigned(dmc_.output) << 4U);
    unsigned tnd = column[triangle];
    if (triangleMean && triangleMeanEligible()) {
        // Каждый DAC0..15 встречается дважды за32 фазы; усредняем ПОСЛЕ nonlinearTND.
        // uint32 вмещает16*65535; округление среднего не требует таблицы/состояния.
        std::uint32_t sum = 0;
        for (unsigned t = 0; t < 16; ++t) sum += column[t];
        tnd = (sum + 8U) >> 4U;
    }
    return static_cast<std::int32_t>(tables_.pulse[pulseOutput(0) + pulseOutput(1)] + tnd) + expansionLevel_;
}

#if (defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) || (defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#elif defined(_MSC_VER)
/**
 * @brief Вносит ступень изменения звукового уровня в фазовый импульсный накопитель.
 * @details Положение определяется CPU-временем и фазой дискретизации; вклад не меняет последовательность регистровых событий.
 * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
 * @param phase Фаза FIR0..63; индексирует коэффициенты дробной задержки, не PPU-фазу.
 */
__forceinline
#else
inline
#endif
/** @details Распределяет точный целочисленный DAC-фронт по FIR-коэффициентам исходной дробной sample-фазы. */
void Apu::depositDeltaAtPhase(std::int32_t delta, unsigned phase, [[maybe_unused]] bool triangleMean) noexcept {
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    if (!(triangleMean && triangleMeanEligible()) && momentSelected()) { depositMomentAtPhase(delta, phase); return; }
#endif
#include "ImpulseDeposit.inl"
}
#endif

#if defined(NES_CORE_APU_TRIANGLE_SCRATCH) && NES_CORE_APU_TRIANGLE_SCRATCH
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#elif defined(_MSC_VER)
/**
 * @brief Накапливает ограниченный вклад triangle в выбранной фазе звукового реконструктора.
 * @details Сохраняет моменты изменения уровня; временное хранилище не является отдельным аудиопотоком.
 * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
 * @param phase Фаза FIR-импульса 0..63 для дробного положения DAC-перехода между PCM-отсчётами.
 * @param scratch Изменяемый временный накопитель вклада сегмента; размер задан FIR-контрактом, заимствуется на вызов.
 */
__forceinline
#else
inline
#endif
/** @details Суммирует разрешённый треугольный участок в принадлежащем renderer scratch без нового heap. */
void Apu::depositTriangleScratch(std::int32_t delta, unsigned phase, std::int32_t* scratch) noexcept {
    // Только ПОСЛЕ первого CPU-такта: постоянный pulse/noise/DMC-контекст делает
    // каждую дельту соседним шагом triangle-TND с модулем <=1258. Прежние
    // границы без пересечения отсчёта допускают максимум 36 членов, включая каждый префикс.
    static_assert(36LL * 1258 * 32768 < 2147483647LL, "Scratch prefix must fit signed32");
    const auto* coefficient = tables_.impulse[phase].data();
    auto* const end = scratch + impulseSize;
#if defined(__clang__)
#pragma clang loop unroll(disable)
#elif defined(__GNUC__)
#pragma GCC unroll 1
#endif
    do {
        scratch[0] += delta * static_cast<std::int32_t>(coefficient[0]);
        scratch[1] += delta * static_cast<std::int32_t>(coefficient[1]);
        scratch[2] += delta * static_cast<std::int32_t>(coefficient[2]);
        scratch[3] += delta * static_cast<std::int32_t>(coefficient[3]);
        scratch += 4;
        coefficient += 4;
    } while (scratch != end);
}
#endif

#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#elif defined(_MSC_VER)
/**
 * @brief Накапливает ограниченный вклад triangle в выбранной фазе звукового реконструктора.
 * @details Сохраняет моменты изменения уровня; временное хранилище не является отдельным аудиопотоком.
 * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
 * @param phase Фаза FIR0..63; индексирует коэффициенты дробной задержки, не PPU-фазу.
 */
__forceinline
#else
inline
#endif
/** @details Вносит момент ограниченного фронта в прежний FIR с неизменной фазой отсчёта. */
void Apu::depositMomentAtPhase(std::int32_t delta, unsigned phase) noexcept {
    static_assert(65535LL * 16384 < 2147483647LL, "Delta/weight product fits int32");
    // Ненулевая полная нелинейная смешанная дельта относится ровно к ОДНОЙ истории.
    if(delta>65535 || delta< -65535){
        for(unsigned j=0;j<4;++j)momentHistory_[j][impulsePosition_]+=static_cast<std::int64_t>(delta)*hybridWeights[phase][j];
    } else {
    for (unsigned j = 0; j < 4; ++j)
        momentHistory_[j][impulsePosition_] += static_cast<std::int64_t>(delta * static_cast<std::int32_t>(hybridWeights[phase][j]));
    }
    momentCountdown_ = 16;
}
#endif

/**
 * @brief Вносит ступень изменения звукового уровня в фазовый импульсный накопитель.
 * @details Положение определяется CPU-временем и фазой дискретизации; вклад не меняет последовательность регистровых событий.
 * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
 */
void Apu::depositDelta(std::int32_t delta, [[maybe_unused]] bool triangleMean) noexcept {
    // samplePhase < cpuRate <= 1789773, поэтому точное произведение помещается в uint32.
    // Избежать программного 64-битного деления в RV32-пути аудиоперехода.
    static_assert(1789773ULL * impulsePhases < (1ULL << 32), "Resampler phase product must fit uint32");
    const unsigned phase = (samplePhase_ * impulsePhases) / cpuRate_;
#if (defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) || (defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4)
    depositDeltaAtPhase(delta, phase, triangleMean);
#else
#include "ImpulseDeposit.inl"
#endif
}

/**
 * @brief Выдаёт очередной PCM-отсчёт в ограниченный внутренний буфер APU.
 * @details Переполнение учитывается, а частота дискретизации выводится из гостевого времени.
 */
void Apu::emitSample() noexcept {
    // Каждая половина содержит непересекающиеся вклады в один логический слот.
    // Их сумма точно равна прежнему значению кольца по модулю.
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    // Оба ядра имеют DC-сумму 2^29. Сохранить ОДИН Q29-интеграл при
    // смене выбора. Не сдвигать влево отрицательный знаковый плотный импульс.
    std::int64_t impulse = (pendingImpulse_[impulsePosition_] + pendingImpulse_[impulsePosition_ + impulseSize]) * std::int64_t(16384);
    if (momentCountdown_) for (unsigned j = 0; j < 4; ++j)
        for (unsigned lag = 0; lag < 16; ++lag)
            impulse += momentHistory_[j][(impulsePosition_ + 16 - lag) & 15] * static_cast<std::int32_t>(hybridBasis[j][lag]);
    integrator_ += impulse;
#else
    integrator_ += pendingImpulse_[impulsePosition_] + pendingImpulse_[impulsePosition_ + impulseSize];
#endif
    pendingImpulse_[impulsePosition_] = pendingImpulse_[impulsePosition_ + impulseSize] = 0;
    impulsePosition_ = (impulsePosition_ + 1) & (impulseSize - 1);
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    if (momentCountdown_) {
        for (auto& history : momentHistory_) history[impulsePosition_] = 0;
        --momentCountdown_; // Свернуть 16-й хвост ДО достижения нуля.
    }
    const auto outputIntegral = integrator_ / 16384;
    const float integrated = outputIntegral >= (-2147483647LL - 1) && outputIntegral <= 2147483647LL
        ? static_cast<float>(static_cast<std::int32_t>(outputIntegral))
        : static_cast<float>(outputIntegral);
#else
    const float integrated = integrator_ >= (-2147483647LL - 1) && integrator_ <= 2147483647LL
        ? static_cast<float>(static_cast<std::int32_t>(integrator_))
        : static_cast<float>(integrator_);
#endif
    const float input = integrated * (1.0F / (65535.0F * 32768.0F));
    highPass90_ = 0.988288F * (highPass90_ + input - previousInput_);
    previousInput_ = input;
    highPass440_ = 0.944032F * (highPass440_ + highPass90_ - previousHighPass90_);
    previousHighPass90_ = highPass90_;
    lowPass14000_ += 0.840002F * (highPass440_ - lowPass14000_);
    float scaled = lowPass14000_ * 30000.0F;
    if (scaled > 32767.0F) scaled = 32767.0F;
    if (scaled < -32768.0F) scaled = -32768.0F;
    ++totalSamples_;
    if (sampleCount_ < samples_.size()) samples_[sampleCount_++] = static_cast<std::int16_t>(scaled);
    else ++droppedSamples_;
}

/**
 * @brief Компонует продвижение noise LFSR на заданное число шагов.
 * @details Применяется только в текущем доказанном интервале; регистровые записи остаются отдельными границами.
 * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 * @param noise Изменяемое состояние noise-таймера/LFSR; переданное число CPU-тактов не пересекает исключённые события.
 */
void Apu::advanceNoise(Noise& noise, unsigned count, bool pal) noexcept {
    const unsigned period = noisePeriods[pal ? 1 : 0][noise.control & 15];
    while (count > noise.timer) {
        count -= noise.timer + 1;
        noise.timer = static_cast<std::uint16_t>(period - 1);
        const unsigned tap = (noise.control & 0x80) ? 6 : 1;
        const unsigned feedback = (noise.shift ^ (noise.shift >> tap)) & 1;
        noise.shift = static_cast<std::uint16_t>((noise.shift >> 1) | (feedback << 14));
    }
    noise.timer = static_cast<std::uint16_t>(noise.timer - count);
}

/**
 * @brief Компонует интервал каналов APU, в котором не нужны отдельные внешние обращения.
 * @details Сохраняет те же конечные таймеры/уровни и реконструкционный вклад; регистровые события остаются границами.
 * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
 * @param startCycle Абсолютный гостевой CPU-такт начала уже разрешённого аудиоинтервала.
 */
void Apu::advanceSkipped(unsigned count, std::uint64_t startCycle) noexcept {
    if (!count) return;
    // Планировщик гарантирует отсутствие фронта кадра, отсчёта, активного DAC-фронта или
    // DMC-выборки внутри. Скрытые фазы генераторов по-прежнему продвигаются точно.
    frameCycle_ += count;
    if (frameResetDelay_) frameResetDelay_ = static_cast<std::uint8_t>(frameResetDelay_ - count);
    samplePhase_ += count * sampleRate;
    const unsigned pulseTicks = (count + static_cast<unsigned>(startCycle & 1)) / 2;
    for (auto& pulse : pulses_) {
        if (pulseTicks <= pulse.timer) pulse.timer = static_cast<std::uint16_t>(pulse.timer - pulseTicks);
        else {
            const unsigned elapsed = pulseTicks - pulse.timer - 1;
            unsigned edges = 1;
            if (pulse.period == 0) { edges += elapsed; pulse.timer = 0; }
            else if (elapsed <= pulse.period) pulse.timer = static_cast<std::uint16_t>(pulse.period - elapsed);
            else {
                const unsigned period = pulse.period + 1;
                edges += elapsed / period;
                pulse.timer = static_cast<std::uint16_t>(pulse.period - elapsed % period);
            }
            pulse.sequence = static_cast<std::uint8_t>((pulse.sequence + edges) & 7);
        }
    }
    if (count <= triangle_.timer) triangle_.timer = static_cast<std::uint16_t>(triangle_.timer - count);
    else {
        const unsigned elapsed = count - triangle_.timer - 1;
        unsigned edges = 1;
        if (triangle_.period == 0) { edges += elapsed; triangle_.timer = 0; }
        else if (elapsed <= triangle_.period) triangle_.timer = static_cast<std::uint16_t>(triangle_.period - elapsed);
        else {
            const unsigned period = triangle_.period + 1;
            edges += elapsed / period;
            triangle_.timer = static_cast<std::uint16_t>(triangle_.period - elapsed % period);
        }
        if (triangle_.length && triangle_.linear)
            triangle_.sequence = static_cast<std::uint8_t>((triangle_.sequence + edges) & 31);
    }
    advanceNoise(noise_, count, pal_);
    unsigned dmcTicks = count;
    while (dmcTicks > dmc_.timer) {
        dmcTicks -= dmc_.timer + 1;
        dmc_.timer = dmcPeriods[pal_ ? 1 : 0][dmc_.control & 15] - 1;
        // У несilent/буферизованного DMC следующий фронт уже включён в расписание,
        // поэтому скрытый фазовый путь видит только пустой молчащий сдвигатель.
        dmc_.shift >>= 1;
        if (--dmc_.bits == 0) { dmc_.bits = 8; dmc_.silent = true; }
    }
    dmc_.timer = static_cast<std::uint16_t>(dmc_.timer - dmcTicks);
}

/**
 * @brief Материализует накопленное время до следующего значимого события устройства.
 * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
 */
void Apu::synchronize() noexcept {
    if (pendingCycles_) {
        cycles_ += pendingCycles_;
        advanceSkipped(pendingCycles_, cycles_ - pendingCycles_);
        pendingCycles_ = 0;
    }
}

/**
 * @brief Сохраняет старую временную границу до изменения внешнего DAC.
 * @details synchronize не пересекает будущий tick: pending интервал уже прошёл
 * без native событий. Новый уровень наблюдается tickEvent на ближайшем CPU
 * такте в том же порядке sample-before-delta, что и обычная запись APU.
 */
void Apu::setExpansionInput(std::int32_t value) noexcept {
    if (value == expansionLevel_) return;
    synchronize();
    expansionLevel_ = value;
    eventCountdown_ = 1;
}

/**
 * @brief Убирает принудительный native tick только для изолированного DAC-фронта.
 * @details При deadline>1 следующий tick не меняет native mixer и не выдаёт PCM.
 * Поэтому его единственная смешанная дельта равна внешней дельте: её можно
 * внести сейчас в ту же FIR-фазу, сохранив native расписание и уже вычисленный
 * lastMix. Исключения timestamp проверяет AudioRenderer до вызова; обычные
 * Machine/setExpansionInput и каждый граничный случай сохраняют старое тело.
 */
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/** @details Принимает только доказанный expansion-only интервал; при сомнении caller использует исходный setter/tick. */
bool Apu::trySetExpansionInputQuiet(std::int32_t value, [[maybe_unused]] bool triangleMean) noexcept {
#if defined(NES_CORE_APU_EXPANSION_QUIET) && NES_CORE_APU_EXPANSION_QUIET
    if (eventCountdown_ <= 1 || value == expansionLevel_) return false;
    synchronize();
    const auto delta = value - expansionLevel_;
    expansionLevel_ = value;
    lastMix_ += delta;
    // До U+1 нет sample edge: позиция кольца и выбранный native kernel прежние.
    // Меняется лишь аргумент дробной задержки, а не исполненное гостевое время.
    samplePhase_ += sampleRate;
    depositDelta(delta, triangleMean);
    samplePhase_ -= sampleRate;
    return true;
#else
    (void)value;
    return false;
#endif
}

/**
 * @brief Вычисляет расстояние до ближайшего события текущего временного APU.
 * @details Отсчёт выражен в CPU-тактах и ограничивает быстрый advance.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Apu::nextEventDistance(bool triangleMean) const noexcept {
    // Активный triangle с нулевым периодом меняет DAC каждый CPU-такт. Оставить
    // этот намеренно ультразвуковой случай на простом однотактовом пути.
    if (!(triangleMean && triangleMeanEligible()) && !triangle_.period && !triangle_.timer && triangle_.length && triangle_.linear) return 1;
    if (dmc_.bufferEmpty && dmc_.remaining) return 1;
    unsigned distance = (cpuRate_ - samplePhase_ + sampleRate - 1) / sampleRate;
    /**
     * @brief Уменьшает локальный горизонт до следующей границы frame-sequencer относительно frameCycle_; саму границу не исполняет.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto frameEdge = [this, &distance](unsigned edge) {
        if (edge > frameCycle_) distance = std::min(distance, static_cast<unsigned>(edge - frameCycle_));
    };
    frameEdge(pal_ ? 8313 : 7457);
    frameEdge(pal_ ? 16627 : 14913);
    frameEdge(pal_ ? 24939 : 22371);
    if (fiveStep_) {
        frameEdge(pal_ ? 41565 : 37281);
        frameEdge(pal_ ? 41566 : 37282);
    } else {
        const unsigned fourth = pal_ ? 33253 : 29829;
        frameEdge(fourth - 1); frameEdge(fourth); frameEdge(fourth + 1);
    }
    if (frameResetDelay_) distance = std::min(distance, unsigned(frameResetDelay_));
    const unsigned firstEven = (cycles_ & 1) ? 1 : 2;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& pulse = pulses_[i];
        if (pulse.length && pulse.period >= 8 && pulse.envelope.output() && sweepTarget(i) <= 0x7FF)
            distance = std::min(distance, firstEven + 2 * pulse.timer);
    }
    if (triangle_.length && triangle_.linear && !(triangleMean && triangleMeanEligible())) {
        // Entry в mean может наступить НА decrement раньше raw-edge: timer100→1 приperiod1.
        const unsigned edge = triangleMean && triangle_.period <= 1U && triangle_.timer > triangle_.period
            ? unsigned(triangle_.timer - triangle_.period) : unsigned(triangle_.timer) + 1U;
        distance = std::min(distance, edge);
    }
    if (noise_.length && noise_.envelope.output()) distance = std::min(distance, unsigned(noise_.timer) + 1);
    if (dmc_.bufferEmpty && dmc_.remaining) distance = 1;
    else if (dmc_.remaining || !dmc_.bufferEmpty || !dmc_.silent)
        distance = std::min(distance, unsigned(dmc_.timer) + 1);
    return distance;
}

/**
 * @brief Материализует накопленное время до следующего значимого события устройства.
 * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
 */
void Apu::tickEvent(bool triangleMean) noexcept {
    cycles_ += static_cast<unsigned>(pendingCycles_) + 1;
    if (pendingCycles_) {
        advanceSkipped(pendingCycles_, cycles_ - pendingCycles_ - 1);
        pendingCycles_ = 0;
    }
    frameCounter();
    fetchDmc();
    clockTimers();
    samplePhase_ += sampleRate;
    if (samplePhase_ >= cpuRate_) { samplePhase_ -= cpuRate_; emitSample(); }
    const auto mixed = mixerOutput(triangleMean);
    if (mixed != lastMix_) {
        depositDelta(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(lastMix_), triangleMean);
        lastMix_ = mixed;
    }
    eventCountdown_ = static_cast<std::uint8_t>(nextEventDistance(triangleMean));
}

#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
/**
 * @brief Возвращает допустимый CPU-интервал до следующего аудиособытия.
 * @details Горизонт ограничивает отложенные clocks; не даёт права терять DMC-выборки или побочные эффекты.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Apu::asyncQuietCpuCycles() const noexcept {
    // Reset/refill может сразу изменить CPU-наблюдаемую линию или запросить
    // украденные такты. Оставить их на прежнем однотактовом пути Machine.
    if (frameResetDelay_ || (dmc_.bufferEmpty && dmc_.remaining)) return 0;
    const unsigned frame = frameCycle_ + pendingCycles_;
    const unsigned last = fiveStep_ ? (pal_ ? 41566U : 37282U) : (pal_ ? 33254U : 29830U);
    if (frame >= last) return 0;
    unsigned distance = last - frame;
    /**
     * @brief Уменьшает локальный горизонт до будущего frame-sequencer-события, сохраняя нулевые/уже пройденные границы внешнему пути.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto edge = [frame, &distance](unsigned when) {
        if (when > frame) distance = std::min(distance, when - frame);
    };
    // Сохранить каждую границу frame-sequencer, уже запланированную
    // nextEventDistance, включая запрещённые и уже установленные IRQ.
    edge(pal_ ? 8313U : 7457U);
    edge(pal_ ? 16627U : 14913U);
    edge(pal_ ? 24939U : 22371U);
    if (fiveStep_) edge(last - 1U);
    else { edge(last - 2U); edge(last - 1U); }
    unsigned quiet = distance - 1U;
    if (dmc_.remaining || !dmc_.bufferEmpty || !dmc_.silent) {
        // Накопленные такты не пересекли активный фронт таймера DMC. Ограничить
        // интервал до следующего фронта, даже если он не вызывает refill/IRQ.
        if (pendingCycles_ > dmc_.timer) return 0;
        quiet = std::min(quiet, unsigned(dmc_.timer) - pendingCycles_);
    }
    return quiet;
}

/**
 * @brief Продвигает APU на точное число CPU-тактов через его текущие события.
 * @details Сохраняет делители таймеров, frame sequencer, DMC и границы накопления PCM.
 * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
 */
void Apu::advanceCpuCycles(unsigned count, bool triangleMean) noexcept {
    while (count) {
        const unsigned distance = eventCountdown_; // Всегда >=1 после reset/event/write.
        if (count < distance) {
            pendingCycles_ = static_cast<std::uint8_t>(pendingCycles_ + count);
            eventCountdown_ = static_cast<std::uint8_t>(distance - count);
            return;
        }
        // Один интервал планировщика заканчивается отсчётом или более ранним DAC/frame/
        // DMC-событием, поэтому pending+distance-1 остаётся <256. Не откладывать через
        // событие: выполнить неизменённое скалярное тело в его исходном порядке.
        pendingCycles_ = static_cast<std::uint8_t>(pendingCycles_ + distance - 1U);
        eventCountdown_ = 0;
        count -= distance;
        tickEvent(triangleMean);
    }
}
#if defined(NES_CORE_APU_TRIANGLE_SEGMENTS) && NES_CORE_APU_TRIANGLE_SEGMENTS
// Только renderer повторно использует проверенный triangle-сегмент. Обычный
// advanceCpuCycles машины и каждая FIR-операция остаются неизменными.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
/**
 * @brief Продвигает соответствующий аудиовладелец на заданное число CPU-тактов.
 * @details Объединение не пересекает наблюдаемое IRQ/DMC-событие и не исполняет CPU повторно.
 * @param count Длина допустимого интервала в гостевых CPU-тактах.
 */
__declspec(noinline)
#endif
/** @details Продвигает renderer на реальные CPU-такты с сохранением ближайших sample/frame/DMC deadlines. */
void Apu::advanceRendererCpuCycles(unsigned count, bool triangleMean) noexcept {
#if defined(NES_CORE_APU_RENDERER_DEADLINES) && NES_CORE_APU_RENDERER_DEADLINES
#include "RendererDeadline.inl"
#endif
    if (triangleMean) {
        // Короткий/reset boundary использует ТОТ ЖЕ policy; raw period0 segment запрещён.
        advanceCpuCycles(count, triangleMean);
        return;
    }
    // Period/length не становятся допустимыми без пакетного I/O регистров.
    // Сохранить обычный планировщик для несвязанных форм волны.
    if (count < 8 || triangle_.period || !triangle_.length) {
        advanceCpuCycles(count);
        return;
    }
#if defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE
    // Регион неизменен внутри пакета. Выбрать точные константы
    // один раз, без деления фиксированного шага внутри CPU-тактового цикла.
    constexpr unsigned stepNumerator = sampleRate * impulsePhases;
    const unsigned stepQ = pal_ ? stepNumerator / 1662607U : stepNumerator / 1789773U;
    const unsigned stepR = pal_ ? stepNumerator % 1662607U : stepNumerator % 1789773U;
#endif
    while (count) {
        unsigned segment = 0;
        if (count >= 8 && !pendingCycles_ && eventCountdown_ == 1 &&
            !triangle_.period && !triangle_.timer && triangle_.length && triangle_.linear) {
            segment = std::min(count, asyncQuietCpuCycles());
            const unsigned sampleEdge = (cpuRate_ - samplePhase_ + sampleRate - 1) / sampleRate;
            segment = std::min(segment, sampleEdge - 1U);
            const unsigned firstEven = (cycles_ & 1) ? 1 : 2;
            for (unsigned i = 0; i < 2; ++i) {
                const auto& pulse = pulses_[i];
                if (pulse.length && pulse.period >= 8 && pulse.envelope.output() && sweepTarget(i) <= 0x7FF)
                    segment = std::min(segment, firstEven + 2U * pulse.timer - 1U);
            }
            if (noise_.length && noise_.envelope.output())
                segment = std::min(segment, unsigned(noise_.timer));
        }
        if (segment >= 8) {
            // Внутри нет frame/sample/reset/DMC или другого активного DAC-фронта.
            // Скрытые фазы фиксируются один раз; callback не наблюдает их.
            const unsigned initialPhase = samplePhase_;
#if defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE
            // Одно деление DIV; восстановление остатка точно в uint32.
            const unsigned numerator = initialPhase * impulsePhases;
            unsigned phaseIndex = numerator / cpuRate_;
            unsigned phaseRemainder = numerator - phaseIndex * cpuRate_;
#endif
            unsigned sequence = triangle_.sequence;
            const unsigned pulse = tables_.pulse[pulseOutput(0) + pulseOutput(1)];
            const unsigned noise = noise_.length && !(noise_.shift & 1) ? noise_.envelope.output() : 0;
            // TND_LAYOUT_LOOKUP_RENDERER_TRIANGLE_SEGMENT: текущее транспонированное
            // расположение noise[4]/DMC[7]/triangle[4]; проверять все места lookup.
            const auto* column = tables_.tnd.data() + (noise << 11U) + (unsigned(dmc_.output) << 4U);
            const auto startCycle = cycles_;
            cycles_ += segment;
            advanceSkipped(segment, startCycle);
            samplePhase_ = initialPhase;
            auto mixedBefore = lastMix_;
#if defined(NES_CORE_APU_TRIANGLE_SCRATCH) && NES_CORE_APU_TRIANGLE_SCRATCH && !(defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4)
            std::array<std::int32_t, impulseSize> scratch{};
#endif
            for (unsigned i = 0; i < segment; ++i) {
                sequence = (sequence + 1U) & 31U;
                samplePhase_ += sampleRate;
#if defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE
                phaseIndex += stepQ;
                phaseRemainder += stepR;
                if (phaseRemainder >= cpuRate_) { phaseRemainder -= cpuRate_; ++phaseIndex; }
#endif
                const unsigned triangle = sequence < 16 ? 15 - sequence : sequence - 16;
                const auto mixed = static_cast<std::int32_t>(pulse + column[triangle]) + expansionLevel_;
                if (mixed != mixedBefore) {
#if defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
                    depositMomentAtPhase(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(mixedBefore), phaseIndex);
#elif defined(NES_CORE_APU_TRIANGLE_SCRATCH) && NES_CORE_APU_TRIANGLE_SCRATCH
                    // ПЕРВЫЙ CPU-такт может быть произвольным скачком после записи
                    // регистра. Даже при нулевой дельте последующие такты уже
                    // соседние шаги волны; не откладывать первый ненулевой.
                    if (i == 0) depositDeltaAtPhase(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(mixedBefore), phaseIndex);
                    else depositTriangleScratch(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(mixedBefore), phaseIndex, scratch.data());
#else
                    depositDeltaAtPhase(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(mixedBefore), phaseIndex);
#endif
#else
                    depositDelta(static_cast<std::int32_t>(mixed) - static_cast<std::int32_t>(mixedBefore));
#endif
                    mixedBefore = mixed;
                }
            }
#if defined(NES_CORE_APU_TRIANGLE_SCRATCH) && NES_CORE_APU_TRIANGLE_SCRATCH && !(defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4)
            // Внутри сегмента нет emit/callback/observer. Объединить вклады в
            // те же непрерывные физические назначения зеркального кольца
            // до следующего отсчёта, пакетного I/O или возврата метода.
            auto* const pending = pendingImpulse_.data() + impulsePosition_;
            for (unsigned tap = 0; tap < impulseSize; ++tap)
                pending[tap] += static_cast<std::int64_t>(scratch[tap]);
#endif
            lastMix_ = mixedBefore;
            count -= segment;
            continue;
        }
        // Пересечь каждый исключённый фронт в прежнем порядке, включая выдачу отсчёта
        // до внесения смешанного DAC-вклада на том же CPU-такте.
        const unsigned distance = eventCountdown_;
        if (count < distance) {
            pendingCycles_ = static_cast<std::uint8_t>(pendingCycles_ + count);
            eventCountdown_ = static_cast<std::uint8_t>(distance - count);
            return;
        }
        pendingCycles_ = static_cast<std::uint8_t>(pendingCycles_ + distance - 1U);
        eventCountdown_ = 0;
        count -= distance;
        tickEvent();
    }
}
#endif

#endif

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Apu::write(std::uint16_t address, std::uint8_t value) noexcept {
    synchronize();
    // Записи регистров могут менять DAC или проявлять скрытую фазу таймера.
    // Прежняя реализация наблюдает их ровно на следующем CPU-такте.
    eventCountdown_ = 1;
    if (address <= 0x4007 && address >= 0x4000) {
        auto& pulse = pulses_[(address >> 2) & 1];
        switch (address & 3) {
        case 0: pulse.envelope.control = value; break;
        case 1: pulse.sweep = value; pulse.sweepReload = true; break;
        case 2: pulse.period = (pulse.period & 0x700) | value; break;
        case 3:
            pulse.period = (pulse.period & 0xFF) | ((value & 7) << 8);
            if (pulse.enabled) pulse.length = lengths[value >> 3];
            pulse.sequence = 0;
            pulse.envelope.start = true;
            break;
        }
        return;
    }
    switch (address) {
    case 0x4008: triangle_.control = value; break;
    case 0x400A: triangle_.period = (triangle_.period & 0x700) | value; break;
    case 0x400B:
        triangle_.period = (triangle_.period & 0xFF) | ((value & 7) << 8);
        if (triangle_.enabled) triangle_.length = lengths[value >> 3];
        triangle_.reload = true;
        break;
    case 0x400C: noise_.envelope.control = value; break;
    case 0x400E: noise_.control = value; break;
    case 0x400F:
        if (noise_.enabled) noise_.length = lengths[value >> 3];
        noise_.envelope.start = true;
        break;
    case 0x4010: dmc_.control = value; if (!(value & 0x80)) dmc_.irq = false; break;
    case 0x4011: dmc_.output = value & 0x7F; break;
    case 0x4012: dmc_.start = value; break;
    case 0x4013: dmc_.length = value; break;
    case 0x4015:
        pulses_[0].enabled = (value & 1) != 0;
        pulses_[1].enabled = (value & 2) != 0;
        triangle_.enabled = (value & 4) != 0;
        noise_.enabled = (value & 8) != 0;
        if (!pulses_[0].enabled) pulses_[0].length = 0;
        if (!pulses_[1].enabled) pulses_[1].length = 0;
        if (!triangle_.enabled) triangle_.length = 0;
        if (!noise_.enabled) noise_.length = 0;
        if (!(value & 0x10)) dmc_.remaining = 0;
        else if (!dmc_.remaining) restartDmc();
        dmc_.irq = false;
        break;
    case 0x4017:
        fiveStep_ = (value & 0x80) != 0;
        irqInhibit_ = (value & 0x40) != 0;
        if (irqInhibit_) frameIrq_ = false;
        frameResetDelay_ = (cycles_ & 1) ? 3 : 4;
        break;
    default: break;
    }
}

/**
 * @brief Читает CPU-видимый статус каналов и подтверждает предусмотренный frame-IRQ.
 * @details Семантика отличается от peekStatus: побочный эффект чтения должен попасть в соответствующий пакет.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Apu::readStatus() noexcept {
    std::uint8_t result = 0;
    if (pulses_[0].length) result |= 1;
    if (pulses_[1].length) result |= 2;
    if (triangle_.length) result |= 4;
    if (noise_.length) result |= 8;
    if (dmc_.remaining) result |= 0x10;
    if (frameIrq_) result |= 0x40;
    if (dmc_.irq) result |= 0x80;
    frameIrq_ = false;
    return result;
}

/**
 * @brief Возвращает снимок внутренних счётчиков и флагов устройства для диагностики.
 * @details Снимок не является полным сохраняемым состоянием всей машины.
 * @return Вычисленное значение или состояние типа Apu::DebugState согласно описанному контракту.
 */
Apu::DebugState Apu::debug() const noexcept {
    auto noise = noise_;
    advanceNoise(noise, pendingCycles_, pal_);
    return {{pulses_[0].period, pulses_[1].period},
            {pulses_[0].length, pulses_[1].length, triangle_.length, noise_.length},
            dmc_.address, dmc_.remaining, noise.shift, dmc_.output, triangle_.linear,
            frameCycle_ + pendingCycles_, frameIrq_, dmc_.irq};
}

}  // пространство имён nes::core
