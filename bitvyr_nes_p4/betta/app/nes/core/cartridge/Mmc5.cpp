/**
 * @file Mmc5.cpp
 * @brief Цифровые регистры MMC5, вычисленные из аппаратной разводки адресных линий.
 */
#include "Mmc5.h"
#include "Mmc5RasterPlan.h"

namespace nes::core {
namespace {
/** @brief Проверяет степень двух в разрешённом диапазоне размеров backing. */
bool powerOfTwoSize(std::size_t bytes, std::size_t low, std::size_t high) noexcept {
    return bytes >= low && bytes <= high && (bytes & (bytes - 1U)) == 0;
}
}

/** @details Заимствует проверенное PRG/CHR/RAM backing картриджа; владение памятью остаётся снаружи mapper. */
bool Mmc5::attach(const std::uint8_t* prg, std::size_t prgBytes,
                  const std::uint8_t* chr, std::size_t chrBytes,
                  std::uint8_t* ram, std::size_t ramBytes, bool splitSlMode) noexcept {
    detach();
    if (!prg || !chr || !powerOfTwoSize(prgBytes, 8192, 1024U * 1024U) ||
        !powerOfTwoSize(chrBytes, 1024, 1024U * 1024U) ||
        (ramBytes && (!ram || !powerOfTwoSize(ramBytes, 8192, MaxPrgRamBytes)))) return false;
    prg_ = prg; chr_ = chr; ram_ = ram;
    prgBytes_ = prgBytes; chrBytes_ = chrBytes; ramBytes_ = ramBytes;
    splitSlMode_ = splitSlMode;
    powerOn();
    return true;
}

/** @details Снимает заимствованную проводку и делает последующие обращения безопасно пустыми. */
void Mmc5::detach() noexcept {
    invalidateRasterPlan();
    prg_ = chr_ = nullptr; ram_ = nullptr;
    prgBytes_ = chrBytes_ = ramBytes_ = 0;
    endFrame();
    irqEnabled_ = false;
}

/** @details Устанавливает начальные регистры и защиту записи; внешнее backing не копируется. */
void Mmc5::powerOn() noexcept {
    invalidateRasterPlan();
    // Неопределённые power-on регистры получают воспроизводимый ноль; это не
    // моделирование аналогового включения питания. Задокументированные reset values сохранены.
    exRam_.fill(0); prgBanks_.fill(0); chrBanks_.fill(0); prgBanks_[4] = 0xff;
    prgMode_ = 3; chrMode_ = 0; protect1_ = 1; protect2_ = 2; exMode_ = 3;
    ntMap_ = fillTile_ = fillAttribute_ = upperChr_ = 0;
    splitControl_ = splitScroll_ = splitBank_ = splitY_ = 0;
    irqCompare_ = multiplierA_ = multiplierB_ = ppuControl_ = ppuMask_ = lastChrSet_ = 0;
    cpuIdle_ = tileAttribute_ = tileSplitAttribute_ = tileSplitY_ = 0;
    tileNtOffset_ = 0; tileExtended_ = tileSplit_ = false;
    ppuReadSinceM2_ = false; irqEnabled_ = false;
    endFrame();
#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
    rebuildNormalChrCache(0); rebuildNormalChrCache(1);
#endif
}

/** @details Применяет reset-сигнал цифровой модели, сохраняя отдельно заданное время жизни backing. */
void Mmc5::resetSignal() noexcept {
    invalidateRasterPlan();
    // Reset-detected поля конфигурации независимы от scanline pending/counter.
    prgMode_ = 3; protect1_ = 1; protect2_ = 2; exMode_ = 3;
    prgBanks_[4] = 0xff; upperChr_ = 0; splitControl_ = splitScroll_ = 0;
    irqEnabled_ = false;
}

/** @details Декодирует текущее CPU-окно PRG/WRAM и его разрешения по регистрам mapper. */
bool Mmc5::decodePrg(std::uint16_t address, unsigned& bank, bool& rom) const noexcept {
    if (address < 0x6000) return false;
    if (address < 0x8000) { bank = prgBanks_[0] & 15U; rom = false; return true; }
    const unsigned page = (address - 0x8000U) >> 13;
    unsigned value;
    if (prgMode_ == 0) {
        value = prgBanks_[4]; bank = (value & 0x7cU) | page; rom = true;
    } else if (prgMode_ == 1) {
        value = prgBanks_[page < 2 ? 2 : 4];
        bank = (value & 0x7eU) | (page & 1U); rom = page >= 2 || (value & 0x80U) != 0;
    } else if (prgMode_ == 2 && page < 2) {
        value = prgBanks_[2]; bank = (value & 0x7eU) | page; rom = (value & 0x80U) != 0;
    } else {
        value = prgBanks_[page + 1]; bank = value & 0x7fU; rom = page == 3 || (value & 0x80U) != 0;
    }
    return true;
}

/** @details Квалифицирует смещение выбранного WRAM-банка в доступном backing. */
std::size_t Mmc5::ramOffset(unsigned bank, unsigned within) const noexcept {
    if (!ram_ || !ramBytes_) return InvalidChrOffset;
    // Коммерческие 8/32 KiB платы имеют один SRAM: CE1 (bank bit2) не подключён.
    // 16 KiB = два 8 KiB чипа; 64 KiB = два 32 KiB. Для 128 KiB задействован A16.
    bank &= 15U;
    if (ramBytes_ == 8192) {
        if (bank & 4U) return InvalidChrOffset;
        bank = 0;
    } else if (ramBytes_ == 16384) bank = (bank & 4U) ? 1U : 0U;
    else if (ramBytes_ == 32768) {
        if (bank & 4U) return InvalidChrOffset;
        bank &= 3U;
    } else if (ramBytes_ == 65536) bank &= 7U;
    return std::size_t(bank) * 8192U + (within & 8191U);
}

/** @brief Выбирает только подключённый $6000 SRAM без CPU read-strobe/IRQ/тактов. */
const std::uint8_t* Mmc5::cpu6000Page() const noexcept {
    const auto offset=ramOffset(prgBanks_[0],0);
    return offset==InvalidChrOffset?nullptr:ram_+offset;
}

/** @details Заимствует текущую физическую PRG-страницу без копирования ROM. */
const std::uint8_t* Mmc5::prgPage(unsigned page) const noexcept {
    if (page >= 4) return nullptr;
    unsigned bank = 0; bool rom = false;
    if (!decodePrg(static_cast<std::uint16_t>(0x8000U + page * 8192U), bank, rom)) return nullptr;
    if (rom) return prg_ && prgBytes_ ? prg_ + (std::size_t(bank) * 8192U & (prgBytes_ - 1U)) : nullptr;
    const auto offset = ramOffset(bank, 0);
    return offset == InvalidChrOffset ? nullptr : ram_ + offset;
}

/** @details Проверяет режим WRAM и защитные регистры до разрешения записи. */
std::uint8_t* Mmc5::writablePrg(std::uint16_t address) noexcept {
    if (protect1_ != 2 || protect2_ != 1) return nullptr;
    unsigned bank = 0; bool rom = false;
    if (!decodePrg(address, bank, rom) || rom) return nullptr;
    const auto offset = ramOffset(bank, address & 8191U);
    return offset == InvalidChrOffset ? nullptr : ram_ + offset;
}

/** @details Читает текущую карту CPU без побочных эффектов настоящего read-strobe. */
std::uint8_t Mmc5::cpuPeek(std::uint16_t address, std::uint8_t openBus) const noexcept {
    if (address == 0x5204)
        return static_cast<std::uint8_t>((openBus & 0x3eU) | (irqPending_ ? 0x80U : 0U) | (inFrame_ ? 0x40U : 0U));
    if (address == 0x5205 || address == 0x5206) {
        const unsigned product = unsigned(multiplierA_) * unsigned(multiplierB_);
        return static_cast<std::uint8_t>(address == 0x5205 ? product : product >> 8);
    }
    if (address >= 0x5c00 && address < 0x6000) return exMode_ >= 2 ? exRam_[address & 1023U] : openBus;
    unsigned bank = 0; bool rom = false;
    if (!decodePrg(address, bank, rom)) return openBus;
    if (rom) return prg_ && prgBytes_ ? prg_[(std::size_t(bank) * 8192U + (address & 8191U)) & (prgBytes_ - 1U)] : openBus;
    const auto offset = ramOffset(bank, address & 8191U);
    return offset == InvalidChrOffset ? openBus : ram_[offset];
}

/** @details Возвращает значение CPU-регистра/окна с требуемыми побочными эффектами чтения. */
std::uint8_t Mmc5::cpuRead(std::uint16_t address, std::uint8_t openBus) noexcept {
    const auto result = cpuPeek(address, openBus);
    if (address == 0x5204) irqPending_ = false;
    cpuReadStrobe(address, result);
    return result;
}

/** @details Наблюдает настоящий read отдельно от peek, сохраняя PCM/read-mode и IRQ-порядок. */
void Mmc5::cpuReadStrobe(std::uint16_t address, std::uint8_t value) noexcept {
    if (address == 0xfffa || address == 0xfffb || (address == 0x2002 && (value & 0x80U))) endFrame();
}

/** @details Применяет запись к фактическому регистру/WRAM и инвалидирует только зависящую от неё проводку. */
void Mmc5::cpuWrite(std::uint16_t address, std::uint8_t value) noexcept {
    if (address == 0x2000) { invalidateRasterPlan(); ppuControl_ = value; return; }
    if (address == 0x2001) {
        invalidateRasterPlan();
        const bool before = (ppuMask_ & 0x18U) != 0;
        ppuMask_ = value;
        if (!before && (value & 0x18U)) endFrame();
        else if (!(value & 0x18U)) { inFrame_ = false; lastPpuAddress_ = 0xffff; nameRepeat_ = 0; }
        return;
    }
    if (address == 0x4014) { endFrame(); return; }
    if (address >= 0x6000) {
        if (auto* target = writablePrg(address)) *target = value;
        return;
    }
    if (address >= 0x5c00) {
        const unsigned target=cpuExRamWriteTarget(address);
        if(target!=~0U)exRam_[target]=value;
        return;
    }
    if (address >= 0x5113 && address <= 0x5117) {
        prgBanks_[address - 0x5113U] = address == 0x5113 ? value & 15U : value;
        if (address == 0x5117) prgBanks_[4] |= 0x80U;
        return;
    }
    if (address >= 0x5120 && address <= 0x512b) {
        invalidateRasterPlan();
        const unsigned shift = 3U - chrMode_;
        // Значение фиксируется в физических единицах 1 KiB при записи, а не
        // пересчитывается задним числом при изменении $5101/$5130.
        // Неисследованные младшие биты больших режимов (hardware '?') равны нулю;
        // известный bit0 всегда получает CPU D0 согласно таблице адресных линий.
        chrBanks_[address - 0x5120U] = static_cast<std::uint16_t>((((unsigned(upperChr_) << 8) | value) << shift & 1023U) | (value & 1U));
        lastChrSet_ = address >= 0x5128 ? 1 : 0;
#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
        rebuildNormalChrCache(lastChrSet_);
#endif
        return;
    }
    switch (address) {
        case 0x5100: prgMode_ = value & 3U; break;
#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
        case 0x5101:
            invalidateRasterPlan();
            chrMode_ = value & 3U;
            rebuildNormalChrCache(0); rebuildNormalChrCache(1);
            break;
#else
        case 0x5101: invalidateRasterPlan(); chrMode_ = value & 3U; break;
#endif
        case 0x5102: protect1_ = value & 3U; break;
        case 0x5103: protect2_ = value & 3U; break;
        case 0x5104: invalidateRasterPlan(); exMode_ = value & 3U; break;
        case 0x5105: invalidateRasterPlan(); ntMap_ = value; break;
        case 0x5106: invalidateRasterPlan(); fillTile_ = value; break;
        case 0x5107: invalidateRasterPlan(); fillAttribute_ = static_cast<std::uint8_t>((value & 3U) * 0x55U); break;
        case 0x5130: invalidateRasterPlan(); upperChr_ = value & 3U; break;
        case 0x5200: invalidateRasterPlan(); splitControl_ = value; break;
        case 0x5201: invalidateRasterPlan(); splitScroll_ = value; if (!inFrame_) splitY_ = value; break;
        case 0x5202: invalidateRasterPlan(); splitBank_ = value; break;
        case 0x5203: irqCompare_ = value; break;
        case 0x5204: irqEnabled_ = (value & 0x80U) != 0; break;
        case 0x5205: multiplierA_ = value; break;
        case 0x5206: multiplierB_ = value; break;
        default: break;
    }
}

/** @details Завершает кадровое состояние детектора без выдуманного дополнительного PPU-фронта. */
void Mmc5::endFrame() noexcept {
    inFrame_ = irqPending_ = false; scanline_ = 0;
    lastPpuAddress_ = 0xffff; nameRepeat_ = 0; splitY_ = splitScroll_;
    tileExtended_ = tileSplit_ = false;
}

/** @details Продвигает цифровые счётчики на реальные CPU/M2-такты, сохраняя IRQ-границы. */
void Mmc5::clockCpu(unsigned cycles) noexcept {
    if (!cycles) return;
    // Флаг означает хотя бы один /RD между предыдущим и этим M2. Поэтому первый
    // M2 после чтения не является первым пустым M2; batch включает оставшиеся cycles-1.
    if (ppuReadSinceM2_) { cpuIdle_ = 0; ppuReadSinceM2_ = false; --cycles; }
    const unsigned room = 3U - cpuIdle_;
    cpuIdle_ = static_cast<std::uint8_t>(cycles >= room ? 3U : cpuIdle_ + cycles);
    if (cpuIdle_ == 3) {
        inFrame_ = false; lastPpuAddress_ = 0xffff; nameRepeat_ = 0;
        tileExtended_ = tileSplit_ = false;
        // Timeout сам не подтверждает pending IRQ: это отдельный latch $5204.
    }
}

/** @details Распознаёт строку по настоящей последовательности PPU-read адресов. */
void Mmc5::detectScanline() noexcept {
    if (!inFrame_) {
        inFrame_ = true; scanline_ = 0; irqPending_ = false; splitY_ = splitScroll_;
    } else if (scanline_ == 239) {
        endFrame();
    } else {
        ++scanline_;
        splitY_ = splitY_ == 239 ? 0 : static_cast<std::uint8_t>(splitY_ + 1U);
        if (irqCompare_ && scanline_ == irqCompare_) irqPending_ = true;
    }
}

/** @details Применяет точный PPU /RD к детектору и защёлкам mapper. */
void Mmc5::ppuReadStrobe(std::uint16_t address) noexcept {
    address &= 0x3fffU;
    if (address >= 0x3f00) return; // Palette read не создаёт cartridge /RD.
    ppuReadSinceM2_ = true;
    // Три одинаковых NT-read вооружают детектор; счётчик меняется только на
    // СЛЕДУЮЩЕМ /RD, независимо от адреса. Это fourth-read edge из аппаратных тестов.
    if (nameRepeat_ == 3) { detectScanline(); nameRepeat_ = 0; }
    if (address >= 0x2000 && address < 0x3000) {
        nameRepeat_ = address == lastPpuAddress_ ? static_cast<std::uint8_t>(nameRepeat_ + 1U) : 1;
    } else nameRepeat_ = 0;
    lastPpuAddress_ = address;
}

/** @details Разрешает текущую CIRAM/ExRAM/fill nametable-проводку без самостоятельного продвижения времени. */
std::uint8_t Mmc5::nametablePeek(std::uint16_t address, const std::uint8_t* ciram) const noexcept {
    const unsigned offset = address & 1023U;
    const unsigned source = (ntMap_ >> (((address >> 10) & 3U) * 2U)) & 3U;
    if (source < 2) return ciram ? ciram[source * 1024U + offset] : 0;
    if (source == 2) return exMode_ < 2 ? exRam_[offset] : 0;
    return offset < 0x3c0 ? fillTile_ : fillAttribute_;
}

/** @details Фиксирует policy и tile-защёлки на настоящей Name-границе background fetch. */
void Mmc5::beginBackground(std::uint16_t address, unsigned tileX) noexcept {
    tileExtended_ = (ppuMask_ & 0x18U) != 0 && exMode_ == 1;
    tileAttribute_ = exRam_[address & 1023U];
    const unsigned threshold = splitControl_ & 31U;
    const bool right = (splitControl_ & 0x40U) != 0;
    tileSplit_ = (ppuMask_ & 0x18U) && exMode_ < 2 && (splitControl_ & 0x80U) &&
        tileX < 32 && (right ? tileX >= threshold : tileX < threshold);
    if (tileSplit_) {
        // Prefetch0/1 относится к следующей строке. Первый visible Name (tile2)
        // предшествует fourth-read IRQ edge и также ещё видит предыдущий counter.
        tileSplitY_ = splitY_;
        if (inFrame_ && (tileX < 2 || nameRepeat_ == 3))
            tileSplitY_ = splitY_ == 239 ? 0 : static_cast<std::uint8_t>(splitY_ + 1U);
        tileNtOffset_ = static_cast<std::uint16_t>((unsigned(tileSplitY_) >> 3) * 32U + tileX);
        const unsigned index = 0x3c0U + (unsigned(tileSplitY_) >> 5) * 8U + (tileX >> 2);
        const unsigned shift = ((unsigned(tileSplitY_) >> 3) & 2U) * 2U + (tileX & 2U);
        tileSplitAttribute_ = static_cast<std::uint8_t>(((exRam_[index] >> shift) & 3U) * 0x55U);
    }
}

#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
/**
 * @details Размер подключённого ROM — степень двух не меньше 1 KiB. После
 * маскирования база остаётся кратной 1 KiB, поэтому прибавление младших десяти
 * разрядов PPU-адреса не требует повторного wrap. Сохранённые chrBanks_ уже имеют
 * единицы 1 KiB: смена $5101 не повторяет старые записи с новым $5130/режимом.
 */
void Mmc5::rebuildNormalChrCache(unsigned set) noexcept {
    if (!chr_ || !chrBytes_) { normalChrBases_[set].fill(0); return; }
    const unsigned width = 1U << (3U - chrMode_);
    const auto mask = static_cast<std::uint32_t>(chrBytes_ - 1U);
    for (unsigned slot = 0; slot < 8; ++slot) {
        const unsigned group = slot | (width - 1U);
        const unsigned index = set ? 8U + (group & 3U) : group & 7U;
        const unsigned bank = (unsigned(chrBanks_[index]) & ~(width - 1U)) + (slot & (width - 1U));
        normalChrBases_[set][slot] = (std::uint32_t(bank) * 1024U) & mask;
    }
}
#endif
/** @details Вычисляет текущий физический CHR offset по типу fetch и активным регистрам. */
std::size_t Mmc5::chrOffset(std::uint16_t address, Fetch kind) const noexcept {
    if (!chr_ || !chrBytes_ || address >= 0x2000) return InvalidChrOffset;
    std::size_t physical;
    // Name фиксирует данные тайла, но не отменяет последующее выключение
    // подстановок настоящей CPU-записью PPUMASK/$5104/$5200 между fetch phases.
    const bool split = tileSplit_ && (ppuMask_ & 0x18U) && exMode_ < 2 && (splitControl_ & 0x80U);
    const bool extended = tileExtended_ && (ppuMask_ & 0x18U) && exMode_ == 1;
    if (kind == Fetch::BackgroundPattern && split) {
        unsigned within = address & 0xfffU;
        if (splitSlMode_) within = (within & ~7U) | (tileSplitY_ & 7U);
        physical = std::size_t(splitBank_) * 4096U + within;
    } else if (kind == Fetch::BackgroundPattern && extended) {
        const unsigned bank = (unsigned(upperChr_) << 6) | (tileAttribute_ & 63U);
        physical = std::size_t(bank) * 4096U + (address & 0xfffU);
    } else {
        const bool tall = (ppuControl_ & 0x20U) != 0;
        const bool second = tall && (kind == Fetch::Cpu ? lastChrSet_ != 0 :
            kind == Fetch::BackgroundPattern && (ppuMask_ & 0x18U) != 0);
#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
        return std::size_t(normalChrBases_[second ? 1U : 0U][unsigned(address) >> 10]) + (address & 1023U);
#else
        const unsigned width = 1U << (3U - chrMode_);
        const unsigned group = ((unsigned(address) >> 10) | (width - 1U));
        const unsigned index = second ? 8U + (group & 3U) : group & 7U;
        physical = std::size_t(chrBanks_[index] & ~(width - 1U)) * 1024U + (address & (width * 1024U - 1U));
#endif
    }
    return physical & (chrBytes_ - 1U);
}

/** @details Прежний read-data алгоритм имеет один источник. Public путь
 * выполняет полный detector до него, qualified PPU24 доказывает конечный переход. */
#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]]
#endif
/** @details Возвращает байт разрешённого PPU-источника, не подменяя detector/strobe caller. */
inline Mmc5::PpuRead Mmc5::readPpuData(std::uint16_t address, Fetch kind,
                                     const std::uint8_t* ciram, unsigned tileX) noexcept {
    if (address < 0x2000) {
        const auto offset = chrOffset(address, kind);
        return {offset == InvalidChrOffset ? std::uint8_t{0} : chr_[offset], offset};
    }
    if (kind == Fetch::BackgroundName) {
        beginBackground(address, tileX);
        if (tileSplit_) return {exRam_[tileNtOffset_], InvalidChrOffset};
    } else if (kind == Fetch::BackgroundAttribute) {
        if (tileSplit_ && (ppuMask_ & 0x18U) && exMode_ < 2 && (splitControl_ & 0x80U))
            return {tileSplitAttribute_, InvalidChrOffset};
        if (tileExtended_ && (ppuMask_ & 0x18U) && exMode_ == 1)
            return {static_cast<std::uint8_t>((tileAttribute_ >> 6) * 0x55U), InvalidChrOffset};
    }
    return {nametablePeek(address, ciram), InvalidChrOffset};
}

/** @details Сохраняет порядок PPU-read эффекта и фактического байта текущей проводки. */
Mmc5::PpuRead Mmc5::ppuRead(std::uint16_t address, Fetch kind,
                           const std::uint8_t* ciram, unsigned tileX) noexcept {
    address &= 0x3fffU;
    if (address >= 0x3f00) return {};
    ppuReadStrobe(address);
    return readPpuData(address,kind,ciram,tileX);
}

/** @details Все реальные bytes и Name latch остаются на прежних PPU фазах.
 * Входной certificate исключает fourth-read и idle timeout до материализации. */
Mmc5::PpuRead Mmc5::ppuReadQuietBackground(std::uint16_t address, Fetch kind,
                                        const std::uint8_t* ciram, unsigned tileX) noexcept {
    return readPpuData(address,kind,ciram,tileX);
}

/** @details Name сначала обновляет прежние per-tile latch. До конца разрешённого
 * тайла MMIO отсутствует, поэтому split/extended policy выбирается один раз.
 * Low имеет bit3=0; high отличается только bit3 в той же 1 KiB CHR-странице.
 * attach допускает только ROM степени двух от 1 KiB: маска не меняет bit3,
 * bank/substitution для planes одинаковы, в том числе при SL fine-Y override.
 * Поэтому валидный high offset равен low+8 без второго банкового декодирования.
 * При detach/отказе attach invalid offset проверяется до обоих разыменований.
 * needPlanes=false оставляет Name-latch и CHR mapping, но вообще не читает CHR:
 * раннему packet token нужен offset, а живому PPU — лишь последние три пары. */
Mmc5::BackgroundTile Mmc5::ppuReadQuietBackgroundTile(std::uint16_t scroll,
                                                     std::uint8_t control,
                                                     const std::uint8_t* ciram,
                                                     unsigned tileX, bool needPlanes) noexcept {
    const auto nameAddress = static_cast<std::uint16_t>(0x2000U | (scroll & 0x0fffU));
    beginBackground(nameAddress, tileX);
    const auto tile = tileSplit_ ? exRam_[tileNtOffset_] : nametablePeek(nameAddress, ciram);
    std::uint8_t attribute;
    if (tileSplit_) attribute = tileSplitAttribute_;
    else if (tileExtended_) attribute = static_cast<std::uint8_t>((tileAttribute_ >> 6U) * 0x55U);
    else {
        const auto attributeAddress = static_cast<std::uint16_t>(0x23c0U |
            (scroll & 0x0c00U) | ((scroll >> 4U) & 0x38U) | ((scroll >> 2U) & 7U));
        attribute = nametablePeek(attributeAddress, ciram);
    }
    const auto lowAddress = static_cast<std::uint16_t>(((control & 0x10U) ? 0x1000U : 0U) |
        (unsigned(tile) << 4U) | ((scroll >> 12U) & 7U));
    const auto offset = chrOffset(lowAddress, Fetch::BackgroundPattern);
    if (!needPlanes || offset == InvalidChrOffset) return {tile, attribute, 0, 0, offset};
    return {tile, attribute, chr_[offset], chr_[offset + 8U], offset};
}


/** @details SpritePattern всегда выбирает наборA: BG split/extended и последний
 * CPU наборB к нему не относятся. Единый chrOffset сохраняет bank/mode/wrap;
 * bit3=0 удерживает high=low+8 в той же1KiB странице. Все bounds проверены
 * до обоих byte loads и до записи выходных ссылок. */
bool Mmc5::readQuietSpritePlanes(std::uint16_t lowAddress,std::uint8_t& low,
                                std::uint8_t& high) const noexcept {
    if(lowAddress>=0x2000U||(lowAddress&8U)||!chr_||!chrBytes_||chrMode_>3U)return false;
    const auto offset=chrOffset(lowAddress,Fetch::SpritePattern);
    if(offset==InvalidChrOffset||offset>=chrBytes_||chrBytes_-offset<=8U)return false;
    const auto a=chr_[offset],b=chr_[offset+8U];
    low=a;high=b;return true;
}

/** @details После целого canonical run каждый M2 видел RD, а последний high
 * сбросил повтор NT. Эти четыре поля закрывают весь временный detector state. */
void Mmc5::finishQuietBackground(std::uint16_t lastHigh) noexcept {
    nameRepeat_=0;
    cpuIdle_=0;
    ppuReadSinceM2_=true;
    lastPpuAddress_=lastHigh;
}

/** @details Только policy и адресные базы получают снимок. CIRAM/ExRAM/CHR байты
 * остаются заимствованными; splitY, inFrame и Name-детектор читаются live после
 * каждого настоящего /RD. Поэтому план пригоден и частичному началу тайла.
 * Normal-cache ON заимствует две готовые таблицы без копии64байт на каждый M2;
 * OFF один раз строит эквивалентные базы в самом плане. Все проверки отказа
 * предшествуют первой записи output; неудачная подготовка не публикует аренду. */
bool Mmc5::prepareRasterPlan(Mmc5RasterPlan& plan, const std::uint8_t* ciram,
                             std::uint8_t control) noexcept {
    if (!chr_ || !powerOfTwoSize(chrBytes_, 1024U, 1024U * 1024U) ||
        chrMode_ > 3U || exMode_ > 3U || upperChr_ > 3U) return false;
    plan.owner_ = this;
    plan.chr_ = chr_;
    plan.exRam_ = exRam_.data();
    plan.chrMask_ = static_cast<std::uint32_t>(chrBytes_ - 1U);
    plan.splitBase_ = std::uint32_t(splitBank_) << 12U;
    plan.extendedBase_ = std::uint32_t(upperChr_) << 18U;
    plan.patternTable_ = static_cast<std::uint16_t>((control & 0x10U) << 8U);
    const bool rendering = (ppuMask_ & 0x18U) != 0;
    const unsigned policy = (rendering && exMode_ == 1U ? 1U : 0U) |
        (rendering && exMode_ < 2U && (splitControl_ & 0x80U) ? 2U : 0U);
    plan.policy_ = static_cast<Mmc5RasterPlan::Policy>(policy);
    plan.splitThreshold_ = splitControl_ & 31U;
    plan.splitRight_ = (splitControl_ & 0x40U) != 0;
    plan.splitSlMode_ = splitSlMode_;
    for (unsigned page = 0; page < 4U; ++page) {
        const unsigned source = (ntMap_ >> (page * 2U)) & 3U;
        plan.ntPages_[page] = source < 2U ? (ciram ? ciram + source * 1024U : nullptr) :
            (source == 2U && exMode_ < 2U ? exRam_.data() : nullptr);
        plan.nameFallback_[page] = source == 3U ? fillTile_ : 0U;
        plan.attributeFallback_[page] = source == 3U ? fillAttribute_ : 0U;
    }
    const unsigned backgroundSet = (ppuControl_ & 0x20U) && rendering ? 1U : 0U;
#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
    plan.normalA_ = normalChrBases_[0].data();
    plan.normalBackground_ = normalChrBases_[backgroundSet].data();
#else
    const unsigned width = 1U << (3U - chrMode_);
    for (unsigned slot = 0; slot < 8U; ++slot) {
        const unsigned group = slot | (width - 1U);
        const unsigned within = slot & (width - 1U);
        const unsigned bankA = (unsigned(chrBanks_[group & 7U]) & ~(width - 1U)) + within;
        const unsigned indexBg = backgroundSet ? 8U + (group & 3U) : group & 7U;
        const unsigned bankBg = (unsigned(chrBanks_[indexBg]) & ~(width - 1U)) + within;
        plan.normalA_[slot] = (std::uint32_t(bankA) << 10U) & plan.chrMask_;
        plan.normalBackground_[slot] = (std::uint32_t(bankBg) << 10U) & plan.chrMask_;
    }
#endif
    return true;
}

/** @details Единственный accepted-target алгоритм для записи и её event witness. */
unsigned Mmc5::cpuExRamWriteTarget(std::uint16_t address) const noexcept {
    return address>=0x5c00U&&address<0x6000U&&(exMode_==2U||(exMode_<2U&&inFrame_))?unsigned(address&1023U):~0U;
}
/** @details CPU ExRAM inFrame gate не переносится на PPU source2. */
unsigned Mmc5::ppuWriteTarget(std::uint16_t address,bool ciram) const noexcept {
    address&=0x3fffU;if(address<0x2000U||address>=0x3f00U)return ~0U;
    const unsigned source=(ntMap_>>(((address>>10U)&3U)*2U))&3U;
    if(source<2U)return ciram?source*1024U+(address&1023U):~0U;
    return source==2U&&exMode_<2U?2048U+(address&1023U):~0U;
}
/** @details Разрешает запись только в доступное RAM backing выбранного PPU-окна. */
void Mmc5::ppuWrite(std::uint16_t address, std::uint8_t value, std::uint8_t* ciram) noexcept {
    address &= 0x3fffU;
    if (address < 0x2000 || address >= 0x3f00) return;
    const unsigned target=ppuWriteTarget(address,ciram!=nullptr);
    if(target<2048U)ciram[target]=value;
    else if(target<3072U)exRam_[target-2048U]=value;
}

} // namespace nes::core
