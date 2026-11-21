/**
 * @file AudioRenderer.cpp
 * @brief Независимый APU-consumer неизменяемого retire-пакета.
 * @ingroup nes_apu
 */
#include "AudioRenderer.h"

namespace nes::core {

/** @brief Вычисляет cold CRC реальных LUT; обновляет по байтам LE16 без padding и alias casts. */
void AudioRenderer::diagnosticTableCrcs(std::uint32_t(&out)[6])const noexcept {
    const auto byte=[](std::uint32_t c,std::uint8_t b)noexcept {c^=b;for(unsigned i=0;i<8;++i)c=(c>>1U)^(0xedb88320U&(0U-(c&1U)));return c;};
    const auto word=[&](std::uint32_t c,std::uint16_t v)noexcept {return byte(byte(c,std::uint8_t(v)),std::uint8_t(v>>8U));};
    const auto&t=apu_.tables_;std::uint32_t pulse=~0U,tnd=~0U,impulse=~0U;
    for(const auto v:t.pulse)pulse=word(pulse,v);
    for(const auto v:t.tnd)tnd=word(tnd,v);
    for(const auto&row:t.impulse)for(const auto v:row)impulse=word(impulse,std::uint16_t(v));
    out[0]=std::uint32_t(t.pulse.size());out[1]=~pulse;out[2]=std::uint32_t(t.tnd.size());out[3]=~tnd;
    out[4]=std::uint32_t(t.impulse.size()*t.impulse[0].size());out[5]=~impulse;
}
namespace {
/** @brief PRIVATE-политика только звукового consumer; цифровой APU и layout не меняются. */
[[maybe_unused]] constexpr bool nativeTriangleMean =
#if defined(NES_CORE_PORTABLE_NATIVE_TRIANGLE_MEAN) && NES_CORE_PORTABLE_NATIVE_TRIANGLE_MEAN
    true;
#else
    false;
#endif
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void AudioRenderer::reset() noexcept {
    apu_.reset();packet_=nullptr;generation_=nextSequence_=lastSequence_=ppuFrame_=0;
    dmcRead_=dmcStall_=0;region_=Region::Ntsc;valid_=poisoned_=dmcFault_=false;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    capabilities_=0;
#endif
}
/**
 * @brief Доводит независимый звуковой renderer до абсолютного гостевого времени.
 * @details Не исполняет CPU повторно; сохраняет NTSC/PAL-фазу, DMC и порядок пакета.
 * @param target Абсолютный гостевой CPU-такт звукового renderer; движение назад запрещено.
 */
void AudioRenderer::advance(std::uint64_t target) noexcept {
    const auto count=static_cast<unsigned>(target-cycles());
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
#if defined(NES_CORE_APU_TRIANGLE_SEGMENTS) && NES_CORE_APU_TRIANGLE_SEGMENTS
#if defined(NES_CORE_APU_RENDERER_DEADLINES) && NES_CORE_APU_RENDERER_DEADLINES
    apu_.advanceRendererCpuCycles(count, nativeTriangleMean);
#elif defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE
    // Несвязанные интервалы направить в прежний планировщик без входа
    // в необязательный renderer-helper. Защитная проверка callee сохраняется.
    if (count >= 8 && !apu_.triangle_.period && apu_.triangle_.length)
        apu_.advanceRendererCpuCycles(count, nativeTriangleMean);
    else apu_.advanceCpuCycles(count, nativeTriangleMean);
#else
    apu_.advanceRendererCpuCycles(count, nativeTriangleMean);
#endif
#else
    apu_.advanceCpuCycles(count, nativeTriangleMean);
#endif
#else
    for(unsigned i=0;i<count;++i)apu_.tick();
#endif
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
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    if(packet_->header().capabilities&AudioPacket::externalDacQ16)
        apu_.setExpansionInputAtDmcFetch(event.expansionLevel);
#endif
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
    const auto view=AudioPacketView::from(packet);
    return render(view);
}

/** @brief Прежний renderer использует только immutable used view; SDK/wire/lease ему не принадлежат. */
AudioRenderResult AudioRenderer::render(const AudioPacketView& packet) noexcept {
    valid_=false;
    if(!packet.readable())return AudioRenderResult::InvalidPacket;
    const auto& h=packet.header();
    if(!h.generation||h.fault!=AudioFault::None||h.endCycle<h.baseCycle||h.endCycle-h.baseCycle>65535||
        h.inputCount>AudioPacket::inputCapacity||h.dmcCount>AudioPacket::dmcCapacity
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        ||h.expansionCount>AudioPacket::expansionCapacity||h.formatVersion!=AudioPacket::expansionVersion||
        (h.capabilities&~AudioPacket::externalDacQ16)||(!h.capabilities&&h.expansionCount)
#endif
        )return AudioRenderResult::InvalidPacket;
    if(h.generation<generation_)return AudioRenderResult::StaleGeneration;
    const bool fresh=h.generation>generation_;
    if(fresh){if(h.sequence||h.baseCycle)return AudioRenderResult::SequenceMismatch;}
    else if(poisoned_)return AudioRenderResult::NeedsReset;
    else if(h.sequence!=nextSequence_)return AudioRenderResult::SequenceMismatch;
    else if(h.baseCycle!=cycles()||h.region!=region_)return AudioRenderResult::TimelineMismatch;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    if(!fresh&&h.capabilities!=capabilities_)return AudioRenderResult::TimelineMismatch;
#endif
    for(unsigned i=0;i<h.inputCount;++i){const auto& e=packet.inputs()[i];
        if(e.offset>h.endCycle-h.baseCycle||(i&&e.offset<packet.inputs()[i-1].offset)||
            (e.operation>23&&e.operation!=AudioPacket::readStatusOperation&&e.operation!=AudioPacket::clearSamplesOperation))return AudioRenderResult::InvalidPacket;
    }
    for(unsigned i=0;i<h.dmcCount;++i){const auto& e=packet.dmc()[i];
        if(e.offset>h.endCycle-h.baseCycle||(i&&e.offset<packet.dmc()[i-1].offset)||!e.stall)return AudioRenderResult::InvalidPacket;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        if(e.reserved||!AudioPacket::validExpansionLevel(e.expansionLevel)||(!h.capabilities&&e.expansionLevel))return AudioRenderResult::InvalidPacket;
#endif
    }
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
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
#endif
    if(fresh){apu_.reset(h.region==Region::Pal);generation_=h.generation;nextSequence_=0;region_=h.region;poisoned_=false;}
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    capabilities_=h.capabilities;
#endif
    packet_=&packet;dmcRead_=dmcStall_=0;dmcFault_=false;
    bool statusFault=false;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
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
#if defined(NES_CORE_APU_EXPANSION_QUIET) && NES_CORE_APU_EXPANSION_QUIET
        // Любое соседнее событие того же timestamp сохраняет append-order и
        // старый общий tick. Последний tick пакета нельзя заранее записывать:
        // следующий пакет может начать с native write при том же абсолютном U.
        const bool nativeAtStamp=(native&&packet.inputs()[native-1U].offset==e.offset)||
            (native<h.inputCount&&packet.inputs()[native].offset==e.offset);
        const bool repeatedStamp=(i&&packet.expansions()[i-1U].offset==e.offset)||
            (i+1U<h.expansionCount&&packet.expansions()[i+1U].offset==e.offset);
        if(h.baseCycle+e.offset<h.endCycle&&!nativeAtStamp&&!repeatedStamp&&
           apu_.trySetExpansionInputQuiet(e.level, nativeTriangleMean))continue;
#endif
        apu_.setExpansionInput(e.level);
    }
    while(native<h.inputCount)applyNative(packet.inputs()[native++]);
#else
    for(unsigned i=0;i<h.inputCount;++i){const auto& e=packet.inputs()[i];advance(h.baseCycle+e.offset);
        if(e.operation==AudioPacket::clearSamplesOperation)apu_.clearSamples();
        else if(e.operation==AudioPacket::readStatusOperation){if(apu_.readStatus()!=e.value)statusFault=true;}
        else apu_.write(static_cast<std::uint16_t>(0x4000+e.operation),e.value);
    }
#endif
    advance(h.endCycle);packet_=nullptr;
    if(dmcFault_||dmcRead_!=h.dmcCount||dmcStall_!=h.dmcCount){poisoned_=true;return AudioRenderResult::DmcMismatch;}
    if(statusFault){poisoned_=true;return AudioRenderResult::StatusMismatch;}
    if(apu_.fixedFilterFault_){poisoned_=true;return AudioRenderResult::FixedFilterFault;}
    valid_=true;lastSequence_=h.sequence;ppuFrame_=h.ppuFrame;++nextSequence_;return AudioRenderResult::Ok;
}
} // пространство имён nes::core
