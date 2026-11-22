#include "../../app/nes/graphics/PresentationRate.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

using nes::app::PresentationRate;

/** @brief Проверяет первое окно, точный секундный порог и отсутствие повторного счёта. */
static void cadenceAndDuplicates() {
    PresentationRate rate;
    assert(!rate.valid() && rate.fps() == 0);
    assert(!rate.observe(100, 10));
    assert(!rate.observe(500100, 40) && !rate.valid());
    assert(!rate.observe(1000099, 70) && !rate.valid());
    assert(rate.observe(1000100, 70) && rate.valid() && rate.fps() == 60);
    assert(!rate.observe(1000100, 70) && rate.fps() == 60);
    assert(!rate.observe(1500100, 100));
    assert(!rate.observe(1500100, 100));
    assert(!rate.observe(2000100, 130) && rate.fps() == 60);
    assert(!rate.observe(2000100, 131));
    assert(!rate.observe(3000100, 190) && rate.fps() == 60);
    assert(rate.observe(4000100, 240) && rate.fps() == 50);
}

/** @brief Моделирует неуспешные begin/submit без роста счётчика и видимую остановку вывода. */
static void noGrowthAndStall() {
    PresentationRate rate;
    rate.reset(0, 100);
    assert(rate.observe(1000000, 100) && rate.valid() && rate.fps() == 0);
    assert(!rate.observe(2000000, 100) && rate.fps() == 0);
    assert(rate.observe(3000000, 160) && rate.fps() == 60);
    assert(!rate.observe(3500000, 160) && rate.fps() == 60);
    assert(rate.observe(4000000, 160) && rate.fps() == 0);
}

/** @brief Проверяет reset при паузе/новой сессии и ненулевой начальный счётчик. */
static void sessionsAndPause() {
    PresentationRate rate;
    rate.reset(0, 1000);
    assert(rate.observe(1000000, 1060) && rate.fps() == 60);
    rate.reset(9000000, 1060);
    assert(!rate.valid() && rate.fps() == 0);
    assert(!rate.observe(9500000, 1090));
    assert(rate.observe(10000000, 1120) && rate.fps() == 60);
    rate.reset(11000000, 0);
    assert(!rate.valid() && !rate.observe(11000000, 0));
    assert(rate.observe(12000000, 30) && rate.fps() == 30);
}

/** @brief Проверяет обратный ход часов и уменьшение счётчика внутри незакрытого окна. */
static void rollbacks() {
    PresentationRate rate;
    rate.reset(0, 100);
    assert(!rate.observe(600000, 130));
    assert(!rate.observe(700000, 129) && !rate.valid());
    assert(rate.observe(1700000, 189) && rate.fps() == 60);
    assert(!rate.observe(2300000, 225));
    assert(rate.observe(2200000, 225) && !rate.valid() && rate.fps() == 0);
    assert(rate.observe(3200000, 285) && rate.fps() == 60);
    assert(!rate.observe(3700000, 315));
    assert(rate.observe(3800000, 314) && !rate.valid());
    assert(rate.observe(4800000, 374) && rate.fps() == 60);
}

/** @brief Проверяет дробную частоту и крайние int64/uint64 без signed overflow или NaN. */
static void boundaries() {
    PresentationRate rate;
    rate.reset(0, 0);
    assert(rate.observe(1001000, 60));
    assert(std::abs(rate.fps() - 60000000.0 / 1001000.0) < 1e-12);
    const auto low = std::numeric_limits<std::int64_t>::min();
    const auto high = std::numeric_limits<std::int64_t>::max();
    rate.reset(low, 0);
    assert(rate.observe(high, 100) && rate.valid() && std::isfinite(rate.fps()) && rate.fps() > 0);
    assert(rate.observe(low, 100) && !rate.valid());
    rate.reset(0, 0);
    assert(rate.observe(1000000, std::numeric_limits<std::uint64_t>::max()) && std::isfinite(rate.fps()));
    assert(rate.observe(1000001, 0) && !rate.valid());
}

/** @brief Запускает чистые host-проверки счётчика software-submit без SDK, Board и эмуляции. */
int main() {
    cadenceAndDuplicates();
    noGrowthAndStall();
    sessionsAndPause();
    rollbacks();
    boundaries();
    std::puts("PASS PresentationRate cadence/reset/stall/rollback/no-double-count/boundaries");
}
