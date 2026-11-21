#pragma once
#include "AudioPacket.h"
#include <cstddef>

namespace nes::core {
/**
 * @brief Не владеющее immutable представление ТОЛЬКО занятых аудиозаписей.
 * @details Указатели не являются wire-форматом. Caller владеет Header/типизированными
 * arrays, подтверждает их настоящий extent и сохраняет неизменными до полного render.
 * Generation/ROM lease/session/frame ticket и TXdone проверяет внешний адаптер.
 * Этот класс не публикует пакет, не отменяет job и не разрешает освобождение буферов.
 */
class AudioPacketView final {
public:
    /**
     * @brief Заимствует Header и точные used counts, не копируя ёмкость AudioPacket.
     * @param header Неизменяемый Header; nullptr допускается как отвергаемое представление.
     * @param input Typed array не менее inputCount записей; nullptr допустим только при0.
     * @param inputCount Число доступных использованных Input, обязано совпасть с Header.
     * @param dmc Typed array не менее dmcCount фактических DMC ответов/stalls.
     * @param dmcCount Число доступных использованных DMC, обязано совпасть с Header.
     */
    AudioPacketView(const AudioPacket::Header* header,const AudioPacket::Input* input,std::size_t inputCount,
        const AudioPacket::Dmc* dmc,std::size_t dmcCount
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        ,const AudioPacket::Expansion* expansion,std::size_t expansionCount
#endif
        ) noexcept:header_(header),input_(input),inputCount_(inputCount),dmc_(dmc),dmcCount_(dmcCount)
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        ,expansion_(expansion),expansionCount_(expansionCount)
#endif
        {}
    /** @brief Заимствует уже опубликованный full packet без копии; не читает активную Machine. */
    static AudioPacketView from(const AudioPacket& packet) noexcept {
        return {&packet.header(),packet.inputs(),packet.header().inputCount,packet.dmc(),packet.header().dmcCount
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
            ,packet.expansions(),packet.header().expansionCount
#endif
        };
    }
    /**
     * @brief Проверяет nullable pointers/declared extents/counts ДО первого чтения записей.
     * @details Не доказывает физическую доступность произвольного адреса; это предусловие
     * caller. Лимиты исходного протокола сохраняются; числа799/800 не являются лимитами кадра.
     */
    [[nodiscard]] bool readable() const noexcept {
        return header_&&inputCount_==header_->inputCount&&inputCount_<=AudioPacket::inputCapacity&&
            (!inputCount_||input_)&&dmcCount_==header_->dmcCount&&dmcCount_<=AudioPacket::dmcCapacity&&(!dmcCount_||dmc_)
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
            &&expansionCount_==header_->expansionCount&&expansionCount_<=AudioPacket::expansionCapacity&&(!expansionCount_||expansion_)
#endif
            ;
    }
    /** @brief Возвращает заимствованный Header; разрешено только после readable()==true. */
    [[nodiscard]] const AudioPacket::Header& header()const noexcept{return *header_;}
    /** @brief Возвращает immutable used Input array, живущий до полного возврата render. */
    [[nodiscard]] const AudioPacket::Input* inputs()const noexcept{return input_;}
    /** @brief Возвращает immutable used DMC array; ROM повторно не читается. */
    [[nodiscard]] const AudioPacket::Dmc* dmc()const noexcept{return dmc_;}
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    /** @brief Возвращает immutable used external-DAC array с исходным nativePrefix order. */
    [[nodiscard]] const AudioPacket::Expansion* expansions()const noexcept{return expansion_;}
#endif
private:
    const AudioPacket::Header* header_{};
    const AudioPacket::Input* input_{};
    std::size_t inputCount_{};
    const AudioPacket::Dmc* dmc_{};
    std::size_t dmcCount_{};
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    const AudioPacket::Expansion* expansion_{};
    std::size_t expansionCount_{};
#endif
};
} // namespace nes::core
