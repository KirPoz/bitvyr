/**
 * @file CartridgeExpansionAudioTests.cpp
 * @brief Независимые timing/pitch/ADSR/gating/budget проверки собственных5B/VRC7 без ROM/SDK/COM.
 */
#include "cartridge/Sunsoft5BAudio.h"
#include "cartridge/Vrc7Audio.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
using nes::core::Sunsoft5BAudio;
using nes::core::Vrc7Audio;
namespace {
unsigned checks=0;
/** @brief Fail-fast test assertion; выражение считается ровно один раз. */
void require(bool value,const char*name){++checks;if(!value){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}}
/** @brief Пишет один5B register через настоящий select/data интерфейс. */
void write(Sunsoft5BAudio&a,unsigned reg,unsigned value){a.cpuWrite(0xc000,std::uint8_t(reg));a.cpuWrite(0xe000,std::uint8_t(value));}
/** @brief Пишет один VRC7 register через canonical select/data интерфейс, без скрытых clocks. */
void write(Vrc7Audio&a,unsigned reg,unsigned value){a.cpuWrite(0x9010,std::uint8_t(reg));a.cpuWrite(0x9030,std::uint8_t(value));}
/** @brief Независимая double-формула номинального5B DAC вместо production LUT. */
int expectedDac(unsigned level){return level<2?0:-int(std::lround(12000.0*std::pow(10.0,-1.5*double(31-level)/20.0)));}
/** @brief Независимое табличное определение CAaH по номеру ramp/step, без production state machine. */
unsigned envelopeLevel(unsigned shape,unsigned steps){
    const bool attack=(shape&4U)!=0;
    if(steps<32)return attack?steps:31U-steps;
    if(!(shape&8U))return 0;
    if(shape&1U)return (attack!=bool(shape&2U))?31U:0U;
    const bool up=attack!=bool((shape&2U)&&((steps/32U)&1U));
    return up?steps%32U:31U-steps%32U;
}
/** @brief Настраивает5B toneA, без noise; period задаётся в16-M2 half-wave ticks. */
void tone(Sunsoft5BAudio&a,unsigned period){write(a,0,period&255U);write(a,1,period>>8U);write(a,7,0x3e);write(a,8,15);}
/** @brief Быстрая custom FM с неограниченным sustain; почти чистый carrier для независимого pitch теста. */
void custom(Vrc7Audio&a){
    const unsigned patch[8]={0x21,0x21,0x3f,0,0xf0,0xf0,0x0f,0x0f};
    for(unsigned i=0;i<8;++i)write(a,i,patch[i]);
}
/** @brief fnum288,block4 даёт72 периода за8192 FM samples независимо от nominal CPU Hz. */
void note(Vrc7Audio&a,unsigned channel=0,unsigned instrument=0,unsigned volume=0){
    write(a,0x30U+channel,(instrument<<4U)|volume);write(a,0x10U+channel,0x20);write(a,0x20U+channel,0x19);
}
/** @brief FNV64 всех signed DAC samples; одновременно проверяет слышимую изменчивость, а не одно ненулевое DC. */
std::uint64_t signature(Vrc7Audio&a,unsigned count,int&minimum,int&maximum){
    std::uint64_t hash=1469598103934665603ULL;minimum=0;maximum=-49152;
    for(unsigned i=0;i<count;++i){a.clockCpu(36);const auto v=a.sample();minimum=std::min(minimum,v);maximum=std::max(maximum,v);hash=(hash^std::uint32_t(v))*1099511628211ULL;
        require(v>=-49152&&v<=0,"VRC Q16 bound");}
    return hash;
}
/** @brief5B timing и запрет select upper nibble; period high mask не превращает4 в3844. */
void sunsoftRegisters(){
    Sunsoft5BAudio a;require(a.sample()==0&&a.portableSample()==0,"5B reset silent");
    write(a,7,0x3f);a.cpuWrite(0xdfff,0x18);a.cpuWrite(0xffff,15);require(a.sample()==0,"5B invalid select disables write");
    a.cpuWrite(0xcfff,8);a.cpuWrite(0xefff,15);require(a.sample()==-12000,"5B mirrored data and DC gates");
    a.reset();tone(a,4);write(a,1,0xf0);
    a.clockCpu(63);require(a.sample()==0,"5B period4 before edge");
    a.clockCpu();require(a.sample()==-12000,"5B period4 edge64");
    a.clockCpu(64);require(a.sample()==0,"5B frequency M2/(32P)");
    Sunsoft5BAudio b,c;tone(b,0);tone(c,1);
    for(unsigned i=0;i<1024;++i){b.clockCpu();c.clockCpu();require(b.sample()==c.sample(),"5B period0 equals1");}
    a.reset();write(a,0,8);write(a,7,0x3e);a.clockCpu(96);write(a,8,15);write(a,0,2);
    a.clockCpu(15);require(a.sample()==0,"5B period shortening retains phase");
    a.clockCpu();require(a.sample()==-12000,"5B shortening flips next16edge");
    a.reset();tone(a,4);write(a,8,0);a.clockCpu(64);write(a,8,15);require(a.sample()==-12000,"5B muted oscillator keeps phase");
}
/** @brief Проверяет polynomial17/14 по независимому LFSR и AND tone/noise, без доступа к полям модели. */
void sunsoftNoise(){
    Sunsoft5BAudio a;write(a,6,1);write(a,7,0x37);write(a,8,15);std::uint32_t lfsr=1;
    bool low=false,high=false;
    for(unsigned i=0;i<1000;++i){a.clockCpu(32);lfsr=((lfsr<<1U)|(((lfsr>>16U)^(lfsr>>13U))&1U))&0x1ffffU;
        const auto expected=(lfsr&0x10000U)?-12000:0;require(a.sample()==expected,"5B noise polynomial/cadence");low|=a.sample()==0;high|=a.sample()!=0;}
    require(low&&high,"5B noise nonconstant");
    Sunsoft5BAudio t,n,both;for(auto*p:{&t,&n,&both}){write(*p,0,3);write(*p,6,2);write(*p,8,15);}
    write(t,7,0x3e);write(n,7,0x37);write(both,7,0x36);
    for(unsigned i=0;i<3000;++i){t.clockCpu();n.clockCpu();both.clockCpu();require(both.sample()==((t.sample()&&n.sample())?-12000:0),"5B tone AND noise");}
}
/** @brief Все16 shapes,32 envelope steps, rate0=1 и reset одинаковой shape проверяются через DAC. */
void sunsoftEnvelope(){
    for(unsigned shape=0;shape<16;++shape){Sunsoft5BAudio a;write(a,7,0x3f);write(a,8,16);write(a,11,1);write(a,13,shape);
        for(unsigned i=0;i<130;++i){require(a.sample()==expectedDac(envelopeLevel(shape,i)),"5B all envelope shapes");a.clockCpu(16);}
        write(a,13,shape);require(a.sample()==expectedDac(envelopeLevel(shape,0)),"5B same shape retriggers");}
    Sunsoft5BAudio a,b;for(auto*p:{&a,&b}){write(*p,7,0x3f);write(*p,8,16);write(*p,13,10);}write(a,11,0);write(b,11,1);
    for(unsigned i=0;i<2048;++i){a.clockCpu();b.clockCpu();require(a.sample()==b.sample(),"5B envelope period0 equals1");}
}
/** @brief Проверяет точное причинное среднее64 M2 и chunk invariance по независимому поклоковому интегралу. */
template<class Audio>void boxcarAndChunk(Audio configured){
    Audio a=configured,b=configured;
    std::int64_t area=0;
    for(unsigned i=0;i<4096;++i){area+=a.sample();a.clockCpu();require(a.nextPortableOutputCpuCycles()>=1&&a.nextPortableOutputCpuCycles()<=64,"portable horizon range");
        if((i+1U)%64U==0){require(a.portableSample()==area/64,"portable exact64 area");area=0;}}
    b.clockCpu(4096);require(a.sample()==b.sample()&&a.portableSample()==b.portableSample(),"clock chunk invariant");
    b.clockCpu(0);require(a.sample()==b.sample(),"clock0 no change");
    unsigned changes=0;auto previous=a.portableSample();
    for(unsigned i=0;i<33248;++i){a.clockCpu();const auto value=a.portableSample();require(value>=-95070&&value<=0,"existing packet DAC range");if(value!=previous){++changes;previous=value;}}
    require(changes<=521,"PAL-frame portable publication bound");
}
/** @brief Независимая проверка частоты чистого carrier и полной шести-channel амплитуды. */
void vrcPitchAndChannels(){
    Vrc7Audio a;custom(a);note(a);unsigned crossings=0;auto previous=a.sample();int minimum=0,maximum=-49152;
    for(unsigned i=0;i<8192;++i){a.clockCpu(36);const auto v=a.sample();if(previous<-4096&&v>=-4096)++crossings;previous=v;minimum=std::min(minimum,v);maximum=std::max(maximum,v);}
    require(crossings>=70&&crossings<=74,"VRC pitch fnum/block law");require(maximum-minimum>7000,"VRC audible AC not DC");
    Vrc7Audio single,six;custom(single);custom(six);note(single);for(unsigned i=0;i<6;++i)note(six,i);
    for(unsigned i=0;i<2048;++i){single.clockCpu(36);six.clockCpu(36);require(six.sample()==single.sample()*6,"VRC all6 independent voices");require(six.sample()>=-49152,"VRC six-voice Q16 sum bound");}
    Vrc7Audio quiet;custom(quiet);note(quiet,0,0,15);int low,high;signature(quiet,4096,low,high);
    require(high-low>1&&high-low<100,"VRC volume15 attenuated not mute");
}
/** @brief Все15 аппаратных fixed patches звучат и дают разные bounded traces. */
void vrcPresets(){
    std::set<std::uint64_t>signatures;
    for(unsigned instrument=1;instrument<16;++instrument){Vrc7Audio a;note(a,0,instrument);int low,high;
        const auto hash=signature(a,16000,low,high);require(high-low>8,"VRC fixed patch audible");signatures.insert(hash);}
    require(signatures.size()==15,"VRC fixed patches distinct");
    Vrc7Audio a,b;custom(a);custom(b);write(b,2,0);write(b,3,7);note(a);note(b);int low,high;
    const auto x=signature(a,4096,low,high),y=signature(b,4096,low,high);require(x!=y,"VRC custom TL/feedback sound sensitivity");
}
/** @brief Key edge, same key, release, invalid register и E000 reset; raw0f phase test не останавливает envelope. */
void vrcControl(){
    Vrc7Audio a;require(a.sample()==0&&a.portableSample()==0,"VRC reset silent");custom(a);note(a);a.clockCpu(3600);auto b=a;
    write(a,0x20,0x19);for(unsigned i=0;i<200;++i){a.clockCpu(36);b.clockCpu(36);require(a.sample()==b.sample(),"VRC same key does not retrigger");}
    write(a,0x20,0x09);a.clockCpu(36*256);require(a.sample()==0,"VRC carrier keyoff releases");
    write(a,0x20,0x19);a.clockCpu(36);require(a.sample()!=0,"VRC keyon retriggers");
    a.setMuted(true);require(a.sample()==0,"VRC E000 immediate raw reset");custom(a);note(a);a.clockCpu(3600);require(a.sample()==0&&a.portableSample()==0,"VRC reset ignores FM writes");a.setMuted(false);a.clockCpu(3600);require(a.sample()==0,"VRC reset cleared register bank");
    custom(a);note(a);write(a,15,4);a.clockCpu(3600);require(a.sample()==0,"VRC phase test silent");
    write(a,15,0);a.clockCpu(36);require(a.sample()!=0,"VRC phase test envelopes continued");
    b=a;write(a,0x16,0xff);write(a,0x26,0xff);write(a,0x36,0xff);write(a,0x80,0xff);
    for(unsigned i=0;i<100;++i){a.clockCpu(36);b.clockCpu(36);require(a.sample()==b.sample(),"VRC invalid registers ignored");}
}
/** @brief AM/PM/KSR/KSL/waveform имеют наблюдаемое влияние; упрощение не подменяет flags молчанием. */
void vrcParameterSensitivity(){
    auto sound=[](unsigned carrierFlags,unsigned levelFlags,unsigned wave,unsigned attack,unsigned control,unsigned testFlags){
        Vrc7Audio a;custom(a);write(a,1,carrierFlags);write(a,2,levelFlags);write(a,3,wave);write(a,5,attack);
        write(a,15,testFlags);note(a);write(a,0x20,control);int low,high;return signature(a,16000,low,high);
    };
    const auto base=sound(0x21,0x3f,0,0xf0,0x19,0);
    require(base!=sound(0xa1,0x3f,0,0xf0,0x19,0),"VRC AM changes sound");
    require(base!=sound(0x61,0x3f,0,0xf0,0x19,0),"VRC PM changes sound");
    require(base!=sound(0x21,0x3f,0x10,0xf0,0x19,0),"VRC carrier halfwave changes sound");
    require(sound(0x21,0x3f,0,0x40,0x1f,0)!=sound(0x31,0x3f,0,0x40,0x1f,0),"VRC KSR changes envelope");
    require(sound(0x21,0x3f,0,0xf0,0x1f,0)!=sound(0x21,0x3f,0xc0,0xf0,0x1f,0),"VRC KSL changes level");
    require(sound(0xa1,0x3f,0,0xf0,0x19,0)!=sound(0xa1,0x3f,0,0xf0,0x19,8),"VRC fast LFO test changes AM");
    Vrc7Audio a;custom(a);write(a,7,0);note(a);a.clockCpu(3600);write(a,0x20,9);int low,high;signature(a,1000,low,high);require(high-low>7000,"VRC rate0 release halts");
    a.reset();custom(a);write(a,5,0x40);note(a);auto b=a;write(a,15,1);
    for(unsigned i=0;i<8000;++i){a.clockCpu(36);b.clockCpu(36);}
    write(a,15,0);a.clockCpu(36);b.clockCpu(36);require(a.sample()==b.sample(),"VRC force envelope test keeps envelope clock");
    a.reset();custom(a);write(a,1,0x61);note(a);write(a,15,8);a.clockCpu(36*5);a.setMuted(true);a.setMuted(false);
    b.reset();for(auto*p:{&a,&b}){custom(*p);write(*p,1,0x61);note(*p);write(*p,15,8);}
    bool difference=false;
    for(unsigned i=0;i<16;++i){a.clockCpu(36);b.clockCpu(36);difference|=a.sample()!=b.sample();}
    require(difference,"VRC sound reset preserves vibrato phase");
}
}
/** @brief Выполняет направленные native проверки, печатает layout для интеграционного бюджета. */
int main(){
    sunsoftRegisters();sunsoftNoise();sunsoftEnvelope();Sunsoft5BAudio s;tone(s,1);boxcarAndChunk(s);
    vrcPitchAndChannels();vrcPresets();vrcControl();vrcParameterSensitivity();Vrc7Audio v;custom(v);note(v);boxcarAndChunk(v);
    std::printf("PASS expansion audio checks=%u sizeof5B=%zu sizeofVRC7=%zu portable_period=64\n",checks,sizeof(Sunsoft5BAudio),sizeof(Vrc7Audio));
    return 0;
}

