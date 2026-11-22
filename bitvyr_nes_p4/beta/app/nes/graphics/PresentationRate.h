#pragma once
#include <cstdint>

namespace nes::app {

/**
 * @brief Частота подтверждённых программных LCD-submit за секундное окно Main.
 * @details Не считает кадры самостоятельно, не обращается к Machine/Board и не
 * доказывает физический VSYNC. Вызывающий передаёт только успешный submitted-счётчик.
 * Единственный владелец — Main; одновременный доступ из Worker не поддерживается.
 */
class PresentationRate final {
public:
    /** @brief Создаёт неинициализированное окно с недействительным значением частоты. */
    constexpr PresentationRate() noexcept = default;

    /**
     * @brief Начинает новое окно при запуске, паузе, смене сессии или явной перебазировке.
     * @param nowUs Текущая host-метка времени в микросекундах.
     * @param submitted Текущий суммарный счётчик подтверждённых программных submit.
     */
    void reset(std::int64_t nowUs, std::uint64_t submitted) noexcept {
        windowUs_ = lastUs_ = nowUs;
        windowSubmitted_ = lastSubmitted_ = submitted;
        fps_ = 0;
        started_ = true;
        valid_ = false;
    }

    /**
     * @brief Наблюдает счётчик и обновляет частоту не чаще одного раза в секунду.
     * @param nowUs Текущая host-метка; обратный ход относительно последнего наблюдения сбрасывает окно.
     * @param submitted Текущий подтверждённый счётчик; уменьшение или wrap сбрасывает окно.
     * @return true при изменении опубликованной частоты/valid; одинаковое завершённое
     * окно перебазируется без сигнала перерисовки. Первое наблюдение лишь задаёт начало.
     * @details Нулевой прирост за полную секунду публикует 0, а не сохраняет старую
     * частоту. При сбросе valid->false также возвращается true для очистки старого HUD.
     */
    bool observe(std::int64_t nowUs, std::uint64_t submitted) noexcept {
        if (!started_) {
            reset(nowUs, submitted);
            return false;
        }
        if (nowUs < lastUs_ || submitted < lastSubmitted_) {
            const bool changed = valid_;
            reset(nowUs, submitted);
            return changed;
        }
        lastUs_ = nowUs;
        lastSubmitted_ = submitted;
        // После проверки монотонности unsigned-разность сохраняет полный диапазон int64 без UB.
        const auto elapsed = static_cast<std::uint64_t>(nowUs) - static_cast<std::uint64_t>(windowUs_);
        if (elapsed < windowLengthUs) return false;
        const double next = static_cast<double>(submitted - windowSubmitted_) * 1000000.0 /
                            static_cast<double>(elapsed);
        const bool changed = !valid_ || next != fps_;
        fps_ = next;
        valid_ = true;
        windowUs_ = nowUs;
        windowSubmitted_ = submitted;
        return changed;
    }

    /** @brief Возвращает true только после полного монотонного секундного окна. */
    bool valid() const noexcept { return valid_; }

    /** @brief Возвращает последнюю частоту software-submit; до валидного окна возвращает 0. */
    double fps() const noexcept { return fps_; }

private:
    static constexpr std::uint64_t windowLengthUs = 1000000;
    std::int64_t windowUs_ = 0, lastUs_ = 0;
    std::uint64_t windowSubmitted_ = 0, lastSubmitted_ = 0;
    double fps_ = 0;
    bool started_ = false, valid_ = false;
};

} // пространство имён nes::app
