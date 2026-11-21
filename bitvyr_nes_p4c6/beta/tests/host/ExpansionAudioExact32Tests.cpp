/**
 * @file ExpansionAudioExact32Tests.cpp
 * @brief Проверяет точную узкую арифметику против сохранённой независимой wide64 модели.
 * @details Exhaustive Q15/amplitude и attack domains, все fnum/block/multiplier/vibrato,
 * затем полный random register/M2 поток, события boxcar и PCM через настоящий renderer.
 */
#include "cartridge/Sunsoft5BAudio.h"
#include "cartridge/Vrc7Audio.h"
#include "reference64/Sunsoft5BAudioWide.h"
#include "reference64/Vrc7AudioWide.h"
#include "apu/AudioRenderer.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using namespace nes::core;
namespace {
std::uint64_t checks=0,kernelPairs=0,modelClocks=0,pcmSamples=0;
volatile std::uint32_t sink=0;
/** @brief Условие проверяется независимо от NDEBUG; mismatch завершает процесс ненулевым кодом. */
void require(bool ok,const char*why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
/** @brief Стабильный PRNG без platform random и без скрытых входов модели. */
std::uint32_t random(std::uint32_t&state){state^=state<<13U;state^=state>>17U;state^=state<<5U;return state;}
#if defined(__GNUC__)
#define TEST_NOINLINE __attribute__((noinline,noipa))
#else
#define TEST_NOINLINE
#endif
/** @brief Wide oracle с исходным делением signed64, округляющим к нулю. */
TEST_NOINLINE std::int32_t multiplyWide(std::int32_t v,std::int32_t a){return std::int32_t(std::int64_t(v)*a/32767);}
/** @brief Кандидат использует signed32; Q15^2 строго меньше INT32_MAX. */
TEST_NOINLINE std::int32_t multiplyNarrow(std::int32_t v,std::int32_t a){return v*a/32767;}
/** @brief Проверяет все 65535*32768 допустимых пар mod/carrier и amp, включая отрицательное округление. */
void exhaustiveQ15(){
    std::uint32_t hash=0;
    for(std::int32_t v=-32767;v<=32767;++v)for(std::int32_t a=0;a<=32767;++a){
        const auto wide=multiplyWide(v,a),narrow=multiplyNarrow(v,a);
        if(wide!=narrow){std::fprintf(stderr,"Q15 mismatch %d %d\n",v,a);std::exit(1);}
        hash=hash*33U+std::uint32_t(narrow);++kernelPairs;
    }
    sink=hash;require(kernelPairs==2147450880ULL,"complete signed Q15 domain");
}
/** @brief Исчерпывает bounded attack attenuation и все AR1..14, не подменяя AR0/15. */
void envelopeAndPhase(){
    std::uint64_t pairs=0;
    for(unsigned ar=1;ar<15;++ar)for(std::uint32_t attenuation=0;attenuation<=255U*4096U;++attenuation){
        const auto wide=std::uint32_t(std::uint64_t(attenuation)*(1U<<(ar-1U))/32768U);
        require(wide==(attenuation>>(16U-ar)),"attack exact power2 division");++pairs;
    }
    require(pairs==14622734ULL,"complete attack domain");
    constexpr unsigned multipliers[16]={1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
    for(unsigned fnum=0;fnum<512;++fnum)for(unsigned block=0;block<8;++block)for(auto m:multipliers){
        const auto increment=std::uint32_t(fnum)<<(block+13U);
        const auto wide=std::uint32_t(std::uint64_t(increment)*m/2U);
        const auto narrow=m==1U?increment>>1U:increment*(m>>1U);
        require(wide==narrow,"phase multiply modulo2^32");
        for(int k=-2;k<=2;++k){
            const auto expected=std::uint32_t(std::int64_t(wide)+std::int64_t(wide)*k/512);
            const auto depth=k<0?-k:k;
            const auto delta=depth?narrow>>(depth==2?8U:9U):0U;
            require(expected==(k<0?narrow-delta:narrow+delta),"vibrato exact signed truncation and wrap");
        }
    }
    for(std::int32_t v=-32767;v<=32767;++v){
        require(std::uint32_t(std::int64_t(v)*65536)==std::uint32_t(v*65536),"phase modulation fits signed32");
        for(unsigned feedback=1;feedback<8;++feedback){
            const auto wide=std::uint32_t(std::int64_t(v)*(1U<<(feedback+11U)));
            require(wide==(std::uint32_t(v)<<(feedback+11U)),"feedback exact unsigned modulo");
        }
    }
}
/** @brief Проверяет интерполяцию с исходными LUT всеми Q12 fractions и sine fraction границами/широким sweep. */
void interpolationAndArea(){
    // Все разности соседних sine-узлов ограничены201; для |d|<=201 product<2^31.
    static_assert(201LL*4194303LL<2147483647LL,"quarter-wave interpolation product bound");
    static_assert(32767LL*32767LL<2147483647LL,"signed Q15 product bound");
    static_assert(65534LL*4096LL<2147483647LL,"channel bias product bound");
    static_assert(49152LL*64LL<2147483647LL,"portable boxcar area bound");
    for(int difference=-201;difference<=201;++difference){
        for(std::uint32_t fraction: {0U,1U,2097151U,2097152U,4194302U,4194303U})
            require(std::int64_t(difference)*fraction/4194304==difference*std::int32_t(fraction)/4194304,"sine endpoints signed truncation");
    }
    for(std::uint32_t fraction=0;fraction<=4194303U;++fraction)
        for(int d:{-201,-1,1,201})require(std::int64_t(d)*fraction/4194304==d*std::int32_t(fraction)/4194304,"sine full fraction extreme slopes");
    // Q12 interpolation max adjacent gain step1385; exhaust every possible bounded difference/fraction.
    for(int d=-1385;d<=0;++d)for(std::int32_t f=0;f<4096;++f)
        require(std::int64_t(d)*f/4096==d*f/4096,"gain full Q12 fraction and slope domain");
    for(std::int32_t raw=-49152;raw<=0;++raw)for(std::int32_t step=0;step<=64;++step)
        require(std::int64_t(raw)*step==raw*step,"portable complete raw and step domain");
    for(std::int32_t sum=0;sum<=65534;++sum)
        require(std::int64_t(sum)*4096/32767==sum*4096/32767,"complete channel bias domain");
}
/** @brief Пишет одинаковый реальный bus strobe обеим моделям; unknown address также сохраняется. */
template<class Narrow,class Wide>void bus(Narrow&n,Wide&w,std::uint16_t address,std::uint8_t value){n.cpuWrite(address,value);w.cpuWrite(address,value);}
/** @brief Записывает canonical register без тайного продвижения времени. */
template<class Narrow,class Wide>void reg(Narrow&n,Wide&w,unsigned index,unsigned value,bool fm){
    bus(n,w,fm?0x9010:0xc000,std::uint8_t(index));bus(n,w,fm?0x9030:0xe000,std::uint8_t(value));
}
/** @brief Сверяет наблюдаемый DAC и обе clock boundaries; сравнение по каждому M2 ловит фазовый сдвиг. */
template<class Narrow,class Wide>void same(const Narrow&n,const Wide&w){
    require(n.sample()==w.sample(),"raw DAC wide equality");
    require(n.portableSample()==w.portableSample(),"selected64 DAC wide equality");
    require(n.nextOutputCpuCycles()==w.nextOutputCpuCycles(),"raw horizon wide equality");
    require(n.nextPortableOutputCpuCycles()==w.nextPortableOutputCpuCycles(),"selected horizon wide equality");
}
/** @brief Все15 FM patches и custom, затем длинный поток случайных MMIO/reset/mute и дробления clocks. */
template<class Narrow,class Wide>void model(bool fm){
    Narrow n;Wide w;std::uint32_t state=fm?0xcafef123U:0x639a415dU;
    for(unsigned instrument=0;instrument<(fm?16U:16U);++instrument){
        n.reset();w.reset();
        if(fm){
            for(unsigned r=0;r<8;++r)reg(n,w,r,random(state),true);
            for(unsigned ch=0;ch<6;++ch){reg(n,w,0x30U+ch,instrument<<4U,true);reg(n,w,0x10U+ch,0xffU-ch*23U,true);reg(n,w,0x20U+ch,0x11U+(ch<<1U),true);}
        }else{
            for(unsigned r=0;r<14;++r)reg(n,w,r,random(state),false);
            reg(n,w,7,0,false);reg(n,w,8,16,false);reg(n,w,9,15,false);reg(n,w,10,16,false);
            reg(n,w,11,1,false);reg(n,w,12,0,false);reg(n,w,13,instrument,false);
        }
        for(unsigned i=0;i<120000;++i){n.clockCpu();w.clockCpu();same(n,w);++modelClocks;}
    }
    for(unsigned event=0;event<120000;++event){
        const auto r=random(state);
        if(event%15001U==0){n.reset();w.reset();}
        if(fm&&event%701U==0)bus(n,w,0xe000,(r&1U)?0x40:0);
        reg(n,w,fm?r%64U:r%32U,random(state),fm);same(n,w);
        const auto clocks=1U+random(state)%127U;
        if((event&7U)==0){
            // Кандидат получает chunk, oracle одиночные M2; следующий отрезок снова сравнивает каждый такт.
            n.clockCpu(clocks);for(unsigned i=0;i<clocks;++i)w.clockCpu();same(n,w);modelClocks+=clocks;
        }else for(unsigned i=0;i<clocks;++i){n.clockCpu();w.clockCpu();same(n,w);++modelClocks;}
    }
}
/** @brief Строит реальные externalDacQ16 пакеты из обоих providers и сверяет PCM48k, включая boundary carry. */
template<class Narrow,class Wide>void pcm(bool fm){
    Narrow n;Wide w;AudioRenderer rn,rw;
    auto an=std::make_unique<AudioPacket>(),aw=std::make_unique<AudioPacket>();
    auto pn=std::make_unique<AudioPacket>(),pw=std::make_unique<AudioPacket>();
    std::uint32_t state=fm?0x5476fae1U:0x6144acdaU;
    if(fm){
        const unsigned patch[8]={0xf1,0xe1,0,7,0xf4,0xf4,0x84,0x84};
        for(unsigned i=0;i<8;++i)reg(n,w,i,patch[i],true);
        for(unsigned ch=0;ch<6;++ch){reg(n,w,0x10+ch,200+ch,true);reg(n,w,0x20+ch,0x19,true);}
    }else{
        reg(n,w,0,5,false);reg(n,w,2,7,false);reg(n,w,4,11,false);
        reg(n,w,6,3,false);reg(n,w,7,0,false);reg(n,w,8,15,false);reg(n,w,9,16,false);
        reg(n,w,10,13,false);reg(n,w,11,7,false);reg(n,w,13,10,false);
    }
    bool nonzero=false;
    for(unsigned frame=0;frame<40;++frame){
        const std::uint64_t base=std::uint64_t(frame)*30000U;
        an->begin(1,frame,base,Region::Ntsc,true);aw->begin(1,frame,base,Region::Ntsc,true);
        require(an->addInput(base,AudioPacket::clearSamplesOperation,0),"PCM clear narrow");
        require(aw->addInput(base,AudioPacket::clearSamplesOperation,0),"PCM clear wide");
        auto previous=n.portableSample();require(an->addExpansion(base,previous),"PCM initial narrow");require(aw->addExpansion(base,w.portableSample()),"PCM initial wide");
        for(unsigned offset=0;offset<30000;++offset){
            if(offset%193U==0)reg(n,w,fm?0x30U+random(state)%6U:8U+random(state)%3U,random(state),fm);
            n.clockCpu();w.clockCpu();same(n,w);
            if(n.portableSample()!=previous){previous=n.portableSample();require(an->addExpansion(base+offset+1U,previous),"PCM append narrow");require(aw->addExpansion(base+offset+1U,w.portableSample()),"PCM append wide");}
        }
        require(an->retireTo(*pn,base+30000U,frame+1U)&&aw->retireTo(*pw,base+30000U,frame+1U),"PCM retire");
        require(pn->header().expansionCount==pw->header().expansionCount,"event count bitmatch");
        require(std::memcmp(pn->expansions(),pw->expansions(),pn->header().expansionCount*sizeof(AudioPacket::Expansion))==0,"event tape bitmatch");
        require(rn.render(*pn)==AudioRenderResult::Ok&&rw.render(*pw)==AudioRenderResult::Ok,"PCM real renderer accepted");
        require(rn.sampleCount()==rw.sampleCount()&&rn.sampleCount()>0,"PCM sample count");
        require(std::memcmp(rn.samples(),rw.samples(),rn.sampleCount()*sizeof(std::int16_t))==0,"PCM bitmatch wide");
        pcmSamples+=rn.sampleCount();for(std::size_t i=0;i<rn.sampleCount();++i)nonzero|=rn.samples()[i]!=0;
    }
    require(nonzero,"PCM contains actual audible modulation");
}
}
/** @brief Выполняет kernel proof и две независимые полные модели без ROM/SDK/UART. */
int main(){
    exhaustiveQ15();envelopeAndPhase();interpolationAndArea();
    model<Sunsoft5BAudio,Sunsoft5BAudioWide>(false);model<Vrc7Audio,Vrc7AudioWide>(true);
    pcm<Sunsoft5BAudio,Sunsoft5BAudioWide>(false);pcm<Vrc7Audio,Vrc7AudioWide>(true);
    std::printf("PASS exact32 checks=%llu q15_pairs=%llu model_M2=%llu PCM_samples=%llu sizes_narrow=%zu/%zu sizes_wide=%zu/%zu sink=%u\n",
        (unsigned long long)checks,(unsigned long long)kernelPairs,(unsigned long long)modelClocks,(unsigned long long)pcmSamples,
        sizeof(Sunsoft5BAudio),sizeof(Vrc7Audio),sizeof(Sunsoft5BAudioWide),sizeof(Vrc7AudioWide),unsigned(sink));
}
