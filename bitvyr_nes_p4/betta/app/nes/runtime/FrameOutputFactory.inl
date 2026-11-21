/**
 * @file FrameOutputFactory.inl
 * @brief Setup/teardown ровно выбранного output backend на истинной joined границе.
 */

/** @details Все большие capability/packet значения размещены в отдельном Impl/Payload. */
FramePipeline* FramePipeline::createOutput(core::Machine&m,const std::uint8_t*rom,std::size_t bytes,
    core::Region region,std::uint64_t generation,core::AudioPacket&activeAudio,
    std::uint16_t*borrowedRgb,std::size_t borrowedWords
    ,Mmc5SessionSpan mmc5Session
    )noexcept {
    if(!validDirectRgb(borrowedRgb,borrowedWords,m,rom,bytes)||!rom||!bytes||!generation||
       m.cpuCycles()!=7U||m.frames()||m.region()!=region||!m.splitAudio()||
       m.outputFramesEnabled()||m.videoFault()!=core::VideoFault::None||
       m.audioFault()!=core::AudioFault::None||activeAudio.header().fault!=core::AudioFault::None||
       activeAudio.header().generation!=m.audioGeneration()||activeAudio.header().region!=region)return nullptr;
    // Прежняя joined materialization остаётся единственной; live module getters не добавляются.
    const bool mapper5=m.cartridge().mapper()==5;
    const bool sessionPresent=mmc5Session.data||mmc5Session.bytes;
    std::uintptr_t sessionFirst=0,sessionLast=0;
    if(sessionPresent){
        if(!checkedMemoryExtent(mmc5Session.data,mmc5Session.bytes,sessionFirst,sessionLast)||
           sessionFirst%16U)return nullptr;
        // Адаптер может выделить тот же app-lifetime metadata-блок во внутренней
        // памяти. Он по-прежнему целиком защищён span[7] до настоящего join;
        // разрешение INTERNAL не меняет требования к ROM/packet/RGB-буферам.
        const bool sessionMemory=rasterExternal(mmc5Session.data,mmc5Session.bytes)
            ||rasterInternal(mmc5Session.data,mmc5Session.bytes,16U)
            ;
        if(!sessionMemory)return nullptr;
    }
    if(mapper5&&(!sessionPresent||activeAudio.header().formatVersion!=core::AudioPacket::expansionVersion||
       activeAudio.header().capabilities!=core::AudioPacket::externalDacQ16))return nullptr;
    constexpr auto psram=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
    constexpr auto local=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    auto*a=static_cast<core::VideoPacket*>(heap_caps_aligned_alloc(alignof(core::VideoPacket),sizeof(core::VideoPacket),psram));
    auto*b=static_cast<core::VideoPacket*>(heap_caps_aligned_alloc(alignof(core::VideoPacket),sizeof(core::VideoPacket),psram));
    auto*x=static_cast<Impl::Payload*>(heap_caps_aligned_alloc(alignof(Impl::Payload),sizeof(Impl::Payload),psram));
    auto*y=static_cast<Impl::Payload*>(heap_caps_aligned_alloc(alignof(Impl::Payload),sizeof(Impl::Payload),psram));
    auto*c=static_cast<Impl::Control*>(heap_caps_aligned_alloc(alignof(Impl::Control),sizeof(Impl::Control),local));
    auto*storage=static_cast<core::VideoStorage*>(heap_caps_aligned_alloc(alignof(void*),sizeof(core::VideoStorage),psram));
    auto*renderer=static_cast<core::VideoRenderer*>(heap_caps_aligned_alloc(alignof(core::VideoRenderer),sizeof(core::VideoRenderer),local));
    auto*memory=static_cast<Impl*>(heap_caps_aligned_alloc(alignof(Impl),sizeof(Impl),psram));
    auto*handle=static_cast<FramePipeline*>(heap_caps_aligned_alloc(alignof(FramePipeline),sizeof(FramePipeline),psram));
    /** @brief Освобождает только raw ещё не заимствованные выделения до constructors/Core bind. */
    const auto freeRaw=[&]()noexcept{heap_caps_free(handle);heap_caps_free(memory);heap_caps_free(renderer);heap_caps_free(storage);heap_caps_free(c);heap_caps_free(y);heap_caps_free(x);heap_caps_free(b);heap_caps_free(a);};
    /** @brief Проверяет выравнивание реального результата allocator до placement-new. */
    const auto aligned=[](const void*p,std::size_t n)noexcept{return p&&reinterpret_cast<std::uintptr_t>(p)%n==0;};
    const bool valid=external(a,sizeof(*a))&&external(b,sizeof(*b))&&external(x,sizeof(*x))&&external(y,sizeof(*y))&&
        internal(c,sizeof(*c))&&external(storage,sizeof(*storage))&&internal(renderer,sizeof(*renderer))&&
        external(memory,sizeof(*memory))&&external(handle,sizeof(*handle))&&aligned(a,alignof(core::VideoPacket))&&
        aligned(b,alignof(core::VideoPacket))&&aligned(x,alignof(Impl::Payload))&&aligned(y,alignof(Impl::Payload))&&
        aligned(c,alignof(Impl::Control))&&aligned(storage,alignof(void*))&&aligned(renderer,alignof(core::VideoRenderer))&&
        aligned(memory,alignof(Impl))&&aligned(handle,alignof(FramePipeline));
    if(!valid){freeRaw();return nullptr;}
    // Bounded setup-only список содержит лишь адреса/размеры, не Binding/Ticket/Cut.
    const core::OutputProtectedSpan allocations[]={{a,sizeof(*a)},{b,sizeof(*b)},{x,sizeof(*x)},{y,sizeof(*y)},
        {c,sizeof(*c)},{storage,sizeof(*storage)},{renderer,sizeof(*renderer)},{memory,sizeof(*memory)},
        {handle,sizeof(*handle)},{borrowedRgb,rgbBytes},{&activeAudio,sizeof(activeAudio)},{&m,sizeof(m)},{rom,bytes}};
    for(unsigned i=0;i<sizeof(allocations)/sizeof(allocations[0]);++i){
        std::uintptr_t first=0,last=0;if(!checkedMemoryExtent(allocations[i].data,allocations[i].bytes,first,last)){freeRaw();return nullptr;}
        for(unsigned j=0;j<i;++j){
            const auto other=reinterpret_cast<std::uintptr_t>(allocations[j].data);
            if(first<other+allocations[j].bytes&&other<last){freeRaw();return nullptr;}
        }
    }
    if(sessionPresent){
        // Не читать module bytes: только попарные extent/alias проверки до constructors/bind.
        for(const auto& allocation:allocations){
            const auto first=reinterpret_cast<std::uintptr_t>(allocation.data);
            if(sessionFirst<first+allocation.bytes&&first<sessionLast){freeRaw();return nullptr;}
        }
    }
    ::new(a)core::VideoPacket{};::new(b)core::VideoPacket{};::new(x)Impl::Payload{};::new(y)Impl::Payload{};
    ::new(c)Impl::Control();::new(storage)core::VideoStorage{};
    ::new(renderer)core::VideoRenderer(*storage,core::VideoRenderer::RgbRowScratch{});
    const core::ColourLease contextLease{&m,rom,bytes,m.colourTransportEpoch(),generation,region};
    auto*s=::new(memory)Impl(c,x,y,borrowedRgb,m,contextLease,activeAudio.header());
    auto*result=::new(handle)FramePipeline(s);
    s->outputPackets[0]=a;s->outputPackets[1]=b;s->outputStorage=storage;s->outputRenderer=renderer;
    auto&binding=s->outputBinding;binding.source.producer=&m;binding.source.rom=rom;binding.source.romBytes=bytes;
    binding.source.romEpoch=contextLease.epoch;binding.source.generation=generation;binding.source.transportEpoch=0;binding.source.region=region;
    binding.packets[0]=a;binding.packets[1]=b;binding.snapshots[0]=x->ram.data();binding.snapshots[1]=y->ram.data();
    binding.protectedSpans[0]={&activeAudio,sizeof(activeAudio)};
    binding.protectedSpans[1]={&x->audio,sizeof(x->audio)};binding.protectedSpans[2]={&y->audio,sizeof(y->audio)};
    binding.protectedSpans[3]={borrowedRgb,rgbBytes};binding.protectedSpans[4]={storage,sizeof(*storage)};
    binding.protectedSpans[5]={renderer,sizeof(*renderer)};binding.protectedSpans[6]={c,sizeof(*c)};
    binding.protectedSpans[7]={mmc5Session.data,mmc5Session.bytes};
    // Impl/Payload capability results — исключительные writable поля. Все целые
    // allocation диапазоны выше проверены попарно, snapshot строго registered ram.
    const bool configured=m.configureOutputFrames(binding,s->outputLease);
    s->outputBound=m.outputFramesEnabled();s->outputWriterAttached=s->outputBound;
    if(!configured){
        s->fail(Error::Binding);
        // Только подтверждённый destroy может вернуть nullptr после Core bind.
        // При неизвестном lifecycle caller получает sticky handle и удерживает ROM.
        if(destroyJoined(result))return nullptr;
        return result;
    }
    s->lease.epoch=m.colourTransportEpoch();
    s->expectedAudio=activeAudio.header();s->mainAudio=activeAudio.header();
    s->outputChr=a->header().chrRom;s->outputChrBytes=a->header().chrRomBytes;
    s->outputVideoGeneration=a->header().generation;
    const auto image=reinterpret_cast<std::uintptr_t>(rom),chr=reinterpret_cast<std::uintptr_t>(s->outputChr);
    if(!s->outputVideoGeneration||a->header().sequence||a->header().startCycle||!a->header().seedPresent||
       s->expectedAudio.generation!=m.audioGeneration()||s->expectedAudio.sequence||s->expectedAudio.baseCycle||
       s->expectedAudio.region!=region||s->expectedAudio.fault!=core::AudioFault::None||
       (s->outputChr?(chr<image||chr-image>bytes||s->outputChrBytes>bytes-(chr-image)):s->outputChrBytes!=0)){
        s->fail(Error::Binding);if(destroyJoined(result))return nullptr;return result;
    }
    renderer->setRgb565Output(borrowedRgb);
    return result;
}

/** @details Только после общих real Worker/consumer/DSP park проверок destroyJoined. */
bool FramePipeline::Impl::releaseOutputJoined()noexcept {
    if(outputBound){
        if(outputWriterAttached){
            if(!producer.stopOutputWriter(outputLease))return false;
            outputWriterAttached=false;
        }
        for(auto*packet:outputPackets){
            if(packet->state()==core::VideoPacket::State::Sealed){if(!packet->release())return false;}
            else if(packet->state()!=core::VideoPacket::State::Free)return false;
        }
        if(!producer.cancelOutputFrames(outputLease))return false;
        outputBound=false;
    }
    // Прежний lifecycle доказал Worker/consumer/DSP joins. Row reset не трогает RGB;
    // аппаратный source fence по-прежнему принадлежит вызывающему App.
    if(outputRenderer){outputRenderer->reset();outputRenderer->setRgb565Output(nullptr);}
    return true;
}
