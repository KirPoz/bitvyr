/**
 * @file Ppu.cpp
 * @brief Полный растровый PPU с упорядоченными выборками, состоянием пикселей и RGB-hook.
 * @ingroup nes_ppu
 */
#include "Ppu.h"
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
#include "PpuRgbOutput.h"
#endif
#include <cstring>
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
#include <new>
#endif

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
#include "Machine.h"
#endif

namespace nes::core {
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
/**
 * @brief Создаёт либо копирует владеющее индексное хранилище полного PPU.
 * @details При heap-политике выделение может не удаться; копирование сохраняет политику и данные, а отсутствие pixels не должно останавливать время или Sprite0.
 * @param allocator Политика allocate/free для индексных пикселей; callback-ресурсы должны оставаться действительными.
 */
Ppu::PixelStorage::PixelStorage(LegacyPixelAllocator allocator) noexcept
    : bytes_(nullptr,Deleter{allocator.release}),allocate_(allocator.allocate) {
    if(bool(allocator.allocate)!=bool(allocator.release))return;
    if(allocator.allocate)bytes_.reset(allocator.allocate(width*height));
    else bytes_.reset(new(std::nothrow) std::uint8_t[width*height]);
}
/**
 * @brief Создаёт либо копирует владеющее индексное хранилище полного PPU.
 * @details При heap-политике выделение может не удаться; копирование сохраняет политику и данные, а отсутствие pixels не должно останавливать время или Sprite0.
 * @param other Источник глубокого копирования индексных пикселей; политика allocator сохраняется, чужое выделение не усваивается.
 */
Ppu::PixelStorage::PixelStorage(const PixelStorage& other) noexcept
    :bytes_(nullptr,Deleter{other.bytes_.get_deleter().release}),allocate_(other.allocate_) {
    if(!other.bytes_)return;
    if(allocate_)bytes_.reset(allocate_(width*height));
    else bytes_.reset(new(std::nothrow) std::uint8_t[width*height]);
    if(bytes_)std::copy_n(other.bytes_.get(),width*height,bytes_.get());
}
#endif

namespace {
/**
 * @brief Формирует неизменяемые таблицы упакованных плоскостей или выбора пикселей.
 * @details Порядок битов соответствует исходному селектору; таблица не является изменяемой палитрой PPU.
 * @return Вычисленное значение или состояние типа auto согласно описанному контракту.
 */
constexpr auto pixelPlanes() noexcept {
    std::array<std::uint32_t, 256> result{};
    for (unsigned value = 0; value < 256; ++value)
        for (unsigned x = 0; x < 8; ++x)
            if (value & (0x80U >> x)) result[value] |= 1U << (x * 4);
    return result;
}
constexpr auto expandedPixelPlane = pixelPlanes();
#if defined(NES_CORE_LEGACY_PPU_ATTR_CARRIER) && NES_CORE_LEGACY_PPU_ATTR_CARRIER
/**
 * @brief Строит таблицу переноса атрибутных битов для целого Legacy-тайла.
 * @details Первый тайл произвольной истории остаётся исходному пути; после него carrier точно восстанавливает конечные шифтеры.
 * @return Вычисленное значение или состояние типа auto согласно описанному контракту.
 */
constexpr auto makeLegacyAttributeCarriers() noexcept {
    std::array<std::array<std::uint32_t, 16>, 8> result{};
    for (unsigned fine = 0; fine < 8; ++fine) {
        const std::uint32_t prefix = 0xFFFFFFFFU >> (fine * 4U);
        for (unsigned current = 0; current < 4; ++current)
            for (unsigned next = 0; next < 4; ++next)
                result[fine][(current << 2U) | next] =
                    ((current * 0x44444444U) & prefix) |
                    ((next * 0x44444444U) & ~prefix);
    }
    return result;
}
constexpr auto legacyAttributeCarriers = makeLegacyAttributeCarriers();
#endif
/**
 * @brief Формирует неизменяемые таблицы упакованных плоскостей или выбора пикселей.
 * @details Порядок битов соответствует исходному селектору; таблица не является изменяемой палитрой PPU.
 * @return Вычисленное значение или состояние типа auto согласно описанному контракту.
 */
constexpr auto pixelSelections() noexcept {
    std::array<std::uint8_t, 1024> result{};
    for (unsigned sprite = 0; sprite < 64; ++sprite) {
        for (unsigned background = 0; background < 16; ++background) {
            const unsigned bg = background & 3U;
            const unsigned sp = sprite & 3U;
            unsigned index = 0;
            if (sp && (!bg || !(sprite & 0x10U))) index = 0x10U + (sprite & 15U);
            else if (bg) index = background;
            const unsigned hit = bg && sp && (sprite & 0x20U) ? 0x20U : 0U;
            result[(sprite << 4) | background] = static_cast<std::uint8_t>(index | hit);
        }
    }
    return result;
}
constexpr auto pixelSelection = pixelSelections();
}


/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 */
void Ppu::reset(bool pal) noexcept {
    backgroundPairsDirty_ = true;
    pendingPixels_ = 0;
    pal_ = pal;
    pixels_.fill(0);
    lineEmphasis_.fill(0);
    palette_.fill(0);
    oam_.fill(0xFF);
#if defined(NES_CORE_LEGACY_PPU_OAM_Y_INDEX) && NES_CORE_LEGACY_PPU_OAM_Y_INDEX
    for(auto& bucket:oamYBuckets_)bucket.fill(0);
    oamYBuckets_[255].fill(0xFFFFFFFFU);
    oamYLow_=oamYHigh_=0;oamYRow_=-1;oamYHeight_=0;oamYDirty_=true;
#endif
    secondary_.fill(0xFF);
    secondaryIndex_.fill(0xFF);
    sprites_.fill(Sprite{});
    spriteRow_.fill(0);
    spriteCacheClocks_ = 0;
    spriteCacheValid_ = false;
    control_ = mask_ = status_ = oamAddress_ = openBus_ = readBuffer_ = fineX_ = 0;
    v_ = t_ = busAddress_ = 0;
    writeToggle_ = false;
    renderingEnabled_ = renderingNext_ = false;
    renderDelay_ = 0;
    backgroundLow_ = backgroundHigh_ = attributeLow_ = attributeHigh_ = 0;
    pendingBackgroundShifts_ = 0;
    nextTile_ = nextAttribute_ = nextLow_ = nextHigh_ = 0;
    evaluateN_ = evaluateM_ = secondaryPosition_ = overflowFetches_ = 0;
    evaluateLatch_ = 0xFF;
    evaluationDone_ = false;
    evaluationNextDot_ = 257;
    scanline_ = -1;
    dot_ = 0;
    frames_ = 0;
    coarseXSignal_ = false;
    scrollSignal_ = false;
    odd_ = frameReady_ = nmiLine_ = nmiPending_ = suppressVblank_ = false;
    nmiDelay_ = 0;
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    if(auto* rgb=PpuRgbOutput::activeFor(this))rgb->resetFor(*this);
#endif
}

/**
 * @brief Публикует текущий адрес на шине PPU.
 * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Ppu::address(std::uint16_t value) noexcept {
    busAddress_ = value & 0x3FFF;
    if (trackBusAddresses_) bus_.ppuAddress(busAddress_);
}

/**
 * @brief Вычисляет физический индекс с правилами зеркалирования PPU.
 * @details Учитывает таблицы имён либо специальные зеркала палитры; сам адрес не выдаёт строб чтения.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Ppu::paletteAddress(std::uint16_t value) noexcept {
    unsigned result = value & 0x1F;
    if ((result & 0x13) == 0x10) result &= 0x0F;
    return result;
}

/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Ppu::read(std::uint16_t value) noexcept {
    address(value);
    if (busAddress_ >= 0x3F00) return palette_[paletteAddress(busAddress_)];
    return bus_.ppuRead(busAddress_);
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param location Нормализованный физический индекс CIRAM с учётом nametable-алиасов, а не логический CPU-адрес.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Ppu::write(std::uint16_t location, std::uint8_t value) noexcept {
    address(location);
    if (busAddress_ >= 0x3F00) {
        const auto index = paletteAddress(busAddress_);
        const auto colour = static_cast<std::uint8_t>(value & 0x3F);
        if (palette_[index] != colour) backgroundPairsDirty_ = true;
        palette_[index] = colour;
    }
    else bus_.ppuWrite(busAddress_, value);
}

/**
 * @brief Продвигает coarse X и переключает горизонтальную таблицу имён при wrap.
 * @details Остальные поля loopy v сохраняются.
 */
void Ppu::incrementX() noexcept {
    if ((v_ & 0x001F) == 31) v_ = (v_ & 0x7FE0) ^ 0x0400;
    else ++v_;
}

/**
 * @brief Продвигает fine/coarse Y по правилам видимых и специальных строк PPU.
 * @details Строки 29/31 и переключение вертикальной таблицы имён не заменяются обычным сложением адреса.
 */
void Ppu::incrementY() noexcept {
    if ((v_ & 0x7000) != 0x7000) v_ += 0x1000;
    else {
        v_ &= 0x0FFF;
        unsigned y = (v_ >> 5) & 31;
        if (y == 29) { y = 0; v_ ^= 0x0800; }
        else if (y == 31) y = 0;
        else ++y;
        v_ = static_cast<std::uint16_t>((v_ & 0x7C1F) | (y << 5));
    }
}

/**
 * @brief Продвигает v после обращения к PPUDATA по текущему режиму.
 * @details Правила rendering и обычного CPU-инкремента различаются; адресные эффекты сохраняются.
 */
void Ppu::incrementDataAddress() noexcept {
    if (rendering() && renderLine()) { incrementX(); incrementY(); }
    else v_ = (v_ + ((control_ & 4) ? 32 : 1)) & 0x7FFF;
    if (!(rendering() && renderLine())) address(v_);
}

/**
 * @brief Обновляет линию/задержку NMI из VBlank и PPUCTRL.
 * @details Сохраняет подавление и delayed-переходы около реального STATUS-чтения.
 */
void Ppu::updateNmi() noexcept {
    const bool line = (control_ & 0x80) && (status_ & 0x80);
    if (line && !nmiLine_) nmiDelay_ = 2;
    if (!line) nmiDelay_ = 0;
    nmiLine_ = line;
}

/**
 * @brief Забирает подготовленное событие NMI, предназначенное CPU.
 * @details Возврат события отличается от простого просмотра уровня линии и должен соблюсти исходный порядок синхронизации.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Ppu::takeNmi() noexcept {
    const bool pending = nmiPending_;
    nmiPending_ = false;
    return pending;
}

/**
 * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
 * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Ppu::cpuRead(std::uint16_t reg) noexcept {
    flushPixels();
    if ((reg & 7) == 2 || (reg & 7) == 4) synchronizeSprites();
    switch (reg & 7) {
    case 2:
        openBus_ = (status_ & 0xE0) | (openBus_ & 0x1F);
        if (scanline_ == 241 && dot_ == 1) suppressVblank_ = true;
        status_ &= 0x7F;
        writeToggle_ = false;
        if (scanline_ == 241 && dot_ <= 3) nmiPending_ = false;
        updateNmi();
        break;
    case 4:
        if (rendering() && renderLine() && dot_ >= 1 && dot_ <= 320) {
            if (dot_ <= 64) openBus_ = 0xFF;
            else if (dot_ <= 256) openBus_ = evaluateLatch_;
            else {
                const unsigned slot = (dot_ - 257) / 8;
                const unsigned field = ((dot_ - 257) % 8) < 4 ? (dot_ - 257) % 8 : 3;
                openBus_ = secondary_[slot * 4 + field];
            }
        } else openBus_ = oam_[oamAddress_];
        break;
    case 7: {
        const std::uint16_t location = v_ & 0x3FFF;
        if (location >= 0x3F00) {
            openBus_ = (read(location) & ((mask_ & 1) ? 0x30 : 0x3F)) | (openBus_ & 0xC0);
            readBuffer_ = read(location - 0x1000);
        } else { openBus_ = readBuffer_; readBuffer_ = read(location); }
        incrementDataAddress();
        break;
    }
    default: break;
    }
    return openBus_;
}

/**
 * @brief Записывает очередной байт OAM DMA в текущий OAM-адрес.
 * @details Продвигает адрес и инвалидирует связанные sprite-кэши в исходном порядке; не заменяет источник DMA.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Ppu::dmaWrite(std::uint8_t value) noexcept {
    flushPixels();
    synchronizeSprites();
    if ((oamAddress_ & 3) == 2) value &= 0xE3;
#if defined(NES_CORE_LEGACY_PPU_OAM_Y_INDEX) && NES_CORE_LEGACY_PPU_OAM_Y_INDEX
    // Прежняя неполная оценка/защёлка синхронизирована до этой настоящей записи.
    if(!(oamAddress_&3U)&&oam_[oamAddress_]!=value){
        const unsigned slot=oamAddress_>>2,word=slot>>5;
        const auto bit=std::uint32_t(1)<<(slot&31U);
        oamYBuckets_[oam_[oamAddress_]][word]&=~bit;
        oamYBuckets_[value][word]|=bit;oamYDirty_=true;
    }
#endif
    oam_[oamAddress_++] = value;
}

/**
 * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
 * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Ppu::cpuWrite(std::uint16_t reg, std::uint8_t value) noexcept {
    flushPixels();
    if ((reg & 7) == 0) synchronizeSprites();
    openBus_ = value;
    switch (reg & 7) {
    case 0:
        control_ = value;
        t_ = (t_ & 0x73FF) | ((value & 3) << 10);
        updateNmi();
        break;
    case 1:
        if ((mask_ ^ value) & 1U) backgroundPairsDirty_ = true;
        mask_ = value;
        // Тактирующее разрешение рендера следует за PPUMASK через несколько PPU-точек.
        // Поздняя запись, в частности, не должна сдвигать границу пропуска нечётного кадра.
        renderingNext_ = (value & 0x18) != 0;
        renderDelay_ = 2;
        break;
    case 3: oamAddress_ = value; break;
    case 4:
        if (rendering() && renderLine()) oamAddress_ += 4;
        else dmaWrite(value);
        break;
    case 5:
        if (!writeToggle_) { fineX_ = value & 7; t_ = (t_ & 0x7FE0) | (value >> 3); }
        else t_ = (t_ & 0x0C1F) | ((value & 7) << 12) | ((value & 0xF8) << 2);
        writeToggle_ = !writeToggle_;
        break;
    case 6:
        if (!writeToggle_) t_ = (t_ & 0x00FF) | ((value & 0x3F) << 8);
        else { t_ = (t_ & 0x7F00) | value; v_ = t_; address(v_); }
        writeToggle_ = !writeToggle_;
        break;
    case 7: write(v_, value); incrementDataAddress(); break;
    default: break;
    }
}

/**
 * @brief Вставляет следующие байты/идентичности фона в текущие сдвиговые регистры.
 * @details Частичные сдвиги и независимые high/low-ссылки не заменяются предположением о полном восьмибитном сдвиге.
 */
void Ppu::loadBackground() noexcept {
    flushPixels();
    // Материализовать отложенные сдвиги лишь при вставке следующего тайла. Чтение
    // выхода сдвигателя выбирает соответственно более низкий бит в renderPixel().
    backgroundLow_ = ((static_cast<std::uint32_t>(backgroundLow_) << pendingBackgroundShifts_) & 0xFF00) | nextLow_;
    backgroundHigh_ = ((static_cast<std::uint32_t>(backgroundHigh_) << pendingBackgroundShifts_) & 0xFF00) | nextHigh_;
    attributeLow_ = ((static_cast<std::uint32_t>(attributeLow_) << pendingBackgroundShifts_) & 0xFF00) | ((nextAttribute_ & 1) ? 0xFF : 0);
    attributeHigh_ = ((static_cast<std::uint32_t>(attributeHigh_) << pendingBackgroundShifts_) & 0xFF00) | ((nextAttribute_ & 2) ? 0xFF : 0);
    pendingBackgroundShifts_ = 0;
}

template<unsigned Phase>
#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief Обслуживает точную фазу фонового конвейера PPU.
 * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
 */
[[gnu::always_inline]]
#endif
/** @details Исполняет одну фазу BG-fetch с исходными адресными/read-фронтами и загрузкой shifters. */
inline void Ppu::fetchBackgroundPhase() noexcept {
    static_assert(Phase < 8, "background fetch phase must fit one tile");
    if constexpr (Phase == 0) address(0x2000 | (v_ & 0x0FFF));
    else if constexpr (Phase == 1) nextTile_ = bus_.ppuReadFetch(busAddress_, PpuFetchKind::BackgroundName, backgroundFetchTileX());
    else if constexpr (Phase == 2) address(0x23C0 | (v_ & 0x0C00) | ((v_ >> 4) & 0x38) | ((v_ >> 2) & 7));
    else if constexpr (Phase == 3) {
        nextAttribute_ = (bus_.ppuReadFetch(busAddress_, PpuFetchKind::BackgroundAttribute, backgroundFetchTileX()) >> (((v_ >> 4) & 4) | (v_ & 2))) & 3;
        // Выставить pattern-адрес после attribute-строба, сохранив
        // прежний ранний A12-фронт и следующий повторный адрес.
        address(((control_ & 0x10) ? 0x1000 : 0) + nextTile_ * 16 + ((v_ >> 12) & 7));
    } else if constexpr (Phase == 4) address(((control_ & 0x10) ? 0x1000 : 0) + nextTile_ * 16 + ((v_ >> 12) & 7));
    else if constexpr (Phase == 5) nextLow_ = bus_.ppuReadFetch(busAddress_, PpuFetchKind::BackgroundPattern, backgroundFetchTileX());
    else if constexpr (Phase == 6) { coarseXSignal_ = true; address(busAddress_ + 8); }
    else if constexpr (Phase == 7) { nextHigh_ = bus_.ppuReadFetch(busAddress_, PpuFetchKind::BackgroundPattern, backgroundFetchTileX()); if (coarseXSignal_) incrementX(); coarseXSignal_ = false; }
}

/**
 * @brief Выполняет предусмотренную текущей точкой загрузку фонового конвейера.
 * @details Не считает текущий адрес автоматически CHR: после переходов rendering может сохраняться иной шинный адрес.
 */
void Ppu::fetchBackground() noexcept {
    switch ((dot_ - 1) & 7) {
    case 0: fetchBackgroundPhase<0>(); break;
    case 1: fetchBackgroundPhase<1>(); break;
    case 2: fetchBackgroundPhase<2>(); break;
    case 3: fetchBackgroundPhase<3>(); break;
    case 4: fetchBackgroundPhase<4>(); break;
    case 5: fetchBackgroundPhase<5>(); break;
    case 6: fetchBackgroundPhase<6>(); break;
    case 7: fetchBackgroundPhase<7>(); break;
    }
}

/**
 * @brief Доводит текущие sprite-плоскости и счётчики X до нужной позиции.
 * @details Накопленные сдвиги и различающиеся моменты low/high-fetch учитываются до использования метаданных.
 */
void Ppu::synchronizeSprites() noexcept {
    if (dot_) synchronizeSpriteEvaluation(dot_ - 1);
}

#if defined(NES_CORE_LEGACY_PPU_OAM_Y_INDEX) && NES_CORE_LEGACY_PPU_OAM_Y_INDEX
namespace {
// Ненулевой 32-битный вход; ограниченная иерархия избегает RV32 ROM CTZ/64-битных helper.
/**
 * @brief Находит первый установленный бит непустой маски OAM-кандидатов.
 * @details Нулевую маску вызывающий обязан обработать до поиска; порядок соответствует исходному обходу OAM.
 * @param word Ненулевое 32-битное множество OAM-кандидатов для поиска первого установленного бита.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
inline unsigned firstLegacyOamYBit(std::uint32_t word) noexcept {
    unsigned bit=0;
    if(!(word&0xFFFFU)){word>>=16;bit+=16;}
    if(!(word&0xFFU)){word>>=8;bit+=8;}
    if(!(word&0xFU)){word>>=4;bit+=4;}
    if(!(word&3U)){word>>=2;bit+=2;}
    return bit+unsigned(!(word&1U));
}
}
/**
 * @brief Находит допустимую серию заведомо неподходящих OAM Y-пар.
 * @details Не перескакивает особые n=63/переполнение/частичную защёлку; последний прочитанный байт и secondary-позиция остаются точными.
 * @param n Начальный OAM-индекс 0..62; индекс 63 и оборачивание остаются скалярными.
 * @param limit Максимум завершённых Y-пар, доступных текущему вызову; последний/wrap-спрайт остаётся скалярным.
 * @param height Высота спрайта 8 или 16 PPU-строк, выбранная из текущего PPUCTRL.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned Ppu::invalidYRun(unsigned n,unsigned limit,unsigned height) noexcept {
    if(n>=63||!limit)return 0;
    if(oamYDirty_||oamYRow_!=scanline_||oamYHeight_!=height){
        std::uint32_t low=0,high=0;
        // Оценка использует знаковое row-Y; верхние строки не переходят к Y255.
        const unsigned last=static_cast<unsigned>(scanline_);
        const unsigned first=last+1U<std::uint32_t(height)?0U:last+1U-height;
        for(unsigned y=first;y<=last;++y){low|=oamYBuckets_[y][0];high|=oamYBuckets_[y][1];}
        oamYLow_=low;oamYHigh_=high;oamYRow_=scanline_;
        oamYHeight_=static_cast<std::uint8_t>(height);oamYDirty_=false;
    }
    unsigned valid=63;
    const auto high=oamYHigh_&0x7FFFFFFFU; // Последний/wrap-спрайт остаётся скалярным.
    if(n<32){
        const auto word=oamYLow_&(0xFFFFFFFFU<<n);
        if(word)valid=firstLegacyOamYBit(word);
        else if(high)valid=32U+firstLegacyOamYBit(high);
    }else{
        const auto word=high&(0xFFFFFFFFU<<(n-32U));
        if(word)valid=32U+firstLegacyOamYBit(word);
    }
    return std::min(limit,valid-n);
}
#endif

/**
 * @brief Доводит аппаратный обход OAM до текущей растровой точки.
 * @details Сохраняет odd/even-защёлку, очистку secondary OAM, byte-позиции и диагональное переполнение, включая частичные срезы.
 * @param until Последняя включённая PPU-точка evaluation; ограничивается 256, частичная odd/even-защёлка сохраняется.
 */
void Ppu::synchronizeSpriteEvaluation(unsigned until) noexcept {
    if (scanline_ < 0 || scanline_ >= 240) return;
    if (until > 256) until = 256;
    unsigned next = evaluationNextDot_;
    if (next > until) return;
    unsigned n = evaluateN_, m = evaluateM_, position = secondaryPosition_;
    unsigned overflow = overflowFetches_;
    std::uint8_t latch = evaluateLatch_;
    bool done = evaluationDone_;
    bool overflowHit = false;
    if (next == 1) {
        n = m = position = overflow = 0;
        done = false;
        secondaryIndex_.fill(0xFF);
    }
    if (next <= 64) {
        const unsigned lastClear = until < 64 ? until : 64;
        for (unsigned even = next + (next & 1); even <= lastClear; even += 2)
            secondary_[even / 2 - 1] = 0xFF;
        next = lastClear + 1;
    }
    const int spriteHeight = (control_ & 0x20) ? 16 : 8;
    while (next <= until) {
#if defined(NES_CORE_LEGACY_PPU_OAM_Y_INDEX) && NES_CORE_LEGACY_PPU_OAM_Y_INDEX
        // Склеиваются только завершённые обычные Y-пары read/evaluate. Чётная
        // следующая точка сохраняет прежнюю sampled-защёлку, в том числе перед DMA.
        if(next>=65&&(next&1U)&&until-next>=1U&&m==0&&!overflow&&!done&&
           position<=28&&!(position&3U)&&n<63){
            const unsigned limit=std::min((until-next+1U)/2U,63U-n);
            const unsigned skip=invalidYRun(n,limit,spriteHeight);
            if(skip){
                latch=oam_[(n+skip-1U)*4U];secondary_[position]=latch;
                n+=skip;next+=2U*skip;continue;
            }
        }
#endif
        if (done) {
            latch = oam_[n * 4];
            next = until + 1;
            break;
        }
        if (next & 1) {
            latch = oam_[n * 4 + m];
            if (++next > until) break;
        }
        if (position < 32) {
            if (m == 0) {
                secondary_[position] = latch;
                const int row = scanline_ - latch;
                if (row >= 0 && row < spriteHeight) {
                    secondaryIndex_[position / 4] = static_cast<std::uint8_t>(n);
                    ++position;
                    m = 1;
                } else ++n;
            } else {
                secondary_[position++] = latch;
                if (++m == 4) { m = 0; ++n; }
            }
        } else if (overflow) {
            m = (m + 1) & 3;
            if (!m) ++n;
            if (!--overflow) done = true;
        } else {
            const int row = scanline_ - latch;
            if (row >= 0 && row < spriteHeight) {
                overflowHit = true;
                m = (m + 1) & 3;
                if (!m) ++n;
                overflow = 3;
            } else {
                ++n;
                m = (m + 1) & 3;
            }
        }
        if (n >= 64) { n = 0; done = true; }
        ++next;
    }
    evaluateN_ = static_cast<std::uint8_t>(n);
    evaluateM_ = static_cast<std::uint8_t>(m);
    secondaryPosition_ = static_cast<std::uint8_t>(position);
    overflowFetches_ = static_cast<std::uint8_t>(overflow);
    evaluateLatch_ = latch;
    evaluationDone_ = done;
    evaluationNextDot_ = static_cast<std::uint16_t>(next);
    if (overflowHit) status_ |= 0x20;
}

/**
 * @brief Разворачивает порядок битов для горизонтального отражения либо индексирования таблицы.
 * @details Результат применяется к той плоскости и тому моменту fetch, для которых он был рассчитан.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Ppu::reverse(std::uint8_t value) noexcept {
    value = ((value & 0x55) << 1) | ((value >> 1) & 0x55);
    value = ((value & 0x33) << 2) | ((value >> 2) & 0x33);
    return static_cast<std::uint8_t>((value << 4) | (value >> 4));
}

/**
 * @brief Вычисляет адрес плоскости спрайта по текущей высоте, строке, тайлу и flip.
 * @details В 8x16 режиме младший бит tile выбирает таблицу; это влияет и на A12.
 * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
 * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
 */
std::uint16_t Ppu::spritePattern(unsigned slot) const noexcept {
    unsigned row = static_cast<std::uint8_t>(scanline_ - secondary_[slot * 4]);
    const unsigned heightPixels = (control_ & 0x20) ? 16 : 8;
    row &= heightPixels - 1;
    if (secondary_[slot * 4 + 2] & 0x80) row ^= heightPixels - 1;
    const unsigned tile = secondary_[slot * 4 + 1];
    if (heightPixels == 16) return static_cast<std::uint16_t>((tile & 1) * 0x1000 + (tile & 0xFE) * 16 + (row & 7) + ((row & 8) ? 16 : 0));
    return static_cast<std::uint16_t>(((control_ & 8) ? 0x1000 : 0) + tile * 16 + row);
}

template<unsigned Phase, bool CachedPattern, bool Wired>
#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
 * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
 * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
 * @param pattern Адрес паттерна текущей плоскости в момент fetch.
 */
[[gnu::always_inline]]
#endif
/** @details Исполняет одну фазу sprite-fetch, сохраняя dummy/CHR адреса и прочитанные plane-байты. */
inline void Ppu::fetchSpritePhase(unsigned slot, std::uint16_t pattern) noexcept {
    static_assert(Phase < 8, "sprite fetch phase must fit one slot");
    /**
     * @brief Выставляет исходный PPU-адрес с выбранными mapper-эффектами; повторный адресный drive не удаляется.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    [[maybe_unused]] const auto drive=[this](std::uint16_t value) noexcept {
        if constexpr(Wired) address(value);
        else busAddress_=value&0x3FFFU;
    };
    oamAddress_ = 0;
    auto& sprite = sprites_[slot];
    if constexpr (Phase == 0) {
        sprite.x = secondary_[slot * 4 + 3];
        sprite.attributes = secondary_[slot * 4 + 2];
        sprite.index = secondaryIndex_[slot];
        const int row = scanline_ - secondary_[slot * 4];
        sprite.valid = scanline_ >= 0 && sprite.index != 0xFF && row >= 0 && row < ((control_ & 0x20) ? 16 : 8);
        drive(0x2000 | (v_ & 0x0FFF));
    } else if constexpr (Phase == 1) {
        (void)bus_.ppuReadFetch(busAddress_, PpuFetchKind::DummyName, 0);
    } else if constexpr (Phase == 2) {
        drive(0x2000 | (v_ & 0x0FFF));
    } else if constexpr (Phase == 3) {
        (void)bus_.ppuReadFetch(busAddress_, PpuFetchKind::DummyName, 0);
        drive(CachedPattern ? pattern : spritePattern(slot));
    } else if constexpr (Phase == 4) {
        drive(CachedPattern ? pattern : spritePattern(slot));
    } else if constexpr (Phase == 5) {
        const auto value = bus_.ppuReadFetch(busAddress_, PpuFetchKind::SpritePattern, 0);
        sprite.low = (sprite.attributes & 0x40) ? reverse(value) : value;
    } else if constexpr (Phase == 6) {
        drive((CachedPattern ? pattern : spritePattern(slot)) + 8);
    } else if constexpr (Phase == 7) {
        const auto value = bus_.ppuReadFetch(busAddress_, PpuFetchKind::SpritePattern, 0);
        sprite.high = (sprite.attributes & 0x40) ? reverse(value) : value;
    }
}

/**
 * @brief Загружает спрайтовое состояние следующей строки в исходном порядке слотов.
 * @details Число видимых спрайтов не отменяет шинные обращения пустых слотов.
 */
void Ppu::fetchSprites() noexcept {
    if (dot_ == 257) synchronizeSpriteEvaluation(256);
    const unsigned slot = (dot_ - 257) / 8;
    switch ((dot_ - 257) & 7) {
    case 0: fetchSpritePhase<0>(slot); break;
    case 1: fetchSpritePhase<1>(slot); break;
    case 2: fetchSpritePhase<2>(slot); break;
    case 3: fetchSpritePhase<3>(slot); break;
    case 4: fetchSpritePhase<4>(slot); break;
    case 5: fetchSpritePhase<5>(slot); break;
    case 6: fetchSpritePhase<6>(slot); break;
    case 7: fetchSpritePhase<7>(slot); break;
    }
}

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
template<bool Clocked, unsigned Phase, bool Wired>
#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
 * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
 * @param slot Текущий sprite-слот 0..7, обновляемый при переходе восьмиточечной группы.
 * @param patternSlot Индекс слота, для которого вычислен локальный pattern-кэш; обновляется вместе с pattern.
 * @param pattern Локальный вычисленный pattern-адрес спрайта; действителен только для соответствующего cached-slot.
 */
[[gnu::always_inline]]
#endif
/** @details Маршрутизирует настоящий sprite-fetch dot к соответствующей фазе без изменения числа тактов. */
inline void Ppu::spriteFetchDot(unsigned& slot, unsigned& patternSlot, std::uint16_t& pattern) noexcept {
    if constexpr (Clocked) bus_.ppuCycle();
    if constexpr (Phase == 3 || Phase == 4 || Phase == 6) {
        // Конкретные callback Machine не меняют регистры PPU или secondary
        // OAM. Повторно использовать адрес только внутри непрерывного CPU-интервала.
        if (patternSlot != slot) { pattern = spritePattern(slot); patternSlot = slot; }
    }
    fetchSpritePhase<Phase, true, Wired>(slot, pattern);
    ++dot_;
    if constexpr (Phase == 7) ++slot;
}

template<bool Clocked>
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/**
 * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
 * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
 * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
 */
void Ppu::spriteFetchGroup(unsigned dots) noexcept {
    unsigned slot = (dot_ - 257) / 8;
    unsigned patternSlot = ~0U;
    std::uint16_t pattern = 0;
#if defined(NES_CORE_PPU_PURE_SPRITE_SLOTS) && NES_CORE_PPU_PURE_SPRITE_SLOTS
    // View заимствован лишь на этот исключительный вызов; control=0 оставляет
    // базу всех восьми 1 KiB CHR pages. Pattern выбирает фактический table/банк.
    Machine::FullPpuSpanView pureView;
    bool pureSlots = false;
    if constexpr (!Clocked) {
        pureSlots = dots >= 8U && !trackBusAddresses_ && bus_.fullPpuSpanView(v_, 0U, pureView);
    }
#endif
#if defined(NES_CORE_PPU_SPRITE_SPAN) && NES_CORE_PPU_SPRITE_SPAN
    while (dots) {
#if defined(NES_CORE_PPU_PURE_SPRITE_SLOTS) && NES_CORE_PPU_PURE_SPRITE_SLOTS
    if constexpr (!Clocked) {
        if (pureSlots && dots >= 8U && ((dot_ - 257U) & 7U) == 0U) {
            pattern = spritePattern(slot); patternSlot = slot;
            // Phase0 сохраняет OAMADDR, metadata/valid и настоящий NT address.
            // Два отброшенных NT reads не имеют mapper-effect по допуску view.
            fetchSpritePhase<0, true, false>(slot, pattern);
            const auto* planes = pureView.chr[(pattern >> 10U) & 7U] + (pattern & 1023U);
            const auto low = planes[0], high = planes[8];
            auto& sprite = sprites_[slot];
            sprite.low = (sprite.attributes & 0x40U) ? reverse(low) : low;
            sprite.high = (sprite.attributes & 0x40U) ? reverse(high) : high;
            // low offset<=1015: +8 остаётся в той же физической 1 KiB page.
            // Частичный следующий slot получает свежий pattern; старые planes
            // в неполном prefix/suffix вообще не меняются этим путём.
            busAddress_ = static_cast<std::uint16_t>(pattern + 8U);
            dot_ = static_cast<std::uint16_t>(dot_ + 8U); ++slot; dots -= 8U;
            continue;
        }
    }
#endif
#if defined(NES_CORE_SPRITE_SLOT_SIGNALS) && NES_CORE_SPRITE_SLOT_SIGNALS
    if(dots>=8&&((dot_-257)&7)==0){
        pattern=spritePattern(slot);patternSlot=slot;
        const bool high=(pattern&0x1000U)!=0;
        if constexpr(Clocked){
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN
            if(trackBusAddresses_&&bus_.usesMmc3SpriteSignals(high)){
                // Сохранить все четыре настоящих чтения памяти; подавить лишь
                // уже доказанную последовательность восьми тактов/пяти адресов.
                spriteFetchDot<false,0,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,1,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,2,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,3,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,4,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,5,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,6,false>(slot,patternSlot,pattern);
                spriteFetchDot<false,7,false>(slot,patternSlot,pattern);
                bus_.advanceMmc3TileSignals(high);dots-=8;continue;
            }
#endif
        }else{
#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN
            if(trackBusAddresses_&&bus_.usesLegacyTileSignals(1,high)){
                fetchSpritePhase<0,true,false>(slot,pattern);
                const auto planes=bus_.legacySpritePlanes(pattern);
                auto& sprite=sprites_[slot];
                const auto low=static_cast<std::uint8_t>(planes),upper=static_cast<std::uint8_t>(planes>>8);
                sprite.low=(sprite.attributes&0x40)?reverse(low):low;
                sprite.high=(sprite.attributes&0x40)?reverse(upper):upper;
                busAddress_=static_cast<std::uint16_t>(pattern+8U);
                dot_=static_cast<std::uint16_t>(dot_+8U);++slot;dots-=8;continue;
            }
#endif
        }
    }
#endif
    switch ((dot_ - 257) & 7) {
    case 0: spriteFetchDot<Clocked, 0>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 1: spriteFetchDot<Clocked, 1>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 2: spriteFetchDot<Clocked, 2>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 3: spriteFetchDot<Clocked, 3>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 4: spriteFetchDot<Clocked, 4>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 5: spriteFetchDot<Clocked, 5>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 6: spriteFetchDot<Clocked, 6>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 7: spriteFetchDot<Clocked, 7>(slot, patternSlot, pattern); if (!--dots) return;
    }
    }
#else
    switch ((dot_ - 257) & 7) {
    case 0: spriteFetchDot<Clocked, 0>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 1: spriteFetchDot<Clocked, 1>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 2: spriteFetchDot<Clocked, 2>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 3: spriteFetchDot<Clocked, 3>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 4: spriteFetchDot<Clocked, 4>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 5: spriteFetchDot<Clocked, 5>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 6: spriteFetchDot<Clocked, 6>(slot, patternSlot, pattern); if (!--dots) return; [[fallthrough]];
    case 7: spriteFetchDot<Clocked, 7>(slot, patternSlot, pattern); if (!--dots) return;
    }
    spriteFetchDot<Clocked, 0>(slot, patternSlot, pattern); if (!--dots) return;
    spriteFetchDot<Clocked, 1>(slot, patternSlot, pattern); if (!--dots) return;
    spriteFetchDot<Clocked, 2>(slot, patternSlot, pattern);
#endif
}
#endif

/**
 * @brief Строит текущий кэш выбора спрайтов для видимой строки.
 * @details Порядок OAM и окклюзия ранними спрайтами сохраняются; отдельная маска Sprite0 не разрешает менять выигравший пиксель.
 */
void Ppu::buildSpriteRow() noexcept {
    flushPixels();
    // Декодировать уже выбранные sprite-единицы с сохранением приоритета
    // первого OAM-победителя. Palette RAM и PPUMASK вычисляются на каждой точке.
    spriteRow_.fill(0);
    for (const auto& sprite : sprites_) {
        if (!sprite.valid) continue;
        const unsigned metadata = ((sprite.attributes & 3) << 2) |
            ((sprite.attributes & 0x20) ? 0x10 : 0) | (sprite.index == 0 ? 0x20 : 0);
        for (unsigned bit = 0; bit < 8 && sprite.x + bit < width; ++bit) {
            const unsigned location = sprite.x + bit;
            if (spriteRow_[location]) continue;
            const unsigned planeMask = 0x80U >> bit;
            const unsigned pixel = ((sprite.low & planeMask) ? 1 : 0) | ((sprite.high & planeMask) ? 2 : 0);
            if (pixel) spriteRow_[location] = static_cast<std::uint8_t>(metadata | pixel);
        }
    }
    spriteCacheClocks_ = 0;
    spriteCacheValid_ = true;
}

/**
 * @brief Доводит отложенное состояние спрайтов до текущего растрового среза.
 * @details Раздельные плоскости, flip и накопленные сдвиги относятся к моменту захвата, а не к новой метаинформации.
 */
void Ppu::materializeSprites() const noexcept {
    flushPixels();
    if (!spriteCacheValid_) return;
    // Все восемь единиц, включая недопустимые, сохраняют точные счётчики
    // и старые плоскости, если рендер остановлен посреди строки или выборки.
    for (auto& sprite : sprites_) {
        if (spriteCacheClocks_ <= sprite.x) sprite.x -= static_cast<std::uint8_t>(spriteCacheClocks_);
        else {
            const unsigned shifts = spriteCacheClocks_ - sprite.x;
            sprite.x = 0;
            if (shifts >= 8) sprite.low = sprite.high = 0;
            else { sprite.low <<= shifts; sprite.high <<= shifts; }
        }
    }
    spriteCacheValid_ = false;
    spriteCacheClocks_ = 0;
}

/**
 * @brief Накапливает отложенный видимый пиксель в ограниченном текущем интервале.
 * @details Перед наблюдаемой границей очередь должна быть доведена без изменения приоритетов и палитры.
 */
void Ppu::queuePixel() noexcept {
    if (!pendingPixels_) {
        pendingPixelX_ = static_cast<std::uint8_t>(dot_ - 1);
        pendingPixelShift_ = pendingBackgroundShifts_;
    }
    if (++pendingPixels_ == 8 || dot_ == 256) flushPixels();
}

/**
 * @brief Восстанавливает упакованные пары фоновых индексов после изменения конвейера.
 * @details Оптимизация не меняет fineX, палитровые биты или оставшийся частичный хвост.
 */
void Ppu::rebuildBackgroundPairs() const noexcept {
    const unsigned colourMask = (mask_ & 1) ? 0x30U : 0x3FU;
    for (unsigned pair = 0; pair < 256; ++pair) {
        const unsigned first = pair & 15U, second = pair >> 4;
        backgroundPairs_[pair][0] = static_cast<std::uint8_t>(palette_[(first & 3U) ? first : 0] & colourMask);
        backgroundPairs_[pair][1] = static_cast<std::uint8_t>(palette_[(second & 3U) ? second : 0] & colourMask);
    }
    backgroundPairsDirty_ = false;
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    if(auto* rgb=PpuRgbOutput::activeFor(this))rgb->paletteRebuilt();
#endif
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
#if defined(NES_CORE_PPU_RGB_SINK_FORWARD) && NES_CORE_PPU_RGB_SINK_FORWARD
/** @details Выдаёт восемь уже разрешённых пикселей в принадлежащий output target. */
inline unsigned Ppu::writeTile8(std::uint32_t background, std::uint8_t* output,
    const std::uint8_t* sprites, unsigned start, unsigned mask, PpuRgbOutput* rgb) const noexcept {
#else
/**
 * @brief Фиксирует восемь уже выбранных пикселей в индексный и допустимый RGB-выход.
 * @details Переданный RGB-контекст действителен лишь в том же исключительном вызове PPU; атомарная идентичность внешнего владельца не кэшируется между API/кадрами.
 * @param background Восемь уже выбранных фоновых индексов, по четыре бита на пиксель.
 * @param output Участок индексного результата на восемь пикселей; вызывающий обеспечивает границы и наличие хранения.
 * @param sprites Уже декодированная строка первых непрозрачных sprite-победителей; читается без изменения счётчиков OAM.
 * @param start Начальная позиция ограниченного участка вывода.
 * @param mask Снимок PPUMASK для этого растрового интервала.
 * @return Накопленный признак Sprite0-попадания выбранных пикселей; индексы/RGB уже записаны, число пикселей не возвращается.
 */
inline unsigned Ppu::writeTile8(std::uint32_t background, std::uint8_t* output,
    const std::uint8_t* sprites, unsigned start, unsigned mask) const noexcept {
#endif
    unsigned hit = 0;
#if defined(NES_CORE_PPU_RGB_SINK_FORWARD) && NES_CORE_PPU_RGB_SINK_FORWARD
    // Исключительный интервал того же актора: вызывающий уже захватил именно этот sink.
#else
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    auto* rgb=PpuRgbOutput::activeFor(this);
#endif
#endif
        // Полный выровненный тайл имеет один набор left-edge-разрешений.
        // Неизменяемый selector содержит только priority/palette-index/hit-логику;
        // palette RAM и grayscale вычисляются и в этом flush.
        const bool backgroundVisible = (mask & 8) && (start >= 8 || (mask & 2));
        const bool spritesVisible = (mask & 0x10) && (start >= 8 || (mask & 4));
        const unsigned colourMask = (mask & 1) ? 0x30 : 0x3F;
        const unsigned spriteMask = spritesVisible ? 63U : 0U;
        if (!backgroundVisible) background = 0;
        bool emptySprites = !spritesVisible;
        if (spritesVisible) {
            std::uint32_t first = 0, second = 0;
            std::memcpy(&first, sprites, sizeof(first));
            std::memcpy(&second, sprites + 4, sizeof(second));
            emptySprites = ((first | second) & 0x03030303U) == 0;
        }
        if (emptySprites) {
            if (!(background & 0x33333333U)) {
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
                if(output)
#endif
                std::memset(output, palette_[0] & colourMask, 8);
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
                if(rgb)rgb->uniform(start,8,palette_[0]&colourMask,mask>>5);
#endif
            } else {
                if (backgroundPairsDirty_) rebuildBackgroundPairs();
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
                if(rgb)rgb->background8(*this,start,background,mask>>5);
#endif
                for (unsigned pair = 0; pair < 4; ++pair, background >>= 8){
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
                    if(output)
#endif
                    std::memcpy(output + pair * 2, backgroundPairs_[background & 255U].data(), 2);
                }
            }
        } else {
            for (unsigned i = 0; i < 8; ++i, background >>= 4) {
                const unsigned key = ((sprites[i] & spriteMask) << 4) | (background & 15U);
                const unsigned selection = pixelSelection[key];
                if (start + i != width - 1) hit |= selection & 0x20U;
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
                if(output)
#endif
                output[i] = palette_[selection & 31U] & colourMask;
            }
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
            if(rgb)rgb->selectedIndices(output,start,8,mask>>5);
#endif
        }
    return hit ? 0x40U : 0U;
}

/**
 * @brief Доводит отложенную очередь видимых пикселей до текущего среза.
 * @details Сначала сохраняет исходный индексный выбор и необходимые хвосты; материализация не даёт права переиспользовать аппаратно читаемый буфер.
 */
void Ppu::flushPixels() const noexcept {
    const unsigned count = pendingPixels_;
    if (!count) return;
    pendingPixels_ = 0;
    const unsigned start = pendingPixelX_;
    const unsigned shift = fineX_ + pendingPixelShift_;
    std::uint32_t background =
        expandedPixelPlane[((static_cast<unsigned>(backgroundLow_) << shift) >> 8) & 255] |
        (expandedPixelPlane[((static_cast<unsigned>(backgroundHigh_) << shift) >> 8) & 255] << 1) |
        (expandedPixelPlane[((static_cast<unsigned>(attributeLow_) << shift) >> 8) & 255] << 2) |
        (expandedPixelPlane[((static_cast<unsigned>(attributeHigh_) << shift) >> 8) & 255] << 3);
    const unsigned mask = mask_;
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
    auto* output = pixels_.data();
    if(output)output += static_cast<unsigned>(scanline_) * width + start;
#else
    auto* output = pixels_.data() + static_cast<unsigned>(scanline_) * width + start;
#endif
    const auto* sprites = spriteRow_.data() + start;
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    auto* rgb=PpuRgbOutput::activeFor(this);
    if(rgb)rgb->beginSpan(*this,static_cast<unsigned>(scanline_),start,count,mask>>5,lineEmphasis_[static_cast<unsigned>(scanline_)]);
#endif
    lineEmphasis_[static_cast<unsigned>(scanline_)] = static_cast<std::uint8_t>(mask >> 5);
    if (count == 8 && (start & 7) == 0) {
#if defined(NES_CORE_PPU_RGB_SINK_FORWARD) && NES_CORE_PPU_RGB_SINK_FORWARD
        const unsigned hit = writeTile8(background, output, sprites, start, mask, rgb);
#else
        const unsigned hit = writeTile8(background, output, sprites, start, mask);
#endif
        if (hit) status_ |= 0x40;
    } else {
    for (unsigned i = 0; i < count; ++i, background >>= 4) {
        const unsigned x = start + i;
        const unsigned bgIndex = ((mask & 8) && (x >= 8 || (mask & 2))) ? (background & 15) : 0;
        const unsigned bg = bgIndex & 3;
        const unsigned sprite = ((mask & 0x10) && (x >= 8 || (mask & 4))) ? sprites[i] : 0;
        const unsigned sp = sprite & 3;
        if (bg && sp && (sprite & 0x20) && x != 255) status_ |= 0x40;
        unsigned location = 0;
        if (sp && (!bg || !(sprite & 0x10))) location = 0x10 + (sprite & 0x0F);
        else if (bg) location = bgIndex;
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
        if(output)
#endif
        output[i] = palette_[location] & ((mask & 1) ? 0x30 : 0x3F);
    }
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    if(rgb)rgb->selectedIndices(output,start,count,mask>>5);
#endif
    }
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    if(rgb)rgb->finishSpan(*this);
#endif
    spriteCacheClocks_ = static_cast<std::uint16_t>(spriteCacheClocks_ + count);
    if (start + count == width) materializeSprites();
}

/**
 * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
 * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
 */
void Ppu::renderPixel() noexcept {
    flushPixels();
    const unsigned x = dot_ - 1;
    if (spriteCacheValid_ && !rendering()) materializeSprites();
    unsigned background = 0, backgroundPalette = 0;
    if (rendering() && (mask_ & 8) && (x >= 8 || (mask_ & 2))) {
        const unsigned bit = 0x8000U >> (fineX_ + pendingBackgroundShifts_);
        background = ((backgroundLow_ & bit) ? 1 : 0) | ((backgroundHigh_ & bit) ? 2 : 0);
        backgroundPalette = ((attributeLow_ & bit) ? 1 : 0) | ((attributeHigh_ & bit) ? 2 : 0);
    }
    unsigned spritePixel = 0, spritePalette = 0;
    bool spriteBehind = false, spriteZero = false;
    if (rendering() && (mask_ & 0x10) && (x >= 8 || (mask_ & 4))) {
        if (spriteCacheValid_) {
            const auto entry = spriteRow_[x];
            spritePixel = entry & 3;
            spritePalette = (entry >> 2) & 3;
            spriteBehind = (entry & 0x10) != 0;
            spriteZero = (entry & 0x20) != 0;
        } else {
            for (const auto& sprite : sprites_) {
                if (!sprite.valid || sprite.x) continue;
                const unsigned pixel = ((sprite.low & 0x80) ? 1 : 0) | ((sprite.high & 0x80) ? 2 : 0);
                if (!pixel) continue;
                spritePixel = pixel;
                spritePalette = sprite.attributes & 3;
                spriteBehind = (sprite.attributes & 0x20) != 0;
                spriteZero = sprite.index == 0;
                break;
            }
        }
    }
    if (background && spritePixel && spriteZero && x != 255) status_ |= 0x40;
    unsigned location = 0;
    if (spritePixel && (!background || !spriteBehind)) location = 0x10 + spritePalette * 4 + spritePixel;
    else if (background) location = backgroundPalette * 4 + background;
    if (!rendering() && (v_ & 0x3F00) == 0x3F00) location = paletteAddress(v_);
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    auto* rgb=PpuRgbOutput::activeFor(this);
    if(rgb)rgb->beginSpan(*this,static_cast<unsigned>(scanline_),x,1,mask_>>5,lineEmphasis_[static_cast<unsigned>(scanline_)]);
#endif
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
    if(pixels_.data())
#endif
    pixels_[static_cast<unsigned>(scanline_) * width + x] = palette_[location] & ((mask_ & 1) ? 0x30 : 0x3F);
    lineEmphasis_[static_cast<unsigned>(scanline_)] = mask_ >> 5;
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    if(rgb){rgb->selectedIndices(pixels_.data()+static_cast<unsigned>(scanline_)*width+x,x,1,mask_>>5);rgb->finishSpan(*this);}
#endif
    if (rendering()) {
        if (spriteCacheValid_) ++spriteCacheClocks_;
        else {
            for (auto& sprite : sprites_) {
                if (sprite.x) --sprite.x;
                else { sprite.low <<= 1; sprite.high <<= 1; }
            }
        }
    }
    if (x == width - 1) materializeSprites();
}

#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
/**
 * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
 * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
 */
[[gnu::always_inline]]
#endif
/** @details Исполняет видимую точку, сохраняя Sprite0, сдвиги и исходные fetch-эффекты. */
inline void Ppu::visibleDot() noexcept {
    if (dot_ == 255) scrollSignal_ = true;
    if (((dot_ - 1) & 7) == 0) loadBackground();
    if (spriteCacheValid_) queuePixel();
    else renderPixel();
    if (pendingBackgroundShifts_ < 16) ++pendingBackgroundShifts_;
    fetchBackground();
    if (dot_ == 256 && scrollSignal_) incrementY();
    ++dot_;
}

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
template<unsigned Phase>
#if defined(__GNUC__) || defined(__clang__)
/**
 * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
 * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
 */
[[gnu::always_inline]]
#endif
/** @details Выбирает фазу видимой точки; группировка не пересекает наблюдаемый MMIO/mapper горизонт. */
inline void Ppu::visibleDotPhase() noexcept {
    static_assert(Phase < 8, "visible fetch phase must fit one tile");
    if constexpr (Phase == 0) loadBackground();
    queuePixel(); // Входная проверка гарантирует действительный sprite-кэш до последней точки.
    if (pendingBackgroundShifts_ < 16) ++pendingBackgroundShifts_;
    fetchBackgroundPhase<Phase>();
    if constexpr (Phase == 6) { if (dot_ == 255) scrollSignal_ = true; }
    if constexpr (Phase == 7) { if (dot_ == 256 && scrollSignal_) incrementY(); }
    ++dot_;
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
/**
 * @brief Обслуживает ограниченную группу видимых точек без отдельного mapper-clock.
 * @details Шаблонные фазы сохраняют тот же конвейер, что и скалярный путь; режим с нужными mapper-часами сюда не переносится.
 * @param dots Число уже прошедших точек PPU.
 */
void Ppu::visibleUnclockedGroup(unsigned dots) noexcept {
    // Ровно три/четыре точки без подключённой clock-линии. Каждая фаза фиксируется
    // до следующей; чтения сохраняют живой входящий busAddress_.
    switch ((dot_ - 1) & 7) {
    case 0: visibleDotPhase<0>(); if (!--dots) return; [[fallthrough]];
    case 1: visibleDotPhase<1>(); if (!--dots) return; [[fallthrough]];
    case 2: visibleDotPhase<2>(); if (!--dots) return; [[fallthrough]];
    case 3: visibleDotPhase<3>(); if (!--dots) return; [[fallthrough]];
    case 4: visibleDotPhase<4>(); if (!--dots) return; [[fallthrough]];
    case 5: visibleDotPhase<5>(); if (!--dots) return; [[fallthrough]];
    case 6: visibleDotPhase<6>(); if (!--dots) return; [[fallthrough]];
    case 7: visibleDotPhase<7>(); if (!--dots) return;
    }
    visibleDotPhase<0>(); if (!--dots) return;
    visibleDotPhase<1>(); if (!--dots) return;
    visibleDotPhase<2>();
}
#endif

#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
[[gnu::always_inline]] inline void Ppu::tickInline() noexcept {
#else
/**
 * @brief Продвигает устройство на одну его логическую единицу времени.
 * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
 */
void Ppu::tick() noexcept {
#endif
    if (trackBusCycles_) bus_.ppuCycle();
    // Большинство активных точек не начинает/заканчивает кадр, не копирует scroll, не выбирает
    // спрайты и не пропускает последнюю pre-render-точку. Сохранить порядок шинных эффектов,
    // избегая проверок несвязанных состояний в обычном пути.
    if (renderingEnabled_ && (renderDelay_ | nmiDelay_) == 0 &&
        static_cast<unsigned>(scanline_) < height && static_cast<unsigned>(dot_ - 1) < width) {
        visibleDot();
        return;
    }
    if (renderDelay_ && --renderDelay_ == 0) {
        if (renderingEnabled_ && !renderingNext_) synchronizeSprites();
        renderingEnabled_ = renderingNext_;
    }
    if (nmiDelay_ && --nmiDelay_ == 0 && nmiLine_) nmiPending_ = true;
    if (scanline_ == -1 && dot_ == 1) {
        status_ &= 0x1F;
        suppressVblank_ = false;
        updateNmi();
    }
    if (scanline_ == 241 && dot_ == 1) {
        if (!suppressVblank_) status_ |= 0x80;
        frameReady_ = true;
        ++frames_;
        updateNmi();
    }
    const bool active = rendering() && renderLine();
    if (dot_ == 255) scrollSignal_ = active;
    if (scanline_ >= 0 && scanline_ < 240 && dot_ == 0) {
        evaluationNextDot_ = 1;
        if (rendering()) buildSpriteRow();
    }
    const bool backgroundFetch = (dot_ >= 1 && dot_ <= 256) || (dot_ >= 321 && dot_ <= 336);
    if (!active && backgroundFetch && ((dot_ - 1) & 7) == 6) coarseXSignal_ = false;
    if (active && backgroundFetch && ((dot_ - 1) & 7) == 0) loadBackground();
    if (scanline_ >= 0 && scanline_ < 240 && dot_ >= 1 && dot_ <= 256) renderPixel();
    if (active) {
        // Dot0 выставляет pattern-адрес, прерывая два последних фиктивных
        // nametable-чтения. Удержание их low-A12 до dot5 дало бы
        // MMC3 ложный лишний фронт при background patterns по $1000.
        if (dot_ == 0 && scanline_ >= 0) address(((control_ & 0x10) ? 0x1000 : 0) + nextTile_ * 16 + ((v_ >> 12) & 7));
        if (backgroundFetch) {
            // После 16 сдвигов все четыре 16-битных регистра равны нулю; насыщаемый
            // счётчик также учитывает произвольные включения тактирования рендера.
            if (pendingBackgroundShifts_ < 16) ++pendingBackgroundShifts_;
            fetchBackground();
        }

        if (dot_ == 257) { loadBackground(); finishHorizontalScrollSignal(); }
        if (scanline_ == -1 && dot_ >= 280 && dot_ <= 304) v_ = (v_ & 0x041F) | (t_ & 0x7BE0);
        if (dot_ >= 257 && dot_ <= 320) fetchSprites();
        if (dot_ == 337) { loadBackground(); address(0x2000 | (v_ & 0x0FFF)); }
        if (dot_ == 338 || dot_ == 340) (void)bus_.ppuReadFetch(busAddress_, PpuFetchKind::DummyName, 0);
        if (dot_ == 339) address(0x2000 | (v_ & 0x0FFF));
    } else address(v_);
    if (!active && backgroundFetch && ((dot_ - 1) & 7) == 7 && coarseXSignal_) { incrementX(); coarseXSignal_ = false; }
    if (dot_ == 256 && scrollSignal_) incrementY();
    if (!active && dot_ == 257) finishHorizontalScrollSignal();
    if (scanline_ >= 0 && scanline_ < 240 && dot_ >= 1 && dot_ <= 256 && !rendering()) evaluationNextDot_ = dot_ + 1;
    if (!pal_ && scanline_ == -1 && dot_ == 339 && odd_ && rendering()) {
        dot_ = 0;
        scanline_ = 0;
        return;
    }
    if (++dot_ > 340) {
        dot_ = 0;
        if (++scanline_ > (pal_ ? 310 : 260)) { scanline_ = -1; odd_ = !odd_; }
    }
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
/**
 * @brief Компонует невидимый растровый интервал до ближайшего особого события.
 * @details Не пересекает очистку/публикацию VBlank, NMI и обязательные адресные фазы.
 * @param dots Число уже прошедших точек PPU.
 */
__declspec(noinline)
#endif
/** @details Группирует только уже разрешённые невидимые точки с сохранением NMI/адресных границ. */
void Ppu::nonVisibleSpan(unsigned dots) noexcept {
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    // Конкретные callback Machine не наблюдают/не меняют растровые поля PPU.
    // Сохранить каждый подключённый callback; обобщённые wired-шины сюда не входят.
    if (trackBusCycles_ || trackBusAddresses_) {
#if defined(NES_CORE_PPU_STABLE_WIRES) && NES_CORE_PPU_STABLE_WIRES
        bus_.advanceStablePpuSignals(dots, busAddress_, trackBusCycles_, trackBusAddresses_);
#else
        unsigned remaining = dots;
        do {
            if (trackBusCycles_) bus_.ppuCycle();
            if (trackBusAddresses_) bus_.ppuAddress(busAddress_);
        } while (--remaining);
#endif
    }
#endif
    unsigned next = dot_ + dots; // <=595: максимум один безопасный wrap холостой строки.
    if (next >= 341) { next -= 341; ++scanline_; }
    dot_ = static_cast<std::uint16_t>(next);
}

#if defined(NES_CORE_PPU_TILE8) && NES_CORE_PPU_TILE8 && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
// Выборки хронологичны. Конкретные callback Machine меняют картридж,
// но не читают/меняют PPU. Внутри уже ограниченного quiet-интервала
// нет CPU-наблюдателя. CHR-чтения не кэшировать: Legacy-защёлка действует
// после каждого возвращённого байта ровно как в tile8().
template<bool Clocked, bool Addressed, bool Mmc3>
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
/**
 * @brief Читает фоновые тайлы ограниченного интервала и сохраняет все живые данные.
 * @details Локальное отображение страниц действительно только внутри этого вызова; реальные mapper-сигналы и четыре вида чтений остаются в нужном порядке.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 */
__declspec(noinline)
#endif
/** @details Составляет точный tile-span внутри исходного quiet-grant, сохраняя каждый требуемый read. */
void Ppu::fetchTileSpan(unsigned tiles) noexcept {
    flushPixels();
    std::uint32_t fetched[32]; // Максимум остаток видимой строки:128 байтов.
    unsigned v = v_;
    unsigned x = dot_ - 1;
    std::uint16_t finalAddress = busAddress_;
    const unsigned control = control_;
    /**
     * @brief Выставляет исходный PPU-адрес с выбранными mapper-эффектами; повторный адресный drive не удаляется.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto drive = [this](std::uint16_t addressValue) noexcept {
        if constexpr (Addressed) bus_.ppuAddress(addressValue);
    };
#if defined(NES_CORE_PPU_FULL_SPAN_VIEW) && NES_CORE_PPU_FULL_SPAN_VIEW
    if constexpr (!Clocked && !Addressed && !Mmc3) {
        Machine::FullPpuSpanView view;
        // flushPixels выше уже опубликовал отложенный старый конвейер.
        // Эффекты wires/strobes не опускаются: у допущенного чистого графа их нет.
        if (bus_.fullPpuSpanView(static_cast<std::uint16_t>(v),control,view)) {
            for (unsigned index = 0; index < tiles; ++index, x += 8) {
                const unsigned page=(v>>10U)&1U;
                const auto tile=view.names[page][v&31U];
                const unsigned attribute=(view.attributes[page][(v>>2U)&7U] >> (((v>>4U)&4U)|(v&2U)))&3U;
                const auto pattern=static_cast<std::uint16_t>(((control&0x10U)?0x1000U:0U)+unsigned(tile)*16U+((v>>12U)&7U));
                const auto* planes=view.chr[(pattern>>10U)&3U]+(pattern&1023U);
                // Младшая тетрада pattern — fineY<=7, поэтому +8 остаётся в той же 1KiB-странице.
                const auto low=planes[0];
                const auto high=planes[8];
                finalAddress=static_cast<std::uint16_t>(pattern+8U);
                fetched[index]=std::uint32_t(low)|(std::uint32_t(high)<<8U)|(attribute<<16U)|(std::uint32_t(tile)<<24U);
                if ((v & 31U) == 31U) v = (v & 0x7FE0U) ^ 0x0400U;
                else ++v;
                if (x == width - 8) {
                    if ((v & 0x7000U) != 0x7000U) v += 0x1000U;
                    else {
                        v &= 0x0FFFU;
                        unsigned y = (v >> 5) & 31U;
                        if (y == 29) { y = 0; v ^= 0x0800U; }
                        else if (y == 31) y = 0;
                        else ++y;
                        v = (v & 0x7C1FU) | (y << 5);
                    }
                }
            }
            v_=static_cast<std::uint16_t>(v);
            busAddress_=finalAddress;
            renderFetchedTiles(fetched,tiles);
            return;
        }
    }
#endif
    for (unsigned index = 0; index < tiles; ++index, x += 8) {
        if constexpr (Clocked) bus_.ppuCycle();
        const auto nametable = static_cast<std::uint16_t>(0x2000 | (v & 0x0FFF));
        drive(nametable);
        if constexpr (Clocked) bus_.ppuCycle();
        const auto tile = bus_.ppuReadFetch(nametable, PpuFetchKind::BackgroundName, (x >> 3U) + 2U);
        if constexpr (Clocked) bus_.ppuCycle();
        const auto attributeAddress = static_cast<std::uint16_t>(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
        drive(attributeAddress);
        if constexpr (Clocked) bus_.ppuCycle();
        const auto attribute = (bus_.ppuReadFetch(attributeAddress, PpuFetchKind::BackgroundAttribute, (x >> 3U) + 2U) >> (((v >> 4) & 4) | (v & 2))) & 3U;
        const auto pattern = static_cast<std::uint16_t>(((control & 0x10) ? 0x1000 : 0) + tile * 16 + ((v >> 12) & 7));
        drive(pattern);
        if constexpr (Clocked) bus_.ppuCycle();
        drive(pattern);
        if constexpr (Clocked) bus_.ppuCycle();
        const auto low = bus_.ppuReadFetch(pattern, PpuFetchKind::BackgroundPattern, (x >> 3U) + 2U);
        if constexpr (Clocked) bus_.ppuCycle();
        finalAddress = static_cast<std::uint16_t>(pattern + 8);
        drive(finalAddress);
        if constexpr (Clocked) bus_.ppuCycle();
        const auto high = bus_.ppuReadFetch(finalAddress, PpuFetchKind::BackgroundPattern, (x >> 3U) + 2U);
        fetched[index] = static_cast<std::uint32_t>(low) |
            (static_cast<std::uint32_t>(high) << 8) | (attribute << 16) |
            (static_cast<std::uint32_t>(tile) << 24);
        if ((v & 31U) == 31U) v = (v & 0x7FE0U) ^ 0x0400U;
        else ++v;
        if (x == width - 8) {
            if ((v & 0x7000U) != 0x7000U) v += 0x1000U;
            else {
                v &= 0x0FFFU;
                unsigned y = (v >> 5) & 31U;
                if (y == 29) { y = 0; v ^= 0x0800U; }
                else if (y == 31) y = 0;
                else ++y;
                v = (v & 0x7C1FU) | (y << 5);
            }
        }
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN
        if constexpr (Mmc3) bus_.advanceMmc3TileSignals((control & 0x10) != 0);
#endif
    }
    v_ = static_cast<std::uint16_t>(v);
    busAddress_ = finalAddress;
    renderFetchedTiles(fetched, tiles);
}

#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
/**
 * @brief Читает низкий Legacy-span только после существующих проверок режима и маркеров.
 * @details Не переносит FD/FE-защёлки через будущий тайл и не меняет допуск временного C1-владельца.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 */
__declspec(noinline)
#endif
/** @details Исполняет tile-span Legacy160 с исходной latch/CHR-проводкой. */
void Ppu::fetchLegacyTileSpan(unsigned tiles) noexcept {
    flushPixels();
    std::uint32_t fetched[32];
    unsigned v = v_;
    unsigned x = dot_ - 1U;
    const unsigned control = control_;
    std::uint16_t finalAddress = busAddress_;
#if defined(NES_CORE_PPU_FULL_LEGACY_LOW_SPAN) && NES_CORE_PPU_FULL_LEGACY_LOW_SPAN
    Machine::FullPpuSpanView view;
    if(bus_.fullLegacyLowSpanView(static_cast<std::uint16_t>(v),control,tiles,view)){
        for(unsigned index=0;index<tiles;++index,x+=8U){
            const unsigned page=(v>>10U)&1U;
            const auto tile=view.names[page][v&31U];
            const unsigned attribute=(view.attributes[page][(v>>2U)&7U] >> (((v>>4U)&4U)|(v&2U)))&3U;
            const auto pattern=static_cast<std::uint16_t>(unsigned(tile)*16U+((v>>12U)&7U));
            const auto* planes=view.chr[(pattern>>10U)&3U]+(pattern&1023U);
            // fineY<=7 доказывает принадлежность high-плоскости той же физической 1KiB-странице.
            const auto low=planes[0];
            const auto high=planes[8];
            finalAddress=static_cast<std::uint16_t>(pattern+8U);
            fetched[index]=std::uint32_t(low)|(std::uint32_t(high)<<8U)|(attribute<<16U)|(std::uint32_t(tile)<<24U);
            if ((v & 31U) == 31U) v = (v & 0x7FE0U) ^ 0x0400U;
            else ++v;
            if (x == width - 8) {
                if ((v & 0x7000U) != 0x7000U) v += 0x1000U;
                else {
                    v &= 0x0FFFU;
                    unsigned y = (v >> 5) & 31U;
                    if (y == 29) { y = 0; v ^= 0x0800U; }
                    else if (y == 31) y = 0;
                    else ++y;
                    v = (v & 0x7C1FU) | (y << 5);
                }
            }
        }
        // До цикла доказаны ноль source1-тактов и отсутствие FD/FE-чтений.
        // Опубликовать точное финальное wire-состояние до материализации пикселей.
        if(bus_.completeLegacyVideoBgSpan(tiles,finalAddress)){
            v_=static_cast<std::uint16_t>(v);
            busAddress_=finalAddress;
            renderFetchedTiles(fetched,tiles);
            return;
        }
        // При отказе завершения записей нет; сырые чтения были чистыми.
        // Восстановить локальные переменные прежнего цикла и перезаписать каждое выбранное слово.
        v=v_;x=dot_-1U;finalAddress=busAddress_;
    }
#endif
    for (unsigned index = 0; index < tiles; ++index, x += 8U) {
        fetched[index] = bus_.legacyBackgroundTile(static_cast<std::uint16_t>(v), control);
        finalAddress = static_cast<std::uint16_t>(((control & 0x10U) ? 0x1000U : 0U) + (fetched[index] >> 24) * 16U + ((v >> 12) & 7U) + 8U);
        if ((v & 31U) == 31U) v = (v & 0x7FE0U) ^ 0x0400U;
        else ++v;
        if (x == width - 8) {
            if ((v & 0x7000U) != 0x7000U) v += 0x1000U;
            else {
                v &= 0x0FFFU;
                unsigned y = (v >> 5) & 31U;
                if (y == 29) { y = 0; v ^= 0x0800U; }
                else if (y == 31) y = 0;
                else ++y;
                v = (v & 0x7C1FU) | (y << 5);
            }
        }
    }
    v_ = static_cast<std::uint16_t>(v);
    busAddress_ = finalAddress;
    renderFetchedTiles(fetched, tiles);
}
#endif

#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
/** @details Смешивает готовые BG/sprite данные в реальный RGB, не исполняя CPU/mapper повторно. */
void Ppu::renderFetchedTiles(const std::uint32_t* fetched, unsigned tiles) noexcept {
    // Две упакованные 16-битные пары плоскостей требуют лишь одного shift/mask после
    // первого произвольного входного сдвига. В пиксельном цикле нет вызовов mapper.
    const unsigned initialShift = pendingBackgroundShifts_;
    std::uint32_t planes = (((static_cast<std::uint32_t>(backgroundLow_) << initialShift) & 0xFF00U) | nextLow_) |
        ((((static_cast<std::uint32_t>(backgroundHigh_) << initialShift) & 0xFF00U) | nextHigh_) << 16);
    std::uint32_t attributes = (((static_cast<std::uint32_t>(attributeLow_) << initialShift) & 0xFF00U) | ((nextAttribute_ & 1) ? 255U : 0U)) |
        ((((static_cast<std::uint32_t>(attributeHigh_) << initialShift) & 0xFF00U) | ((nextAttribute_ & 2) ? 255U : 0U)) << 16);
    unsigned x = dot_ - 1;
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
    auto* output = pixels_.data();
    if(output)output += static_cast<unsigned>(scanline_) * width + x;
#else
    auto* output = pixels_.data() + static_cast<unsigned>(scanline_) * width + x;
#endif
    const auto* sprites = spriteRow_.data() + x;
    const unsigned fineX = fineX_;
#if defined(NES_CORE_LEGACY_PPU_ATTR_CARRIER) && NES_CORE_LEGACY_PPU_ATTR_CARRIER
    // Сохранить произвольную историю первого тайла. Завершённая последующая загрузка имеет
    // однородные current/next palette-байты даже после неполных входных сдвигов.
    std::uint32_t attributeCarrier =
        (expandedPixelPlane[(attributes >> (8U - fineX)) & 255U] << 2) |
        (expandedPixelPlane[(attributes >> (24U - fineX)) & 255U] << 3);
    unsigned previousAttribute = nextAttribute_ & 3U;
    const auto& carrierRow = legacyAttributeCarriers[fineX];
#endif
    const unsigned mask = mask_;
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    auto* rgb=PpuRgbOutput::activeFor(this);
    if(rgb)rgb->beginSpan(*this,static_cast<unsigned>(scanline_),x,tiles*8,mask>>5,lineEmphasis_[static_cast<unsigned>(scanline_)]);
#endif
    unsigned hit = 0;
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
    for (unsigned index = 0; index < tiles; ++index, x += 8, output=output?output+8:nullptr, sprites += 8) {
#else
    for (unsigned index = 0; index < tiles; ++index, x += 8, output += 8, sprites += 8) {
#endif
        const std::uint32_t background =
            expandedPixelPlane[(planes >> (8U - fineX)) & 255U] |
            (expandedPixelPlane[(planes >> (24U - fineX)) & 255U] << 1) |
#if defined(NES_CORE_LEGACY_PPU_ATTR_CARRIER) && NES_CORE_LEGACY_PPU_ATTR_CARRIER
            attributeCarrier;
#else
            (expandedPixelPlane[(attributes >> (8U - fineX)) & 255U] << 2) |
            (expandedPixelPlane[(attributes >> (24U - fineX)) & 255U] << 3);
#endif
#if defined(NES_CORE_PPU_RGB_SINK_FORWARD) && NES_CORE_PPU_RGB_SINK_FORWARD
        hit |= writeTile8(background, output, sprites, x, mask, rgb);
#else
        hit |= writeTile8(background, output, sprites, x, mask);
#endif
        if (index + 1 < tiles) {
            const auto record = fetched[index];
            planes = ((planes << 8) & 0xFF00FF00U) | (record & 255U) | ((record & 0xFF00U) << 8);
#if defined(NES_CORE_LEGACY_PPU_ATTR_CARRIER) && NES_CORE_LEGACY_PPU_ATTR_CARRIER
            const unsigned nextAttribute = (record >> 16U) & 3U;
            attributeCarrier = carrierRow[(previousAttribute << 2U) | nextAttribute];
            previousAttribute = nextAttribute;
#else
            attributes = ((attributes << 8) & 0xFF00FF00U) |
                ((record & 0x10000U) ? 255U : 0U) | ((record & 0x20000U) ? 0x00FF0000U : 0U);
#endif
        }
    }
    const auto last = fetched[tiles - 1];
#if defined(NES_CORE_LEGACY_PPU_ATTR_CARRIER) && NES_CORE_LEGACY_PPU_ATTR_CARRIER
    if (tiles > 1) {
        const unsigned currentAttribute = tiles == 2 ? (nextAttribute_ & 3U) :
            ((fetched[tiles - 3] >> 16U) & 3U);
        attributes = ((currentAttribute & 1U) ? 0xFF00U : 0U) |
            ((currentAttribute & 2U) ? 0xFF000000U : 0U) |
            ((previousAttribute & 1U) ? 0xFFU : 0U) |
            ((previousAttribute & 2U) ? 0x00FF0000U : 0U);
    }
#endif
    backgroundLow_ = static_cast<std::uint16_t>(planes);
    backgroundHigh_ = static_cast<std::uint16_t>(planes >> 16);
    attributeLow_ = static_cast<std::uint16_t>(attributes);
    attributeHigh_ = static_cast<std::uint16_t>(attributes >> 16);
    nextLow_ = static_cast<std::uint8_t>(last);
    nextHigh_ = static_cast<std::uint8_t>(last >> 8);
    nextAttribute_ = static_cast<std::uint8_t>((last >> 16) & 3U);
    nextTile_ = static_cast<std::uint8_t>(last >> 24);
    pendingBackgroundShifts_ = 8;
    pendingPixelX_ = static_cast<std::uint8_t>(x - 8);
    pendingPixelShift_ = 0;
    pendingPixels_ = 0;
    if (hit) status_ |= 0x40;
    lineEmphasis_[static_cast<unsigned>(scanline_)] = static_cast<std::uint8_t>(mask >> 5);
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    if(rgb)rgb->finishSpan(*this);
#endif
    spriteCacheClocks_ = static_cast<std::uint16_t>(spriteCacheClocks_ + tiles * 8);
    dot_ = static_cast<std::uint16_t>(dot_ + tiles * 8);
    coarseXSignal_ = false;
    if (dot_ == 257) scrollSignal_ = true;
    if (x == width) materializeSprites(); // Опубликовать логический кэш перед sync.
}

template<bool Clocked, bool Addressed>
#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
/**
 * @brief Рисует ограниченный восьмипиксельный тайл по опубликованным плоскостям и атрибутам.
 * @details Сохраняет текущие clipping, приоритет и row-метаданные потребителя.
 */
[[gnu::always_inline]]
#endif
/** @details Применяет восьмиточечный kernel только к допущенному полному tile-интервалу. */
inline void Ppu::tile8() noexcept {
    const unsigned start = dot_ - 1;
    const std::uint16_t v = v_;
    /**
     * @brief Выставляет исходный PPU-адрес с выбранными mapper-эффектами; повторный адресный drive не удаляется.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto drive = [this](std::uint16_t value) noexcept {
        if constexpr (Addressed) {
            busAddress_ = value & 0x3FFF;
            bus_.ppuAddress(busAddress_);
        }
    };
    // Конкретные callback Machine наблюдают сигналы Cartridge, а не поля PPU.
    // Сохранить порядок каждой wired-точки/адреса/чтения; растр фиксируется атомарно.
    if constexpr (Clocked) bus_.ppuCycle();
    loadBackground();
    const auto nametable = static_cast<std::uint16_t>(0x2000 | (v & 0x0FFF));
    drive(nametable);

    if constexpr (Clocked) bus_.ppuCycle();
    const std::uint8_t tile = bus_.ppuReadFetch(nametable, PpuFetchKind::BackgroundName, (start >> 3U) + 2U);

    if constexpr (Clocked) bus_.ppuCycle();
    const auto attributeAddress = static_cast<std::uint16_t>(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
    drive(attributeAddress);

    if constexpr (Clocked) bus_.ppuCycle();
    const auto attribute = static_cast<std::uint8_t>((bus_.ppuReadFetch(attributeAddress, PpuFetchKind::BackgroundAttribute, (start >> 3U) + 2U) >> (((v >> 4) & 4) | (v & 2))) & 3);
    const auto pattern = static_cast<std::uint16_t>(((control_ & 0x10) ? 0x1000 : 0) + tile * 16 + ((v >> 12) & 7));
    drive(pattern);

    if constexpr (Clocked) bus_.ppuCycle();
    drive(pattern); // Физический повтор pattern-адреса на пятой точке.

    if constexpr (Clocked) bus_.ppuCycle();
    const std::uint8_t low = bus_.ppuReadFetch(pattern, PpuFetchKind::BackgroundPattern, (start >> 3U) + 2U);

    if constexpr (Clocked) bus_.ppuCycle();
    const auto highAddress = static_cast<std::uint16_t>(pattern + 8);
    drive(highAddress);

    if constexpr (Clocked) bus_.ppuCycle();
    const std::uint8_t high = bus_.ppuReadFetch(highAddress, PpuFetchKind::BackgroundPattern, (start >> 3U) + 2U);
    incrementX();
    coarseXSignal_ = false;
    if (start == width - 8) { scrollSignal_ = true; incrementY(); }

    nextTile_ = tile;
    nextAttribute_ = attribute;
    nextLow_ = low;
    nextHigh_ = high;
    busAddress_ = highAddress;
    pendingPixelX_ = static_cast<std::uint8_t>(start);
    pendingPixelShift_ = 0;
    pendingPixels_ = 8;
    // loadBackground выполнялся только при входе: это current-плоскости,
    // а nextLow/High выше относятся к будущей перезагрузке следующего тайла.
    flushPixels();
    pendingBackgroundShifts_ = 8;
    dot_ = static_cast<std::uint16_t>(dot_ + 8);
}
#endif

#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
[[gnu::always_inline]] inline void Ppu::tickDotsInline(unsigned dots) noexcept {
#else
/** @details Продвигает запрошенные реальные PPU-точки; события и отказы ограничивают быстрые spans. */
void Ppu::tickDots(unsigned dots) noexcept {
#endif
#if defined(NES_CORE_PPU_SPRITE_SPAN) && NES_CORE_PPU_SPRITE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    // Dot257 выполняет evaluation/scroll-copy;321 начинает background-prefetch.
    // Внутри уже прошедшего интервала остаются только упорядоченные sprite-выборки.
    if (dots && dot_ >= 258 && dot_ + dots <= 321 && renderingEnabled_ &&
        (renderDelay_ | nmiDelay_) == 0 && static_cast<unsigned>(scanline_) < height) {
        if (trackBusCycles_) spriteFetchGroup<true>(dots);
        else spriteFetchGroup<false>(dots);
        return;
    }
#endif
#if defined(NES_CORE_PPU_TILE8) && NES_CORE_PPU_TILE8 && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    if (dots >= 8 && (dot_ & 7) == 1 && dot_ <= 249 &&
        renderingEnabled_ && (renderDelay_ | nmiDelay_) == 0 &&
        static_cast<unsigned>(scanline_) < height && spriteCacheValid_) {
        // Прямые callback Machine не меняют регистры PPU. Полный видимый
        // тайл не имеет delayed/NMI-события; ограничить цикл dot256.
        unsigned tiles = dots / 8;
        const unsigned remaining = (257U - dot_) / 8;
        if (tiles > remaining) tiles = remaining;
        dots -= tiles * 8;
        if (tiles > 1) {
            if (trackBusCycles_) {
                if (trackBusAddresses_) {
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN
                    if (bus_.usesMmc3TileSignals()) fetchTileSpan<false, false, true>(tiles);
                    else
#endif
                    fetchTileSpan<true, true>(tiles);
                } else fetchTileSpan<true, false>(tiles);
            } else {
                if (trackBusAddresses_) {
#if defined(NES_CORE_LEGACY_TILE_SPAN) && NES_CORE_LEGACY_TILE_SPAN && defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
                    if (bus_.usesLegacyTileSignals(tiles, (control_ & 0x10U) != 0)) fetchLegacyTileSpan(tiles);
                    else
#endif
                    fetchTileSpan<false, true>(tiles);
                }
                else fetchTileSpan<false, false>(tiles);
            }
        } else if (trackBusCycles_) {
            if (trackBusAddresses_) {
#if defined(NES_CORE_MMC3_TILE_SPAN) && NES_CORE_MMC3_TILE_SPAN
                if (bus_.usesMmc3TileSignals()) {
                    const bool high = (control_ & 0x10) != 0;
                    do {
                        tile8<false, false>(); // Все четыре чтения памяти и растровая/выходная работа сохраняются.
                        bus_.advanceMmc3TileSignals(high);
                    } while (--tiles);
                } else
#endif
                { do { tile8<true, true>(); } while (--tiles); }
            }
            else { do { tile8<true, false>(); } while (--tiles); }
        } else {
            if (trackBusAddresses_) { do { tile8<false, true>(); } while (--tiles); }
            else { do { tile8<false, false>(); } while (--tiles); }
        }
        if (!dots) return;
    }
#endif
    // В холостых строках нет эффектов pixel/evaluation/fetch.
    // Сохранить vblank-фронт и все переходы строк на скалярном пути.
    if (scanline_ >= 240 && dots) {
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
        if (dots <= nonVisibleQuietDots()) {
#else
        if (!trackBusCycles_ && !trackBusAddresses_ && dots <= nonVisibleQuietDots()) {
#endif
            nonVisibleSpan(dots);
            return;
        }
        if (dots <= 4 && (renderDelay_ | nmiDelay_) == 0 &&
            dot_ + dots <= 340 && (scanline_ != 241 || dot_ >= 2)) {
            if (!trackBusCycles_ && !trackBusAddresses_) {
                busAddress_ = v_ & 0x3FFF;
                dot_ = static_cast<std::uint16_t>(dot_ + dots);
                return;
            }
            do {
                if (trackBusCycles_) bus_.ppuCycle();
                address(v_);
                ++dot_;
            } while (--dots);
            return;
        }
    }
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
    if (dots > 8) { tickLongDots(dots); return; }
#endif
    // Стабильные видимые интервалы имеют общую проверку диапазона/задержки. Рендер
    // сохраняет упорядоченные поточечные callback; forced blank можно свернуть
    // только если ни одна mapper-линия не наблюдает промежуточные точки.
    if (dots && dots <= 8 && (renderDelay_ | nmiDelay_) == 0 &&
        static_cast<unsigned>(scanline_) < height &&
        static_cast<unsigned>(dot_ - 1) <= width - dots) {
        if (renderingEnabled_) {
            if (trackBusCycles_) {
                do { bus_.ppuCycle(); visibleDot(); } while (--dots);
            } else {
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
                if (spriteCacheValid_ && (dots == 3 || dots == 4)) {
                    visibleUnclockedGroup(dots);
                    return;
                }
#endif
                do { visibleDot(); } while (--dots);
            }
            return;
        }
        if (!trackBusCycles_ && !trackBusAddresses_ && !coarseXSignal_ && !scrollSignal_) {
            if (dot_ <= 255 && dot_ + dots > 255) scrollSignal_ = false;
            coarseXSignal_ = false;
            flushPixels();
            if (spriteCacheValid_) materializeSprites();
            const unsigned location = (v_ & 0x3F00) == 0x3F00 ? paletteAddress(v_) : 0;
            const auto colour = static_cast<std::uint8_t>(palette_[location] & ((mask_ & 1) ? 0x30 : 0x3F));
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
            auto* output = pixels_.data();
            if(output){output += static_cast<unsigned>(scanline_) * width + dot_ - 1;
                for(unsigned i=0;i<dots;++i)output[i]=colour;}
#else
            auto* output = pixels_.data() + static_cast<unsigned>(scanline_) * width + dot_ - 1;
            for (unsigned i = 0; i < dots; ++i) output[i] = colour;
#endif
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
            if(auto* rgb=PpuRgbOutput::activeFor(this)){rgb->beginSpan(*this,static_cast<unsigned>(scanline_),dot_-1,dots,mask_>>5,lineEmphasis_[static_cast<unsigned>(scanline_)]);rgb->uniform(dot_-1,dots,colour,mask_>>5);}
#endif
            lineEmphasis_[static_cast<unsigned>(scanline_)] = static_cast<std::uint8_t>(mask_ >> 5);
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
            if(auto* rgb=PpuRgbOutput::activeFor(this))rgb->finishSpan(*this);
#endif
            busAddress_ = v_ & 0x3FFF;
            evaluationNextDot_ = static_cast<std::uint16_t>(dot_ + dots);
            dot_ = static_cast<std::uint16_t>(dot_ + dots);
            return;
        }
    }
    // Стабильные sprite/preload-интервалы видимой строки не имеют pixel/vblank/
    // scroll-copy/переходов строки. Сохранить каждый физический callback
    // и фиксацию dot в скалярном порядке, проверяя общее состояние один раз.
    // Dot257 и pre-render vertical-copy остаются на скалярном пути.
    if (dots && dots <= 4 && renderingEnabled_ && (renderDelay_ | nmiDelay_) == 0 &&
        static_cast<unsigned>(scanline_) < height) {
        if (dot_ >= 258 && dot_ + dots <= 321) {
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
            if (dots == 3 || dots == 4) {
                if (trackBusCycles_) spriteFetchGroup<true>(dots);
                else spriteFetchGroup<false>(dots);
                return;
            }
#endif
            do {
                if (trackBusCycles_) bus_.ppuCycle();
                fetchSprites();
                ++dot_;
            } while (--dots);
            return;
        }
        if (dot_ >= 321 && dot_ + dots <= 337) {
            do {
                if (trackBusCycles_) bus_.ppuCycle();
                if (((dot_ - 1) & 7) == 0) loadBackground();
                if (pendingBackgroundShifts_ < 16) ++pendingBackgroundShifts_;
                fetchBackground();
                ++dot_;
            } while (--dots);
            return;
        }
    }
    while (dots--) {
#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
        tickInline();
#else
        tick();
#endif
    }
}

#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
/**
 * @brief Продвигает PPU на указанное число уже прошедших точек.
 * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
 * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
 */
__declspec(noinline)
#endif
/** @details Разбивает длинное продвижение по исходным кадровым/строчным границам. */
void Ppu::tickLongDots(unsigned dots) noexcept {
    while (dots) {
        unsigned group = dots < 4 ? dots : 4;
        const bool stable = (renderDelay_ | nmiDelay_) == 0;
        if (stable && static_cast<unsigned>(scanline_) < height) {
            if (dot_ == 0 || dot_ == 257) group = 1;
            else if (dot_ <= 256) {
                const unsigned toEnd = 257U - dot_;
                group = dots < toEnd ? dots : toEnd;
#if defined(NES_CORE_PPU_TILE8) && NES_CORE_PPU_TILE8
                if (renderingEnabled_ && spriteCacheValid_) {
                    const unsigned prefix = deferredPrefix();
                    if (prefix) group = group < prefix ? group : prefix;
                    else if (group >= 8) group &= ~7U;
                } else
#endif
                if (group > 8) group = 8;
            } else if (dot_ < 321) {
                const unsigned toEnd = 321U - dot_;
#if defined(NES_CORE_PPU_SPRITE_SPAN) && NES_CORE_PPU_SPRITE_SPAN
                if (renderingEnabled_ && dot_ >= 258) group = dots < toEnd ? dots : toEnd;
                else
#endif
                if (group > toEnd) group = toEnd;
            } else if (dot_ < 337) {
                const unsigned toEnd = 337U - dot_;
                if (group > toEnd) group = toEnd;
            } else {
                const unsigned toEnd = 341U - dot_;
                if (group > toEnd) group = toEnd;
            }
        } else if (scanline_ >= 240) {
            const unsigned blank = nonVisibleQuietDots();
            if (blank) group = dots < blank ? dots : blank;
            else group = 1;
        }
        // Длинные группы используются лишь если существующая tile/stable-blank
        // проверка поглотит их до повторного входа в helper. Другие
        // куски сохраняют принятый 3/4-dot sprite/preload или скалярный путь.
        tickDots(group);
        dots -= group;
    }
}
#endif

#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#elif defined(_MSC_VER)
/**
 * @brief Продвигает PPU на указанное число уже прошедших точек.
 * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
 * @param dots Число уже прошедших точек PPU.
 */
__declspec(noinline)
#endif
/** @details Продвигает запрошенные реальные PPU-точки; события и отказы ограничивают быстрые spans. */
void Ppu::tickDots(unsigned dots) noexcept { tickDotsInline(dots); }
#endif

#if defined(NES_CORE_INLINE_PPU) && NES_CORE_INLINE_PPU
// Сохранить публичную out-of-line точку входа для вызывающих вне Machine.
/**
 * @brief Продвигает устройство на одну его логическую единицу времени.
 * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
 */
void Ppu::tick() noexcept { tickInline(); }
#endif

}  // пространство имён nes::core
