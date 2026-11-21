/** @file CartridgeMmc5.cpp @brief Адаптер MMC5 к общей шине без SDK и выделения памяти. */
#include "Cartridge.h"
#include "Mmc5.h"
#include "Mmc5Audio.h"
namespace nes::core {
/** @brief Читает выбранную страницу MMC5 либо значение открытой шины и учитывает PCM-эффект чтения. */
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
/** @brief Продвигает цифровую часть и аудио MMC5 на одинаковое число тактов CPU. */
void Cartridge::clockMmc5Cpu(unsigned count) noexcept {
    if(mmc5_)mmc5_->clockCpu(count);
    if(mmc5Audio_)mmc5Audio_->clockCpu(count);
}
/** @details Provider присутствует; waveform clock не меняет PCM IRQ без запрещённых CPU reads/writes. */
bool Cartridge::canCompleteMmc5H2() const noexcept {
    return mapper_==5&&mmc5_&&mmc5Audio_&&mmc5_->canCompleteH2();
}
/** @brief Объединяет запросы прерывания детектора строк и PCM-канала MMC5. */
bool Cartridge::mmc5Irq() const noexcept {
    return (mmc5_&&mmc5_->irqLine())||(mmc5Audio_&&mmc5Audio_->irqLine());
}
/** @brief Передаёт MMC5 уже выполненное чтение с адресом и значением для определения границы кадра. */
void Cartridge::observeMmc5Read(std::uint16_t address,std::uint8_t value) noexcept {
    if(mapper_==5&&mmc5_)mmc5_->cpuReadStrobe(address,value);
}
/** @brief Передаёт активному MMC5 запись шины CPU, включая наблюдаемые регистры PPU. */
void Cartridge::observeMmc5Write(std::uint16_t address,std::uint8_t value) noexcept {
    if(mapper_==5&&mmc5_)mmc5_->cpuWrite(address,value);
}
/** @brief Выбирает мгновенный уровень звука картриджа; внешний FM-рендерер самостоятельно учитывает VRC7. */
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
