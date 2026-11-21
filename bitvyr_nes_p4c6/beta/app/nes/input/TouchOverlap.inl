/**
 * @file TouchOverlap.inl
 * @brief Необязательный ранний опрос GT911 на Main с сохранением снимка и длительности.
 */
// Включается внутри nes::app. Действия ввода остаются в NesApp::frame после передачи владения.
namespace detail {
/** @brief Main-снимок необязательного раннего опроса GT911; не переносит I2C-владение в Worker. */
class TouchPrefetch final {
public:
    /**
     * @brief Начинает необязательный ранний опрос сенсора на Main; не создаёт второго владельца Board.
     * @param board Аппаратный адаптер Main; функция заимствует ссылку и не передаёт его Worker.
     * @param eligible Разрешён ли ранний опрос в текущем кадровом режиме.
     */
    TouchPrefetch(platform::Board& board,bool eligible) noexcept {
        if(!eligible)return;
        begin_=esp_timer_get_time();
        valid_=board.pollTouchContacts(contacts_);
        end_=esp_timer_get_time();
        elapsed_=end_-begin_;
        attempted_=true;
    }
    /**
     * @brief Возвращает свежий ранний снимок; при отсутствии или истечении его срока заново опрашивает I2C.
     * @param board Аппаратный адаптер Main; функция заимствует ссылку и не передаёт его Worker.
     * @param out Выходной снимок контактов текущего опроса.
     * @param pal Выбирает предельный возраст снимка:20000 мкс для PAL,16639 мкс для NTSC.
     */
    bool contacts(platform::Board& board,platform::TouchContacts& out,bool pal) noexcept {
        if(!attempted_||esp_timer_get_time()-end_>(pal?20000:16639))
            return board.pollTouchContacts(out);
        out=contacts_;
        return valid_;
    }
    /**
     * @brief Отмечает начало раннего опроса для отдельного учёта времени ввода.
     * @param fallback Ранее снятое host-время, если предварительный опрос не запускался.
     */
    std::int64_t begin(std::int64_t fallback) const noexcept { return attempted_?begin_:fallback; }
    /**
     * @brief Возвращает host-микросекунды, затраченные на сохранённый опрос сенсора.
     */
    std::int64_t elapsed() const noexcept { return elapsed_; }
private:
    platform::TouchContacts contacts_{};
    std::int64_t begin_=0,end_=0,elapsed_=0;
    bool valid_=false,attempted_=false;
};
}
#define NES_APP_TOUCH_PREFETCH     detail::TouchPrefetch fallbackTouch(board_,false);     auto& earlyTouch=(worker_&&worker_->touchPrefetch&&worker_->touchPrefetchGeneration==sessionGeneration_)         ? *worker_->touchPrefetch : fallbackTouch;
#define NES_APP_TOUCH_CONTACTS(boardArg,contactsArg,palArg) earlyTouch.contacts(boardArg,contactsArg,palArg)
#define NES_APP_TOUCH_INPUT_BEGIN(fallbackArg) earlyTouch.begin(fallbackArg)
#define NES_APP_TOUCH_INPUT_US(elapsedArg) ((elapsedArg)+earlyTouch.elapsed())
