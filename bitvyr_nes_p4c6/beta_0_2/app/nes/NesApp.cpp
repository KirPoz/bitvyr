/**
 * @file NesApp.cpp
 * @brief Координация консоли: библиотека, настройки, кадры, владение Worker и безопасный UART.
 */
#include "NesApp.h"
#include "AudioStageToken.h"
#include "audio/c6/runtime/MainOwner.h"
#include "audio/c6/qualification/MainPumpCost.h"
#include "audio/PcmStreamGuard.h"
#include "esp_random.h"
#include "storage/ZipArchive.h"
#include "storage/Mmc5ChrPadding.h"
#include "cartridge/Mmc5.h"
#include "cartridge/Mmc5Audio.h"
#include "esp_memory_utils.h"
namespace {
/** @brief MMC5 использует Thin/Coupled. Остальные картриджи обслуживает Frame. */
constexpr bool usesCoupledVideo(std::uint16_t mapper) noexcept {
    return mapper == 5U;
}

/** @brief Единственный ленивый блок цифрового MMC5/звука; WRAM остаётся существующей8KiB RAM картриджа. */
struct Mmc5Session final { nes::core::Mmc5 digital; nes::core::Mmc5Audio audio; };
}
#include "Palette.h"
#include "ppu/VideoRenderer.h"
#include "graphics/MenuLayout.h"
#include "graphics/GameControls.h"
#include "graphics/GameChrome.h"
#include "graphics/PauseMenu.h"
#include "apu/AudioRenderer.h"
#include "console/BufferedUart.h"
#include "esp_chip_info.h"
#include <cstdarg>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
// Использует публичный getter текущего CPU; прямого доступа к TCB нет.
#if !configGENERATE_RUN_TIME_STATS || !INCLUDE_xTaskGetIdleTaskHandle || !defined(CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32)
#error "WORKER_IDLE_PROGRESS requires the existing U32 idle runtime API"
#endif
#if defined(CONFIG_FREERTOS_SMP) && CONFIG_FREERTOS_SMP
#error "WORKER_IDLE_PROGRESS is qualified only for the current legacy FreeRTOS port"
#endif
#if !configGENERATE_RUN_TIME_STATS || !INCLUDE_xTaskGetIdleTaskHandle || !defined(CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U32) || !defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER)
#error "COUPLED_MAIN_IDLE_BUDGET requires the qualified U32 idle runtime API"
#endif
#if (defined(CONFIG_FREERTOS_SMP) && CONFIG_FREERTOS_SMP) || !defined(CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0) || configTICK_RATE_HZ != 1000
#error "COUPLED_MAIN_IDLE_BUDGET requires legacy CPU0 Main and1000Hz tick"
#endif
#include "runtime/coupled/MainIdleBudget.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>

#include "runtime/coupled/CoupledVideoPipeline.h"
#include "runtime/coupled/DonePollBatch.h"
#include "runtime/frame/FrameVideoPipeline.h"
#include "audio/c6/runtime/PostJoinFrameAudio.h"
#include "audio/c6/runtime/FrameFmBridgeAllocation.h"
#ifndef NES_APP_CPU0_VRC7
#define NES_APP_CPU0_VRC7 0
#endif
#define NES_APP_FRAME_INPUT_INDEX framePipelineInputFrame()
#define NES_APP_FRAME_PACE_CYCLES framePipelineCycle()
namespace nes::app {
#include "input/TouchOverlap.inl"

namespace {
// Диагностика принадлежит Main; наблюдатель производителя, планировщик и состояние ядра не затрагиваются.
/** @brief Фазы измеряемой Main-итерации; интервалы классифицируются без суммирования параллельной работы CPU1/PPA. */
enum class LoopPhase : unsigned {Join,Cli,Pace,Input,Dispatch,Consume,Report,Yield,Other,Count};
/** @brief Идентичность итерации и признак допустимости измерения; не является владельцем Machine. */
struct LoopMeta final {
    const void*worker=nullptr;const void*context=nullptr;
    std::uint64_t generation=0,accepted=0,submitted=0,started=0;
    std::uint64_t snapshotGeneration=0,snapshotFrame=0,dispatchedInput=0;
    std::uint64_t pendingGeneration=0,pendingFrame=0;
    bool pending=false;
};
/** @brief Main-рекордер этапов цикла с ограниченным окном; команды/наблюдатели помечают затронутые измерения. */
struct FrameLoopMetrics final {
    static constexpr unsigned phases=unsigned(LoopPhase::Count);
    const void*owner=nullptr;bool active=false,dirty=false;
    LoopMeta before{},after{};
    std::int64_t began=0,last=0;LoopPhase current=LoopPhase::Join;
    std::uint64_t elapsed[phases]{};
    bool joinedInputValid=false;
    std::uint64_t joinedInput=0,joinedInputUs=0;
    std::uint64_t joinWorkerWaitUs=0,joinPreviousSourceUs=0;
    /** @brief Накопители текущего окна Main-метрик; не содержат изменяемое гостевое состояние. */
    struct Window final {
        const void*worker=nullptr;const void*context=nullptr;
        unsigned count=0;std::uint64_t generation=0,first=0,last=0;
        std::uint64_t submittedBefore=0,submittedAfter=0,startedBefore=0,startedAfter=0;
        std::uint64_t dispatchedFirst=0,dispatchedLast=0;
        std::uint64_t acceptedInputFirst=0,acceptedInputLast=0,acceptedInputUs=0;
        std::uint64_t wall=0,elapsed[phases]{};
        std::uint64_t maxLoopUs=0,maxLoopAcceptedFrame=0,maxStageUs[phases]{};
        // Три величины относятся к одной итерации с наибольшим outer join.
        std::uint64_t slowJoinInput=0,slowJoinWorkerUs=0,slowJoinSourceUs=0;
        bool pending=false;std::uint64_t pendingGeneration=0,pendingFrame=0;
    }window{};
    /**
     * @brief Связывает фиксированное хранилище метрик с Main-циклом до начала сбора.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     */
    void bind(const void*app)noexcept{owner=app;active=dirty=false;window={};}
    /**
     * @brief Проверяет, относится ли активное окно метрик к указанному экземпляру приложения.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     */
    bool activeFor(const void*app)const noexcept{return owner==app&&active;}
    /**
     * @brief Снимает активное окно метрик без публикации неполного результата.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     */
    void cancel(const void*app)noexcept{if(owner!=app)return;active=false;dirty=true;window={};}
    /**
     * @brief Помечает окно затронутым наблюдателем/командой, чтобы не выдавать его за чистый замер.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     */
    void taint(const void*app)noexcept{if(owner!=app)return;dirty=true;window={};}
    /**
     * @brief Начинает измерение очередной Main-итерации с её точной идентичностью.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     */
    void begin(const void*app)noexcept{
        if(owner!=app)return;
        if(active)window={};
        before={};after={};for(auto&v:elapsed)v=0;
        joinedInputValid=false;joinedInput=joinedInputUs=0;dirty=false;active=true;
        joinWorkerWaitUs=joinPreviousSourceUs=0;
        current=LoopPhase::Join;began=last=esp_timer_get_time();
    }
    /**
     * @brief Закрывает предыдущий этап Main-цикла и начинает следующий по одному host-таймеру.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     * @param next Следующий этап измеряемой Main-итерации.
     */
    void phase(const void*app,LoopPhase next)noexcept{
        if(!activeFor(app)||next==current)return;
        const auto now=esp_timer_get_time();
        if(now<last){cancel(app);return;}
        elapsed[unsigned(current)]+=static_cast<std::uint64_t>(now-last);
        current=next;last=now;
    }
    /**
     * @brief Отмечает подтверждённое завершение задания в текущем измерительном окне.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     * @param input Номер гостевого input-кадра/задания, связанного с завершённым join.
     * @param inputUs Обслуживание ввода для этого кадра, мкс.
     * @param workerUs Ожидание настоящего done и подтверждение join, мкс.
     * @param sourceUs Ожидание освобождения предыдущего источника C6 после join, мкс.
     */
    void joined(const void*app,std::uint64_t input,std::int64_t inputUs,
                std::int64_t workerUs,std::int64_t sourceUs)noexcept{
        if(!activeFor(app)||inputUs<0||workerUs<0||sourceUs<0)return;
        joinedInput=input;joinedInputUs=static_cast<std::uint64_t>(inputUs);joinedInputValid=true;
        joinWorkerWaitUs=static_cast<std::uint64_t>(workerUs);
        joinPreviousSourceUs=static_cast<std::uint64_t>(sourceUs);
    }
    /**
     * @brief Закрывает измерение Main-итерации; публикуются только внутренне согласованные этапы.
     * @param app Непрозрачная идентичность экземпляра Main; для сравнения, без разыменования.
     */
    [[gnu::noinline]] void finish(const void*app)noexcept{
        if(!activeFor(app))return;
        const auto now=esp_timer_get_time();active=false;
        if(now<last||now<began){window={};return;}
        elapsed[unsigned(current)]+=static_cast<std::uint64_t>(now-last);
        const auto wall=static_cast<std::uint64_t>(now-began);
        if(dirty||!before.generation||before.generation!=after.generation||
           before.worker!=after.worker||before.context!=after.context||
           before.accepted==~std::uint64_t{0}||after.accepted!=before.accepted+1||
           after.snapshotGeneration!=after.generation||after.snapshotFrame!=after.accepted||
           before.started==~std::uint64_t{0}||after.started!=before.started+1||
           after.submitted<before.submitted||after.submitted>after.accepted||
           after.dispatchedInput!=after.accepted||!joinedInputValid||
           joinedInput!=after.accepted-1||
           (after.pending&&(after.pendingGeneration!=after.generation||after.pendingFrame!=after.accepted))){window={};return;}
        std::uint64_t total=0;for(auto v:elapsed)total+=v;
        if(total!=wall){window={};return;}
        auto&w=window;
        if(!w.count){
            if(before.accepted%120)return;
            w.worker=before.worker;w.context=before.context;w.generation=before.generation;
            w.first=after.accepted;w.last=before.accepted;
            w.submittedBefore=before.submitted;w.startedBefore=before.started;
            w.dispatchedFirst=after.dispatchedInput;w.acceptedInputFirst=joinedInput;
        }
        if(w.worker!=before.worker||w.context!=before.context||w.generation!=after.generation||w.last!=before.accepted){window={};return;}
        ++w.count;w.last=after.accepted;w.submittedAfter=after.submitted;w.startedAfter=after.started;
        w.dispatchedLast=after.dispatchedInput;w.acceptedInputLast=joinedInput;w.acceptedInputUs+=joinedInputUs;
        w.pending=after.pending;w.pendingGeneration=after.pendingGeneration;w.pendingFrame=after.pendingFrame;
        w.wall+=wall;for(unsigned i=0;i<phases;++i)w.elapsed[i]+=elapsed[i];
        if(w.count==1||wall>w.maxLoopUs){w.maxLoopUs=wall;w.maxLoopAcceptedFrame=after.accepted;}
        constexpr auto join=unsigned(LoopPhase::Join);
        if(w.count==1||elapsed[join]>w.maxStageUs[join]){
            w.slowJoinInput=joinedInput;
            w.slowJoinWorkerUs=joinWorkerWaitUs;
            w.slowJoinSourceUs=joinPreviousSourceUs;
        }
        for(unsigned i=0;i<phases;++i)w.maxStageUs[i]=std::max(w.maxStageUs[i],elapsed[i]);
        if(w.count!=120)return;
        if(w.last%120||w.last-w.first!=119){window={};return;}
        // Теперь запись завершена полностью, включая настоящий end-yield.
        // Сам диагностический вывод намеренно находится за пределами измеряемого интервала.
        emitWindow();
        window={};
    }
    // Аргументы форматирования вынесены из глубоких стеков run/CLI/retire.
    // Слот принадлежит Main; принятая строка снимается только после успешной отправки.
    static inline char pendingReport[1801]{};
    static inline unsigned pendingSize=0;

    /** @brief Проверяет UART без ожидания на очередной итерации Main. */
    static void flushReportIfIdle()noexcept{
        if(pendingSize&&console::tryWriteReportLine(pendingReport,pendingSize))pendingSize=0;
    }

    /** @brief Готовит строку. Если предыдущая всё ещё ждёт, передаёт её штатно перед заменой. */
    [[gnu::noinline]] static void scheduleReport(const char* format,std::va_list arguments)noexcept{
        if(pendingSize){
            // Если UART был постоянно занят весь предыдущий интервал, сохраняем исходную строку.
            static_cast<void>(std::fwrite(pendingReport,1,pendingSize,stdout));
            pendingSize=0;
        }
        std::va_list copy;
        va_copy(copy,arguments);
        const int size=std::vsnprintf(pendingReport,sizeof(pendingReport),format,copy);
        va_end(copy);
        if(size>0&&static_cast<unsigned>(size)<sizeof(pendingReport))pendingSize=static_cast<unsigned>(size);
        else std::vprintf(format,arguments);
    }

    /** @brief Формирует полную строку, откладывая отправку при занятом UART. */
    [[gnu::format(printf,1,2)]] static void writeReport(const char* format,...)noexcept{
        std::va_list arguments;
        va_start(arguments,format);
        if(console::bufferedReportAvailable())scheduleReport(format,arguments);
        else std::vprintf(format,arguments);
        va_end(arguments);
    }

    /**
     * @brief Печатает собранное окно этапов вне глубокой цепочки рендера, ограничивая расход стека printf.
     */
    [[gnu::noinline]] void emitWindow()const noexcept{
        const auto&w=window;
        writeReport("NES_FRAME_LOOP_METRICS generation=%llu accepted_first=%llu accepted_last=%llu completed_main_iterations=120 submitted_before=%llu submitted_after=%llu present_started_before=%llu present_started_after=%llu pending_after=%u pending_generation_after=%llu pending_frame_after=%llu dispatched_input_first=%llu dispatched_input_last=%llu accepted_input_first=%llu accepted_input_last=%llu accepted_input_service_us=%llu loop_wall_us=%llu outer_join_us=%llu cli_menu_us=%llu pace_us=%llu input_current_us=%llu dispatch_us=%llu consume_present_us=%llu report_us=%llu end_yield_us=%llu remaining_us=%llu loop_wall_max_us=%llu loop_wall_max_accepted_frame=%llu outer_join_max_us=%llu cli_menu_max_us=%llu pace_max_us=%llu input_current_max_us=%llu dispatch_max_us=%llu consume_present_max_us=%llu report_max_us=%llu end_yield_max_us=%llu remaining_max_us=%llu slow_join_input=%llu slow_join_worker_us=%llu slow_join_previous_c6_source_us=%llu stage_maxima_sum_valid=0 main_core=0 unit=completed_main_iteration game_fps_claim=0 producer_time_added=0 separate_ppa_elapsed_added=0\n",
            static_cast<unsigned long long>(w.generation),static_cast<unsigned long long>(w.first),static_cast<unsigned long long>(w.last),
            static_cast<unsigned long long>(w.submittedBefore),static_cast<unsigned long long>(w.submittedAfter),
            static_cast<unsigned long long>(w.startedBefore),static_cast<unsigned long long>(w.startedAfter),unsigned(w.pending),
            static_cast<unsigned long long>(w.pendingGeneration),static_cast<unsigned long long>(w.pendingFrame),
            static_cast<unsigned long long>(w.dispatchedFirst),static_cast<unsigned long long>(w.dispatchedLast),
            static_cast<unsigned long long>(w.acceptedInputFirst),static_cast<unsigned long long>(w.acceptedInputLast),
            static_cast<unsigned long long>(w.acceptedInputUs/120),static_cast<unsigned long long>(w.wall/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Join)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Cli)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Pace)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Input)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Dispatch)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Consume)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Report)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Yield)]/120),
            static_cast<unsigned long long>(w.elapsed[unsigned(LoopPhase::Other)]/120),
            static_cast<unsigned long long>(w.maxLoopUs),static_cast<unsigned long long>(w.maxLoopAcceptedFrame),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Join)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Cli)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Pace)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Input)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Dispatch)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Consume)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Report)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Yield)]),
            static_cast<unsigned long long>(w.maxStageUs[unsigned(LoopPhase::Other)]),
            static_cast<unsigned long long>(w.slowJoinInput),
            static_cast<unsigned long long>(w.slowJoinWorkerUs),
            static_cast<unsigned long long>(w.slowJoinSourceUs));
    }
};
FrameLoopMetrics frameLoopMetrics;
// Счётчики Board снимаются асинхронно; наблюдатель производителя/C0 не вызывается.
/**
 * @brief Печатает реальные Board audio-счётчики, mute/volume и поколение принятого кадра.
 * @param board Аппаратный адаптер Main; функция заимствует ссылку и не передаёт его Worker.
 * @param generation Идентичность текущей игровой сессии; старые пакеты этого значения не наследуют.
 * @param accepted Номер принятого кадра, которому соответствуют печатаемые счётчики.
 * @param playing Признак активного игрового исполнения, а не наличия сохранённого снимка.
 * @param muted Текущее пользовательское состояние mute для диагностической строки.
 */
[[gnu::noinline]] void frameAudioStatus(const ::nes::platform::Board&board,std::uint64_t generation,
                                      std::uint64_t accepted,bool playing,bool muted)noexcept{
    const auto audio=board.audioStats();
    std::printf(R"json(NES_FRAME_AUDIO_STATUS {"generation":%llu,"accepted_frame":%llu,"playing":%s,"muted":%s,"volume":%u,"active":%s,"accepted_frames":%lu,"dma_frames":%lu,"missed_refill_blocks":%lu,"missed_refill_frames":%lu,"max_write_gap_us":%lu,"max_write_wait_us":%lu,"source":"board_audio_stats","game_fps_claim":false})json" "\n",
        static_cast<unsigned long long>(generation),static_cast<unsigned long long>(accepted),playing?"true":"false",muted?"true":"false",board.volume(),audio.active?"true":"false",
        static_cast<unsigned long>(audio.acceptedFrames),static_cast<unsigned long>(audio.dmaFrames),static_cast<unsigned long>(audio.missedRefillBlocks),static_cast<unsigned long>(audio.missedRefillFrames),
        static_cast<unsigned long>(audio.maxWriteGapUs),static_cast<unsigned long>(audio.maxWriteWaitUs));
}
#define NES_LOOP_TAINT() frameLoopMetrics.taint(this);
#define NES_LOOP_PHASE(phaseName) frameLoopMetrics.phase(this,LoopPhase::phaseName);

/**
 * @brief Печатает изменившуюся маску ввода с кадровой идентичностью, не подменяя сами нажатия.
 * @param count Число элементов/байтов в переданном диапазоне.
 * @param generation Идентичность текущей игровой сессии; старые пакеты этого значения не наследуют.
 * @param frame Номер гостевого кадра/выход разбора команды, не время RTOS.
 * @param raw Исходная аппаратная маска кнопок до преобразований.
 * @param effective Итоговая маска ввода после remap/подавления/объединения источников.
 * @param serial Маска кнопок, введённых через UART.
 * @param touch Маска NES-кнопок от сенсорных элементов.
 * @param applied Маска scheduled-pad событий, реально применённых на этой кадровой границе.
 * @param ids Битовая маска идентификаторов активных сенсорных контактов.
 * @param valid Признак допустимости текущего снимка/события.
 * @param suppressed Признак подавления удержанного контакта при переходе экранов.
 * @param action Заимствованная метка действия в диагностике ввода.
 */
void traceInput(std::uint64_t generation,std::uint64_t frame,std::uint8_t raw,std::uint8_t effective,
                std::uint8_t serial,std::uint8_t touch,std::uint8_t applied,unsigned count,
                std::uint16_t ids,bool valid,bool suppressed,const char* action) noexcept {
    /** @brief Сохранённая идентичность предыдущей Main-итерации для проверки реального end-yield. */
    struct Previous final {std::uint64_t generation=~0ULL,value=0;const char* action=nullptr;};
    static Previous previous; // Только CPU0; условный layout класса/заголовка не вводится.
    const auto value=std::uint64_t(raw)|(std::uint64_t(effective)<<8)|(std::uint64_t(serial)<<16)
        |(std::uint64_t(touch)<<24)|(std::uint64_t(applied)<<32)|(std::uint64_t(count&7)<<40)
        |(std::uint64_t(valid)<<43)|(std::uint64_t(suppressed)<<44)|(std::uint64_t(ids)<<48);
    if(frame!=0&&previous.generation==generation&&previous.value==value&&previous.action&&
       !std::strcmp(previous.action,action))return;
    previous={generation,value,action};
    std::printf("NES_INPUT generation=%llu frame=%llu xbox_raw=%u xbox_effective=%u serial=%u touch=%u applied=%u contacts=%u ids=%04x poll_ok=%u suppressed=%u action=%s\n",
        static_cast<unsigned long long>(generation),static_cast<unsigned long long>(frame),unsigned(raw),unsigned(effective),
        unsigned(serial),unsigned(touch),unsigned(applied),count,unsigned(ids),unsigned(valid),unsigned(suppressed),action);
}
constexpr const char* tag = "nes_app";
/**
 * @brief Проверяет объявленную область видеотракта для mapper; допуск не означает полную совместимость игры.
 * @param mapper Номер mapper из проверенного заголовка ROM.
 */
constexpr bool videoMapperSupported(std::uint16_t mapper) noexcept {
    return core::Cartridge::supportsTimingVideoMapper(mapper);
}
constexpr const char* romDirectory = "/sdcard/nes_rom";
constexpr const char* settingsTabs[]{"SOUND","CONTROLLER","VIDEO","SYSTEM"};
constexpr unsigned settingsRows[]{2,3,2,4};
/**
 * @brief Возвращает постоянное имя региона для протокольной диагностики.
 * @param mode Режим региона из настроек.
 */
const char* regionName(RegionMode mode) noexcept {
    return mode==RegionMode::Ntsc?"NTSC 60":mode==RegionMode::Pal?"PAL 50":"AUTO";
}
/**
 * @brief Печатает экранированную JSON-строку, не интерпретируя имена ROM как управляющий UART-текст.
 * @param value Значение для безопасного протокольного вывода; владение остаётся у вызывающего.
 */
void jsonString(const char* value) noexcept {
    std::putchar('"');
    for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        if (*p == '"' || *p == '\\') { std::putchar('\\'); std::putchar(*p); }
        else if (*p < 32) std::printf("\\u%04x", unsigned(*p));
        else std::putchar(*p);
    }
    std::putchar('"');
}
/**
 * @brief Вычисляет диагностический отпечаток принятого PCM без изменения истории DSP.
 * @param count Число элементов/байтов в переданном диапазоне.
 * @param samples Заимствованный массив завершённого PCM16; число элементов передаётся отдельно.
 */
std::uint64_t pcmStateHash(const std::int16_t* samples, std::size_t count) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < count; ++i) {
        const auto sample = static_cast<std::uint16_t>(samples[i]);
        hash = (hash ^ static_cast<std::uint8_t>(sample)) * 1099511628211ULL;
        hash = (hash ^ static_cast<std::uint8_t>(sample >> 8)) * 1099511628211ULL;
    }
    return hash;
}
/**
 * @brief Вычисляет отпечаток принятого кадра для сопоставления контролей; хеш не заменяет побайтовую проверку.
 * @param machine Машина с разрешённым владельцем; этот помощник не делает доступ к занятой CPU1 безопасным.
 * @param available Выходной признак наличия корректной памяти изображения; отсутствие не считается чёрным кадром.
 */
std::uint64_t frameStateHash(const core::Machine& machine,bool* available=nullptr) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto* pixels = machine.framebuffer();
    if(available)*available=pixels!=nullptr;
    if(!pixels)return 0; // Недоступная память не является корректным чёрным кадром.
    for (unsigned i = 0; i < 256 * 240; ++i) hash = (hash ^ pixels[i]) * 1099511628211ULL;
    for (unsigned row = 0; row < 240; ++row) hash = (hash ^ machine.emphasis(row)) * 1099511628211ULL;
    return hash;
}
/**
 * @brief Разбирает ограниченное десятичное число и отказывает при переполнении/неверном формате.
 * @param text Заимствованный текст; у десятичного парсера ссылка продвигается только по разобранному входу.
 * @param maximum Максимальное допустимое разобранное число; переполнение означает отказ.
 * @param result Выход разбора/декодирования; результат считается действительным только при успешном статусе.
 */
bool parseDecimal(const char*& text, std::uint64_t maximum, std::uint64_t& result) noexcept {
    if (*text < '0' || *text > '9') return false;
    std::uint64_t value = 0;
    do {
        const unsigned digit = static_cast<unsigned>(*text - '0');
        if (value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
        ++text;
    } while (*text >= '0' && *text <= '9');
    result = value;
    return true;
}
/**
 * @brief Разбирает пару гостевой кадр/маска кнопок без частичного принятия ошибочной команды.
 * @param mask Маска кнопок/битов в формате вызывающего интерфейса.
 * @param text Заимствованный текст; у десятичного парсера ссылка продвигается только по разобранному входу.
 * @param frame Номер гостевого кадра/выход разбора команды, не время RTOS.
 */
bool parsePadArguments(const char* text, std::uint64_t& frame, std::uint8_t& mask) noexcept {
    while (*text == ' ') ++text;
    std::uint64_t parsedFrame = 0, parsedMask = 0;
    if (!parseDecimal(text, std::numeric_limits<std::uint64_t>::max(), parsedFrame) || *text != ' ') return false;
    while (*text == ' ') ++text;
    if (!parseDecimal(text, 255, parsedMask)) return false;
    while (*text == ' ') ++text;
    if (*text) return false;
    frame = parsedFrame;
    mask = static_cast<std::uint8_t>(parsedMask);
    return true;
}
/**
 * @brief Читает standard32bit cycle CSR CPU0 с compiler-only memory clobber по обе стороны scope.
 * @details Никакого hardware fence/ESPtimer. Delta требует менее одного32-bit wrap;
 * фактическая частота берётся из ROM clock API при печати, а не из профиля сборки.
 */
std::uint32_t c6PumpCycles()noexcept{std::uint32_t value;asm volatile("rdcycle %0":"=r"(value)::"memory");return value;}
/**
 * @brief Печатает реальные FreeRTOS task runtime/stack minima и состояние heap.
 */
void taskStats() noexcept {
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY
    TaskStatus_t tasks[32]{};
    configRUN_TIME_COUNTER_TYPE total = 0;
    const auto count = uxTaskGetSystemState(tasks, 32, &total);
    std::printf("NES_TASKS total_us=%llu count=%u\n", static_cast<unsigned long long>(total), unsigned(count)); std::printf("NES_HEAP largest_internal_dma=%u largest_internal=%u minimum_internal=%u\n", unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT)), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)), unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)));
    for (unsigned i = 0; i < count; ++i) {
        int core = -2;
#if configTASKLIST_INCLUDE_COREID
        core = static_cast<int>(tasks[i].xCoreID);
#endif
        std::printf("NES_TASK id=%u name=%s run_us=%llu priority=%u core=%d stack_min=%u\n",
            unsigned(tasks[i].xTaskNumber), tasks[i].pcTaskName,
            static_cast<unsigned long long>(tasks[i].ulRunTimeCounter),
            unsigned(tasks[i].uxCurrentPriority), core, unsigned(tasks[i].usStackHighWaterMark));
    }
#else
    std::printf("NES_TASKS disabled\n");
#endif
}
/**
 * @brief Рисует ограниченный прямоугольник интерфейса в предоставленном framebuffer.
 * @param x Логическая координата по горизонтали, в пикселях.
 * @param y Логическая координата по вертикали, в пикселях.
 * @param buffer Заимствованный RGB565 framebuffer Main, в который разрешена запись.
 * @param w Ширина прямоугольника в логических пикселях.
 * @param h Высота прямоугольника в логических пикселях.
 * @param color Цвет заливки RGB565.
 */
void rectangle(std::uint16_t* buffer, int x, int y, int w, int h, std::uint16_t color) noexcept {
    if (!buffer) return;
    const int right = std::min(800, x+w), bottom = std::min(480, y+h);
    for (int row = std::max(0, y); row < bottom; ++row)
        std::fill(buffer + row*800 + std::max(0,x), buffer + row*800 + right, color);
}

}

/** @brief Долгоживущий INTERNAL control задания CPU1. Семафоры публикуют результат; поля Main и Worker нельзя читать поперёк неподтверждённой границы. */
struct NesApp::EmulationWorkerState final {
    // Только Main: policy не публикуется CPU1 и не меняет NesApp/Core header layout.
    coupled_idle::MainIdleBudget coupledMainIdle{};
    // Только Main: I2C-снимок живёт между prefetch, join и frame; Worker его не читает.
    std::optional<detail::TouchPrefetch> touchPrefetch{};
    std::uint64_t touchPrefetchGeneration=0;
    /** @brief Кадровый снимок CPU/RAM/PCM/индексов и идентичности задания. Main использует опубликованную копию, а не занятую Machine. */
    struct Snapshot final {
        std::uint8_t pixels[256 * 240]{};
        std::uint8_t emphasis[240]{};
        std::int16_t pcm[core::Apu::sampleCapacity]{};
        std::size_t sampleCount = 0;
        core::Region region = core::Region::Ntsc;
        std::uint64_t generation = 0, completedFrames = 0;
        std::uint64_t audioGeneration = 0, audioSequence = 0, ppuFrame = 0, cpuCycles = 0;
        std::uint64_t videoGeneration=0,videoSequence=0,videoFrame=0,videoCycles=0;
        std::uint8_t inputPad = 0;
        bool ready = false;
    };
    CoupledVideoPipeline* coupled=nullptr;
    c6_async::MainPumpCost c6PumpCost{}; // Только Main, отдельные128B; allocator соблюдает alignof WorkerState.
    audio::PcmStreamGuard c6PcmStream{}; // Обязательные full-key/count/phase checks до первой I2S выдачи.
    c6audio::async::AudioPublished c6Published{}; // HP INTERNAL control; Worker меняет только release-публикацию.
    c6_async::MainOwner*c6Audio=nullptr; // Только Main, boot-long PSRAM.
    std::uint64_t c6DispatchedInput=0;bool c6Used=false;
    CoupledVideoPipeline::Times coupledTimes{};
    std::uint64_t coupledPumpUs=0,coupledCopyUs=0;
    // Только Main после actual done: totals текущего accepted окна; Worker их не читает.
    std::uint64_t coupledDoneLoops=0,coupledDonePumps=0,coupledDoneAttempts=0,coupledDoneForced=0,coupledDoneTokens=0;
    bool coupledDoneOverflow=false;
    // RGB/native lease Board принадлежит Main; Worker/CPU1 его не читает.
    std::uint64_t presentGeneration=0,presentFrame=0;
    bool presentPending=false,presentFailed=false;
    platform::GameSourceTicket presentSource{}; // Только Main: фактический источник прошлого PPA.
    FrameVideoPipeline* framePipeline=nullptr;
    c6_async::PostJoinFrameAudio<FrameVideoPipeline> frameC6Audio{}; // Единственный original audio loan owner.
    c6_async::FrameFmBridge<FrameVideoPipeline>*frameFm=nullptr; // Main-owned PSRAM, только actualFM85 cold opt-in.
    FrameRamPolicy ramWitnessPolicy=FrameRamPolicy::Sparse120; // Main: только joined Library; не NVS.
    FrameVideoPipeline::Times pipelineTimes{}; // Только Main, завершённые слоты.
    std::uint64_t pipelineCopyUs=0;
    // Lease Board принадлежит Main; Worker не наблюдает эти счётчики фаз.
    std::uint64_t pipelinePresentGeneration=0,pipelinePresentFrame=0;
    std::uint64_t pipelinePresentStarted=0,pipelinePresentCompleted=0;
    std::uint64_t pipelineCompletedGeneration=0,pipelineCompletedFrame=0;
    bool pipelinePresentPending=false,pipelinePresentFailed=false;
    NesApp* app = nullptr;
    Snapshot* snapshot = nullptr;
    StaticSemaphore_t startStorage{}, doneStorage{};
    SemaphoreHandle_t start = nullptr, done = nullptr;
    TaskHandle_t task = nullptr;
    // Только result/coreUs пишет Worker; публикация идёт через done.
    std::uint8_t inputPad = 0;
    core::RunStatus result = core::RunStatus::NoCartridge;
    std::int64_t coreUs = 0;
    std::int64_t inputUs = 0; // Длительность ввода этого задания принадлежит Main.
    std::uint64_t inputFrame = 0, generation = 0;
    bool inFlight = false; // Принадлежит Main; тайм-аут не обнуляет заимствование.
    // Выбирается Main только после join по capability mapper; неизменяем на всё задание CPU1.
    bool legacyMachine = false;
    core::VideoPacket* videoStorage[2]{};
    core::VideoPacket* spareVideo=nullptr;
    core::VideoPacket* retiredVideo=nullptr;
    core::VideoRenderer* videoRenderer=nullptr;
    core::VideoStorage* videoShadow=nullptr; // Заимствуется Renderer; удерживается и при терминальном тайм-ауте.
    bool videoBound=false,videoPending=false,videoForSnapshot=false,videoRgbReady=false;
    bool videoFaultPending=false;
    std::int64_t audioDspUs = 0; // Время replay/copy принадлежит CPU0; Worker его не пишет.
    core::AudioPacket* activeAudio = nullptr;
    core::AudioPacket* retiredAudio = nullptr;
    core::AudioRenderer* audioRenderer = nullptr;
    // Стабильный caller-owned token; lifetime продолжается до true join/reset.
    core::AudioStageToken preAckAudioToken{};
    bool audioSplit = false, audioPacketPending = false, packetForSnapshot = false;
    bool audioFaultPending = false;
};


#include "runtime/coupled/AsyncPresent.inl"

#include "runtime/app/VideoSetup.inl"
#include "runtime/app/FrameVideo.inl"
#include "runtime/app/C6Audio.inl"
#include "runtime/app/CoupledVideo.inl"
#include "runtime/app/Worker.inl"
#include "runtime/app/Session.inl"
#include "runtime/app/Ui.inl"
#include "runtime/app/FrameDispatch.inl"
#include "runtime/app/Library.inl"
#include "runtime/app/Telemetry.inl"
#include "runtime/app/Console.inl"
/**
 * @brief Инициализирует плату, настройки, SD и Worker, затем обслуживает основной цикл. Вывод, меню и команды выполняются Main; ошибки владельца переводят выполнение в Faulted.
 */
void NesApp::run() noexcept {
    NES_INITIALIZE_BUFFERED_UART();
    // Политики выбираются до запуска Worker. Команды Library могут менять их между играми.
    frameDirectPresent.requested=true;
    CoupledVideoPipeline::setIndexedMemoryProbe(true);
    machine_.selectMmc5InactiveGrant(true);
    machine_.selectMmc5ZeroPoll(true);
#if NES_APP_CPU0_VRC7
    std::printf("NES_EXPERIMENTAL_CPU0_VRC7 enabled=1 scope=frame_m85 pipeline=postdispatch wire=unchanged\n");
#endif
#if defined(NES_APP_OAM_TERMINAL_MASK) && NES_APP_OAM_TERMINAL_MASK
    std::printf("NES_EXPERIMENTAL_OAM_TERMINAL_MASK schema=1 enabled=1 scope=first_to256 lt8_y_candidates=1 partial=scalar overflow=scalar guest_clock=same default=off\n");
#endif
#if defined(NES_APP_GENERIC_AFFINE_QUIET) && NES_APP_GENERIC_AFFINE_QUIET
    std::printf("NES_EXPERIMENTAL_GENERIC_AFFINE_QUIET schema=1 enabled=1 scope=nonbanked_quiet grammar=incdec_clc_lda_adc_sta_jmp horizon=existing RAM=internal2k guest_clock=same default=off\n");
#endif
#if defined(NES_EXPERIMENTAL_FRAME_COLOUR_PUMP) && NES_EXPERIMENTAL_FRAME_COLOUR_PUMP
    std::printf("NES_EXPERIMENTAL_FRAME_COLOUR_PUMP enabled=1 cadence_ops=64 cadence_rows=8 owner=main deferred_fault=1 default=0\n");
#endif
#if defined(NES_EXPERIMENTAL_BATCH_FETCH46) && NES_EXPERIMENTAL_BATCH_FETCH46
    std::printf("NES_EXPERIMENTAL_BATCH_FETCH46 enabled=1 backend=batch legacy43=unchanged max_fetch_samples=832 default=0\n");
#endif
    for (unsigned e=0;e<8;++e) for(unsigned c=0;c<64;++c)
        palette_[e][c]=core::displayRgb565(static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(e));
    esp_chip_info_t chip{};
    esp_chip_info(&chip);

    ESP_LOGI(tag, "Own NES runtime; ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(tag, "Chip: cores=%u revision=%u", static_cast<unsigned>(chip.cores),
             static_cast<unsigned>(chip.revision));
    const auto& ready = board_.initialize();
    if(!settingsStore_.load(settings_))ESP_LOGW(tag,"SETTINGS_LOAD_DEFAULTS %s",settingsStore_.errorText());
    static_cast<void>(applyAudioSettings());
    static_cast<void>(library_.open(romDirectory));
    ESP_LOGI(tag,"NES_LIBRARY source=sd root=%s games=%u skipped=%u error=%s",romDirectory,
             unsigned(library_.count()),unsigned(library_.skipped()),library_.errorText());
    if (!uart_is_driver_installed(UART_NUM_0))
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0,2048,0,0,nullptr,0));
    static_cast<void>(gamepad_.initialize());
    static_cast<void>(initializeWorker());
    static_cast<void>(initializeSplitAudio());
    static_cast<void>(initializeSplitVideo());
    ESP_LOGI(tag,"NES_RUNTIME_READY games=%u psram=%d lcd=%d touch=%d audio=%d sd=%d",
             unsigned(library_.count()),ready.psram,ready.display,ready.touch,ready.audio,ready.storage);
    status();
    std::int64_t lastMenu=0;
    bool previousConnection=false;
    frameLoopMetrics.bind(this);
    for (;;) {
        FrameLoopMetrics::flushReportIfIdle();
        if(playing_&&executionMode_==ExecutionMode::Worker&&worker_&&worker_->framePipeline){
            frameLoopMetrics.begin(this);
            auto&v=frameLoopMetrics.before;
            v.worker=worker_;v.context=worker_->framePipeline;v.generation=sessionGeneration_;
            v.accepted=frames_;v.submitted=submittedFrames_;
            v.started=worker_->pipelinePresentStarted;
        }else frameLoopMetrics.cancel(this);
        // Снять I2C до ожидания CPU1; команды после join могут заменить поколение.
        // Хранение принадлежит Main, а frame проверяет поколение и возраст снимка.
        NES_LOOP_PHASE(Input)
        if(worker_){
            const bool eligible=playing_&&executionMode_==ExecutionMode::Worker&&worker_->inFlight;
            worker_->touchPrefetch.emplace(board_,eligible);
            worker_->touchPrefetchGeneration=sessionGeneration_;
        }
        NES_LOOP_PHASE(Join)
        if (!retireFrame()) {
            frameLoopMetrics.cancel(this);
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        NES_LOOP_PHASE(Cli)
        serialCommands();
        if (gamepad_.menuRequested()) {
            if(screen_==Screen::Settings)closeSettings();
            else if(screen_==Screen::Pause)pauseControls(core::B);
            else if(playing_)openPause();
            else openSettings();
        }
        if (playing_) {
            NES_LOOP_PHASE(Other)
            frame();
            // Текущее владение Main сравнивается ДО доступа к текущей памяти.
            // Сохранённые identity Worker/context непрозрачны и не разыменовываются.
            if(frameLoopMetrics.activeFor(this)){
                if(!frameLoopMetrics.dirty&&playing_&&executionMode_==ExecutionMode::Worker&&
                   sessionGeneration_==frameLoopMetrics.before.generation&&worker_&&
                   worker_==frameLoopMetrics.before.worker&&worker_->framePipeline&&
                   worker_->framePipeline==frameLoopMetrics.before.context&&worker_->snapshot){
                    auto&v=frameLoopMetrics.after;
                    v.worker=worker_;v.context=worker_->framePipeline;v.generation=sessionGeneration_;
                    v.accepted=frames_;v.submitted=submittedFrames_;
                    v.snapshotGeneration=worker_->snapshot->generation;
                    v.snapshotFrame=worker_->snapshot->completedFrames;
                    v.dispatchedInput=worker_->inputFrame;
                    v.started=worker_->pipelinePresentStarted;v.pending=worker_->pipelinePresentPending;
                    v.pendingGeneration=worker_->pipelinePresentGeneration;v.pendingFrame=worker_->pipelinePresentFrame;
                    frameLoopMetrics.finish(this); // frame вернулся ПОСЛЕ настоящего end-yield.
                }else frameLoopMetrics.cancel(this);
            }
            continue;
        }
        frameLoopMetrics.cancel(this);
        const auto now=esp_timer_get_time();
        const auto buttons=gamepad_.buttons();
        auto pressed=static_cast<std::uint8_t>(buttons & ~previousButtons_);
        previousButtons_=buttons;
        auto repeatMask=static_cast<std::uint8_t>(core::Up|core::Down);
        if(screen_==Screen::Library || (settingsTab_==0 && settingsRow_==0))repeatMask|=core::Left|core::Right;
        const auto held=static_cast<std::uint8_t>(buttons&repeatMask);
        if(held!=repeatButtons_){repeatButtons_=held;repeatAtUs_=now+350000;}
        else if(held && now>=repeatAtUs_){pressed|=held;repeatAtUs_=now+80000;}
        if(screen_==Screen::Pause){
            pauseControls(pressed);
            if(screen_==Screen::Pause&&menuDirty_)pauseMenu();
            vTaskDelay(pdMS_TO_TICKS(10));continue;
        }
        if(screen_==Screen::Settings) {
            settingsControls(pressed);
            if(screen_==Screen::Settings && (menuDirty_ || now-lastMenu>2000000)){settingsMenu();lastMenu=now;}
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if(pressed & core::B){openSettings();continue;}
        if (pressed & core::Select) { rescanLibrary(); continue; }
        if (library_.count()) {
            if (pressed & core::Down) { selected_=(selected_+1)%library_.count(); menuDirty_=true; }
            if (pressed & core::Up) { selected_=(selected_+library_.count()-1)%library_.count(); menuDirty_=true; }
            if (pressed & core::Left) { selected_=menuLayout::previous(selected_); menuDirty_=true; }
            if (pressed & core::Right) { selected_=menuLayout::next(selected_,library_.count()); menuDirty_=true; }
            if (pressed & (core::A | core::Start)) { static_cast<void>(startGame(selected_)); continue; }
        }
        platform::TouchPoint point{};
        if (board_.pollTouch(point)) {
            const bool tapped = point.pressed && !touchHeld_;
            touchHeld_=point.pressed;
            if (tapped && point.x>=640 && point.y>=390 && point.y<430) {
                rescanLibrary(); continue;
            }
            if (tapped && point.y>=10 && point.y<65) {
                if(point.x>=440 && point.x<540){openSettings();continue;}
                if (point.x>=560 && point.x<660) { selected_=menuLayout::previous(selected_); menuDirty_=true; }
                if (point.x>=680 && point.x<780) { selected_=menuLayout::next(selected_,library_.count()); menuDirty_=true; }
            }
            if (tapped) {
                const auto i=menuLayout::hit(selected_,library_.count(),point.x,point.y);
                if(i!=menuLayout::noItem){ selected_=i; static_cast<void>(startGame(i)); }
            }
        }
        if (previousConnection!=gamepad_.connected()) { previousConnection=gamepad_.connected(); menuDirty_=true; }
        if (menuDirty_ || now-lastMenu>2000000) { menu(); lastMenu=now; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Возвращает адаптер выделения Legacy-пикселей в PSRAM. Функции освобождения применяются только к собственной, уже не заимствованной памяти.
 */
core::LegacyPixelAllocator NesApp::legacyPixelAllocator() noexcept {
    return {+/** @brief Выделяет только ожидаемый Legacy-пиксельный размер в PSRAM; неверный размер/OOM возвращает nullptr. */
             [](std::size_t bytes) noexcept -> std::uint8_t* {
        if(bytes!=core::Ppu::width*core::Ppu::height)return nullptr;
        return static_cast<std::uint8_t*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    },+/** @brief Освобождает собственный уже отсоединённый Legacy-буфер; вызывающий подтверждает завершение пользователей. */
       [](std::uint8_t* data) noexcept {heap_caps_free(data);}};
}
/**
 * @brief Фиксирует ошибку Legacy-цветного тракта и прекращает небезопасное продолжение. Заимствованные данные освобождаются только после установленного завершения.
 * @param reason Заимствованное диагностическое описание отказа.
 */
bool NesApp::legacyVideoFailure(const char* reason) noexcept {
    if(worker_){workerFault(reason);return false;}
    // При синхронном запуске нет владельца CPU1 для join/карантина.
    playing_=pausedGame_=false;board_.setAudioActive(false);menuDirty_=false;
    ESP_LOGE(tag,"NES_RUNTIME_FAIL reason=%s execution=synchronous",reason);
    board_.clear(0x0841);board_.drawText(24,100,"VIDEO MEMORY UNAVAILABLE",0xf800,2);
    static_cast<void>(board_.present());return false;
}
}  // namespace nes::app



