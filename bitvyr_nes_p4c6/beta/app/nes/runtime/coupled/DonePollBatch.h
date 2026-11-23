/** @file DonePollBatch.h @brief Main-only ограниченная периодичность проверок настоящего done-token. */
#pragma once
#include <cstdint>
#include <limits>

namespace nes::app::coupled_done {
/** @brief Результат ожидания; Ready означает только успешный tryDone, не matching identity или join. */
enum class WaitResult : std::uint8_t { Ready, Fault };

/** @brief Итог одного шага цветовой очереди CPU0. */
enum class VideoResult : std::uint8_t { Processed, Empty, Fault };

/**
 * @brief Обслуживает аудио при пустой очереди или после четырёх подряд обработанных видеозапросов.
 * @details Main обрабатывает не больше одного video callback за вызов. Внешний цикл проверяет
 * семафор и deadline после каждого callback. Уже ожидающие префиксы сгруппированы без
 * изменения их FIFO-порядка. Пустая очередь сразу возвращает ход C6 и I2S, включая паузы
 * между короткими сериями. Ошибка видео прерывает вызов до следующего обращения к аудио.
 */
class CoupledPump final {
public:
    static constexpr unsigned maximumVideoBurst=4;

    /** @brief Передаёт ход аудиоканалу в пределах максимальной серии. */
    template<class PumpAudio>
    bool afterVideo(VideoResult result,PumpAudio&& audio) noexcept {
        if(result==VideoResult::Fault)return false;
        if(result==VideoResult::Processed&&++videoBurst_<maximumVideoBurst)return true;
        videoBurst_=0;
        return audio();
    }
private:
    unsigned videoBurst_=0;
};

/**
 * @brief Main-owned числовая статистика одного ожидания без владения ресурсами.
 * @details loops/pumpCalls считают начатые pump; semAttempts включает initial,
 * scheduled и forced probes. forcedAttempts является подмножеством semAttempts.
 * tokens считает только true от tryDone. При насыщении overflow липкий, поведение
 * ожидания не меняется. Новый объект разрешён на следующем job после настоящего
 * token/проверки владельца; Fault сам по себе не разрешает освобождение ресурсов.
 */
struct WaitStats final {
    std::uint32_t loops=0, pumpCalls=0, semAttempts=0, forcedAttempts=0, tokens=0;
    bool overflow=false;
    /** @brief Увеличивает числовое поле без wrap; насыщение лишь диагностическое. */
    void increment(std::uint32_t& value) noexcept {
        if(value==std::numeric_limits<std::uint32_t>::max())overflow=true;
        else ++value;
    }
};

/**
 * @brief Ожидает реальный done-token с ограниченным числом healthy pump между probes.
 * @tparam Batch Число healthy pump между scheduled probes:1/2/4/8/16/32/64.
 * @param stats Заимствованная Main-only статистика текущего ожидания, не разделяемая с Worker.
 * @param tryDone Неблокирующий адаптер настоящего semaphore: только его true даёт Ready.
 * @param pump Адаптер actual consumer; false обозначает fault, но не завершение Worker.
 * @param expired Проверка deadline после каждого healthy pump; true не доказывает join.
 * @param pause Неблокирующий адаптер signal_fence/yield после незавершённой итерации.
 * @return Ready после true tryDone; Fault лишь после обязательного последнего false tryDone.
 * @details Первичный probe всегда выполняется до pump. При pump fault/timeout
 * выполняется ровно один forced probe: одновременно прибывший token выигрывает
 * у Fault. Нет чтения generation/finished/CPU или освобождения ROM/контекста.
 * После Ready вызывающий всё ещё обязан проверить identity и настоящий completion;
 * после Fault сохранить все заимствования до true join/reset. Все callbacks
 * заимствованы на длительность вызова и не должны бросать исключения/reenter.
 */
template<unsigned Batch=1,class TryDone,class Pump,class Expired,class Pause>
inline WaitResult waitDone(WaitStats& stats,TryDone&& tryDone,Pump&& pump,
                           Expired&& expired,Pause&& pause) noexcept {
    static_assert(Batch>=1&&Batch<=64&&(Batch&(Batch-1U))==0,
                  "done-poll Batch must be 1/2/4/8/16/32/64");
    /** @brief Учитывает каждый actual probe; forced не заменяет semaphore вызов. */
    const auto probe=[&](bool forced)noexcept{
        stats.increment(stats.semAttempts);
        if(forced)stats.increment(stats.forcedAttempts);
        const bool ready=tryDone();
        if(ready)stats.increment(stats.tokens);
        return ready;
    };
    if(probe(false))return WaitResult::Ready;
    unsigned remaining=Batch;
    for(;;){
        stats.increment(stats.loops);
        stats.increment(stats.pumpCalls);
        const bool healthy=pump();
        if(!healthy||expired())return probe(true)?WaitResult::Ready:WaitResult::Fault;
        if(--remaining==0){
            remaining=Batch;
            if(probe(false))return WaitResult::Ready;
        }
        pause();
    }
}
} // namespace nes::app::coupled_done
