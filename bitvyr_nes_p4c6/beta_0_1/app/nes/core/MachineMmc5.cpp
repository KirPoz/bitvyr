/** @file MachineMmc5.cpp @brief Переносимый адаптер MMC5 fetch в отдельной единице трансляции. */
#include "Machine.h"
#include "cartridge/Mmc5.h"
namespace nes::core {
/** @brief Передаёт MMC5 настоящую фазу PPU fetch, сохраняя один read-strobe на доступ. */
std::uint8_t Machine::mmc5PpuFetch(std::uint16_t address,PpuFetchKind kind,unsigned tileX) noexcept {
    if(!cartridge_.mmc5_)return 0;
    Mmc5::Fetch fetch=Mmc5::Fetch::Cpu;
    switch(kind){
    case PpuFetchKind::BackgroundName:fetch=Mmc5::Fetch::BackgroundName;break;
    case PpuFetchKind::BackgroundAttribute:fetch=Mmc5::Fetch::BackgroundAttribute;break;
    case PpuFetchKind::BackgroundPattern:fetch=Mmc5::Fetch::BackgroundPattern;break;
    case PpuFetchKind::SpritePattern:fetch=Mmc5::Fetch::SpritePattern;break;
    case PpuFetchKind::DummyName:fetch=Mmc5::Fetch::Dummy;break;
    case PpuFetchKind::Cpu:break;
    }
    return cartridge_.mmc5_->ppuRead(address&0x3fffU,fetch,nametables_.data(),tileX).value;
}


}
