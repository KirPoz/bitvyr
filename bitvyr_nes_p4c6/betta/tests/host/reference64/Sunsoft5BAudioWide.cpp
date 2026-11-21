/**
 * @file Sunsoft5BAudioWide.cpp
 * @brief Собственные делители/17-bit LFSR/CAaH envelope по NESdev Sunsoft_5B_audio.
 * @details https://www.nesdev.org/wiki/Sunsoft_5B_audio
 * Источник — регистры/измерения, не реализация другого emulator.
 */
#include "Sunsoft5BAudioWide.h"
#include <algorithm>
namespace nes::core {
namespace {
// Номинальная1.5dB лестница: round(12000*10^(-1.5*(31-i)/20));0/1 silent.
// Собственно рассчитанные константы, не дамп аналоговой кривой YM2149.
constexpr std::int32_t dac[32]={0,0,80,95,113,135,160,190,226,269,319,379,451,536,637,757,900,1070,1271,1511,1795,2134,2536,3014,3582,4258,5060,6014,7148,8495,10097,12000};
/** @brief Аппаратный period0 эквивалентен1; остальные значения не изменяются. */
std::uint16_t period(std::uint16_t value) noexcept {return value?value:1;}
}
/** @details Cold-reset не выдаётся за физически измеренную power-on фазу. */
void Sunsoft5BAudioWide::reset() noexcept {
    registers_={};toneCounter_={};toneHigh_={};noiseLfsr_=1;
    envelopeCounter_=0;selected_=noiseCounter_=envelopeStep_=0;envelopeLevel_=31;
    noiseHalf_=envelopeAttack_=envelopeHolding_=false;
    dividerRemaining_=16;portableRemaining_=64;rawSample_=portableSample_=0;portableArea_=0;
}
/** @details Верхние4 select bits запрещают запись, как address latch YM2149; генераторы не сбрасываются. */
void Sunsoft5BAudioWide::cpuWrite(std::uint16_t address,std::uint8_t value) noexcept {
    if(address>=0xc000&&address<0xe000){selected_=value;return;}
    if(address<0xe000||selected_>=16)return;
    static constexpr std::uint8_t masks[16]={255,15,255,15,255,15,31,63,31,31,31,255,255,15,255,255};
    registers_[selected_]=value&masks[selected_];
    if(selected_==13){
        envelopeCounter_=0;envelopeStep_=0;envelopeHolding_=false;
        envelopeAttack_=(value&4U)!=0;envelopeLevel_=envelopeAttack_?0:31;
    }
    refreshSample();
}
/** @details CAaH применяется на границе32 шагов; alternating повторяет крайний уровень на стыке ramp. */
void Sunsoft5BAudioWide::tickEnvelope() noexcept {
    if(envelopeHolding_)return;
    if(envelopeStep_<31){++envelopeStep_;envelopeLevel_=envelopeAttack_?envelopeStep_:31U-envelopeStep_;return;}
    const auto shape=registers_[13];
    if(!(shape&8U)){envelopeLevel_=0;envelopeHolding_=true;return;}
    if(shape&1U){
        envelopeLevel_=envelopeAttack_?31:0;
        if(shape&2U)envelopeLevel_=31U-envelopeLevel_;
        envelopeHolding_=true;return;
    }
    if(shape&2U)envelopeAttack_=!envelopeAttack_;
    envelopeStep_=0;envelopeLevel_=envelopeAttack_?0:31;
}
/** @details Tone и envelope считают16-M2, noise32-M2; disabled gates не останавливают divider/LFSR. */
void Sunsoft5BAudioWide::tick() noexcept {
    for(unsigned i=0;i<3;++i){
        const auto reload=period(std::uint16_t(registers_[i*2])|(std::uint16_t(registers_[i*2+1])<<8U));
        if(++toneCounter_[i]>=reload){toneCounter_[i]=0;toneHigh_[i]=!toneHigh_[i];}
    }
    noiseHalf_=!noiseHalf_;
    if(!noiseHalf_&&++noiseCounter_>=period(registers_[6])){
        noiseCounter_=0;
        const auto bit=((noiseLfsr_>>16U)^(noiseLfsr_>>13U))&1U;
        noiseLfsr_=((noiseLfsr_<<1U)|bit)&0x1ffffU;
    }
    const auto envelopePeriod=period(std::uint16_t(registers_[11])|(std::uint16_t(registers_[12])<<8U));
    if(++envelopeCounter_>=envelopePeriod){envelopeCounter_=0;tickEnvelope();}
    refreshSample();
}
/** @details Логическое AND tone/noise; оба disable дают DC volume, как настоящая схема, а не mute. */
void Sunsoft5BAudioWide::refreshSample() noexcept {
    std::int32_t sum=0;
    for(unsigned i=0;i<3;++i){
        const auto gate=registers_[7];
        if(!((gate&(1U<<i))||toneHigh_[i])||!((gate&(8U<<i))||((noiseLfsr_>>16U)&1U)))continue;
        const auto volume=registers_[8+i];
        const auto level=(volume&16U)?envelopeLevel_:((volume&15U)?2U*(volume&15U)+1U:0U);
        sum+=dac[level];
    }
    rawSample_=-sum;
}
/** @details Area интегрируется ДО внутреннего фронта: причинный64-M2 boxcar, без drop отдельных raw events. */
void Sunsoft5BAudioWide::clockCpu(std::uint32_t cycles) noexcept {
    while(cycles){
        const auto step=std::min(cycles,std::uint32_t(std::min(dividerRemaining_,portableRemaining_)));
        portableArea_+=std::int64_t(rawSample_)*step;
        cycles-=step;dividerRemaining_=std::uint8_t(dividerRemaining_-step);
        portableRemaining_=std::uint8_t(portableRemaining_-step);
        if(!dividerRemaining_){dividerRemaining_=16;tick();}
        if(!portableRemaining_){portableRemaining_=64;portableSample_=std::int32_t(portableArea_/64);portableArea_=0;}
    }
}
} // namespace nes::core

