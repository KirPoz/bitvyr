#pragma once
#include <cstddef>
#include <cstdint>

namespace nes::app::c6_async {
/**
 * @brief Main-only счётчики cycle-wall runtime pump; отдельные полные cache lines от Worker publication.
 * @details Не являются exclusiveCPU: интервалы включают preemption/IRQ/SDK tasks.
 * RPC-stats unchanged не означает purePending: возможны publication/encode/I2S действия.
 * Модуль не управляет временем гостя, протоколом или заимствованиями и не выделяет память.
 */
struct alignas(64) MainPumpCost final {
    std::uint64_t calls=0,cycles=0,maxCycles=0;
    std::uint64_t noRpcCalls=0,noRpcCycles=0,changedCalls=0,changedCycles=0;
    std::uint64_t noRpcNoI2sCalls=0,noRpcNoI2sCycles=0,i2sCalls=0,i2sSamples=0,sinkSamples=0;
    /** @brief Main начинает новое accepted120 окно только вне активного MainPumpSpan. */
    void reset()noexcept{*this=MainPumpCost{};}
    /** @brief Учитывает реальные writeAudio(timeout0) вызовы и принятые отсчёты; mute-sink записывается отдельно. */
    void noteOutput(bool physical,std::size_t accepted)noexcept{
        if(physical){++i2sCalls;i2sSamples+=accepted;}else sinkSamples+=accepted;
    }
    /**
     * @brief Добавляет один завершённый вызов; unsigned32 subtraction допускает ровно один cycle wrap.
     * @param begin/end Два same-CPU0 CSR timestamp. Истинная длительность должна быть меньше2^32cycles.
     * @param changed Изменился хотя бы один RPC counter, не признак C6 completion сам по себе.
     * @param output За этот вызов был I2S вызов или mute-sink acceptance; даже I2Saccepted0 учитывается.
     */
    void record(std::uint32_t begin,std::uint32_t end,bool changed,bool output)noexcept{
        const auto elapsed=std::uint32_t(end-begin);++calls;cycles+=elapsed;
        if(elapsed>maxCycles)maxCycles=elapsed;
        if(changed){++changedCalls;changedCycles+=elapsed;}
        else{++noRpcCalls;noRpcCycles+=elapsed;if(!output){++noRpcNoI2sCalls;noRpcNoI2sCycles+=elapsed;}}
    }
};
static_assert(sizeof(MainPumpCost)==128,"Main-only observer uses two complete64byte cache lines");

/**
 * @brief Два ClockFn вызова вокруг полного runtime pump body; update counters находится за концом интервала.
 * @details Stats принадлежат Main и переживают scope. Снимки четырёх existing RPC counters
 * берутся до первого/после второго CSR. Никаких retries, ожиданий, логов или аллокаций.
 */
template<class Stats> class MainPumpSpan final {
public:
    using Clock=std::uint32_t(*)()noexcept;
    /** @brief Clock обязан быть ненулевым sameCPU0 monotonic CSR reader с compiler memory barriers. */
    MainPumpSpan(MainPumpCost&cost,const Stats&stats,Clock clock)noexcept:
        cost_(cost),stats_(stats),clock_(clock),before_(snapshot(stats)),writes_(cost.i2sCalls),sink_(cost.sinkSamples),begin_(clock_()){}
    /** @brief На любом обычном/early return закрывает измерение; fault не превращается в освобождение владельца. */
    ~MainPumpSpan()noexcept{
        const auto end=clock_();const auto after=snapshot(stats_);
        const bool changed=before_.calls!=after.calls||before_.frames!=after.frames||before_.chunks!=after.chunks||before_.elapsed!=after.elapsed;
        cost_.record(begin_,end,changed,writes_!=cost_.i2sCalls||sink_!=cost_.sinkSamples);
    }
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    MainPumpSpan(const MainPumpSpan&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    MainPumpSpan&operator=(const MainPumpSpan&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    MainPumpSpan(MainPumpSpan&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    MainPumpSpan&operator=(MainPumpSpan&&)=delete;
private:
    /** @brief Только stable Main-owned numeric values, без изменения transport state. */
    struct Snapshot final {std::uint64_t calls,frames,chunks,elapsed;};
    /** @brief Читает existing counters; это classifier, а не подтверждение completion или slot reuse. */
    static Snapshot snapshot(const Stats&stats)noexcept{return {stats.calls,stats.frames,stats.pcmChunks,stats.hostElapsedUs};}
    MainPumpCost&cost_;const Stats&stats_;Clock clock_;Snapshot before_;
    std::uint64_t writes_,sink_;std::uint32_t begin_;
};
} // namespace nes::app::c6_async
