/** @file Library.inl
 * @brief Main-only каталог, ZIP/SD, ROM lease и запуск нового сеанса после drain.
 * @details Private implementation fragment: включается только один раз из NesApp.cpp,
 * внутри прежнего namespace и после общего owner-state. Самостоятельным TU не является.
 */
/**
 * @brief Перерисовывает библиотеку игр и элементы навигации на Main. Индекс выбирается из текущего поколения GameLibrary.
 */
void NesApp::menu() noexcept {
    NES_LOOP_TAINT()
    clearSerialInput();
    board_.setAudioActive(false);
    board_.clear(0x0841);
    board_.drawText(24,16,"NES",0x07ff,4);
    const auto count = library_.count();
    if (count && selected_ >= count) selected_ = count - 1;
    const auto page = count ? selected_ / menuLayout::pageSize : 0;
    char line[112]{};
    std::snprintf(line,sizeof(line),"%u GAMES  PAGE %u/%u",unsigned(count),unsigned(count?page+1:0),unsigned(menuLayout::pages(count)));
    board_.drawText(132,26,line,0xffff,2);
    rectangle(board_.framebuffer(),440,16,100,40,0x21ec);
    board_.drawText(450,28,"SETUP",0xffff,2);
    rectangle(board_.framebuffer(),560,16,100,40,0x21ec);
    rectangle(board_.framebuffer(),680,16,100,40,0x21ec);
    board_.drawText(574,28,"<PREV",0xffff,2);
    board_.drawText(694,28,"NEXT>",0xffff,2);
    const auto start = menuLayout::first(selected_);
    const auto visible = start < count ? std::min(menuLayout::pageSize,count-start) : 0;
    for (std::size_t slot = 0; slot < visible; ++slot) {
        const auto i = start + slot;
        GameInfo info{};
        if (!library_.game(i, info)) continue;
        const int col = static_cast<int>(slot / 10), row = static_cast<int>(slot % 10);
        const int x = 20 + col*395, y = 76 + row*31;
        if (i == selected_) rectangle(board_.framebuffer(),x-4,y-4,385,28,0x21ec);
        std::snprintf(line,sizeof(line),"%02u %.27s",unsigned(i+1),info.name);
        for(auto* p=line;*p;++p) if(*p=='_') *p=' ';
        board_.drawText(x,y,line,i==selected_?0xffe0:0xffff,2);
    }
    if (count == 0) {
        board_.drawText(32,140,"NO NES GAMES ON SD",0xffe0,3);
        board_.drawText(32,185,"COPY .NES OR .ZIP TO nes_rom",0xffff,2);
        if (!board_.status().storage) board_.drawText(32,220,"SD CARD NOT READY",0xf800,2);
        else {
            std::snprintf(line,sizeof(line),"%.55s",library_.errorText());
            board_.drawText(32,220,line,0x8410,2);
        }
    }
    std::snprintf(line,sizeof(line),"XBOX: %s",gamepad_.statusText());
    board_.drawText(24,404,line,gamepad_.connected()?0x07e0:0xffe0,2);
    rectangle(board_.framebuffer(),640,390,140,36,0x21ec);
    board_.drawText(656,400,"RELOAD SD",0xffff,2);
    board_.drawText(24,432,"A: PLAY  UP/DOWN: CHOOSE  LEFT/RIGHT: PAGE",0xc618,2);
    board_.drawText(24,456,"B: SETTINGS  |  SELECT: RELOAD SD  |  GUIDE: MENU",0xc618,1);
    static_cast<void>(board_.present());
    menuDirty_ = false;
}

/**
 * @brief Повторно индексирует SD и обновляет выбор игры. Старые GameInfo после смены поколения больше не разрешают чтение.
 */
void NesApp::rescanLibrary() noexcept {
    if (!retireFrame()) return;
    stopPlayback();
    board_.clear(0x0841);
    board_.drawText(32,140,"READING SD CARD...",0xffff,3);
    static_cast<void>(board_.present());
    static_cast<void>(board_.refreshStorage());
    const bool opened = library_.open(romDirectory);
    selected_ = 0;
    menuDirty_ = true;
    ESP_LOGI(tag,"NES_LIBRARY source=sd games=%u skipped=%u ready=%u error=%s",
             unsigned(library_.count()),unsigned(library_.skipped()),unsigned(opened),library_.errorText());
}

/**
 * @brief Печатает ограниченную страницу каталога в UART. Имена экранируются; диапазон ограничен фактическим числом записей.
 * @param offset Нулевой индекс первой выводимой записи каталога.
 * @param limit Верхняя граница числа выдаваемых записей.
 */
void NesApp::catalog(std::size_t offset, std::size_t limit) const noexcept {
    std::printf("NES_CATALOG {\"source\":\"sd\",\"root\":\"/sdcard/nes_rom\",\"count\":%u,\"offset\":%u,\"skipped\":%u,\"error\":",
                unsigned(library_.count()),unsigned(offset),unsigned(library_.skipped()));
    jsonString(library_.errorText());
    std::printf("}\n");
    if (offset >= library_.count()) return;
    const auto end = offset + std::min(limit,library_.count()-offset);
    for (auto i=offset;i<end;++i) {
        GameInfo info{};
        if (!library_.game(i,info)) continue;
        std::printf("NES_ROM {\"index\":%u,\"title\":",unsigned(i)); jsonString(info.name);
        std::printf(",\"path\":"); jsonString(info.path);
        std::printf(",\"member\":"); jsonString(info.member);
        std::printf(",\"size\":%lu,\"crc32\":\"%08lx\",\"mapper\":%u,\"pal\":%s,\"archived\":%s}\n",
            static_cast<unsigned long>(info.size),static_cast<unsigned long>(info.crc),unsigned(info.mapper),
            info.pal?"true":"false",info.archived?"true":"false");
    }
}


/** @details Main читает файл только после штатного завершения предыдущей игры.
 * Путь берётся из текущего каталога, а не из произвольной UART-строки. CRC всего
 * архива отличается от CRC распакованного NES-member в catalog. Записи SD/NVS нет. */
void NesApp::archiveCheck(std::size_t index) const noexcept {
    if(playing_||pausedGame_||executionMode_==ExecutionMode::Faulted||
       (worker_&&worker_->inFlight)){
        std::printf("NES_ARCHIVE_CHECK_ERROR reason=menu_and_join_required\n");return;
    }
    GameInfo info{};
    if(!library_.game(index,info)){
        std::printf("NES_ARCHIVE_CHECK_ERROR reason=invalid_index\n");return;
    }
    std::uint32_t crc=0;
    std::uint64_t bytes=0;
    const auto result=nes::storage::ZipArchive::archiveCrc(info.path,crc,bytes);
    std::printf("NES_ARCHIVE_CHECK {\"index\":%u,\"bytes\":%llu,\"crc32\":\"%08lx\",\"error\":",
                unsigned(index),static_cast<unsigned long long>(bytes),static_cast<unsigned long>(crc));
    jsonString(nes::storage::ZipArchive::errorText(result));
    std::printf("}\n");
}

/**
 * @brief Загружает выбранную распакованную или ZIP-ROM во временную память и связывает новое поколение исполнения. При отказе не публикует частично созданный контекст Worker.
 * @param index Нулевой индекс записи текущего поколения каталога.
 */
/** @details Вызывается до load после завершения Worker. Блок переиспользуется без копий ROM; неизвестное владение запрещает освобождение. */
bool NesApp::prepareCartridgeSession(std::uint16_t nextMapper) noexcept {
    const bool enabled=nextMapper==5U; // Только MMC5 требует отдельной цифровой арены.
    if(worker_&&worker_->inFlight)return false;
    if(enabled&&!mmc5Session_){
        bool fallback=false;
        auto* memory=heap_caps_aligned_alloc(16,sizeof(Mmc5Session),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!memory){
            fallback=true;
            memory=heap_caps_aligned_alloc(16,sizeof(Mmc5Session),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        }
        if(!memory)return false;
        // Это один фиксированный metadata-блок вне hot loop, без копий ROM.
        // OOM сохраняет старый режим; фактическое размещение проверяется до new.
        auto* last=static_cast<std::uint8_t*>(memory)+sizeof(Mmc5Session)-1U;
        if((reinterpret_cast<std::uintptr_t>(memory)&15U)||!esp_ptr_byte_accessible(memory)||!esp_ptr_byte_accessible(last)){
            heap_caps_free(memory);return false;
        }
        const bool internal=esp_ptr_internal(memory)&&esp_ptr_internal(last);
        mmc5Session_=new(memory)Mmc5Session{};
        ESP_LOGI(tag,"NES_MMC5_STORAGE bytes=%u lifetime=app location=%s fallback=%u",unsigned(sizeof(Mmc5Session)),internal?"internal":"psram",unsigned(fallback));
    }
    if(mmc5Session_){auto*state=static_cast<Mmc5Session*>(mmc5Session_);machine_.configureMmc5(&state->digital,&state->audio);}
    if(worker_){
        if(worker_->audioPacketPending)return false;
        const bool split=worker_->activeAudio&&worker_->audioRenderer;
        // До load старый mapper ещё исполняет reset; оба MMC5 направления требуют capability.
        const bool expansion=core::Cartridge::hasExpansionAudioMapper(nextMapper)||
            core::Cartridge::hasExpansionAudioMapper(machine_.cartridge().mapper());
        if(enabled&&!split)return false; // M5 ON не подменяет отсутствующий DSP полным APU.
        // На joined load границе переустанавливается и capability, даже если bool split прежний.
        // Иначе переход NES→M5 либо M5→NES сохранил бы неверный тип аудиопакета.
        machine_.configureSplitAudio(split?worker_->activeAudio:nullptr,expansion&&split);
        if(machine_.splitAudio()!=split)return false;
        if(split&&(worker_->activeAudio->header().formatVersion!=core::AudioPacket::expansionVersion||
           worker_->activeAudio->header().capabilities!=(expansion?core::AudioPacket::externalDacQ16:0U)||
           machine_.audioFault()!=core::AudioFault::None))return false;
        worker_->audioSplit=split;
    }
    return true;
}
/**
 * @brief Запускает выбранную ROM только после завершения прежнего Worker и внешних video/audio loans.
 * @param index Индекс текущего каталога; CRC и фактические байты проверяются при чтении, индекс не является долговечным ключом ROM.
 * @details Новый образ временно принадлежит Main в PSRAM. Ошибка join/drain/UNBIND удерживает старых владельцев и запрещает заменить их новой allocation или сбросом C6.
 */
bool NesApp::startGame(std::size_t index) noexcept {
    // Повторный ROM разрешает только releaseCoupledForLoad с actual UNBIND;
    // poisoned owner не заменяется новой allocation или сбросом C6.
    if(executionMode_==ExecutionMode::Faulted)return false;
    NES_LOOP_TAINT()
    if (!retireFrame()) return false;
    stopPlayback();
    if(executionMode_==ExecutionMode::Faulted)return false;
    GameInfo info{};
    if (!library_.game(index,info)) {
        ESP_LOGE(tag,"ROM_NOT_FOUND index=%u",unsigned(index));
        return false;
    }
    board_.clear(0x0841);
    board_.drawText(32,90,"LOADING FROM SD...",0xffff,3);
    board_.drawText(32,145,info.name,0xffe0,1);
    static_cast<void>(board_.present());
    auto* image = static_cast<std::uint8_t*>(heap_caps_malloc(info.size,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!image) {
        ESP_LOGE(tag,"ROM PSRAM allocation failed size=%lu",static_cast<unsigned long>(info.size));
        board_.drawText(32,210,"NOT ENOUGH MEMORY FOR THIS GAME",0xf800,2);
        static_cast<void>(board_.present());
        vTaskDelay(pdMS_TO_TICKS(1500));
        return false;
    }
    std::uint32_t crc = 0;
    if (!library_.read(info,image,info.size,crc)) {
        heap_caps_free(image);
        ESP_LOGE(tag,"ROM_READ_FAIL index=%u path=%s error=%s",unsigned(index),info.path,library_.errorText());
        board_.drawText(32,210,"CANNOT READ THIS GAME",0xf800,2);
        char error[64]{};
        std::snprintf(error,sizeof(error),"%.55s",library_.errorText());
        board_.drawText(32,245,error,0x8410,2);
        static_cast<void>(board_.present());
        vTaskDelay(pdMS_TO_TICKS(1500));
        return false;
    }
    info.crc = crc;
    std::size_t imageBytes = info.size;
    if (info.mapper == 5U) {
        const auto padding = mmc5ChrPadding(image, imageBytes);
        if (padding.bytes) {
            // Размер и CRC в библиотеке описывают файл на SD. Видео получает
            // полный диапазон рабочей копии от начала заголовка до хвоста CHR.
            constexpr auto caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
            if (auto* expanded = static_cast<std::uint8_t*>(heap_caps_realloc(image, padding.bytes, caps))) {
                image = expanded;
                std::memset(image + imageBytes, 0xff, padding.bytes - imageBytes);
                image[5] = padding.chrBanks;
                imageBytes = padding.bytes;
            }
        }
    }
    currentPalHint_ = info.pal;
    info.pal = settings_.effectivePal(info.pal);
    if(!videoMapperSupported(info.mapper)){
        if(!setSplitVideoMode(false)){heap_caps_free(image);return false;}
        ESP_LOGI(tag,"NES_VIDEO_EXECUTION mode=legacy mapper=%u reason=unsupported",unsigned(info.mapper));
    }
    if(!releaseCoupledForLoad()){
        // Новый образ ещё не передан Machine; старый ROM и чужие заимствования удерживаются отдельно.
        heap_caps_free(image);
        workerFault("coupled_load_lease");return false;
    }
    if(!prepareCartridgeSession(info.mapper)){
        heap_caps_free(image);ESP_LOGE(tag,"ROM_LOAD_FAIL index=%u reason=cartridge_state_or_audio_owner",unsigned(index));return false;
    }
    const auto result = machine_.load(image,imageBytes,
                                      info.pal?core::Region::Pal:core::Region::Ntsc);
    if (result != core::LoadResult::Ok) {
        heap_caps_free(image);
        stopPlayback();
        ESP_LOGE(tag,"ROM_LOAD_FAIL index=%u mapper=%u reason=%s",unsigned(index),info.mapper,
                 machine_.cartridge().errorText(result));
        board_.clear(0x0841);
        board_.drawText(24,60,info.name,0xffff,2);
        board_.drawText(24,120,machine_.cartridge().errorText(result),0xf800,2);
        static_cast<void>(board_.present());
        vTaskDelay(pdMS_TO_TICKS(1500));
        menuDirty_ = true;
        return false;
    }
    heap_caps_free(activeRom_);activeRom_=image;activeRomBytes_=imageBytes;
    selected_=index;current_=info;
    // Читаем загруженный картридж и выбранный регион после успешного Machine::load.
    const auto mapper=machine_.cartridge().mapper();
    frameDirectPresent.enabled=frameDirectPresent.requested&&machine_.region()==core::Region::Ntsc&&
        (mapper==7U||mapper==71U);
    if(!configureVideoWorker()){workerFault("cpu_bench_launch");return false;}
    invalidateSnapshot();
    resetCounters();
    if(!configureCoupledVideoPipeline()){workerFault("coupled_launch");return false;}
    if(!configureFrameVideoPipeline()){workerFault("frame_pipeline_launch");return false;}
    playing_ = true;
    suppressedGamepad_ = gamepad_.buttons();
    suppressTouchUntilRelease_ = touchHeld_;
    static_cast<void>(applyAudioSettings());
    prepareGameDisplay();
    ESP_LOGI(tag,"NES_GAME_START index=%u name=%s mapper=%u region=%s crc=%08lx source=sd zip=%u size=%lu",
             unsigned(index),info.name,info.mapper,info.pal?"PAL":"NTSC",static_cast<unsigned long>(info.crc),
             unsigned(info.archived),static_cast<unsigned long>(info.size));
    return true;
}

