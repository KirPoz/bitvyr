/**
 * @file AudioRenderer.cpp
 * @brief Независимый APU-consumer неизменяемого retire-пакета.
 * @ingroup nes_apu
 */
#include "AudioRenderer.h"

namespace nes::core {
namespace {
/** @brief PRIVATE-политика только звукового consumer; цифровой APU и layout не меняются. */
[[maybe_unused]] constexpr bool nativeTriangleMean =
    true;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void AudioRenderer::reset() noexcept {
    apu_.reset();packet_=nullptr;generation_=nextSequence_=lastSequence_=ppuFrame_=0;
    dmcRead_=dmcStall_=0;region_=Region::Ntsc;valid_=poisoned_=dmcFault_=false;
    capabilities_=0;
}
/**
 * @brief Доводит независимый звуковой renderer до абсолютного гостевого времени.
 * @details Не исполняет CPU повторно; сохраняет NTSC/PAL-фазу, DMC и порядок пакета.
 * @param target Абсолютный гостевой CPU-такт звукового renderer; движение назад запрещено.
 */
void AudioRenderer::advance(std::uint64_t target) noexcept {
    const auto count=static_cast<unsigned>(target-cycles());
    apu_.advanceRendererCpuCycles(count, nativeTriangleMean);
}
/**
 * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
 * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t AudioRenderer::dmcRead(std::uint16_t address) noexcept {
    if(!packet_||dmcRead_>=packet_->header().dmcCount){dmcFault_=true;return 0;}
    const auto& event=packet_->dmc()[dmcRead_++];
    if(address!=event.address||packet_->header().baseCycle+event.offset!=cycles())dmcFault_=true;
    if(packet_->header().capabilities&AudioPacket::externalDacQ16)
        apu_.setExpansionInputAtDmcFetch(event.expansionLevel);
    return event.value;
}
/**
 * @brief Учитывает украденные DMC такты в установленном порядке.
 * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 */
void AudioRenderer::dmcStall(unsigned count) noexcept {
    if(!packet_||!dmcRead_||dmcStall_>=dmcRead_){dmcFault_=true;return;}
    const auto& event=packet_->dmc()[dmcRead_-1];
    if(count!=event.stall||packet_->header().baseCycle+event.offset!=cycles())dmcFault_=true;
    ++dmcStall_; // CPU-владелец уже оплатил эти украденные такты.
}
/**
 * @brief Восстанавливает звук либо изображение из проверенного входного пакета.
 * @details Проверяет идентичность и временной порядок. Неполный или ошибочный пакет не является готовым результатом.
 * @return Вычисленное значение или состояние типа AudioRenderResult согласно описанному контракту.
 * @param packet Запечатанный неизменяемый AudioPacket с реальными DMC-байтами; сохраняется до завершения render.
 */
AudioRenderResult AudioRenderer::render(const AudioPacket& packet) noexcept {
    valid_=false;
    const auto& h=packet.header();
    if(!h.generation||h.fault!=AudioFault::None||h.endCycle<h.baseCycle||h.endCycle-h.baseCycle>65535||
        h.inputCount>AudioPacket::inputCapacity||h.dmcCount>AudioPacket::dmcCapacity
        ||h.expansionCount>AudioPacket::expansionCapacity||h.formatVersion!=AudioPacket::expansionVersion||
        (h.capabilities&~AudioPacket::externalDacQ16)||(!h.capabilities&&h.expansionCount)
        )return AudioRenderResult::InvalidPacket;
    if(h.generation<generation_)return AudioRenderResult::StaleGeneration;
    const bool fresh=h.generation>generation_;
    if(fresh){if(h.sequence||h.baseCycle)return AudioRenderResult::SequenceMismatch;}
    else if(poisoned_)return AudioRenderResult::NeedsReset;
    else if(h.sequence!=nextSequence_)return AudioRenderResult::SequenceMismatch;
    else if(h.baseCycle!=cycles()||h.region!=region_)return AudioRenderResult::TimelineMismatch;
    if(!fresh&&h.capabilities!=capabilities_)return AudioRenderResult::TimelineMismatch;
    for(unsigned i=0;i<h.inputCount;++i){const auto& e=packet.inputs()[i];
        if(e.offset>h.endCycle-h.baseCycle||(i&&e.offset<packet.inputs()[i-1].offset)||
            (e.operation>23&&e.operation!=AudioPacket::readStatusOperation&&e.operation!=AudioPacket::clearSamplesOperation))return AudioRenderResult::InvalidPacket;
    }
    for(unsigned i=0;i<h.dmcCount;++i){const auto& e=packet.dmc()[i];
        if(e.offset>h.endCycle-h.baseCycle||(i&&e.offset<packet.dmc()[i-1].offset)||!e.stall)return AudioRenderResult::InvalidPacket;
        if(e.reserved||!AudioPacket::validExpansionLevel(e.expansionLevel)||(!h.capabilities&&e.expansionLevel))return AudioRenderResult::InvalidPacket;
    }
    // Проверка всего merge до reset/mutation: prefix кодирует точный append order.
    unsigned native=0;std::uint16_t lastOffset=0,lastPrefix=0;
    for(unsigned i=0;i<h.expansionCount;++i){const auto&e=packet.expansions()[i];
        if(e.offset>h.endCycle-h.baseCycle||e.nativePrefix>h.inputCount||e.nativePrefix<lastPrefix||
           !AudioPacket::validExpansionLevel(e.level))return AudioRenderResult::InvalidPacket;
        while(native<e.nativePrefix){const auto&in=packet.inputs()[native++];
            if(in.offset<lastOffset)return AudioRenderResult::InvalidPacket;
            lastOffset=in.offset;}
        if(e.offset<lastOffset)return AudioRenderResult::InvalidPacket;
        lastOffset=e.offset;lastPrefix=e.nativePrefix;
    }
    for(;native<h.inputCount;++native){if(packet.inputs()[native].offset<lastOffset)return AudioRenderResult::InvalidPacket;lastOffset=packet.inputs()[native].offset;}
    if(fresh){apu_.reset(h.region==Region::Pal);generation_=h.generation;nextSequence_=0;region_=h.region;poisoned_=false;}
    capabilities_=h.capabilities;
    packet_=&packet;dmcRead_=dmcStall_=0;dmcFault_=false;
    bool statusFault=false;
    /** @brief Повторяет только прежнюю native operation; timestamp-фаза не меняется. */
    const auto applyNative=[&](const AudioPacket::Input&e)noexcept{advance(h.baseCycle+e.offset);
        if(e.operation==AudioPacket::clearSamplesOperation)apu_.clearSamples();
        else if(e.operation==AudioPacket::readStatusOperation){if(apu_.readStatus()!=e.value)statusFault=true;}
        else apu_.write(static_cast<std::uint16_t>(0x4000+e.operation),e.value);
    };
    native=0;
    for(unsigned i=0;i<h.expansionCount;++i){const auto&e=packet.expansions()[i];
        while(native<e.nativePrefix)applyNative(packet.inputs()[native++]);
        advance(h.baseCycle+e.offset);
        // Любое соседнее событие того же timestamp сохраняет append-order и
        // старый общий tick. Последний tick пакета нельзя заранее записывать:
        // следующий пакет может начать с native write при том же абсолютном U.
        const bool nativeAtStamp=(native&&packet.inputs()[native-1U].offset==e.offset)||
            (native<h.inputCount&&packet.inputs()[native].offset==e.offset);
        const bool repeatedStamp=(i&&packet.expansions()[i-1U].offset==e.offset)||
            (i+1U<h.expansionCount&&packet.expansions()[i+1U].offset==e.offset);
        if(h.baseCycle+e.offset<h.endCycle&&!nativeAtStamp&&!repeatedStamp&&
           apu_.trySetExpansionInputQuiet(e.level, nativeTriangleMean))continue;
        apu_.setExpansionInput(e.level);
    }
    while(native<h.inputCount)applyNative(packet.inputs()[native++]);
    advance(h.endCycle);packet_=nullptr;
    if(dmcFault_||dmcRead_!=h.dmcCount||dmcStall_!=h.dmcCount){poisoned_=true;return AudioRenderResult::DmcMismatch;}
    if(statusFault){poisoned_=true;return AudioRenderResult::StatusMismatch;}
    valid_=true;lastSequence_=h.sequence;ppuFrame_=h.ppuFrame;++nextSequence_;return AudioRenderResult::Ok;
}
} // пространство имён nes::core
