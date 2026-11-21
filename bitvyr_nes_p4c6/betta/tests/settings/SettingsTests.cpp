#include "ConsoleSettings.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace nes::app;
/**
 * @brief Сравнивает все логические настройки, не полагаясь на padding и бинарный layout C++-структуры.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static bool same(const ConsoleSettings& a,const ConsoleSettings& b) {
    return a.volume==b.volume && a.muted==b.muted && a.swapAB==b.swapAB && a.showFps==b.showFps && a.region==b.region;
}
/**
 * @brief Пересчитывает CRC первых12байт тестового blob, чтобы отдельно проверять диапазоны и reserved-поля при валидной контрольной сумме.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static void crc(EncodedSettings& bytes) {
    const auto value=settings_detail::checksum(bytes.data(),12);
    for(unsigned i=0;i<4;++i)bytes[12+i]=static_cast<std::uint8_t>(value>>(8*i));
}
/**
 * @brief Требует отказа decode для диапазона bytes/length и восстановления тихих defaults вместо частично принятых полей.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
static void invalid(const std::uint8_t* bytes,std::size_t length) {
    ConsoleSettings out{};out.volume=100;out.muted=false;out.swapAB=true;out.region=RegionMode::Pal;
    assert(!decodeSettings(bytes,length,out) && same(out,ConsoleSettings{}));
}
/**
 * @brief Проверяет сериализацию всех допустимых настроек, обмен A/B для256масок, CRC, одиночные повреждения и ограничения формата.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int main() {
    ConsoleSettings defaults{};assert(defaults.volume==1 && defaults.muted && !defaults.swapAB && defaults.showFps);
    assert(defaults.region==RegionMode::Ntsc);
    assert(defaults.effectivePal(false)==false && defaults.effectivePal(true)==false);
    assert(settings_detail::checksum(reinterpret_cast<const std::uint8_t*>("123456789"),9)==0xcbf43926U);
    EncodedSettings original{};assert(encodeSettings(defaults,original));
    const std::uint8_t prefix[]{'N','E','S','C',1,16,1,5,1,0,0,0};
    assert(!std::memcmp(original.data(),prefix,12));
    unsigned rounds=0;
    for(unsigned volume=0;volume<=100;++volume)for(unsigned flags=0;flags<8;++flags)for(unsigned region=0;region<3;++region) {
        ConsoleSettings s{};s.volume=static_cast<std::uint8_t>(volume);s.muted=(flags&1)!=0;s.swapAB=(flags&2)!=0;s.showFps=(flags&4)!=0;s.region=static_cast<RegionMode>(region);
        EncodedSettings bytes{};ConsoleSettings out{};assert(encodeSettings(s,bytes));assert(decodeSettings(bytes.data(),bytes.size(),out)&&same(s,out));++rounds;
        for(bool hint:{false,true}) assert(s.effectivePal(hint)==(region==2 || (region==0&&hint)));
    }
    for(unsigned mask=0;mask<256;++mask) {
        ConsoleSettings s{};assert(s.mapGamepadButtons(static_cast<std::uint8_t>(mask))==mask);s.swapAB=true;
        const auto mapped=s.mapGamepadButtons(static_cast<std::uint8_t>(mask));
        assert((mapped&0xfc)==(mask&0xfc) && (mapped&1)==((mask&2)>>1) && (mapped&2)==((mask&1)<<1));
        assert(s.mapGamepadButtons(mapped)==mask);
    }
    for(unsigned bit=0;bit<128;++bit){auto bytes=original;bytes[bit/8]^=static_cast<std::uint8_t>(1U<<(bit%8));invalid(bytes.data(),bytes.size());}
    for(unsigned value=101;value<256;++value){auto bytes=original;bytes[6]=static_cast<std::uint8_t>(value);crc(bytes);invalid(bytes.data(),bytes.size());}
    for(unsigned value=3;value<256;++value){auto bytes=original;bytes[8]=static_cast<std::uint8_t>(value);crc(bytes);invalid(bytes.data(),bytes.size());}
    for(unsigned value=8;value<256;++value){auto bytes=original;bytes[7]=static_cast<std::uint8_t>(value);crc(bytes);invalid(bytes.data(),bytes.size());}
    for(unsigned index:{0U,1U,2U,3U,4U,5U,9U,10U,11U}){auto bytes=original;bytes[index]^=1;crc(bytes);invalid(bytes.data(),bytes.size());}
    invalid(nullptr,16);invalid(nullptr,0);invalid(original.data(),15);invalid(original.data(),17);
    ConsoleSettings bad{};bad.volume=255;EncodedSettings zero{};zero.fill(0xff);assert(!encodeSettings(bad,zero));for(auto b:zero)assert(b==0);
    bad=ConsoleSettings{};bad.region=static_cast<RegionMode>(255);assert(!encodeSettings(bad,zero));
    std::printf("SETTINGS_PURE_PASS roundtrips=%u mask_cases=256 single_bit_corruptions=128 ranges_crc_reserved_size\n",rounds);
    std::printf("DEFAULT_BYTES ");for(auto b:original)std::printf("%02x",b);std::puts("");
}
