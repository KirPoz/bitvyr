/** @file CartridgeMmc5.cpp @brief Адаптер MMC5 к общей шине без SDK и выделения памяти. */
#include "Cartridge.h"
#include "Mmc5.h"
#include "Mmc5Audio.h"
namespace nes::core {
std::uint8_t Cartridge::readMmc5Cpu(std::uint16_t address,std::uint8_t openBus) const noexcept {
    if(!mmc5_)return openBus;
    if(address>=0x8000){
        const auto* page=prgPages_[(address>>13)&3U];
        const auto value=page?page[address&8191U]:openBus;
        mmc5_->cpuReadStrobe(address,value);
        return mmc5Audio_?mmc5Audio_->cpuRead(address,value):value;
    }
    const auto value=mmc5_->cpuRead(address,openBus);
    return mmc5Audio_?mmc5Audio_->cpuRead(address,value):value;
}
void Cartridge::clockMmc5Cpu(unsigned count) noexcept {
    if(mmc5_)mmc5_->clockCpu(count);
    if(mmc5Audio_)mmc5Audio_->clockCpu(count);
}
/** @details Real audio clock сохраняет один старый clock(1), без digital state. */
void Cartridge::clockMmc5AudioCpu() noexcept { if(mmc5Audio_)mmc5Audio_->clockCpu(1); }
/** @details Replay остаётся serial: digital(T), dots(T), digital(T+1), dots(T+1). */
void Cartridge::clockMmc5DigitalCpu() noexcept { if(mmc5_)mmc5_->clockCpu(1); }
/** @details Provider присутствует; waveform clock не меняет PCM IRQ без запрещённых CPU reads/writes. */
bool Cartridge::canCompleteMmc5H2() const noexcept {
    return mapper_==5&&mmc5_&&mmc5Audio_&&mmc5_->canCompleteH2();
}
bool Cartridge::mmc5Irq() const noexcept {
    return (mmc5_&&mmc5_->irqLine())||(mmc5Audio_&&mmc5Audio_->irqLine());
}
void Cartridge::observeMmc5Read(std::uint16_t address,std::uint8_t value) noexcept {
    if(mapper_==5&&mmc5_)mmc5_->cpuReadStrobe(address,value);
}
void Cartridge::observeMmc5Write(std::uint16_t address,std::uint8_t value) noexcept {
    if(mapper_==5&&mmc5_)mmc5_->cpuWrite(address,value);
}
std::int32_t Cartridge::expansionSample() const noexcept {
    if(mapper_==69)return sunsoft5bAudio_.sample();
    if(mapper_==85)return submapper_==1||externalVrc7_?0:vrc7Audio_.sample();
    return mapper_==5&&mmc5Audio_?mmc5Audio_->sample():0;
}
/** @details Профиль выбирается в одной точке для scalar/GQ и обоих DMC трактов.
 * Усреднение принадлежит Mmc5Audio; адаптер не дублирует duty/gate арифметику. */
std::int32_t Cartridge::selectedExpansionSample() const noexcept {
    if(mapper_==69)return sunsoft5bAudio_.portableSample();
    if(mapper_==85)return submapper_==1||externalVrc7_?0:vrc7Audio_.portableSample();
    return mapper_==5&&mmc5Audio_?mmc5Audio_->portableSample():0;
}
/** @details Пропускаются только удалённые portable-несущие между уже разрешёнными
 * CPU clocks. Без portable флага сохранён исходный raw output deadline. */
std::uint32_t Cartridge::selectedExpansionOutputCpuCycles() const noexcept {
    if(mapper_==69)return sunsoft5bAudio_.nextPortableOutputCpuCycles();
    // Внешний FM строится Main по tape: CPU1 не публикует portable DAC и не имеет 64M2 audio deadline.
    if(mapper_==85)return submapper_==1||externalVrc7_?~0U:vrc7Audio_.nextPortableOutputCpuCycles();
    if(mapper_!=5||!mmc5Audio_)return 1;
    return mmc5Audio_->nextPortableOutputCpuCycles();
}
} // namespace nes::core
