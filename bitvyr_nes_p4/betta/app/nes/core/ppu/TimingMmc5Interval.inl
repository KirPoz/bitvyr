/**
 * @file TimingMmc5Interval.inl
 * @brief Арифметический visible raster interval с прежними proto0 записями.
 * @details Включается внутри nes::core единственного TimingMmc5Raster.cpp.
 * Нет новых clocks, grants, opcode, staging, allocations или второго PPU.
 */

/** @details Pure часть целиком предшествует записи. Геометрия исключает255/256,
 * наблюдатели и Sprite0-кандидаты исключают зависимость от промежуточных debts.
 * Каждый переход phase0 может дать Draw+BgLoad; phase3/5/7 дают по записи.
 * Поэтому точный slot-count доказывается до common reserveEvents, а отказ
 * оставляет прежнему пути исходную фазу capacity fault. */
bool TimingPpu::tryMmc5RasterInterval(unsigned dots,Mmc5RasterPlan& plan) noexcept {
    const unsigned begin=dot_;
    if(!dots||dots>24U||begin<9U||begin>=255U||dots>255U-begin||
       static_cast<unsigned>(plan.policy())>1U||pal_||trackBusCycles_||trackBusAddresses_||
       this!=&bus_.timingPpu_||plan.owner_!=bus_.mmc5QuietMapper()||
       !packet_||packet_!=bus_.videoPacket_||capture_||!bus_.outputOwner_||
       !bus_.outputJob_||bus_.outputInitializing_||bus_.videoObserver_||bus_.videoObservableOnly_||
       unsigned(scanline_)>=height||!renderingEnabled_||!renderingNext_||!(mask_&0x18U)||
       renderDelay_||nmiDelay_||nmiPending_||frameReady_||(status_&0x80U)||
       fineX_>7U||(v_&0x8000U)||(t_&0x8000U)||nextAttribute_>3U||
       !spriteCacheValid_||spriteCacheClocks_>width-dots||pendingBackgroundShifts_>16U||
       pendingPixels_>width-dots)return false;
    const unsigned x=begin-1U;
    if(pendingPixels_&&(pendingFill_||pendingPaletteIndex_||pendingPixelX_>=width||
       pendingPixelX_+pendingPixels_!=x))return false;
    if(spriteZeroRow_&&!(status_&0x40U)&&(mask_&0x18U)==0x18U){
        unsigned at=x,left=dots;
        while(left){
            const unsigned count=std::min(left,8U-(at&7U));
            const unsigned bits=((1U<<count)-1U)<<(at&7U);
            if(spriteZeroMask_[at>>3U]&bits)return false;
            at+=count;left-=count;
        }
    }
    const unsigned phase=(begin-1U)&7U;
    const unsigned untilLoad=(8U-phase)&7U;
    if(untilLoad<dots){
        const unsigned shifts=std::min(16U,unsigned(pendingBackgroundShifts_)+untilLoad);
        // Обычные0/8/16 копируют refs без resolver. Только частичный mix
        // требует literal source; следующая полная группа всегда даёт shift8.
        if(shifts&&shifts!=8U&&shifts<16U)
            for(unsigned base:{unsigned(LowHigh),unsigned(HighHigh)})
                if((backgroundAtoms_[base]&VideoByteRef::kindMask)!=VideoByteRef::literal||
                   (backgroundAtoms_[base+1U]&VideoByteRef::kindMask)!=VideoByteRef::literal)return false;
    }
    auto& hot=packet_->hot();
    if(hot.state!=VideoPacket::State::Recording||hot.header.fault!=VideoFault::None||
       hot.header.protocol||hot.literalPrefixCount||hot.header.eventCount>VideoPacket::eventCapacity||
       hot.legacyCount>VideoPacket::eventCapacity-hot.header.eventCount)return false;
    /** @brief Считает появления заданной фазы в полуинтервале без перебора точек. */
    const auto phases=[phase,dots](unsigned wanted)noexcept{
        const unsigned first=(wanted+8U-phase)&7U;
        return first<dots?1U+(dots-1U-first)/8U:0U;
    };
    const unsigned loads=phases(0U);
    const unsigned records=2U*loads-unsigned(!phase&&!pendingPixels_)+
        phases(3U)+phases(5U)+phases(7U);
    const unsigned used=packet_->legacyEventCount();
    if(used>VideoPacket::eventCapacity||records>VideoPacket::eventCapacity-used)return false;
    // Proto0 не меняет legacyCount в reserveEvents. Все failure-предикаты
    // только что доказаны; sole owner не допускает мутации между proof и call.
    static_cast<void>(packet_->reserveEvents(records));
    if(plan.policy()==Mmc5RasterPlan::Policy::Normal)runMmc5RasterInterval<0U>(dots,plan);
    else runMmc5RasterInterval<1U>(dots,plan);
    return true;
}

/** @details Один chunk оканчивается границей тайла: максимум4 chunks на24dots.
 * Исполняются только load и четыре реальные read-фазы. Address-only2/4/6
 * выводятся из прежней фазы в момент read либо конечной материализации.
 * PPU-поля не читает mapper callback; нет CPU/MMIO и наблюдателя пакета внутри.
 * Поэтому cursor/pixels/shifts/sprite clocks публикуются на исходном endpoint,
 * а прежние event bytes записываются до единственного eventCount store. */
template<unsigned Policy>
void TimingPpu::runMmc5RasterInterval(unsigned dots,Mmc5RasterPlan& plan) noexcept {
    static_assert(Policy<2U,"normal or extended raster interval");
    constexpr auto policy=static_cast<Mmc5RasterPlan::Policy>(Policy);
    unsigned cursor=dot_,left=dots,phase=(cursor-1U)&7U;
    auto scroll=v_,address=busAddress_;
    auto tile=nextTile_,attribute=nextAttribute_;
    bool coarse=coarseXSignal_;
    unsigned pixels=pendingPixels_,pixelX=pendingPixelX_,shifts=pendingBackgroundShifts_;
    auto& hot=packet_->hot();
    auto* const first=packet_->events_.data()+hot.header.eventCount;
    auto* output=first;
    const unsigned drawBits=(unsigned(scanline_)<<8U)|(unsigned(mask_)<<16U)|(unsigned(fineX_)<<24U);
    /** @brief Записывает прежний8-byte record в уже доказанную ёмкость proto0. */
    const auto emit=[&output](VideoOp op,unsigned arg,std::uint32_t value)noexcept{
        *output++={static_cast<std::uint32_t>(op)|((arg&255U)<<8U),value};
    };
    do{
        const unsigned count=std::min(left,8U-phase),end=phase+count;
        if(!phase){
            if(pixels){emit(VideoOp::Draw,pixels,pixelX|drawBits);pixels=0;}
            for(unsigned base:{unsigned(LowHigh),unsigned(HighHigh)}){
                std::uint32_t high;
                if(!shifts)high=backgroundAtoms_[base];
                else if(shifts==8U)high=backgroundAtoms_[base+1U];
                else if(shifts>=16U)high=VideoByteRef::literal;
                else{
                    const unsigned low=static_cast<std::uint8_t>(backgroundAtoms_[base+1U]);
                    const unsigned old=shifts<8U?static_cast<std::uint8_t>(backgroundAtoms_[base]):0U;
                    high=VideoByteRef::literal|(((((old<<8U)|low)<<shifts)>>8U)&255U);
                }
                backgroundAtoms_[base]=high;
                backgroundAtoms_[base+1U]=backgroundAtoms_[base+2U];
            }
            shifts=0;emit(VideoOp::BgLoad,0U,0U);
            address=static_cast<std::uint16_t>(0x2000U|(scroll&0x0fffU));
        }
        if(!pixels)pixelX=cursor-1U;
        pixels+=count;
        if(phase<=1U&&end>1U)
            tile=plan.readName<policy>(address,((cursor-1U)>>3U)+2U);
        if(phase<=3U&&end>3U){
            if(phase<=2U)address=static_cast<std::uint16_t>(0x23c0U|(scroll&0x0c00U)|
                ((scroll>>4U)&0x38U)|((scroll>>2U)&7U));
            const unsigned shift=((scroll>>4U)&4U)|(scroll&2U);
            const auto value=plan.readAttribute<policy>(address);
            attribute=static_cast<std::uint8_t>((value>>shift)&3U);
            emit(VideoOp::BgAttribute,shift,VideoByteRef::literal|value);
            address=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+
                unsigned(tile)*16U+((scroll>>12U)&7U));
        }
        if(phase<=5U&&end>5U){
            // Phase4 повторяет уже выставленный phase3 адрес; отдельный вход4
            // всё ещё обязан исправить произвольный held address после CPU MMIO.
            if(phase==4U)address=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+
                unsigned(tile)*16U+((scroll>>12U)&7U));
            const auto value=plan.readPattern<policy>(address);
            backgroundAtoms_[LowNext]=VideoByteRef::literal|value;
            emit(VideoOp::BgLow,0U,VideoByteRef::literal|value);
        }
        if(end>7U){
            if(phase<=6U){address=static_cast<std::uint16_t>((address+8U)&0x3fffU);coarse=true;}
            const auto value=plan.readPattern<policy>(address);
            backgroundAtoms_[HighNext]=VideoByteRef::literal|value;
            emit(VideoOp::BgHigh,0U,VideoByteRef::literal|value);
            if(coarse){
                if((scroll&0x001fU)==31U)scroll=static_cast<std::uint16_t>((scroll&0x7fe0U)^0x0400U);
                else ++scroll;
            }
            coarse=false;
        }else if(end==3U)
            address=static_cast<std::uint16_t>(0x23c0U|(scroll&0x0c00U)|((scroll>>4U)&0x38U)|((scroll>>2U)&7U));
        else if(end==5U&&phase==4U)
            address=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+unsigned(tile)*16U+((scroll>>12U)&7U));
        else if(end==7U){address=static_cast<std::uint16_t>((address+8U)&0x3fffU);coarse=true;}
        shifts=std::min(16U,shifts+count);
        cursor+=count;left-=count;phase=0U;
    }while(left);
    v_=scroll;busAddress_=address;nextTile_=tile;nextAttribute_=attribute;coarseXSignal_=coarse;
    pendingPixels_=pixels;pendingPixelX_=pixelX;pendingFill_=false;pendingPaletteIndex_=0;
    pendingBackgroundShifts_=static_cast<std::uint8_t>(shifts);
    spriteCacheClocks_=static_cast<std::uint16_t>(spriteCacheClocks_+dots);
    dot_=static_cast<std::uint16_t>(cursor);
    hot.header.eventCount+=static_cast<unsigned>(output-first);
}
