nes::c6audio::adapter::BatchAudioControl fullAudio;
nes::c6audio::adapter::RawIdleGate rawIdleGate;
std::uint32_t lastAudioEncodeCycles{},lastAudioQueueCycles{},lastAudioSourceCycles{};
nes::c6raw::batch::Stream batchStream;
/* Собственный C6 endpoint: включается внутрь existing single-worker anonymous namespace. */
/** @brief Четыре RX/reply слота сохраняются при timeout, DMA и неизвестном завершении. */
struct RawSlot final {
    std::atomic<unsigned> state{0}; // Free0/Writing1/Ready2/Computing3/Reply4/DMA5/PCMdone6/Quarantine7/Callback8.
    std::uint32_t ticket{};
    nes::c6raw::View parsed{};
    std::uint32_t rxCycles{};
    bool finishesRow{};
    bool releasesAudioPcm{};
    bool idleAck{};
    std::uint32_t readyCycles{},sendCycles{},doneCycles{};
    nes::c6audio::service::Envelope audioSource{};
    std::size_t bytes{},replyBytes{};
    std::array<std::uint8_t,nes::c6raw::requestPacketBytes> input{};
    std::array<std::uint8_t,nes::c6raw::replyPacketBytes> output{};
};
std::array<RawSlot,4> rawSlots;
std::atomic<std::uint32_t> nextRawTicket{0},rawDropped{0};
nes::c6_colour::RowSeed rawSeed;
nes::c6_colour::RowContext rawRow;
nes::c6_colour::RunResult rawResult;
nes::c6raw::Key rawRowKey;
std::uint64_t rawSession{};
std::uint16_t rawY{},rawPrefix{};
bool rawRowValid=false;
std::array<std::uint8_t,32> rawDirty{};
std::atomic<bool> rawFinishOutstanding{false};

/** @brief Пишет LE-поле без зависимости от C6 C++ ABI. */
void rawPut(std::uint8_t* out,std::uint64_t value,unsigned bytes) noexcept {for(unsigned i=0;i<bytes;++i)out[i]=std::uint8_t(value>>(8U*i));}
/** @brief Сверяет владельца row cache, исключая отдельные command opcode/sequence/time. */
bool sameRow(const nes::c6raw::Key& key) noexcept {return rawRowValid&&key.session==rawRowKey.session&&key.generation==rawRowKey.generation&&key.lease==rawRowKey.lease&&key.frame==rawRowKey.frame&&key.region==rawRowKey.region;}
/** @brief Возвращает RX-слот только после реального прекращения чтения DMA-конверта драйвера. */
void rawDone(void* context) noexcept {
    auto& slot=*static_cast<RawSlot*>(context);
    unsigned sending=5U;
    if(!slot.state.compare_exchange_strong(sending,8U,std::memory_order_acq_rel))return;
    // ticket и flags читаются до Free: новый RX вправе немедленно переиспользовать slot.
    const auto ticket=slot.ticket;const bool idleAck=slot.idleAck,pcm=slot.releasesAudioPcm;
    const auto index=std::uint32_t(&slot-rawSlots.data());
    if(slot.finishesRow)rawFinishOutstanding.store(false,std::memory_order_release);
    if(pcm)slot.doneCycles=esp_cpu_get_cycle_count();
    slot.state.store(pcm?6U:0U,std::memory_order_release);
    if(idleAck)(void)rawIdleGate.releaseAck(index,ticket);
    xTaskNotifyGive(taskHandle);
}
/** @brief Закрывает RX до полного service/control commit; любой другой Raw owner запрещает reset. */
bool rawEnterIdle(void* context) noexcept {
    const auto& current=*static_cast<RawSlot*>(context);
    if(!rawIdleGate.closeIdle())return false;
    bool free=current.state.load(std::memory_order_acquire)==3U&&!rawIdleGate.ackHeld()&&
        !fullAudio.sourceHeld()&&!batchStream.frameOpen()&&!rawRowValid&&!rawFinishOutstanding.load(std::memory_order_acquire);
    for(const auto&other:rawSlots)if(&other!=&current&&other.state.load(std::memory_order_acquire)!=0U)free=false;
    if(free&&rawIdleGate.armAck(std::uint32_t(&current-rawSlots.data()),current.ticket))return true;
    rawIdleGate.open();return false;
}
/** @brief Открывает только RX-admission; ACK loan остаётся до driver sourceDone. */
void rawLeaveIdle(void*) noexcept {rawIdleGate.open();}
/** @brief Только текущий computing слот; любые другие Raw loans/queued requests запрещают boot selection. */
bool rawSelectionOwnerFree(const RawSlot&current) noexcept {
    if(fullAudio.bound()||fullAudio.sourceHeld()||batchStream.frameOpen()||rawRowValid||rawFinishOutstanding.load(std::memory_order_acquire))return false;
    for(const auto&other:rawSlots)if(&other!=&current&&other.state.load(std::memory_order_acquire)!=0)return false;
    return current.state.load(std::memory_order_acquire)==3;
}
/** @brief Вычисляет HELLO/row seed/run/final row; пишет лишь локальный bounded reply. */
void rawExecute(RawSlot& slot) noexcept {
    const auto& view=slot.parsed;
    auto* out=slot.output.data()+nes::c6raw::headerBytes;
    std::uint32_t status=0;std::size_t body=0;const auto started=esp_cpu_get_cycle_count();
    if(view.key.opcode==nes::c6audio::lifecycle::unbindOpcode&&view.key.session==rawSession){
        nes::c6audio::service::IdleUnbindRequest request{};
        if(!nes::c6audio::lifecycle::decodeRequest(view.payload,view.size,request)||!nes::c6audio::lifecycle::matchesKey(view.key,request))status=36;
        else{
            const nes::c6audio::service::IdleAdmissionBarrier barrier{rawEnterIdle,rawLeaveIdle,&slot};
            const auto code=fullAudio.unbindIdle(request,barrier);
            status=code==nes::c6audio::service::Code::Ok?0U:100U+std::uint32_t(code);
            if(status)(void)rawIdleGate.releaseAck(std::uint32_t(&slot-rawSlots.data()),slot.ticket);
            else{
                slot.idleAck=true;
                auto queryKey=view.key;queryKey.opcode=14;
                const auto query=fullAudio.process(queryKey,nullptr,0,out+120,40,readClock,[]()noexcept{return std::uint32_t(uxTaskGetStackHighWaterMark(taskHandle));});
                rawPut(out,0,4);rawPut(out+4,0,4);rawPut(out+8,esp_clk_cpu_freq(),4);
                rawPut(out+12,slot.rxCycles,4);rawPut(out+16,0,4);rawPut(out+20,0,4);rawPut(out+24,0,4);rawPut(out+28,0,4);
                if(query.status||query.bytes!=40||!nes::c6audio::lifecycle::encodeRequest(request,out+32,88)||!nes::c6audio::lifecycle::decodeAck(out,160,request)){
                    slot.replyBytes=0;slot.state.store(7U,std::memory_order_release);return;
                }
                body=nes::c6audio::lifecycle::ackBytes-12U;
            }
        }
    }else if((view.key.opcode==12||view.key.opcode==13||view.key.opcode==14||(view.key.opcode>=0x40&&view.key.opcode<=0x46))&&view.key.session==rawSession){
        if(batchStream.frameOpen()||rawRowValid)status=30;
        else{const auto audio=fullAudio.process(view.key,view.payload,view.size,out+32,nes::c6raw::payloadBytes-32,readClock,[]()noexcept{return std::uint32_t(uxTaskGetStackHighWaterMark(taskHandle));});
            status=audio.status;body=20+audio.bytes;slot.releasesAudioPcm=audio.pcmSource;slot.audioSource=audio.source;
            rawPut(out+12,slot.rxCycles,4);rawPut(out+16,audio.operationCycles,4);rawPut(out+20,lastAudioEncodeCycles,4);rawPut(out+24,lastAudioQueueCycles,4);rawPut(out+28,lastAudioSourceCycles,4);}
    }else if(view.key.opcode==15&&view.key.session==rawSession&&rawSession){
        if(!rawSelectionOwnerFree(slot))status=std::uint32_t(nes::c6rollback::Code::OwnerBusy);
        else{std::size_t written=0;status=std::uint32_t(c6SelectRetainedA09(view.payload,view.size,out+12,nes::c6raw::payloadBytes-12,written));
            if(!status){if(written!=nes::c6rollback::replyBytes)status=std::uint32_t(nes::c6rollback::Code::PostVerify);else body=written;}}
    }else if(fullAudio.bound()&&view.key.opcode!=0&&view.key.opcode!=10)status=34;
    else if(view.key.opcode==0&&!fullAudio.permitsHello(view.key.session))status=35;
    else if(view.key.opcode==4&&view.key.session==rawSession){
        nes::c6raw::batch::Metrics metrics{};std::size_t written=0;
        status=std::uint32_t(batchStream.process(view.key,view.payload,view.size,out+12,nes::c6raw::payloadBytes-12,written,metrics,readClock));
        if(!status){body=written;nes::c6raw::batch::put(out+24,slot.rxCycles,4);}
    }else if(view.key.opcode==11&&view.key.session==rawSession){
        // Только immutable встроенный corpus; текущие colour state/очередь не переиспользуются.
        if(batchStream.frameOpen()||rawRowValid)status=30;
        else{const auto written=nes::audio_probe::filterProbe(view.payload,view.size,out+12,nes::c6raw::payloadBytes-12,readClock);if(!written)status=32;else body=written;}
    }else if(view.key.opcode==10&&view.size==0&&view.key.session==rawSession){
        if(batchStream.frameOpen()||rawRowValid)status=30;
        else{std::size_t written=0;const int rc=c6BootMeta(out+12,nes::c6raw::payloadBytes-12,written);if(rc)status=31;else body=written;}
    }else if(view.key.opcode==0&&view.size==0){
        rawSession=view.key.session;rawRowValid=false;batchStream.reset();const auto* app=esp_app_get_description();esp_chip_info_t info{};esp_chip_info(&info);
        rawPut(out+12,1,4);rawPut(out+16,nes::c6raw::helloCapabilities|nes::c6audio::lifecycle::capability,4);std::memcpy(out+20,app->app_elf_sha256,32);
        rawPut(out+52,app->secure_version,4);rawPut(out+56,info.revision,4);rawPut(out+60,esp_clk_cpu_freq(),4);
        rawPut(out+64,heap_caps_get_free_size(MALLOC_CAP_INTERNAL),4);rawPut(out+68,heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),4);
        rawPut(out+72,heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),4);rawPut(out+76,uxTaskGetStackHighWaterMark(taskHandle),4);
        rawPut(out+80,nes::c6raw::requestPacketBytes,4);rawPut(out+84,2,4);rawPut(out+88,sizeof(nes::c6_colour::Run),4);
        rawPut(out+92,nes::c6raw::replyPacketBytes,4);rawPut(out+96,nes::c6raw::directionalProfile,4);body=88;
    }else if(view.key.session!=rawSession||!rawSession)status=2;
    else if(view.key.opcode==1){
        nes::c6_colour::RowSeed seed{};
        if(view.size!=76||!nes::c6_colour::wire::decodeSeed(view.payload+2,74,seed)||view.payload[1]!=0||view.payload[0]>=240||view.key.region>1||bool(view.key.region)!=seed.pal)status=3;
        else{const bool continuation=sameRow(view.key)&&rawY==view.payload[0];const auto previousMask=rawRow.maskKey;
            if(!nes::c6_colour::prepareRow(seed,view.flags,rawRow))status=3;
            else if(continuation){if(rawPrefix<256&&(previousMask&0xe0U)!=(view.flags&0xe0U))nes::c6_colour::recolourPrefix(rawRow,rawResult.rgb.data(),rawResult.indices.data(),rawPrefix,std::uint8_t(view.flags>>5U));rawSeed=seed;}
            else{rawY=view.payload[0];rawSeed=seed;rawRowKey=view.key;rawPrefix=0;rawRowValid=true;rawDirty.fill(0);rawResult.after={};rawResult.rgb.fill(rawRow.rgbPalette[0]);rawResult.indices.fill(rawRow.indexPalette[0]);}}
    }else if(view.key.opcode==2){
        nes::c6_colour::Run run{};
        if(!sameRow(view.key)||!nes::c6_colour::wire::decodeRun(view.payload,view.size,run)||run.y!=rawY||run.x<rawPrefix)status=4;
        else{
            if(rawRow.maskKey!=(run.mask&0xe1U)){
                if(!nes::c6_colour::prepareRow(rawSeed,run.mask,rawRow)||(rawPrefix<256&&!nes::c6_colour::recolourPrefix(rawRow,rawResult.rgb.data(),rawResult.indices.data(),rawPrefix,std::uint8_t(run.mask>>5U))))status=4;
            }
            if(!status&&!nes::c6_colour::renderRun(rawRow,run,rawResult))status=4;
            if(!status){rawPrefix=std::uint16_t(run.x+run.tiles*8U);for(unsigned x=run.x;x<rawPrefix;++x)rawDirty[x>>3U]|=std::uint8_t(1U<<(x&7U));const auto& carry=rawResult.after;rawPut(out+12,carry.low,2);rawPut(out+14,carry.high,2);rawPut(out+16,carry.attrLow,2);rawPut(out+18,carry.attrHigh,2);out[20]=carry.nextLow;out[21]=carry.nextHigh;out[22]=carry.nextAttr;rawPut(out+23,carry.spriteClocks,2);rawPut(out+25,rawPrefix,2);body=15;}
        }
    }else if(view.key.opcode==3&&view.size==0){
        if(!sameRow(view.key))status=5;
        else{for(unsigned i=0;i<256;++i)rawPut(out+12+i*2U,rawResult.rgb[i],2);std::memcpy(out+524,rawResult.indices.data(),256);rawPut(out+780,rawPrefix,2);rawPut(out+782,rawY,2);std::memcpy(out+784,rawDirty.data(),32);body=804;slot.finishesRow=true;rawFinishOutstanding.store(true,std::memory_order_release);rawRowValid=false;}
    }else status=1;
    rawPut(out,status,4);rawPut(out+4,esp_cpu_get_cycle_count()-started,4);rawPut(out+8,esp_clk_cpu_freq(),4);
    if(status)body=0;
    const auto encodeBegin=esp_cpu_get_cycle_count();
    slot.replyBytes=nes::c6raw::encode(slot.output.data(),slot.output.size(),view.key,2,0,out,12+body);
    slot.readyCycles=esp_cpu_get_cycle_count();
    if(view.key.opcode==12||view.key.opcode==13||view.key.opcode==14||(view.key.opcode>=0x40&&view.key.opcode<=0x46))lastAudioEncodeCycles=slot.readyCycles-encodeBegin;
    slot.state.store(slot.replyBytes?4U:((slot.releasesAudioPcm||slot.idleAck)?7U:0U),std::memory_order_release);
}
/** @brief Обслуживает Ready строго по arrival ticket, затем неблокирующе публикует готовые ответы. */
void rawDrain() noexcept {
    // Только worker завершает service PCM borrow после actual driver source completion.
    for(auto& slot:rawSlots)if(slot.state.load(std::memory_order_acquire)==6U){
        if(fullAudio.sourceReleased(slot.audioSource)){lastAudioQueueCycles=slot.sendCycles-slot.readyCycles;lastAudioSourceCycles=slot.doneCycles-slot.sendCycles;slot.releasesAudioPcm=false;slot.state.store(0U,std::memory_order_release);}
        // При несовпадении completion состояние6 и весь context остаются до reboot.
    }
    for(unsigned pass=0;pass<4;++pass){RawSlot* oldest=nullptr;
        if(rawIdleGate.ackHeld()||rawFinishOutstanding.load(std::memory_order_acquire)||fullAudio.sourceHeld())break;
        for(auto& slot:rawSlots)if(slot.state.load(std::memory_order_acquire)==2&&(!oldest||std::int32_t(slot.ticket-oldest->ticket)<0))oldest=&slot;
        if(!oldest)break;
        unsigned ready=2;if(oldest->state.compare_exchange_strong(ready,3,std::memory_order_acquire))rawExecute(*oldest);
    }
    for(unsigned index=0;index<4;++index){auto& slot=rawSlots[index];unsigned ready=4;if(!slot.state.compare_exchange_strong(ready,5,std::memory_order_acquire))continue;
        slot.sendCycles=esp_cpu_get_cycle_count();
        if(!nes_hosted_raw_try_tx(0,slot.output.data(),slot.replyBytes,&slot,rawDone) && !nes_hosted_raw_try_tx(1,slot.output.data(),slot.replyBytes,&slot,rawDone))slot.state.store(4,std::memory_order_release);
    }
}
/** @brief Hosted RX копирует только валидный raw request и сразу освобождает borrowed transport input. */
void receiveRaw(const std::uint8_t* bytes,std::size_t count) noexcept {
    const nes::c6audio::adapter::RawRxLease admission(rawIdleGate);
    if(!admission.held()){rawDropped.fetch_add(1,std::memory_order_relaxed);return;}
    const auto rxStarted=esp_cpu_get_cycle_count();
    nes::c6raw::View view{};if(!taskHandle||!nes::c6raw::decode(bytes,count,view)||view.kind!=1){rawDropped.fetch_add(1,std::memory_order_relaxed);return;}
    for(auto& slot:rawSlots){unsigned free=0;if(!slot.state.compare_exchange_strong(free,1,std::memory_order_acquire))continue;
        std::memcpy(slot.input.data(),bytes,count);slot.bytes=count;slot.finishesRow=false;slot.releasesAudioPcm=false;slot.idleAck=false;slot.parsed=view;slot.parsed.payload=slot.input.data()+nes::c6raw::headerBytes;slot.rxCycles=esp_cpu_get_cycle_count()-rxStarted;slot.ticket=nextRawTicket.fetch_add(1,std::memory_order_relaxed);slot.state.store(2,std::memory_order_release);xTaskNotifyGive(taskHandle);return;}
    rawDropped.fetch_add(1,std::memory_order_relaxed);
}
