#pragma once
#include "FrameFmBridge.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <new>

namespace nes::app::c6_async {
/** @brief Cold Main allocation только PSRAM/8bit; step/build не аллоцируют и SDK не вызывают. */
template<class Pipeline> FrameFmBridge<Pipeline>*createFrameFmBridgePsram()noexcept{
    using Bridge=FrameFmBridge<Pipeline>;
    auto*memory=heap_caps_aligned_alloc(alignof(Bridge),sizeof(Bridge),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory)return nullptr;
    const auto*last=static_cast<const std::uint8_t*>(memory)+sizeof(Bridge)-1U;
    if(!esp_ptr_external_ram(memory)||!esp_ptr_external_ram(last)||reinterpret_cast<std::uintptr_t>(memory)%alignof(Bridge)){
        heap_caps_free(memory);return nullptr;
    }
    return ::new(memory)Bridge{};
}
/**
 * @brief Удаляет только пустой/drained FM bridge; не умеет добывать Worker join/PPA/UNBOUND.
 * @pre Root уже получил actualWorkerDone, остановил consumers/PPA/C6 и вернул source/Tape.
 * Отказ/timeout не дают права free. Pause использует прежний объект без FM reset.
 */
template<class Pipeline> bool destroyFrameFmBridgeJoined(FrameFmBridge<Pipeline>*&pointer,const PostJoinFrameAudio<Pipeline>&owner)noexcept{
    if(!pointer)return true;
    if(!pointer->emptyForTeardown(owner))return false;
    using Bridge=FrameFmBridge<Pipeline>;
    pointer->~Bridge();heap_caps_free(pointer);pointer=nullptr;return true;
}
} // namespace nes::app::c6_async
