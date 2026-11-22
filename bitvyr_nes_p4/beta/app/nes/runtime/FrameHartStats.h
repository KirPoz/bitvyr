#pragma once
#include <array>
#include <cstdint>
#include <limits>
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if !defined(CONFIG_IDF_TARGET_ESP32P4) || !defined(__riscv) || __riscv_xlen != 32
#error "FRAME_HART_STATS supports only the ESP32-P4 RV32 adapter"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rv_utils.h"
#endif

namespace nes::app::debug {

/** @brief Качество диагностической выборки; отказ не меняет состояние эмулятора. */
enum class HartStatus : std::uint8_t {
    Empty, Recording, Ok, Unsupported, WrongCore, Inhibited, Unstable,
    ContextChanged, NonMonotone, NoRetired, Arithmetic, Identity, Count
};

/** @brief Выбор пары64-bit CSR; HPM и LP CPU здесь не используются. */
enum class HartCounter : std::uint8_t {Cycles, Retired};

/** @brief Одна граница HP hart: cycles/retired и идентичность выполняющего ядра. */
struct HartStamp final {
    std::uint64_t cycles=0,retired=0;
    std::uint32_t rawHart=0,inhibit=0;
    std::int32_t logicalCore=-1;
};

/**
 * @brief Привязка измерения к существующему immutable Output ticket.
 * @details Указатели — только числовые идентификаторы, не разрешение чтения ROM/Machine.
 */
struct HartIdentity final {
    std::uintptr_t producer=0,rom=0;
    std::uint64_t romBytes=0,appGeneration=0,leaseGeneration=0,romEpoch=0;
    std::uint64_t transportEpoch=0,sequence=0,baseCycle=0;
    std::uint32_t slot=0;
};

/**
 * @brief Worker-owned снимки одного stepFrame, публикуемые прежним sealed Payload.
 * @details begin/end относятся только к stepFrame; исходный tail barrier не входит.
 * После seal Worker не меняет этот объект до освобождения соответствующего slot.
 */
struct HartSample final {
    HartStamp begin{},end{};
    HartIdentity identity{};
    std::uint64_t cycles=0,retired=0;
    HartStatus status=HartStatus::Empty;
};

/**
 * @brief Read-only адаптер P4 CSR и штатной логической нумерации FreeRTOS.
 * @details Не пишет CSR, не включает counters, не маскирует IRQ и не создаёт fence.
 * В host-сборке недоступен: нули не выдаются за аппаратную выборку.
 */
struct HartHardware final {
    /** @brief Разрешает аппаратные чтения только внутри настоящего ESP32-P4 SDK target. */
    static constexpr bool available()noexcept {
#if defined(ESP_PLATFORM)
        return true;
#else
        return false;
#endif
    }
    /**
     * @brief Возвращает логический номер текущего ядра через штатный FreeRTOS API.
     * @details Не заменяет SDK mapping самодельной маской raw mhartid.
     */
    static std::int32_t logicalCore()noexcept {
#if defined(ESP_PLATFORM)
        return xPortGetCoreID();
#else
        return -1;
#endif
    }
    /** @brief Читает raw mhartid для сравнения границ; значение не сравнивается с литералом1. */
    static std::uint32_t rawHart()noexcept {
#if defined(ESP_PLATFORM)
        return RV_READ_CSR(mhartid);
#else
        return 0;
#endif
    }
    /** @brief Читает mcountinhibit, не меняя CY/IR и чужие HPM controls. */
    static std::uint32_t inhibit()noexcept {
#if defined(ESP_PLATFORM)
        return RV_READ_CSR(mcountinhibit);
#else
        return 0;
#endif
    }
    /** @brief Читает старшую половину выбранного HP CSR, без изменения счётчика. @param c Cycles либо Retired. */
    static std::uint32_t high(HartCounter c)noexcept {
#if defined(ESP_PLATFORM)
        if(c==HartCounter::Cycles)return RV_READ_CSR(mcycleh);
        return RV_READ_CSR(minstreth);
#else
        static_cast<void>(c);return 0;
#endif
    }
    /** @brief Читает младшую половину выбранного HP CSR. @param c Cycles либо Retired. */
    static std::uint32_t low(HartCounter c)noexcept {
#if defined(ESP_PLATFORM)
        if(c==HartCounter::Cycles)return RV_READ_CSR(mcycle);
        return RV_READ_CSR(minstret);
#else
        static_cast<void>(c);return 0;
#endif
    }
    /**
     * @brief Ограничивает только перестановку обращений компилятором.
     * @details Пустой GNU asm с memory clobber не выполняет аппаратный fence/flush.
     */
    static void compilerBarrier()noexcept {
#if defined(__GNUC__)
        asm volatile("" ::: "memory");
#endif
    }
};

/**
 * @brief Конечный read-only протокол снимков, параметризованный CSR transport.
 * @tparam Access Аппаратный адаптер либо явный fake будущего host protocol-теста.
 */
template<class Access> struct HartProbe final {
    /**
     * @brief Получает устойчивую пару high/low/high не более чем за3 попытки.
     * @param c Выбранный64-bit HP counter.
     * @param out Результат в тиках/инструкциях; изменяется только при устойчивой паре.
     * @return false при трёх несовпадениях high; бесконечного retry нет.
     */
    static bool read64(HartCounter c,std::uint64_t&out)noexcept {
        for(unsigned attempt=0;attempt<3;++attempt){
            const auto a=Access::high(c);const auto lo=Access::low(c);const auto b=Access::high(c);
            if(a==b){out=(std::uint64_t(a)<<32)|lo;return true;}
        }
        return false;
    }
    /**
     * @brief Снимает контекст перед stepFrame и проверяет уже включённые CY/IR.
     * @param s Payload текущего job; identity уже заполнена из его существующего ticket.
     * @details Ошибка остаётся диагностической, сам stepFrame всё равно вызывается один раз.
     */
    static void begin(HartSample&s)noexcept {
        s.status=HartStatus::Unsupported;
        if(!Access::available())return;
        auto&v=s.begin;v.logicalCore=Access::logicalCore();v.rawHart=Access::rawHart();v.inhibit=Access::inhibit();
        if(v.logicalCore!=1){s.status=HartStatus::WrongCore;return;}
        if(v.inhibit&5U){s.status=HartStatus::Inhibited;return;}
        if(!read64(HartCounter::Cycles,v.cycles)||!read64(HartCounter::Retired,v.retired)){
            s.status=HartStatus::Unstable;return;
        }
        s.status=HartStatus::Recording;Access::compilerBarrier();
    }
    /**
     * @brief Завершает снимок сразу после stepFrame, до существующего tail barrier.
     * @param s Тот же Payload, без публикации Main до обычного seal.
     * @details Сравнивает raw/logical hart и полный inhibit. Сброс/64-bit wrap,
     * нулевой retired и переполнение CPI-Q10 дают invalid; счётчики не исправляются.
     */
    static void end(HartSample&s)noexcept {
        if(s.status!=HartStatus::Recording)return;
        Access::compilerBarrier();auto&v=s.end;
        if(!read64(HartCounter::Retired,v.retired)||!read64(HartCounter::Cycles,v.cycles)){
            s.status=HartStatus::Unstable;return;
        }
        v.inhibit=Access::inhibit();v.rawHart=Access::rawHart();v.logicalCore=Access::logicalCore();
        if(v.logicalCore!=1||v.logicalCore!=s.begin.logicalCore||v.rawHart!=s.begin.rawHart||v.inhibit!=s.begin.inhibit){
            s.status=HartStatus::ContextChanged;return;
        }
        if(v.inhibit&5U){s.status=HartStatus::Inhibited;return;}
        if(v.cycles<s.begin.cycles||v.retired<s.begin.retired){s.status=HartStatus::NonMonotone;return;}
        s.cycles=v.cycles-s.begin.cycles;s.retired=v.retired-s.begin.retired;
        if(!s.retired){s.status=HartStatus::NoRetired;return;}
        if(s.cycles>(std::numeric_limits<std::uint64_t>::max()>>10)){
            s.status=HartStatus::Arithmetic;return;
        }
        s.status=HartStatus::Ok;
    }
};

/**
 * @brief Только Main-owned bounded агрегат принятых кадров; не наблюдает Worker.
 * @details CPI bins Q10: <512,<768,<1024,<1280,<1536,<2048,<3072,остальное.
 * При переполнении/разрыве последовательности sticky overflow запрещает дальнейшее
 * накопление до явного Main reset. Нет массива кадров, heap или деления float.
 */
struct HartWindow final {
    std::uint64_t samples=0,valid=0,invalid=0,generation=0,first=0,last=0;
    std::uint64_t cycles=0,retired=0,guestFrameCycles=0;
    std::uint64_t cycleMin=0,cycleMax=0,retiredMin=0,retiredMax=0;
    std::array<std::uint64_t,8> cpiBins{};
    std::array<std::uint64_t,static_cast<unsigned>(HartStatus::Count)> statusCounts{};
    bool overflow=false;

    /**
     * @brief Добавляет только уже принятый Main кадр из предварительно скопированного sample.
     * @param s Main-owned копия, взятая до Free/reuse исходного slot.
     * @param guestCycles Разность существующего sealed cut; не длительность одного stepFrame.
     * @details Все суммы проверяются до изменения. Отказ диагностики не отменяет принятую NES-картинку.
     */
    void add(const HartSample&s,std::uint64_t guestCycles)noexcept {
        constexpr auto max=std::numeric_limits<std::uint64_t>::max();
        if(overflow)return;
        const auto index=static_cast<unsigned>(s.status);
        if(index>=statusCounts.size()||samples==max||statusCounts[index]==max||
           (samples&&(generation!=s.identity.appGeneration||last==max||s.identity.sequence!=last+1))){overflow=true;return;}
        const bool good=s.status==HartStatus::Ok;
        unsigned bin=0;
        if(good){
            if(!s.retired||s.cycles>(max>>10)){overflow=true;return;}
            const auto q=(s.cycles<<10)/s.retired;
            constexpr std::uint32_t limits[]{512,768,1024,1280,1536,2048,3072};
            while(bin<7&&q>=limits[bin])++bin;
            if(valid==max||cycles>max-s.cycles||retired>max-s.retired||
               guestFrameCycles>max-guestCycles||cpiBins[bin]==max){overflow=true;return;}
        }else if(invalid==max){overflow=true;return;}
        if(!samples){generation=s.identity.appGeneration;first=s.identity.sequence;}
        last=s.identity.sequence;++samples;++statusCounts[index];
        if(!good){++invalid;return;}
        if(!valid){cycleMin=cycleMax=s.cycles;retiredMin=retiredMax=s.retired;}
        if(s.cycles<cycleMin)cycleMin=s.cycles;
        if(s.cycles>cycleMax)cycleMax=s.cycles;
        if(s.retired<retiredMin)retiredMin=s.retired;
        if(s.retired>retiredMax)retiredMax=s.retired;
        ++valid;cycles+=s.cycles;retired+=s.retired;guestFrameCycles+=guestCycles;++cpiBins[bin];
    }
};
}
