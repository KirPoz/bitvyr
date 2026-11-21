#include "Mmc5HeadForecast.h"
/**
 * @file TimingPpu.cpp
 * @brief CPU-наблюдаемый временной PPU и точные packet-события выбранного владельца.
 * @ingroup nes_ppu
 */
#include "TimingPpu.h"
#include "ThinOamElide.h"
#include "../Machine.h"
#include "../cartridge/Mmc5.h"
#include <algorithm>
#include <type_traits>

namespace nes::core {

/** @brief Выбирает side-effectful MMC5 fetch без обещания тихих интервалов. */
bool TimingPpu::mmc5Fetches() const noexcept { return bus_.cartridge_.mapper()==5; }
/** @brief Сохраняет будущую экранную колонку независимо от loopy scroll. */
unsigned TimingPpu::backgroundFetchTileX() const noexcept {
    return dot_>=321U?((dot_-321U)>>3U):((dot_-1U)>>3U)+2U;
}

#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
/**
 * @brief Проверяет право основного владельца не формировать ненаблюдаемые видеоданные.
 * @details Не расширяет поддерживаемые mapper и не относится к копиям, чужим PPU, временным захватам либо активному потребителю пакета.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
inline bool TimingPpu::elideCaptureData() const noexcept {
    // Без второго обхода предикатов Machine; копии/вспомогательные вызывающие и локальный
    // observer-capture сохраняют все исходные байты метаданных.
    return (bus_.videoPolicy_&2U)&&this==&bus_.timingPpu_&&!packet_&&!capture_;
}

/** @details Единое внешнее определение нужно также отдельному raster TU:
 * event() может сохранить этот вызов после оптимизации. Зависимость от полей
 * Machine не позволяет определить predicate в заголовке с неполным типом. */
bool TimingPpu::outputSpriteRows() const noexcept {
    return this==&bus_.timingPpu_&&packet_&&packet_==bus_.videoPacket_&&!capture_;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 */
void TimingPpu::reset(bool pal) noexcept {
    mmc5Lazy_ = Mmc5LazyState{};
    mmc5LazyCounts_ = Mmc5LazyCounts{};
    invalidateMmc5RasterPlan();
    capture_=nullptr;
    pal_=pal; pendingPixels_=0; pendingFill_=false;
    palette_.fill(0);oam_.fill(0xFF);secondary_.fill(0xFF);secondaryIndex_.fill(0xFF);
    for(auto& bucket:oamYBuckets_)bucket.fill(0);
    oamYBuckets_[255].fill(0xFFFFFFFFU);
    oamYLow_=oamYHigh_=0;oamYRow_=-1;oamYHeight_=0;oamYDirty_=true;
    sprites_.fill(Sprite{});spriteRow_.fill(0);spriteCacheClocks_=0;spriteCacheValid_=false;spriteZeroRow_=false;
    control_=mask_=status_=oamAddress_=openBus_=readBuffer_=fineX_=0;
    v_=t_=busAddress_=0;writeToggle_=renderingEnabled_=renderingNext_=false;renderDelay_=0;
    backgroundAtoms_.fill(VideoByteRef::literal);spriteZeroMask_.fill(0);
    pendingBackgroundShifts_=0;nextTile_=nextAttribute_=0;
    evaluateN_=evaluateM_=secondaryPosition_=overflowFetches_=0;evaluateLatch_=0xFF;
    evaluationDone_=false;evaluationNextDot_=257;scanline_=-1;dot_=0;frames_=0;
    coarseXSignal_=scrollSignal_=false;
    odd_=frameReady_=nmiLine_=nmiPending_=suppressVblank_=false;nmiDelay_=0;
}
/**
 * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
 * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned TimingPpu::quietDots() const noexcept {
    if(renderDelay_|nmiDelay_)return 0;
    if(scanline_>=0&&scanline_<=240)return std::min(65535U,(241U-unsigned(scanline_))*341U+1U-dot_);
    if(scanline_==241&&dot_<=1)return 1U-dot_;
    return 65535;
}
/**
 * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
 * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned TimingPpu::nonVisibleQuietDots() const noexcept {
    const int last=pal_?310:260;
    if(scanline_<240||scanline_>last||(renderDelay_|nmiDelay_)||pendingPixels_||coarseXSignal_||scrollSignal_||busAddress_!=(v_&0x3FFF))return 0;
    // Все строки свернутого интервала должны оставаться безопасными холостыми строками.
    // В частности, внутренняя холостая строка НЕ может перескочить pre-render.
    unsigned d=(unsigned(last-scanline_)+1U)*341U-1U-dot_;
    if(scanline_==240)d=342U-dot_;
    else if(scanline_==241&&dot_<2)d=1U-dot_;
    else if(scanline_==last)d=340U-dot_;
    return d;
}
/**
 * @brief Публикует текущий адрес на шине PPU.
 * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void TimingPpu::address(std::uint16_t value) noexcept {
    busAddress_ = value & 0x3FFF;
    if (trackBusAddresses_) bus_.ppuAddress(busAddress_);
}

/**
 * @brief Вычисляет физический индекс с правилами зеркалирования PPU.
 * @details Учитывает таблицы имён либо специальные зеркала палитры; сам адрес не выдаёт строб чтения.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned TimingPpu::paletteAddress(std::uint16_t value) noexcept {
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
std::uint8_t TimingPpu::read(std::uint16_t value) noexcept {
    address(value);
    if (busAddress_ >= 0x3F00) return palette_[paletteAddress(busAddress_)];
    return bus_.ppuRead(busAddress_);
}

/**
 * @brief Продвигает coarse X и переключает горизонтальную таблицу имён при wrap.
 * @details Остальные поля loopy v сохраняются.
 */
void TimingPpu::incrementX() noexcept {
    if ((v_ & 0x001F) == 31) v_ = (v_ & 0x7FE0) ^ 0x0400;
    else ++v_;
}

/**
 * @brief Продвигает fine/coarse Y по правилам видимых и специальных строк PPU.
 * @details Строки 29/31 и переключение вертикальной таблицы имён не заменяются обычным сложением адреса.
 */
void TimingPpu::incrementY() noexcept {
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
void TimingPpu::incrementDataAddress() noexcept {
    if (rendering() && renderLine()) { incrementX(); incrementY(); }
    else v_ = (v_ + ((control_ & 4) ? 32 : 1)) & 0x7FFF;
    if (!(rendering() && renderLine())) address(v_);
}

/**
 * @brief Обновляет линию/задержку NMI из VBlank и PPUCTRL.
 * @details Сохраняет подавление и delayed-переходы около реального STATUS-чтения.
 */
void TimingPpu::updateNmi() noexcept {
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
bool TimingPpu::takeNmi() noexcept {
    const bool pending = nmiPending_;
    nmiPending_ = false;
    return pending;
}

/**
 * @brief Читает CPU-видимый статус каналов и подтверждает предусмотренный frame-IRQ.
 * @details Семантика отличается от peekStatus: побочный эффект чтения должен попасть в соответствующий пакет.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t TimingPpu::readStatus() noexcept {
    openBus_ = (status_ & 0xE0) | (openBus_ & 0x1F);
    if (scanline_ == 241 && dot_ == 1) suppressVblank_ = true;
    status_ &= 0x7F;
    writeToggle_ = false;
    if (scanline_ == 241 && dot_ <= 3) nmiPending_ = false;
    updateNmi();
    return openBus_;
}
/**
 * @brief Классифицирует STATUS по единой границе строки, Sprite0 и OAM.
 * @details Eval означает только необходимость оценки; право менять основной PPU проверяет вызывающий read-путь.
 */
TimingPpu::StatusReadClassification TimingPpu::classifyDeferredStatus(unsigned dots) const noexcept {
    constexpr StatusReadClassification blocked{StatusReadKind::Blocked, 0U};
    constexpr StatusReadClassification stable{StatusReadKind::Stable, 0U};
    if (static_cast<unsigned>(scanline_) >= 240U || dot_ < 1U || dot_ > 340U ||
        dots >= 341U - dot_ || (renderDelay_ | nmiDelay_) || (status_ & 0x80U)) {
        return blocked;
    }
    // Во всём нематериализованном интервале не должно быть возможного Sprite0-hit.
    if (!(status_ & 0x40U) && dots && dot_ <= 255U && renderingEnabled_ && (mask_ & 0x18U) == 0x18U) {
        if (!spriteCacheValid_) return blocked;
        if (spriteZeroRow_) {
            unsigned first = dot_ - 1U;
            if ((mask_ & 6U) != 6U) first = std::max(first, 8U);
            const unsigned last = std::min(dot_ + dots - 2U, 254U);
            for (unsigned x = first; x <= last; x = (x & ~7U) + 8U) {
                const unsigned hi = std::min(last, (x & ~7U) + 7U) & 7U;
                const unsigned mask = (0xFFU << (x & 7U)) & (0xFFU >> (7U - hi));
                if (spriteZeroMask_[x >> 3U] & mask) return blocked;
            }
        }
    }
    if (status_ & 0x20U) return stable;
    const unsigned until = std::min(256U, dot_ + dots - 1U);
    const unsigned next = evaluationNextDot_;
    if (next > until) return stable;
    if (!next) return blocked;
    // Next1 заново очищает secondary OAM: прежним Done/position до reset доверять нельзя.
    if (next > 1U && evaluationDone_) return stable;
    const unsigned position = next == 1U ? 0U : secondaryPosition_;
    const unsigned firstEven = next <= 64U ? 66U : next + (next & 1U);
    const unsigned count = until < firstEven ? 0U : 1U + (until - firstEven) / 2U;
    // Последний заполняющий secondary байт ещё не устанавливает overflow.
    if (!count || (position <= 32U && 32U - position >= count)) return stable;
    return {StatusReadKind::Eval, until};
}

/**
 * @brief Разрешает сворачивание только при неизменном STATUS и прежних защёлках.
 * @details Eval не исполняется внутри const-поиска горизонта повторов.
 */
bool TimingPpu::canRepeatStatusPoll(unsigned dots) const noexcept {
    return classifyDeferredStatus(dots).kind == StatusReadKind::Stable && !(status_ & 0x40U) &&
        !writeToggle_ && !nmiLine_ && !nmiPending_ &&
        (openBus_ & 0xE0U) == (status_ & 0xE0U);
}

/**
 * @brief Читает STATUS после одной общей проверки, не закрывая допустимый растровый долг.
 * @details Отдельная OAM-оценка остаётся ограниченной прошлым временем той же строки.
 */
bool TimingPpu::tryReadDeferredStatus(unsigned dots, std::uint8_t& value) noexcept {
    const auto access = classifyDeferredStatus(dots);
    if (access.kind == StatusReadKind::Blocked) return false;
    if (access.kind == StatusReadKind::Eval) {
        // Dot1/reset, forced blank и чужой/наблюдаемый PPU сохраняют исходный полный путь.
        if (this != &bus_.timingPpu_ || !bus_.outputOwner_ || !bus_.outputFrameExecutable() ||
            !bus_.pureStatusPollContract() || !renderingEnabled_ || dot_ < 2U || !dots) {
            return false;
        }
        synchronizeSpriteEvaluation(access.evalUntil);
        // Оценка меняет только OAM/status5 и ставит next после evalUntil.
        // Строка, delays и Sprite0-кэш прежние: второй scan/классификация не нужны.
    }
    value = readStatus();
    return true;
}

/**
 * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
 * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t TimingPpu::cpuRead(std::uint16_t reg) noexcept {
    flushPixels();
    if ((reg & 7) == 2 || (reg & 7) == 4) synchronizeSprites();
    switch (reg & 7) {
    case 2:
        return readStatus();
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
void TimingPpu::dmaWrite(std::uint8_t value) noexcept {
    // Исходный DMA byte уже принадлежит отдельному C0 событию.
    // Primary Signals сохраняет wrap адреса, а не вторую ненаблюдаемую OAM.
    if (NES_PRIMARY_MMC5_OAM_UNUSED()) { ++oamAddress_; return; }
    flushPixels();
    synchronizeSprites();
    if ((oamAddress_ & 3) == 2) value &= 0xE3;
    // Прежняя неполная оценка/защёлка синхронизирована до этой настоящей записи.
    if(!(oamAddress_&3U)&&oam_[oamAddress_]!=value){
        const unsigned slot=oamAddress_>>2,word=slot>>5;
        const auto bit=std::uint32_t(1)<<(slot&31U);
        oamYBuckets_[oam_[oamAddress_]][word]&=~bit;
        oamYBuckets_[value][word]|=bit;oamYDirty_=true;
    }
    oam_[oamAddress_++] = value;
}

/**
 * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
 * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void TimingPpu::cpuWrite(std::uint16_t reg, std::uint8_t value) noexcept {
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
 * @brief Доводит текущие sprite-плоскости и счётчики X до нужной позиции.
 * @details Накопленные сдвиги и различающиеся моменты low/high-fetch учитываются до использования метаданных.
 */
void TimingPpu::synchronizeSprites() noexcept {
    // STATUS/OAMDATA возвращает C0; primary latch в Signals не наблюдается.
    if (NES_PRIMARY_MMC5_OAM_UNUSED()) return;
    if (dot_) synchronizeSpriteEvaluation(dot_ - 1);
}

namespace {
// Ненулевой 32-битный вход; ограниченная иерархия избегает RV32 ROM CTZ/64-битных helper.
/**
 * @brief Находит первый установленный бит непустой маски OAM-кандидатов.
 * @details Нулевую маску вызывающий обязан обработать до поиска; порядок соответствует исходному обходу OAM.
 * @param word Ненулевое 32-битное множество OAM-кандидатов для поиска первого установленного бита.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
inline unsigned firstOamYBit(std::uint32_t word) noexcept {
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
unsigned TimingPpu::invalidYRun(unsigned n,unsigned limit,unsigned height) noexcept {
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
        if(word)valid=firstOamYBit(word);
        else if(high)valid=32U+firstOamYBit(high);
    }else{
        const auto word=high&(0xFFFFFFFFU<<(n-32U));
        if(word)valid=32U+firstOamYBit(word);
    }
    return std::min(limit,valid-n);
}

/**
 * @brief Доводит аппаратный обход OAM до текущей растровой точки.
 * @details Сохраняет odd/even-защёлку, очистку secondary OAM, byte-позиции и диагональное переполнение, включая частичные срезы.
 * @param until Последняя включённая PPU-точка evaluation; ограничивается 256, частичная odd/even-защёлка сохраняется.
 */
void TimingPpu::synchronizeSpriteEvaluation(unsigned until) noexcept {
    // У единственного Signals-владельца secondary OAM/overflow считает C0.
    if (NES_PRIMARY_MMC5_OAM_UNUSED()) return;
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
#if defined(NES_EXPERIMENTAL_OAM_TERMINAL_MASK) && NES_EXPERIMENTAL_OAM_TERMINAL_MASK
    // До первого наблюдения прошёл весь evaluation-интервал. При <8 Y-кандидатах
    // не возникает diagonal-overflow: полный обход 64 занимает не более 234dots.
    // Точный terminal latch после done — Y спрайта 0, но secondary хвост хранит
    // Y последнего отвергнутого 63-го спрайта, если он не был выбран.
    if(next==1U&&until==256U){
        const unsigned height=(control_&0x20U)?16U:8U;
        (void)invalidYRun(0,63,height); // Единственный существующий builder Y-mask.
        auto low=oamYLow_,high=oamYHigh_;
        std::array<std::uint8_t,8> selected{};unsigned count=0;
        while((low||high)&&count<8U){
            unsigned index=0;
            if(low){index=firstOamYBit(low);low&=low-1U;}
            else{index=32U+firstOamYBit(high);high&=high-1U;}
            selected[count++]=static_cast<std::uint8_t>(index);
        }
        if(count<8U){
            secondary_.fill(0xffU);
            for(unsigned i=0;i<count;++i){
                const unsigned n=selected[i];secondaryIndex_[i]=static_cast<std::uint8_t>(n);
                for(unsigned byte=0;byte<4U;++byte)secondary_[i*4U+byte]=oam_[n*4U+byte];
            }
            if(!(oamYHigh_&0x80000000U))secondary_[count*4U]=oam_[63U*4U];
            evaluateN_=evaluateM_=overflowFetches_=0;
            secondaryPosition_=static_cast<std::uint8_t>(count*4U);
            evaluateLatch_=oam_[0];evaluationDone_=true;evaluationNextDot_=257;
            return;
        }
    }
#endif
    if (next <= 64) {
        const unsigned lastClear = until < 64 ? until : 64;
        for (unsigned even = next + (next & 1); even <= lastClear; even += 2)
            secondary_[even / 2 - 1] = 0xFF;
        next = lastClear + 1;
    }
    const int spriteHeight = (control_ & 0x20) ? 16 : 8;
    while (next <= until) {
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
std::uint8_t TimingPpu::reverse(std::uint8_t value) noexcept {
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
std::uint16_t TimingPpu::spritePattern(unsigned slot) const noexcept {
    // MMC5 детектор различает NT equality и nonNT, но не pattern tile bits.
    if (NES_PRIMARY_MMC5_OAM_UNUSED()) return 0;
    unsigned row = static_cast<std::uint8_t>(scanline_ - secondary_[slot * 4]);
    const unsigned heightPixels = (control_ & 0x20) ? 16 : 8;
    row &= heightPixels - 1;
    if (secondary_[slot * 4 + 2] & 0x80) row ^= heightPixels - 1;
    const unsigned tile = secondary_[slot * 4 + 1];
    if (heightPixels == 16) return static_cast<std::uint16_t>((tile & 1) * 0x1000 + (tile & 0xFE) * 16 + (row & 7) + ((row & 8) ? 16 : 0));
    return static_cast<std::uint16_t>(((control_ & 8) ? 0x1000 : 0) + tile * 16 + row);
}

/**
 * @brief Строит текущий кэш выбора спрайтов для видимой строки.
 * @details Порядок OAM и окклюзия ранними спрайтами сохраняются; отдельная маска Sprite0 не разрешает менять выигравший пиксель.
 */
void TimingPpu::buildSpriteRow() noexcept {
    flushPixels();
    // Только непрозрачность выигравшего Sprite0 видна CPU. Рендерер декодирует все
    // восемь colour/priority-строк из неизменных выбранных sprite-записей.
    spriteZeroMask_.fill(0);
    unsigned lastZero=8;
    for(unsigned i=0;i<sprites_.size();++i)
        if(sprites_[i].valid&&sprites_[i].index==0)lastZero=i;
    spriteZeroRow_=lastZero<8;
    if(lastZero==0){
        const auto& sprite=sprites_[0];
        const unsigned opaque=reverse(static_cast<std::uint8_t>(sprite.low|sprite.high));
        const unsigned byte=sprite.x>>3,shift=sprite.x&7U;
        spriteZeroMask_[byte]=static_cast<std::uint8_t>(opaque<<shift);
        if(shift&&byte<31)spriteZeroMask_[byte+1]=static_cast<std::uint8_t>(opaque>>(8U-shift));
    }else if(lastZero<8){
        // Редкие переставленные/повторные индексы сохраняют правило первой непрозрачной единицы.
        // Поздние ненулевые единицы не скрывают уже выигравший Sprite0-пиксель.
        std::array<std::uint8_t,32> covered{};
        for(unsigned i=0;i<=lastZero;++i){
            const auto& sprite=sprites_[i];if(!sprite.valid)continue;
            const unsigned opaque=reverse(static_cast<std::uint8_t>(sprite.low|sprite.high));
            if(!opaque)continue;
            const unsigned byte=sprite.x>>3,shift=sprite.x&7U;
            const unsigned first=(opaque<<shift)&255U;
            if(sprite.index==0)spriteZeroMask_[byte]|=static_cast<std::uint8_t>(first&~covered[byte]);
            covered[byte]|=static_cast<std::uint8_t>(first);
            if(shift&&byte<31){
                const unsigned second=opaque>>(8U-shift);
                if(sprite.index==0)spriteZeroMask_[byte+1]|=static_cast<std::uint8_t>(second&~covered[byte+1]);
                covered[byte+1]|=static_cast<std::uint8_t>(second);
            }
        }
    }
    spriteCacheClocks_ = 0;
    spriteCacheValid_ = true;
}

/**
 * @brief Записывает байт через протокол устройства или шины.
 * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
 * @param location Нормализованный физический индекс CIRAM с учётом nametable-алиасов, а не логический CPU-адрес.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void TimingPpu::write(std::uint16_t location,std::uint8_t value) noexcept {
    address(location);
    if(busAddress_>=0x3F00){
        const auto index=paletteAddress(busAddress_);palette_[index]=value&63;
        event(VideoOp::PaletteWrite,index,0,0,palette_[index]);
    }else bus_.ppuWrite(busAddress_,value);
}
/**
 * @brief Материализует ранее сохранённый байт паттерна по его виду и смещению.
 * @details Работает с буквальным значением, CHR-ROM либо защищённой CHR-RAM-идентичностью; недопустимая ссылка является ошибкой.
 * @param slot Индекс элемента текущей таблицы/буфера; диапазон задаётся этим типом.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t TimingPpu::resolveAtom(unsigned slot) noexcept {
    const auto reference=backgroundAtoms_[slot];
    if((reference&VideoByteRef::kindMask)==VideoByteRef::literal)return static_cast<std::uint8_t>(reference);
    const auto value=bus_.resolveVideoPattern(reference);
    // Неразрешённые RAM-идентичности равны текущему физическому байту: COW
    // фиксирует все старые версии до записи. Зафиксировать дубликаты вместе.
    for(auto& atom:backgroundAtoms_)if(atom==reference)atom=VideoByteRef::literal|value;
    return value;
}
/**
 * @brief Переводит отложенные ссылки конвейера в независимые значения перед сменой backing-памяти.
 * @details Порядок важен при загрузке, отказе загрузки, reset и CHR-RAM copy-on-write.
 */
void TimingPpu::materializeBackgroundRefs() noexcept {
    for(unsigned slot=0;slot<backgroundAtoms_.size();++slot)static_cast<void>(resolveAtom(slot));
}
/**
 * @brief Замораживает живые ссылки перед изменением CHR-RAM.
 * @details Старый байт сохраняется до записи, чтобы отложенный конвейер не увидел будущее значение.
 * @param offset Смещение внутри соответствующего физического диапазона.
 * @param oldValue Исторический CHR-RAM-байт до настоящей записи; COW фиксирует все ещё заимствованные версии.
 */
void TimingPpu::beforeChrRamWrite(unsigned offset,std::uint8_t oldValue) noexcept {
    const auto reference=VideoByteRef::chrRam|offset;
    for(auto& atom:backgroundAtoms_)if(atom==reference)atom=VideoByteRef::literal|oldValue;
}
/**
 * @brief Вставляет следующие байты/идентичности фона в текущие сдвиговые регистры.
 * @details Частичные сдвиги и независимые high/low-ссылки не заменяются предположением о полном восьмибитном сдвиге.
 */
void TimingPpu::loadBackgroundAtoms() noexcept {
    const unsigned shifts=pendingBackgroundShifts_;
    for(unsigned base:{unsigned(LowHigh),unsigned(HighHigh)}){
        std::uint32_t high;
        if(!shifts)high=backgroundAtoms_[base];
        else if(shifts==8)high=backgroundAtoms_[base+1];
        else if(shifts>=16)high=VideoByteRef::literal;
        else{
            // Редкий неполный gated-сдвиг может смешать фрагменты двух старых байтов.
            // Материализовать лишь участвующие старые байты, не next-байт.
            const unsigned low=resolveAtom(base+1);
            const unsigned oldHigh=shifts<8?resolveAtom(base):0;
            high=VideoByteRef::literal|(((((oldHigh<<8)|low)<<shifts)>>8)&255U);
        }
        backgroundAtoms_[base]=high;
        backgroundAtoms_[base+1]=backgroundAtoms_[base+2];
    }
    pendingBackgroundShifts_=0;
}
/**
 * @brief Проверяет непрозрачность фоновой позиции, необходимой Sprite0.
 * @details Материализует лишь нужные плоскости; атрибут палитры не влияет на сам факт непрозрачности.
 * @param shift Смещение проверяемой фоновой точки 0..15 в текущей паре байтов; >=16 означает отсутствие непрозрачности.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool TimingPpu::backgroundOpaque(unsigned shift) noexcept {
    if(shift>=16)return false;
    const unsigned bit=0x80U>>(shift&7U),slot=shift<8?LowHigh:LowLow;
    return (resolveAtom(slot)&bit)!=0||(resolveAtom(slot+3)&bit)!=0;
}
/**
 * @brief Вставляет следующие байты/идентичности фона в текущие сдвиговые регистры.
 * @details Частичные сдвиги и независимые high/low-ссылки не заменяются предположением о полном восьмибитном сдвиге.
 */
void TimingPpu::loadBackground() noexcept {
    flushPixels();
    loadBackgroundAtoms();event(VideoOp::BgLoad);
}
/**
 * @brief Обслуживает точную фазу фонового конвейера PPU.
 * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
 */
template<unsigned Phase>
inline void TimingPpu::fetchBackgroundPhase() noexcept {
    static_assert(Phase<8,"one tile phase");
    // MMC5 возвращает настоящий byte один раз; immutable literal не зависит от N+1 banks.
    if(mmc5Fetches()){
        if constexpr(Phase==0)address(0x2000|(v_&0x0FFF));
        else if constexpr(Phase==1)nextTile_=bus_.ppuReadFetch(busAddress_,PpuFetchKind::BackgroundName,backgroundFetchTileX());
        else if constexpr(Phase==2)address(0x23C0|(v_&0x0C00)|((v_>>4)&0x38)|((v_>>2)&7));
        else if constexpr(Phase==3){
            const unsigned shift=((v_>>4)&4)|(v_&2);
            const auto value=bus_.ppuReadFetch(busAddress_,PpuFetchKind::BackgroundAttribute,backgroundFetchTileX());
            nextAttribute_=(value>>shift)&3;
            event(VideoOp::BgAttribute,shift,0,0,VideoByteRef::literal|value);
            address(((control_&0x10)?0x1000:0)+nextTile_*16+((v_>>12)&7));
        }else if constexpr(Phase==4)address(((control_&0x10)?0x1000:0)+nextTile_*16+((v_>>12)&7));
        else if constexpr(Phase==5){
            const auto value=bus_.ppuReadFetch(busAddress_,PpuFetchKind::BackgroundPattern,backgroundFetchTileX());
            backgroundAtoms_[LowNext]=VideoByteRef::literal|value;
            event(VideoOp::BgLow,0,0,0,VideoByteRef::literal|value);
        }else if constexpr(Phase==6){coarseXSignal_=true;address(busAddress_+8);}
        else if constexpr(Phase==7){
            const auto value=bus_.ppuReadFetch(busAddress_,PpuFetchKind::BackgroundPattern,backgroundFetchTileX());
            backgroundAtoms_[HighNext]=VideoByteRef::literal|value;
            event(VideoOp::BgHigh,0,0,0,VideoByteRef::literal|value);if(coarseXSignal_)incrementX();coarseXSignal_=false;
        }
        return;
    }
    if constexpr(Phase==0)address(0x2000|(v_&0x0FFF));
    else if constexpr(Phase==1)nextTile_=bus_.ppuRead(busAddress_);
    else if constexpr(Phase==2)address(0x23C0|(v_&0x0C00)|((v_>>4)&0x38)|((v_>>2)&7));
    else if constexpr(Phase==3){
        if(elideCaptureData()){
            // Значение питает только цветовые метаданные. Сохранить исходный read-
            // строб в этой фазе; CPU PPUDATA-чтения используют неизменённый путь.
            bus_.videoReadStrobeOnly();nextAttribute_=0;
        }else{
            const unsigned shift=((v_>>4)&4)|(v_&2);const auto ref=bus_.videoReference(busAddress_);
            nextAttribute_=(bus_.ppuRead(busAddress_)>>shift)&3;event(VideoOp::BgAttribute,shift,0,0,ref);
        }
        address(((control_&0x10)?0x1000:0)+nextTile_*16+((v_>>12)&7));
    }else if constexpr(Phase==4)address(((control_&0x10)?0x1000:0)+nextTile_*16+((v_>>12)&7));
    else if constexpr(Phase==5){const auto ref=bus_.videoReference(busAddress_);
        backgroundAtoms_[LowNext]=bus_.captureVideoPattern(busAddress_,ref);
        event(VideoOp::BgLow,0,0,0,ref);}
    else if constexpr(Phase==6){coarseXSignal_=true;address(busAddress_+8);}
    else if constexpr(Phase==7){const auto ref=bus_.videoReference(busAddress_);
        backgroundAtoms_[HighNext]=bus_.captureVideoPattern(busAddress_,ref);
        event(VideoOp::BgHigh,0,0,0,ref);if(coarseXSignal_)incrementX();coarseXSignal_=false;}
}
/**
 * @brief Выполняет предусмотренную текущей точкой загрузку фонового конвейера.
 * @details Не считает текущий адрес автоматически CHR: после переходов rendering может сохраняться иной шинный адрес.
 */
void TimingPpu::fetchBackground() noexcept {
    switch((dot_-1)&7){
    case 0:fetchBackgroundPhase<0>();break;
    case 1:fetchBackgroundPhase<1>();break;
    case 2:fetchBackgroundPhase<2>();break;
    case 3:fetchBackgroundPhase<3>();break;
    case 4:fetchBackgroundPhase<4>();break;
    case 5:fetchBackgroundPhase<5>();break;
    case 6:fetchBackgroundPhase<6>();break;
    case 7:fetchBackgroundPhase<7>();break;
    }
}
/**
 * @brief Публикует метаданные конкретного sprite-слота после нужной фазы.
 * @details Не перепривязывает старую low/high-плоскость к новому flip, X либо номеру Sprite0.
 * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
 */
void TimingPpu::loadSpriteMetadata(unsigned slot) noexcept {
    oamAddress_=0;flushPixels();auto& sprite=sprites_[slot];
    sprite.x=secondary_[slot*4+3];sprite.attributes=secondary_[slot*4+2];sprite.index=secondaryIndex_[slot];
    const int row=scanline_-secondary_[slot*4];
    sprite.valid=scanline_>=0&&sprite.index!=0xFF&&row>=0&&row<((control_&0x20)?16:8);
}
/**
 * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
 * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
 * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
 * @param pattern Адрес паттерна текущей плоскости в момент fetch.
 */
template<unsigned Phase,bool CachedPattern>
inline void TimingPpu::fetchSpritePhase(unsigned slot,std::uint16_t pattern) noexcept {
    static_assert(Phase<8,"one sprite phase");oamAddress_=0;auto& sprite=sprites_[slot];
    // Dummy и обе плоскости остаются настоящими /RD даже для невалидного sprite slot.
    if(mmc5Fetches()){
        if constexpr(Phase==0){
            loadSpriteMetadata(slot);
            event(VideoOp::SpriteMeta,slot,0,0,unsigned(sprite.x)|(unsigned(sprite.attributes)<<8)|(unsigned(sprite.index)<<16)|(unsigned(sprite.valid)<<24));
            address(0x2000|(v_&0x0FFF));
        }else if constexpr(Phase==1)(void)bus_.ppuReadFetch(busAddress_,PpuFetchKind::DummyName,0);
        else if constexpr(Phase==2)address(0x2000|(v_&0x0FFF));
        else if constexpr(Phase==3){(void)bus_.ppuReadFetch(busAddress_,PpuFetchKind::DummyName,0);address(CachedPattern?pattern:spritePattern(slot));}
        else if constexpr(Phase==4)address(CachedPattern?pattern:spritePattern(slot));
        else if constexpr(Phase==5){
            const auto value=bus_.ppuReadFetch(busAddress_,PpuFetchKind::SpritePattern,0);
            sprite.low=(sprite.attributes&0x40)?reverse(value):value;
            event(VideoOp::SpriteLow,slot,0,0,VideoByteRef::literal|value);
        }else if constexpr(Phase==6)address((CachedPattern?pattern:spritePattern(slot))+8);
        else if constexpr(Phase==7){
            const auto value=bus_.ppuReadFetch(busAddress_,PpuFetchKind::SpritePattern,0);
            sprite.high=(sprite.attributes&0x40)?reverse(value):value;
            event(VideoOp::SpriteHigh,slot,0,0,VideoByteRef::literal|value);
        }
        return;
    }
    if constexpr(Phase==0){
        loadSpriteMetadata(slot);
        if(!elideCaptureData())event(VideoOp::SpriteMeta,slot,0,0,unsigned(sprite.x)|(unsigned(sprite.attributes)<<8)|(unsigned(sprite.index)<<16)|(unsigned(sprite.valid)<<24));
        address(0x2000|(v_&0x0FFF));
    }else if constexpr(Phase==1)(void)bus_.ppuRead(busAddress_);
    else if constexpr(Phase==2)address(0x2000|(v_&0x0FFF));
    else if constexpr(Phase==3){(void)bus_.ppuRead(busAddress_);address(CachedPattern?pattern:spritePattern(slot));}
    else if constexpr(Phase==4)address(CachedPattern?pattern:spritePattern(slot));
    else if constexpr(Phase==5){
        const bool elide=elideCaptureData();
        const auto ref=elide?0U:bus_.videoReference(busAddress_);
        const auto value=bus_.ppuRead(busAddress_);
        sprite.low=(sprite.attributes&0x40)?reverse(value):value;
        if(!elide)event(VideoOp::SpriteLow,slot,0,0,ref);
    }
    else if constexpr(Phase==6)address((CachedPattern?pattern:spritePattern(slot))+8);
    else if constexpr(Phase==7){
        const bool elide=elideCaptureData();
        const auto ref=elide?0U:bus_.videoReference(busAddress_);
        const auto value=bus_.ppuRead(busAddress_);
        sprite.high=(sprite.attributes&0x40)?reverse(value):value;
        if(!elide)event(VideoOp::SpriteHigh,slot,0,0,ref);
    }
}
/**
 * @brief Загружает спрайтовое состояние следующей строки в исходном порядке слотов.
 * @details Число видимых спрайтов не отменяет шинные обращения пустых слотов.
 */
void TimingPpu::fetchSprites() noexcept {
    if(dot_==257)synchronizeSpriteEvaluation(256);
    const unsigned slot=(dot_-257)/8;
    switch((dot_-257)&7){
    case 0:fetchSpritePhase<0>(slot);break;
    case 1:fetchSpritePhase<1>(slot);break;
    case 2:fetchSpritePhase<2>(slot);break;
    case 3:fetchSpritePhase<3>(slot);break;
    case 4:fetchSpritePhase<4>(slot);break;
    case 5:fetchSpritePhase<5>(slot);break;
    case 6:fetchSpritePhase<6>(slot);break;
    case 7:fetchSpritePhase<7>(slot);break;
    }
}

// Группа не пересекает CPU/MMIO, задержки и границы растровых эффектов.
// Входные read-фазы сохраняют busAddress_ предшествующего CPU-интервала.
// Повтор pattern допустим лишь внутри одного локального sprite-слота при стабильных OAM/CTRL.
/**
 * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
 * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
 * @param slot Текущий sprite-слот 0..7, обновляемый при переходе восьмиточечной группы.
 * @param cachedSlot Индекс слота локального pattern-кэша; не сохраняется через CPU/MMIO или смену OAM/CTRL.
 * @param pattern Локальный вычисленный pattern-адрес спрайта; действителен только для соответствующего cached-slot.
 */
template<bool Clocked,unsigned Phase>
inline void TimingPpu::spriteFetchDot(unsigned&slot,unsigned&cachedSlot,std::uint16_t&pattern) noexcept {
    if constexpr(Clocked)bus_.ppuCycle();
    if constexpr(Phase==3||Phase==4||Phase==6){if(cachedSlot!=slot){pattern=spritePattern(slot);cachedSlot=slot;}}
    fetchSpritePhase<Phase,true>(slot,pattern);++dot_;
    if constexpr(Phase==7)++slot;
}
#if defined(__GNUC__) && !defined(__clang__)
#define NES_VIDEO_GROUP_NOINLINE __attribute__((noinline,noclone))
#elif defined(__clang__)
#define NES_VIDEO_GROUP_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define NES_VIDEO_GROUP_NOINLINE __declspec(noinline)
#else
#define NES_VIDEO_GROUP_NOINLINE
#endif
/**
 * @brief Передаёт текущее событие временному захвату PPU.
 * @details Захват не является постоянным владельцем и не допускает элиминации данных по политике чужого основного PPU.
 * @param sprite Различает sprite/background-траекторию выборки; не заменяет проверку сигналов маппера.
 * @param capture Ограниченная завершённая временная группа; публикуются только count инициализированных event-слов.
 */
void TimingPpu::emitCapture(const Capture& capture,bool sprite) noexcept {
    if(capture.overflow||capture.count>12){packet_->fault(VideoFault::InvalidEvent);return;}
    /**
     * @brief Восстанавливает VideoEvent из двух полностью инициализированных raw-слов локального Capture; индекс ограничен опубликованным count.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    auto at=[&](unsigned i)noexcept{return VideoEvent{capture.words[i*2],capture.words[i*2+1]};};
    if(sprite&&capture.count==3){
        const auto meta=at(0),low=at(1),high=at(2);
        const auto kind=low.value&VideoByteRef::kindMask;
        if(meta.op()==VideoOp::SpriteMeta&&low.op()==VideoOp::SpriteLow&&high.op()==VideoOp::SpriteHigh&&
           meta.arg0()==low.arg0()&&meta.arg0()==high.arg0()&&meta.arg0()<8&&
           !meta.arg1()&&!meta.arg2()&&!low.arg1()&&!low.arg2()&&!high.arg1()&&!high.arg2()&&
           !(meta.value>>25)&&(kind==VideoByteRef::rom||kind==VideoByteRef::chrRam)&&
           (low.value&VideoByteRef::offsetMask)<=0x1FFFFFF7U&&high.value==low.value+8U){
            packet_->addSpriteTile(meta.arg0(),meta.value&255U,(meta.value>>8)&255U,(meta.value>>16)&255U,
                (meta.value>>24)!=0,low.value);return;
        }
    }else if(!sprite&&capture.count==12){
        bool valid=true;
        for(unsigned i=0;i<12;++i){
            const auto e=at(i);
            if(i==0)valid&=e.tag==static_cast<unsigned>(VideoOp::BgLoad)&&!e.value;
            else if(i==4)valid&=e.op()==VideoOp::BgAttribute&&(e.arg0()==0||e.arg0()==2||e.arg0()==4||e.arg0()==6)&&!e.arg1()&&!e.arg2();
            else if(i==7)valid&=e.tag==static_cast<unsigned>(VideoOp::BgLow);
            else if(i==10)valid&=e.tag==static_cast<unsigned>(VideoOp::BgHigh);
            else valid&=e.tag==(static_cast<unsigned>(VideoOp::BgShift)|(1U<<8))&&!e.value;
        }
        const auto low=at(7).value,high=at(10).value,kind=low&VideoByteRef::kindMask;
        if(valid&&(kind==VideoByteRef::rom||kind==VideoByteRef::chrRam)&&
           (low&VideoByteRef::offsetMask)<=0x1FFFFFF7U&&high==low+8U){
            packet_->addPreloadTile(low,nextAttribute_);return;
        }
    }
    // Редкий некомпактный захват сохраняет порядок каждого исходного tag/value. Пока
    // стековый sink активен, payload не виден consumer.
    for(unsigned i=0;i<capture.count;++i){const auto e=at(i);packet_->add(e.op(),e.arg0(),e.arg1(),e.arg2(),e.value);}
}
/**
 * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
 * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
 * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
 * @return Вычисленное значение или состояние типа NES_VIDEO_GROUP_NOINLINE согласно описанному контракту.
 */
template<bool Clocked>
NES_VIDEO_GROUP_NOINLINE
void TimingPpu::spriteFetchGroup(unsigned dots) noexcept {
    unsigned slot=(dot_-257)/8,cachedSlot=~0U;std::uint16_t pattern=0;
    do{
    if(!capture_&&(packet_
        ||(this==&bus_.timingPpu_&&bus_.observableVideoEnabled())
)&&slot>0&&((dot_-257)&7)==0&&dots>=8){
        pattern=spritePattern(slot);cachedSlot=slot;
        const auto mode=bus_.videoSpriteMode(pattern,Clocked,trackBusAddresses_);
        if(mode!=Machine::VideoSpriteMode::None){
            // Нет скалярного replay-события или стекового sink на обычном полном слоте.
            // Тот же загрузчик метаданных остаётся определяющим для неполных точек.
            loadSpriteMetadata(slot);
            const auto fetch=bus_.videoSpritePlanes(pattern,mode,this);
            const auto low=static_cast<std::uint8_t>(fetch.packed),high=static_cast<std::uint8_t>(fetch.packed>>8);
            auto& sprite=sprites_[slot];
            sprite.low=(sprite.attributes&0x40)?reverse(low):low;
            sprite.high=(sprite.attributes&0x40)?reverse(high):high;
            if(packet_){
                if(!outputSpriteRows()){
                if((fetch.lowReference&VideoByteRef::offsetMask)<=0x1FFFFFF7U)
                    packet_->addSpriteTile(slot,sprite.x,sprite.attributes,sprite.index,sprite.valid,fetch.lowReference);
                else {
                    event(VideoOp::SpriteMeta,slot,0,0,unsigned(sprite.x)|(unsigned(sprite.attributes)<<8)|(unsigned(sprite.index)<<16)|(unsigned(sprite.valid)<<24));
                    event(VideoOp::SpriteLow,slot,0,0,fetch.lowReference);event(VideoOp::SpriteHigh,slot,0,0,fetch.lowReference+8U);
                }
                }
            }
            busAddress_=static_cast<std::uint16_t>(pattern+8U);dot_+=8;++slot;dots-=8;
            if(!dots)return;
            continue;
        }
    }
    switch((dot_-257)&7){
    case 0:spriteFetchDot<Clocked,0>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 1:spriteFetchDot<Clocked,1>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 2:spriteFetchDot<Clocked,2>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 3:spriteFetchDot<Clocked,3>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 4:spriteFetchDot<Clocked,4>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 5:spriteFetchDot<Clocked,5>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 6:spriteFetchDot<Clocked,6>(slot,cachedSlot,pattern);if(!--dots)return;[[fallthrough]];
    case 7:spriteFetchDot<Clocked,7>(slot,cachedSlot,pattern);if(!--dots)return;
    }}while(dots);
}
/**
 * @brief Обслуживает точную фазу фонового конвейера PPU.
 * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
 */
template<bool Clocked,bool Visible,unsigned Phase>
inline void TimingPpu::backgroundDot() noexcept {
    if constexpr(Clocked)bus_.ppuCycle();
    if constexpr(Phase==0)loadBackground();
    if constexpr(Visible)renderPixel();
    if(pendingBackgroundShifts_<16)++pendingBackgroundShifts_;
    fetchBackgroundPhase<Phase>();
    if constexpr(!Visible)event(VideoOp::BgShift,1);
    if constexpr(Visible&&Phase==6){if(dot_==255)scrollSignal_=true;}
    if constexpr(Visible&&Phase==7){if(dot_==256&&scrollSignal_)incrementY();}
    ++dot_;
}
/**
 * @brief Обслуживает точную фазу фонового конвейера PPU.
 * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
 * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
 * @return Вычисленное значение или состояние типа NES_VIDEO_GROUP_NOINLINE согласно описанному контракту.
 */
template<bool Clocked,bool Visible>
NES_VIDEO_GROUP_NOINLINE
void TimingPpu::backgroundGroup(unsigned dots) noexcept {
    do{
    // Полный offscreen-тайл не рисует и не тактирует спрайты. Повторно использовать
    // прежнюю упорядоченную шинную выборку и опубликовать одну неизменяемую preload-запись.
    // Неполные группы, pre-render и нестандартные callback сохраняют прежний путь.
    if constexpr(!Visible)if(!capture_&&(packet_
        ||(this==&bus_.timingPpu_&&bus_.observableVideoEnabled())
)&&unsigned(scanline_)<240&&
        (dot_==321||dot_==329)&&dots>=8&&bus_.cartridge_.loaded()){
        const auto reader=bus_.videoTileReader(Clocked,trackBusAddresses_,1,control_);
        if(reader){
            flushPixels();loadBackgroundAtoms();
            const auto fetch=(bus_.*reader)(v_,control_,this);
            nextTile_=static_cast<std::uint8_t>(fetch.packed>>24);
            nextAttribute_=static_cast<std::uint8_t>((fetch.packed>>16)&3U);
            backgroundAtoms_[LowNext]=fetch.lowReference;
            backgroundAtoms_[HighNext]=fetch.lowReference+8U;
            busAddress_=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+
                unsigned(nextTile_)*16U+((v_>>12)&7U)+8U);
            incrementX();coarseXSignal_=false;pendingBackgroundShifts_=8;dot_+=8;
            if(packet_)packet_->addPreloadTile(fetch.lowReference,nextAttribute_);
            dots-=8;if(!dots)return;continue;
        }
    }
    switch((dot_-1)&7){
    case 0:backgroundDot<Clocked,Visible,0>();if(!--dots)return;[[fallthrough]];
    case 1:backgroundDot<Clocked,Visible,1>();if(!--dots)return;[[fallthrough]];
    case 2:backgroundDot<Clocked,Visible,2>();if(!--dots)return;[[fallthrough]];
    case 3:backgroundDot<Clocked,Visible,3>();if(!--dots)return;[[fallthrough]];
    case 4:backgroundDot<Clocked,Visible,4>();if(!--dots)return;[[fallthrough]];
    case 5:backgroundDot<Clocked,Visible,5>();if(!--dots)return;[[fallthrough]];
    case 6:backgroundDot<Clocked,Visible,6>();if(!--dots)return;[[fallthrough]];
    case 7:backgroundDot<Clocked,Visible,7>();if(!--dots)return;
    }}while(dots);
}
#undef NES_VIDEO_GROUP_NOINLINE
/**
 * @brief Доводит отложенное состояние спрайтов до текущего растрового среза.
 * @details Раздельные плоскости, flip и накопленные сдвиги относятся к моменту захвата, а не к новой метаинформации.
 */
void TimingPpu::materializeSprites() noexcept {
    if(!spriteCacheValid_)return;
    for(auto& sprite:sprites_){
        if(spriteCacheClocks_<=sprite.x)sprite.x-=static_cast<std::uint8_t>(spriteCacheClocks_);
        else{const unsigned shifts=spriteCacheClocks_-sprite.x;sprite.x=0;
            if(shifts>=8)sprite.low=sprite.high=0;else{sprite.low<<=shifts;sprite.high<<=shifts;}}
    }
    spriteCacheValid_=false;spriteCacheClocks_=0;
}
/**
 * @brief Доводит отложенную очередь видимых пикселей до текущего среза.
 * @details Сначала сохраняет исходный индексный выбор и необходимые хвосты; материализация не даёт права переиспользовать аппаратно читаемый буфер.
 */
void TimingPpu::flushPixels() noexcept {
    if(!pendingPixels_)return;
    event(pendingFill_?VideoOp::Fill:VideoOp::Draw,pendingPixels_&255U,0,0,
        pendingPixelX_|(unsigned(scanline_)<<8)|(unsigned(mask_)<<16)|
        (unsigned(pendingFill_?pendingPaletteIndex_:fineX_)<<24));
    pendingPixels_=0;
}
/**
 * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
 * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
 */
void TimingPpu::renderPixel() noexcept {
    const unsigned x=dot_-1;
    if(spriteCacheValid_&&!rendering())materializeSprites();
    if(rendering()){
        if((!spriteCacheValid_||spriteZeroRow_)&&!(status_&0x40)&&(mask_&0x18)==0x18&&x!=255&&(x>=8||(mask_&6)==6)){
            bool zero=false;
            if(spriteCacheValid_)zero=(spriteZeroMask_[x>>3]&(1U<<(x&7U)))!=0;
            else for(const auto& s:sprites_){if(!s.valid||s.x)continue;if(!((s.low|s.high)&0x80))continue;zero=s.index==0;break;}
            if(zero&&backgroundOpaque(fineX_+pendingBackgroundShifts_))status_|=0x40;
        }
        if(spriteCacheValid_)++spriteCacheClocks_;
        else for(auto& sprite:sprites_){if(sprite.x)--sprite.x;else{sprite.low<<=1;sprite.high<<=1;}}
    }
    if(elideCaptureData()){
        // Sprite0-детекция, сдвиги и end-of-row материализация остаются настоящими.
        if(x==255)materializeSprites();
        return;
    }
    const bool fill=!rendering();
    const unsigned index=fill&&(v_&0x3F00)==0x3F00?paletteAddress(v_):0;
    if(pendingPixels_&&(pendingFill_!=fill||pendingPixelX_+pendingPixels_!=x||pendingPaletteIndex_!=index))flushPixels();
    if(!pendingPixels_){pendingPixelX_=x;pendingFill_=fill;pendingPaletteIndex_=static_cast<std::uint8_t>(index);}
    ++pendingPixels_;
    if(x==255){flushPixels();materializeSprites();}
}
/**
 * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
 * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
 */
void TimingPpu::visibleDot() noexcept {
    if(dot_==255)scrollSignal_=true;
    if(((dot_-1)&7)==0)loadBackground();
    renderPixel();if(pendingBackgroundShifts_<16)++pendingBackgroundShifts_;
    fetchBackground();if(dot_==256&&scrollSignal_)incrementY();++dot_;
}
/**
 * @brief Обслуживает последовательность полных фоновых тайлов в текущей строке.
 * @details Временной PPU может удержать только живой хвост при доказанном отсутствии нужного Sprite0-наблюдения; полный вывод сохраняет фактический выбор пикселей.
 * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
 */
void TimingPpu::backgroundRun(unsigned tiles) noexcept {
    flushPixels();
    constexpr unsigned minimumSpanTiles=3;
    /** @brief Доказывает отсутствие Sprite0-hit только в уже разрешённом run.
     * @details Opaque Sprite0 вне этого интервала не требует чтения BG planes.
     * По краям сохраняются левый clip и запрет x255; при неизвестном cache/геометрии
     * остаётся прежний per-tile путь. Маска принадлежит текущей строке, MMIO снаружи. */
    const auto noSpriteZeroInRun=[this,tiles]() noexcept {
        if(!spriteZeroRow_||(status_&0x40U)||(mask_&0x18U)!=0x18U)return true;
        const unsigned x=dot_-1U;
        if(!spriteCacheValid_||x>=256U||(x&7U)||tiles>(256U-x)/8U)return false;
        const unsigned end=(x>>3U)+tiles;
        for(unsigned byte=x>>3U;byte<end;++byte){
            if(byte==0U&&(mask_&6U)!=6U)continue;
            const unsigned visible=byte==31U?0x7fU:0xffU;
            if(spriteZeroMask_[byte]&visible)return false;
        }
        return true;
    };
    if(!capture_&&this==&bus_.timingPpu_&&tiles>=minimumSpanTiles&&(packet_
        ||(this==&bus_.timingPpu_&&bus_.observableVideoEnabled())
)&&
       noSpriteZeroInRun()&&
       bus_.canVideoBgSpan(trackBusCycles_,trackBusAddresses_,tiles,control_,v_)&&bus_.prepareVideoBgView()){
        const auto startV=v_;
        const auto tail=bus_.captureVideoBgTail(v_,tiles,control_,this);
        // Отвергнутое завершение ничего не меняет; сохранить старый BgRun fallback.
        if(bus_.completeLegacyVideoBgSpan(tiles,tail.address)){
        if(trackBusCycles_)bus_.completeVideoBgSpan(tiles,control_);
        if(packet_)packet_->addBgSpan(tiles,dot_-1,scanline_,mask_,fineX_,startV,control_);
        for(unsigned i=0;i<3;++i){backgroundAtoms_[i]=tail.low[i];backgroundAtoms_[i+3]=tail.low[i]+8U;}
        nextTile_=tail.tile;nextAttribute_=tail.attribute;busAddress_=tail.address;v_=tail.v;
        pendingBackgroundShifts_=8;spriteCacheClocks_+=static_cast<std::uint16_t>(tiles*8U);dot_+=tiles*8U;coarseXSignal_=false;
        if(dot_==257){scrollSignal_=true;incrementY();materializeSprites();}
        return;
        }
    }
    std::uint32_t tokens[32];const unsigned start=dot_-1;
    const auto reader=bus_.videoTileReader(trackBusCycles_,trackBusAddresses_,tiles,control_);
    for(unsigned i=0;i<tiles;++i){
        loadBackgroundAtoms();
        if(spriteZeroRow_&&!(status_&0x40)&&(mask_&0x18)==0x18){
            const unsigned x=dot_-1;
            unsigned candidates=spriteZeroMask_[x>>3];
            if(x<8&&(mask_&6)!=6)candidates=0;
            if(x==248)candidates&=0x7FU; // Sprite0-hit никогда не возникает при x255.
            for(unsigned p=0;candidates&&p<8;++p,candidates>>=1)
                if((candidates&1U)&&backgroundOpaque(fineX_+p)){status_|=0x40;break;}
        }
        spriteCacheClocks_+=8;
        if(reader){
            const auto fetch=(bus_.*reader)(v_,control_,this);
            nextTile_=static_cast<std::uint8_t>(fetch.packed>>24);
            nextAttribute_=static_cast<std::uint8_t>((fetch.packed>>16)&3U);
            backgroundAtoms_[LowNext]=fetch.lowReference;
            backgroundAtoms_[HighNext]=fetch.lowReference+8U;
            busAddress_=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+unsigned(nextTile_)*16U+((v_>>12)&7U)+8U);
            const auto ref=fetch.lowReference;
            tokens[i]=(ref&0x3FFFFFFFU)|((ref&VideoByteRef::chrRam)?0x20000000U:0U)|(unsigned(nextAttribute_)<<30);
        }else
        {
        if(trackBusCycles_)bus_.ppuCycle();
        address(0x2000|(v_&0x0FFF));
        if(trackBusCycles_)bus_.ppuCycle();
        nextTile_=bus_.ppuRead(busAddress_);
        if(trackBusCycles_)bus_.ppuCycle();
        address(0x23C0|(v_&0x0C00)|((v_>>4)&0x38)|((v_>>2)&7));
        if(trackBusCycles_)bus_.ppuCycle();
        nextAttribute_=(bus_.ppuRead(busAddress_)>>(((v_>>4)&4)|(v_&2)))&3;
        address(((control_&0x10)?0x1000:0)+nextTile_*16+((v_>>12)&7));
        if(trackBusCycles_)bus_.ppuCycle();
        address(((control_&0x10)?0x1000:0)+nextTile_*16+((v_>>12)&7));
        if(trackBusCycles_)bus_.ppuCycle();
        const auto ref=bus_.videoReference(busAddress_);
        backgroundAtoms_[LowNext]=bus_.captureVideoPattern(busAddress_,ref);
        if(trackBusCycles_)bus_.ppuCycle();
        address(busAddress_+8);
        if(trackBusCycles_)bus_.ppuCycle();
        // FD/FE-защёлки действуют после возвращённого high-байта. Разрешить обе
        // идентичности до read-стробов; не кэшировать bank-выбор
        // следующего тайла через этот строб.
        const auto highRef=bus_.videoReference(busAddress_);
        if(highRef!=ref+8U&&packet_)packet_->fault(VideoFault::InvalidReference);
        backgroundAtoms_[HighNext]=bus_.captureVideoPattern(busAddress_,highRef);
        tokens[i]=(ref&0x3FFFFFFFU)|((ref&VideoByteRef::chrRam)?0x20000000U:0U)|(unsigned(nextAttribute_)<<30);
        }
        incrementX();coarseXSignal_=false;pendingBackgroundShifts_=8;dot_+=8;
        if(dot_==257){scrollSignal_=true;incrementY();materializeSprites();}
    }
    if(packet_)packet_->addBgRun(tiles,start,scanline_,mask_,fineX_,tokens);
}
/**
 * @brief Продвигает PPU на указанное число уже прошедших точек.
 * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
 * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
 */
void TimingPpu::tickDots(unsigned dots) noexcept {
    if(this==&bus_.timingPpu_&&bus_.thinPpuBound()){
        if(packet_||capture_){bus_.invalidateThinPpu();return;}
        if(bus_.thinSignalMode()){thinTickDots(dots);return;}
    }
    if(mmc5Fetches()
       && !(mmc5InplaceNoRdDots()>=dots)
    ){
        // Только уже прошедшие 3/4 точки одного M2: ни CPU/MMIO, ни строка не пересекаются.
        // Visible=true исключает preload/span shortcuts; каждая фаза остаётся typed read/event.
        if((dots==3U||dots==4U)&&this==&bus_.timingPpu_&&bus_.outputOwner_&&
           packet_&&packet_==bus_.videoPacket_&&!capture_&&renderingEnabled_&&
           !(renderDelay_|nmiDelay_)&&static_cast<unsigned>(scanline_)<height&&
           dot_>=1U&&dot_<=width&&dots<=width+1U-dot_){
            if(auto* mapper=bus_.mmc5QuietMapper())if(auto* plan=acquireMmc5RasterPlan(*mapper)){
                runMmc5PlanVisibleM2(dots,*plan);return;
            }
            const unsigned phase=(dot_-1U)&7U;
            // Без load/flush, S0-наблюдения и конца строки эти три долга не читаются
            // M5 fetch-callback. CPU получит полностью материализованный M2-срез.
            if(phase&&phase+dots<=8U&&dot_+dots<=width&&bus_.outputJob_&&
               !bus_.outputInitializing_&&packet_->state()==VideoPacket::State::Recording&&
               packet_->header().fault==VideoFault::None&&spriteCacheValid_&&
               ((status_&0x40U)||!spriteZeroRow_)&&pendingPixels_&&
               !pendingFill_&&!pendingPaletteIndex_&&pendingPixelX_<width&&
               pendingPixels_<=width-dots&&pendingPixelX_+pendingPixels_==dot_-1U&&
               spriteCacheClocks_<=width-dots&&pendingBackgroundShifts_<=16U){
                /**
                 * @brief Сохраняет фазовые M5-стробы одного уже допущенного M2 без невидимых промежуточных debt stores.
                 * @param clocked compile-time признак прежнего ppuCycle на каждой точке; не новый mapper clock.
                 */
                const auto phases=[&](auto clocked)noexcept{
                    unsigned remaining=dots;
                    /**
                     * @brief Исполняет прежний clock/fetch/dot prefix без renderPixel и shift debt, закрываемых после span.
                     * @param tag compile-time фаза 1..7; phase0 и dot256 исключены до первого строба.
                     */
                    const auto next=[&](auto tag)noexcept{
                        if constexpr(decltype(clocked)::value)bus_.ppuCycle();
                        fetchBackgroundPhase<decltype(tag)::value>();
                        // Укороченная M2-ветка обходит backgroundDot, поэтому сама сохраняет E_EV255.
                        if constexpr(decltype(tag)::value==6)if(dot_==255)scrollSignal_=true;
                        ++dot_;
                    };
                    switch(phase){
                    case 1:next(std::integral_constant<unsigned,1>{});if(!--remaining)return;[[fallthrough]];
                    case 2:next(std::integral_constant<unsigned,2>{});if(!--remaining)return;[[fallthrough]];
                    case 3:next(std::integral_constant<unsigned,3>{});if(!--remaining)return;[[fallthrough]];
                    case 4:next(std::integral_constant<unsigned,4>{});if(!--remaining)return;[[fallthrough]];
                    case 5:next(std::integral_constant<unsigned,5>{});if(!--remaining)return;[[fallthrough]];
                    case 6:next(std::integral_constant<unsigned,6>{});if(!--remaining)return;[[fallthrough]];
                    case 7:next(std::integral_constant<unsigned,7>{});return;
                    }
                };
                if(trackBusCycles_)phases(std::true_type{});else phases(std::false_type{});
                spriteCacheClocks_=static_cast<std::uint16_t>(spriteCacheClocks_+dots);
                pendingPixels_+=dots;
                pendingBackgroundShifts_=static_cast<std::uint8_t>(std::min(16U,unsigned(pendingBackgroundShifts_)+dots));
                return;
            }
            if(trackBusCycles_)backgroundGroup<true,true>(dots);
            else backgroundGroup<false,true>(dots);
            return;
        }
        // Неполные интервалы, delay, capture, pre-render, sprite и dummy-фазы прежние.
        while(dots){tick();--dots;}return;
    }
    while(dots){
        if(renderingEnabled_&&!(renderDelay_|nmiDelay_)&&spriteCacheValid_&&
           unsigned(scanline_)<240&&dot_>=1&&((dot_-1)&7)==0&&dot_<=249&&dots>=8){
            const unsigned tiles=std::min(dots/8,(257U-dot_)/8);backgroundRun(tiles);dots-=tiles*8;continue;
        }
        if(renderingEnabled_&&!(renderDelay_|nmiDelay_)&&unsigned(scanline_)<240){
            if(dot_>=1&&dot_<=256){
                unsigned n=std::min(dots,257U-dot_);
                const unsigned prefix=deferredPrefix();
                if(prefix)n=std::min(n,prefix);
                if(trackBusCycles_)backgroundGroup<true,true>(n);else backgroundGroup<false,true>(n);
                dots-=n;continue;
            }
            if(dot_>=258&&dot_<=320){
                const unsigned n=std::min(dots,321U-dot_);
                if(trackBusCycles_)spriteFetchGroup<true>(n);else spriteFetchGroup<false>(n);
                dots-=n;continue;
            }
            if(dot_>=321&&dot_<=336){
                const unsigned n=std::min(dots,337U-dot_);
                if(trackBusCycles_)backgroundGroup<true,false>(n);else backgroundGroup<false,false>(n);
                dots-=n;continue;
            }
        }
        // Forced-blank видимая строка не имеет scroll/fetch/shift-переходов.
        // Line-событие dot0 оставить скалярным; разделить pixel/tail-фазы.
        // CPU/MMIO и отложенные rendering/NMI-изменения остаются внешними границами.
        if(!renderingEnabled_&&!(renderDelay_|nmiDelay_)&&
           !coarseXSignal_&&!scrollSignal_&&unsigned(scanline_)<240U&&dot_>=1U&&dot_<=340U){
            const unsigned n=std::min(dots,(dot_<=256U?257U:341U)-dot_);
            // Скалярный порядок начинается тактом, затем address(v_). У конкретного
            // mapper-графа нет обратного pixel-наблюдателя; последующие удерживаемые адреса
            // лишь старят A12. Сохранить даже IRQ-фронт первого перехода.
            if(trackBusCycles_||trackBusAddresses_)
                bus_.advanceStablePpuSignals(n,v_&0x3FFFU,trackBusCycles_,trackBusAddresses_);
            busAddress_=v_&0x3FFFU;
            if(dot_<=256U){
                if(spriteCacheValid_)materializeSprites();
                if(!elideCaptureData()){
                    const unsigned x=dot_-1U;
                    const unsigned index=(v_&0x3F00U)==0x3F00U?paletteAddress(v_):0U;
                    if(pendingPixels_&&(!pendingFill_||pendingPixelX_+pendingPixels_!=x||pendingPaletteIndex_!=index))flushPixels();
                    if(!pendingPixels_){pendingPixelX_=x;pendingFill_=true;pendingPaletteIndex_=static_cast<std::uint8_t>(index);}
                    pendingPixels_+=n;
                }
                evaluationNextDot_=static_cast<std::uint16_t>(dot_+n);
                if(dot_+n==257U){flushPixels();materializeSprites();}
            }
            dot_=static_cast<std::uint16_t>(dot_+n);dots-=n;
            if(dot_==341U){dot_=0;++scanline_;}
            continue;
        }
        // Холостые строки не содержат растровой работы или event-границы внутри этой границы.
        const unsigned quiet=nonVisibleQuietDots();
        if(quiet){
            const unsigned n=std::min(dots,quiet);
            if(trackBusCycles_||trackBusAddresses_)
                bus_.advanceStablePpuSignals(n,busAddress_,trackBusCycles_,trackBusAddresses_);
            const unsigned next=dot_+n;
            scanline_+=static_cast<std::int16_t>(next/341);dot_=next%341;dots-=n;continue;
        }
        tick();--dots;
    }
}
/**
 * @brief Продвигает устройство на одну его логическую единицу времени.
 * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
 */
void TimingPpu::tick() noexcept {
    if(this==&bus_.timingPpu_&&bus_.thinPpuBound()){
        if(packet_||capture_){bus_.invalidateThinPpu();return;}
        if(bus_.thinSignalMode()){thinTick();return;}
    }
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
        flushPixels();event(VideoOp::FrameEnd);
        updateNmi();
    }
    const bool active = rendering() && renderLine();
    if(dot_==255)scrollSignal_=active;
    if (scanline_ >= 0 && scanline_ < 240 && dot_ == 0) {
        evaluationNextDot_ = 1;
        if (rendering()) buildSpriteRow();
        if(outputSpriteRows())packet_->addSpriteRow(static_cast<unsigned>(scanline_),sprites_,spriteCacheValid_?unsigned(spriteCacheClocks_):0U);
        else
        event(VideoOp::VisibleLine,static_cast<unsigned>(scanline_));
    }
    const bool backgroundFetch = (dot_ >= 1 && dot_ <= 256) || (dot_ >= 321 && dot_ <= 336);
    if(!active&&backgroundFetch&&((dot_-1)&7)==6)coarseXSignal_=false;
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
            if(scanline_<0||dot_>256)event(VideoOp::BgShift,1);
        }

        if (dot_ == 257) { loadBackground(); finishHorizontalScrollSignal(); }
        if (scanline_ == -1 && dot_ >= 280 && dot_ <= 304) v_ = (v_ & 0x041F) | (t_ & 0x7BE0);
        if (dot_ >= 257 && dot_ <= 320) fetchSprites();
        if (dot_ == 337) { loadBackground(); address(0x2000 | (v_ & 0x0FFF)); }
        if (dot_ == 338 || dot_ == 340) {
            if(mmc5Fetches())(void)bus_.ppuReadFetch(busAddress_,PpuFetchKind::DummyName,0);
            else (void)bus_.ppuRead(busAddress_);
        }
        if (dot_ == 339) address(0x2000 | (v_ & 0x0FFF));
    } else address(v_);
    if(!active&&backgroundFetch&&((dot_-1)&7)==7&&coarseXSignal_){incrementX();coarseXSignal_=false;}
    if(dot_==256&&scrollSignal_)incrementY();
    if(!active&&dot_==257)finishHorizontalScrollSignal();
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

} // пространство имён nes::core


namespace nes::core {
/** @details Resolver не нужен: все шесть refs доказанно literal. Ветка shifts!=8 остаётся прежним битовым mix. */
void TimingPpu::loadMmc5TrustedPlanes(Mmc5TrustedState& state) noexcept {
    const unsigned shifts=state.shifts;
    for(unsigned base:{unsigned(LowHigh),unsigned(HighHigh)}){
        std::uint32_t high;
        if(!shifts)high=state.atoms[base];
        else if(shifts==8U)high=state.atoms[base+1U];
        else if(shifts>=16U)high=VideoByteRef::literal;
        else{
            const unsigned low=static_cast<std::uint8_t>(state.atoms[base+1U]);
            const unsigned oldHigh=shifts<8U?static_cast<std::uint8_t>(state.atoms[base]):0U;
            high=VideoByteRef::literal|(((((oldHigh<<8U)|low)<<shifts)>>8U)&255U);
        }
        state.atoms[base]=high;state.atoms[base+1U]=state.atoms[base+2U];
    }
    state.shifts=0;
}
}

namespace nes::core {

/** @details Здесь нет PPU read/data observer. Старые generic helpers сохраняют Fill,
 * sprite debt и evaluationNextDot; Mmc5 clock(n) отдельно сохраняет idle3 timeout. */
unsigned TimingPpu::mmc5InplaceNoRdDots() const noexcept {
    if(pal_||trackBusCycles_||trackBusAddresses_||renderDelay_||nmiDelay_||
       this!=&bus_.timingPpu_||capture_)return 0;
    if(!renderingEnabled_&&!renderingNext_&&!coarseXSignal_&&!scrollSignal_&&
       unsigned(scanline_)<240U&&dot_>=1U&&dot_<=340U)
        return (dot_<=256U?257U:341U)-dot_;
    return std::min(nonVisibleQuietDots(),quietDots());
}
/** @details Normal R0 path не симулирует head. Alien held/R после CPU2007
 * требует отдельного pure forecast; SafeReset без cartridge RD в первом M2
 * не разрешает one-clock projection. В prefetch применён только cheap proof. */
unsigned TimingPpu::mmc5InplaceActiveDots() const noexcept {
    if(pal_||trackBusCycles_||trackBusAddresses_||this!=&bus_.timingPpu_||capture_||
       unsigned(scanline_)>=240U||dot_<9U||dot_>336U||!renderingEnabled_||!renderingNext_||
       !(mask_&0x18U)||renderDelay_||nmiDelay_||nmiPending_||frameReady_||
       (status_&0x80U)||(v_&0x8000U)||(t_&0x8000U))return 0;
    const auto* mapper=bus_.mmc5QuietMapper();
    if(!mapper||!mapper->inFrame_||mapper->irqLine()||!mapper->ppuReadSinceM2_||mapper->cpuIdle_>1U)return 0;
    const unsigned phase=(dot_-1U)&7U;
    const bool sprite=dot_>=257U&&dot_<=320U;
    const bool heldSafe=sprite?(phase!=7U||busAddress_<0x2000U):
        (phase<=4U||(phase<=6U?busAddress_<=0x1ff7U:busAddress_<0x2000U));
    if(mapper->nameRepeat_==0U&&mapper->lastPpuAddress_<0x2000U&&heldSafe)return 337U-dot_;
    if(dot_>320U)return 0;
    const auto head=mmc5epoch::forecastHead(dot_,v_,t_,busAddress_,mapper->lastPpuAddress_,
        mapper->nameRepeat_,coarseXSignal_,scrollSignal_);
    return head.status==mmc5epoch::HeadStatus::SafeReset&&(head.cartRdMask&7U)?337U-dot_:0U;
}
/** @details Оператор не пересекает границу тайла: backgroundFetchTileX остаётся
 * постоянным даже при локально удерживаемом dot. Поэтому частичные fetch используют
 * прежние compile-time фазы, но renderPixel/shift/sprite debt не повторяются на
 * каждой точке. Полный тайл может свернуть четыре typed data reads только после
 * настоящего quiet-detector certificate; Name-latch и конечный /RD сохраняются.
 * Запас шести legacy credits существующего pure predicate покрывает максимум
 * Draw+BgLoad+Attribute+Low+High; отказ не flush-ит пакет и не меняет fault phase. */
bool TimingPpu::tryMmc5VisiblePhaseSegment(unsigned dots) noexcept {
    if(auto* mapper=bus_.mmc5QuietMapper())if(auto* plan=acquireMmc5RasterPlan(*mapper))
        return tryMmc5PlanPhaseSegment(dots,*plan);
#define NES_MMC5_PHASE_TILE(Scroll,TileX) mapper->ppuReadQuietBackgroundTile((Scroll),control_,bus_.nametables_.data(),(TileX))
#define NES_MMC5_PHASE_STEP(Phase) fetchBackgroundPhase<Phase>()
#include "Mmc5PhaseSegment.inl"
#undef NES_MMC5_PHASE_STEP
#undef NES_MMC5_PHASE_TILE
}
/** @details Первый digital M2 исполняется ровно один раз. Certificate гарантирует
 * RD в каждом трёхточечном M2 и отсутствие detector edge до 337, поэтому последующие
 * clocks только снова дают I0/Pfalse перед очередным RD. Public partial strobes
 * сохраняют настоящий конечный R/last/P. Full BgRun закрывает только известный High. */
void TimingPpu::replayMmc5InplaceDots(unsigned dots) noexcept {
    replayMmc5InplaceDotsEager(dots,true);
}

/** @details Старое тело сохраняет независимый exact fallback и порядок событий.
 * Только scalar-origin seed уже получил первый Cart.clockCpu перед tickDots. */
void TimingPpu::replayMmc5InplaceDotsEager(unsigned dots,bool clockFirstM2) noexcept {
    auto& mapper=*bus_.mmc5QuietMapper();
    if(clockFirstM2)mapper.clockCpu(1U);
    auto* const plan=acquireMmc5RasterPlan(mapper);
    while(dots){
        if(dot_<=256U){
            // V2 ring требует минимум 3 tiles и оставляет E_EV255/256 снаружи.
            unsigned tiles=std::min(dots/8U,(249U-std::min(249U,unsigned(dot_)))/8U);
            if(tiles>=3U&&((dot_-1U)&7U)==0U&&spriteCacheValid_&&
               (!spriteZeroRow_||(status_&0x40U))&&fineX_<=7U&&nextAttribute_<=3U&&
               pendingBackgroundShifts_<=16U&&pendingPixels_&&!pendingFill_&&!pendingPaletteIndex_&&
               pendingPixelX_<256U&&pendingPixelX_+pendingPixels_==dot_-1U&&
               evaluationNextDot_>=1U&&evaluationNextDot_<=dot_&&mapper.nameRepeat_==0U&&mapper.lastPpuAddress_<0x2000U){
                const unsigned n=tiles*8U;
                bool literal=true;for(const auto atom:backgroundAtoms_)literal&=(atom&~255U)==VideoByteRef::literal;
                if(literal&&pendingPixels_<=256U-n&&spriteCacheClocks_<=256U-n){
                    if(plan)runMmc5PlanBg(tiles,mapper,*plan);else
                    runMmc5InplaceBg(tiles,mapper);
                    mapper.finishQuietBackground(busAddress_);dots-=n;continue;
                }
            }
            // Новый bounded operator получает только уже разрешённый quiet
            // интервал. Приоритет крупного bulk и прежний fallback сохранены.
            if(plan){
                const unsigned head=(dot_-1U)&7U;
                // Длинный невыровненный префикс доводим до tile boundary:
                // шаг 24 сохранял бы фазу и мог вытеснить следующий крупный bulk.
                const unsigned interval=head&&dots>=32U-head?8U-head:std::min(dots,24U);
                if(tryMmc5RasterInterval(interval,*plan)){dots-=interval;continue;}
            }
            const unsigned phase=(dot_-1U)&7U;
            const unsigned n=std::min(dots,8U-phase);
            if(plan?tryMmc5PlanPhaseSegment(n,*plan):tryMmc5VisiblePhaseSegment(n)){dots-=n;continue;}
            backgroundGroup<false,true>(n);dots-=n;continue;
        }
        if(dot_==257U){tick();--dots;continue;}
        if(dot_<=320U){
            const unsigned n=std::min(dots,321U-dot_);
            replayMmc5HeldSpriteGroup(n);
            dots-=n;continue;
        }
        const unsigned n=std::min(dots,337U-dot_);backgroundGroup<false,false>(n);dots-=n;
    }
}

/** @details Pure дополнительный certificate сужает B3 до literal BG и безопасного
 * Sprite0. Вне этого домена прежний scalar replay получает все clocks.
 * На dot249 и далее новый оператор не входит; E_EV255/256 остаются исходными. */
unsigned TimingPpu::replayMmc5QuietBlock(unsigned clocks) noexcept {
    const unsigned grouped=clocks-(clocks%8U);
    const unsigned dots=grouped*3U;
    if(!grouped||clocks>80U||bus_.ppuRemainder_>4U||pal_||trackBusCycles_||trackBusAddresses_||
       !packet_||capture_||this!=&bus_.timingPpu_||packet_!=bus_.videoPacket_||
       !bus_.outputJob_||bus_.outputInitializing_||bus_.videoObserver_||bus_.videoObservableOnly_||
       !renderingEnabled_||!renderingNext_||!(mask_&0x18U)||renderDelay_||nmiDelay_||
       nmiPending_||frameReady_||(status_&0x80U)||unsigned(scanline_)>=240U||
       dot_<9U||dot_>225U||((dot_-1U)&7U)||dots>249U-dot_||
       !spriteCacheValid_||(spriteZeroRow_&&!(status_&0x40U))||spriteCacheClocks_>256U-dots||
       fineX_>7U||(v_&0x8000U)||(t_&0x8000U)||nextAttribute_>3U||pendingBackgroundShifts_>16U||
       !pendingPixels_||pendingPixels_>256U-dots||pendingFill_||pendingPaletteIndex_||
       pendingPixelX_>=256U||pendingPixelX_+pendingPixels_!=dot_-1U||
       evaluationNextDot_<1U||evaluationNextDot_>dot_||!packet_->canAppendMmc5Quiet(dots))return 0;
    for(const auto atom:backgroundAtoms_)if((atom&~255U)!=VideoByteRef::literal)return 0;
    auto* mapper=bus_.mmc5QuietMapper();
    if(!mapper||!mapper->canRunQuietBackground())return 0;
    // Тот же полный certificate; новый оператор не создаёт дополнительный M2 owner.
    runMmc5InplaceBg(dots/8U,*mapper);
    mapper->finishQuietBackground(busAddress_);
    return grouped;
    Mmc5TrustedState state{backgroundAtoms_,pendingPixels_,pendingPixelX_,pendingBackgroundShifts_,spriteCacheClocks_};
    for(unsigned count=0;count<grouped;count+=8U){
        runMmc5QuietTile(state,*mapper);
        runMmc5QuietTile(state,*mapper);
        runMmc5QuietTile(state,*mapper);
    }
    backgroundAtoms_=state.atoms;
    pendingPixels_=state.pixels;pendingPixelX_=state.x;
    pendingBackgroundShifts_=static_cast<decltype(pendingBackgroundShifts_)>(state.shifts);
    spriteCacheClocks_=static_cast<decltype(spriteCacheClocks_)>(state.sprites);
    mapper->finishQuietBackground(busAddress_);
    return grouped;
}
}
namespace nes::core {
/** @details Первый Draw закрывает incoming tile. Existing BgRun содержит первые
 * N−1 current tiles, последний сохраняет старые четыре ops и pending Draw.
 * Наблюдателей внутри certificate нет; Main не запрашивает живые banks/ExRAM.
 * Offset возвращается тем же data read, без отдельного lookup/validation pass. */
void TimingPpu::runMmc5InplaceBg(unsigned tiles,Mmc5& mapper) noexcept {
    if(auto* plan=acquireMmc5RasterPlan(mapper)){runMmc5PlanBg(tiles,mapper,*plan);return;}
#define NES_MMC5_BG_TILE(Scroll,TileX,Planes) mapper.ppuReadQuietBackgroundTile((Scroll),control_,bus_.nametables_.data(),(TileX),(Planes))
#include "Mmc5BackgroundRun.inl"
#undef NES_MMC5_BG_TILE
}
/** @details Порядок literal Draw/load/fetch и четыре реальных MMC5 reads прежний.
 * Только certified owner без наблюдателей удерживает dot/адрес/next в локальных
 * значениях до конца тайла. Mapped getters не читают TimingPpu; BgLoad предшествует
 * новым fetch. Все выходные поля публикуются до следующего тайла либо scalar tail.
 * Machine time материализует caller после всего завершённого run. */
void TimingPpu::runMmc5QuietTile(Mmc5TrustedState& state,Mmc5& mapper) noexcept {
    const unsigned beginDot=dot_;
    const unsigned tileX=backgroundFetchTileX();
    const auto scroll=v_;
    if(state.pixels)event(VideoOp::Draw,state.pixels&255U,0,0,
        state.x|(unsigned(scanline_)<<8U)|(unsigned(mask_)<<16U)|(unsigned(fineX_)<<24U));
    // BgLoad видит прежние next/atoms до новых Name/Attr/planes, как scalar PPU.
    loadMmc5TrustedPlanes(state);event(VideoOp::BgLoad);
    const auto nameAddress=static_cast<std::uint16_t>(0x2000U|(scroll&0x0FFFU));
    const auto tile=mapper.ppuReadQuietBackground(nameAddress,Mmc5::Fetch::BackgroundName,bus_.nametables_.data(),tileX).value;
    const auto attributeAddress=static_cast<std::uint16_t>(0x23C0U|(scroll&0x0C00U)|((scroll>>4U)&0x38U)|((scroll>>2U)&7U));
    const unsigned shift=((scroll>>4U)&4U)|(scroll&2U);
    const auto attribute=mapper.ppuReadQuietBackground(attributeAddress,Mmc5::Fetch::BackgroundAttribute,bus_.nametables_.data(),tileX).value;
    event(VideoOp::BgAttribute,shift,0,0,VideoByteRef::literal|attribute);
    const auto lowAddress=static_cast<std::uint16_t>(((control_&0x10U)?0x1000U:0U)+tile*16U+((scroll>>12U)&7U));
    const auto low=mapper.ppuReadQuietBackground(lowAddress,Mmc5::Fetch::BackgroundPattern,bus_.nametables_.data(),tileX).value;
    state.atoms[LowNext]=VideoByteRef::literal|low;event(VideoOp::BgLow,0,0,0,VideoByteRef::literal|low);
    const auto highAddress=static_cast<std::uint16_t>((lowAddress+8U)&0x3FFFU);
    const auto high=mapper.ppuReadQuietBackground(highAddress,Mmc5::Fetch::BackgroundPattern,bus_.nametables_.data(),tileX).value;
    state.atoms[HighNext]=VideoByteRef::literal|high;event(VideoOp::BgHigh,0,0,0,VideoByteRef::literal|high);
    // Нет допустимого observer/MMIO между фазами. Материализовать прежний exit
    // до следующего тайла, scalar tail или возврата к CPU.
    nextTile_=tile;nextAttribute_=(attribute>>shift)&3U;busAddress_=highAddress;
    incrementX();coarseXSignal_=false;dot_=beginDot+8U;
    state.pixels=8U;state.x=beginDot-1U;state.shifts=8U;state.sprites+=8U;
}
}
