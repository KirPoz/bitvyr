/** @file @brief O(1) prefix endpoint и настоящий tail3; RGB восстанавливает consumer. */
    const unsigned start=dot_-1U,first=tiles-3U;
    const auto initialScroll=v_;
    const unsigned initialTileX=backgroundFetchTileX();
    std::array<std::uint32_t,14> view{};
    view[0]=unsigned(initialScroll)|(unsigned(control_)<<16U)|(initialTileX<<24U);
    view[1]=unsigned(mapper.ntMap_)|(unsigned(mapper.exMode_)<<8U)|(unsigned(mapper.fillTile_)<<16U)|(unsigned(mapper.fillAttribute_)<<24U);
    view[2]=Policy|(unsigned(mapper.splitControl_)<<8U)|(unsigned(mapper.splitY_)<<16U)|(unsigned(mapper.splitSlMode_)<<24U)|(unsigned(mapper.inFrame_)<<25U)|(unsigned(mapper.nameRepeat_)<<26U);
    view[3]=plan.chrMask_;view[4]=plan.extendedBase_;view[5]=plan.splitBase_;
    for(unsigned i=0;i<8U;++i)view[6U+i]=plan.normalBackground_[i];
    /** @brief Сохраняет coarseX wrap и прежнее очищение bit15 только при wrap. */
    const auto advanceX=[](std::uint16_t value,unsigned count) noexcept {
        const unsigned sum=(value&31U)+count;
        return static_cast<std::uint16_t>(((value&(sum>=32U?0x7fe0U:0xffe0U))^(((sum>>5U)&1U)*0x400U))|(sum&31U));
    };
    flushPixels();
    if constexpr((Policy&2U)!=0U){
        // Последний active Name сохраняет stale split-поля, если весь tail уже outside.
        if(initialTileX<32U){unsigned last=std::min(initialTileX+tiles-1U,31U);bool active=true;
            if(plan.splitRight_)active=last>=std::max(initialTileX,unsigned(plan.splitThreshold_));
            else if(!plan.splitThreshold_)active=false;
            else{last=std::min(last,unsigned(plan.splitThreshold_)-1U);active=last>=initialTileX;}
            if(active&&last-initialTileX<first){const auto kept=advanceX(initialScroll,last-initialTileX);
                static_cast<void>(plan.template nameData<static_cast<Mmc5RasterPlan::Policy>(Policy)>(std::uint16_t(0x2000U|(kept&0x0fffU)),last));}
        }
    }
    auto scroll=advanceX(initialScroll,first);unsigned tileX=initialTileX+first;
    std::array<std::array<std::uint32_t,2>,3> tail;
    std::uint8_t lastTile=0,lastAttribute=0,lastLow=0,lastHigh=0;unsigned lastShift=0;std::uint16_t lastHighAddress=busAddress_;
    for(unsigned i=0;i<3U;++i){
        const auto data=plan.template fetch<static_cast<Mmc5RasterPlan::Policy>(Policy)>(scroll,tileX,true);
        tail[i]={VideoByteRef::literal|data.low,VideoByteRef::literal|data.high};
        lastTile=data.tile;lastAttribute=data.attribute;lastLow=data.low;lastHigh=data.high;lastShift=((scroll>>4U)&4U)|(scroll&2U);
        lastHighAddress=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+unsigned(data.tile)*16U+((scroll>>12U)&7U)+8U);
        scroll=advanceX(scroll,1U);++tileX;
    }
    v_=scroll;busAddress_=lastHighAddress;coarseXSignal_=false;dot_=static_cast<std::uint16_t>(dot_+tiles*8U);
    static_cast<void>(packet_->addMmc5ViewSpan(tiles-1U,start,scanline_,mask_,fineX_,view));
    event(VideoOp::BgLoad);event(VideoOp::BgAttribute,lastShift,0,0,VideoByteRef::literal|lastAttribute);
    event(VideoOp::BgLow,0,0,0,VideoByteRef::literal|lastLow);event(VideoOp::BgHigh,0,0,0,VideoByteRef::literal|lastHigh);
    for(unsigned i=0;i<3U;++i){backgroundAtoms_[i]=tail[i][0];backgroundAtoms_[i+3U]=tail[i][1];}
    pendingPixels_=8U;pendingPixelX_=start+(tiles-1U)*8U;pendingBackgroundShifts_=8U;
    spriteCacheClocks_=static_cast<std::uint16_t>(spriteCacheClocks_+tiles*8U);
    nextTile_=lastTile;nextAttribute_=static_cast<std::uint8_t>((lastAttribute>>lastShift)&3U);
