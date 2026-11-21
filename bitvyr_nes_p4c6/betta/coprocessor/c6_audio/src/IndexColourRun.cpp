/** @file IndexColourRun.cpp @brief Exact resolved-index kernel: RGB остаётся позднему Main merge. */
#include "IndexColourRun.h"
#include "Palette.h"
#include <algorithm>
#include <cstring>

namespace nes::c6_colour::index_only {
namespace {
/** @brief Разворачивает одну bitplane в8 nibble selectors; constexpr Flash LUT,1024Б. */
constexpr std::array<std::uint32_t,256> makePlanes() noexcept {
    std::array<std::uint32_t,256> result{};
    for(unsigned b=0;b<256;++b)for(unsigned i=0;i<8;++i)if(b&(128U>>i))result[b]|=1U<<(i*4U);
    return result;
}
constexpr auto expanded=makePlanes();
/** @brief Собирает8 background palette selectors с точным fine-X и двумя attribute planes. */
inline std::uint32_t selectors(std::uint32_t p,std::uint32_t a,unsigned fine) noexcept {
    return expanded[(p>>(8U-fine))&255U]|(expanded[(p>>(24U-fine))&255U]<<1U)|
        (expanded[(a>>(8U-fine))&255U]<<2U)|(expanded[(a>>(24U-fine))&255U]<<3U);
}
/** @brief Читает4 sprite bytes без alias/alignment UB; частый aligned cursor даёт RV32 lw. */
inline std::uint32_t spriteWord(const std::uint8_t* bytes) noexcept {
    std::uint32_t word=0;
#if defined(__GNUC__)
    bytes=static_cast<const std::uint8_t*>(__builtin_assume_aligned(bytes,4));
#endif
    std::memcpy(&word,bytes,4);return word;
}
/** @brief Записывает8 indices двумя RV32 sw только после общего alignment proof; иначе безопасный memcpy. */
inline void storeWords(std::uint8_t* out,std::uint32_t first,std::uint32_t second,bool aligned) noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    static_cast<void>(aligned);
    for(unsigned i=0;i<4U;++i){out[i]=std::uint8_t(first>>(i*8U));out[i+4U]=std::uint8_t(second>>(i*8U));}
#else
    if(aligned){
#if defined(__GNUC__)
        auto* destination=static_cast<std::uint8_t*>(__builtin_assume_aligned(out,4));
#else
        auto* destination=out;
#endif
        std::memcpy(destination,&first,4);std::memcpy(destination+4,&second,4);
    }else{std::memcpy(out,&first,4);std::memcpy(out+4,&second,4);}
#endif
}
/** @brief Рисует8 exact indices; пустой sprite участок проверяет двумя32-битными loads вместо8 ветвей. */
inline void draw8(const Context& row,const Run& run,Result& out,unsigned x,unsigned cursor,std::uint32_t bg) noexcept {
    if(!(run.mask&8U))bg=0;
    else if(x<8U&&!(run.mask&2U)){const unsigned hidden=8U-x;bg=hidden==8U?0:bg&(~0U<<(hidden*4U));}
    const unsigned hidden=!(run.mask&16U)?8U:((x<8U&&!(run.mask&4U))?8U-x:0U);
    bool empty=hidden==8U||cursor>=256U;
    if(!empty){
        if(!hidden&&cursor<=248U&&!(cursor&3U))empty=(spriteWord(row.sprites.data()+cursor)|spriteWord(row.sprites.data()+cursor+4U))==0;
        else{empty=true;for(unsigned i=hidden;i<8U&&cursor+i<256U;++i)if(row.sprites[cursor+i]&3U){empty=false;break;}}
    }
    if(empty){
        const std::uint32_t first=std::uint32_t(row.indexPairs[bg&255U])|(std::uint32_t(row.indexPairs[(bg>>8U)&255U])<<16U);
        const std::uint32_t second=std::uint32_t(row.indexPairs[(bg>>16U)&255U])|(std::uint32_t(row.indexPairs[bg>>24U])<<16U);
        storeWords(out.indices.data()+x,first,second,(x&3U)==0);return;
    }
    for(unsigned i=0;i<8U;++i,bg>>=4U){
        const unsigned sp=i<hidden||cursor+i>=256U?0U:row.sprites[cursor+i],back=bg&15U;
        const unsigned selected=(sp&3U)&&(!(back&3U)||!(sp&16U))?16U+(sp&15U):((back&3U)?back:0U);
        out.indices[x+i]=row.indexPalette[selected];
    }
}
}

/** @brief Проверяет seed и подготавливает только caller-owned index context текущей строки. */
bool prepare(const RowSeed& seed,std::uint8_t mask,Context& row) noexcept {
    for(const auto&s:seed.sprites)if(s.flags&~3U)return false;
    const bool spriteChange=!row.valid||std::memcmp(row.descriptors.data(),seed.sprites.data(),sizeof(seed.sprites))!=0;
    const bool gray=(mask&1U)!=0;
    const bool paletteChange=!row.valid||gray!=row.gray||row.palette!=seed.palette;
    if(spriteChange){
        row.sprites.fill(0);
        for(const auto&s:seed.sprites){if(!(s.flags&1U))continue;
            const unsigned meta=((s.attributes&3U)<<2U)|((s.attributes&32U)?16U:0U)|((s.flags&2U)?32U:0U);
            for(unsigned bit=0;bit<8U&&s.x+bit<256U;++bit){
                if(row.sprites[s.x+bit])continue;
                const unsigned m=128U>>bit,pixel=((s.low&m)?1U:0U)|((s.high&m)?2U:0U);
                if(pixel)row.sprites[s.x+bit]=std::uint8_t(meta|pixel);
            }
        }
        row.descriptors=seed.sprites;
    }
    if(paletteChange){
        for(unsigned i=0;i<32U;++i)row.indexPalette[i]=seed.palette[i]&(gray?0x30U:0x3fU);
        for(unsigned pair=0;pair<256U;++pair){const unsigned a=pair&15U,b=pair>>4U;
            row.indexPairs[pair]=std::uint16_t(row.indexPalette[(a&3U)?a:0U])|(std::uint16_t(row.indexPalette[(b&3U)?b:0U])<<8U);
        }
        row.palette=seed.palette;row.gray=gray;
    }
    row.valid=true;return true;
}

/** @brief Проверяет run/capacity и сохраняет прежние index/Sprite0 результаты. */
bool render(const Context& row,const Run& in,Result& out) noexcept {
    Carry after{};if(!row.valid||row.gray!=bool(in.mask&1U)||!advanceRunCarry(in,after))return false;
    for(unsigned i=0;i<in.tiles;++i)if(in.fetched[i].attribute>3U)return false;
    auto p=std::uint32_t(in.before.low)|(std::uint32_t(in.before.high)<<16U);
    auto a=std::uint32_t(in.before.attrLow)|(std::uint32_t(in.before.attrHigh)<<16U);
    unsigned lo=in.before.nextLow,hi=in.before.nextHigh,attr=in.before.nextAttr,cursor=in.before.spriteClocks;
    for(unsigned i=0;i<in.tiles;++i){
        p=(p&0xff00ff00U)|lo|(hi<<16U);a=(a&0xff00ff00U)|((attr&1U)?255U:0U)|((attr&2U)?0x00ff0000U:0U);
        draw8(row,in,out,in.x+i*8U,cursor,selectors(p,a,in.fine));
        p=(p<<8U)&0xff00ff00U;a=(a<<8U)&0xff00ff00U;cursor=std::min(256U,cursor+8U);
        lo=in.fetched[i].low;hi=in.fetched[i].high;attr=in.fetched[i].attribute;
    }
    out.after=after;return true;
}

bool mergeDirty(std::uint16_t* rgb,std::uint8_t* scratch,const std::uint8_t* indices,const std::uint8_t* dirty,
    unsigned prefix,std::uint8_t emphasis,bool pal) noexcept {
    if(!rgb||!scratch||!indices||!dirty||prefix>256U||emphasis>7U)return false;
    std::array<std::uint16_t,64> palette{};
    for(unsigned i=0;i<64U;++i)palette[i]=::nes::core::displayRgb565(std::uint8_t(i),emphasis,pal);
    for(unsigned x=0;x<prefix;++x)if(dirty[x>>3U]&(1U<<(x&7U))){scratch[x]=indices[x];rgb[x]=palette[indices[x]&63U];}
    return true;
}
}
