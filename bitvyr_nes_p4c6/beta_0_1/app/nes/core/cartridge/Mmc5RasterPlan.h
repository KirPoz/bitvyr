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
 * При смене owner/version/CIRAM/control нужна новая подготовка. Версия 0 не
 * разрешает cache hit даже при численно совпавшем теге. SplitY/IRQ/Name state
 * намеренно не заморожены и принадлежат единственному живому mapper-владельцу.
 */
class Mmc5RasterPlan final {
public:
    /** @brief Биты policy: bit0 extended attributes, bit1 разрешён вертикальный split. */
    enum class Policy : std::uint8_t { Normal=0, Extended=1, SplitNormal=2, SplitExtended=3 };

    /** @brief Восемь байтов отложенных tile-latches одного quiet span; ROM/mapper не владеет. */
    struct DeferredLatches {
        std::uint16_t tileNtOffset_ = 0;
        std::uint8_t tileAttribute_ = 0, tileSplitY_ = 0, tileSplitAttribute_ = 0;
        bool tileExtended_ = false, tileSplit_ = false;
    };
    static_assert(sizeof(DeferredLatches) == 8, "deferred MMC5 tile state must remain eight bytes");

    /**
     * @brief Заимствованные строки Normal-policy одного синхронного quiet span.
     * @details Шесть указателей не копируют ROM/NT. До следующего CPU/MMIO
     * источники неизменны; горизонтальный NT-wrap меняет только две NT строки.
     * Объект полностью заполняет prepareNormalRow, default содержимое не читать.
     */
    struct NormalRowView {
        const std::uint8_t* names;
        const std::uint8_t* attributes;
        std::array<const std::uint8_t*, 4> patterns;
        std::uint8_t nameFallback, attributeFallback, attributeYShift;

        /**
         * @brief Читает четыре прежних data bytes и упаковывает palette2/raw-name.
         * @param coarseX Текущий coarse X0..31 до прежнего incX.
         * @pre Строки относятся к текущему NT/fineY; CPU/MMIO и /RD внутри нет.
         */
        std::uint32_t fetchRecord(unsigned coarseX) const noexcept {
            const auto tile = names ? names[coarseX] : nameFallback;
            const auto attribute = attributes ? attributes[coarseX >> 2U] : attributeFallback;
            const unsigned palette = (unsigned(attribute) >> (attributeYShift | (coarseX & 2U))) & 3U;
            const auto* const row = patterns[tile >> 6U] + ((unsigned(tile) & 63U) << 4U);
            return std::uint32_t(row[0]) | (std::uint32_t(row[8]) << 8U) |
                (std::uint32_t(palette) << 16U) | (std::uint32_t(tile) << 24U);
        }
    };
    static_assert(sizeof(void*) != 4 || sizeof(NormalRowView) == 28,
                  "normal MMC5 row view must remain twenty-eight RV32 bytes");

    /**
     * @brief Подготавливает только Normal-строку через существующие NT/CHR mapping helpers.
     * @param scroll Loopy v первого quiet fetch после настоящего head при наличии.
     * @param view Заимствованный output, полностью записанный перед использованием.
     * @pre Prepared policy Normal; positive span не пересекает incY до последнего fetch.
     */
    void prepareNormalRow(std::uint16_t scroll, NormalRowView& view) const noexcept {
        rebindNormalNametableRow(scroll, view);
        const unsigned fineY = (scroll >> 12U) & 7U;
        for (unsigned page = 0; page < 4U; ++page)
            view.patterns[page] = chr_ + normalOffset<true>(
                static_cast<std::uint16_t>(patternTable_ | (page << 10U) | fineY));
        view.attributeYShift = static_cast<std::uint8_t>((scroll >> 4U) & 4U);
    }

    /**
     * @brief Обновляет NT row pointers после прежнего горизонтального incX-wrap.
     * @details fineY/CHR и вертикальная часть attribute shift внутри строки прежние.
     * Name на coarseY30/31 выбирает attribute fallback по реальному offset.
     */
    void rebindNormalNametableRow(std::uint16_t scroll, NormalRowView& view) const noexcept {
        const auto source = nametableSource((scroll >> 10U) & 3U);
        const unsigned nameOffset = scroll & 0x03e0U;
        view.names = source.data ? source.data + nameOffset : nullptr;
        view.attributes = source.data ? source.data + 0x03c0U + ((scroll >> 4U) & 0x38U) : nullptr;
        view.nameFallback = source.fallback(nameOffset);
        view.attributeFallback = source.fallback(0x03c0U);
    }

    /**
     * @brief Восстанавливает raw ExRAM/false флаги последнего Normal Name общим алгоритмом.
     * @pre Нет наблюдателя промежуточных quiet latches; вызвать до publish/finalizer.
     * @details Старые split Y/offset/attribute сохраняются самим nameDataState.
     */
    void finishNormalRowName(std::uint16_t scroll, unsigned tileX, DeferredLatches& state) const noexcept {
        (void)nameDataState<Policy::Normal>(static_cast<std::uint16_t>(0x2000U | (scroll & 0x0fffU)), tileX, state);
    }

    /** @brief Снимает шесть живых tile-latches после реального head; prepared plan должен быть действителен. */
    DeferredLatches captureDeferredLatches() const noexcept {
        const auto& m = *owner_;
        return {m.tileNtOffset_, m.tileAttribute_, m.tileSplitY_, m.tileSplitAttribute_, m.tileExtended_, m.tileSplit_};
    }
    /**
     * @brief Публикует последний quiet tile и сохранённые last-active split поля один раз до endpoint.
     * @pre Между capture/commit нет наблюдателя mapper, CPU/MMIO или смены prepared epoch.
     */
    void publishDeferredLatches(const DeferredLatches& state) noexcept {
        auto& m = *owner_;
        m.tileExtended_ = state.tileExtended_; m.tileAttribute_ = state.tileAttribute_;
        m.tileSplit_ = state.tileSplit_; m.tileSplitY_ = state.tileSplitY_;
        m.tileNtOffset_ = state.tileNtOffset_; m.tileSplitAttribute_ = state.tileSplitAttribute_;
    }

    /** @brief Возвращает выбранную при подготовке policy; читать только после true prepare. */
    Policy policy() const noexcept { return policy_; }

    /**
     * @brief Получает целый BG-тайл без повторных enum/bank/NT policy проходов.
     * @tparam P Policy, совпадающая с policy() подготовленного плана.
     * @param scroll Loopy v до Name, включая fine Y и nametable bits.
     * @param tileX Настоящая экранная колонка 0..33, в том числе prefetch0/1.
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
     * @brief Выполняет прежние quiet data fetches с value-latches и сразу выбирает двухбитную palette.
     * @param scroll Loopy v перед текущим Name; tileX сохраняет прежнюю split колонку.
     * @param state Состояние всего непрерывного span, захваченное после реального head при наличии.
     * @return Packed low/high/palette2/raw-name в байтах 0/1/2/3 для прежнего fused pixel pipeline.
     * @pre Prepared policy верна, CPU/MMIO/mapper observers до publish отсутствуют; /RD не создаются.
     */
    template<Policy P>
    std::uint32_t fetchDeferredRecord(std::uint16_t scroll, unsigned tileX, DeferredLatches& state) const noexcept {
        const auto nameAddress = static_cast<std::uint16_t>(0x2000U | (scroll & 0x0fffU));
        const auto tile = nameDataState<P>(nameAddress, tileX, state);
        const auto attributeAddress = static_cast<std::uint16_t>(0x23c0U |
            (scroll & 0x0c00U) | ((scroll >> 4U) & 0x38U) | ((scroll >> 2U) & 7U));
        const unsigned shift = ((scroll >> 4U) & 4U) | (scroll & 2U);
        const auto palette = attributeDataState<P, true>(attributeAddress, state, shift);
        const auto lowAddress = static_cast<std::uint16_t>(patternTable_ |
            (unsigned(tile) << 4U) | ((scroll >> 12U) & 7U));
        const auto offset = patternOffsetState<P>(lowAddress, state);
        const auto low = chr_[offset], high = chr_[offset + 8U];
        return std::uint32_t(low) | (std::uint32_t(high) << 8U) |
            (std::uint32_t(palette) << 16U) | (std::uint32_t(tile) << 24U);
    }

    /**
     * @brief Исполняет настоящую Name-фазу без Machine/Fetch enum redispatch.
     * @tparam P Policy текущей подготовленной эпохи.
     * @param address Настоящий PPU-адрес; зеркалируется до 14 бит как прежний ppuRead.
     * @param tileX Экранная колонка выборки 0..33 для split/prefetch latch.
     * @return Байт после ровно одного исходного /RD; palette-область возвращает 0 без /RD.
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
     * @param address Настоящий PPU-адрес до 14-битного зеркалирования.
     * @return Байт прежнего ppuRead после одного /RD, либо 0 для palette.
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
     * @return Байт прежнего ppuRead после одного /RD, либо 0 для palette.
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

    /** @brief Один prepared NT source и общий выбор zero/fill по физическому offset. */
    struct NametableSource {
        const std::uint8_t* data;
        std::uint8_t name, attribute;
        /** @brief Возвращает прежний null-page байт, включая Name в attribute-области. */
        std::uint8_t fallback(unsigned offset) const noexcept { return offset < 0x03c0U ? name : attribute; }
    };

    /** @brief Заимствует уже разрешённую NT-страницу; ntMap/exMode заново не декодируются. */
    NametableSource nametableSource(unsigned page) const noexcept {
        return {ntPages_[page], nameFallback_[page], attributeFallback_[page]};
    }

    /**
     * @brief Читает уже разрешённый NT источник без повторного ntMap/exMode decode.
     * @param address Нормализованный PPU-адрес; младшие 10 бит задают offset.
     * @return Живой байт CIRAM/ExRAM либо подготовленное fill/zero значение.
     * @details Даже Name на coarseY30/31 может попасть в область атрибутов;
     * fill выбирается по фактическому offset, а не по имени fetch-фазы.
     */
    std::uint8_t nametable(std::uint16_t address) const noexcept {
        const unsigned page = (address >> 10U) & 3U;
        const unsigned offset = address & 1023U;
        const auto source = nametableSource(page);
        return source.data ? source.data[offset] : source.fallback(offset);
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
     * @param tileX Экранная колонка 0..33.
     * @return Name byte из исходного NT mapping либо split ExRAM.
     * @details Общий helper whole/partial путей сохраняет все old latch stores,
     * включая tileAttribute в normal режиме и неактивные старые split поля.
     */
    template<Policy P>
    std::uint8_t nameData(std::uint16_t address, unsigned tileX) noexcept {
        return nameDataState<P>(address, tileX, *owner_);
    }

    /** @brief Единый Name-algorithm: изменяемые tile поля адресуются live либо value состоянием. */
    template<Policy P, class TileState>
    std::uint8_t nameDataState(std::uint16_t address, unsigned tileX, TileState& state) const noexcept {
        static_assert(static_cast<unsigned>(P) < 4U, "MMC5 raster policy");
        if (address < 0x2000U) return chr_[normalOffset<false>(address)];
        auto& mapper = *owner_;
        state.tileExtended_ = (static_cast<unsigned>(P) & 1U) != 0;
        state.tileAttribute_ = exRam_[address & 1023U];
        const bool split = (static_cast<unsigned>(P) & 2U) && tileX < 32U &&
            (splitRight_ ? tileX >= splitThreshold_ : tileX < splitThreshold_);
        state.tileSplit_ = split;
        if (split) {
            auto y = mapper.splitY_;
            if (mapper.inFrame_ && (tileX < 2U || mapper.nameRepeat_ == 3U))
                y = y == 239U ? 0U : static_cast<std::uint8_t>(y + 1U);
            state.tileSplitY_ = y;
            state.tileNtOffset_ = static_cast<std::uint16_t>((unsigned(y) >> 3U) * 32U + tileX);
            const unsigned index = 0x3c0U + (unsigned(y) >> 5U) * 8U + (tileX >> 2U);
            const unsigned shift = ((unsigned(y) >> 3U) & 2U) * 2U + (tileX & 2U);
            state.tileSplitAttribute_ = static_cast<std::uint8_t>(((exRam_[index] >> shift) & 3U) * 0x55U);
            return exRam_[state.tileNtOffset_];
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
        return attributeDataState<P, false>(address, *owner_, 0);
    }

    /** @brief Выбирает raw атрибут либо palette2 одним общим policy алгоритмом без повторного 0x55 expansion. */
    template<Policy P, bool Selected, class TileState>
    std::uint8_t attributeDataState(std::uint16_t address, const TileState& state, unsigned shift) const noexcept {
        if (address < 0x2000U) {
            const auto value = chr_[normalOffset<false>(address)];
            if constexpr (Selected) return static_cast<std::uint8_t>((value >> shift) & 3U);
            else return value;
        }
        if constexpr ((static_cast<unsigned>(P) & 2U) != 0)
            if (state.tileSplit_) {
                if constexpr (Selected) return state.tileSplitAttribute_ & 3U;
                else return state.tileSplitAttribute_;
            }
        if constexpr ((static_cast<unsigned>(P) & 1U) != 0)
            if (state.tileExtended_) {
                if constexpr (Selected) return state.tileAttribute_ >> 6U;
                else return static_cast<std::uint8_t>((state.tileAttribute_ >> 6U) * 0x55U);
            }
        const auto value = nametable(address);
        if constexpr (Selected) return static_cast<std::uint8_t>((value >> shift) & 3U);
        else return value;
    }

    /**
     * @brief Разрешает pattern по live tile-latch и неизменяемым CHR базам.
     * @tparam P Подготовленная policy.
     * @param address Фактический CHR-адрес ниже$2000, включая low/high bit3.
     * @return Физический offset с исходным wrap подключённого CHR-ROM.
     */
    template<Policy P>
    std::size_t patternOffset(std::uint16_t address) const noexcept {
        return patternOffsetState<P>(address, *owner_);
    }

    /** @brief Общая CHR-address логика читает соответствующий live/value tile state, сохраняя stale split поля. */
    template<Policy P, class TileState>
    std::size_t patternOffsetState(std::uint16_t address, const TileState& state) const noexcept {
        if constexpr ((static_cast<unsigned>(P) & 2U) != 0) {
            if (state.tileSplit_) {
                unsigned within = address & 0x0fffU;
                if (splitSlMode_) within = (within & ~7U) | (state.tileSplitY_ & 7U);
                return (splitBase_ + within) & chrMask_;
            }
        }
        if constexpr ((static_cast<unsigned>(P) & 1U) != 0)
            if (state.tileExtended_)
                return (extendedBase_ + (std::uint32_t(state.tileAttribute_ & 63U) << 12U) +
                        (address & 0x0fffU)) & chrMask_;
        return normalOffset<true>(address);
    }

    Mmc5* owner_;
    const std::uint8_t* chr_;
    const std::uint8_t* exRam_;
    std::array<const std::uint8_t*, 4> ntPages_;
    const std::uint32_t* normalA_;
    const std::uint32_t* normalBackground_;
    std::uint32_t chrMask_, splitBase_, extendedBase_;
    std::array<std::uint8_t, 4> nameFallback_, attributeFallback_;
    std::uint16_t patternTable_;
    Policy policy_;
    std::uint8_t splitThreshold_;
    bool splitRight_, splitSlMode_;
};

} // namespace nes::core
