#include "Crc32IEEE.h"
#include "RawWire.h"
#include <cstring>
namespace nes::c6raw { namespace {
/** @brief Пишет LE без невыровненного доступа RV32. */
void put(std::uint8_t* out, std::uint64_t value, unsigned bytes) noexcept { for(unsigned i=0;i<bytes;++i) out[i]=std::uint8_t(value>>(8U*i)); }
/** @brief Читает LE независимо от target ABI. */
std::uint64_t get(const std::uint8_t* in, unsigned bytes) noexcept { std::uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=std::uint64_t(in[i])<<(8U*i);return value; }
}
/** @brief Считает IEEE CRC по borrowed extent без изменения bytes; nullptr допустим только при нулевой длине. */
std::uint32_t crc(const std::uint8_t* data,std::size_t bytes) noexcept { return crc_detail::compute(data,bytes); }
/** @brief После проверки key/capacity записывает LE header и payload, затем два CRC; отказ0 не передаёт владение output. */
std::size_t encode(std::uint8_t* out,std::size_t capacity,const Key& key,std::uint8_t kind,std::uint8_t flags,const std::uint8_t* payload,std::size_t bytes) noexcept {
    if((kind!=1&&kind!=2)||!out||capacity<headerBytes||bytes>capacity-headerBytes||
       bytes>(kind==1?requestPayloadBytes:payloadBytes)||(!payload&&bytes)||key.slot>=slotCount||
       key.begin>key.end||key.end>=45000||!key.generation||!key.lease||!key.session)return 0;
    std::memset(out,0,headerBytes);put(out,0x3653454eU,4);out[4]=1;out[5]=headerBytes;out[6]=kind;out[7]=key.slot;
    put(out+8,key.generation,8);put(out+16,key.lease,8);put(out+24,key.frame,4);put(out+28,key.sequence,4);put(out+32,key.begin,4);put(out+36,key.end,4);
    put(out+40,key.opcode,2);out[42]=key.region;out[43]=flags;put(out+44,bytes,2);put(out+56,key.session,8);
    if(bytes&&payload!=out+headerBytes)std::memcpy(out+headerBytes,payload,bytes);
    put(out+48,crc(out+headerBytes,bytes),4);put(out+52,crc(out,headerBytes),4);return headerBytes+bytes;
}
/** @brief Проверяет bounded header и оба CRC до публикации локального View; при false caller output прежний. */
bool decode(const std::uint8_t* packet,std::size_t bytes,View& out) noexcept {
    if(!packet||bytes<headerBytes||bytes>packetBytes||get(packet,4)!=0x3653454eU||packet[4]!=1||packet[5]!=headerBytes||packet[7]>=slotCount||packet[46]||packet[47]|| (packet[6]!=1&&packet[6]!=2))return false;
    if(packet[6]==1&&bytes>requestPacketBytes)return false;
    const auto size=std::size_t(get(packet+44,2));if(size!=bytes-headerBytes||crc(packet+headerBytes,size)!=get(packet+48,4))return false;
    std::uint8_t header[headerBytes];std::memcpy(header,packet,headerBytes);std::memset(header+52,0,4);if(crc(header,headerBytes)!=get(packet+52,4))return false;
    View value{};value.key={get(packet+8,8),get(packet+16,8),get(packet+56,8),std::uint32_t(get(packet+24,4)),std::uint32_t(get(packet+28,4)),std::uint32_t(get(packet+32,4)),std::uint32_t(get(packet+36,4)),std::uint16_t(get(packet+40,2)),packet[7],packet[42]};
    if(!value.key.generation||!value.key.lease||!value.key.session||value.key.begin>value.key.end||value.key.end>=45000)return false;
    value.kind=packet[6];value.flags=packet[43];value.payload=packet+headerBytes;value.size=size;out=value;return true;
}
/** @brief Сверяет все identity/cut поля без сокращения до frame или sequence; borrowed данные не читает. */
bool equal(const Key& a,const Key& b) noexcept {return a.generation==b.generation&&a.lease==b.lease&&a.session==b.session&&a.frame==b.frame&&a.sequence==b.sequence&&a.begin==b.begin&&a.end==b.end&&a.opcode==b.opcode&&a.slot==b.slot&&a.region==b.region;}
}
