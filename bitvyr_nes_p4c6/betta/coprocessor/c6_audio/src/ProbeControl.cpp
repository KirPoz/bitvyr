#include "ProbeControl.h"
#include "FixedFilter.h"
#include "FloatReference.h"
#include "FilterFixtures.h"
#include "Crc32IEEE.h"
#include <array>
#include <cstring>
#include <limits>

namespace nes::audio_probe { namespace {
// Не стек4096Б: единственный существующий worker владеет этими двумя3200Б arrays.
std::array<std::int16_t, sampleLimit> fixedPcm{}, floatPcm{};

/** @brief Записывает ограниченное LE поле результата независимо от C++ ABI. */
void put(std::uint8_t* to, std::uint64_t value, unsigned bytes) noexcept {
    for (unsigned i=0;i<bytes;++i) to[i]=static_cast<std::uint8_t>(value>>(8U*i));
}
/** @brief Считает CRC32 IEEE little-endian PCM; за измеренным kernel интервалом. */
std::uint32_t pcmCrc(const std::int16_t* pcm, std::size_t count) noexcept {
    std::uint32_t crc=0xffffffffU;
    for (std::size_t i=0;i<count;++i) for (unsigned byte=0;byte<2;++byte) {
        crc^=(static_cast<std::uint16_t>(pcm[i])>>(8U*byte))&0xffU;
        crc=(crc>>8U)^nes::c6raw::crc_detail::table[crc&255U];
    }
    return ~crc;
}
/** @brief Восстанавливает exact binary32 состояние memcpy, без численного преобразования. */
FloatState initialFloat(const fixtures::Fixture& fixture) noexcept {
    FloatState value{};
    static_assert(sizeof(value)==sizeof(fixture.initialFloatBits),"five binary32 states");
    std::memcpy(&value,fixture.initialFloatBits.data(),sizeof(value));
    return value;
}
} // namespace

std::size_t filterProbe(const std::uint8_t* request,std::size_t requestBytes,
    std::uint8_t* output,std::size_t capacity,ReadCycles readCycles) noexcept {
    constexpr std::size_t replyBytes=144;
    if (!request||requestBytes!=8||!output||capacity<replyBytes||!readCycles||
        request[0]!=1||request[1]>=fixtures::count||request[2]<1||request[2]>16||
        request[3]||request[4]||request[5]||request[6]||request[7]) return 0;
    const auto& fixture=fixtures::all[request[1]];
    FixedState fixed=fixture.initialFixed;
    FloatState exact=initialFloat(fixture);
    // Один нетаймируемый warmup каждого пути; cold-cache цена этим leaf не доказывается.
    if (!fixedBlock(fixed,fixture.integral,fixture.samples,fixedPcm.data())) return 0;
    floatBlock(exact,fixture.integral,fixture.samples,floatPcm.data());
    std::uint32_t fixedMin=std::numeric_limits<std::uint32_t>::max(),fixedMax=0;
    std::uint32_t floatMin=std::numeric_limits<std::uint32_t>::max(),floatMax=0;
    std::uint64_t fixedSum=0,floatSum=0;
    for (unsigned iteration=0;iteration<request[2];++iteration) {
        fixed=fixture.initialFixed;
        auto begin=readCycles();
        const bool ok=fixedBlock(fixed,fixture.integral,fixture.samples,fixedPcm.data());
        const auto fixedCycles=readCycles()-begin;
        if (!ok) return 0;
        exact=initialFloat(fixture);
        begin=readCycles();floatBlock(exact,fixture.integral,fixture.samples,floatPcm.data());
        const auto floatCycles=readCycles()-begin;
        if (fixedCycles<fixedMin) fixedMin=fixedCycles;
        if (fixedCycles>fixedMax) fixedMax=fixedCycles;
        if (floatCycles<floatMin) floatMin=floatCycles;
        if (floatCycles>floatMax) floatMax=floatCycles;
        fixedSum+=fixedCycles;floatSum+=floatCycles;
    }
    unsigned mismatch=0,floatMismatch=0,maxError=0,clipped=0;
    std::uint64_t squares=0,maxIntegral=0;
    for (std::size_t i=0;i<fixture.samples;++i) {
        const int delta=int(fixedPcm[i])-int(fixture.pcm[i]);
        const unsigned absolute=unsigned(delta<0?-delta:delta);
        mismatch+=absolute!=0;floatMismatch+=floatPcm[i]!=fixture.pcm[i];
        if (absolute>maxError) maxError=absolute;
        squares+=std::uint64_t(absolute)*absolute;
        clipped+=fixedPcm[i]==32767||fixedPcm[i]==-32768;
        const auto integral=fixture.integral[i];
        const auto magnitude=std::uint64_t(integral<0?-integral:integral);
        if (magnitude>maxIntegral) maxIntegral=magnitude;
    }
    std::memset(output,0,replyBytes);
    put(output,1,4);put(output+4,request[1],4);put(output+8,fixture.samples,4);put(output+12,request[2],4);
    put(output+16,fixedMin,4);put(output+20,fixedMax,4);put(output+24,fixedSum,8);
    put(output+32,floatMin,4);put(output+36,floatMax,4);put(output+40,floatSum,8);
    put(output+48,mismatch,4);put(output+52,maxError,4);put(output+56,squares,8);
    put(output+64,floatMismatch,4);put(output+68,pcmCrc(fixedPcm.data(),fixture.samples),4);
    put(output+72,pcmCrc(floatPcm.data(),fixture.samples),4);put(output+76,pcmCrc(fixture.pcm,fixture.samples),4);
    const std::int32_t fixedFields[]={fixed.highPass90,fixed.highPass440,fixed.lowPass14000,fixed.previousInput,fixed.previousHighPass90};
    std::array<std::uint32_t,5> floatBits{};std::memcpy(floatBits.data(),&exact,sizeof(exact));
    for (unsigned i=0;i<5;++i) {put(output+80+4*i,static_cast<std::uint32_t>(fixedFields[i]),4);put(output+100+4*i,floatBits[i],4);}
    put(output+120,maxIntegral,8);put(output+128,clipped,4);
    put(output+132,fixture.romCRC,4);put(output+136,fixture.inputFrame,4);
    put(output+140,1,4); // warmup excluded; state reset before every repeated timing.
    return replyBytes;
}
} // namespace nes::audio_probe
