/** @file TouchOverlapTests.cpp @brief Проверяет свежесть и поколение раннего Main-снимка без SDK/I2C. */
#include <cassert>
#include <cstdint>
#include <optional>

namespace { std::int64_t hostTime=1000; }
/** @brief Возвращает управляемое тестом монотонное host-время. */
std::int64_t esp_timer_get_time() noexcept { return hostTime; }
namespace nes::platform {
/** @brief Минимальное содержимое контактов, позволяющее отличить старую и новую выборки. */
struct TouchContacts { unsigned value=0; };
/** @brief Считает обращения к одному Main-владельцу; чтение занимает600 условных микросекунд. */
struct Board {
    TouchContacts next{11}; unsigned reads=0; bool valid=true;
    /** @brief Копирует текущий контакт и учитывает реальный повторный опрос модели. */
    bool pollTouchContacts(TouchContacts& out) noexcept { ++reads;hostTime+=600;out=next;return valid; }
};
}
#define NES_APP_OVERLAP_TOUCH_POLL 1
namespace nes::app {
#include "../../app/nes/input/TouchOverlap.inl"
/** @brief Моделирует исключительно Main-поля непрозрачного Worker control. */
struct TouchWorker {
    std::optional<detail::TouchPrefetch> touchPrefetch{};
    std::uint64_t touchPrefetchGeneration=0;
};
/** @brief Исполняет тот же generation-guard macro, что и настоящая frame. */
struct Harness {
    platform::Board board_{};TouchWorker storage{};TouchWorker* worker_=&storage;
    std::uint64_t sessionGeneration_=1;
    /** @brief Снимает I2C до ожидания producer и связывает результат с текущей сессией. */
    void prefetch() { storage.touchPrefetch.emplace(board_,true);storage.touchPrefetchGeneration=sessionGeneration_; }
    /** @brief Возвращает свежий снимок либо выполняет единственный безопасный повторный опрос. */
    bool poll(platform::TouchContacts& out,bool pal=false) {
        NES_APP_TOUCH_PREFETCH
        return NES_APP_TOUCH_CONTACTS(board_,out,pal);
    }
};
}
/** @brief Проверяет скрытое чтение, точные границы возраста, смену игры, отказ I2C и отсутствие Worker. */
int main() {
    using nes::app::Harness;nes::platform::TouchContacts out;
    {
        Harness h;h.prefetch();h.board_.next.value=22;hostTime+=5000;
        assert(h.poll(out)&&out.value==11&&h.board_.reads==1);
    }
    for(bool pal:{false,true}) {
        Harness h;h.prefetch();h.board_.next.value=22;hostTime+=pal?20000:16639;
        assert(h.poll(out,pal)&&out.value==11&&h.board_.reads==1);
        ++hostTime;
        assert(h.poll(out,pal)&&out.value==22&&h.board_.reads==2);
    }
    {
        Harness h;h.prefetch();++h.sessionGeneration_;h.board_.next.value=33;
        assert(h.poll(out)&&out.value==33&&h.board_.reads==2);
    }
    {
        Harness h;h.board_.valid=false;h.prefetch();h.board_.valid=true;
        assert(!h.poll(out)&&h.board_.reads==1);
    }
    {
        Harness h;h.prefetch();h.worker_=nullptr;h.board_.next.value=44;
        assert(h.poll(out)&&out.value==44&&h.board_.reads==2);
    }
    {
        Harness h;assert(h.poll(out)&&out.value==11&&h.board_.reads==1);
    }
}
