/**
 * @file MachineMmc5LocalReadFold.cpp
 * @brief Частный trusted MMC5 LDA/BNE fixed point без State address escape.
 * @details Контракт квалификации находится в заголовке; clocks/heap/cache нет.
 */
#include "Machine.h"
#include "cpu/RomWindow.h"

namespace nes::core {
/** @details Исторический begin уже проверил identity. Этот once-frame guard
 * дополнительно сохраняет defensive refusal private-corruption fixtures03.
 * Dynamic faults/events/capacity по-прежнему проверяются каждым old admission. */
bool Machine::mmc5ReadFoldFrameSource() const noexcept {
    return false;
}

/** @details Все code reads сначала ограничиваются одним 8KiB window, затем
 * квалифицируются одним qRDS. Дополнительная ROM data/dummy может занять ещё
 * одно window; третье отвергается. Никакие guest bytes не читаются до bounds.
 * LDA/BNE сохраняют X/Y/SP/прочие P, A/NZ/base/open проверяются как fixed point. */
#if defined(__GNUC__) && !defined(__clang__)
[[gnu::pure,gnu::noinline,gnu::noclone]]
#elif defined(__clang__)
[[gnu::pure,gnu::noinline]]
#endif
unsigned Machine::mmc5LocalReadFold(std::uint32_t pcAx,std::uint32_t baseOpenP,std::uint32_t branchInfo,
    unsigned remaining,const std::uint8_t* ram,const std::uint8_t* wram,
    const std::uint8_t* const* dataPrg) const noexcept {
    // FrameScope/active/frameQualified принадлежат текущему ThinM5 owner.
    // Dynamic IRQ/DMC/APU/AudioPacket guards уже ограничили исходный grant.
    if(!thinSignalMode()||cartridge_.mapper()!=5U||thinStickyFault_||thin_->fault()!=ColourFault::None)return 0;
    if(remaining<36U||remaining>80U||remaining>cpuReadBudget_||
       !mmc5ReadQuantum_.active||!mmc5ReadQuantum_.frameQualified||
       (branchInfo&0x1000000U)||ram!=ram_.data()||wram!=mmc5ReadQuantum_.ram6000||
       dataPrg!=quietCpuReadPages())return 0;
    const auto pc=static_cast<std::uint16_t>(pcAx);
    const auto branch=static_cast<std::uint16_t>(branchInfo);
    const unsigned length=static_cast<std::uint16_t>(branch-pc);
    if((length!=2U&&length!=3U)||pc<0x8000U||branch>0xfffdU)return 0;
    const auto after=static_cast<std::uint16_t>(branch+2U);
    const bool branchCross=(after&0xff00U)!=(pc&0xff00U);
    const auto dummy=branchCross?static_cast<std::uint16_t>((after&0xff00U)|(pc&0xffU)):after;
    const unsigned window=pc>>13U;
    if((branch>>13U)!=window||(after>>13U)!=window||(dummy>>13U)!=window||
       (pc<=0xfffbU&&after>=0xfffaU)||dummy==0xfffaU||dummy==0xfffbU||
       ((branchInfo>>16U)&255U)!=3U+unsigned(branchCross))return 0;
    const auto source=quietRomWindow(pc);
    if(!source.immutable||!source.physicalPage||!source.romLease||source.romLease!=romMappingEpoch_)return 0;
    const auto* const code=dataPrg[window&3U];if(!code)return 0;
    const auto body=Cpu6502::instructions_[code[pc&8191U]];
    const auto ending=Cpu6502::instructions_[code[branch&8191U]];
    if(body.op!=Cpu6502::Op::LDA||ending.op!=Cpu6502::Op::BNE||ending.mode!=Cpu6502::Mode::Rel||
       static_cast<std::uint16_t>(after+static_cast<std::int8_t>(code[(branch+1U)&8191U]))!=pc)return 0;
    std::uint16_t base=0,address=0;unsigned bodyCycles=0;
    if(body.mode==Cpu6502::Mode::Zp&&length==2U){base=address=code[(pc+1U)&8191U];bodyCycles=3U;}
    else if(body.mode==Cpu6502::Mode::AbsX&&length==3U){
        base=static_cast<std::uint16_t>(code[(pc+1U)&8191U]|(unsigned(code[(pc+2U)&8191U])<<8U));
        address=static_cast<std::uint16_t>(base+(pcAx>>24U));
        bodyCycles=4U+unsigned((base&0xff00U)!=(address&0xff00U));
    }else return 0;
    const auto indexedDummy=static_cast<std::uint16_t>((base&0xff00U)|(address&0xffU));
    const unsigned dataWindow=address>>13U,dummyWindow=indexedDummy>>13U;
    const bool dataRom=address>=0x8000U,dummyRom=bodyCycles==5U&&indexedDummy>=0x8000U;
    if((address>=0x2000U&&address<0x6000U)||(address>=0x6000U&&address<0x8000U&&!wram)||
       address==0xfffaU||address==0xfffbU)return 0;
    if(bodyCycles==5U&&((indexedDummy>=0x2000U&&indexedDummy<0x6000U)||
       (indexedDummy>=0x6000U&&indexedDummy<0x8000U&&!wram)||indexedDummy==0xfffaU||indexedDummy==0xfffbU))return 0;
    if(dataRom||dummyRom){
        const unsigned extra=dataRom&&dataWindow!=window?dataWindow:(dummyRom&&dummyWindow!=window?dummyWindow:window);
        if((dataRom&&dataWindow!=window&&dataWindow!=extra)||(dummyRom&&dummyWindow!=window&&dummyWindow!=extra))return 0;
        if(extra!=window){const auto other=quietRomWindow(static_cast<std::uint16_t>(extra<<13U));
            if(!other.immutable||!other.physicalPage||!other.romLease||other.romLease!=romMappingEpoch_)return 0;}
        if((dataRom&&!dataPrg[dataWindow&3U])||(dummyRom&&!dataPrg[dummyWindow&3U]))return 0;
    }
    if(static_cast<std::uint16_t>(baseOpenP>>16U)!=base||code[dummy&8191U]!=((baseOpenP>>8U)&255U))return 0;
    const auto value=address<0x2000U?ram[address&0x7ffU]:(address<0x8000U?wram[address&8191U]:dataPrg[dataWindow&3U][address&8191U]);
    if(!value||value!=((pcAx>>16U)&255U)||(baseOpenP&Cpu6502::Z)||(baseOpenP&Cpu6502::N)!=(value&Cpu6502::N))return 0;
    const unsigned period=bodyCycles+3U+unsigned(branchCross),iterations=remaining/period;
    return iterations>=4U?iterations*period:0U;
}
} // namespace nes::core
