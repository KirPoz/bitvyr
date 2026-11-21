#pragma once
#include "ppu/c6/ResolvedColourRun.h"
#include <array>
#include <cstdint>

namespace nes::c6_colour::index_only {
/** @brief C6-owned cache только resolved6-bit indices; emphasis/PAL не влияют на кэш индексов. */
struct alignas(4) Context final {
    alignas(4) std::array<std::uint8_t,256> sprites{};
    std::array<std::uint8_t,32> palette{},indexPalette{};
    std::array<std::uint16_t,256> indexPairs{};
    std::array<SpriteDescriptor,8> descriptors{};
    bool valid=false,gray=false;
};
/** @brief Результат без RGB; outsideRun pixels не меняются, carry совпадает с fullRGB reference. */
struct alignas(4) Result final {Carry after{};alignas(4) std::array<std::uint8_t,256> indices{};};
/** @brief Проверяет seed и обновляет только изменившиеся sprite/palette caches; false не меняет Context. */
bool prepare(const RowSeed& seed,std::uint8_t mask,Context& row) noexcept;
/** @brief Исполняет настоящий shipping Run1..32; проверяет весь input до stores; никаких RGB/cache expansion. */
bool render(const Context& row,const Run& run,Result& out) noexcept;
/** @brief Main-only exact dirty merge: исторические indices плюс final emphasis/PAL, native gaps не меняются. */
bool mergeDirty(std::uint16_t* rgb,std::uint8_t* scratch,const std::uint8_t* indices,
    const std::uint8_t* dirty,unsigned prefix,std::uint8_t emphasis,bool pal) noexcept;
static_assert(sizeof(Context)<=896,"bounded index-only cache fits896bytes");
static_assert(sizeof(Result)<=276,"no RGB result allocation");
}
