#include "ppu/Ppu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using nes::core::Ppu;

/**
 * @brief Параметры текущего scalar/batched случая и число проверок.
 * @details elapsed/group выражены в PPU dots; pal/tracking задают вид теста и выводятся при первом расхождении.
 */
struct Context {
    unsigned pal = 0, tracking = 0, elapsed = 0, group = 0;
    std::uint64_t checks = 0;
} context;

/** @brief Останавливает fixture при нарушении независимого ожидаемого условия. */
void require(bool condition, const char* message) {
    ++context.checks;
    if (!condition) {
        std::fprintf(stderr, "PPU batch FAIL: %s (pal=%u tracking=%u dots=%u group=%u)\n",
                     message, context.pal, context.tracking, context.elapsed, context.group);
        std::exit(1);
    }
}

/** @brief Возвращает воспроизводимое псевдослучайное слово без внешнего состояния. */
std::uint32_t randomWord(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/**
 * @brief Независимая PPU-шина с16KiB backing, четырьмя event counters и timestamped FNV trace.
 * @details Заимствованный ppu нужен только read-only frame/scanline/dot getters. У scalar и batched разные memory/events; record не продвигает гостевые часы.
 */
struct FakeBus final : nes::core::PpuBus {
    enum Event : unsigned { Cycle, Read, Write, Address };
    std::array<std::uint8_t, 0x4000> memory{};
    std::array<std::uint64_t, 4> events{};
    std::uint64_t trace = 14695981039346656037ULL;
    const Ppu* ppu = nullptr;

    /** @brief Добавляет наблюдённый PPU-фронт в собственный ограниченный журнал fixture. */
    void record(Event kind, std::uint16_t address = 0, std::uint8_t value = 0) noexcept {
        ++events[kind];
        /**
         * @brief Смешивает одно64-битное слово события в FNV trace этой тестовой шины.
         * @details Последовательно учитываются frame, scanline/dot и kind/address/value, что сохраняет зависимость хеша от порядка callback.
         */
        const auto mix = [this](std::uint64_t word) { trace = (trace ^ word) * 1099511628211ULL; };
        // Read-only getters времени не материализуют пиксели. Timestamp каждого callback
        // берётся из самого PPU, даже при выключенном cycle tracking.
        mix(ppu ? ppu->frameNumber() : 0);
        mix(ppu ? (std::uint64_t(static_cast<std::uint16_t>(ppu->scanline())) << 16) | ppu->dot() : 0);
        mix((std::uint64_t(kind) << 24) | (std::uint64_t(address) << 8) | value);
    }
    /** @brief Возвращает байт независимой тестовой PPU-шины и учитывает настоящий /RD. */
    std::uint8_t ppuRead(std::uint16_t address) noexcept override {
        const auto value = memory[address & 0x3FFF];
        record(Read, address, value);
        return value;
    }
    /** @brief Записывает байт независимой тестовой PPU-шины на исходной границе события. */
    void ppuWrite(std::uint16_t address, std::uint8_t value) noexcept override {
        record(Write, address, value);
        memory[address & 0x3FFF] = value;
    }
    /** @brief Фиксирует адресный фронт PPU отдельно от чтения данных. */
    void ppuAddress(std::uint16_t address) noexcept override { record(Address, address); }
    /** @brief Фиксирует реальный номер PPU-такта для сравнения порядка событий. */
    void ppuCycle() noexcept override { record(Cycle); }
};

/** @brief Сравнивает полный порядок адресных и read-фронтов двух независимых PPU. */
void compareTrace(const FakeBus& scalar, const FakeBus& batched) {
    require(scalar.events == batched.events, "bus callback counts");
    require(scalar.trace == batched.trace, "timestamped bus callback order/address/value");
}

/** @brief Сравнивает весь готовый RGB-буфер, включая палитру и emphasis. */
void comparePixels(const Ppu& scalar, const Ppu& batched) {
    require(std::memcmp(scalar.framebuffer(), batched.framebuffer(), Ppu::width * Ppu::height) == 0,
            "framebuffer at an observation barrier");
    for (unsigned row = 0; row < Ppu::height; ++row)
        require(scalar.scanlineEmphasis(row) == batched.scanlineEmphasis(row), "scanline emphasis");
    require(scalar.scanlineEmphasis(Ppu::height) == batched.scanlineEmphasis(Ppu::height),
            "out-of-range scanline emphasis");
}

/** @brief Сравнивает наблюдаемое состояние и границы обоих независимых PPU. */
void compareState(Ppu& scalar, Ppu& batched, const FakeBus& first, const FakeBus& second) {
    require(scalar.dot() == batched.dot(), "current dot");
    require(scalar.scanline() == batched.scanline(), "current scanline");
    require(scalar.frameNumber() == batched.frameNumber(), "frame number");
    require(scalar.frameReady() == batched.frameReady(), "frame-ready flag");
    require(scalar.emphasis() == batched.emphasis(), "current emphasis");
    // Оба observer принимают фронт на одной границе группы; внутри batched-интервала
    // не вставляются дополнительные CPU-доступы и наблюдения.
    require(scalar.takeNmi() == batched.takeNmi(), "consumed NMI edge");
    compareTrace(first, second);
}

/** @brief Выполняет один воспроизводимый scalar/batched сценарий с независимой памятью. */
void runCase(bool pal, unsigned tracking) {
    constexpr unsigned totalDots = 500000;
    constexpr unsigned stableDots = 120000;
    context.pal = unsigned(pal);
    context.tracking = tracking;
    context.elapsed = context.group = 0;
    std::uint32_t random = 0x9E3779B9U ^ (tracking * 0x51ED270BU) ^ (pal ? 0xABCDEF01U : 0);
    FakeBus first, second;
    for (auto& byte : first.memory) byte = static_cast<std::uint8_t>(randomWord(random));
    second.memory = first.memory; // Каждый эталон имеет независимое хранилище шины.
    Ppu scalar(first), batched(second);
    first.ppu = &scalar;
    second.ppu = &batched;
    scalar.reset(pal); batched.reset(pal);
    const bool cycles = (tracking & 1) != 0, addresses = (tracking & 2) != 0;
    scalar.setBusTracking(cycles, addresses);
    batched.setBusTracking(cycles, addresses);

    /**
     * @brief Подаёт одинаковую CPU-запись в независимые scalar и batched PPU.
     * @details После каждой пары сравнивает trace; запись не вставляется внутрь уже сравниваемой группы.
     */
    const auto write = [&](std::uint16_t reg, std::uint8_t value) {
        scalar.cpuWrite(reg, value); batched.cpuWrite(reg, value);
        compareTrace(first, second);
    };
    /**
     * @brief Сравнивает результат одного реального CPU-read регистра у обоих PPU и весь накопленный trace.
     * @details Побочные эффекты status/буферов исполняет каждый объект самостоятельно.
     */
    const auto read = [&](std::uint16_t reg) {
        require(scalar.cpuRead(reg) == batched.cpuRead(reg), "CPU register read value/status");
        compareTrace(first, second);
    };
    /**
     * @brief Подаёт один и тот же OAM DMA-байт обоим PPU и сравнивает порядок событий.
     * @details Не заменяет DMA обычной записью в backing и не разделяет состояние OAM между объектами.
     */
    const auto dma = [&](std::uint8_t value) {
        scalar.dmaWrite(value); batched.dmaWrite(value);
        compareTrace(first, second);
    };

    write(6, 0x3F); write(6, 0);
    for (unsigned i = 0; i < 32; ++i) write(7, static_cast<std::uint8_t>(3 * i + 1));
    write(3, 0);
    for (unsigned sprite = 0; sprite < 64; ++sprite) {
        dma(static_cast<std::uint8_t>((sprite / 8) * 24));
        dma(static_cast<std::uint8_t>(sprite));
        dma(static_cast<std::uint8_t>((sprite & 3) | ((sprite & 4) ? 0x20 : 0)));
        dma(static_cast<std::uint8_t>((sprite & 7) * 28));
    }
    read(2);
    write(0, 0x90); write(5, 0); write(5, 0); write(1, 0x1E);

    unsigned nextWrite = 37, nextRead = 53, nextDma = 97, nextBurst = 4093;
    unsigned nextGetter = 128 + randomWord(random) % 257;
    unsigned nextConst = 501 + randomWord(random) % 997;
    std::array<unsigned, 9> groups{};
    unsigned completedFrames = 0, statusReads = 0, getters = 0, constGetters = 0;
    while (context.elapsed < totalDots) {
        const unsigned boundary = std::min({totalDots, nextWrite, nextRead, nextDma,
                                            nextBurst, nextGetter, nextConst});
        context.group = std::min(1U + randomWord(random) % 8, boundary - context.elapsed);
        require(context.group >= 1 && context.group <= 8, "group fits the next shared observation point");
        ++groups[context.group];
        for (unsigned i = 0; i < context.group; ++i) scalar.tick();
        batched.tickDots(context.group);
        context.elapsed += context.group;
        compareState(scalar, batched, first, second);

        if (scalar.frameReady()) {
            comparePixels(scalar, batched);
            require(first.memory == second.memory, "bus memory at completed frame");
            scalar.clearFrameReady(); batched.clearFrameReady();
            ++completedFrames;
        }
        if (context.elapsed == nextWrite) {
            const auto value = randomWord(random);
            // Сначала проверяются устойчивые активные кадры, затем рендеринг прерывается
            // произвольными записями control/mask/scroll/address/data/OAM.
            if (context.elapsed >= stableDots)
                write(static_cast<std::uint16_t>(0x2000 | (value & 7)), static_cast<std::uint8_t>(value >> 16));
            nextWrite += 37;
        }
        if (context.elapsed == nextRead) {
            const auto value = randomWord(random);
            const unsigned reg = (++statusReads & 3) == 0 ? 2 : value & 7;
            read(static_cast<std::uint16_t>(0x2000 | reg));
            nextRead += 53;
        }
        if (context.elapsed == nextDma) {
            dma(static_cast<std::uint8_t>(randomWord(random)));
            nextDma += 97;
        }
        if (context.elapsed == nextBurst) {
            // Серии записей проверяют wrap OAM-адреса и изменение посреди evaluation.
            if (context.elapsed >= stableDots)
                for (unsigned i = 0; i < 256; ++i) dma(static_cast<std::uint8_t>(randomWord(random)));
            nextBurst += 4093;
        }
        if (context.elapsed == nextGetter) {
            comparePixels(scalar, batched);
            require(first.memory == second.memory, "bus memory at random getter barrier");
            nextGetter += 128 + randomWord(random) % 513;
            ++getters;
        }
        if (context.elapsed == nextConst) {
            // Это настоящие const-объекты, а не const-ссылки на
            // изменяемые PPU; getters материализуют только свой отложенный вывод.
            const Ppu scalarCopy(scalar), batchedCopy(batched);
            const auto firstTrace = first.trace, secondTrace = second.trace;
            comparePixels(scalarCopy, batchedCopy);
            require(first.trace == firstTrace && second.trace == secondTrace,
                    "const-copy getters do not advance external buses");
            nextConst += 501 + randomWord(random) % 997;
            ++constGetters;
        }
        compareState(scalar, batched, first, second);
    }
    comparePixels(scalar, batched);
    require(first.memory == second.memory, "final independent bus memory");
    require(first.events[FakeBus::Cycle] == (cycles ? totalDots : 0), "configured cycle tracking");
    require(addresses || first.events[FakeBus::Address] == 0, "configured address tracking");
    require(completedFrames > 0 && getters > 0 && constGetters > 0, "frame/getter/const barriers exercised");
    require(first.events[FakeBus::Read] > 0 && first.events[FakeBus::Write] > 0, "bus reads and writes exercised");
    for (unsigned count = 1; count <= 8; ++count) require(groups[count] > 0, "all group lengths exercised");
    std::printf("PPU batch pal=%u tracking=%u dots=%u frames=%u getters=%u const=%u\n",
                unsigned(pal), tracking, totalDots, completedFrames, getters, constGetters);
}
} // namespace

/**
 * @brief Сравнивает scalar и batch PPU на4000000 dots во всех NTSC/PAL×tracking комбинациях.
 * @details Воспроизводимые register/DMA/getter события и группы1..8 сравнивают timestamps, состояние и весь RGB при независимом backing.
 */
int main() {
    for (unsigned pal = 0; pal < 2; ++pal)
        for (unsigned tracking = 0; tracking < 4; ++tracking) runCase(pal != 0, tracking);
    std::printf("PPU scalar/batch contract passed: 4,000,000 dots, groups1..8, %llu checks\n",
                static_cast<unsigned long long>(context.checks));
}
