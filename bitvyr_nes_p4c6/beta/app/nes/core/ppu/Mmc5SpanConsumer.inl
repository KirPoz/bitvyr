/** @file @brief RUN consumer view без CPU/PPU-timing: только immutable byte history. */
VideoRenderResult VideoRenderer::mmc5SpanEvent(const VideoEvent& h,const VideoEvent* p,unsigned available,unsigned& slots) noexcept {
    if(h.op()==VideoOp::Mmc5Seed){
        slots=641U;
        if(available<slots||h.arg0()!=1U||h.arg1()||h.arg2()||h.value!=5120U||mmc5Seeded_)return VideoRenderResult::InvalidPacket;
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(p);
        std::memcpy(names_.data(),bytes,4096U);std::memcpy(mmc5ExRam_.data(),bytes+4096U,1024U);mmc5Seeded_=true;return VideoRenderResult::Ok;
    }
    if(!mmc5Seeded_)return VideoRenderResult::MissingSeed;
    if(h.op()==VideoOp::Mmc5PhysicalWrite){
        const unsigned physical=h.arg0()|(h.arg1()<<8U);
        if(physical>=3072U||h.arg2()||h.value>255U)return VideoRenderResult::InvalidPacket;
        if(physical<2048U)names_[physical]=static_cast<std::uint8_t>(h.value);
        else mmc5ExRam_[physical-2048U]=static_cast<std::uint8_t>(h.value);
        return VideoRenderResult::Ok;
    }
    slots=8U;
    const unsigned count=h.arg0(),x=h.value&255U,y=(h.value>>8U)&255U,fine=h.value>>24U;
    if(available<slots||h.op()!=VideoOp::Mmc5Span||count<2U||count>31U||h.arg1()!=1U||h.arg2()||x+count*8U>256U||y>=240U||fine>7U)return VideoRenderResult::InvalidPacket;
    std::array<std::uint32_t,14> v{};std::memcpy(v.data(),p,56U);
    unsigned scroll=v[0]&65535U,tileX=v[0]>>24U,control=(v[0]>>16U)&255U;
    const unsigned nt=v[1]&255U,exmode=(v[1]>>8U)&3U,fill=v[1]>>16U,policy=v[2]&3U;
    const unsigned splitControl=(v[2]>>8U)&255U,splitY=(v[2]>>16U)&255U;
    const bool sl=(v[2]&(1U<<24U))!=0,inFrame=(v[2]&(1U<<25U))!=0;
    const unsigned nameRepeat=(v[2]>>26U)&3U,mask=v[3];
    if(tileX>33U||tileX+count>34U||(v[1]&0x0000fc00U)||(v[2]&0xf00000fcU)||
       !rom_||romBytes_<1024U||romBytes_>1024U*1024U||(romBytes_&(romBytes_-1U))||mask!=romBytes_-1U||
       (v[4]&0x3ffffU)||(v[5]&4095U))return VideoRenderResult::InvalidReference;
    for(unsigned i=0;i<8U;++i)if((v[6U+i]&1023U)||v[6U+i]>mask||mask-v[6U+i]<1023U)return VideoRenderResult::InvalidReference;
    /** @brief Разрешает физический NT/fill источник текущего immutable descriptor. */
    const auto readName=[&](unsigned address)noexcept->unsigned{
        const unsigned source=(nt>>(((address>>10U)&3U)*2U))&3U,offset=address&1023U;
        if(source<2U)return names_[source*1024U+offset];
        if(source==2U)return exmode<2U?mmc5ExRam_[offset]:0U;
        return offset<0x3c0U?(fill&255U):(fill>>8U);
    };
    std::array<VideoEvent,16> tokens{};
    for(unsigned i=0;i<count;++i,++tileX){
        const unsigned name=0x2000U|(scroll&0x0fffU),shift=((scroll>>4U)&4U)|(scroll&2U);
        const unsigned attrAddress=0x23c0U|(scroll&0x0c00U)|((scroll>>4U)&0x38U)|((scroll>>2U)&7U);
        const unsigned ex=mmc5ExRam_[name&1023U];
        const unsigned threshold=splitControl&31U;
        const bool split=(policy&2U)&&tileX<32U&&((splitControl&0x40U)?tileX>=threshold:tileX<threshold);
        unsigned tile=0,attribute=0,offset=0;
        if(split){
            const unsigned sy=inFrame&&(tileX<2U||nameRepeat==3U)?(splitY==239U?0U:(splitY+1U)&255U):splitY;
            tile=mmc5ExRam_[(sy>>3U)*32U+tileX];
            const unsigned a=mmc5ExRam_[0x3c0U+(sy>>5U)*8U+(tileX>>2U)];
            const unsigned as=((sy>>3U)&2U)*2U+(tileX&2U);attribute=((a>>as)&3U)*0x55U;
            unsigned address=(((control&0x10U)?0x1000U:0U)|(tile<<4U)|((scroll>>12U)&7U))&0xfffU;
            if(sl){address=(address&~7U)|(sy&7U);}offset=(v[5]+address)&mask;
        }else{
            tile=readName(name);
            const unsigned address=((control&0x10U)?0x1000U:0U)|(tile<<4U)|((scroll>>12U)&7U);
            if(policy&1U){attribute=(ex>>6U)*0x55U;offset=(v[4]+((ex&63U)<<12U)+(address&0xfffU))&mask;}
            else{attribute=readName(attrAddress);offset=v[6U+(address>>10U)]+(address&1023U);}
        }
        if(offset>mask||mask-offset<8U)return VideoRenderResult::InvalidReference;
        const auto token=offset|(((attribute>>shift)&3U)<<30U);
        if(i&1U)tokens[i/2U].value=token;else tokens[i/2U].tag=token;
        if((scroll&31U)==31U)scroll=(scroll&0x7fe0U)^0x400U;else scroll=static_cast<std::uint16_t>(scroll+1U);
    }
    const VideoEvent header{static_cast<std::uint32_t>(VideoOp::BgRun)|(count<<8U),h.value};
    return backgroundRun(header,tokens.data());
}
