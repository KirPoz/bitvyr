/**
 * @file MenuLayout.h
 * @brief Чистая арифметика страниц/попаданий каталога; не обращается к SD и дисплею.
 */
#pragma once

#include <cstddef>
#include <limits>

namespace nes::app::menuLayout {
constexpr std::size_t rows = 10;
constexpr std::size_t pageSize = 20;
constexpr auto noItem = std::numeric_limits<std::size_t>::max();
/**
 * @brief Вычисляет число страниц каталога с учётом неполной последней; пустой каталог обрабатывается без underflow.
 * @param count Число проиндексированных игр в каталоге.
 */
constexpr std::size_t pages(std::size_t count) noexcept {
    return count / pageSize + (count % pageSize != 0);
}
/**
 * @brief Возвращает первый индекс страницы, содержащей выбранную игру.
 * @param selected Нулевой индекс выбранной игры; допустимость выбора обеспечивает вызывающий.
 */
constexpr std::size_t first(std::size_t selected) noexcept {
    return selected / pageSize * pageSize;
}
/**
 * @brief Возвращает начало предыдущей страницы; на первой странице оставляет selected. Число игр не проверяется.
 * @param selected Нулевой индекс выбранной игры; допустимость выбора обеспечивает вызывающий.
 */
constexpr std::size_t previous(std::size_t selected) noexcept {
    return first(selected) >= pageSize ? first(selected) - pageSize : selected;
}
/**
 * @brief Возвращает начало существующей следующей страницы, иначе исходный selected. Неверный исходный выбор не исправляется.
 * @param count Число проиндексированных игр в каталоге.
 * @param selected Нулевой индекс выбранной игры; допустимость выбора обеспечивает вызывающий.
 */
constexpr std::size_t next(std::size_t selected, std::size_t count) noexcept {
    const auto start = first(selected);
    return start < count && count - start > pageSize ? start + pageSize : selected;
}
/**
 * @brief Преобразует координату касания в элемент текущей страницы либо отсутствие попадания.
 * @param count Число проиндексированных игр в каталоге.
 * @param x Логическая координата по горизонтали, в пикселях.
 * @param y Логическая координата по вертикали, в пикселях.
 * @param selected Нулевой индекс для определения текущей страницы; hit дополнительно проверяет попадание в count.
 */
constexpr std::size_t hit(std::size_t selected, std::size_t count, int x, int y) noexcept {
    if (x < 0 || x >= 800 || y < 72 || y >= 72 + 31 * int(rows)) return noItem;
    const auto local = std::size_t(x >= 410 ? rows : 0) + std::size_t((y - 72) / 31);
    const auto start = first(selected);
    return start < count && local < count - start ? start + local : noItem;
}
} // namespace nes::app::menuLayout
