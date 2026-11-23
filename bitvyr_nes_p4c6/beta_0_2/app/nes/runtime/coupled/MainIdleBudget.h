#pragma once
#include <cstdint>

namespace nes::app::coupled_idle {
/**
 * @brief Main-owned50мс бюджет добровольного CPU0 idle opportunity только Coupled MMC5.
 * @details Не кормит TWDT вручную, не меняет guest-time и не выделяет память.
 * Clock — модульные U64 микросекунды; idleCounter — наблюдаемый U32 runtime
 * именно IDLE0. Прогресс учитывается с предыдущего sample, не с now:
 * ранний idle внутри длинного frame не отодвигает fallback ещё на целый frame.
 */
class MainIdleBudget final {
public:
    static constexpr std::uint64_t maximumUncreditedUs=50000;
    /** @brief Инвалидирует накопленный credit при паузе/ошибке/смене режима; ресурсами не владеет. */
    void reset() noexcept {
        creditedUs_=sampleUs_=generation_=0;idleCounter_=0;armed_=false;
    }
    /**
     * @brief Решает на границе кадра, нужен ли настоящий vTaskDelay(1).
     * @param nowUs U64 host microseconds; unsigned subtraction определена при rollover.
     * @param idleCounter Счётчик времени IDLE0; используется лишь неравенство, включая wrap.
     * @param generation Текущая ненулевая App session; старый credit не переносится.
     * @param active Разрешённый joined Coupled MMC5; false требует baseline delay и reset.
     * @return true требует delay; первый кадр и неизвестный credit всегда дают true.
     */
    bool shouldDelay(std::uint64_t nowUs,std::uint32_t idleCounter,
                     std::uint64_t generation,bool active) noexcept {
        if(!active||!generation){reset();return true;}
        if(!armed_||generation_!=generation){
            generation_=generation;sampleUs_=nowUs;idleCounter_=idleCounter;
            creditedUs_=nowUs-maximumUncreditedUs;armed_=true;return true;
        }
        observe(nowUs,idleCounter);
        return nowUs-creditedUs_>=maximumUncreditedUs;
    }
    /**
     * @brief Наблюдает результат уже выполненного delay, не выдавая request за IDLE progress.
     * @param nowUs Host microseconds после фактического возврата vTaskDelay.
     * @param idleCounter Повторный runtime IDLE0; неизменный counter не создаёт credit.
     * @param generation Та же App session; несовпадение инвалидирует политику.
     * @details Без реального counter progress следующий frame снова потребует delay.
     */
    void afterDelay(std::uint64_t nowUs,std::uint32_t idleCounter,
                    std::uint64_t generation) noexcept {
        if(!armed_||!generation||generation_!=generation){reset();return;}
        observe(nowUs,idleCounter);
    }
private:
    /** @brief При changed counter нижняя временная граница idle — предыдущий sample. */
    void observe(std::uint64_t nowUs,std::uint32_t idleCounter) noexcept {
        if(idleCounter!=idleCounter_){creditedUs_=sampleUs_;idleCounter_=idleCounter;}
        sampleUs_=nowUs;
    }
    std::uint64_t creditedUs_=0,sampleUs_=0,generation_=0;
    std::uint32_t idleCounter_=0;
    bool armed_=false;
};
} // namespace nes::app::coupled_idle
