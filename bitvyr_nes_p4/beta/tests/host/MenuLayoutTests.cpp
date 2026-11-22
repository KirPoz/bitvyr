#include "../../app/nes/graphics/MenuLayout.h"
#include <cassert>
#include <initializer_list>

/**
 * @brief Проверяет геометрию меню и game-viewport при заданных размерах экрана.
 * @details Ожидания не требуют Board/шрифтов/аппаратного PPA; нарушение layout assertion завершает тест.
 */
int main() {
    namespace m = nes::app::menuLayout;
    assert(m::pages(0) == 0 && m::pages(19) == 1 && m::pages(20) == 1);
    assert(m::pages(21) == 2 && m::pages(1001) == 51);
    assert(m::hit(0, 0, 20, 76) == m::noItem);
    assert(m::hit(0, 19, 415, 355) == m::noItem);
    assert(m::hit(0, 20, 415, 355) == 19);
    assert(m::hit(20, 21, 20, 76) == 20);
    assert(m::hit(20, 21, 415, 76) == m::noItem);
    assert(m::hit(1000, 1001, 20, 76) == 1000);
    for (int y : {-1, 0, 71, 382, 404, 479}) assert(m::hit(0, 1000, 20, y) == m::noItem);
    assert(m::hit(0, 1000, -1, 76) == m::noItem);
    assert(m::hit(0, 1000, 800, 76) == m::noItem);
    assert(m::next(0, 21) == 20 && m::next(20, 21) == 20);
    assert(m::previous(20) == 0 && m::previous(0) == 0);
    const std::size_t counts[]{1, 19, 20, 21, 64, 65, 1001};
    for (std::size_t count : counts) {
        for (std::size_t start = 0; start < count; start += m::pageSize) {
            for (std::size_t local = 0; local < m::pageSize; ++local) {
                const int x = local < 10 ? 20 : 415;
                const int y = 76 + int(local % 10) * 31;
                assert(m::hit(start, count, x, y) == (local < count - start ? start + local : m::noItem));
            }
        }
    }
}
