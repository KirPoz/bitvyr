#pragma once
#include "Machine.h"
#include "ppu/PpuColourReplay.h"
#include "ppu/PpuRgbOutput.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <new>
namespace nes::app::coupled_hot {
/** @brief Проверяет полуоткрытый extent без переполнения; память остаётся у caller. */
inline bool extent(const void*p,std::size_t n,std::uintptr_t&first,std::uintptr_t&last)noexcept{
    first=reinterpret_cast<std::uintptr_t>(p);if(!first||!n||n>std::numeric_limits<std::uintptr_t>::max()-first)return false;last=first+n;return true;
}
/** @brief Проверяет непересечение двух живых диапазонов, не читая их байты. */
inline bool separate(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
    std::uintptr_t af=0,al=0,bf=0,bl=0;return extent(a,an,af,al)&&extent(b,bn,bf,bl)&&(al<=bf||bl<=af);
}
/**
 * @brief Требует HP SRAM обоих концов и заданное выравнивание; LP/RTC RAM отвергается.
 * @details DMA capability выбирает быстрый HP bank, а не разрешает переиспользование
 * буфера до завершения DMA/PPA-reader. Правила joined lifetime остаются прежними.
 */
inline bool internal(const void*p,std::size_t n,std::size_t alignment)noexcept{
    std::uintptr_t first=0,last=0;if(!extent(p,n,first,last)||first%alignment)return false;const auto*end=reinterpret_cast<const void*>(last-1);
    return esp_ptr_internal(p)&&esp_ptr_internal(end)&&esp_ptr_dma_capable(p)&&esp_ptr_dma_capable(end)&&esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&!esp_ptr_external_ram(p)&&!esp_ptr_external_ram(end);
}
/** @brief Требует PSRAM/8BIT обоих концов; индексное изображение не переносится INTERNAL. */
inline bool external(const void*p,std::size_t n)noexcept{
    std::uintptr_t first=0,last=0;if(!extent(p,n,first,last))return false;const auto*end=reinterpret_cast<const void*>(last-1);
    return esp_ptr_external_ram(p)&&esp_ptr_external_ram(end)&&esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&!esp_ptr_internal(p)&&!esp_ptr_internal(end);
}
/**
 * @brief Явно отключает скрытую индексную плоскость embedded shadow PPU.
 * @details Оба callback ненулевые: PixelStorage вызывает null-return allocate и не переходит к new[].
 * Выбранный external PPU обязан получить отдельные полноценные pixels до actor.begin.
 */
inline core::LegacyPixelAllocator noPixels()noexcept{return{
    +[](std::size_t)noexcept->std::uint8_t*{return nullptr;},+[](std::uint8_t*)noexcept{}};}
/** @brief Выделяет только одну61440B PSRAM-плоскость и отвергает неверные caps без fallback. */
inline core::LegacyPixelAllocator pixels()noexcept{return{
    +[](std::size_t n)noexcept->std::uint8_t*{
        if(n!=core::Ppu::width*core::Ppu::height)return nullptr;
        auto*p=static_cast<std::uint8_t*>(heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(p&&!external(p,n)){heap_caps_free(p);return nullptr;}return p;
    },+[](std::uint8_t*p)noexcept{heap_caps_free(p);}};}
/**
 * @brief Горячие mutable C0-объекты; единственный RGB остаётся прежним PSRAM staging.
 * @details Шина shadow Machine живёт дольше ppu; mapper/audio принадлежат этому C0 owner.
 */
struct HotRaster final {
    core::Mmc5 mapper;core::Mmc5Audio audio;
    core::Ppu ppu;core::PpuRgbOutput output;
    std::array<core::PpuRgbOutput::Pair,core::PpuRgbOutput::pairCount> table{};
    /** @brief Создаёт свежий внешний PPU на существующей shadow-шине с одной индексной плоскостью. */
    explicit HotRaster(core::Machine&shadow)noexcept:ppu(shadow,pixels()){}
};
/**
 * @brief Joined-only владелец INTERNAL-блока, объявляемый после shadow и до actor.
 * @details Actor/output должны завершить заимствование до деструктора. OOM/caps/alias
 * возвращают false; активный эксперимент не переключается незаметно на PSRAM actor.
 */
class Owner final {
    core::Machine*shadow_=nullptr;
public:
    static constexpr std::size_t alignment=alignof(HotRaster)>16U?alignof(HotRaster):16U;
    // DMA здесь ограничивает heap быстрым HP SRAM вместо LP/RTC INTERNAL bank.
    static constexpr unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT;
    enum class Reason:unsigned{NotCreated,Ready,Allocation,Storage,Pixels,Alias,Repeated};
    HotRaster*value=nullptr;Reason reason=Reason::NotCreated;
    std::size_t freeBefore=0,largestBefore=0;
    /** @brief Пустой owner ничего не выделяет; выбор выполняется только фабрикой на Main. */
    Owner()noexcept=default;
    /** @brief Не допускает копирование resource owner или повторную регистрацию PPU. */
    Owner(const Owner&)=delete;
    /** @brief Не допускает перенос владения обычным присваиванием. */
    Owner&operator=(const Owner&)=delete;
    /**
     * @brief Создаёт и проверяет hot-память до external bind и публикации Worker.
     * @param shadow Свежая C0 Machine с noPixels, живущая дольше owner.
     * @param context PSRAM Impl и его bytes запрещены для alias с новым блоком.
     * @param bytes Размер полного Impl в байтах.
     * @param producer Joined primary, которую hot-память не может перекрывать.
     * @param lease Immutable ROM extent текущего generation/epoch.
     * @param rgb Единственный122880B PSRAM staging; им продолжает владеть Impl.
     * @return true только с INTERNAL/8BIT hot + PSRAM pixels и свежими C0 models.
     */
    bool create(core::Machine&shadow,const void*context,std::size_t bytes,const core::Machine&producer,const core::ColourLease&lease,const void*rgb)noexcept{
        if(value){reason=Reason::Repeated;return false;}
        freeBefore=heap_caps_get_free_size(caps);largestBefore=heap_caps_get_largest_free_block(caps);
        void*memory=heap_caps_aligned_alloc(alignment,sizeof(HotRaster),caps);
        if(!memory){reason=Reason::Allocation;return false;}
        if(!internal(memory,sizeof(HotRaster),alignment)){reason=Reason::Storage;heap_caps_free(memory);return false;}
        if(!separate(memory,sizeof(HotRaster),context,bytes)||!separate(memory,sizeof(HotRaster),&producer,sizeof(producer))||
           !separate(memory,sizeof(HotRaster),lease.rom,lease.romBytes)||!separate(memory,sizeof(HotRaster),rgb,122880U)){
            reason=Reason::Alias;heap_caps_free(memory);return false;
        }
        auto*item=::new(memory)HotRaster(shadow);const auto*indices=item->ppu.framebuffer();
        if(!item->ppu.pixelsReady()||!external(indices,61440U)){reason=Reason::Pixels;item->~HotRaster();heap_caps_free(memory);return false;}
        if(!separate(indices,61440U,memory,sizeof(HotRaster))||!separate(indices,61440U,context,bytes)||
           !separate(indices,61440U,&producer,sizeof(producer))||!separate(indices,61440U,lease.rom,lease.romBytes)||!separate(indices,61440U,rgb,122880U)){
            reason=Reason::Alias;item->~HotRaster();heap_caps_free(memory);return false;
        }
        value=item;shadow_=&shadow;shadow.configureMmc5(&value->mapper,&value->audio);reason=Reason::Ready;return true;
    }
    /**
     * @brief После actor destruction/detach освобождает pixels и hot, сохраняя живую shadow bus.
     * @details Null mapper bind убирает dangling borrowed pointers до последующего Machine destruction.
     */
    ~Owner()noexcept{if(value){shadow_->configureMmc5(nullptr,nullptr);value->~HotRaster();heap_caps_free(value);}}
    /** @brief Печатает admission/caps/resource сведения вне измеряемого кадра, без обещания скорости. */
    void report(bool ready)const noexcept{
        std::printf("NES_COUPLED_HOT mode=%s ready=%u reason=%u block=%p bytes=%u alignment=%u ppu_bytes=%u output_bytes=%u table_bytes=%u mapper_bytes=%u audio_bytes=%u indexed_bytes=61440 indexed_external=%u shadow_indexed=0 new_rgb=0 free_before=%u largest_before=%u free_after=%u largest_after=%u\n",
            ready?"internal":"failed",unsigned(ready),unsigned(reason),static_cast<void*>(value),unsigned(sizeof(HotRaster)),unsigned(alignment),unsigned(sizeof(core::Ppu)),unsigned(sizeof(core::PpuRgbOutput)),unsigned(sizeof(HotRaster::table)),unsigned(sizeof(core::Mmc5)),unsigned(sizeof(core::Mmc5Audio)),
            unsigned(value&&external(value->ppu.framebuffer(),61440)),unsigned(freeBefore),unsigned(largestBefore),unsigned(heap_caps_get_free_size(caps)),unsigned(heap_caps_get_largest_free_block(caps)));
    }
};
}
