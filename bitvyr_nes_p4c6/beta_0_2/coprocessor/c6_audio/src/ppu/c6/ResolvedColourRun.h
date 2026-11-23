#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "ResolvedColourTypes.h"
namespace nes::c6_colour {
/** @brief Post-flip sprite planes; flags bit0=valid, bit1=OAM sprite0, порядок8 slots сохраняется. */
struct SpriteDescriptor {std::uint8_t low=0,high=0,x=0,attributes=0,flags=0;};
/** @brief Компактный row seed без ROM/NT/primary state; палитра обновляется в точном порядке. */
struct RowSeed {
    std::array<std::uint8_t,32> palette{};
    std::array<SpriteDescriptor,8> sprites{};
    bool pal=false;
};
/** @brief Только output-consumer carry; не состояние первичного PPU/mapper/IRQ. */
struct Carry {
    std::uint16_t low=0,high=0,attrLow=0,attrHigh=0;
    std::uint8_t nextLow=0,nextHigh=0,nextAttr=0;
    std::uint16_t spriteClocks=0;
};
/** @brief Shipping backgroundRun grammar: загрузить old next, draw8, shift8, затем fetched tile. */
struct Run {
    Carry before{};
    std::uint16_t x=0,y=0;
    std::uint8_t tiles=0,mask=0,fine=0;
    std::array<Tile,32> fetched{};
};
/** @brief C6-owned row/palette cache; отдельно от RGB/index row lifetime. */
struct RowContext {
    std::array<std::uint8_t,256> sprites{};
    std::array<std::uint8_t,32> indexPalette{};
    std::array<std::uint16_t,32> rgbPalette{};
    std::array<std::array<std::uint8_t,2>,256> indexPairs{};
    std::array<std::array<std::uint16_t,2>,256> rgbPairs{};
    std::uint8_t maskKey=0;
    bool pal=false,valid=false;
};
/** @brief Результат одного run; caller сохраняет RGB/index row до окончательного emphasis. */
struct RunResult {Carry after{};std::array<std::uint16_t,256> rgb{};std::array<std::uint8_t,256> indices{};};
/** @brief Строит row и palette/pair caches из73Б seed, не меняя guest state; false при bad flags. */
bool prepareRow(const RowSeed&,std::uint8_t mask,RowContext&) noexcept;
/** @brief Рисует shipping run1..32 tiles; outside[x,x+tiles*8) не меняется; false до stores. */
bool renderRun(const RowContext&,const Run&,RunResult&) noexcept;
/** @brief Вычисляет shipping carry за O(1) без RGB: count1 использует seed next, иначе предпоследний fetch. */
bool advanceRunCarry(const Run&,Carry&) noexcept;
/** @brief Повторяет поздний emphasis по уже разрешённым indices prefix; row нельзя выдать раньше. */
bool recolourPrefix(const RowContext&,std::uint16_t* rgb,const std::uint8_t* indices,unsigned prefix,std::uint8_t emphasis) noexcept;
static_assert(sizeof(SpriteDescriptor)==5,"explicit wire descriptor5B");
static_assert(sizeof(RowSeed)==73,"wire fields serialized explicitly, not memcpy native struct");
static_assert(sizeof(Run)<128,"resolved run fits compact raw payload");
}
