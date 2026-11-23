/**
 * @file ThinTimingPpu.cpp
 * @brief Отдельный mapper4 signal-only путь с сохранением raster/NMI/OAM/A12.
 * @ingroup nes_ppu
 */
#include "TimingPpu.h"
#include "ThinOamElide.h"
#include "../Machine.h"
#include "../cartridge/Mmc5.h"
#include <algorithm>
namespace nes::core {
// Режим — сигнальный владелец mapper4, а не цветовая эмуляция нулевыми значениями.
// Младшие pattern-address-биты из фоновых данных намеренно не определены;
// табличный A12 и каждый сохранённый CPU-адрес/+8 carry остаются настоящими.
/**
 * @brief Продвигает тонкий сигнальный PPU без формирования ненаблюдаемых цветовых данных.
 * @details Доступен только основному привязанному владельцу. Растр, NMI, loopy/OAM и mapper-провода остаются точными, а реальные чтения делегируются полному PPU.
 */
void TimingPpu::thinTick()noexcept{
    // MMC5 наблюдает реальные /RD, а не только адресные drive как MMC3 A12.
    const bool mmc5=bus_.cartridge_.mapper()==5U;
    const bool oamUnused=NES_PRIMARY_MMC5_OAM_UNUSED();
    const auto rd=[this,mmc5]()noexcept{if(mmc5&&bus_.mmc5QuietMapper())bus_.mmc5QuietMapper()->ppuReadStrobe(busAddress_);};
    if(trackBusCycles_)bus_.ppuCycle();
    if(renderDelay_&&--renderDelay_==0){
        if(renderingEnabled_&&!renderingNext_)synchronizeSprites();
        renderingEnabled_=renderingNext_;
    }
    if(nmiDelay_&&--nmiDelay_==0&&nmiLine_)nmiPending_=true;
    if(scanline_==-1&&dot_==1){status_&=0x1f;suppressVblank_=false;updateNmi();}
    if(scanline_==241&&dot_==1){if(!suppressVblank_)status_|=0x80;frameReady_=true;++frames_;updateNmi();}
    if(unsigned(scanline_)<240U&&dot_==0)evaluationNextDot_=1;
    const bool active=rendering()&&renderLine();
    if(dot_==255)scrollSignal_=active;
    const bool background=(dot_>=1&&dot_<=256)||(dot_>=321&&dot_<=336);
    if(!active&&background&&((dot_-1)&7U)==6)coarseXSignal_=false;
    if(active){
        if(dot_==0&&scanline_>=0)address(((control_&0x10)?0x1000:0)+((v_>>12)&7));

        if(background){
            switch((dot_-1)&7U){
            case 0:address(0x2000|(v_&0x0fff));break;
            case 1:rd();nextTile_=0;break;
            case 2:address(0x23c0|(v_&0x0c00)|((v_>>4)&0x38)|((v_>>2)&7));break;
            case 3:rd();address(((control_&0x10)?0x1000:0)+((v_>>12)&7));break;
            case 4:address(((control_&0x10)?0x1000:0)+((v_>>12)&7));break;
            case 5:rd();break;
            case 6:coarseXSignal_=true;if(dot_==255)scrollSignal_=true;address(busAddress_+8U);break;
            case 7:rd();if(coarseXSignal_)incrementX();coarseXSignal_=false;break;
            }
        }

        if(dot_==257){finishHorizontalScrollSignal();synchronizeSpriteEvaluation(256);}
        if(scanline_==-1&&dot_>=280&&dot_<=304)v_=(v_&0x041f)|(t_&0x7be0);
        if(dot_>=257&&dot_<=320){
            oamAddress_=0;const unsigned slot=(dot_-257)/8U;
            switch((dot_-257)&7U){
            case 0:case 2:address(0x2000|(v_&0x0fff));break;
            case 3:rd();address((oamUnused?0U:spritePattern(slot)));break;
            case 4:address((oamUnused?0U:spritePattern(slot)));break;
            case 1:case 5:case 7:rd();break;
            case 6:address((oamUnused?0U:spritePattern(slot))+8U);break;
            default:break;
            }
        }
        if(dot_==337||dot_==339)address(0x2000|(v_&0x0fff));
        if(dot_==338||dot_==340)rd();
    }else address(v_);
    if(!active&&background&&((dot_-1)&7U)==7&&coarseXSignal_){incrementX();coarseXSignal_=false;}
    if(dot_==256&&scrollSignal_)incrementY();
    if(!active&&dot_==257)finishHorizontalScrollSignal();
    if(unsigned(scanline_)<240U&&dot_>=1&&dot_<=256&&!rendering())evaluationNextDot_=static_cast<std::uint16_t>(dot_+1U);
    if(!pal_&&scanline_==-1&&dot_==339&&odd_&&rendering()){dot_=0;scanline_=0;return;}
    if(++dot_>340){dot_=0;if(++scanline_>(pal_?310:260)){scanline_=-1;odd_=!odd_;}}
}
/**
 * @brief Продвигает тонкий сигнальный PPU без формирования ненаблюдаемых цветовых данных.
 * @details Доступен только основному привязанному владельцу. Растр, NMI, loopy/OAM и mapper-провода остаются точными, а реальные чтения делегируются полному PPU.
 * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
 */
void TimingPpu::thinTickDots(unsigned dots)noexcept{

    // Caller уже завершил настоящий M2. Конечный оператор заменяет только три
    // PPU-точки BG interior, sprite258..320 или BG321..336; Cart/APU/DMC не меняются.
    // Y/copy256/257,337/339, NMI/delay и любой fourth-RD остаются scalar.
    if(dots==3U&&!pal_&&bus_.cartridge_.mapper()==5U&&unsigned(scanline_)<240U&&
       ((dot_>=9U&&dot_+3U<=255U)||(dot_>=321U&&dot_+3U<=337U)||(NES_PRIMARY_MMC5_OAM_UNUSED()&&dot_>=258U&&dot_+3U<=321U))&&renderingEnabled_&&renderingNext_&&(mask_&0x18U)&&
       !(renderDelay_|nmiDelay_)&&!trackBusCycles_&&!trackBusAddresses_&&!packet_&&!capture_){
        auto* digital=bus_.mmc5QuietMapper();
        if(digital&&digital->nameRepeat_<=3U){
            unsigned v=v_,address=busAddress_,repeat=digital->nameRepeat_,last=digital->lastPpuAddress_;
            bool coarse=coarseXSignal_,clearTile=false,seenRead=false,okay=true;
            const unsigned pattern=((control_&0x10U)?0x1000U:0U)+((v>>12U)&7U);
            /** @brief Моделирует один cartridge /RD без живой mutation до complete proof. */
            const auto rd=[&](unsigned a)noexcept{
                a&=0x3fffU;if(a>=0x3f00U)return;
                if(repeat==3U){okay=false;return;}
                repeat=a>=0x2000U&&a<0x3000U?(a==last?repeat+1U:1U):0U;
                last=a;seenRead=true;
            };
            /** @brief Выполняет ровно исходный coarseX carry, не затрагивая fine/coarseY. */
            const auto incX=[&]()noexcept{if((v&31U)==31U)v=(v&~31U)^0x400U;else ++v;};
            // На трёх dots максимум два /RD. Первое обращение фаз 1/3/5/7
            // обязано использовать actual busAddress после CPU2007, а не guessed bank.
            const bool sprite=dot_>=258U&&dot_<321U;
            // Невычисление локальной OAM требует MMC5 owner predicate. Здесь только RD-class:
            // ровно две NT и две pattern за slot; actual initial drive после 2007 сохранён.
            if(sprite){
                const unsigned nt=0x2000U|(v&0x0fffU);
                switch((dot_-257U)&7U){
                case 0:rd(nt);address=nt;break;
                case 1:rd(address);rd(nt);address=0U;break;
                case 2:rd(nt);address=0U;break;
                case 3:rd(address);rd(0U);address=0U;break;
                case 4:rd(0U);address=8U;break;
                case 5:rd(address);address=8U;rd(address);break;
                case 6:address=8U;rd(address);address=nt;break;
                case 7:rd(address);address=nt;rd(address);break;
                }
            }else switch((dot_-1U)&7U){
            case 0:rd(0x2000U|(v&0x0fffU));address=0x23c0U|(v&0x0c00U)|((v>>4U)&0x38U)|((v>>2U)&7U);clearTile=true;break;
            case 1:rd(address);rd(0x23c0U|(v&0x0c00U)|((v>>4U)&0x38U)|((v>>2U)&7U));address=pattern;clearTile=true;break;
            case 2:rd(0x23c0U|(v&0x0c00U)|((v>>4U)&0x38U)|((v>>2U)&7U));address=pattern;break;
            case 3:rd(address);rd(pattern);address=pattern;break;
            case 4:rd(pattern);address=pattern+8U;coarse=true;break;
            case 5:rd(address);address=(address+8U)&0x3fffU;rd(address);incX();coarse=false;break;
            case 6:address=(address+8U)&0x3fffU;rd(address);incX();address=0x2000U|(v&0x0fffU);coarse=false;break;
            case 7:rd(address);if(coarse)incX();coarse=false;address=0x2000U|(v&0x0fffU);rd(address);clearTile=true;break;
            }
            if(okay){
                v_=static_cast<std::uint16_t>(v);busAddress_=static_cast<std::uint16_t>(address);dot_+=3U;
                coarseXSignal_=coarse;if(clearTile)nextTile_=0;if(sprite)oamAddress_=0;
                digital->nameRepeat_=static_cast<std::uint8_t>(repeat);digital->lastPpuAddress_=static_cast<std::uint16_t>(last);
                if(seenRead)digital->ppuReadSinceM2_=true;
                
                return;
            }
            
        }
    }
    // Первый correctness-срез: MMC3 composers нельзя использовать для MMC5 /RD.
    if(bus_.cartridge_.mapper()==5U){
        auto* digital=bus_.mmc5QuietMapper();
        while(dots){
            // Только primary Signals без bus/packet/capture observers. Cart/APU M2
            // уже произошёл снаружи и здесь не сворачивается; mapper /RD отсутствуют.
            if((scanline_>=240||!renderingEnabled_)&&dots>=2U&&digital&&this==&bus_.timingPpu_&&
               bus_.thinSignalMode()&&!packet_&&!capture_&&!trackBusCycles_&&!trackBusAddresses_&&
               !pendingPixels_&&!(renderDelay_|nmiDelay_)&&!coarseXSignal_&&!scrollSignal_&&busAddress_==(v_&0x3fffU)){
                const unsigned nonvisible=nonVisibleQuietDots();
                // Disabled visible rendering всё ещё обновляет evaluationNextDot.
                // Dot0 и pre-render остаются scalar; до следующей строки лишь stable address(v).
                const bool blank=!nonvisible&&!renderingEnabled_&&!renderingNext_&&
                    unsigned(scanline_)<240U&&dot_>=1U&&dot_<=340U;
                const unsigned quiet=nonvisible?nonvisible:(blank?341U-dot_:0U);
                if(quiet>=2U){
                    const unsigned n=std::min(dots,quiet),next=dot_+n;
                    if(blank&&dot_<=256U)evaluationNextDot_=static_cast<std::uint16_t>(std::min(257U,next));
                    // Все пропущенные inactive dots повторяли тот же address(v).
                    // CoarseX/scroll gates сохраняют PPUMASK-edge carry на старом scalar пути.
                    busAddress_=static_cast<std::uint16_t>(v_&0x3fffU);
                    scanline_+=static_cast<std::int16_t>(next/341U);dot_=static_cast<std::uint16_t>(next%341U);
                    dots-=n;
                    continue;
                }
            }
            // Только steady visible interior.255/256 scroll, sprite/OAM,
            // pre-render/NMI и delayed enable остаются прежними scalar tails.
            if(digital&&dots>=3U&&unsigned(scanline_)<240U&&dot_>=9U&&dot_+3U<=255U&&
               renderingEnabled_&&!(renderDelay_|nmiDelay_)){
                const unsigned n=std::min({dots,8U,255U-dot_});
                
                for(unsigned i=0;i<n;++i){
                    if(trackBusCycles_)bus_.ppuCycle();
                    switch((dot_-1U)&7U){
                    case 0:address(0x2000U|(v_&0x0fffU));break;
                    case 1:digital->ppuReadStrobe(busAddress_);nextTile_=0;break;
                    case 2:address(0x23c0U|(v_&0x0c00U)|((v_>>4)&0x38U)|((v_>>2)&7U));break;
                    case 3:digital->ppuReadStrobe(busAddress_);address(((control_&0x10U)?0x1000U:0U)+((v_>>12)&7U));break;
                    case 4:address(((control_&0x10U)?0x1000U:0U)+((v_>>12)&7U));break;
                    case 5:digital->ppuReadStrobe(busAddress_);break;
                    case 6:coarseXSignal_=true;address(busAddress_+8U);break;
                    case 7:digital->ppuReadStrobe(busAddress_);if(coarseXSignal_)incrementX();coarseXSignal_=false;break;
                    }
                    ++dot_;
                }
                dots-=n;continue;
            }
            thinTick();--dots;
        }
        return;
    }
    while(dots){

        if(renderingEnabled_&&!(renderDelay_|nmiDelay_)&&unsigned(scanline_)<240U){
            if(dot_>=1&&dot_<=249&&((dot_-1U)&7U)==0&&dots>=8){
                const unsigned tiles=std::min(dots/8U,(257U-dot_)/8U);
                const bool high=(control_&0x10U)!=0;
                if(bus_.cartridge_.ppuIrqQuietFor(tiles*8U,high)){
                    // Простые mapper не имеют A12-состояния MMC3; не создавать им фиктивные IRQ-поля.
                    if(bus_.cartridge_.mapper()==4U)bus_.advanceThinBackgroundSignals(tiles,high);
                    const unsigned x=(v_&31U)+tiles;
                    busAddress_=static_cast<std::uint16_t>((high?0x1000U:0U)+((v_>>12)&7U)+8U);
                    v_=static_cast<std::uint16_t>((v_&~0x041fU)|(x&31U)|((v_^((x>>5U)<<10U))&0x0400U));
                    dot_=static_cast<std::uint16_t>(dot_+tiles*8U);dots-=tiles*8U;
                    coarseXSignal_=false;if(dot_==257){scrollSignal_=true;incrementY();}
                    continue;
                }
            }
            if(dot_>=265&&dot_<=313&&((dot_-257U)&7U)==0&&dots>=8){
                const auto pattern=spritePattern((dot_-257U)/8U);
                if(bus_.videoSpriteMode(pattern,true,true)==Machine::VideoSpriteMode::Mmc3){
                    bus_.advanceMmc3TileSignals((pattern&0x1000U)!=0);
                    oamAddress_=0;busAddress_=static_cast<std::uint16_t>(pattern+8U);dot_+=8;dots-=8;continue;
                }
            }
        }
        const unsigned quiet=nonVisibleQuietDots();
        if(quiet){
            const unsigned n=std::min(dots,quiet);
            bus_.advanceStablePpuSignals(n,busAddress_,trackBusCycles_,trackBusAddresses_);
            const unsigned next=dot_+n;scanline_+=static_cast<std::int16_t>(next/341U);dot_=next%341U;dots-=n;continue;
        }
        thinTick();--dots;
    }
}
/**
 * @brief Делегирует реальное CPU-чтение единственному полному PPU и сохраняет локальные эффекты сигналов.
 * @details Сначала закрывается текущий префикс; ответ соответствует точному post-clock срезу и не угадывается.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool TimingPpu::readThinSignals(std::uint16_t reg,std::uint8_t value)noexcept{
    if(this!=&bus_.timingPpu_||!bus_.thinSignalMode()||packet_||capture_)return false;
    if((reg&7U)==2U||(reg&7U)==4U)synchronizeSprites();
    if((reg&7U)==2U){
        if((value&0x80U)!=(status_&0x80U))return false;
        if(scanline_==241&&dot_==1)suppressVblank_=true;
        status_&=0x7f;writeToggle_=false;
        if(scanline_==241&&dot_<=3)nmiPending_=false;
        updateNmi();
    }else if((reg&7U)==7U){
        const auto location=static_cast<std::uint16_t>(v_&0x3fffU);
        address(location);
        if(bus_.cartridge_.mapper()==5U&&bus_.mmc5QuietMapper())bus_.mmc5QuietMapper()->ppuReadStrobe(location);
        if(location>=0x3f00U){
            address(location-0x1000U);
            if(bus_.cartridge_.mapper()==5U&&bus_.mmc5QuietMapper())bus_.mmc5QuietMapper()->ppuReadStrobe(location-0x1000U);
        }
        incrementDataAddress();
        // Read-buffer-данные в этом режиме принадлежат только полному C0 Ppu.
        readBuffer_=0;
    }
    openBus_=value;return true;
}
}
