/**
 * @file VideoPacketHotStateTests.cpp
 * @brief Жизненный цикл и точная эквивалентность локальных/заимствованных метаданных видеопакета.
 * @details Тесты не обращаются к плате; скорость PSRAM/SRAM подтверждается отдельным P4 A/B.
 */
#include "ppu/VideoPacket.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

using nes::core::VideoFault;
using nes::core::VideoOp;
using nes::core::VideoPacket;

namespace {
unsigned checks = 0;

/** @brief Завершает fixture с точной строкой нарушенного контракта независимо от NDEBUG. */
[[noreturn]] void fail(const char* condition, unsigned line) {
    std::fprintf(stderr, "video hot state line %u: %s\n", line, condition);
    std::exit(1);
}
#define CHECK(condition) do { ++checks; if (!(condition)) fail(#condition, __LINE__); } while (false)

/**
 * @brief Записывает собственный смешанный протокол и seed в один Recording-пакет.
 * @param packet Свободный слот; большой event buffer не очищается при begin.
 * @param rom Общий неизменяемый источник обоих сравниваемых пакетов.
 * @param seed Начальная CHR-RAM для проверки отдельного большого хранилища.
 */
void writeStream(VideoPacket& packet, const std::array<std::uint8_t, 8192>& rom,
                 const std::array<std::uint8_t, 8192>& seed) {
    CHECK(packet.begin(11, 72, 123456, false, rom.data(), static_cast<std::uint32_t>(rom.size())));
    packet.setSeed(seed.data());
    CHECK(packet.add(VideoOp::NameWrite, 5, 7, 9, 0x12345678));
    CHECK(packet.add(VideoOp::PaletteWrite, 0x20, 0, 0, 13));
    const std::uint32_t tokens[] = {0, 16, 32};
    CHECK(packet.addBgRun(3, 8, 23, 0x1e, 2, tokens));
    for (unsigned row = 0; row < 240; ++row) {
        CHECK(packet.add(VideoOp::VisibleLine, row));
        CHECK(packet.add(VideoOp::Draw, row & 255, 3, 0, row * 7));
    }
    CHECK(packet.add(VideoOp::FrameEnd));
    CHECK(packet.seal(153220, 941));
}

/** @brief Сопоставляет весь открытый протокол без memcmp padding и без будущего live mapper. */
void samePacket(const VideoPacket& a, const VideoPacket& b) {
    const auto& x = a.header();
    const auto& y = b.header();
    CHECK(x.generation == y.generation && x.sequence == y.sequence);
    CHECK(x.startCycle == y.startCycle && x.endCycle == y.endCycle && x.ppuFrame == y.ppuFrame);
    CHECK(x.chrRom == y.chrRom && x.chrRomBytes == y.chrRomBytes);
    CHECK(x.eventCount == y.eventCount && x.fault == y.fault);
    CHECK(x.pal == y.pal && x.seedPresent == y.seedPresent && x.protocol == y.protocol);
    CHECK(a.state() == b.state());
    CHECK(a.legacyEventCount() == b.legacyEventCount());
    CHECK(a.literalPrefixPending() == b.literalPrefixPending());
    for (std::uint32_t i = 0; i < x.eventCount; ++i) {
        CHECK(a.events()[i].tag == b.events()[i].tag);
        CHECK(a.events()[i].value == b.events()[i].value);
    }
    for (unsigned i = 0; i < 8192; ++i) CHECK(a.chrRamSeed()[i] == b.chrRamSeed()[i]);
}

/** @brief Проверяет неизменность протокола и Free/Recording/Sealed ограничения bind/detach. */
void lifecycleAndStream() {
    VideoPacket::HotState external;
    auto local = std::make_unique<VideoPacket>();
    auto borrowed = std::make_unique<VideoPacket>();
    std::array<std::uint8_t, 8192> rom{}, seed{};
    for (unsigned i = 0; i < 8192; ++i) { rom[i] = static_cast<std::uint8_t>(i); seed[i] = static_cast<std::uint8_t>(i * 17); }
    CHECK(borrowed->bindHotState(nullptr));
    CHECK(!borrowed->bindHotState(&external));
    CHECK(external.owner == nullptr);
    writeStream(*local, rom, seed);
    writeStream(*borrowed, rom, seed);
    samePacket(*local, *borrowed);
    CHECK(!borrowed->bindHotState(nullptr)); // Sealed ещё принадлежит consumer.
    CHECK(local->release() && borrowed->release());
    CHECK(borrowed->bindHotState(nullptr));
    CHECK(external.owner == nullptr);
    samePacket(*local, *borrowed); // Detach сохраняет последний Free-заголовок и события.
    CHECK(!borrowed->release());

    CHECK(borrowed->begin(12, 73, 200000, true, rom.data(), static_cast<std::uint32_t>(rom.size())));
    CHECK(!borrowed->bindHotState(nullptr));
    CHECK(!borrowed->bindHotState(&external));
    CHECK(borrowed->add(VideoOp::Fill, 1, 2, 3, 4));
    borrowed->fault(VideoFault::InvalidReference);
    borrowed->fault(VideoFault::Capacity);
    CHECK(borrowed->header().fault == VideoFault::InvalidReference);
    borrowed->abandon();
    CHECK(borrowed->state() == VideoPacket::State::Free);
    CHECK(borrowed->header().eventCount == 0 && borrowed->header().fault == VideoFault::None);
}

/** @brief Проверяет отказ от внешнего хранилища и успешное использование локального состояния пакета. */
void rejectedBindings() {
    VideoPacket::HotState external;
    auto first = std::make_unique<VideoPacket>();
    auto second = std::make_unique<VideoPacket>();
    CHECK(!first->bindHotState(&external));
    CHECK(second->bindHotState(nullptr));
}
} // namespace

/** @brief Выполняет equivalence и lifetime gates для одного публичного layout-профиля. */
int main() {
    lifecycleAndStream();
    rejectedBindings();
    std::printf("Video hot state: %u checks; packet=%zu; hot=%zu; twoHot=%zu\n",
                checks, sizeof(VideoPacket), sizeof(VideoPacket::HotState), 2 * sizeof(VideoPacket::HotState));
    return 0;
}
