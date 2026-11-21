/**
 * @file MachineVideo.cpp
 * @brief Выбор видеофронтенда, точные mapper-сигналы и мосты packet/replay-режимов.
 * @ingroup nes_core
 */
#include "Machine.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"

#if defined(NES_CORE_SPLIT_PPU) && NES_CORE_SPLIT_PPU
namespace nes::core {

// Один тайл phase0. В конкретном графе Machine нет обратного наблюдателя PPU,
// поэтому текущие значения публикуются на границе интервала вызывающего.
// Память читается ДО соответствующего Legacy-строба; старшая FD/FE-защёлка
// может выбрать физические страницы только следующего тайла.
/**
 * @brief Получает один тайл с точно выбранными тактами, адресами и стробами.
 * @details Физические байты читаются до соответствующего Legacy-строба; высокое FD/FE-чтение может изменить только последующие страницы.
 * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
 * @return Результат четырёх настоящих фоновых чтений с нужными данными и физической low-ссылкой.
 */
template<bool Clocked,bool Addressed,bool ReadEffects,bool ComposeMmc3>
/** @details Выбирает точную compile-time policy уже разрешённого background tile; гостевой горизонт не расширяется. */
Machine::VideoTileFetch Machine::directVideoBackgroundTile(std::uint16_t v,unsigned control,const TimingPpu* caller) noexcept {
    /**
     * @brief Продвигает предусмотренный mapper-clock текущей фазы до соответствующего адресного эффекта.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto clock=[this]() noexcept {if constexpr(Clocked&&!ComposeMmc3)cartridge_.ppuCycle();};
    /**
     * @brief Выставляет исходный PPU-адрес с выбранными mapper-эффектами; повторный адресный drive не удаляется.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto drive=[this](std::uint16_t a) noexcept {
        if constexpr(Addressed&&!ComposeMmc3){
            if constexpr(ReadEffects)cartridge_.legacy_.ppuAddress(a);
            else cartridge_.ppuAddress(a);
        }
    };
    /**
     * @brief Применяет read-защёлку после получения старого байта и обновляет CHR-страницы, если нужно.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto strobe=[this]() noexcept {
        if constexpr(ReadEffects){if(cartridge_.legacy_.ppuRead())cartridge_.refreshChrPages();}
    };
    clock();const auto nt=static_cast<std::uint16_t>(0x2000U|(v&0x0FFFU));drive(nt);
    clock();const auto tile=nametables_[nametablePages_[(nt>>10)&3U]|(nt&1023U)];strobe();
    clock();const auto at=static_cast<std::uint16_t>(0x23C0U|(v&0x0C00U)|((v>>4)&0x38U)|((v>>2)&7U));drive(at);
    clock();const auto raw=elideVideoCaptureData(caller)?std::uint8_t{0}:nametables_[nametablePages_[(at>>10)&3U]|(at&1023U)];strobe();
    const unsigned attr=(raw>>(((v>>4)&4U)|(v&2U)))&3U;
    const auto pattern=static_cast<std::uint16_t>(((control&0x10U)?0x1000U:0U)+unsigned(tile)*16U+((v>>12)&7U));
    drive(pattern);clock();drive(pattern);clock();
    const auto* lowByte=cartridge_.chrPages_[(pattern>>10)&7U]+(pattern&1023U);
    const auto ref=cachedVideoReference(lowByte);
#if defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
    const std::uint8_t low=0;strobe();clock();drive(pattern+8U);clock();
    const std::uint8_t high=0;
#else
    const auto low=*lowByte;strobe();clock();drive(pattern+8U);clock();
    std::uint8_t high;
    if constexpr(ReadEffects){
        const auto* highByte=cartridge_.chrPages_[((pattern+8U)>>10)&7U]+((pattern+8U)&1023U);
        high=*highByte;
    }else high=lowByte[8];
#endif
    strobe();
    if constexpr(ComposeMmc3)cartridge_.advanceMmc3TileSignals((control&0x10U)!=0);
    return {std::uint32_t(low)|(std::uint32_t(high)<<8)|(attr<<16)|(std::uint32_t(tile)<<24),ref};
}

/**
 * @brief Выбирает конкретный обработчик тайла для текущих проводов и mapper.
 * @details Выбор делается для ограниченного интервала; неподдерживаемая комбинация возвращает прежний общий путь.
 * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
 * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @return Вычисленное значение или состояние типа Machine::VideoTileReader согласно описанному контракту.
 */
Machine::VideoTileReader Machine::videoTileReader(bool clocked,bool addressed,[[maybe_unused]] unsigned tiles,[[maybe_unused]] unsigned control) noexcept {
    if(cartridge_.mapper()==5)return nullptr; // MMC5 требует typed скалярные чтения этого прототипа.
    if(!cartridge_.loaded())return nullptr; // Сохранить холодную семантику незагруженного картриджа.
    const bool effects=cartridge_.hasPpuReadSideEffects();
#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    if(!clocked&&addressed&&usesLegacyTileSignals(tiles,(control&0x10U)!=0))return &Machine::videoLegacyBackgroundTile;
#endif
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    if(clocked&&addressed&&cartridge_.mapper()==4&&cartridge_.ppuIrqQuietFor(tiles*8U,(control&0x10U)!=0))
        return &Machine::directVideoBackgroundTile<true,true,false,true>;
#endif
    if(clocked){
        if(addressed)return effects?&Machine::directVideoBackgroundTile<true,true,true>:&Machine::directVideoBackgroundTile<true,true,false>;
        return effects?&Machine::directVideoBackgroundTile<true,false,true>:&Machine::directVideoBackgroundTile<true,false,false>;
    }
    if(addressed)return effects?&Machine::directVideoBackgroundTile<false,true,true>:&Machine::directVideoBackgroundTile<false,true,false>;
    return effects?&Machine::directVideoBackgroundTile<false,false,true>:&Machine::directVideoBackgroundTile<false,false,false>;
}

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS && defined(NES_CORE_SPRITE_SLOT_SIGNALS) && NES_CORE_SPRITE_SLOT_SIGNALS
/**
 * @brief Выбирает точный режим шинной композиции fetch-слота спрайта.
 * @details Clocked/addressed, таблица паттернов и mapper проверяются до замены обычной последовательности.
 * @param pattern Адрес паттерна текущей плоскости в момент fetch.
 * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
 * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
 * @return Вычисленное значение или состояние типа Machine::VideoSpriteMode согласно описанному контракту.
 */
Machine::VideoSpriteMode Machine::videoSpriteMode(std::uint16_t pattern,bool clocked,bool addressed) const noexcept {
    if(!cartridge_.loaded()||pattern>=0x2000U||(pattern&8U))return VideoSpriteMode::None;
    [[maybe_unused]] const bool high=(pattern&0x1000U)!=0;
#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN
    if(!clocked&&addressed&&usesLegacyTileSignals(1,high))return VideoSpriteMode::Legacy;
#endif
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN
    if(clocked&&addressed&&usesMmc3SpriteSignals(high))return VideoSpriteMode::Mmc3;
#endif
    if(!clocked&&!addressed){
        switch(cartridge_.mapper()){
        // 69/85 не имеют PPU read-latch/A12 clock; CPU IRQ/FM/PSG не пропускаются здесь.
        case 0:case 1:case 2:case 3:case 7:case 69:case 85:return VideoSpriteMode::Plain;
        default:break;
        }
    }
    return VideoSpriteMode::None;
}
/**
 * @brief Получает либо связывает две плоскости спрайта с исходным порядком стробов.
 * @details Старую физическую идентичность фиксируют до высокого Legacy-чтения; оптимизация не удаляет A12 и побочные эффекты.
 * @param pattern Адрес паттерна текущей плоскости в момент fetch.
 * @param mode Выбор вычисления sprite-данных для packet/capture; временные и шинные эффекты сохраняются.
 * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
 * @return Упакованные настоящие low/high-значения sprite-пары и её проверенная физическая ссылка.
 */
Machine::VideoTileFetch Machine::videoSpritePlanes(std::uint16_t pattern,[[maybe_unused]] VideoSpriteMode mode,const TimingPpu* caller) noexcept {
    const auto* low=cartridge_.chrPages_[(pattern>>10)&7U]+(pattern&1023U);
    // Старшее FD/FE-чтение может заменить chrPages_. Зафиксировать старую физическую
    // идентичность ДО загрузки байта старого банка или фиксации защёлки.
    // Удаление данных допустимо чистым мапперам без packet-consumer и заимствованной плоскости.
    const auto reference=elideVideoCaptureData(caller)?0U:cachedVideoReference(low);
#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN
    if(mode==VideoSpriteMode::Legacy)return {legacySpritePlanes(pattern),reference};
#endif
    const auto planes=std::uint32_t(low[0])|(std::uint32_t(low[8])<<8);
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN
    if(mode==VideoSpriteMode::Mmc3)advanceMmc3TileSignals((pattern&0x1000U)!=0);
#endif
    return {planes,reference};
}
#endif

#if defined(NES_CORE_VIDEO_POLICY_CACHE) && NES_CORE_VIDEO_POLICY_CACHE
/**
 * @brief Пересчитывает неизменяемую для текущего режима часть видеополитики.
 * @details Идентичность вызывающего PPU, временный packet/capture и активное владение всё равно проверяются в месте использования.
 */
void Machine::refreshVideoPolicy() noexcept {
#if !(defined(NES_CORE_PPU_OBSERVABLE_PURE_MAPPERS) && NES_CORE_PPU_OBSERVABLE_PURE_MAPPERS)
    const auto mapper=cartridge_.mapper();
#endif
    videoPolicy_=static_cast<std::uint8_t>((videoPacket_||videoObservableOnly_
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
        ||outputOwner_
#endif
    )&&videoCapable_);
    if(videoObservableOnly_&&!videoPacket_&&videoCapable_&&cartridge_.loaded()&&
#if defined(NES_CORE_PPU_OBSERVABLE_PURE_MAPPERS) && NES_CORE_PPU_OBSERVABLE_PURE_MAPPERS
       // Загрузка проверила модель; свойство чтений постоянно до смены типа маппера.
       // Legacy160 исключён; настоящие плоскости, такты и адресные стробы сохранены.
       !cartridge_.hasPpuReadSideEffects())videoPolicy_|=2U;
#else
       (mapper==0||mapper==2||mapper==4)&&!cartridge_.hasPpuReadSideEffects())videoPolicy_|=2U;
#endif
}
#endif

/**
 * @brief Подключает заимствованный видеопакет и наблюдателя раздельного вывода.
 * @details Разрешён только при остановленном CPU-владельце и освобождённых пакетах; nullptr возвращает обычный путь.
 * @param storage Заимствованное хранилище выбранного протокола; nullptr обрабатывается контрактом метода.
 * @param observer Заимствованный наблюдатель опубликованных данных; не владелец CPU/ROM.
 */
void Machine::configureSplitVideo(VideoPacket* storage, VideoObserver* observer) noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_){return;}
#endif
    // Явный сброс только владельцем. Неудачная привязка не должна отнимать
    // или менять запечатанный либо принадлежащий consumer пакет.
    if(storage && storage != videoPacket_ && storage->state()!=VideoPacket::State::Free){
        videoFaultLatch_=VideoFault::BadBinding;return;
    }
    observeCpuReads();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    flushPpu();
#endif
    if(videoPacket_)videoPacket_->abandon();
#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
    videoObservableOnly_=false;
#endif
    videoPacket_=storage;videoObserver_=observer;videoFaultLatch_=VideoFault::None;
    reset();
}

#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
/** @details Проверяет внешние video-владельцы и режим перед привязкой; backing живёт до завершения всех пользователей. */
bool Machine::configureObservableVideo(bool enabled) noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_){return false;}
#endif
    // Обычный configure API тоже сбрасывает состояние. Узкий эксперимент запрещает
    // смену владельца внутри поколения, а не принимает устаревшее legacy-состояние.
    if(cpu_.inStep_ || cpuCycles()>7U)return false;
    const auto mapper=cartridge_.mapper();
    if(enabled && cartridge_.loaded() && mapper!=0 && mapper!=2 && mapper!=4
#if defined(NES_CORE_PPU_OBSERVABLE_LEGACY160) && NES_CORE_PPU_OBSERVABLE_LEGACY160
        && mapper!=69 && mapper!=85 && mapper!=160
#endif
#if defined(NES_CORE_PPU_OBSERVABLE_M1_M7) && NES_CORE_PPU_OBSERVABLE_M1_M7
        && mapper!=1 && mapper!=7
#endif
)return false;
    observeCpuReads();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    flushPpu();
#endif
    if(videoPacket_)videoPacket_->abandon();
    videoPacket_=nullptr;videoObserver_=nullptr;videoObservableOnly_=enabled;
    reset();
    return !enabled || splitVideoEnabled();
}
#endif

/**
 * @brief Сбрасывает выбранный видеотракт и обновляет допустимость mapper.
 * @details Отказ загрузки не должен неявно переносить старые ссылки на новый backing; политика владельца меняется только в разрешённом жизненном цикле.
 */
void Machine::videoReset() noexcept {
#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
    videoBgViewValid_=false;videoBgViewDirty_=true;
#endif
    timingPpu_.reset(region_==Region::Pal);
    const auto mapper=cartridge_.mapper();
#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
    // Успешные load/reset создают свежую границу. Неподдерживаемый режим использует
    // уже сброшенный полный Ppu; отказ загрузки не вызывает videoReset и не переключает владельца.
    if(videoObservableOnly_ && cartridge_.loaded() && mapper!=0 && mapper!=2 && mapper!=4
#if defined(NES_CORE_PPU_OBSERVABLE_LEGACY160) && NES_CORE_PPU_OBSERVABLE_LEGACY160
        && mapper!=69 && mapper!=85 && mapper!=160
#endif
#if defined(NES_CORE_PPU_OBSERVABLE_M1_M7) && NES_CORE_PPU_OBSERVABLE_M1_M7
        && mapper!=1 && mapper!=7
#endif
)
        videoObservableOnly_=false;
#endif
    videoCapable_=!cartridge_.loaded()||mapper==0||mapper==1||mapper==2||
        mapper==3||mapper==4||mapper==7||mapper==69||mapper==85||mapper==160
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
        ||(mapper==5&&outputOwner_)
#endif
        ;
#if defined(NES_CORE_VIDEO_POLICY_CACHE) && NES_CORE_VIDEO_POLICY_CACHE
    refreshVideoPolicy(); // Окончательный выбор владельца до тактов сброса и frontend.
#endif
    videoFaultLatch_=VideoFault::None;
    if(!videoPacket_){
        timingPpu_.bind(nullptr);
#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
        if(videoObservableOnly_)timingPpu_.setBusTracking(mapper==4,mapper==4
#if defined(NES_CORE_PPU_OBSERVABLE_LEGACY160) && NES_CORE_PPU_OBSERVABLE_LEGACY160
            || mapper==160
#endif
        );
#endif
        return;
    }
    videoPacket_->abandon();++videoGeneration_;videoSequence_=0;
    if(!videoPacket_->begin(videoGeneration_,0,cycles_,region_==Region::Pal,
            cartridge_.chr_,static_cast<std::uint32_t>(cartridge_.chrSize_))){
        videoFaultLatch_=VideoFault::BadBinding;return;
    }
    videoPacket_->cursor(cycles_,timingPpu_.frameNumber());
    videoPacket_->setSeed(cartridge_.chrRam_.data());
    timingPpu_.bind(videoPacket_);
    bool cycleObserver=mapper==4;
    bool addressObserver=mapper==4||mapper==160;
    // MMC5 получает настоящие /RD и M2 отдельно. Общие PPU-провода здесь
    // обслуживают MMC3 A12 и Legacy160, внутренний busAddress_ сохраняется.
    timingPpu_.setBusTracking(cycleObserver,addressObserver);
    if(!videoCapable_)videoPacket_->fault(VideoFault::UnsupportedMapper);
}

/**
 * @brief Возвращает текущую ошибку соответствующего тракта.
 * @details Первичная ошибка сохраняется до установленного reset/begin; отсутствие ошибки не означает завершения физического вывода.
 * @return Вычисленное значение или состояние типа VideoFault согласно описанному контракту.
 */
VideoFault Machine::videoFault() const noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_)return videoFaultLatch_!=VideoFault::None?videoFaultLatch_:
        (videoPacket_?videoPacket_->header().fault:VideoFault::None);
#endif
    if(videoFaultLatch_!=VideoFault::None)return videoFaultLatch_;
#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
    if(videoObservableOnly_)return VideoFault::None;
#endif
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
    return videoPacket_?videoPacket_->header().fault:(ppu_.pixelsReady()?VideoFault::None:VideoFault::NoMemory);
#else
    return videoPacket_?videoPacket_->header().fault:VideoFault::None;
#endif
}

/**
 * @brief Завершает активный видеопакет и подключает другой свободный слот.
 * @details Вызывающий обеспечивает исключительное владение и отдельное хранилище. Сначала доводятся реальные долги и пиксели, затем публикуется старый пакет.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param nextFree Отдельный действительно Free-видеослот, предоставляемый только после освобождения прежнего consumer.
 * @param retired Выходной указатель на прежний запечатанный видеослот; освобождается лишь после завершения consumer.
 */
bool Machine::swapVideoPacket(VideoPacket& nextFree,VideoPacket*& retired) noexcept {
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
    if(outputOwner_){retired=nullptr;return false;}
#endif
    retired=nullptr;
    if(!videoPacket_||!videoCapable_||videoHasFault()||&nextFree==videoPacket_||
       nextFree.state()!=VideoPacket::State::Free||videoPacket_->state()!=VideoPacket::State::Recording){
        videoFaultLatch_=VideoFault::BadBinding;return false;
    }
    observeCpuReads();
#if defined(NES_CORE_PPU_DEFER8) && NES_CORE_PPU_DEFER8
    flushPpu();
#endif
    timingPpu_.flushPixels();
    if(videoHasFault()||!videoPacket_->seal(cpuCycles(),timingPpu_.frameNumber()))return false;
    auto* old=videoPacket_;
    // Выше проверено Free; begin не отказывает без незаконного конкурентного
    // доступа владельца. Не копировать ёмкий массив и не менять потреблённый префикс.
    if(!nextFree.begin(videoGeneration_,++videoSequence_,cpuCycles(),region_==Region::Pal,
          cartridge_.chr_,static_cast<std::uint32_t>(cartridge_.chrSize_))){
        videoFaultLatch_=VideoFault::BadBinding;return false;
    }
    nextFree.cursor(cpuCycles(),timingPpu_.frameNumber());
    videoPacket_=&nextFree;timingPpu_.bind(videoPacket_);retired=old;
    return true;
}

/**
 * @brief Доводит активный видеопакет и передаёт его наблюдателю.
 * @details При отказе наблюдателя защёлкивает ошибку; вызывающий не получает частичную картинку как успешный результат.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Machine::observeVideo() const noexcept {
    if(!videoPacket_)return true;
    if(!videoCapable_||videoHasFault())return false;
    timingPpu_.flushPixels();
    videoPacket_->cursor(cpuCycles(),timingPpu_.frameNumber());
    if(!videoPacket_->readRecording())return false;
    if(!videoObserver_||!videoObserver_->observeVideo(*videoPacket_)){
        videoFaultLatch_=VideoFault::ObserverFailure;return false;
    }
    return true;
}

/**
 * @brief Определяет физическую либо буквальную ссылку для текущего адреса PPU.
 * @details При недопустимой идентичности защёлкивает ошибку; адрес и память не следует смешивать с текущим банком после mapper-строба.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
std::uint32_t Machine::videoReference(std::uint16_t address) const noexcept {
    if(cartridge_.mapper()==5){videoFaultLatch_=VideoFault::InvalidReference;return VideoByteRef::literal;}
    address&=0x3FFFU;
    if(address>=0x2000U)return VideoByteRef::nametable|nametableAddress(address);
    if(!cartridge_.loaded())return VideoByteRef::literal;
    const auto* ptr=cartridge_.chrPages_[(address>>10)&7U]+(address&1023U);
    const auto raw=reinterpret_cast<std::uintptr_t>(ptr);
    const auto ram=reinterpret_cast<std::uintptr_t>(cartridge_.chrRam_.data());
    if(raw>=ram&&raw-ram<cartridge_.chrRam_.size())return VideoByteRef::chrRam|static_cast<std::uint32_t>(raw-ram);
    const auto rom=reinterpret_cast<std::uintptr_t>(cartridge_.chr_);
    if(cartridge_.chr_&&raw>=rom&&raw-rom<cartridge_.chrSize_)return static_cast<std::uint32_t>(raw-rom);
    videoFaultLatch_=VideoFault::InvalidReference;return VideoByteRef::literal;
}

#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
/**
 * @brief Проверяет, допустима ли исходная композиция фонового интервала.
 * @details Учитывает mapper, часы, адресные сигналы, длину, таблицу паттернов и опасные маркеры; отказ не изменяет состояние.
 * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
 * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Machine::canVideoBgSpan(bool clocked,bool addressed,[[maybe_unused]] unsigned tiles,[[maybe_unused]] unsigned control,[[maybe_unused]] std::uint16_t v) const noexcept {
    if(!cartridge_.loaded())return false;
    [[maybe_unused]] constexpr unsigned minimumSpanTiles=3;
#if defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
    if(cartridge_.mapper()==160)return !clocked&&addressed&&tiles>=minimumSpanTiles&&!(control&0x10U)&&
        cartridge_.legacy_.canLowBackgroundSpan()&&videoNameMarkers_.clearForSpan(v,tiles,nametablePages_);
#endif
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    if(clocked&&addressed&&cartridge_.mapper()==4&&tiles>=minimumSpanTiles&&tiles<=32)
        return cartridge_.ppuIrqQuietFor(tiles*8U,(control&0x10U)!=0);
#endif
    if(clocked||addressed)return false;
    switch(cartridge_.mapper()){
    // Только чистые PPU-чтения: CPU IRQ и selected64 audio horizon ограничены отдельно.
    case 0:case 1:case 2:case 3:case 7:case 69:case 85:return true;
    default:return false;
    }
}
/**
 * @brief Применяет доказанную композицию тактов и A12 фонового MMC3-span.
 * @details Данные RGB не формируются, но счётчик, reload, pending и возраст A12 остаются теми же.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
 */
void Machine::completeVideoBgSpan([[maybe_unused]] unsigned tiles,[[maybe_unused]] unsigned control) noexcept {
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    // Вызывающий допустил полный тихий интервал mapper4. Сохранить каждый прежний
    // восьмиточечный переход, включая pending/counter/reload и возраст A12.
#if defined(NES_CORE_MMC3_BG_SPAN_O1) && NES_CORE_MMC3_BG_SPAN_O1
    cartridge_.advanceMmc3BgSpan(tiles,(control&0x10U)!=0);
#else
    for(unsigned i=0;i<tiles;++i)cartridge_.advanceMmc3TileSignals((control&0x10U)!=0);
#endif
#endif
}
/**
 * @brief Готовит метаданные банков только если они нужны потребителю видеопакета.
 * @details Observable-владелец не требует записи mapping-view; остальные пути сохраняют проверки физических CHR-ссылок и ёмкости.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Machine::prepareVideoBgView() noexcept {
    if(!cartridge_.loaded())return false;
#if defined(NES_CORE_PPU_OBSERVABLE_ONLY) && NES_CORE_PPU_OBSERVABLE_ONLY
    // Consumer не требует записи отображения. Вызывающий допускает только прежние
    // полные проверки S0/mapper/wires и фиксирует то же хвостовое состояние.
    if(videoObservableOnly_)return true;
#endif
    if(!videoPacket_)return false;
    // Sprite/scalar/CPU FD/FE-чтения могут заменить страницы без CPU-записи.
    // Cartridge помечает все refresh, поэтому повторный разбор восьми ссылок
    // нужен лишь после инвалидации. Стробы и сами отображения уже применены.
    if(videoBgViewValid_&&!videoBgViewDirty_
#if defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
       &&!cartridge_.chrMappingDirty_
#endif
       )return true;
    const auto names=nametablePages_;
    std::array<std::uint32_t,8> chr{};
    for(unsigned i=0;i<8;++i){
        const auto ref=cachedVideoReference(cartridge_.chrPages_[i]);
        const auto offset=ref&VideoByteRef::offsetMask,kind=ref&VideoByteRef::kindMask;
        const auto size=kind==VideoByteRef::chrRam?cartridge_.chrRam_.size():cartridge_.chrSize_;
        if((kind!=VideoByteRef::rom&&kind!=VideoByteRef::chrRam)||(offset&1023U)||size<1024U||offset>size-1024U)return false;
        chr[i]=ref;
    }
    if(!videoBgViewValid_||names!=videoBgNames_||chr!=videoBgChr_){
        if(!videoPacket_->addBgView(names,chr))return false;
        videoBgNames_=names;videoBgChr_=chr;videoBgViewValid_=true;
    }
#if defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
    // При отказе ссылки либо addBgView этот acknowledge недостижим.
    cartridge_.chrMappingDirty_=false;
#endif
    videoBgViewDirty_=false;return true;
}
/** @details Записывает только уже завершённый BG-префикс в ограниченный video packet. */
Machine::VideoBgTail Machine::captureVideoBgTail(std::uint16_t v,unsigned tiles,unsigned control,const TimingPpu* caller) const noexcept {
    // N>=3 перезаписывает все ШЕСТЬ входящих идентичностей байтов. Только три последних
    // выбранных тайла остаются в high/low/next; внутренний байт нельзя наблюдать.
    const unsigned x=(v&31U)+tiles-3U;
    v=static_cast<std::uint16_t>((v&~0x041FU)|(x&31U)|((v^((x>>5)<<10))&0x0400U));
    VideoBgTail tail;
    for(unsigned i=0;i<3;++i){
        const unsigned nt=0x2000U|(v&0x0FFFU);
        const auto tile=nametables_[nametablePages_[(nt>>10)&3U]|(nt&1023U)];
        const unsigned pattern=((control&0x10U)?0x1000U:0U)+unsigned(tile)*16U+((v>>12)&7U);
        const auto* low=cartridge_.chrPages_[(pattern>>10)&7U]+(pattern&1023U);
        tail.low[i]=cachedVideoReference(low);
        if(i==2){
            const unsigned at=0x23C0U|(v&0x0C00U)|((v>>4)&0x38U)|((v>>2)&7U);
            const auto raw=elideVideoCaptureData(caller)?std::uint8_t{0}:nametables_[nametablePages_[(at>>10)&3U]|(at&1023U)];
            tail.attribute=static_cast<std::uint8_t>((raw>>(((v>>4)&4U)|(v&2U)))&3U);
            tail.tile=tile;tail.address=static_cast<std::uint16_t>(pattern+8U);
        }
        if((v&31U)==31U)v=static_cast<std::uint16_t>((v&~0x001FU)^0x0400U);else ++v;
    }
    tail.v=v;return tail;
}
#if defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
/**
 * @brief Завершает разрешённый низкий Legacy-интервал с его конечным адресом.
 * @details Отказ не должен менять mapper; последующие FD/FE-запросы и CPU-наблюдаемые эффекты сохраняют прежний путь.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 * @param lastHighAddress Реально удержанный конечный high-адрес интервала.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Machine::completeLegacyVideoBgSpan(unsigned tiles,std::uint16_t lastHighAddress) noexcept {
    if(!cartridge_.loaded())return false;
    if(cartridge_.mapper()!=160)return true;
    return cartridge_.legacy_.completeLowBackgroundSpan(tiles,lastHighAddress);
}
#endif
#endif

#if defined(NES_CORE_PPU_SPARSE_OPACITY) && NES_CORE_PPU_SPARSE_OPACITY
/**
 * @brief Сохраняет ссылку либо немедленно замораживает нетипичное чтение паттерна.
 * @details Даже отложенное значение сохраняет реальный read-strobe. Сохранённый NT/палитровый адрес при смене rendering нельзя ошибочно считать CHR.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
std::uint32_t Machine::captureVideoPattern(std::uint16_t address,std::uint32_t reference) noexcept {
    if(cartridge_.loaded()&&address<0x2000U&&
        (reference&VideoByteRef::kindMask)!=VideoByteRef::nametable&&
        (reference&VideoByteRef::kindMask)!=VideoByteRef::literal){
        // Адресные и тактовые линии выставлены в исходной фазе. Строб
        // чтения остаётся настоящим и при ленивом неиспользуемом значении.
        cartridge_.ppuReadStrobe();return reference;
    }
    // Включение рендера посреди тайла может сохранить nametable/palette-адрес
    // на фазе выборки паттерна. Его прежнее eager-значение фиксируется СЕЙЧАС;
    // это не CHR-идентичность, CHR-RAM COW-контракт к нему неприменим.
    return VideoByteRef::literal|ppuRead(address);
}
/**
 * @brief Материализует ранее сохранённый байт паттерна по его виду и смещению.
 * @details Работает с буквальным значением, CHR-ROM либо защищённой CHR-RAM-идентичностью; недопустимая ссылка является ошибкой.
 * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::resolveVideoPattern(std::uint32_t reference) const noexcept {
    const auto offset=reference&VideoByteRef::offsetMask;
    switch(reference&VideoByteRef::kindMask){
    case VideoByteRef::literal:return static_cast<std::uint8_t>(offset);
    case VideoByteRef::chrRam:
        if(offset<cartridge_.chrRam_.size())return cartridge_.chrRam_[offset];
        break;
    case VideoByteRef::rom:
        if(cartridge_.chr_&&offset<cartridge_.chrSize_)return cartridge_.chr_[offset];
        break;
    default:break;
    }
    videoFaultLatch_=VideoFault::InvalidReference;return 0;
}
/**
 * @brief Замораживает живые ссылки перед изменением CHR-RAM.
 * @details Старый байт сохраняется до записи, чтобы отложенный конвейер не увидел будущее значение.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::freezeVideoChrWrite(std::uint16_t address,std::uint8_t value) noexcept {
    if(cartridge_.chr_)return; // Запись ROM не меняет его хранилище.
    unsigned offset;
    if(cartridge_.mapper_==160||!cartridge_.prg_){
        if(cartridge_.mapper_==160&&!cartridge_.legacy_.chrWriteEnabled())return;
        offset=static_cast<unsigned>(cartridge_.chrOffset(address));
    }else{
        const auto* byte=cartridge_.chrRamPages_[(address>>10)&7U]+(address&1023U);
        offset=static_cast<unsigned>(byte-cartridge_.chrRam_.data());
    }
    if(offset>=cartridge_.chrRam_.size()){videoFaultLatch_=VideoFault::InvalidReference;return;}
    const auto old=cartridge_.chrRam_[offset];
    if(old!=value)timingPpu_.beforeChrRamWrite(offset,old);
}
#endif
} // пространство имён nes::core
#if defined(NES_CORE_OUTPUT_FRAME_LEASE) && NES_CORE_OUTPUT_FRAME_LEASE
#include "MachineOutput.inl"
#endif
#endif
