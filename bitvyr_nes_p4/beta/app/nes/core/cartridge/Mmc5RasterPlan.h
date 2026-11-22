/**
 * @file Mmc5RasterPlan.h
 * @brief Подготовленная адресация синхронного участка MMC5 без SDK и копий ROM.
 * @details Заимствованные policy-дескрипторы сохраняют CPU/MMIO-эпоху владельца.
 * Цифровой detector остаётся единственным Mmc5::ppuReadStrobe; live tile-latches
 * обновляются после настоящего /RD, а whole-fetch требует сертификата caller.
 */
#pragma once

#include "Mmc5.h"

namespace nes::core {

/**
 * @brief Заимствованные базы и редкая policy вместо повторного декодирования каждого fetch.
 * @details Не содержит памяти ROM/CIRAM/ExRAM и не выделяет heap. Успешный
 * prepareRasterPlan полностью инициализирует используемые поля. Неподготовленный
 * default объект читать нельзя; caller хранит отдельный результат подготовки.
 * При смене owner/version/CIRAM/control нужна новая подготовка. Версия0 не
 * разрешает cache hit даже при численно совпавшем теге. SplitY/IRQ/Name state
 * намеренно не заморожены и принадлежат единственному живому mapper-владельцу.
 */
class Mmc5RasterPlan final {
public:
    /** @brief Биты policy: bit0 extended attributes, bit1 разрешён вертикальный split. */
    enum class Policy : std::uint8_t { Normal=0, Extended=1, SplitNormal=2, SplitExtended=3 };

    /** @brief Возвращает выбранную при подготовке policy; читать только после true prepare. */
    Policy policy() const noexcept { return policy_; }

    /**
     * @brief Получает целый BG-тайл без повторных enum/bank/NT policy проходов.
     * @tparam P Policy, совпадающая с policy() подготовленного плана.
     * @param scroll Loopy v до Name, включая fine Y и nametable bits.
     * @param tileX Настоящая экранная колонка0..33, в том числе prefetch0/1.
     * @param needPlanes false оставляет CHR байты непрочитанными; offset/latches прежние.
     * @return Прежние Name/attribute/low/high и физический offset младшей plane.
     * @pre Нет CPU/MMIO между четырьмя fetch; caller доказал quiet digital-domain.
     * @details Этот метод, как прежний ppuReadQuietBackgroundTile, не создаёт /RD.
     * Caller материализует прежний detector/M2 конец через finishQuietBackground.
     */
    template<Policy P>
    Mmc5::BackgroundTile fetch(std::uint16_t scroll, unsigned tileX,
                               bool needPlanes=true) noexcept {
        const auto nameAddress = static_cast<std::uint16_t>(0x2000U | (scroll & 0x0fffU));
        const auto tile = nameData<P>(nameAddress, tileX);
        const auto attributeAddress = static_cast<std::uint16_t>(0x23c0U |
            (scroll & 0x0c00U) | ((scroll >> 4U) & 0x38U) | ((scroll >> 2U) & 7U));
        const auto attribute = attributeData<P>(attributeAddress);
        const auto lowAddress = static_cast<std::uint16_t>(patternTable_ |
            (unsigned(tile) << 4U) | ((scroll >> 12U) & 7U));
        const auto offset = patternOffset<P>(lowAddress);
        if (!needPlanes) return {tile, attribute, 0, 0, offset};
        return {tile, attribute, chr_[offset], chr_[offset + 8U], offset};
    }

    /**
     * @brief Исполняет настоящую Name-фазу без Machine/Fetch enum redispatch.
     * @tparam P Policy текущей подготовленной эпохи.
     * @param address Настоящий PPU-адрес; зеркалируется до14бит как прежний ppuRead.
     * @param tileX Экранная колонка выборки0..33 для split/prefetch latch.
     * @return Байт после ровно одного исходного /RD; palette-область возвращает0 без /RD.
     * @details Detector может сменить splitY/inFrame прежде Name-latch: эти поля
     * читаются live после strobe, а не из подготовленного policy-снимка.
     */
    template<Policy P>
    std::uint8_t readName(std::uint16_t address, unsigned tileX) noexcept {
        address &= 0x3fffU;
        if (address >= 0x3f00U) return 0;
        owner_->ppuReadStrobe(address);
        return nameData<P>(address, tileX);
    }

    /**
     * @brief Исполняет настоящую Attribute-фазу с live per-tile подстановкой.
     * @tparam P Policy текущей подготовленной эпохи.
     * @param address Настоящий PPU-адрес до14битного зеркалирования.
     * @return Байт прежнего ppuRead после одного /RD, либо0 для palette.
     * @details CHR-адрес в необычном partial prefix сохраняет обычный наборA,
     * а detector edge может отменить старые tile-latches до чтения атрибута.
     */
    template<Policy P>
    std::uint8_t readAttribute(std::uint16_t address) noexcept {
        address &= 0x3fffU;
        if (address >= 0x3f00U) return 0;
        owner_->ppuReadStrobe(address);
        return attributeData<P>(address);
    }

    /**
     * @brief Исполняет настоящую BG-pattern фазу через подготовленные CHR базы.
     * @tparam P Policy текущей подготовленной эпохи.
     * @param address Настоящий low/high адрес; исходные NT/palette случаи сохраняются.
     * @return Байт прежнего ppuRead после одного /RD, либо0 для palette.
     * @details Необычный NT-адрес не считается CHR. Это существенно для начала
     * partial после CPU PPUADDR/PPUDATA, когда old busAddress ещё не канонический.
     */
    template<Policy P>
    std::uint8_t readPattern(std::uint16_t address) noexcept {
        address &= 0x3fffU;
        if (address >= 0x3f00U) return 0;
        owner_->ppuReadStrobe(address);
        return address < 0x2000U ? chr_[patternOffset<P>(address)] : nametable(address);
    }

private:
    friend class Mmc5;
    friend class TimingPpu;

    /**
     * @brief Читает уже разрешённый NT источник без повторного ntMap/exMode decode.
     * @param address Нормализованный PPU-адрес; младшие10бит задают offset.
     * @return Живой байт CIRAM/ExRAM либо подготовленное fill/zero значение.
     * @details Даже Name на coarseY30/31 может попасть в область атрибутов;
     * fill выбирается по фактическому offset, а не по имени fetch-фазы.
     */
    std::uint8_t nametable(std::uint16_t address) const noexcept {
        const unsigned page = (address >> 10U) & 3U;
        const unsigned offset = address & 1023U;
        const auto* const data = ntPages_[page];
        return data ? data[offset] : (offset < 0x3c0U ? nameFallback_[page] : attributeFallback_[page]);
    }

    /**
     * @brief Возвращает физическое normal-CHR смещение подготовленного набора.
     * @tparam Background true использует нормальный BG-набор, false — наборA.
     * @param address Нормализованный CHR-адрес ниже$2000.
     * @return Смещение живого CHR-ROM; подготовка доказала backing степени двух.
     */
    template<bool Background>
    std::size_t normalOffset(std::uint16_t address) const noexcept {
        const unsigned slot = address >> 10U;
        if constexpr (Background) return normalBackground_[slot] + (address & 1023U);
        else return normalA_[slot] + (address & 1023U);
    }

    /**
     * @brief Выполняет единственный Name-latch алгоритм подготовленного участка.
     * @tparam P Policy, совпадающая с policy().
     * @param address Фактический Name-адрес; CHR-случай не изменяет tile-latches.
     * @param tileX Экранная колонка0..33.
     * @return Name byte из исходного NT mapping либо split ExRAM.
     * @details Общий helper whole/partial путей сохраняет все old latch stores,
     * включая tileAttribute в normal режиме и неактивные старые split поля.
     */
    template<Policy P>
    std::uint8_t nameData(std::uint16_t address, unsigned tileX) noexcept {
        static_assert(static_cast<unsigned>(P) < 4U, "MMC5 raster policy");
        if (address < 0x2000U) return chr_[normalOffset<false>(address)];
        auto& mapper = *owner_;
        mapper.tileExtended_ = (static_cast<unsigned>(P) & 1U) != 0;
        mapper.tileAttribute_ = exRam_[address & 1023U];
        const bool split = (static_cast<unsigned>(P) & 2U) && tileX < 32U &&
            (splitRight_ ? tileX >= splitThreshold_ : tileX < splitThreshold_);
        mapper.tileSplit_ = split;
        if (split) {
            auto y = mapper.splitY_;
            if (mapper.inFrame_ && (tileX < 2U || mapper.nameRepeat_ == 3U))
                y = y == 239U ? 0U : static_cast<std::uint8_t>(y + 1U);
            mapper.tileSplitY_ = y;
            mapper.tileNtOffset_ = static_cast<std::uint16_t>((unsigned(y) >> 3U) * 32U + tileX);
            const unsigned index = 0x3c0U + (unsigned(y) >> 5U) * 8U + (tileX >> 2U);
            const unsigned shift = ((unsigned(y) >> 3U) & 2U) * 2U + (tileX & 2U);
            mapper.tileSplitAttribute_ = static_cast<std::uint8_t>(((exRam_[index] >> shift) & 3U) * 0x55U);
            return exRam_[mapper.tileNtOffset_];
        }
        return nametable(address);
    }

    /**
     * @brief Выбирает live split/extended attribute без повторного чтения global policy.
     * @tparam P Подготовленная policy.
     * @param address Фактический attribute-адрес; CHR-случай читает наборA.
     * @return Атрибут, совпадающий с прежним readPpuData.
     */
    template<Policy P>
    std::uint8_t attributeData(std::uint16_t address) const noexcept {
        if (address < 0x2000U) return chr_[normalOffset<false>(address)];
        if constexpr ((static_cast<unsigned>(P) & 2U) != 0)
            if (owner_->tileSplit_) return owner_->tileSplitAttribute_;
        if constexpr ((static_cast<unsigned>(P) & 1U) != 0)
            if (owner_->tileExtended_) return static_cast<std::uint8_t>((owner_->tileAttribute_ >> 6U) * 0x55U);
        return nametable(address);
    }

    /**
     * @brief Разрешает pattern по live tile-latch и неизменяемым CHR базам.
     * @tparam P Подготовленная policy.
     * @param address Фактический CHR-адрес ниже$2000, включая low/high bit3.
     * @return Физический offset с исходным wrap подключённого CHR-ROM.
     */
    template<Policy P>
    std::size_t patternOffset(std::uint16_t address) const noexcept {
        if constexpr ((static_cast<unsigned>(P) & 2U) != 0) {
            if (owner_->tileSplit_) {
                unsigned within = address & 0x0fffU;
                if (splitSlMode_) within = (within & ~7U) | (owner_->tileSplitY_ & 7U);
                return (splitBase_ + within) & chrMask_;
            }
        }
        if constexpr ((static_cast<unsigned>(P) & 1U) != 0)
            if (owner_->tileExtended_)
                return (extendedBase_ + (std::uint32_t(owner_->tileAttribute_ & 63U) << 12U) +
                        (address & 0x0fffU)) & chrMask_;
        return normalOffset<true>(address);
    }

    Mmc5* owner_;
    const std::uint8_t* chr_;
    const std::uint8_t* exRam_;
    std::array<const std::uint8_t*, 4> ntPages_;
#if defined(NES_CORE_MMC5_NORMAL_CHR_CACHE) && NES_CORE_MMC5_NORMAL_CHR_CACHE
    const std::uint32_t* normalA_;
    const std::uint32_t* normalBackground_;
#else
    std::array<std::uint32_t, 8> normalA_;
    std::array<std::uint32_t, 8> normalBackground_;
#endif
    std::uint32_t chrMask_, splitBase_, extendedBase_;
    std::array<std::uint8_t, 4> nameFallback_, attributeFallback_;
    std::uint16_t patternTable_;
    Policy policy_;
    std::uint8_t splitThreshold_;
    bool splitRight_, splitSlMode_;
};

} // namespace nes::core
