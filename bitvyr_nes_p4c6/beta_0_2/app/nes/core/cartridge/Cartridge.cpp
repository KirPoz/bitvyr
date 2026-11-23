/**
 * @file Cartridge.cpp
 * @brief Проверка iNES, bank-отображение и собственные mapper-протоколы.
 * @ingroup nes_cartridge
 */
#include "Cartridge.h"
#include "Mmc5.h"
#include "Mmc5Audio.h"

#include <algorithm>
#include <cstring>

namespace nes::core {
namespace {
/**
 * @brief Проверяет поддерживаемую комбинацию заголовка и Legacy-подварианта.
 * @details Не является универсальным допуском любого mapper160-образа.
 * @param image Неизменяемый заимствованный образ ROM; вызывающий сохраняет его срок жизни.
 * @param size Размер переданного диапазона в байтах.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool identifiedAladdin(const std::uint8_t* image, std::size_t size) noexcept {
    if (size != 524304) return false;
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= image[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc == 0xaf74d272u;
}
}

/**
 * @brief Проверяет и подключает заимствованный образ ROM.
 * @details Владелец образа удерживает неизменённые байты на весь срок использования. Отказ загрузки нельзя считать сохранением всех старых указателей картриджа; ранее выданные ссылки и эпохи обрабатываются установленным жизненным циклом.
 * @param image Неизменяемый заимствованный образ ROM; вызывающий сохраняет его срок жизни.
 * @param size Размер переданного диапазона в байтах.
 * @return Код проверки iNES/поддержки mapper; только Ok означает принятую загрузку, отказ имеет описанные эффекты инвалидирования.
 */
LoadResult Cartridge::load(const std::uint8_t* image, std::size_t size) noexcept {
    detachCpuPrgTail(); // Восстановить физические данные до reset/load, инвалидирующего заимствование.
    if (mmc5_) mmc5_->detach();
    prg_ = chr_ = nullptr;
    cpuPrgSourceImage_=nullptr;cpuPrgSourceImageBytes_=0;
    prgSize_ = chrSize_ = 0;
    if (!image || size < 16 || std::memcmp(image, "NES\x1a", 4) != 0)
        return LoadResult::InvalidHeader;
    const bool nes2 = (image[7] & 0x0c) == 0x08;
    if ((image[7] & 3) != 0) return LoadResult::UnsupportedFormat; // Форматы VS/PlayChoice.
    mapper_ = static_cast<std::uint16_t>((image[6] >> 4) | (image[7] & 0xf0));
    legacyHeader_ = !nes2 && (image[12] || image[13] || image[14] || image[15]);
    // Старые dump-инструменты писали сигнатуры в зарезервированные байты. Сохранить разобранный
    // mapper при нулевой старшей тетраде; не угадывать mapper по имени игры.
    if (legacyHeader_ && (image[7] & 0xf0)) return LoadResult::UnsupportedFormat;
    submapper_ = nes2 ? image[8] >> 4 : 0;
    if (nes2) {
        mapper_ |= static_cast<std::uint16_t>((image[8] & 0x0f) << 8);
        const bool knownSubmapper = submapper_ == 0 || (mapper_ == 71 && submapper_ == 1) ||
            ((mapper_ == 7 || mapper_ == 85) && submapper_ <= 2);
        if (!knownSubmapper || (image[9] & 0x0f) == 0x0f || (image[9] >> 4) == 0x0f)
            return LoadResult::UnsupportedFormat;
    }
    prgSize_ = static_cast<std::size_t>(image[4] | (nes2 ? ((image[9] & 15) << 8) : 0)) * 16384;
    chrSize_ = static_cast<std::size_t>(image[5] | (nes2 ? ((image[9] >> 4) << 8) : 0)) * 8192;
    const std::size_t offset = 16 + ((image[6] & 4) ? 512 : 0);
    if (!prgSize_ || offset > size || prgSize_ > size - offset || chrSize_ > size - offset - prgSize_)
        return LoadResult::Truncated;
    switch (mapper_) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 7: case 69: case 71: case 85: break;
        case 160:
            if (!identifiedAladdin(image, size)) return LoadResult::UnsupportedMapper;
            break;
        default: return LoadResult::UnsupportedMapper;
    }
    if (mapper_ == 0 && prgSize_ > 32768) return LoadResult::UnsupportedFormat;
    if (mapper_ == 71) {
        const std::size_t maxPrgBytes = (submapper_ == 1 ? 128U : 256U) * 1024U;
        if (prgSize_ > maxPrgBytes || chrSize_ > 8192U || (image[6] & 8U))
            return LoadResult::UnsupportedFormat;
        // NES 2.0 задаёт разводку явно. У старого формата $9000 отличает Fire Hawk
        // от фиксированных плат, которые также могут записывать по $8000.
        camericaMirrorStart_ = submapper_ == 1 ? 0x8000 : (!nes2 ? 0x9000 : 0xa000);
    }
    expansionRamBytes_ = 8192;
    if (mapper_ == 69 || mapper_ == 85) {
        // Поддерживаемая арена не расширяется из-за заголовка: неизвестную RAM-плату отвергаем.
        if (prgSize_ > 512U * 1024U || chrSize_ > 256U * 1024U || (image[6] & 8))
            return LoadResult::UnsupportedFormat;
        const auto ramSize = [](unsigned shift) noexcept -> std::size_t {
            return shift ? std::size_t{64} << shift : 0;
        };
        const auto ramBytes = nes2 ? ramSize(image[10] & 15U) + ramSize(image[10] >> 4)
                                  : std::size_t(image[8] ? image[8] : 1U) * 8192U;
        const auto chrRamBytes = nes2 ? ramSize(image[11] & 15U) + ramSize(image[11] >> 4) : 8192U;
        if ((ramBytes != 0 && ramBytes != 8192) ||
            (chrSize_ == 0 && chrRamBytes != 8192) || (chrSize_ != 0 && nes2 && chrRamBytes != 0))
            return LoadResult::UnsupportedFormat;
        expansionRamBytes_ = static_cast<std::uint16_t>(ramBytes);
    }
    fourScreen_ = (image[6] & 8) != 0;
    initialMirroring_ = fourScreen_ ? Mirroring::FourScreen :
        ((image[6] & 1) ? Mirroring::Vertical : Mirroring::Horizontal);
    palHint_ = nes2 ? (image[12] & 3) == 1 : (image[9] & 1) != 0;
    prg_ = image + offset;
    cpuPrgSourceImage_=image;cpuPrgSourceImageBytes_=size;
    chr_ = chrSize_ ? prg_ + prgSize_ : nullptr;
    prgRam_.fill(0);
    chrRam_.fill(0);
    if (image[6] & 4) std::memcpy(prgRam_.data() + 0x1000, image + 16, 512);
    if (mapper_ == 5) {
        if (!mmc5_ || !mmc5Audio_) { prg_=chr_=nullptr; return LoadResult::MissingMapperStorage; }
        const unsigned a=nes2?(image[10]&15U):7U, b=nes2?(image[10]>>4U):0U;
        const std::size_t ramBytes=(a?(std::size_t{64}<<a):0)+(b?(std::size_t{64}<<b):0);
        if(ramBytes>prgRam_.size() || !mmc5_->attach(prg_,prgSize_,chr_,chrSize_,ramBytes?prgRam_.data():nullptr,ramBytes)) {
            prg_=chr_=nullptr; return LoadResult::UnsupportedFormat;
        }
    }
    reset();
    return LoadResult::Ok;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void Cartridge::reset() noexcept {
    detachCpuPrgTail(); // Восстановить физические данные до reset/load, инвалидирующего заимствование.
    if (mapper_ == 5 && mmc5_) { mmc5_->powerOn(); if(mmc5Audio_)mmc5Audio_->reset(); }
    expansionChr_.fill(0); expansionPrg_.fill(0);
    expansionMirror_ = initialMirroring_ == Mirroring::Horizontal ? 1 : 0;
    if (mapper_ == 71 && submapper_ == 1) expansionMirror_ = 2;
    expansionRamControl_ = fme7Command_ = fme7IrqControl_ = 0;
    fme7Prg6000_ = 0x40; fme7Counter_ = 0;
    vrc7Counter_ = vrc7Latch_ = vrc7IrqControl_ = 0; vrc7Prescaler_ = 341;
    // submapper0 обозначает старую неоднозначную плату: совместимое OR A3/A4.
    vrc7AddressMask_ = submapper_ == 1 ? 0x08 : (submapper_ == 2 ? 0x10 : 0x18);
    sunsoft5bAudio_.reset(); vrc7Audio_.reset();
    vrc7Writer_=nullptr;vrc7TraceFault_=Vrc7WriteTape::Fault::None;
    vrc7ShadowRemaining_=64;externalVrc7_=false;
    bank_ = 0;
    mmc1Shift_ = 0x10; mmc1Control_ = 0x0c;
    mmc1Chr0_ = mmc1Chr1_ = mmc1Prg_ = 0;
    mmc1LastWrite_ = ~std::uint64_t{0};
    mmc3Select_ = 0; mmc3Mirror_ = initialMirroring_ == Mirroring::Horizontal;
    mmc3Ram_ = 0x80;
    mmc3Banks_ = {0, 2, 4, 5, 6, 7, 0, 1};
    irqLatch_ = irqCounter_ = 0;
    irqReload_ = irqEnabled_ = irqPending_ = a12_ = false;
    a12LowDots_ = 16;
    legacy_.reset(prgSize_, chrSize_);
    refreshPrgPages();
    refreshChrPages();
}

/**
 * @brief Перестраивает прямые указатели банков после изменения отображения картриджа.
 * @details Содержимое ROM не копируется; обновление должно предшествовать следующему доступу с новой банковой раскладкой.
 */
void Cartridge::refreshPrgPages() noexcept {
    if (mapper_ == 5 && mmc5_) {
        for(unsigned n=0;n<4;++n)prgPages_[n]=mmc5_->prgPage(n);
        cpuPrgTail_.refresh(prgPages_);
        return;
    }
    if(!prg_){prgPages_.fill(nullptr);cpuPrgTail_.refresh(prgPages_);return;}
    // Каждое поддерживаемое PRG-окно кратно 8 KiB; iNES PRG-длины
    // кратны 16 KiB, поэтому каждая кэшированная страница остаётся в своём ROM.
    for (unsigned page = 0; page < prgPages_.size(); ++page)
        prgPages_[page] = prg_ + prgOffset(static_cast<std::uint16_t>(0x8000U + page * 8192U));
    cpuPrgTail_.refresh(prgPages_); // Только переназначение указателей; без копирования при смене банка.
}

/**
 * @brief Перестраивает прямые указатели банков после изменения отображения картриджа.
 * @details Содержимое ROM не копируется; обновление должно предшествовать следующему доступу с новой банковой раскладкой.
 */
void Cartridge::refreshChrPages() noexcept {
    // Любая смена физических CHR-страниц инвалидирует опубликованный view;
    // это рабочий контракт Frame, а не переключатель профиля native-тестов.
    chrMappingDirty_ = true;
    chrPages_.fill(nullptr);
    chrRamPages_.fill(nullptr);
    if (!prg_) return;
    for (unsigned page = 0; page < chrPages_.size(); ++page) {
        const auto offset = chrOffset(static_cast<std::uint16_t>(page * 1024U));
        if (chr_) chrPages_[page] = chr_ + offset;
        else {
            chrRamPages_[page] = chrRam_.data() + offset;
            chrPages_[page] = chrRamPages_[page];
        }
    }
}

/**
 * @brief Возвращает текущую характеристику подключённого картриджа.
 * @details Признак PAL из заголовка не подменяет явно выбранный режим Machine; loaded описывает действительность backing текущего картриджа.
 * @return Вычисленное значение или состояние типа Mirroring согласно описанному контракту.
 */
Mirroring Cartridge::mirroring() const noexcept {
    if (fourScreen_) return Mirroring::FourScreen;
    if (mapper_ == 7) return (bank_ & 0x10) ? Mirroring::Single1 : Mirroring::Single0;
    if (mapper_ == 1) {
        constexpr Mirroring modes[]{Mirroring::Single0, Mirroring::Single1,
                                     Mirroring::Vertical, Mirroring::Horizontal};
        return modes[mmc1Control_ & 3];
    }
    if (mapper_ == 4) return mmc3Mirror_ ? Mirroring::Horizontal : Mirroring::Vertical;
    if (mapper_ == 69 || mapper_ == 71 || mapper_ == 85) {
        // Значение 0 выводит PPU A10 на CIRAM A10, значение 1 — PPU A11.
        constexpr Mirroring modes[]{Mirroring::Vertical, Mirroring::Horizontal, Mirroring::Single0, Mirroring::Single1};
        return modes[expansionMirror_ & 3U];
    }
    if (mapper_ == 160) {
        // D001 выбирает CIRAM A10: PPU A10, PPU A11, земля, Vcc.
        // Названия раскладок в документации ASIC не являются названиями mirroring.
        constexpr Mirroring modes[]{Mirroring::Vertical, Mirroring::Horizontal, Mirroring::Single0, Mirroring::Single1};
        return modes[legacy_.mirrorMode()];
    }
    return initialMirroring_;
}

/**
 * @brief Вычисляет физическое смещение для текущей банковской раскладки.
 * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
 */
std::size_t Cartridge::prgOffset(std::uint16_t address) const noexcept {
    const std::size_t a = address - 0x8000;
    if (mapper_ == 160) return legacy_.prgOffset(address);
    if (mapper_ == 69 || mapper_ == 85) {
        const unsigned slot = static_cast<unsigned>(a / 8192);
        const std::size_t bank = slot == 3 ? prgSize_ / 8192 - 1 : expansionPrg_[slot] & 0x3fU;
        return (bank * 8192 + (a & 8191U)) % prgSize_;
    }
    if (mapper_ == 2 || mapper_ == 71) {
        const std::size_t b = address < 0xc000 ? (bank_ & 0x0f) : prgSize_ / 16384 - 1;
        return (b * 16384 + (a & 0x3fff)) % prgSize_;
    }
    if (mapper_ == 7) return ((bank_ & 7) * 32768 + a) % prgSize_;
    if (mapper_ == 1) {
        const auto mode = (mmc1Control_ >> 2) & 3;
        const std::size_t b = mmc1Prg_ & 0x0f;
        if (mode < 2) return ((b & ~std::size_t{1}) * 16384 + a) % prgSize_;
        if (mode == 2) return ((address < 0xc000 ? 0 : b) * 16384 + (a & 0x3fff)) % prgSize_;
        return ((address < 0xc000 ? b : prgSize_ / 16384 - 1) * 16384 + (a & 0x3fff)) % prgSize_;
    }
    if (mapper_ == 4) {
        const auto last = prgSize_ / 8192 - 1;
        const std::size_t slot = a / 8192;
        const bool swap = (mmc3Select_ & 0x40) != 0;
        const std::size_t banks[]{swap ? last - 1 : std::size_t(mmc3Banks_[6] & 0x3f),
                                 std::size_t(mmc3Banks_[7] & 0x3f),
                                 swap ? std::size_t(mmc3Banks_[6] & 0x3f) : last - 1, last};
        return (banks[slot] * 8192 + (a & 8191)) % prgSize_;
    }
    return a % prgSize_;
}

/**
 * @brief Один раз выбирает описание источника для окна $6000.
 * @details Описание действительно пока тот же владелец не меняет регистры картриджа; это не право кэшировать изменяемые байты.
 * @return Вычисленное значение или состояние типа Cartridge::Cpu6000Source согласно описанному контракту.
 */
Cartridge::Cpu6000Source Cartridge::selectCpu6000Source() const noexcept {
    if (!prg_) return {nullptr, Cpu6000Kind::OpenBus};
    if(mapper_==5){
        const auto* page=mmc5_?mmc5_->cpu6000Page():nullptr;
        return {page,page?Cpu6000Kind::Ram:Cpu6000Kind::OpenBus};
    }
    if (mapper_ == 69) {
        if (!(fme7Prg6000_ & 0x40U))
            return {prg_ + (std::size_t(fme7Prg6000_ & 0x3fU) * 8192U) % prgSize_, Cpu6000Kind::Rom};
        if (!(fme7Prg6000_ & 0x80U) || !expansionRamBytes_) return {nullptr, Cpu6000Kind::OpenBus};
    }
    if (mapper_ == 71 || (mapper_ == 7 && submapper_ != 0) ||
        (mapper_ == 85 && (!(expansionRamControl_ & 0x80U) || !expansionRamBytes_)))
        return {nullptr, Cpu6000Kind::OpenBus};
    if (mapper_ == 160 && legacy_.mapsPrgAt6000()) return {prg_, Cpu6000Kind::LegacyPrg};
    if ((mapper_ == 1 && (mmc1Prg_ & 0x10)) ||
        (mapper_ == 4 && !(mmc3Ram_ & 0x80))) return {nullptr, Cpu6000Kind::OpenBus};
    return {prgRam_.data(), Cpu6000Kind::Ram};
}
/**
 * @brief Читает байт по уже выбранному описанию источника $6000.
 * @details Не выполняет повторный выбор mapper и сохраняет исходную семантику RAM, ROM либо открытой шины.
 * @param offset Смещение внутри соответствующего физического диапазона.
 * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 * @param source Ранее выбранное описание источника CPU $6000; действительно только пока тот же владелец не изменяет mapping.
 */
std::uint8_t Cartridge::readCpu6000Source(const Cpu6000Source& source,
                                        std::uint16_t offset, std::uint8_t openBus) const noexcept {
    if (source.kind == Cpu6000Kind::OpenBus) return openBus;
    if (source.kind == Cpu6000Kind::LegacyPrg)
        return source.data[legacy_.prgOffset(static_cast<std::uint16_t>(0x6000U + offset))];
    return source.data[offset];
}
/**
 * @brief Копирует окно $6000-$7FFF выбранного картриджа в память вызывающего.
 * @details Выбор источника соответствует обычному cpuRead; не должен сбрасывать IRQ или изменять mapper.
 * @param destination Приёмный диапазон вызывающего, не перекрывающий действующие заимствованные источники.
 * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
 */
void Cartridge::copyCpu6000Window(std::uint8_t* destination, std::uint8_t openBus) const noexcept {
    const auto source = selectCpu6000Source();
    if (source.kind == Cpu6000Kind::OpenBus) { std::memset(destination, openBus, 8192); return; }
    if (source.kind == Cpu6000Kind::Ram || source.kind == Cpu6000Kind::Rom) { std::memcpy(destination, source.data, 8192); return; }
    const auto first = legacy_.prgOffset(0x6000), last = legacy_.prgOffset(0x7FFF);
    // В этом фиксированном Legacy-окне offset(n)=(K+n)%period. Разность концов
    // 8191 доказывает отсутствие wrap; проверка extent удерживает каждый копируемый байт в ROM.
    // Содержимое не исследуется; нечётные/малые/нулевые периоды сохраняют побайтовые чтения.
    if (prgSize_ >= 8192 && first <= prgSize_ - 8192 && last == first + 8191) {
        std::memcpy(destination, source.data + first, 8192); return;
    }
    for (unsigned n = 0; n < 8192; ++n)
        destination[n] = readCpu6000Source(source, static_cast<std::uint16_t>(n), openBus);
}

/**
 * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
 * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
/** @brief Заимствует полностью подключённую страницу для записи в текущем тихом блоке. */
Cartridge::QuietWram6000 Cartridge::quietWritableWram6000() noexcept {
    if (!prg_) return {};
    if (mapper_ == 5U) {
        // У MMC5 тот же декодер CE и защиты, что в обычной cpuWrite.
        // Только полная страница во внутреннем массиве Cartridge может быть заимствована.
        auto* const page = mmc5_ ? mmc5_->writablePrg(QuietWram6000::base) : nullptr;
        return {page == prgRam_.data() ? page : nullptr};
    }
    if ((mapper_!=69U && mapper_!=85U) || expansionRamBytes_!=prgRam_.size()) return {};
    const auto source=selectCpu6000Source();
    if (source.kind!=Cpu6000Kind::Ram || source.data!=prgRam_.data()) return {};
    return {prgRam_.data()};
}

/** @brief Выбирает источник по адресу CPU. Для MMC5 учитывает подтверждение IRQ и побочные эффекты PCM, для остальных плат читает выбранный банк или открытую шину. */
std::uint8_t Cartridge::cpuRead(std::uint16_t address, std::uint8_t openBus) const noexcept {
    if (!prg_) return openBus;
    if (mapper_ == 5) return readMmc5Cpu(address,openBus);
    if(address>=0x8000)return cpuPrgTail_.pages[(address>>13)&3U][address&8191U];
    if (address >= 0x6000)
        return readCpu6000Source(selectCpu6000Source(), static_cast<std::uint16_t>(address & 8191U), openBus);
    if (mapper_ == 160) return legacy_.cpuRead(address, openBus);
    return openBus;
}

/**
 * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
 * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 */
void Cartridge::cpuWrite(std::uint16_t address, std::uint8_t value, std::uint64_t cycle) noexcept {
    if (!prg_) return;
    if (mapper_ == 5) {
        if(mmc5Audio_ && address>=0x5000 && address<=0x5015) mmc5Audio_->cpuWrite(address,value);
        else if(mmc5_)mmc5_->cpuWrite(address,value);
        if(address==0x5100 || (address>=0x5113 && address<=0x5117))refreshPrgPages();
        return;
    }
    if (mapper_ == 69 || mapper_ == 85) {
        if (address >= 0x6000 && address < 0x8000) {
            const bool enabled = mapper_ == 69 ? (fme7Prg6000_ & 0xc0U) == 0xc0U
                                               : (expansionRamControl_ & 0x80U) != 0;
            if (enabled && expansionRamBytes_) prgRam_[address & 8191U] = value;
        } else if (address >= 0x8000) {
            if (mapper_ == 69) writeFme7(address, value); else writeVrc7(address,value,cycle);
        }
        return;
    }
    if (mapper_ == 71) {
        if (address >= 0xc000) {
            // BF9093 подключает четыре линии банка, BF9097 подключает три.
            const auto selected = static_cast<std::uint8_t>(value & (submapper_ == 1 ? 7U : 15U));
            if (bank_ != selected) {
                bank_ = selected;
                refreshPrgPages();
            }
        } else if (address >= camericaMirrorStart_ && address < 0xa000) {
            expansionMirror_ = static_cast<std::uint8_t>(2U + ((value >> 4) & 1U));
        }
        return;
    }
    if (mapper_ == 160) {
        if (address >= 0x6000 && address < 0x8000) {
            if (!legacy_.mapsPrgAt6000()) prgRam_[address & 8191] = value;
        } else {
            legacy_.write(address, value);
            refreshChrPages();
            const auto decoded = address & 0xF803U;
            if ((address & 0xF800U) == 0x8000U || decoded == 0xD000U || decoded == 0xD003U)
                refreshPrgPages();
        }
        return;
    }
    if (address >= 0x6000 && address < 0x8000) {
        if (mapper_ == 7 && submapper_ != 0) return;
        if (mapper_ == 1 && (mmc1Prg_ & 0x10)) return;
        if (mapper_ == 4 && ((mmc3Ram_ & 0xc0) != 0x80)) return;
        prgRam_[address & 8191] = value;
    } else if (address >= 0x8000) {
        if (mapper_ == 7) {
            writeAxromLatch(axromLatchValue(address, value));
        } else if (mapper_ == 2 || mapper_ == 3) {
            bank_ = value;
            if (mapper_ == 3) refreshChrPages();
            else refreshPrgPages();
        }
        else if (mapper_ == 1) {
            const bool consecutive = mmc1LastWrite_ != ~std::uint64_t{0} && cycle == mmc1LastWrite_ + 1;
            mmc1LastWrite_ = cycle;
            if (value & 0x80) { mmc1Shift_ = 0x10; mmc1Control_ |= 0x0c; refreshPrgPages(); return; }
            if (consecutive) return;
            const bool complete = (mmc1Shift_ & 1) != 0;
            mmc1Shift_ = static_cast<std::uint8_t>((mmc1Shift_ >> 1) | ((value & 1) << 4));
            if (complete) {
                switch ((address >> 13) & 3) {
                    case 0: mmc1Control_ = mmc1Shift_; refreshPrgPages(); refreshChrPages(); break;
                    case 1: mmc1Chr0_ = mmc1Shift_; refreshChrPages(); break;
                    case 2: mmc1Chr1_ = mmc1Shift_; refreshChrPages(); break;
                    case 3: mmc1Prg_ = mmc1Shift_; refreshPrgPages(); break;
                }
                mmc1Shift_ = 0x10;
            }
        } else if (mapper_ == 4) mmc3Write(address, value);
    }
}

/**
 * @brief Вычисляет физическое смещение для текущей банковской раскладки.
 * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
 */
std::size_t Cartridge::chrOffset(std::uint16_t address) const noexcept {
    if(mapper_==5&&mmc5_)return mmc5_->chrOffset(address);
    std::size_t offset = address & 8191;
    if (mapper_ == 160) return legacy_.chrOffset(address) % (chrSize_ ? chrSize_ : chrRam_.size());
    if (mapper_ == 69 || mapper_ == 85) {
        offset = std::size_t(expansionChr_[(address >> 10) & 7U]) * 1024U + (address & 1023U);
    } else if (mapper_ == 1) {
        const std::size_t bank = (mmc1Control_ & 0x10) ?
            (address < 0x1000 ? mmc1Chr0_ : mmc1Chr1_) :
            (mmc1Chr0_ & 0x1e) + (address >> 12);
        offset = bank * 4096 + (address & 4095);
    } else if (mapper_ == 3) offset += (bank_ & 3) * 8192;
    else if (mapper_ == 4) {
        const auto a = static_cast<std::uint16_t>(address ^ ((mmc3Select_ & 0x80) ? 0x1000 : 0));
        std::size_t bank;
        if (a < 0x0800) bank = (mmc3Banks_[0] & 0xfe) + ((a >> 10) & 1);
        else if (a < 0x1000) bank = (mmc3Banks_[1] & 0xfe) + ((a >> 10) & 1);
        else bank = mmc3Banks_[2 + ((a - 0x1000) >> 10)];
        offset = bank * 1024 + (a & 1023);
    }
    return offset % (chrSize_ ? chrSize_ : chrRam_.size());
}

/**
 * @brief Обслуживает отображённый доступ к памяти CHR картриджа.
 * @details CHR-ROM не становится изменяемой RAM; банковая логика и стробовые защёлки выполняются на своих исходных границах.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Cartridge::chrRead(std::uint16_t address) const noexcept {
    // Читать страницу, выбранную до этой PPU-передачи. ppuReadStrobe обновляет
    // необязательные mapper160-защёлки лишь после получения байта вызывающим.
    if (!prg_) return chr_ ? chr_[chrOffset(address)] : chrRam_[chrOffset(address)];
    return chrPages_[(address >> 10) & 7U][address & 1023U];
}
/**
 * @brief Обслуживает отображённый доступ к памяти CHR картриджа.
 * @details CHR-ROM не становится изменяемой RAM; банковая логика и стробовые защёлки выполняются на своих исходных границах.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Cartridge::chrWrite(std::uint16_t address, std::uint8_t value) noexcept {
    if (chr_) return;
    if (mapper_ == 160 || !prg_) {
        if (mapper_ != 160 || legacy_.chrWriteEnabled()) chrRam_[chrOffset(address)] = value;
    } else chrRamPages_[(address >> 10) & 7U][address & 1023U] = value;
}
/**
 * @brief Декодирует запись регистров MMC3 и обновляет связанные отображения/IRQ.
 * @details Не заменяет последующие PPU-такты; CHR/PRG-страницы обновляются при изменении банков.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */

/** @details FME-7 command13 подтверждает IRQ независимо от оставленных enable bits. */
void Cartridge::writeFme7(std::uint16_t address, std::uint8_t value) noexcept {
    switch (address & 0xe000U) {
    case 0x8000: fme7Command_ = value & 15U; return;
    case 0xc000: sunsoft5bAudio_.cpuWrite(0xc000, value); return;
    case 0xe000: sunsoft5bAudio_.cpuWrite(0xe000, value); return;
    default: break;
    }
    if (fme7Command_ < 8) {
        expansionChr_[fme7Command_] = value; refreshChrPages();
    } else if (fme7Command_ == 8) fme7Prg6000_ = value;
    else if (fme7Command_ <= 11) {
        expansionPrg_[fme7Command_ - 9] = value & 0x3fU; refreshPrgPages();
    } else if (fme7Command_ == 12) expansionMirror_ = value & 3U;
    else if (fme7Command_ == 13) { fme7IrqControl_ = value; irqPending_ = false; }
    else if (fme7Command_ == 14) fme7Counter_ = (fme7Counter_ & 0xff00U) | value;
    else fme7Counter_ = (fme7Counter_ & 255U) | (std::uint16_t(value) << 8);
}
/** @details Точный NES2 decode не смешивает платы 1/2; sub0 сохраняет историческое OR. */
/** @brief Cold setup после настоящего full reset7; повторный joined arm не меняет FM phase. */
bool Cartridge::armExternalVrc7(std::uint64_t cycle)noexcept{
    if(externalVrc7_)return vrc7TraceFault_==Vrc7WriteTape::Fault::None;
    if(!hasVrc7Fm()||cycle!=7U||vrc7Audio_.nextPortableOutputCpuCycles()!=57U)return false;
    vrc7Writer_=nullptr;vrc7ShadowRemaining_=57;externalVrc7_=true;return true;
}
/** @brief Начатый caller-slot не переиспользуется до внешнего release; здесь нет callback. */
bool Cartridge::bindVrc7Writer(Vrc7WriteTape*tape)noexcept{
    if(!externalVrc7_||vrc7TraceFault_!=Vrc7WriteTape::Fault::None)return false;
    if(vrc7Writer_||!tape||!tape->recording()){failVrc7Trace(Vrc7WriteTape::Fault::MissingWriter);return false;}
    vrc7Writer_=tape;return true;
}
/** @brief Запись после actualM2, включая RMW duplicates и sound-resetE000; digital-эффекты не отменяются. */
void Cartridge::recordVrc7Write(std::uint64_t cycle,std::uint16_t address,std::uint8_t value)noexcept{
    if(vrc7TraceFault_!=Vrc7WriteTape::Fault::None)return;
    if(!vrc7Writer_||!vrc7Writer_->recording()){failVrc7Trace(Vrc7WriteTape::Fault::MissingWriter);return;}
    if(!vrc7Writer_->append(cycle,address,value))failVrc7Trace(vrc7Writer_->fault());
}
/** @brief Срез закрывается только после finalmetadata на том же audioEnd; fault не освобождает writer. */
bool Cartridge::sealVrc7Writer(Vrc7WriteTape*tape,std::uint64_t end)noexcept{
    if(!externalVrc7_||vrc7TraceFault_!=Vrc7WriteTape::Fault::None)return false;
    if(!tape||vrc7Writer_!=tape){failVrc7Trace(Vrc7WriteTape::Fault::MissingWriter);return false;}
    if(!tape->seal(end)){failVrc7Trace(tape->fault());return false;}
    vrc7Writer_=nullptr;return true;
}
/** @brief Декодирует линии адреса VRC7, изменяет банки и IRQ. Для регистра FM сохраняет гостевой такт записи, по которому строится звуковой кадр. */
void Cartridge::writeVrc7(std::uint16_t address,std::uint8_t value,std::uint64_t cycle)noexcept {
    const unsigned high = address & 0xf000U;
    const unsigned sub = (address & vrc7AddressMask_) != 0;
    if (high == 0x8000 || (high == 0x9000 && !sub)) {
        expansionPrg_[high == 0x9000 ? 2 : sub] = value & 0x3fU;
        refreshPrgPages();
    } else if (high == 0x9000 && sub) {
        if(submapper_!=1){
            const auto canonical=std::uint16_t((address&0x20U)?0x9030U:0x9010U);
            if(externalVrc7_)recordVrc7Write(cycle,canonical,value);
            else vrc7Audio_.cpuWrite(canonical,value);
        }
    } else if (high >= 0xa000 && high <= 0xd000) {
        expansionChr_[((high - 0xa000U) >> 11) + sub] = value;
        refreshChrPages();
    } else if (high == 0xe000 && !sub) {
        expansionMirror_ = value & 3U; expansionRamControl_ = value;
        if(externalVrc7_)recordVrc7Write(cycle,0xe000U,value);
        else vrc7Audio_.setMuted((value&0x40U)!=0);
    } else if (high == 0xe000) vrc7Latch_ = value;
    else if (high == 0xf000 && !sub) {
        vrc7IrqControl_ = value & 7U; irqPending_ = false;
        vrc7Prescaler_ = 341;
        if (value & 2U) vrc7Counter_ = vrc7Latch_;
    } else if (high == 0xf000) {
        irqPending_ = false;
        vrc7IrqControl_ = (vrc7IrqControl_ & ~2U) | ((vrc7IrqControl_ & 1U) << 1);
    }
}
/** @details Quiet grant исключает такт IRQ; отключённый counter заморожен аппаратно. */
unsigned Cartridge::expansionIrqQuietCycles() const noexcept {
    if (irqPending_) return ~0U;
    if (mapper_ == 69)
        return (fme7IrqControl_ & 0x81U) == 0x81U ? fme7Counter_ : ~0U;
    if (!(vrc7IrqControl_ & 2U)) return ~0U;
    const unsigned ticks = 256U - vrc7Counter_;
    const unsigned until = (vrc7IrqControl_ & 4U) ? ticks : (vrc7Prescaler_ + (ticks - 1U) * 341U + 2U) / 3U;
    return until - 1U;
}
/** @details Эквивалент count serial M2; арифметика не теряет overflow при больших quiet spans. */
void Cartridge::clockExpansionCpu(unsigned count) noexcept {
    if (mapper_ == 69) {
        sunsoft5bAudio_.clockCpu(count);
        if (!(fme7IrqControl_ & 0x80U)) return;
        if (count > fme7Counter_ && (fme7IrqControl_ & 1U)) irqPending_ = true;
        fme7Counter_ = static_cast<std::uint16_t>(fme7Counter_ - count);
        return;
    }
    if(submapper_!=1){
        if(externalVrc7_){
            if(count){
                if(!vrc7Writer_||!vrc7Writer_->recording())
                    failVrc7Trace(vrc7Writer_&&vrc7Writer_->fault()!=Vrc7WriteTape::Fault::None?vrc7Writer_->fault():Vrc7WriteTape::Fault::MissingWriter);
                // Только старый portable64 deadline; синтез/patch/boxcar находится у CPU0.
                const unsigned remaining=vrc7ShadowRemaining_;
                vrc7ShadowRemaining_=static_cast<std::uint8_t>(count<remaining?remaining-count:64U-(count-remaining)%64U);
            }
        }else vrc7Audio_.clockCpu(count);
    }
    if (!(vrc7IrqControl_ & 2U)) return;
    std::uint64_t clocks = count;
    if (!(vrc7IrqControl_ & 4U)) {
        const std::uint64_t dots = std::uint64_t(count) * 3U;
        clocks = dots < vrc7Prescaler_ ? 0 : 1U + (dots - vrc7Prescaler_) / 341U;
        vrc7Prescaler_ = static_cast<std::uint16_t>(vrc7Prescaler_ + clocks * 341U - dots);
    }
    const unsigned until = 256U - vrc7Counter_;
    if (clocks >= until) {
        irqPending_ = true;
        vrc7Counter_ = static_cast<std::uint8_t>(vrc7Latch_ + (clocks - until) % (256U - vrc7Latch_));
    } else vrc7Counter_ = static_cast<std::uint8_t>(vrc7Counter_ + clocks);
}

/** @brief Декодирует регистры MMC3 по маске $E001. При смене режима банков пересчитывает таблицы страниц, при записи в регистры IRQ обновляет счётчик и линию прерывания. */
void Cartridge::mmc3Write(std::uint16_t address, std::uint8_t value) noexcept {
    switch (address & 0xe001) {
        case 0x8000: {
            const auto changed = mmc3Select_ ^ value;
            mmc3Select_ = value;
            if (changed & 0x40U) refreshPrgPages();
            if (changed & 0x80U) refreshChrPages();
            break;
        }
        case 0x8001:
            mmc3Banks_[mmc3Select_ & 7] = value;
            if ((mmc3Select_ & 7) < 6) refreshChrPages();
            else refreshPrgPages();
            break;
        case 0xa000: mmc3Mirror_ = value & 1; break;
        case 0xa001: mmc3Ram_ = value; break;
        case 0xc000: irqLatch_ = value; break;
        case 0xc001: irqReload_ = true; break;
        case 0xe000: irqEnabled_ = irqPending_ = false; break;
        case 0xe001: irqEnabled_ = true; break;
    }
}
/**
 * @brief Продвигает временное состояние картриджа на одну точку PPU.
 * @details Сохраняет возраст A12 и фильтрацию фронтов MMC3, даже когда видеоданные не выводятся.
 */
void Cartridge::ppuCycle() noexcept {
    if (!a12_ && a12LowDots_ < 0xffff) ++a12LowDots_;
}
/**
 * @brief Публикует текущий адрес на шине PPU.
 * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 */
void Cartridge::ppuAddress(std::uint16_t address) noexcept {
    if (mapper_ == 160) { legacy_.ppuAddress(address); return; }
    if (mapper_ != 4) return;
    const bool high = (address & 0x1000) != 0;
    if (high && !a12_) {
        // Три спада M2 соответствуют примерно восьми точкам PPU.
        if (a12LowDots_ >= 8) {
            if (irqCounter_ == 0 || irqReload_) irqCounter_ = irqLatch_;
            else --irqCounter_;
            irqReload_ = false;
            if (irqCounter_ == 0 && irqEnabled_) irqPending_ = true;
        }
    }
    if (high != a12_) a12LowDots_ = 0;
    a12_ = high;
}

/**
 * @brief Возвращает состояние или результат текущей привязки.
 * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
 * @param r Код результата разбора iNES/допуска mapper; преобразуется в статическое диагностическое сообщение.
 * @return Указатель на статическую строку; память не освобождается вызывающим.
 */
const char* Cartridge::errorText(LoadResult r) const noexcept {
    switch (r) {
        case LoadResult::Ok: return "OK";
        case LoadResult::InvalidHeader: return "Invalid NES header";
        case LoadResult::Truncated: return "Truncated NES image";
        case LoadResult::UnsupportedMapper: return "Mapper is not implemented";
        case LoadResult::MissingMapperStorage: return "MMC5 state storage is not bound";
        case LoadResult::UnsupportedFormat: return "Unsupported cartridge variant";
    }
    return "Unknown cartridge error";
}
} // пространство имён nes::core
