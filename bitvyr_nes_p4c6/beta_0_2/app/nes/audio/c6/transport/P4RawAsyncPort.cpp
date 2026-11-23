#include "P4RawAsyncPort.h"
#include "RawPort.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <new>

namespace nes::c6audio::p4 {namespace {
std::atomic<raw::Channel*>installed{nullptr};TaskHandle_t mainOwner{};
/** @brief Boot-long callback context; sourceDone не читает App/ROM/PCM. */
struct Completion final {void*context{};void(*done)(void*){};};
Completion completions[raw::slotCount];
/** @brief Driver прекратил чтение источника; это не remote delivery или WorkerDone. */
void sourceDone(void*p)noexcept{auto&c=*static_cast<Completion*>(p);c.done(c.context);}
/** @brief Main-only admission в lower-priority Hosted queue, без waits/alloc/printf. */
bool send(unsigned slot,const std::uint8_t*p,std::size_t n,void*context,void(*done)(void*))noexcept{
    if(slot>=raw::slotCount||xTaskGetCurrentTaskHandle()!=mainOwner)return false;
    completions[slot]={context,done};return nes_hosted_raw_try_tx(slot,p,n,&completions[slot],sourceDone)!=0;
}
/** @brief Проверяет оба конца allocation: только HP DRAM/internal/DMA/8BIT, не PSRAM/LP. */
bool capable(void*p)noexcept{
    if(!p||reinterpret_cast<std::uintptr_t>(p)%64U)return false;
    auto*last=static_cast<std::uint8_t*>(p)+sizeof(raw::Channel)-1;
    for(const void*q:{p,static_cast<void*>(last)})if(!esp_ptr_in_dram(q)||!esp_ptr_internal(q)||!esp_ptr_dma_capable(q)||!esp_ptr_byte_accessible(q))return false;
    return true;
}
}
/** @brief Cold Main создаёт boot-long HP/DMA Channel и публикует полностью построенный объект RX; OOM освобождает known storage. */
raw::Channel*initializeMainChannel()noexcept{
    if(auto*channel=installed.load(std::memory_order_acquire))return xTaskGetCurrentTaskHandle()==mainOwner?channel:nullptr;
    void*storage=heap_caps_aligned_alloc(64,sizeof(raw::Channel),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    if(!capable(storage)){if(storage)heap_caps_free(storage);return nullptr;}
    auto*channel=::new(storage)raw::Channel(send);mainOwner=xTaskGetCurrentTaskHandle();installed.store(channel,std::memory_order_release);return channel;
}
/** @brief Возвращает SDK monotonic microseconds Main start/completion; inclusive wall не является частотой C6. */
std::uint64_t clockMicroseconds()noexcept{return std::uint64_t(esp_timer_get_time());}
/** @brief Existing RX callback копирует только exact-key reply; ни alloc, ни log, ни App reads. */
void receive(const std::uint8_t*p,std::size_t n)noexcept{if(auto*channel=installed.load(std::memory_order_acquire))channel->receive(p,n);}
} // namespace nes::c6audio::p4
/** @brief C ABI RX передаёт borrowed packet Channel; callback не читает App/ROM и не сохраняет чужой указатель. */
extern "C" void nes_hosted_raw_rx(const std::uint8_t*p,std::size_t n){nes::c6audio::p4::receive(p,n);}
