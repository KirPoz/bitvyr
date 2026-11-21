/**
 * @file Mmc5PhaseSegment.inl
 * @brief Единый guard/debt/packet алгоритм короткого endpoint с typed reader.
 * @details Включается внутри соответствующего метода; dots задаёт разрешённый отрезок.
 * NES_MMC5_PHASE_TILE и NES_MMC5_PHASE_STEP сохраняют прежние точки наблюдения.
 */
    const unsigned begin=dot_;
    const unsigned phase=(begin-1U)&7U;
    if(!dots||dots>8U-phase||begin<9U||begin>=255U||dots>255U-begin||
       pal_||trackBusCycles_||trackBusAddresses_||this!=&bus_.timingPpu_||bus_.cartridge_.mapper()!=5U||
       !packet_||packet_!=bus_.videoPacket_||capture_||!bus_.outputOwner_||
       !bus_.outputJob_||bus_.outputInitializing_||bus_.videoObserver_||bus_.videoObservableOnly_||
       unsigned(scanline_)>=240U||!renderingEnabled_||!renderingNext_||!(mask_&0x18U)||
       renderDelay_||nmiDelay_||nmiPending_||frameReady_||(status_&0x80U)||
       fineX_>7U||(v_&0x8000U)||(t_&0x8000U)||nextAttribute_>3U||
       !spriteCacheValid_||spriteCacheClocks_>256U-dots||pendingBackgroundShifts_>16U||
       pendingPixels_>256U-dots||!packet_->canAppendMmc5H2())return false;
    const unsigned x=begin-1U;
    if(pendingPixels_&&(pendingFill_||pendingPaletteIndex_||pendingPixelX_>=256U||
       pendingPixelX_+pendingPixels_!=x))return false;
    // Отрезок целиком в одном байте маски. Здесь x>=8 и x<=253: left clip и
    // запрет hit на x255 не требуют расширения исходного Sprite0-контракта.
    if(spriteZeroRow_&&!(status_&0x40U)&&(mask_&0x18U)==0x18U){
        const unsigned mask=(0xffU>>(8U-dots))<<phase;
        if(spriteZeroMask_[x>>3U]&mask)return false;
    }
    auto* const mapper=bus_.mmc5QuietMapper();
    if(!mapper)return false;
    // finishQuietBackground материализует I0; произвольный private/test вход I1
    // сохраняет исходный idle через настоящие read-фазы. Owned replay начинает с I0.
    const bool whole=phase==0U&&dots==8U&&mapper->cpuIdle_==0U&&mapper->canRunQuietBackground();
    if(!phase)loadBackground();
    if(!pendingPixels_){pendingPixelX_=x;pendingFill_=false;pendingPaletteIndex_=0;}
    if(whole){
        const auto scroll=v_;
        const auto data=NES_MMC5_PHASE_TILE(scroll,backgroundFetchTileX());
        const unsigned shift=((scroll>>4U)&4U)|(scroll&2U);
        nextTile_=data.tile;nextAttribute_=static_cast<std::uint8_t>((data.attribute>>shift)&3U);
        backgroundAtoms_[LowNext]=VideoByteRef::literal|data.low;
        backgroundAtoms_[HighNext]=VideoByteRef::literal|data.high;
        event(VideoOp::BgAttribute,shift,0,0,VideoByteRef::literal|data.attribute);
        event(VideoOp::BgLow,0,0,0,VideoByteRef::literal|data.low);
        event(VideoOp::BgHigh,0,0,0,VideoByteRef::literal|data.high);
        busAddress_=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+
            unsigned(data.tile)*16U+((scroll>>12U)&7U)+8U);
        incrementX();coarseXSignal_=false;mapper->finishQuietBackground(busAddress_);
    }else{
        unsigned remaining=dots;
        switch(phase){
        case 0:NES_MMC5_PHASE_STEP(0);if(!--remaining)break;[[fallthrough]];
        case 1:NES_MMC5_PHASE_STEP(1);if(!--remaining)break;[[fallthrough]];
        case 2:NES_MMC5_PHASE_STEP(2);if(!--remaining)break;[[fallthrough]];
        case 3:NES_MMC5_PHASE_STEP(3);if(!--remaining)break;[[fallthrough]];
        case 4:NES_MMC5_PHASE_STEP(4);if(!--remaining)break;[[fallthrough]];
        case 5:NES_MMC5_PHASE_STEP(5);if(!--remaining)break;[[fallthrough]];
        case 6:NES_MMC5_PHASE_STEP(6);if(!--remaining)break;[[fallthrough]];
        case 7:NES_MMC5_PHASE_STEP(7);break;
        }
    }
    pendingPixels_+=dots;
    pendingBackgroundShifts_=static_cast<std::uint8_t>(std::min(16U,unsigned(pendingBackgroundShifts_)+dots));
    spriteCacheClocks_=static_cast<std::uint16_t>(spriteCacheClocks_+dots);
    dot_=static_cast<std::uint16_t>(begin+dots);
    return true;
