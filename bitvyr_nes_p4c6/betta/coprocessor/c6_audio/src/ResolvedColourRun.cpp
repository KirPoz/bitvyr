/** @file ResolvedColourRun.cpp @brief Shipping Run compositing без primary timing, mapper и ROM. */
#include "ResolvedColourRun.h"
#include "Palette.h"
#include <algorithm>
#include <cstring>

namespace nes::c6_colour {
namespace {
/** @brief Константная ROM LUT разворачивает два pattern bytes в8 packed palette selectors. */
constexpr std::array<std::uint32_t,256> makePlanes() noexcept {
    std::array<std::uint32_t,256> result{};
    for(unsigned b=0;b<256;++b)for(unsigned bit=0;bit<8;++bit)if(b&(128U>>bit))result[b]|=1U<<(bit*4U);
    return result;
}
constexpr auto expanded=makePlanes();
/** @brief Проверяет O(1) форму shipping run без просмотра tile массива. */
bool validShape(const Run& in) noexcept {
    if(!in.tiles||in.tiles>32||in.x>=256||in.y>=240||in.x+unsigned(in.tiles)*8U>256||in.fine>7||
       in.before.spriteClocks>256||in.before.nextAttr>3)return false;
    return true;
}
/** @brief Проверяет все transmitted selectors до C6 pixel stores. */
bool validRun(const Run& in) noexcept {
    if(!validShape(in))return false;
    for(unsigned i=0;i<in.tiles;++i)if(in.fetched[i].attribute>3)return false;
    return true;
}
/** @brief Fine-X selector двух16битных плоскостей и attribute shifters. */
std::uint32_t pixels(std::uint32_t planes,std::uint32_t attrs,unsigned fine) noexcept {
    return expanded[(planes>>(8U-fine))&255U]|(expanded[(planes>>(24U-fine))&255U]<<1U)|
        (expanded[(attrs>>(8U-fine))&255U]<<2U)|(expanded[(attrs>>(24U-fine))&255U]<<3U);
}
/** @brief Рисует8 пикселей с pair-LUT fast path для пустого sprite участка. */
void draw8(const RowContext& row,const Run& in,RunResult& out,unsigned x,unsigned cursor,std::uint32_t background) noexcept {
    if(!(in.mask&8U))background=0;
    else if(x<8U&&!(in.mask&2U)){const auto hidden=std::min(8U,8U-x);background=hidden==8?0:background&(~0U<<(hidden*4U));}
    const unsigned hiddenSprites=!(in.mask&16U)?8U:((x<8U&&!(in.mask&4U))?8U-x:0U);
    bool empty=true;
    for(unsigned i=hiddenSprites;i<8U&&cursor+i<256;++i)if(row.sprites[cursor+i]&3U){empty=false;break;}
    if(empty){
        for(unsigned pair=0;pair<4;++pair,background>>=8U){
            const auto key=background&255U;
            std::memcpy(out.indices.data()+x+pair*2U,row.indexPairs[key].data(),2);
            std::memcpy(out.rgb.data()+x+pair*2U,row.rgbPairs[key].data(),4);
        }
        return;
    }
    for(unsigned i=0;i<8;++i,background>>=4U){
        const auto sprite=i<hiddenSprites||cursor+i>=256?0U:row.sprites[cursor+i];
        const auto bg=background&15U;
        const auto selected=(sprite&3U)&&(!(bg&3U)||!(sprite&0x10U))?16U+(sprite&15U):((bg&3U)?bg:0U);
        out.indices[x+i]=row.indexPalette[selected];out.rgb[x+i]=row.rgbPalette[selected];
    }
}
}

/** @brief Подготавливает caller-owned row из проверенного immutable seed без нового fetch. */
bool prepareRow(const RowSeed& seed,std::uint8_t mask,RowContext& row) noexcept {
    for(const auto& sprite:seed.sprites)if(sprite.flags&~3U)return false;
    row.sprites.fill(0);row.pal=seed.pal;row.maskKey=mask&0xe1U;
    for(const auto& sprite:seed.sprites){
        if(!(sprite.flags&1U))continue;
        const unsigned meta=((sprite.attributes&3U)<<2U)|((sprite.attributes&0x20U)?0x10U:0U)|((sprite.flags&2U)?0x20U:0U);
        for(unsigned bit=0;bit<8U&&sprite.x+bit<256;++bit){
            if(row.sprites[sprite.x+bit])continue;
            const auto maskBit=128U>>bit,pixel=((sprite.low&maskBit)?1U:0U)|((sprite.high&maskBit)?2U:0U);
            if(pixel)row.sprites[sprite.x+bit]=std::uint8_t(meta|pixel);
        }
    }
    for(unsigned i=0;i<32;++i){
        row.indexPalette[i]=seed.palette[i]&((mask&1U)?0x30U:0x3fU);
        row.rgbPalette[i]=::nes::core::displayRgb565(row.indexPalette[i],mask>>5U,seed.pal);
    }
    for(unsigned pair=0;pair<256;++pair){
        const auto a=pair&15U,b=pair>>4U,first=(a&3U)?a:0U,second=(b&3U)?b:0U;
        row.indexPairs[pair]={row.indexPalette[first],row.indexPalette[second]};
        row.rgbPairs[pair]={row.rgbPalette[first],row.rgbPalette[second]};
    }
    row.valid=true;return true;
}

/** @brief Вычисляет следующий carry только для валидного bounded run. */
bool advanceRunCarry(const Run& in,Carry& out) noexcept {
    if(!validShape(in))return false;
    const auto previous=in.tiles==1?Tile{in.before.nextLow,in.before.nextHigh,in.before.nextAttr}:in.fetched[in.tiles-2U];
    const auto next=in.fetched[in.tiles-1U];
    if(previous.attribute>3||next.attribute>3)return false;
    out.low=std::uint16_t(previous.low)<<8U;out.high=std::uint16_t(previous.high)<<8U;
    out.attrLow=(previous.attribute&1U)?0xff00U:0U;out.attrHigh=(previous.attribute&2U)?0xff00U:0U;
    out.nextLow=next.low;out.nextHigh=next.high;out.nextAttr=next.attribute;
    out.spriteClocks=std::min(256U,unsigned(in.before.spriteClocks)+unsigned(in.tiles)*8U);
    return true;
}

/** @brief Рендерит проверенный run в caller result, сохраняя RGB и Sprite0. */
bool renderRun(const RowContext& row,const Run& in,RunResult& out) noexcept {
    if(!row.valid||row.maskKey!=(in.mask&0xe1U)||!validRun(in))return false;
    auto planes=std::uint32_t(in.before.low)|(std::uint32_t(in.before.high)<<16U);
    auto attrs=std::uint32_t(in.before.attrLow)|(std::uint32_t(in.before.attrHigh)<<16U);
    unsigned nextLow=in.before.nextLow,nextHigh=in.before.nextHigh,nextAttr=in.before.nextAttr,cursor=in.before.spriteClocks;
    for(unsigned i=0;i<in.tiles;++i){
        planes=(planes&0xff00ff00U)|nextLow|(nextHigh<<16U);
        attrs=(attrs&0xff00ff00U)|((nextAttr&1U)?255U:0U)|((nextAttr&2U)?0x00ff0000U:0U);
        draw8(row,in,out,in.x+i*8U,cursor,pixels(planes,attrs,in.fine));
        planes=(planes<<8U)&0xff00ff00U;attrs=(attrs<<8U)&0xff00ff00U;
        cursor=std::min(256U,cursor+8U);nextLow=in.fetched[i].low;nextHigh=in.fetched[i].high;nextAttr=in.fetched[i].attribute;
    }
    // Сравниваемый O(1) helper использует ту же shipping carry, не запускает pixel loop на Main.
    return advanceRunCarry(in,out.after);
}

/** @brief Перекрашивает только подтверждённый prefix; pointers остаются caller-owned. */
bool recolourPrefix(const RowContext& row,std::uint16_t* rgb,const std::uint8_t* indices,unsigned prefix,std::uint8_t emphasis) noexcept {
    if(!row.valid||!rgb||!indices||prefix>256||emphasis>7)return false;
    for(unsigned i=0;i<prefix;++i)rgb[i]=::nes::core::displayRgb565(indices[i]&63U,emphasis,row.pal);
    return true;
}
}
