/** @file ResolvedColourCodec.cpp @brief Явный LE wire между P4/C6, независимый от ABI/padding. */
#include "ResolvedColourCodec.h"
#include <cstring>
namespace nes::c6_colour::wire {
namespace {
/** @brief Записывает16битное поле в явно проверенный byte buffer. */
void put16(std::uint8_t* p,std::uint16_t value) noexcept {p[0]=std::uint8_t(value);p[1]=std::uint8_t(value>>8U);}
/** @brief Читает16битное поле после проверки complete header size. */
std::uint16_t get16(const std::uint8_t* p) noexcept {return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8U);}
/** @brief Проверяет shape и все attributes без обращения к памяти вне tile count. */
bool valid(const Run& in) noexcept {
    Carry ignored{};if(!advanceRunCarry(in,ignored))return false;
    for(unsigned i=0;i<in.tiles;++i)if(in.fetched[i].attribute>3)return false;
    return true;
}
}
bool encodeSeed(const RowSeed& in,std::uint8_t* out,std::size_t capacity,std::size_t& written) noexcept {
    if(!out||capacity<seedBytes)return false;
    for(const auto& row:in.sprites)if(row.flags&~3U)return false;
    out[0]=1;out[1]=in.pal?1:0;std::memcpy(out+2,in.palette.data(),32);
    for(unsigned i=0;i<8;++i){auto*p=out+34+i*5;const auto&s=in.sprites[i];p[0]=s.low;p[1]=s.high;p[2]=s.x;p[3]=s.attributes;p[4]=s.flags;}
    written=seedBytes;return true;
}
bool decodeSeed(const std::uint8_t* in,std::size_t bytes,RowSeed& out) noexcept {
    if(!in||bytes!=seedBytes||in[0]!=1||in[1]>1)return false;
    RowSeed next{};next.pal=in[1]!=0;std::memcpy(next.palette.data(),in+2,32);
    for(unsigned i=0;i<8;++i){const auto*p=in+34+i*5;if(p[4]&~3U)return false;next.sprites[i]={p[0],p[1],p[2],p[3],p[4]};}
    out=next;return true;
}
bool encodeRun(const Run& in,std::uint8_t* out,std::size_t capacity,std::size_t& written) noexcept {
    if(!valid(in)||!out||capacity<runHeaderBytes+in.tiles*3U)return false;
    std::memset(out,0,runHeaderBytes);out[0]=1;out[1]=in.tiles;put16(out+2,in.x);put16(out+4,in.y);out[6]=in.mask;out[7]=in.fine;
    put16(out+8,in.before.low);put16(out+10,in.before.high);put16(out+12,in.before.attrLow);put16(out+14,in.before.attrHigh);
    out[16]=in.before.nextLow;out[17]=in.before.nextHigh;out[18]=in.before.nextAttr;put16(out+20,in.before.spriteClocks);
    for(unsigned i=0;i<in.tiles;++i){auto*p=out+runHeaderBytes+i*3;const auto&t=in.fetched[i];p[0]=t.low;p[1]=t.high;p[2]=t.attribute;}
    written=runHeaderBytes+in.tiles*3U;return true;
}
bool decodeRun(const std::uint8_t* in,std::size_t bytes,Run& out) noexcept {
    if(!in||bytes<runHeaderBytes||in[0]!=1||!in[1]||in[1]>32||bytes!=runHeaderBytes+in[1]*3U||in[19]||in[22]||in[23])return false;
    Run next{};next.tiles=in[1];next.x=get16(in+2);next.y=get16(in+4);next.mask=in[6];next.fine=in[7];
    next.before.low=get16(in+8);next.before.high=get16(in+10);next.before.attrLow=get16(in+12);next.before.attrHigh=get16(in+14);
    next.before.nextLow=in[16];next.before.nextHigh=in[17];next.before.nextAttr=in[18];next.before.spriteClocks=get16(in+20);
    for(unsigned i=0;i<next.tiles;++i){const auto*p=in+runHeaderBytes+i*3;next.fetched[i]={p[0],p[1],p[2]};}
    if(!valid(next))return false;
    out=next;return true;
}
}
