/** @file Проверка независимого PCM stream guard на захваченных временных ключах. */
#include "../../app/nes/audio/PcmStreamGuard.h"
#include "../../app/nes/audio/c6/runtime/PcmQueue.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {
using Guard = nes::audio::PcmStreamGuard;
unsigned checks = 0;
std::array<std::array<std::int16_t, nes::core::Apu::sampleCapacity>, 2> pcm{};

/** @brief Завершает тест кодом73 при первом расхождении, без Windows CRT dialog. */
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "PCM_GUARD_CHECK_FAILED %s\n", message); std::exit(73); }
}

/** @brief Создаёт только тестовую identity, не заимствуя Machine/ROM. */
Guard::Binding binding(nes::core::Region region = nes::core::Region::Ntsc, std::uint64_t generation = 3) {
    return {11, 12, 13, 14, generation, 1, region, nes::core::AudioPacket::externalDacQ16};
}

/** @brief Формирует опубликованный ключ заданного интервала с чередованием arena. */
Guard::Key key(std::uint64_t frame, std::uint64_t base, std::uint64_t end,
               nes::core::Region region = nes::core::Region::Ntsc, std::uint64_t generation = 3) {
    return {11, 12, 13, 14, frame, {generation, frame, frame + 1, base, end, region,
            nes::core::AudioPacket::externalDacQ16}, static_cast<std::uint8_t>(frame & 1)};
}

/** @brief Требует отказа до мутации всего числового observer. */
void rejected(Guard& guard, const Guard::Key& bad, const std::int16_t* data, std::size_t count) {
    std::array<unsigned char, sizeof(Guard)> before{};
    std::memcpy(before.data(), &guard, sizeof guard);
    require(!guard.observe(bad, data, count), "invalid ticket/count accepted");
    require(!std::memcmp(before.data(), &guard, sizeof guard), "refusal changed state");
}

/** @brief Проверяет три настоящих CV capture cuts и отказы всех отдельных доменов. */
void directed() {
    Guard guard;
    const auto first = key(0, 0, 27511);
    rejected(guard, first, pcm[0].data(), 737);
    auto badBinding = binding(); badBinding.session = 0;
    require(!guard.bind(badBinding), "zero session bind");
    require(guard.bind(binding()), "fresh bind");
    require(!guard.bind(binding()), "double bind");
    auto bad = first;
    for (unsigned field = 0; field != 13; ++field) {
        bad = first;
        switch (field) {
        case 0: ++bad.appGeneration; break;
        case 1: ++bad.romLease; break;
        case 2: ++bad.romEpoch; break;
        case 3: ++bad.session; break;
        case 4: ++bad.inputFrame; break;
        case 5: ++bad.audio.generation; break;
        case 6: ++bad.audio.sequence; break;
        case 7: ++bad.audio.ppuFrame; break;
        case 8: ++bad.audio.baseCycle; break;
        case 9: bad.audio.endCycle = 45000; break;
        case 10: bad.audio.region = nes::core::Region::Pal; break;
        case 11: bad.audio.capabilities = 0; break;
        default: bad.arena = 2; break;
        }
        rejected(guard, bad, pcm[0].data(), 737);
    }
    rejected(guard, first, pcm[0].data(), 736);
    rejected(guard, first, pcm[0].data(), 738);
    rejected(guard, first, nullptr, 737);
    require(guard.observe(first, pcm[0].data(), 737), "captured first737");
    for (unsigned i = 0; i != 100; ++i)
        require(guard.observe(first, pcm[0].data(), 737), "partial-I2S repeat");
    require(guard.frames() == 1, "repeat counted as another frame");
    rejected(guard, first, pcm[1].data(), 737);
    const auto second = key(1, 27511, 57291);
    require(guard.observe(second, pcm[1].data(), 799), "captured second799");
    rejected(guard, first, pcm[0].data(), 737);
    require(guard.observe(key(2, 57291, 87072), pcm[0].data(), 799), "captured third799");
    require(guard.frames() == 3, "whole ticket count");
    auto wrapped = key(0, 0, 1); wrapped.inputFrame = ~std::uint64_t{0};
    rejected(guard, wrapped, pcm[0].data(), 0);

    Guard pal, ntsc;
    require(pal.bind(binding(nes::core::Region::Pal)), "PAL bind");
    require(ntsc.bind(binding()), "NTSC bind");
    require(pal.observe(key(0, 0, 35, nes::core::Region::Pal), pcm[0].data(), 1), "PAL first sample at35");
    require(ntsc.observe(key(0, 0, 35), nullptr, 0), "NTSC no sample at35");
    require(ntsc.observe(key(0, 0, 35), nullptr, 0), "zero PCM repeat");
    require(ntsc.observe(key(1, 35, 38), pcm[1].data(), 1), "NTSC phase carried to38");
}

/**
 * @brief Сравнивает stream guard с независимыми captured header/sample-count в TSV.
 * @details TSV готовится из сохранённого wire и JSON capture; PCM/ROM сюда не копируются.
 */
void corpus(const char* path) {
    std::ifstream input(path);
    require(bool(input), "open captured vectors");
    Guard guard;
    std::uint64_t frame{}, generation{}, sequence{}, ppu{}, base{}, end{};
    unsigned region{}, capabilities{}, count{};
    std::size_t frames = 0;
    while (input >> frame >> generation >> sequence >> ppu >> base >> end >> region >> capabilities >> count) {
        const auto r = static_cast<nes::core::Region>(region);
        if (!frames) {
            auto b = binding(r, generation); b.audioCapabilities = static_cast<std::uint16_t>(capabilities);
            require(guard.bind(b), "captured stream bind");
        }
        auto actual = key(frame, base, end, r, generation);
        actual.audio.sequence = sequence; actual.audio.ppuFrame = ppu;
        actual.audio.capabilities = static_cast<std::uint16_t>(capabilities);
        require(guard.observe(actual, pcm[frame & 1].data(), count), "captured sample-count");
        require(guard.observe(actual, pcm[frame & 1].data(), count), "captured repeat");
        require(guard.frames() == frames + 1, "captured ticket counter");
        rejected(guard, actual, pcm[frame & 1].data(), count + 1);
        ++frames;
    }
    require(input.eof() && frames, "complete captured stream");
    std::printf("PCM_GUARD_CORPUS frames=%zu\n", frames);
}
/** @brief Проверяет порядок и окна выдачи PCM при заполнении трёх блоков. */
void threePcmTickets() {
    using Queue = nes::app::c6_async::PcmQueue;
    using Key = nes::c6audio::async::PublishedKey;
    constexpr unsigned samples = 4;
    std::array<std::array<std::int16_t, samples>, 3> buffers{};
    Queue queue;
    require(!queue.initialize(buffers[0].data(), buffers[1].data(), samples, buffers[0].data()),
            "reject overlapping blocks");
    require(queue.initialize(buffers[0].data(), buffers[1].data(), samples, buffers[2].data()),
            "initialize three blocks");
    const auto ticket = [](std::uint64_t frame) -> Key {
        constexpr std::uint64_t cycles = 29781;
        return {11, 12, 13, 14, frame,
                {15, frame, frame + 1, frame * cycles, (frame + 1) * cycles, nes::core::Region::Ntsc, 0}, 1};
    };

    std::int16_t* destination = nullptr;
    for (unsigned frame = 0; frame < buffers.size(); ++frame) {
        require(queue.reserve(ticket(frame), destination) && destination == buffers[frame].data(),
                "reserve the next distinct block");
        destination[1] = static_cast<std::int16_t>(frame + 10);
        require(queue.ready(ticket(frame), samples), "publish completed PCM");
    }
    require(queue.peakOccupied() == 3 && queue.canDispatch(2) && !queue.canDispatch(3),
            "bound dispatch at two guest frames");
    require(!queue.reserve(ticket(3), destination), "reserve cannot reuse an occupied block");

    Queue::Output output{};
    require(!queue.beginOutput(0, false, output), "reject playback before next dispatch");
    require(queue.beginOutput(2, false, output) && output.key.inputFrame == 0 && output.samples == samples,
            "play the oldest block after two dispatches");
    require(queue.outputAccepted(ticket(0), 1) && queue.occupied() == 3,
            "partial I2S write retains ownership");
    require(!queue.beginOutput(1, false, output), "reject a backwards dispatch watermark");
    require(queue.beginOutput(2, false, output) && output.offset == 1 && *output.data == 10,
            "resume at exactly the first unwritten sample");
    require(!queue.outputAccepted(ticket(1), 3), "a later reply cannot release the oldest block");
    require(queue.outputAccepted(ticket(0), 3) && queue.canDispatch(3), "release only after all samples");
    require(queue.reserve(ticket(3), destination) && destination == buffers[0].data(),
            "reuse the released block");
    require(queue.ready(ticket(3), 0), "publish a block without samples");

    for (unsigned frame = 1; frame <= 3; ++frame) {
        require(queue.beginOutput(3, true, output) && output.key.inputFrame == frame,
                "drain remaining blocks in order");
        require(queue.outputAccepted(output.key, output.samples), "release the drained block");
    }
    require(queue.maxOutputLag() == 2 && queue.emptyForRebind() && queue.rebindEmpty(),
            "release all owners and reset for the next ROM");
}

} // namespace

/** @brief Выполняет baseline/negative и необязательные реальные captured streams. */
int main(int argc, char** argv) {
    directed();
    threePcmTickets();
    if (argc == 2 && !std::strcmp(argv[1], "--negative")) {
        Guard guard; require(guard.bind(binding()), "negative bind");
        require(guard.observe(key(0, 0, 27511), pcm[0].data(), 738), "deliberate bad sample count");
    }
    for (int i = 1; i < argc; ++i) corpus(argv[i]);
    std::printf("PCM_GUARD_PASS checks=%u host_bytes=%zu\n", checks, sizeof(Guard));
}
