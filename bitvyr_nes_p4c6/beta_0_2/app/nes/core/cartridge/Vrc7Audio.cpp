/**
 * @file Vrc7Audio.cpp
 * @brief Собственный portable two-operator FM; не порт стороннего OPLL.
 * @details Факты: https://www.nesdev.org/wiki/VRC7_audio и оригинальные
 * исследования register reset https://forums.nesdev.org/viewtopic.php?t=17745 .
 * ROM bytes ниже — аппаратные параметры; sine/gain LUT самостоятельно рассчитаны.
 */
#include "Vrc7Audio.h"
#include <algorithm>
namespace nes::core {
namespace {
constexpr std::uint32_t envelopeMaximum=255U*4096U;
// Фактический fixed instrument ROM из NESdev, включая custom slot0.
// Это аппаратные числовые данные, не алгоритм/код стороннего emulator.
constexpr std::uint8_t patches[16][8]={
 {0,0,0,0,0,0,0,0},
 {0x03,0x21,0x05,0x06,0xe8,0x81,0x42,0x27},
 {0x13,0x41,0x14,0x0d,0xd8,0xf6,0x23,0x12},
 {0x11,0x11,0x08,0x08,0xfa,0xb2,0x20,0x12},
 {0x31,0x61,0x0c,0x07,0xa8,0x64,0x61,0x27},
 {0x32,0x21,0x1e,0x06,0xe1,0x76,0x01,0x28},
 {0x02,0x01,0x06,0x00,0xa3,0xe2,0xf4,0xf4},
 {0x21,0x61,0x1d,0x07,0x82,0x81,0x11,0x07},
 {0x23,0x21,0x22,0x17,0xa2,0x72,0x01,0x17},
 {0x35,0x11,0x25,0x00,0x40,0x73,0x72,0x01},
 {0xb5,0x01,0x0f,0x0f,0xa8,0xa5,0x51,0x02},
 {0x17,0xc1,0x24,0x07,0xf8,0xf8,0x22,0x12},
 {0x71,0x23,0x11,0x06,0x65,0x74,0x18,0x16},
 {0x01,0x02,0xd3,0x05,0xc9,0x95,0x03,0x02},
 {0x61,0x63,0x0c,0x00,0x94,0xc0,0x33,0xf6},
 {0x21,0x72,0x0d,0x00,0xc1,0xd5,0x56,0x06}
};
constexpr std::uint8_t multipliers2[16]={1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
// Собственная Q12 envelope-rate аппроксимация. Не выдаётся за silicon OPLL EG ROM.
constexpr std::uint16_t rateStep[16]={0,1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192,32768};
// round(32767*sin(pi*i/512)), quarter wave; runtime floating point отсутствует.
constexpr std::int16_t quarterSine[257]={
    0,201,402,603,804,1005,1206,1407,1608,1809,2009,2210,2410,2611,2811,3012,
    3212,3412,3612,3811,4011,4210,4410,4609,4808,5007,5205,5404,5602,5800,5998,6195,
    6393,6590,6786,6983,7179,7375,7571,7767,7962,8157,8351,8545,8739,8933,9126,9319,
    9512,9704,9896,10087,10278,10469,10659,10849,11039,11228,11417,11605,11793,11980,12167,12353,
    12539,12725,12910,13094,13279,13462,13645,13828,14010,14191,14372,14553,14732,14912,15090,15269,
    15446,15623,15800,15976,16151,16325,16499,16673,16846,17018,17189,17360,17530,17700,17869,18037,
    18204,18371,18537,18703,18868,19032,19195,19357,19519,19680,19841,20000,20159,20317,20475,20631,
    20787,20942,21096,21250,21403,21554,21705,21856,22005,22154,22301,22448,22594,22739,22884,23027,
    23170,23311,23452,23592,23731,23870,24007,24143,24279,24413,24547,24680,24811,24942,25072,25201,
    25329,25456,25582,25708,25832,25955,26077,26198,26319,26438,26556,26674,26790,26905,27019,27133,
    27245,27356,27466,27575,27683,27790,27896,28001,28105,28208,28310,28411,28510,28609,28706,28803,
    28898,28992,29085,29177,29268,29358,29447,29534,29621,29706,29791,29874,29956,30037,30117,30195,
    30273,30349,30424,30498,30571,30643,30714,30783,30852,30919,30985,31050,31113,31176,31237,31297,
    31356,31414,31470,31526,31580,31633,31685,31736,31785,31833,31880,31926,31971,32014,32057,32098,
    32137,32176,32213,32250,32285,32318,32351,32382,32412,32441,32469,32495,32521,32545,32567,32589,
    32609,32628,32646,32663,32678,32692,32705,32717,32728,32737,32745,32752,32757,32761,32765,32766,
    32767
};
// round(32767*10^(-0.375*i/20)), nominal amplitude Q15.
constexpr std::uint16_t attenuationQ15[256]={
    32767,31382,30056,28786,27570,26405,25289,24221,23197,22217,21278,20379,19518,18693,17903,17147,
    16422,15728,15064,14427,13818,13234,12675,12139,11626,11135,10664,10214,9782,9369,8973,8594,
    8231,7883,7550,7231,6925,6633,6352,6084,5827,5581,5345,5119,4903,4696,4497,4307,
    4125,3951,3784,3624,3471,3324,3184,3049,2920,2797,2679,2566,2457,2353,2254,2159,
    2067,1980,1896,1816,1740,1666,1596,1528,1464,1402,1343,1286,1232,1179,1130,1082,
    1036,992,950,910,872,835,800,766,734,703,673,644,617,591,566,542,
    519,497,476,456,437,418,401,384,368,352,337,323,309,296,284,272,
    260,249,239,229,219,210,201,192,184,176,169,162,155,148,142,136,
    130,125,120,115,110,105,101,96,92,88,85,81,78,74,71,68,
    65,63,60,57,55,53,50,48,46,44,42,41,39,37,36,34,
    33,31,30,29,28,26,25,24,23,22,21,20,20,19,18,17,
    16,16,15,14,14,13,13,12,12,11,11,10,10,9,9,9,
    8,8,8,7,7,7,6,6,6,6,5,5,5,5,4,4,
    4,4,4,4,3,3,3,3,3,3,3,3,2,2,2,2,
    2,2,2,2,2,2,2,2,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};
/** @brief Интерполирует четверть синуса по phaseQ32; отрицательные значения не сдвигает signed-shift. */
std::int32_t sineQ15(std::uint32_t phase) noexcept {
    const auto position=(phase>>22U)&255U,quadrant=phase>>30U,fraction=phase&0x3fffffU;
    const auto a=quarterSine[(quadrant&1U)?256U-position:position];
    const auto b=quarterSine[(quadrant&1U)?255U-position:position+1U];
    const auto value=std::int32_t(a)+(std::int32_t(b-a)*std::int32_t(fraction)/4194304);
    return quadrant&2U?-value:value;
}
/** @brief Увеличивает ненулевой rate по высоте ключа; hardware-rate0 остаётся остановленным. */
unsigned rate(unsigned value,bool keyScale,std::uint8_t control,std::uint16_t fnum) noexcept {
    if(!value)return 0;
    const auto keyCode=unsigned((control>>1U)&7U)*2U+unsigned(fnum>>8U);
    return std::min(15U,value+(keyCode>>(keyScale?1U:3U)));
}
/** @brief Приближает KSL; byte2/3 key-level bits влияют на тембр, но не на цифровой IRQ. */
unsigned keyAttenuation(unsigned scale,std::uint8_t control,std::uint16_t fnum) noexcept {
    if(!scale)return 0;
    const int position=int((control>>1U)&7U)*8+int(fnum>>5U)-40;
    return position>0?unsigned(position)*(1U<<(scale-1U)):0U;
}
}
/** @details Полный cold reset не сохраняет LFO от предыдущей ROM. */
void Vrc7Audio::reset() noexcept {
    registers_={};channels_={};tremoloPhase_=vibratoPhase_=0;selected_=0;
    synthesisRemaining_=36;portableRemaining_=64;muted_=false;
    rawSample_=portableSample_=0;portableArea_=0;
}
/** @details Регистровый reset сохраняет oscillator/boxcar clock и vibrato phase, как наблюдалось на VRC7. */
void Vrc7Audio::clearRegisters() noexcept {
    registers_={};channels_={};tremoloPhase_=0;selected_=0;rawSample_=0;
}
/** @details Непрерывный reset не размораживается записью FM; CPU/M2 продолжает двигаться независимо. */
void Vrc7Audio::setMuted(bool asserted) noexcept {
    if(asserted&&!muted_)clearRegisters();
    muted_=asserted;
}
/** @details Canonical address задаёт digital board decoder; busy-нарушения некорректного ПО не эмулируются. */
void Vrc7Audio::cpuWrite(std::uint16_t address,std::uint8_t value) noexcept {
    if(address==0xe000){setMuted((value&0x40U)!=0);return;}
    if(muted_)return;
    if(address==0x9010)selected_=value;
    else if(address==0x9030)writeRegister(selected_,value);
}
/** @details Отсутствуют 9channel/rhythm/debug расширения YM2413; неподдержанные адреса не aliasят канал 0. */
void Vrc7Audio::writeRegister(std::uint8_t index,std::uint8_t value) noexcept {
    if(index<8){
        registers_[index]=value;
        // Custom patch меняет только голоса с instrument0, включая уже звучащий envelope.
        for(unsigned i=0;i<6;++i)if(!(registers_[0x30U+i]>>4U))refreshParameters(i);
        return;
    }
    if(index==15){registers_[index]=value&15U;return;}
    if(index>=0x10&&index<=0x15){registers_[index]=value;refreshParameters(index-0x10U);return;}
    if(index>=0x20&&index<=0x25){
        const bool old=(registers_[index]&16U)!=0;
        registers_[index]=value&63U;
        refreshParameters(index-0x20U);
        if(old!=bool(value&16U))keyChange(index-0x20U,(value&16U)!=0);
        return;
    }
    if(index>=0x30&&index<=0x35){registers_[index]=value;refreshParameters(index-0x30U);}
}
/** @details Повтор key-on без edge не сбрасывает фазу/envelope; release модульного sustain отличается от carrier. */
void Vrc7Audio::keyChange(unsigned index,bool on) noexcept {
    auto&channel=channels_[index];
    const auto instrument=registers_[0x30U+index]>>4U;
    const auto*patch=instrument?patches[instrument]:registers_.data();
    if(on){
        for(auto&op:channel.op){op.phase=0;op.attenuation=envelopeMaximum;op.envelope=Envelope::Attack;}
        channel.previous=channel.older=0;
    }else{
        channel.op[1].envelope=Envelope::Release;
        if(!(patch[0]&0x20U))channel.op[0].envelope=Envelope::Release;
    }
}
/** @details Кэш хранит точные целочисленные функции регистров: MMIO остаётся единственным invalidation boundary. */
void Vrc7Audio::refreshParameters(unsigned index) noexcept {
    auto&channel=channels_[index];
    const auto control=registers_[0x20U+index];
    const auto fnum=std::uint16_t(registers_[0x10U+index])|std::uint16_t((control&1U)<<8U);
    const auto instrument=registers_[0x30U+index]>>4U;
    const auto*patch=instrument?patches[instrument]:registers_.data();
    const auto increment=std::uint32_t(fnum)<<(((control>>1U)&7U)+13U);
    for(unsigned slot=0;slot<2;++slot){
        auto&p=channel.parameters[slot];const auto properties=patch[slot];
        const auto multiplier=multipliers2[properties&15U];
        p.phaseStep=multiplier==1U?increment>>1U:increment*(multiplier>>1U);
        p.attack=std::uint8_t(rate(patch[4U+slot]>>4U,(properties&16U)!=0,control,fnum));
        p.decay=std::uint8_t(rate(patch[4U+slot]&15U,(properties&16U)!=0,control,fnum));
        p.release=std::uint8_t(rate((control&32U)?5U:patch[6U+slot]&15U,(properties&16U)!=0,control,fnum));
        const auto sl=patch[6U+slot]>>4U;
        p.sustainTarget=sl==15U?envelopeMaximum:std::uint32_t(sl)*8U*4096U;
        const unsigned external=(slot?unsigned(registers_[0x30U+index]&15U)*8U:unsigned(patch[2]&63U)*2U)
            +keyAttenuation(patch[2U+slot]>>6U,control,fnum);
        p.externalAttenuation=std::uint16_t(external);
        p.flags=std::uint8_t(((properties&0x80U)?1U:0U)|((properties&0x40U)?2U:0U)|((properties&0x20U)?4U:0U));
    }
}
/** @details ADSR использует прежние rates и transitions; ни один envelope tick не пропущен. */
void Vrc7Audio::envelopeTick(Operator&op,const Parameters&p) noexcept {
    switch(op.envelope){
    case Envelope::Off:break;
    case Envelope::Attack:
        if(!p.attack)break;
        if(p.attack==15){op.attenuation=0;op.envelope=Envelope::Decay;break;}
        {
            const auto delta=std::max<std::uint32_t>(1U,(op.attenuation>>(16U-p.attack)));
            if(op.attenuation<=delta+64U){op.attenuation=0;op.envelope=Envelope::Decay;}
            else op.attenuation-=delta;
        }
        break;
    case Envelope::Decay:
        if(op.attenuation>=p.sustainTarget){op.attenuation=p.sustainTarget;op.envelope=(p.flags&4U)?Envelope::Sustain:Envelope::Release;break;}
        if(p.decay)op.attenuation=std::min(p.sustainTarget,op.attenuation+rateStep[p.decay]);
        break;
    case Envelope::Sustain:break;
    case Envelope::Release:
        if(p.release)op.attenuation=std::min(envelopeMaximum,op.attenuation+rateStep[p.release]);
        if(op.attenuation>=envelopeMaximum)op.envelope=Envelope::Off;
        break;
    }
}
/** @details Динамические envelope/AM/test сохраняют старое округление; только статический KSL/volume взят из кэша. */
std::int32_t Vrc7Audio::amplitude(const Operator&op,const Parameters&p,unsigned tremolo)const noexcept {
    const bool force=(registers_[15]&1U)!=0;
    if(op.envelope==Envelope::Off&&!force)return 0;
    std::uint32_t attenuation=force?0:op.attenuation;
    const unsigned external=unsigned(p.externalAttenuation)+((p.flags&1U)?tremolo:0U);
    attenuation=std::min<std::uint32_t>(255U*4096U,attenuation+external*4096U);
    const auto index=attenuation>>12U;
    if(index==255U)return attenuationQ15[255];
    const auto a=std::int32_t(attenuationQ15[index]),b=std::int32_t(attenuationQ15[index+1U]);
    return a+((b-a)*std::int32_t(attenuation&4095U)/4096);
}
/** @details Tick использует шесть FM channels и 12 integer operators; envelope/oscillator не зависят от LCD/host. */
void Vrc7Audio::tick() noexcept {
    if(muted_){vibratoPhase_=std::uint16_t((vibratoPhase_+1U)&8191U);rawSample_=0;return;}
    const auto test=registers_[15];
    if(test&2U){tremoloPhase_=vibratoPhase_=0;}
    else{
        tremoloPhase_=std::uint16_t((tremoloPhase_+((test&8U)?64U:1U))%(210U*64U));
        vibratoPhase_=std::uint16_t((vibratoPhase_+((test&8U)?1024U:1U))&8191U);
    }
    static constexpr int vibrato[8]={0,1,2,1,0,-1,-2,-1};
    std::int32_t mix=0;
    // Общая AM-фаза одинакова для всех 12 операторов; вычисляется один раз на tick.
    const auto position=unsigned(tremoloPhase_>>6U);
    const auto tremolo=(position<=105U?position:210U-position)*13U/105U;
    for(unsigned i=0;i<6;++i){
        auto&channel=channels_[i];
        const auto instrument=registers_[0x30U+i]>>4U;
        const auto*patch=instrument?patches[instrument]:registers_.data();
        std::int32_t amp[2]{};
        for(unsigned slot=0;slot<2;++slot){
            auto&op=channel.op[slot];
            const auto&parameters=channel.parameters[slot];
            envelopeTick(op,parameters);
            auto step=parameters.phaseStep;
            if(parameters.flags&2U){
                const auto modulation=vibrato[vibratoPhase_>>10U];
                const auto depth=modulation<0?-modulation:modulation;
                const auto delta=depth?step>>(depth==2?8U:9U):0U;
                step=modulation<0?step-delta:step+delta;
            }
            op.phase=(test&4U)?0U:op.phase+step;
            amp[slot]=amplitude(op,parameters,tremolo);
        }
        std::int32_t mod=0;
        // Нулевая amp не отменяет clocks выше: mod=0 всё равно сдвигает feedback history.
        if(amp[0]){
            const auto feedback=patch[3]&7U;
            const auto feedbackOffset=feedback?std::uint32_t((channel.previous+channel.older)/2)<<(feedback+11U):0U;
            mod=sineQ15(channel.op[0].phase+std::uint32_t(feedbackOffset));
            if((patch[3]&8U)&&mod<0)mod=0;
            // Точные границы Q15: 32767^2<INT32_MAX; (carrier+amp)*4096<=268427264.
            mod=mod*amp[0]/32767;
        }
        channel.older=channel.previous;channel.previous=mod;
        // Нулевой carrier не отменяет уже обновлённую историю модулятора.
        if(amp[1]){
            auto carrier=sineQ15(channel.op[1].phase+std::uint32_t(mod*65536));
            if((patch[3]&16U)&&carrier<0)carrier=0;
            carrier=carrier*amp[1]/32767;
            // Униполярный nominal DAC совместим с существующим externalDacQ16.
            // Граница<=8192/channel; DC bias следует envelope, удаляется общим HPF.
            mix+=(carrier+amp[1])*4096/32767;
        }
    }
    rawSample_=(test&4U)?0:-mix;
}
/** @details Area учитывает все raw intervals;64-M2 boxcar ограничивает publications, не clock генератора. */
void Vrc7Audio::clockCpu(std::uint32_t cycles) noexcept {
    while(cycles){
        const auto step=std::min(cycles,std::uint32_t(std::min(synthesisRemaining_,portableRemaining_)));
        portableArea_+=rawSample_*std::int32_t(step);
        cycles-=step;synthesisRemaining_=std::uint8_t(synthesisRemaining_-step);
        portableRemaining_=std::uint8_t(portableRemaining_-step);
        if(!synthesisRemaining_){synthesisRemaining_=36;tick();}
        if(!portableRemaining_){portableRemaining_=64;portableSample_=std::int32_t(portableArea_/64);portableArea_=0;}
    }
}
} // namespace nes::core

