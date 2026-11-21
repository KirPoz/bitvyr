/**
 * @file Mmc5BackgroundRun.inl
 * @brief Единое packet/raster тело certified BG с подставляемым typed reader.
 * @details Включается только внутри private TimingPpu-метода с tiles/mapper.
 * NES_MMC5_BG_TILE меняет выборку данных; хвост planes, credits и выход прежние.
 */
    const unsigned start=dot_-1U;
    // Оба private caller доказывают 3..32 тайла. Все публикуемые tokens и три
    // хвостовые пары записываются ниже; нечётный padding обнуляет packet writer.
    std::array<std::uint32_t,32> tokens;
    std::array<std::array<std::uint32_t,2>,3> tail;
    std::uint8_t lastAttribute=0,lastLow=0,lastHigh=0,lastTile=0;
    unsigned lastShift=0;
    flushPixels();
    // Certificate исключает наблюдателей внутри run: mapper не читает TimingPpu,
    // а packet/event получает конечное состояние только после всех tile fetch.
    auto scroll=v_;
    unsigned tileX=backgroundFetchTileX();
    std::uint16_t lastHighAddress=busAddress_;
    for(unsigned i=0;i<tiles;++i){
        const unsigned shift=((scroll>>4U)&4U)|(scroll&2U);
        // Только последние три fetch нужны live planes; остальные идут CPU0 как ROM refs.
        const bool needPlanes=i+3U>=tiles;
        const auto data=NES_MMC5_BG_TILE(scroll,tileX,needPlanes);
        const auto tile=data.tile;
        const auto attribute=data.attribute;
        const Mmc5::PpuRead low{data.low,data.lowChrOffset};
        const Mmc5::PpuRead high{data.high,data.lowChrOffset==Mmc5::InvalidChrOffset?
            Mmc5::InvalidChrOffset:data.lowChrOffset+8U};
        const auto highAddress=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+
            unsigned(tile)*16U+((scroll>>12U)&7U)+8U);
        const auto lo=VideoByteRef::literal|low.value;
        const auto hi=VideoByteRef::literal|high.value;
        // Предикат гарантирует прямой индекс 0..2, без записи отбрасываемого ring-prefix.
        if(needPlanes)tail[i+3U-tiles]={lo,hi};
        tokens[i]=static_cast<std::uint32_t>(low.chrOffset)|(unsigned((attribute>>shift)&3U)<<30U);
        lastTile=tile;lastAttribute=attribute;lastLow=low.value;lastHigh=high.value;lastShift=shift;
        lastHighAddress=highAddress;
        if((scroll&0x001fU)==31U)scroll=static_cast<std::uint16_t>((scroll&0x7fe0U)^0x0400U);
        else ++scroll;
        ++tileX;
    }
    // Материализация предшествует packet/event и возврату к scalar tail. Геометрия
    // existing bulk остаётся прежней; Sprite0, PPUMASK и mapper /RD не расширяются.
    v_=scroll;busAddress_=lastHighAddress;coarseXSignal_=false;
    dot_=static_cast<std::uint16_t>(dot_+tiles*8U);
    // Весь run уже pre-reserved старым worst-case certificate. Последний tile
    // остаётся не нарисованным до прежнего следующего BgLoad/scalar flush.
    static_cast<void>(packet_->addMmc5SegmentRun(tiles-1U,start,scanline_,mask_,fineX_,tokens.data()));
    event(VideoOp::BgLoad);
    event(VideoOp::BgAttribute,lastShift,0,0,VideoByteRef::literal|lastAttribute);
    event(VideoOp::BgLow,0,0,0,VideoByteRef::literal|lastLow);
    event(VideoOp::BgHigh,0,0,0,VideoByteRef::literal|lastHigh);
    for(unsigned i=0;i<3U;++i){const auto& pair=tail[i];backgroundAtoms_[i]=pair[0];backgroundAtoms_[i+3U]=pair[1];}
    pendingPixels_=8U;pendingPixelX_=start+(tiles-1U)*8U;pendingBackgroundShifts_=8U;
    spriteCacheClocks_=static_cast<std::uint16_t>(spriteCacheClocks_+tiles*8U);
    nextTile_=lastTile;nextAttribute_=static_cast<std::uint8_t>((lastAttribute>>lastShift)&3U);
