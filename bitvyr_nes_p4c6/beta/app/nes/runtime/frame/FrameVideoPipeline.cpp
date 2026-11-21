#include "FrameVideoPipeline.h"
#include <cstdio>
#include "ppu/PpuColourReplay.h"
#include "ppu/VideoRenderer.h"
#include "ppu/PpuRgbOutput.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <array>
#include <atomic>
#include <cstring>
#include <new>
#include <limits>

#include <limits>
#include <cstdio>

namespace nes::app {
namespace {
constexpr std::size_t rgbBytes=122880,ramBytes=2048+8192;
/**
 * @brief Проверяет ненулевой extent без переполнения адреса; не проверяет аппаратные capabilities.
 * @param p Начало заимствованного диапазона.
 * @param bytes Ненулевая длина в байтах.
 * @param first Выходной числовой адрес начала.
 * @param last Выходная исключающая верхняя граница, записываемая при успехе.
 * @return true при допустимой арифметике диапазона; false при nullptr, нулевой длине или переполнении.
 */
bool checkedMemoryExtent(const void*p,std::size_t bytes,std::uintptr_t&first,std::uintptr_t&last)noexcept{
    first=reinterpret_cast<std::uintptr_t>(p);
    if(!p||!bytes||bytes>std::numeric_limits<std::uintptr_t>::max()-first)return false;
    last=first+bytes;return true;
}



/**
 * @brief Проверяет полный RGB source: INTERNAL/DMA/8BIT либо PSRAM/8BIT только в C6_FRAME_VIDEO.
 * @details PPA должен быть уже завершён вызывающей стороной.

 * @param[in] p Постоянный свободный Board RGB source, полученный после реального PPA drain.
 * @param[in] words Число RGB565 элементов; требуется ровно61440, не число байтов.
 * @param[in] m Joined producer, диапазон которого RGB не должен перекрывать.
 * @param[in] rom Живое ROM image, которое RGB не должен перекрывать.
 * @param[in] bytes Полный размер ROM image в байтах.
 */
bool validDirectRgb(std::uint16_t*p,std::size_t words,const core::Machine&m,const std::uint8_t*rom,std::size_t bytes)noexcept{
    std::uintptr_t first=0,last=0,producerFirst=0,producerLast=0,romFirst=0,romLast=0;
    if(words!=core::PpuRgbOutput::pixelCount||!checkedMemoryExtent(p,rgbBytes,first,last)||(first&127U)||
       !checkedMemoryExtent(&m,sizeof(m),producerFirst,producerLast)||!checkedMemoryExtent(rom,bytes,romFirst,romLast))return false;
    if((first<producerLast&&producerFirst<last)||(first<romLast&&romFirst<last))return false;
    const auto*end=reinterpret_cast<const void*>(last-1);
    const bool internal=esp_ptr_internal(p)&&esp_ptr_internal(end)&&esp_ptr_dma_capable(p)&&esp_ptr_dma_capable(end)&&
        esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&!esp_ptr_external_ram(p)&&!esp_ptr_external_ram(end);
    // Только выбранный C6 frontend допускает тот же полный PSRAM источник Board.
    // SDK PPA делает C2M msync перед DMA; прежний sourceDone остаётся обязательным.
    const bool external=esp_ptr_external_ram(p)&&esp_ptr_external_ram(end)&&
        esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&!esp_ptr_internal(p)&&!esp_ptr_internal(end);
    return internal||external;
}

/**
 * @brief Проверяет принадлежность обоих концов ненулевого диапазона внешней RAM.
 * @details Выделение, срок жизни и непересечение с другими объектами обеспечивает вызывающая сторона.

 * @param[in] p Начало живой аллокации для проверки принадлежности её концов PSRAM.
 * @param[in] bytes Её ненулевой размер в байтах; allocator/extent/lifetime остаются обязанностью caller.
 */
bool external(const void*p,std::size_t bytes)noexcept{
    return p&&bytes&&esp_ptr_external_ram(p)&&esp_ptr_external_ram(static_cast<const std::uint8_t*>(p)+bytes-1);
}

/**
 * @brief Проверяет принадлежность обоих концов ненулевого диапазона внутренней RAM.
 * @details Используется до размещения атомарных controls и не даёт права совместного доступа к обычным payload.

 * @param[in] p Начало живой аллокации для проверки принадлежности её концов INTERNAL RAM.
 * @param[in] bytes Её ненулевой размер в байтах; это не проверка взаимного исключения payload.
 */
bool internal(const void*p,std::size_t bytes)noexcept{
    return p&&bytes&&esp_ptr_internal(p)&&esp_ptr_internal(static_cast<const std::uint8_t*>(p)+bytes-1);
}

/**
 * @brief Возвращает PSRAM-аллокатор индексированного буфера C0.
 * @details Освобождение соответствует выделению; Board RGB в эту пару не входит.
 */




/**
 * @brief Проверяет непересечение двух живых диапазонов hot/indexed/контекста.
 * @details Это локальная проверка конкретных аллокаций, не глобальный реестр.

 * @param[in] a Начало первого живого диапазона.
 * @param[in] an Размер первого диапазона в байтах.
 * @param[in] b Начало второго живого диапазона.
 * @param[in] bn Размер второго диапазона в байтах; касание границ не является пересечением.
 */
bool rasterSeparate(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
    std::uintptr_t af=0,al=0,bf=0,bl=0;
    return checkedMemoryExtent(a,an,af,al)&&checkedMemoryExtent(b,bn,bf,bl)&&(al<=bf||bl<=af);
}

/**
 * @brief Проверяет выровненный INTERNAL/8BIT диапазон hot-raster до placement-new.

 * @param[in] p Raw storage hot-объекта до конструктора.
 * @param[in] bytes Размер всего hot-объекта в байтах.
 * @param[in] alignment Требуемое выравнивание типа/owner в байтах, минимум16 в текущем factory.
 */
bool rasterInternal(const void*p,std::size_t bytes,std::size_t alignment)noexcept{
    std::uintptr_t first=0,last=0;
    if(!checkedMemoryExtent(p,bytes,first,last)||first%alignment)return false;
    const auto*end=reinterpret_cast<const void*>(last-1);
    return esp_ptr_internal(p)&&esp_ptr_internal(end)&&esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&
        !esp_ptr_external_ram(p)&&!esp_ptr_external_ram(end);
}

/**
 * @brief Проверяет доступную PSRAM для отдельного индексированного изображения hot-raster.

 * @param[in] p Отдельное raw storage индексированных пикселей.
 * @param[in] bytes Размер индексированного буфера в байтах, не размер RGB565.
 */
bool rasterExternal(const void*p,std::size_t bytes)noexcept{
    std::uintptr_t first=0,last=0;if(!checkedMemoryExtent(p,bytes,first,last))return false;
    const auto*end=reinterpret_cast<const void*>(last-1);
    return esp_ptr_external_ram(p)&&esp_ptr_external_ram(end)&&esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&
        !esp_ptr_internal(p)&&!esp_ptr_internal(end);
}

/**
 * @brief Создаёт проверяемый PSRAM-аллокатор пикселей выбранного внешнего Ppu.
 * @details Не переносит индексированные пиксели во внутренний hot-блок.
 */
core::LegacyPixelAllocator rasterPixelsAllocator()noexcept{return{
    +[](std::size_t n)noexcept->std::uint8_t*{
        auto*p=static_cast<std::uint8_t*>(heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(p&&!rasterExternal(p,n)){heap_caps_free(p);return nullptr;}return p;
    },+[](std::uint8_t*p)noexcept{heap_caps_free(p);}};}

/**
 * @brief Выбранный C0 Ppu, RGB output и таблица пар во внутреннем блоке.
 * @details Индексированные пиксели остаются отдельной PSRAM-аллокацией; дополнительного RGB здесь нет.
 */
struct HotRaster final{
    core::Ppu ppu;core::PpuRgbOutput output{};
    std::array<core::PpuRgbOutput::Pair,core::PpuRgbOutput::pairCount> table{};
    
    /**
     * @brief Конструирует выбранный Ppu на той же shadow-шине и RGB output внутри уже выделенного INTERNAL блока.
     * @details Индексированное изображение получает отдельный PSRAM owner.
    
     * @param[in] shadow Та же shadow Machine, которая служит шиной выбранного external Ppu и переживает его деструктор.
     */
    explicit HotRaster(core::Machine&shadow)noexcept:ppu(shadow,rasterPixelsAllocator()){}
};

/**
 * @brief RAII-владелец необязательного INTERNAL hot-raster с явной причиной fallback.
 * @details Объявляется до actor, чтобы разрушаться после detach RGB/Ppu lease.
 */
struct HotRasterOwner final{
    // Только внутреннее CPU-состояние; существующий Board RGB остаётся единственным PPA source.
    static constexpr std::size_t alignment=alignof(HotRaster)>16?alignof(HotRaster):16;
    static constexpr unsigned cpuCaps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
    static constexpr unsigned dmaCaps=cpuCaps|MALLOC_CAP_DMA;
    HotRaster*value=nullptr;
    
    /**
     * @brief Фабричный результат допуска hot-raster: allocation, storage, pixels или Ready.
     * @details Не является оценкой скорости.
     */
    enum class Reason:unsigned{Ready,Allocation,Storage,Pixels};Reason reason=Reason::Allocation;
    std::size_t freeBefore=heap_caps_get_free_size(cpuCaps),largestBefore=heap_caps_get_largest_free_block(cpuCaps);
    std::size_t dmaFreeBefore=heap_caps_get_free_size(dmaCaps),dmaLargestBefore=heap_caps_get_largest_free_block(dmaCaps);
    
    /**
     * @brief Пытается выделить и проверить внутренний hot-блок и его пиксели, сохраняя точную причину fallback.
     * @details Копирование владельца запрещено; выбор делается до начала кадров.
    
     * @param[in] shadow Живая shadow-шина будущего Ppu.
     * @param[in] context Диапазон Impl, с которым INTERNAL block не должен пересекаться.
     * @param[in] contextBytes Полный размер Impl в байтах.
     * @param[in] producer Joined producer, чья память не может стать hot storage.
     * @param[in] lease Неизменный ROM/epoch/region lease для проверки непересечения.
     * @param[in] rgb Постоянный RGB source; его122880 байтов нельзя перекрывать hot-блоком.
     */
    HotRasterOwner(core::Machine&shadow,const void*context,std::size_t contextBytes,
                   const core::Machine&producer,const core::ColourLease&lease,const void*rgb)noexcept{
        void*memory=heap_caps_aligned_alloc(alignment,sizeof(HotRaster),cpuCaps);
        if(!memory)return;
        if(!rasterInternal(memory,sizeof(HotRaster),alignment)||
           !rasterSeparate(memory,sizeof(HotRaster),context,contextBytes)||
           !rasterSeparate(memory,sizeof(HotRaster),&producer,sizeof(producer))||
           !rasterSeparate(memory,sizeof(HotRaster),lease.rom,lease.romBytes)||
           !rasterSeparate(memory,sizeof(HotRaster),rgb,rgbBytes)){
            reason=Reason::Storage;heap_caps_free(memory);return;
        }
        auto*p=::new(memory)HotRaster(shadow);
        if(!p->ppu.pixelsReady()){
            reason=Reason::Pixels;p->~HotRaster();heap_caps_free(memory);return;
        }
        value=p;reason=Reason::Ready;
    }
    
    /**
     * @brief Уничтожает hot output/Ppu и освобождает их блок только после detach/деструктора actor.
     * @details Порядок полей Impl сохраняет живую shadow-шину до этого момента.
     */
    ~HotRasterOwner()noexcept{
        // Impl объявляет owner до actor: его выбранный RGB lease уже
        // отсоединён к моменту разрушения output/PPU/pixels и INTERNAL блока.
        if(value){value->~HotRaster();heap_caps_free(value);}
    }
    
    /**
     * @brief Запрещает копирование владельца ресурсов.
     * @details Передача ordinary полей, атомарных controls и callback-регистрации копией не поддерживается.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Источник запрещённого копирования; передача его resource/endpoint ownership не выполняется.
     */
    HotRasterOwner(const HotRasterOwner&)=delete;
    
    /**
     * @brief Запрещает копирование владельца ресурсов.
     * @details Передача ordinary полей, атомарных controls и callback-регистрации копией не поддерживается.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Источник запрещённого копирования; передача его resource/endpoint ownership не выполняется.
     */
    HotRasterOwner&operator=(const HotRasterOwner&)=delete;
    
    /**
     * @brief Печатает результат выбора INTERNAL hot-raster и heap telemetry вне измеряемого кадра.
     * @details Это сведения о размещении, а не доказательство ускорения.
    
     * @param[in] rgb Адрес выбранного RGB source для telemetry; не даёт права читать/писать его при PPA.
     * @param[in] ready Итоговая готовность внешнего bind; причина fallback хранится отдельно.
     */
    void report(const void*rgb,bool ready)const noexcept{
        const auto*indices=value?value->ppu.framebuffer():nullptr;
        std::printf("NES_FRAME_HOT_RASTER mode=%s reason=%u ready=%u block=%p bytes=%u alignment=%u ppu_bytes=%u output_bytes=%u table_bytes=%u rgb=%p new_rgb=0\n",
            value?"internal":"fallback",unsigned(reason),unsigned(ready),static_cast<void*>(value),unsigned(sizeof(HotRaster)),unsigned(alignment),
            unsigned(sizeof(core::Ppu)),unsigned(sizeof(core::PpuRgbOutput)),unsigned(sizeof(HotRaster::table)),const_cast<void*>(rgb));
        std::printf("NES_FRAME_HOT_RASTER_SPANS ppu_offset=%u output_offset=%u table_offset=%u internal=%u indexed=%p indexed_bytes=%u indexed_external=%u\n",
            unsigned(value?reinterpret_cast<std::uintptr_t>(&value->ppu)-reinterpret_cast<std::uintptr_t>(value):0),
            unsigned(value?reinterpret_cast<std::uintptr_t>(&value->output)-reinterpret_cast<std::uintptr_t>(value):0),
            unsigned(value?reinterpret_cast<std::uintptr_t>(&value->table)-reinterpret_cast<std::uintptr_t>(value):0),
            unsigned(value&&rasterInternal(value,sizeof(HotRaster),alignment)),const_cast<std::uint8_t*>(indices),unsigned(core::Ppu::width*core::Ppu::height),
            unsigned(indices&&rasterExternal(indices,core::Ppu::width*core::Ppu::height)));
        std::printf("NES_FRAME_HOT_RASTER_HEAP free_before=%u largest_before=%u free_after=%u largest_after=%u dma_free_before=%u dma_largest_before=%u dma_free_after=%u dma_largest_after=%u min_internal=%u\n",
            unsigned(freeBefore),unsigned(largestBefore),unsigned(heap_caps_get_free_size(cpuCaps)),unsigned(heap_caps_get_largest_free_block(cpuCaps)),
            unsigned(dmaFreeBefore),unsigned(dmaLargestBefore),unsigned(heap_caps_get_free_size(dmaCaps)),unsigned(heap_caps_get_largest_free_block(dmaCaps)),unsigned(heap_caps_get_minimum_free_size(cpuCaps)));
    }
};

/**
 * @brief Вычисляет диагностический FNV-свидетель по указанным живым байтам.
 * @details Хэш не является бесколлизионным разрешением пропускать эмуляцию.

 * @param[in] p Живое читаемое начало RAM/PCM/RGB accepted-данных.
 * @param[in] n Точное число байтов хэширования, не число samples или пикселей.
 * @param[in] h Начальное FNV-состояние, позволяющее продолжить hash; не гостевой open bus.
 */
std::uint64_t hashBytes(const void*p,std::size_t n,std::uint64_t h=1469598103934665603ULL)noexcept{
    const auto*b=static_cast<const std::uint8_t*>(p);for(std::size_t i=0;i<n;++i)h=(h^b[i])*1099511628211ULL;return h;
}

/**
 * @brief Проверяет явную область обычного observable-owner для принятого mapper.
 * @details Неподдержанный mapper не допускается к скрытому fallback в этом benchmark.

 * @param[in] n Номер уже принятого mapper, проверяемый по явному списку0/1/2/4/7/160.
 */








/**
 * @brief Проверяет общие пределы phase<5, dot<=340 и scanline от -1 до311 перед передачей C0.
 * @details Точный регион, cycle и соответствие paired cut проверяются остальными условиями потребления.

 * @param[in] c Проверяемые raster/phase поля; полная cycle/region identity проверяется отдельными условиями.
 */

}

/**
 * @brief Скрытое хранилище соответствующего benchmark: отдельные Main/Worker данные, caller-owned buffers и стабильные endpoints.
 * @details Право доступа определяется joined/slot protocol; произвольное копирование недопустимо.
 */
#if defined(NES_APP_FRAME_NATIVE_PROOF) && NES_APP_FRAME_NATIVE_PROOF
/**
 * @brief Native-only точка удержания настоящего Reading для проверки N+1/late done.
 * @details Определяется proof adapter; target без NATIVE_PROOF не содержит вызова.
 * Наблюдатель не получает Machine и не создаёт гостевых тактов/PPU witnesses.
 */
void frameOutputNativeReading(const void* slot)noexcept;
#endif
/**
 * @brief Отдельный atomic control output-слота без ColourPacket/PPU witness.
 * @details Все ordinary Payload/VideoPacket данные публикуются release/acquire.
 * Сам VideoPacket остаётся неатомарным и не заменяет внешний Worker done.
 */
struct alignas(64) FrameOutputSlot final {
    /** @brief Внешние роли slot handoff; они не утверждают наличие C0 PPU-актора. */
    enum class State:unsigned {Free,Writing,Sealed,Reading,Consumed};
    /** @brief Только историческая identity задания для общего DSP loan протокола. */
    struct Header {std::uint64_t sequence=0;} metadata{};
    std::atomic<unsigned> role{unsigned(State::Free)};
    /** @brief Читает опубликованную роль, не обращаясь к первичной Machine. */
    State state()const noexcept{return State(role.load(std::memory_order_acquire));}
    /** @brief Возвращает immutable job sequence для прежнего bounded DSP loan id. */
    const Header&header()const noexcept{return metadata;}
    /** @brief Захватывает ранее подтверждённый Free слот до первого guest изменения. */
    bool begin(std::uint64_t sequence)noexcept{unsigned wanted=unsigned(State::Free);if(!role.compare_exchange_strong(wanted,unsigned(State::Writing),std::memory_order_acq_rel))return false;metadata.sequence=sequence;return true;}
    /** @brief Публикует полный paired payload только после успешных Core/video/audio cuts. */
    bool seal()noexcept{unsigned wanted=unsigned(State::Writing);return role.compare_exchange_strong(wanted,unsigned(State::Sealed),std::memory_order_release);}
    /** @brief Подтверждает единственного C0 читателя перед первым compositor доступом. */
    bool read()noexcept{
        unsigned wanted=unsigned(State::Sealed);
        if(!role.compare_exchange_strong(wanted,unsigned(State::Reading),std::memory_order_acq_rel))return false;
#if defined(NES_APP_FRAME_NATIVE_PROOF) && NES_APP_FRAME_NATIVE_PROOF
        frameOutputNativeReading(this);
#endif
        return true;
    }
    /** @brief Отмечает завершение чтения; packet всё ещё удерживается до paired accept. */
    bool consumed()noexcept{unsigned wanted=unsigned(State::Reading);return role.compare_exchange_strong(wanted,unsigned(State::Consumed),std::memory_order_release);}
    /** @brief Последним release подтверждает Free после ordinary VideoPacket.release. */
    bool release()noexcept{unsigned wanted=unsigned(State::Consumed);return role.compare_exchange_strong(wanted,unsigned(State::Free),std::memory_order_release);}
};
static_assert(alignof(FrameOutputSlot)==64&&sizeof(FrameOutputSlot)%64U==0,
    "independent output slot cache lines");
#define NES_FRAME_SLOT_STATE FrameOutputSlot::State

/** @brief Рабочее хранилище Main/Worker; доступ к слотам передаётся Control, память сохраняется до полного join. */ struct FrameVideoPipeline::Impl final {
    
    /**
     * @brief Неподвижная INTERNAL область атомарного stop/slot handoff и audio control.
     * @details Ordinary payload защищается публикацией владения, а не volatile.
     */
    struct alignas(64) Control {
        std::atomic<unsigned> fault{0},workerBorrowed{0},workerFinished{0},consumerActive{0};
        std::atomic<unsigned> ready[slotsCount]{};
        FrameOutputSlot packets[slotsCount];
        
        /**
         * @brief Инициализирует атомарные ready состояния двух существующих Output slots.
         * @details Память INTERNAL и выравнивание уже проверил factory; конструктор не выделяет
         * буферы и не публикует Worker. Control живёт до настоящего join обеих сторон.
         */
        Control()noexcept
        {
            for(auto&r:ready)r.store(0,std::memory_order_relaxed);
        }
    };
    
    /**
     * @brief Числовое качество usage одного кадра: inactive, полный подсчёт, насыщение или несовпадение identity.
     * @details Статус описывает DTO и не передаёт владение слотом, Machine, RGB или PCM.
     */
    /** @brief Числовой статус usage DTO; не guest fault и не разрешение освобождения. */
    enum class UsageStatus:std::uint32_t {Inactive,Complete,Saturated,Identity};
    /**
     * @brief Worker пишет usage и key своего SlotN; Main копирует до любого Free.
     * @details Поля безусловны; counts не дают права читать Machine или менять clocks.
     */
    struct UsageSample {
        core::Machine::Mmc5Usage counts{};
        std::uint64_t generation=0,sequence=0,baseCycle=0,endCycle=0;
        std::uint64_t leaseGeneration=0,romEpoch=0,transportEpoch=0;
        unsigned slot=0;
        core::Region region=core::Region::Ntsc;
        UsageStatus status=UsageStatus::Inactive;
    };
    /** @brief Main-only окно максимум120 released accepted кадров, без доступа к PayloadN+1. */
    struct UsageWindow {
        std::uint64_t h2=0,tile8=0,carry=0,generation=0,leaseGeneration=0,romEpoch=0,transportEpoch=0;
        std::uint64_t first=0,last=0,baseCycle=0,endCycle=0;
        core::Region region=core::Region::Ntsc;
        unsigned frames=0,invalid=0,saturated=0,gaps=0;
        /**
         * @brief Принимает только Main copy после двух releases; выводит одно bounded окно.
         * @param record Числовой DTO со stamp, проверенным до Free; не ссылка на Payload.
         * @details Смена key/разрыв начинает новое окно с quality gap, не смешивает ROM/region.
         * Насыщенный/invalid кадр учитывается в quality, но не в точных суммах.
         */
        void add(const UsageSample& record)noexcept {
            if(frames&&(generation!=record.generation||leaseGeneration!=record.leaseGeneration||
               romEpoch!=record.romEpoch||transportEpoch!=record.transportEpoch||region!=record.region||
               last==~std::uint64_t{0}||record.sequence!=last+1U||record.baseCycle!=endCycle)){
                *this={};gaps=1U;
            }
            if(!frames){generation=record.generation;leaseGeneration=record.leaseGeneration;
                romEpoch=record.romEpoch;transportEpoch=record.transportEpoch;region=record.region;
                first=record.sequence;baseCycle=record.baseCycle;}
            last=record.sequence;endCycle=record.endCycle;++frames;
            if(record.status==UsageStatus::Complete){h2+=record.counts.h2AdmittedM2;
                tile8+=record.counts.tile8Groups;carry+=record.counts.carryContinuations;}
            else if(record.status==UsageStatus::Saturated)++saturated;
            else ++invalid;
            if(frames==120U){report();*this={};}
        }
        /** @brief Печатает только собственные числовые суммы; стоимость входит в measured service. */
        void report()const noexcept {
            unsigned flags=16U; // Usage включён; биты0/1/2 задают пути, бит3 задаёт DIAG.
            flags|=8U;
            std::printf("NES_MMC5_PATH_USAGE schema=1 diagnostic=1 generation=%llu lease_generation=%llu rom_epoch=%llu transport_epoch=%llu region=%s input_frame_first=%llu input_frame_last=%llu base_cycle=%llu end_cycle=%llu accepted=%u valid=%u invalid=%u saturated=%u gaps=%u h2_admitted_m2=%llu tile8_groups=%llu carry_continuations=%llu flags=%u game_fps_claim=0\n",
                static_cast<unsigned long long>(generation),static_cast<unsigned long long>(leaseGeneration),
                static_cast<unsigned long long>(romEpoch),static_cast<unsigned long long>(transportEpoch),
                region==core::Region::Pal?"PAL":"NTSC",static_cast<unsigned long long>(first),
                static_cast<unsigned long long>(last),static_cast<unsigned long long>(baseCycle),
                static_cast<unsigned long long>(endCycle),frames,frames-invalid-saturated,invalid,saturated,gaps,
                static_cast<unsigned long long>(h2),static_cast<unsigned long long>(tile8),
                static_cast<unsigned long long>(carry),flags);
        }
    };
    /** @brief PSRAM-срез слота: AudioPacket, RAM/input/CPU, timing и Output ticket; Worker публикует его неизменным до освобождения всеми consumers. */ struct Payload {
        core::AudioPacket audio{};
        UsageSample usage{}; // Оба lanes: WorkerSlotN→immutable Main copy до Free.
        Snapshot snapshot{};Times times{};
        debug::HartSample hart{}; // Только Worker до прежней release-публикации Payload.
        std::array<std::uint8_t,ramBytes> ram{};
        std::int64_t deadline=0;
        // Крупные capability значения находятся в PSRAM payload, не в Main stack.
        core::OutputFrameTicket outputTicket{};
        core::OutputFrameCut outputCut{};
        const core::VideoPacket* outputPacket=nullptr;
        std::uint8_t outputPad=0;
        core::Vrc7WriteTape::View fmTape{}; // Worker seal/copy до Ready; Main только читает до exact reclaim.
    };
    /** @brief Единственная bounded PSRAM-арена двух FM-лент; создаётся только для actual FM85.
     * При cut<45000M2 16384 событий покрывают даже RMW с двумя writes за6M2.
     * Main не меняет events; после exact C6 reclaim возвращает только tape phase.
     */
    struct Vrc7TapeStorage {
        static constexpr unsigned capacity=16384;
        core::Vrc7WriteTape tape[slotsCount]{};
        core::Vrc7WriteTape::Event events[slotsCount][capacity]{};
    };
    Vrc7TapeStorage*fmStorage=nullptr;
    Control*control;
    Payload*payload[slotsCount];std::uint16_t*rgb;
    core::VideoPacket*outputPackets[slotsCount]{};
    core::VideoStorage*outputStorage=nullptr;
    core::VideoRenderer*outputRenderer=nullptr;
    core::OutputFrameBinding outputBinding{};
    core::OutputFrameLease outputLease{};
    const std::uint8_t*outputChr=nullptr;
    std::uint32_t outputChrBytes=0;
    std::uint64_t outputVideoGeneration=0;
    bool outputBound=false,outputWriterAttached=false;
    FrameRamPolicy ramPolicy=FrameRamPolicy::Full; // Main только atCreate; Worker затем только читает.
    core::Machine&producer;core::ColourLease lease{};

    core::AudioPacket::Header expectedAudio{}; // Владеет Worker между join-границами.
    std::uint64_t appGeneration=0,produced=0,accepted=0;
    unsigned jobIndex=0;std::uint64_t jobInputFrame=0,jobAppGeneration=0;std::uint8_t jobPad=0;
    int readyIndex=-1;bool pendingAcceptance=false,rgbAccepted=false;
    bool audioHeld[slotsCount]{},audioIssued[slotsCount]{}; // Только Main, до прежнего start/done.
    Snapshot acceptedSnapshot{},pendingSnapshot{};Times latestTimes{};
    UsageSample acceptedUsage{}; // Только Main после paired validation до любого Free.
    UsageWindow usageWindow{}; // Только после обоих releases; не producer counters.
    debug::HartWindow hartWindow{}; // Только Main, независимо от следующего Worker job.
    debug::HartSample acceptedHart{}; // Копия до Free; failed accept не попадает в суммы.
    std::uint64_t acceptedHartGuestCycles=0; // Sealed frame cut, не guest clocks только stepFrame.
    std::array<std::uint8_t,ramBytes> acceptedRam{}; // Владеет Main; данные переживают переиспользование слота C1.
    // Только Main. expectedAudio выше принадлежит Worker, а не этому stage.
    
    /**
     * @brief Main-owned фазы подготовки DSP и последующего colour-потребления одного sealed слота.
     * @details Reentrant fault обязан проверять живой owner до обращения к контексту.
     */
    enum class DspPhase:std::uint8_t {Idle,Preparing,Prepared,Consuming,ColourReady,Aborted};
    DspPhase dspPhase=DspPhase::Idle;
    std::uint8_t preparedIndex=0;
    core::AudioRenderer*preparedRenderer=nullptr;
    core::AudioPacket::Header mainAudio{};
    core::ColourCut mainColourCut{};
    /** @brief Заимствует проверенные factory Control c, Payload x/y, RGB r и остановленный reset7 Machine m; копирует lease l/header audio, задаёт исходный accepted cut без аллокаций и публикации Worker. */ Impl(Control*c,
         Payload*x,Payload*y,std::uint16_t*r,core::Machine&m,
         const core::ColourLease&l,const core::AudioPacket::Header&audio)noexcept:
        control(c),payload{x,y},rgb(r),producer(m),lease(l),expectedAudio(audio),appGeneration(l.generation){
        acceptedSnapshot.cpu=m.cpuState();acceptedSnapshot.cycle=7;
        acceptedSnapshot.appGeneration=l.generation;acceptedSnapshot.region=l.region;
        mainAudio=audio;mainColourCut={7,0,-1,static_cast<std::uint16_t>(l.region==core::Region::Pal?22:21),static_cast<std::uint8_t>(l.region==core::Region::Pal?2:0)};
    }
    
    /**
     * @brief Сохраняет первую sticky-ошибку и останавливает контекст.
     * @details Не освобождает borrowed слоты/ROM и не публикует частичный RGB/PCM.
    
     * @param[in] e Причина первой sticky-остановки; этот вызов не освобождает borrowed context/ROM.
     */
    bool fail(Error e)noexcept{
        unsigned expected=0;control->fault.compare_exchange_strong(expected,unsigned(e),std::memory_order_acq_rel);return false;
    }
    
    /**
     * @brief Проверяет атомарную sticky-остановку контекста.
     * @details Обычные поля producer или renderer через этот помощник не читаются.
     */
    bool okay()const noexcept{return control->fault.load(std::memory_order_acquire)==0;}
    
    /**
     * @brief Проверяет абсолютный deadline текущего задания по монотонным микросекундам.
     * @details Истечение срока не заменяет фактическое завершение владельцев.
    
     * @param[in] i Индекс уже зарезервированного слота0..1, чей абсолютный deadline проверяется.
     */
    bool expired(unsigned i)const noexcept{return esp_timer_get_time()>=payload[i]->deadline;}
    
    /**
     * @brief Копирует идентичность sealed слота в Main-stage до вызова реального DSP.
     * @details Эти данные переживают повторное использование producer-слотов.
    
     * @param[in] i Индекс живого sealed/retired слота0..1, которым владеет текущая Main-фаза.
     * @param[in] wanted Точное требуемое состояние ColourPacket до копирования immutable metadata.
     */
    bool stageMetadata(unsigned i,NES_FRAME_SLOT_STATE wanted)const noexcept{
        return stageOutputMetadata(i,wanted);
    }
    
    /**
     * @brief Проверяет renderer и неизменный stage-header после DSP, включая sampleCount/cut/sequence.
     * @details Вызывается только при законном Main-владении renderer.
     */
    /** @brief Сверяет sealed immutable output/video/audio/lease metadata перед C0 доступом. */
    bool stageOutputMetadata(unsigned i,FrameOutputSlot::State wanted)const noexcept;
    /** @brief Выполняет один Worker JOB через explicit Core output capabilities. */
    /** @brief Выполняет Worker JOB в output-пакет без необязательного cache-наблюдателя. */
    core::RunStatus produceOutput(unsigned i)noexcept;
    /** @brief Составляет один Main RGB после фактического PPA source-release fence caller. */
    bool renderOutput(unsigned i,bool(*progress)(void*)noexcept,void*context)noexcept;
    /** @brief После общих join/consumer/loan checks снимает Core/output-аренды до free. */
    bool releaseOutputJoined()noexcept;
    /** @brief Сверяет завершённый DSP renderer с принадлежащим Main заголовком парного аудиопакета. */
    bool preparedDspValid()const noexcept{
        // PCM принадлежит C6 queue, не этому renderer. Здесь проверяется лишь
        // ранее sealed header удержанного пакета; никаких вымышленных samples.
        return preparedIndex<slotsCount&&audioIssued[preparedIndex]&&
            core::sameAudioHeader(payload[preparedIndex]->audio.header(),mainAudio);
    }
};
#include "FrameOutputTransport.inl"

#include "FrameOutputFactory.inl"

/**
 * @brief Освобождает контекст только после реального Worker done и отсутствия активного Main consumer/DSP.
 * @details При неизвестном владении возвращает false и удерживает память/ROM; порядок detach предшествует free.

 * @param[in] p Фабричный контекст после настоящего Worker done; неизвестное владение возвращает false с удержанием всех ресурсов.
 */
bool FrameVideoPipeline::destroyJoined(FrameVideoPipeline*p)noexcept{
    if(!p)return true;
    auto*s=p->state_;
    if(s->audioHeld[0]||s->audioHeld[1])return false; // Не free при неизвестном C6 input owner.
    if(s->control->workerBorrowed.load(std::memory_order_acquire)||s->control->consumerActive.load(std::memory_order_acquire))return false;
    s->fail(Error::Stopped);
    if(!s->releaseOutputJoined())return false; // Ни один borrowed ресурс не освобождается при неизвестности.
    auto*c=s->control;auto*x=s->payload[0];auto*y=s->payload[1];
    auto*a=s->outputPackets[0];auto*b=s->outputPackets[1];
    auto*storage=s->outputStorage;auto*renderer=s->outputRenderer;auto*fm=s->fmStorage;
    renderer->~VideoRenderer();storage->~VideoStorage();a->~VideoPacket();b->~VideoPacket();
    if(fm)fm->~Vrc7TapeStorage();
    s->~Impl();c->~Control();x->~Payload();y->~Payload();p->~FrameVideoPipeline();
    heap_caps_free(fm);heap_caps_free(p);heap_caps_free(s);heap_caps_free(renderer);heap_caps_free(storage);
    heap_caps_free(c);heap_caps_free(y);heap_caps_free(x);heap_caps_free(b);heap_caps_free(a);return true;
}

/**
 * @brief На Main перед dispatch резервирует свободный producer-слот и фиксирует inputFrame/pad/App generation.
 * @details Не читает работающий producer и отказывает при незавершённой предыдущей передаче.

 * @param[in] inputFrame Исторический номер кадра ввода для следующего CPU job, не номер LCD submission.
 * @param[in] pad Фактическая восьмибитная маска pad, применяемая к этому job.
 * @param[in] generation Текущее App generation; повторное использование старого сеанса запрещено.
 */
bool FrameVideoPipeline::prepareJob(std::uint64_t inputFrame,std::uint8_t pad,std::uint64_t generation)noexcept{
    auto&s=*state_;if(!s.okay()||s.control->workerBorrowed.load(std::memory_order_acquire)||s.pendingAcceptance||
        inputFrame!=s.produced||generation!=s.appGeneration||s.produced-s.accepted>1)return s.fail(Error::Sequence);
    const unsigned index=unsigned(inputFrame%slotsCount);
    if(s.audioHeld[index])return s.fail(Error::Busy); // App заранее pump-ит C6 до available.
    if(s.fmStorage&&s.fmStorage->tape[index].state()!=core::Vrc7WriteTape::State::Empty)return s.fail(Error::Busy);
    s.audioIssued[index]=false;
    if(s.control->packets[index].state()!=NES_FRAME_SLOT_STATE::Free||s.control->ready[index].load(std::memory_order_acquire))return s.fail(Error::Slot);
    s.jobIndex=index;s.jobInputFrame=inputFrame;s.jobAppGeneration=generation;s.jobPad=pad;
    s.payload[index]->outputPad=pad; // Исторический ввод этого Free slot, не mutable job N+1.
    s.payload[index]->deadline=esp_timer_get_time()+1500000;s.control->workerFinished.store(0,std::memory_order_relaxed);
    s.control->workerBorrowed.store(1,std::memory_order_release);return true;
}

/**
 * @brief На Worker выполняет обычный observable CPU-кадр, seal colour/audio и неизменный CPU/RAM/input snapshot.
 * @details Публикует только полностью согласованный слот; локальный guard отмечает фактический выход Worker.
 */
/** @brief На CPU1 завершает гостевой кадр и публикует согласованный слот без cache-диагностики. */
core::RunStatus FrameVideoPipeline::produce()noexcept{
    auto&s=*state_;const unsigned i=s.jobIndex;
    
    /**
     * @brief Локальный scope guard, публикующий фактический выход из этапа на всех return-путях.
     * @details Не заменяет внешний worker done и PPA drain.
     */
    struct Finished {Impl::Control&c;
                /**
                 * @brief Публикует workerFinished release-записью при фактическом выходе produce на любом return.
                 * @details Внешний done-семафор App по-прежнему нужен для завершения всего Worker job.
                 */
                ~Finished(){c.workerFinished.store(1,std::memory_order_release);}}finished{*s.control};
    return s.produceOutput(i);
}

/**
 * @brief На Main после настоящего done проверяет producer epoch/region/cut, slot publication и отсутствие faults.
 * @details Только здесь разрешены соответствующие producer getters.
 */
bool FrameVideoPipeline::joinedProducer()noexcept{
    auto&s=*state_;if(!s.control->workerBorrowed.load(std::memory_order_acquire)||!s.control->workerFinished.load(std::memory_order_acquire))return s.fail(Error::Busy);
    s.control->workerBorrowed.store(0,std::memory_order_release);
    const unsigned i=s.jobIndex;if(!s.okay()||s.expired(i)||s.readyIndex!=-1||!s.control->ready[i].load(std::memory_order_acquire))return s.fail(Error::Producer);
    const auto&p=*s.payload[i];
    if(p.snapshot.frame!=s.produced+1||p.snapshot.inputFrame!=s.produced||p.snapshot.appGeneration!=s.appGeneration||
       !s.producer.outputFramesEnabled()||
       s.producer.region()!=s.lease.region||
       s.producer.frames()!=p.snapshot.frame||s.producer.cpuCycles()!=p.snapshot.cycle)return s.fail(Error::Lease);
    s.readyIndex=int(i);
    if(!s.stageOutputMetadata(i,FrameOutputSlot::State::Sealed))return s.fail(Error::Lease);
    ++s.produced;return true;
}

namespace {
/** @brief Сверяет полный joined ticket: ROM/epoch/region, slot, sequence, input и base cut, без padding и mutable primary getters. */
bool sameJoinedOutput(const core::OutputFrameTicket&a,const core::OutputFrameTicket&b)noexcept{
    const auto&x=a.lease;const auto&y=b.lease;
    return x.producer==y.producer&&x.rom==y.rom&&x.romBytes==y.romBytes&&x.romEpoch==y.romEpoch&&
        x.generation==y.generation&&x.transportEpoch==y.transportEpoch&&x.region==y.region&&
        a.slot==b.slot&&a.sequence==b.sequence&&a.inputFrame==b.inputFrame&&a.baseCycle==b.baseCycle;
}
}
/** @details readyIndex выставляется только joinedProducer после caller actual done. */
bool FrameVideoPipeline::borrowJoinedAudio(core::AudioPacketView&view,JoinedAudioTicket&ticket)noexcept{
    auto&s=*state_;
    if(!s.okay()||s.readyIndex<0||s.pendingAcceptance)return false;
    const auto i=unsigned(s.readyIndex);
    if(i>=slotsCount||s.audioHeld[i]||s.audioIssued[i]||!s.stageOutputMetadata(i,FrameOutputSlot::State::Sealed))return false;
    const auto&p=*s.payload[i];const auto next=core::AudioPacketView::from(p.audio);
    if(!next.readable())return false;
    JoinedAudioTicket t{};t.output=p.outputTicket;t.audio=p.audio.header();t.appGeneration=s.appGeneration;
    s.audioHeld[i]=true;s.audioIssued[i]=true;ticket=t;view=next;return true;
}
/** @details Работает и после sticky fault: точный sourceDone возвращает borrow, а не стирает fault. */
bool FrameVideoPipeline::releaseJoinedAudio(const JoinedAudioTicket&t,bool inputSourceReleased)noexcept{
    auto&s=*state_;const auto i=t.output.slot;
    if(!inputSourceReleased||i>=slotsCount||!s.audioHeld[i])return false;
    const auto&p=*s.payload[i];
    if(t.appGeneration!=s.appGeneration||!sameJoinedOutput(t.output,p.outputTicket)||
       !core::sameAudioHeader(t.audio,p.audio.header())||t.audio.endCycle!=p.outputCut.endCycle||
       t.audio.ppuFrame!=p.outputCut.ppuFrame)return false;
    // Tape phase меняется только после exact original audio source reclaim.
    // Сам Ready Payload и его immutable View не перезаписываются Main.
    if(s.fmStorage&&!s.fmStorage->tape[i].release())return false;
    s.audioHeld[i]=false;return true;
}
/** @details Сверяет только held SlotN; Primary Machine и противоположный Worker slot не читаются. */
bool FrameVideoPipeline::borrowJoinedVrc7Tape(const JoinedAudioTicket&t,core::Vrc7WriteTape::View&out)const noexcept{
    const auto&s=*state_;const auto i=t.output.slot;
    if(!s.fmStorage||i>=slotsCount||!s.audioHeld[i]||!s.audioIssued[i])return false;
    const auto&p=*s.payload[i];const auto&v=p.fmTape;
    if(t.appGeneration!=s.appGeneration||!sameJoinedOutput(t.output,p.outputTicket)||
       !core::sameAudioHeader(t.audio,p.audio.header())||v.state!=core::Vrc7WriteTape::State::Sealed||
       v.fault!=core::Vrc7WriteTape::Fault::None||v.events!=s.fmStorage->events[i]||
       v.count>Impl::Vrc7TapeStorage::capacity||v.baseCycle!=t.output.baseCycle||
       v.endCycle!=t.audio.endCycle||v.endCycle!=p.outputCut.endCycle)return false;
    out=v;return true;
}
/** @details Указатель арены неизменен до joined destruction. */
bool FrameVideoPipeline::cpu0Vrc7Enabled()const noexcept{return state_->fmStorage!=nullptr;}
/** @details Только cached SlotN; WorkerN+1 имеет другой Payload и не меняет readyIndex Main. */
std::int64_t FrameVideoPipeline::pendingDeadlineUs()const noexcept{
    const auto&s=*state_;
    if(!s.okay()||s.readyIndex<0||unsigned(s.readyIndex)>=slotsCount)return 0;
    return s.payload[unsigned(s.readyIndex)]->deadline;
}
/** @details Читает только immutable cached capability исходного cold контекста. */
bool FrameVideoPipeline::outputLeaseAtCreate(core::OutputFrameLease&out)const noexcept{
    const auto&s=*state_;
    if(!s.okay()||!s.outputBound||s.produced||s.accepted||s.readyIndex!=-1||
       s.control->workerBorrowed.load(std::memory_order_acquire))return false;
    out=s.outputLease;return true;
}
/** @details Свободный video-slot без sourceDone недостаточен; никаких догадок по таймеру. */
bool FrameVideoPipeline::audioSlotAvailable(std::uint64_t nextInput)const noexcept{
    const auto&s=*state_;
    return s.okay()&&nextInput==s.produced&&!s.audioHeld[unsigned(nextInput%slotsCount)];
}


/**
 * @brief На Main удерживает sealed слот и consumer lease, выполняя DSP до ожидания предыдущего PPA.
 * @details Ppu/RGB/Board не трогает; caller обязан сохранить renderer и проверить reentrant lifecycle после возврата.

 * @param[in] dsp Живой Main-owned AudioRenderer для sealed N; удерживается до colour/abort и не читается Main при Worker loan.
 */
bool FrameVideoPipeline::prepareDsp(core::AudioRenderer&dsp)noexcept{
    auto&s=*state_;
    if(!s.okay()||s.readyIndex<0||unsigned(s.readyIndex)>=slotsCount||s.pendingAcceptance||s.dspPhase!=Impl::DspPhase::Idle)return s.fail(Error::Slot);
    unsigned zero=0;if(!s.control->consumerActive.compare_exchange_strong(zero,1,std::memory_order_acq_rel))return s.fail(Error::Busy);
    
    /**
     * @brief Локальный guard раннего отказа DSP preparation; сохраняет безопасную Aborted-фазу, если подготовка не подтверждена.
     */
    struct Failed {Impl&s;bool keep=false;
                /**
                 * @brief При раннем выходе из подготовки DSP помечает фазу Aborted и снимает consumer-active.
                 * @details При подтверждённом успехе guard оставляет слот и renderer за Main до colour/abort.
                 */
                ~Failed(){if(!keep){s.dspPhase=Impl::DspPhase::Aborted;s.preparedRenderer=nullptr;s.control->consumerActive.store(0,std::memory_order_release);}}}failed{s};
    s.dspPhase=Impl::DspPhase::Preparing;const auto i=unsigned(s.readyIndex);auto&p=*s.payload[i];
    if(s.expired(i)||!s.stageMetadata(i,NES_FRAME_SLOT_STATE::Sealed))return s.fail(s.expired(i)?Error::Deadline:Error::Cut);
    const auto&a=p.audio.header();
    if((s.accepted&&s.mainAudio.sequence==~std::uint64_t{0})||a.generation!=s.mainAudio.generation||a.region!=s.mainAudio.region||
       a.sequence!=(s.accepted?s.mainAudio.sequence+1:s.mainAudio.sequence)||a.baseCycle!=(s.accepted?s.mainAudio.endCycle:s.mainAudio.baseCycle))return s.fail(Error::Cut);
    s.mainAudio=a; // Фиксируем неизменную metadata до входа в настоящий renderer.
    s.latestTimes=p.times;const auto begin=esp_timer_get_time();
    if(!s.audioIssued[i])return s.fail(Error::Audio); // Публикация до C0, без local DSP.
    s.latestTimes.dsp=esp_timer_get_time()-begin;
    if(!s.okay()||s.expired(i)||!s.stageMetadata(i,NES_FRAME_SLOT_STATE::Sealed)||!core::sameAudioHeader(a,s.mainAudio))return s.fail(s.expired(i)?Error::Deadline:Error::Cut);
    s.preparedRenderer=&dsp;s.preparedIndex=static_cast<std::uint8_t>(i);s.dspPhase=Impl::DspPhase::Prepared;failed.keep=true;return true;
}

/**
 * @brief На Main безопасно закрывает подготовленный DSP-stage либо удерживает его при необратимом Worker-владении.
 * @details Не выдаёт отмену за завершение исполняющегося DSP.
 */
bool FrameVideoPipeline::abortPrepared()noexcept{
    auto&s=*state_;
    if(s.dspPhase==Impl::DspPhase::Preparing||s.dspPhase==Impl::DspPhase::Consuming)return s.fail(Error::Busy); // Реальный метод сам обязан снять собственную activity.
    if(s.dspPhase!=Impl::DspPhase::Prepared)return true;
    s.fail(Error::Stopped);s.dspPhase=Impl::DspPhase::Aborted;s.preparedRenderer=nullptr;s.control->consumerActive.store(0,std::memory_order_release);return true;
}

/**
 * @brief После настоящего PPA drain на Main воспроизводит sealed colour N через единственный C0 actor.
 * @details Сверяет реальные read-byte/cut/witness и ранее подготовленный DSP; ошибка запрещает paired принятие.

 * @param[in] dsp Тот же renderer и неизменный DSP-stage N; Board/PPA fence обязан уже выполниться.
 */
bool FrameVideoPipeline::consumeColour(core::AudioRenderer&dsp,bool(*progress)(void*)noexcept,void*context)noexcept{
    auto&s=*state_;if(!s.okay()||s.readyIndex<0||unsigned(s.readyIndex)>=slotsCount||s.pendingAcceptance||s.dspPhase!=Impl::DspPhase::Prepared||
       s.preparedIndex!=unsigned(s.readyIndex)||s.preparedRenderer!=&dsp||!s.control->consumerActive.load(std::memory_order_acquire))return s.fail(Error::Slot);
    
    /**
     * @brief Локальный scope guard, публикующий фактический выход из этапа на всех return-путях.
     * @details Не заменяет внешний worker done и PPA drain.
     */
    struct Finished {Impl&s;bool success=false;
                /**
                 * @brief Закрывает colour-этап и снимает consumerActive после безопасного возврата renderer.
                 * @details При неотменяемом Worker DSP удерживает borrowed slot/context и active-признак; отказ помечает DSP-фазу Aborted.
                 */
                ~Finished(){
        if(!success){s.dspPhase=Impl::DspPhase::Aborted;s.preparedRenderer=nullptr;}
        s.control->consumerActive.store(0,std::memory_order_release);
    }}finished{s};
    s.dspPhase=Impl::DspPhase::Consuming;
    const auto i=unsigned(s.readyIndex);auto&p=*s.payload[i];
    if(s.expired(i)||!s.stageMetadata(i,NES_FRAME_SLOT_STATE::Sealed)||!core::sameAudioHeader(p.audio.header(),s.mainAudio)||
       !s.preparedDspValid()
)return s.fail(s.expired(i)?Error::Deadline:Error::Cut);
    s.rgbAccepted=false;const auto begin=esp_timer_get_time();
    if(!s.renderOutput(i,progress,context))return false;
    s.latestTimes.replay=esp_timer_get_time()-begin;
    if(!s.okay()||s.expired(i)||!s.stageMetadata(i,NES_FRAME_SLOT_STATE::Consumed)||!core::sameAudioHeader(p.audio.header(),s.mainAudio)||
       !s.preparedDspValid()
)return s.fail(s.expired(i)?Error::Deadline:Error::Cut);
    const auto witness=esp_timer_get_time();
    s.pendingSnapshot=p.snapshot;
    s.pendingSnapshot.sampleCount=0; // Отсутствие local PCM явно, C6 output имеет собственный key.
    s.pendingSnapshot.readHash=0;s.pendingSnapshot.readCount=0;
    s.latestTimes.witness=esp_timer_get_time()-witness;
    if(!s.okay()||s.expired(i))return s.fail(Error::Deadline);
    s.pendingAcceptance=true;s.dspPhase=Impl::DspPhase::ColourReady;finished.success=true;return true;
}

/**
 * @brief На Main потребляет предыдущий sealed кадр с реальным DSP и C0 replay либо делегирует EarlyDSP-фазы.
 * @details Нельзя вызывать при владении PPA выбранным RGB или при чужом consumer.

 * @param[in] dsp Эксклюзивный реальный AudioRenderer для paired sealed кадра; указатель живёт до завершения consume.
 */
bool FrameVideoPipeline::consume(core::AudioRenderer&dsp)noexcept{
    if(!prepareDsp(dsp))return false;
    const bool result=consumeColour(dsp);if(!result)static_cast<void>(abortPrepared());return result;
}

/**
 * @brief После проверки обоих владельцев переносит согласованные CPU/RAM/input/audio/colour данные в Main-owned accepted snapshot.
 * @details Частичный или stale кадр не становится accepted.

 * @param[in] generation Текущее App generation, совпадающее с подготовленным paired stage перед публикацией.
 */
bool FrameVideoPipeline::accept(std::uint64_t generation)noexcept{
    auto&s=*state_;if(!s.okay()||!s.pendingAcceptance||s.readyIndex<0||generation!=s.appGeneration||
       s.pendingSnapshot.appGeneration!=generation||s.pendingSnapshot.frame!=s.accepted+1||s.expired(unsigned(s.readyIndex)))return s.fail(Error::Lease);
    if(s.dspPhase!=Impl::DspPhase::ColourReady||s.preparedIndex!=unsigned(s.readyIndex)||!s.preparedDspValid()||
       !s.stageMetadata(unsigned(s.readyIndex),NES_FRAME_SLOT_STATE::Consumed)||!core::sameAudioHeader(s.payload[unsigned(s.readyIndex)]->audio.header(),s.mainAudio))return s.fail(Error::Cut);
    const auto i=unsigned(s.readyIndex);const auto witness=esp_timer_get_time();
    s.acceptedUsage=s.payload[i]->usage; // Ordinary copy до любого Free, только после acquire/consume.
    auto&usage=s.acceptedUsage;const auto&usageTicket=s.payload[i]->outputTicket;const auto&usageCut=s.payload[i]->outputCut;
    const auto&usageLease=usageTicket.lease;
    if(!s.stageOutputMetadata(i,FrameOutputSlot::State::Consumed)||usage.generation!=generation||
       usage.sequence!=usageTicket.sequence||usage.sequence!=s.pendingSnapshot.inputFrame||usage.slot!=i||
       usage.baseCycle!=usageTicket.baseCycle||usage.endCycle!=usageCut.endCycle||usage.endCycle<=usage.baseCycle||
       usage.leaseGeneration!=usageLease.generation||usage.romEpoch!=usageLease.romEpoch||
       usage.transportEpoch!=usageLease.transportEpoch||usage.region!=usageLease.region)usage.status=Impl::UsageStatus::Identity;
    // Даже invalid usage суммируется под настоящим validated accepted key, не испорченным DTO-stamp.
    usage.generation=generation;usage.sequence=usageTicket.sequence;usage.slot=i;
    usage.baseCycle=usageTicket.baseCycle;usage.endCycle=usageCut.endCycle;usage.region=usageLease.region;
    usage.leaseGeneration=usageLease.generation;usage.romEpoch=usageLease.romEpoch;usage.transportEpoch=usageLease.transportEpoch;
    if(!s.stageOutputMetadata(i,FrameOutputSlot::State::Consumed)||
       !matchesFrameRamWitness(s.pendingSnapshot.ram,s.payload[i]->outputTicket,s.payload[i]->outputCut,generation,s.ramPolicy))return s.fail(Error::Lease);
    s.acceptedSnapshot=s.pendingSnapshot;
    if(s.acceptedSnapshot.ram.valid)std::memcpy(s.acceptedRam.data(),s.payload[i]->ram.data(),ramBytes);
    ++s.accepted;
    s.acceptedHart=s.payload[i]->hart;
    const auto&hi=s.acceptedHart.identity;const auto&ht=s.payload[i]->outputTicket;const auto&hl=ht.lease;
    if(hi.producer!=reinterpret_cast<std::uintptr_t>(hl.producer)||hi.rom!=reinterpret_cast<std::uintptr_t>(hl.rom)||
       hi.romBytes!=hl.romBytes||hi.appGeneration!=generation||hi.leaseGeneration!=hl.generation||
       hi.romEpoch!=hl.romEpoch||hi.transportEpoch!=hl.transportEpoch||hi.sequence!=ht.sequence||
       hi.baseCycle!=ht.baseCycle||hi.slot!=i)s.acceptedHart.status=debug::HartStatus::Identity;
    s.acceptedHartGuestCycles=s.payload[i]->outputCut.endCycle-s.payload[i]->outputCut.baseCycle;
    if(!s.outputPackets[i]->release())return s.fail(Error::Slot); // Завершено C0 packet чтение, RGB/PPA отдельно.
    s.control->ready[i].store(0,std::memory_order_release);if(!s.control->packets[i].release())return s.fail(Error::Slot);
    s.usageWindow.add(s.acceptedUsage); // Оба releases успешны; Payload больше не читается этим счётчиком.
    s.mainColourCut.cycle=s.payload[i]->outputCut.endCycle;s.mainColourCut.frame=s.payload[i]->outputCut.ppuFrame;
    s.mainColourCut.scanline=s.payload[i]->outputCut.scanline;s.mainColourCut.dot=s.payload[i]->outputCut.dot;
    s.mainColourCut.phase=s.payload[i]->outputCut.remainder;
    s.dspPhase=Impl::DspPhase::Idle;s.preparedRenderer=nullptr;
    s.hartWindow.add(s.acceptedHart,s.acceptedHartGuestCycles); // После успешных release; исходный Payload уже не читается.
    s.readyIndex=-1;s.pendingAcceptance=false;s.rgbAccepted=true;s.latestTimes.witness+=esp_timer_get_time()-witness;return true;
}

/**
 * @brief В отдельном STATUS-наблюдателе обновляет хэши только Main-owned принятого RGB/PCM.
 * @details Не обращается к producer N+1 и не добавляет большие хэши незаметно в каждый измеряемый кадр.

 * @param[in] dsp Main-owned renderer именно accepted audio generation/sequence/cut; не producer N+1.
 */
bool FrameVideoPipeline::refreshWitness(core::AudioRenderer&dsp)noexcept{
    auto&s=*state_;
    if(!s.okay()||!s.rgbAccepted||!s.accepted||s.pendingAcceptance||s.control->consumerActive.load(std::memory_order_acquire)||
       false)return false;
    // STATUS — отдельно проверяемый observer. Большие хэши не
    // добавляются скрыто в service interval каждого защищённого кадра.
    s.acceptedSnapshot.ramHash=s.acceptedSnapshot.ram.valid?hashBytes(s.acceptedRam.data(),s.acceptedRam.size()):0U;
    static_cast<void>(dsp);s.acceptedSnapshot.pcmHash=0; // Не PCM oracle; actual queue проверяется отдельно.
    s.acceptedSnapshot.rgbHash=hashBytes(s.rgb,rgbBytes);return true;
}

/**
 * @brief Выбирает RAM-witness policy после создания привязанного pipeline, до первого dispatch.
 * @param policy Допустимая частота/полнота диагностики RAM; после первого produced/accepted кадра она не меняется даже на паузе.
 * @details Требует отсутствия Worker/consumer, готового или непарного stage; гостевая RAM не изменяется.
 */
bool FrameVideoPipeline::setRamWitnessPolicyAtCreate(FrameRamPolicy policy)noexcept {
    auto&s=*state_;
    if(!validFrameRamPolicy(policy)||!s.okay()||!s.outputBound||s.produced||s.accepted||
       s.readyIndex!=-1||s.pendingAcceptance||s.control->workerBorrowed.load(std::memory_order_acquire)||
       s.control->consumerActive.load(std::memory_order_acquire))return false;
    s.ramPolicy=policy;s.acceptedSnapshot.ram.policy=policy;return true;
}
/**
 * @brief Обновляет App generation на внешней joined pause-границе, сохраняя ROM, Core cut и transport epoch.
 * @param generation Новое ненулевое поколение Main; caller заранее завершил потребителей.
 * @details Worker borrow, ready/pending stage и удержанные audio loans дают sticky Busy. Обновляется только accepted metadata; Full RAM-witness сохраняет тот же снимок, sparse-диагностика сбрасывается.
 */
bool FrameVideoPipeline::rebindAppGeneration(std::uint64_t generation)noexcept{
    auto&s=*state_;if(!s.okay()||!generation||s.control->workerBorrowed.load(std::memory_order_acquire)||s.readyIndex!=-1||s.pendingAcceptance)return s.fail(Error::Busy);
    if(s.audioHeld[0]||s.audioHeld[1])return s.fail(Error::Busy);
    s.appGeneration=generation;s.acceptedSnapshot.appGeneration=generation;
    // Full сохраняет старую диагностику того же joined cut; гостевой RAM не менялся.
    if(s.ramPolicy==FrameRamPolicy::Full)s.acceptedSnapshot.ram.appGeneration=generation;
    else{s.acceptedSnapshot.ram={};s.acceptedSnapshot.ram.policy=s.ramPolicy;s.acceptedSnapshot.ramHash=0;}
    return true;
}

/**
 * @brief Сверяет неизменные App/ROM/region/generation данные контекста, допустимые во время Worker.
 * @details Актуальный producer epoch/cut дополнительно проверяет joinedProducer после реального done.

 * @param[in] r Адрес удерживаемого ROM текущего контекста.
 * @param[in] n Полный размер ROM image в байтах.
 * @param[in] region Ожидаемый неизменный NTSC/PAL регион.
 * @param[in] generation Текущее App generation, без чтения mutable producer epoch во время job.
 */
bool FrameVideoPipeline::matches(const std::uint8_t*r,std::size_t n,core::Region region,std::uint64_t generation)const noexcept{
    const auto&s=*state_;return s.okay()&&r==s.lease.rom&&n==s.lease.romBytes&&region==s.lease.region&&generation==s.appGeneration;
}

/**
 * @brief Атомарно помечает sticky-остановку и первую причину.
 * @details Borrowed context/slot/ROM остаются живыми до действительного joined teardown.

 * @param[in] e Sticky Error остановки; значение по умолчанию Stopped, без освобождения работающих владельцев.
 */
void FrameVideoPipeline::stop(Error e)noexcept{state_->fail(e);}

/**
 * @brief Возвращает атомарно опубликованную первую ошибку контекста.
 * @details None не отменяет требований владельца для остальных getters.
 */
FrameVideoPipeline::Error FrameVideoPipeline::error()const noexcept{return Error(state_->control->fault.load(std::memory_order_acquire));}

/**
 * @brief Показывает наличие готового sealed слота для Main.
 * @details Признак не равен публикации на LCD и не разрешает чтение producer.
 */
bool FrameVideoPipeline::hasReady()const noexcept{return state_->readyIndex>=0;}




/**
 * @brief Возвращает Main-счётчик подтверждённых producer-кадров.
 * @details Он может опережать accepted/submitted и не является FPS дисплея.
 */
std::uint64_t FrameVideoPipeline::producedFrames()const noexcept{return state_->produced;}

/**
 * @brief Возвращает число принятых обоими владельцами кадров.
 * @details STATUS-drain может принять кадр без LCD submission.
 */
std::uint64_t FrameVideoPipeline::acceptedFrames()const noexcept{return state_->accepted;}

/**
 * @brief Возвращает cycle последнего Main-owned принятого снимка.
 * @details Работающий producer N+1 через getter не читается.
 */
std::uint64_t FrameVideoPipeline::acceptedCycle()const noexcept{return state_->accepted?state_->acceptedSnapshot.cycle:7;}

/**
 * @brief Возвращает заимствованный неизменный Main-owned accepted snapshot.
 * @details Он сохраняет исторический input/CPU/RAM/PCM/RGB кадра N независимо от producer N+1.
 */
const FrameVideoPipeline::Snapshot& FrameVideoPipeline::snapshot()const noexcept{return state_->acceptedSnapshot;}

/**
 * @brief Возвращает Main-owned времена последнего paired кадра.
 * @details C1 producer и C0/DSP интервалы могут перекрываться; их сумма не является длительностью общего цикла.
 */
const FrameVideoPipeline::Times& FrameVideoPipeline::times()const noexcept{return state_->latestTimes;}


/** @details Только Main-owned данные; сброс окна не затрагивает producer N+1. */
void FrameVideoPipeline::clearHartStats()noexcept{state_->hartWindow={};}

/** @details Сырые суммы не превращаются в stall cycles; CPI включает IRQ/вытеснение HP hart. */
void FrameVideoPipeline::reportHartStats(unsigned game)noexcept{
    const auto&w=state_->hartWindow;
    ESP_LOGI("nes_app","NES_FRAME_HART_STATS schema=1 game=%u generation=%llu first_sequence=%llu last_sequence=%llu samples=%llu valid=%llu invalid=%llu overflow=%u cycles=%llu retired=%llu guest_frame_cycles=%llu cycle_min=%llu cycle_max=%llu retired_min=%llu retired_max=%llu scope=stepFrame_hart task_exclusive=0 guest_scope=sealed_frame_cut guest_valid_samples_only=1 old_core_scope=stepFrame_tail_probe game_fps_claim=0",
        game,static_cast<unsigned long long>(w.generation),static_cast<unsigned long long>(w.first),static_cast<unsigned long long>(w.last),
        static_cast<unsigned long long>(w.samples),static_cast<unsigned long long>(w.valid),static_cast<unsigned long long>(w.invalid),unsigned(w.overflow),
        static_cast<unsigned long long>(w.cycles),static_cast<unsigned long long>(w.retired),static_cast<unsigned long long>(w.guestFrameCycles),
        static_cast<unsigned long long>(w.cycleMin),static_cast<unsigned long long>(w.cycleMax),static_cast<unsigned long long>(w.retiredMin),static_cast<unsigned long long>(w.retiredMax));
    ESP_LOGI("nes_app","NES_FRAME_HART_HIST schema=1 game=%u generation=%llu cpi_q10_limits=512,768,1024,1280,1536,2048,3072 bins=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
        game,static_cast<unsigned long long>(w.generation),
        static_cast<unsigned long long>(w.cpiBins[0]),static_cast<unsigned long long>(w.cpiBins[1]),static_cast<unsigned long long>(w.cpiBins[2]),static_cast<unsigned long long>(w.cpiBins[3]),
        static_cast<unsigned long long>(w.cpiBins[4]),static_cast<unsigned long long>(w.cpiBins[5]),static_cast<unsigned long long>(w.cpiBins[6]),static_cast<unsigned long long>(w.cpiBins[7]));
    ESP_LOGI("nes_app","NES_FRAME_HART_QUALITY schema=1 game=%u status_order=empty,recording,ok,unsupported,wrongcore,inhibited,unstable,context,nonmonotone,noretired,arithmetic,identity counts=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
        game,static_cast<unsigned long long>(w.statusCounts[0]),static_cast<unsigned long long>(w.statusCounts[1]),static_cast<unsigned long long>(w.statusCounts[2]),static_cast<unsigned long long>(w.statusCounts[3]),
        static_cast<unsigned long long>(w.statusCounts[4]),static_cast<unsigned long long>(w.statusCounts[5]),static_cast<unsigned long long>(w.statusCounts[6]),static_cast<unsigned long long>(w.statusCounts[7]),
        static_cast<unsigned long long>(w.statusCounts[8]),static_cast<unsigned long long>(w.statusCounts[9]),static_cast<unsigned long long>(w.statusCounts[10]),static_cast<unsigned long long>(w.statusCounts[11]));
    clearHartStats();
}

/**
 * @brief Возвращает RGB только принятого и проверенного Main кадра.
 * @details Даже getter допустим лишь при выполненном caller PPA fence и действующем контексте.
 */
const std::uint16_t*FrameVideoPipeline::pixels()const noexcept{return state_->rgbAccepted&&state_->okay()?state_->rgb:nullptr;}
}
