#include "FramePipeline.h"
#include <cstdio>
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
 * @brief Проверяет требуемые размер, выравнивание и INTERNAL/DMA/8BIT свойства постоянного RGB source.
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
    return esp_ptr_internal(p)&&esp_ptr_internal(end)&&esp_ptr_dma_capable(p)&&esp_ptr_dma_capable(end)&&
        esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&!esp_ptr_external_ram(p)&&!esp_ptr_external_ram(end);
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
 * @brief Проверяет оба конца выровненного INTERNAL/8BIT диапазона владельца.
 * @param p Начало живого хранилища сеанса; проверка не передаёт владение.
 * @param bytes Ненулевой размер полного диапазона в байтах.
 * @param alignment Ненулевое требование к выравниванию в байтах; фабрика передаёт 16.
 */
bool rasterInternal(const void*p,std::size_t bytes,std::size_t alignment)noexcept{
    std::uintptr_t first=0,last=0;
    if(!checkedMemoryExtent(p,bytes,first,last)||first%alignment)return false;
    const auto*end=reinterpret_cast<const void*>(last-1);
    return esp_ptr_internal(p)&&esp_ptr_internal(end)&&esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&
        !esp_ptr_external_ram(p)&&!esp_ptr_external_ram(end);
}

/**
 * @brief Проверяет оба конца непустого PSRAM/8BIT диапазона сеанса.
 * @param p Начало живого заимствованного хранилища.
 * @param bytes Ненулевой размер полного диапазона в байтах.
 */
bool rasterExternal(const void*p,std::size_t bytes)noexcept{
    std::uintptr_t first=0,last=0;if(!checkedMemoryExtent(p,bytes,first,last))return false;
    const auto*end=reinterpret_cast<const void*>(last-1);
    return esp_ptr_external_ram(p)&&esp_ptr_external_ram(end)&&esp_ptr_byte_accessible(p)&&esp_ptr_byte_accessible(end)&&
        !esp_ptr_internal(p)&&!esp_ptr_internal(end);
}

/**
 * @brief Вычисляет рабочий FNV-свидетель по указанным живым байтам.
 * @details Хэш не является бесколлизионным разрешением пропускать эмуляцию.

 * @param[in] p Живое читаемое начало RAM/PCM/RGB accepted-данных.
 * @param[in] n Точное число байтов хэширования, не число samples или пикселей.
 * @param[in] h Начальное FNV-состояние, позволяющее продолжить hash; не гостевой open bus.
 */
std::uint64_t hashBytes(const void*p,std::size_t n,std::uint64_t h=1469598103934665603ULL)noexcept{
    const auto*b=static_cast<const std::uint8_t*>(p);for(std::size_t i=0;i<n;++i)h=(h^b[i])*1099511628211ULL;return h;
}


}

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

/** @brief Рабочее хранилище Main/Worker; доступ к слотам передаётся Control, память сохраняется до полного join. */ struct FramePipeline::Impl final {
    
    /**
     * @brief Неподвижная INTERNAL область атомарного stop/slot handoff и audio control.
     * @details Ordinary payload защищается публикацией владения, а не volatile.
     */
    struct alignas(64) Control {
        std::atomic<unsigned> fault{0},workerBorrowed{0},workerFinished{0},consumerActive{0};
        std::atomic<unsigned> ready[slotsCount]{};
        FrameOutputSlot packets[slotsCount];
        
        /**
         * @brief Инициализирует ready двух INTERNAL слотов; не публикует Worker и не выделяет память.
         * @details Неподвижный Control живёт до join обеих сторон; инициализация не заимствует event-массивы.
        
         * Два Output-слота уже входят в Control и получают обычные начальные значения своих полей.
         * Factory отвечает за INTERNAL память и выравнивание; публикация выполняется только последующим job handoff.
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
    };
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
    /**
     * @brief Связывает проверенные буферы с producer и начальным cut после reset.
     * @param c Атомарные роли фабричного контекста.
     * @param x Первый slot payload; после публикации читается CPU0.
     * @param y Второй payload для перекрытия следующего кадра.
     * @param r Свободный от PPA RGB565-буфер Board.
     * @param m Остановленная первичная Machine после reset на седьмом CPU-такте.
     * @param l Неизменный ROM lease без передачи владения.
     * @param audio Согласованный цифровой audio-header того же cut.
     * @details Память не выделяется, Worker не запускается; все ссылки
     * удерживаются фабричным владельцем до настоящего join и destroyJoined.
     */
    Impl(Control*c,
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
    bool renderOutput(unsigned i)noexcept;
    /** @brief После общих join/consumer/loan checks снимает Core/output-аренды до free. */
    bool releaseOutputJoined()noexcept;
    /** @brief Сверяет завершённый DSP renderer с принадлежащим Main заголовком парного аудиопакета. */
    bool preparedDspValid()const noexcept{
        return preparedRenderer&&preparedRenderer->valid()&&preparedRenderer->generation()==mainAudio.generation&&
            preparedRenderer->sequence()==mainAudio.sequence&&preparedRenderer->ppuFrame()==mainAudio.ppuFrame&&
            preparedRenderer->cycles()==mainAudio.endCycle&&preparedRenderer->sampleCount()<=core::Apu::sampleCapacity;
    }
};
#include "FrameOutputTransport.inl"

#include "FrameOutputFactory.inl"

/**
 * @brief Освобождает контекст только после реального Worker done и отсутствия активного Main consumer/DSP.
 * @details При неизвестном владении возвращает false и удерживает память/ROM; порядок detach предшествует free.

 * @param[in] p Фабричный контекст после настоящего Worker done; неизвестное владение возвращает false с удержанием всех ресурсов.
 */
bool FramePipeline::destroyJoined(FramePipeline*p)noexcept{
    if(!p)return true;
    auto*s=p->state_;
    if(s->control->workerBorrowed.load(std::memory_order_acquire)||s->control->consumerActive.load(std::memory_order_acquire))return false;
    s->fail(Error::Stopped);
    if(!s->releaseOutputJoined())return false; // Ни один borrowed ресурс не освобождается при неизвестности.
    auto*c=s->control;auto*x=s->payload[0];auto*y=s->payload[1];
    auto*a=s->outputPackets[0];auto*b=s->outputPackets[1];
    auto*storage=s->outputStorage;auto*renderer=s->outputRenderer;
    renderer->~VideoRenderer();storage->~VideoStorage();a->~VideoPacket();b->~VideoPacket();
    s->~Impl();c->~Control();x->~Payload();y->~Payload();p->~FramePipeline();
    heap_caps_free(p);heap_caps_free(s);heap_caps_free(renderer);heap_caps_free(storage);
    heap_caps_free(c);heap_caps_free(y);heap_caps_free(x);heap_caps_free(b);heap_caps_free(a);return true;
}

/**
 * @brief На Main перед dispatch резервирует свободный producer-слот и фиксирует inputFrame/pad/App generation.
 * @details Не читает работающий producer и отказывает при незавершённой предыдущей передаче.

 * @param[in] inputFrame Исторический номер кадра ввода для следующего CPU job, не номер LCD submission.
 * @param[in] pad Фактическая восьмибитная маска pad, применяемая к этому job.
 * @param[in] generation Текущее App generation; повторное использование старого сеанса запрещено.
 */
bool FramePipeline::prepareJob(std::uint64_t inputFrame,std::uint8_t pad,std::uint64_t generation)noexcept{
    auto&s=*state_;if(!s.okay()||s.control->workerBorrowed.load(std::memory_order_acquire)||s.pendingAcceptance||
        inputFrame!=s.produced||generation!=s.appGeneration||s.produced-s.accepted>1)return s.fail(Error::Sequence);
    const unsigned index=unsigned(inputFrame%slotsCount);
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
core::RunStatus FramePipeline::produce()noexcept{
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
bool FramePipeline::joinedProducer()noexcept{
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

/**
 * @brief На Main удерживает sealed слот и consumer lease, выполняя DSP до ожидания предыдущего PPA.
 * @details Ppu/RGB/Board не трогает; caller обязан сохранить renderer и проверить reentrant lifecycle после возврата.

 * @param[in] dsp Живой Main-owned AudioRenderer для sealed N; удерживается до colour/abort и не читается Main при Worker loan.
 */
bool FramePipeline::prepareDsp(core::AudioRenderer&dsp)noexcept{
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
    if(dsp.render(p.audio)!=core::AudioRenderResult::Ok||!dsp.valid()||dsp.generation()!=a.generation||dsp.sequence()!=a.sequence||dsp.ppuFrame()!=a.ppuFrame||dsp.cycles()!=a.endCycle||dsp.sampleCount()>core::Apu::sampleCapacity)return s.fail(Error::Dsp);
    s.latestTimes.dsp=esp_timer_get_time()-begin;
    if(!s.okay()||s.expired(i)||!s.stageMetadata(i,NES_FRAME_SLOT_STATE::Sealed)||!core::sameAudioHeader(a,s.mainAudio))return s.fail(s.expired(i)?Error::Deadline:Error::Cut);
    s.preparedRenderer=&dsp;s.preparedIndex=static_cast<std::uint8_t>(i);s.dspPhase=Impl::DspPhase::Prepared;failed.keep=true;return true;
}

/**
 * @brief На Main безопасно закрывает подготовленный DSP-stage либо удерживает его при необратимом Worker-владении.
 * @details Не выдаёт отмену за завершение исполняющегося DSP.
 */
bool FramePipeline::abortPrepared()noexcept{
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
bool FramePipeline::consumeColour(core::AudioRenderer&dsp)noexcept{
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
    if(!s.renderOutput(i))return false;
    s.latestTimes.replay=esp_timer_get_time()-begin;
    if(!s.okay()||s.expired(i)||!s.stageMetadata(i,NES_FRAME_SLOT_STATE::Consumed)||!core::sameAudioHeader(p.audio.header(),s.mainAudio)||
       !s.preparedDspValid()
)return s.fail(s.expired(i)?Error::Deadline:Error::Cut);
    const auto witness=esp_timer_get_time();
    s.pendingSnapshot=p.snapshot;s.pendingSnapshot.sampleCount=dsp.sampleCount();
    s.pendingSnapshot.readHash=0;s.pendingSnapshot.readCount=0;
    s.latestTimes.witness=esp_timer_get_time()-witness;
    if(!s.okay()||s.expired(i))return s.fail(Error::Deadline);
    s.pendingAcceptance=true;s.dspPhase=Impl::DspPhase::ColourReady;finished.success=true;return true;
}

/**
 * @brief После проверки обоих владельцев переносит согласованные CPU/RAM/input/audio/colour данные в Main-owned accepted snapshot.
 * @details Частичный или stale кадр не становится accepted.

 * @param[in] generation Текущее App generation, совпадающее с подготовленным paired stage перед публикацией.
 */
bool FramePipeline::accept(std::uint64_t generation)noexcept{
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
bool FramePipeline::refreshWitness(core::AudioRenderer&dsp)noexcept{
    auto&s=*state_;
    if(!s.okay()||!s.rgbAccepted||!s.accepted||s.pendingAcceptance||s.control->consumerActive.load(std::memory_order_acquire)||
       dsp.generation()!=s.acceptedSnapshot.audioGeneration||dsp.sequence()!=s.acceptedSnapshot.audioSequence||
       dsp.ppuFrame()!=s.acceptedSnapshot.frame||dsp.cycles()!=s.acceptedSnapshot.cycle||
       dsp.sampleCount()!=s.acceptedSnapshot.sampleCount)return false;
    // STATUS — отдельно проверяемый observer. Большие хэши не
    // добавляются скрыто в service interval каждого защищённого кадра.
    s.acceptedSnapshot.ramHash=s.acceptedSnapshot.ram.valid?hashBytes(s.acceptedRam.data(),s.acceptedRam.size()):0U;
    s.acceptedSnapshot.pcmHash=hashBytes(dsp.samples(),dsp.sampleCount()*sizeof(std::int16_t));
    s.acceptedSnapshot.rgbHash=hashBytes(s.rgb,rgbBytes);return true;
}

/**
 * @brief На joined pause-границе обновляет App generation без подмены ROM/cut/transport epoch.
 * @details Требует завершённых владельцев и отсутствия непарного stage.

 * @param[in] generation Новое ненулевое App generation на joined pause-границе; transport epoch/ROM не меняются.
 */
/** @details Нет dispatch/reader, policy после первого кадра не меняется даже на паузе. */
bool FramePipeline::setRamWitnessPolicyAtCreate(FrameRamPolicy policy)noexcept {
    auto&s=*state_;
    if(!validFrameRamPolicy(policy)||!s.okay()||!s.outputBound||s.produced||s.accepted||
       s.readyIndex!=-1||s.pendingAcceptance||s.control->workerBorrowed.load(std::memory_order_acquire)||
       s.control->consumerActive.load(std::memory_order_acquire))return false;
    s.ramPolicy=policy;s.acceptedSnapshot.ram.policy=policy;return true;
}
/**
 * @brief Перепривязывает остановленный конвейер к поколению интерфейса.
 * @param generation Ненулевое поколение Main после паузы.
 * @return false и sticky Busy при незавершённом владельце либо слоте.
 * @details Гостевое состояние не сбрасывается; RAM-снимок сохраняется
 * только при полном согласованном cut.
 */
bool FramePipeline::rebindAppGeneration(std::uint64_t generation)noexcept{
    auto&s=*state_;if(!s.okay()||!generation||s.control->workerBorrowed.load(std::memory_order_acquire)||s.readyIndex!=-1||s.pendingAcceptance)return s.fail(Error::Busy);
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
bool FramePipeline::matches(const std::uint8_t*r,std::size_t n,core::Region region,std::uint64_t generation)const noexcept{
    const auto&s=*state_;return s.okay()&&r==s.lease.rom&&n==s.lease.romBytes&&region==s.lease.region&&generation==s.appGeneration;
}

/**
 * @brief Атомарно помечает sticky-остановку и первую причину.
 * @details Borrowed context/slot/ROM остаются живыми до действительного joined teardown.

 * @param[in] e Sticky Error остановки; значение по умолчанию Stopped, без освобождения работающих владельцев.
 */
void FramePipeline::stop(Error e)noexcept{state_->fail(e);}

/**
 * @brief Возвращает атомарно опубликованную первую ошибку контекста.
 * @details None не отменяет требований владельца для остальных getters.
 */
FramePipeline::Error FramePipeline::error()const noexcept{return Error(state_->control->fault.load(std::memory_order_acquire));}

/**
 * @brief Показывает наличие готового sealed слота для Main.
 * @details Признак не равен публикации на LCD и не разрешает чтение producer.
 */
bool FramePipeline::hasReady()const noexcept{return state_->readyIndex>=0;}

/**
 * @brief Возвращает Main-счётчик подтверждённых producer-кадров.
 * @details Он может опережать accepted/submitted и не является FPS дисплея.
 */
std::uint64_t FramePipeline::producedFrames()const noexcept{return state_->produced;}

/**
 * @brief Возвращает число принятых обоими владельцами кадров.
 * @details STATUS-drain может принять кадр без LCD submission.
 */
std::uint64_t FramePipeline::acceptedFrames()const noexcept{return state_->accepted;}

/**
 * @brief Возвращает cycle последнего Main-owned принятого снимка.
 * @details Работающий producer N+1 через getter не читается.
 */
std::uint64_t FramePipeline::acceptedCycle()const noexcept{return state_->accepted?state_->acceptedSnapshot.cycle:7;}

/**
 * @brief Возвращает заимствованный неизменный Main-owned accepted snapshot.
 * @details Он сохраняет исторический input/CPU/RAM/PCM/RGB кадра N независимо от producer N+1.
 */
const FramePipeline::Snapshot& FramePipeline::snapshot()const noexcept{return state_->acceptedSnapshot;}

/**
 * @brief Возвращает Main-owned времена последнего paired кадра.
 * @details C1 producer и C0/DSP интервалы могут перекрываться; их сумма не является длительностью общего цикла.
 */
const FramePipeline::Times& FramePipeline::times()const noexcept{return state_->latestTimes;}


/** @details Только Main-owned данные; сброс окна не затрагивает producer N+1. */
void FramePipeline::clearHartStats()noexcept{state_->hartWindow={};}

/** @details Сырые суммы не превращаются в stall cycles; CPI включает IRQ/вытеснение HP hart. */
void FramePipeline::reportHartStats(unsigned game)noexcept{
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
const std::uint16_t*FramePipeline::pixels()const noexcept{return state_->rgbAccepted&&state_->okay()?state_->rgb:nullptr;}
}
