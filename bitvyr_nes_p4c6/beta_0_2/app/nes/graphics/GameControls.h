/**
 * @file GameControls.h
 * @brief Общая геометрия восьми NES-кнопок и EXIT/SETTINGS вне центральных640пикселей игры.
 */
#pragma once
#include "Types.h"
#include <cstddef>
#include <cstdint>

namespace nes::app::gameControls {
// Игровая область640×480 занимает x80..719. Каждый видимый и сенсорный
// элемент полностью помещается в одну из существующих боковых80-пиксельных полос.
/** @brief Полуоткрытый прямоугольник бокового сенсорного элемента в логических пикселях. */
struct Box final {
    int x,y,width,height;
    /**
     * @brief Проверяет попадание логической координаты в полуоткрытый прямоугольник кнопки.
     * @param px Проверяемая горизонтальная координата, в пикселях.
     * @param py Проверяемая вертикальная координата, в пикселях.
     */
    constexpr bool contains(unsigned px,unsigned py) const noexcept {
        return px>=unsigned(x)&&px<unsigned(x+width)&&py>=unsigned(y)&&py<unsigned(y+height);
    }
};
/** @brief Системное действие сенсора, отделённое от NES-битовой маски кнопок. */
enum class Action : std::uint8_t { None, ExitLibrary, Settings };
/** @brief Результат попадания: игровые кнопки либо отдельное действие EXIT/SETTINGS. */
struct Hit final { std::uint8_t buttons=0; Action action=Action::None; };
/** @brief Геометрия, подпись, NES-маска и масштаб шрифта одной боковой кнопки. */
struct Button final { Box area;const char* label;std::uint8_t mask;unsigned scale; };
// Одинаковая ширина72 сохраняет внутренний отступ4px в каждой боковой полосе.
// Системные действия, направления, SEL/START и A/B имеют общий размер в своей группе.
constexpr Box exit{4,8,72,56},pause{724,8,72,56},fps{4,80,72,64};
// Кнопки направлений намеренно широкие: крест из трёх колонок оставил бы
// для пальца цели шириной около3мм на панели4.3дюйма.
constexpr Button buttons[]{
    {{4,168,72,64},"",core::Up,2},
    {{4,240,72,64},"",core::Left,2},
    {{4,312,72,64},"",core::Right,2},
    {{4,384,72,64},"",core::Down,2},
    {{724,80,72,52},"SEL",core::Select,2},
    {{724,140,72,52},"START",core::Start,2},
    {{724,208,72,112},"B",core::B,4},
    {{724,336,72,112},"A",core::A,4}
};
/**
 * @brief Отличает четыре направления, рисуемые геометрическими стрелками.
 * @param mask Одиночная NES-маска кнопки из buttons, не произвольная комбинация ввода.
 * @return true только для Up/Down/Left/Right; UTF-8-глифы шрифту не требуются.
 */
constexpr bool isDirection(std::uint8_t mask) noexcept {
    return mask==core::Up||mask==core::Down||mask==core::Left||mask==core::Right;
}
/**
 * @brief Преобразует касание боковых80-пиксельных полос в NES-кнопку либо EXIT/SETTINGS. Центральная игра не перехватывается.
 * @param x Логическая координата по горизонтали, в пикселях.
 * @param y Логическая координата по вертикали, в пикселях.
 */
constexpr Hit hit(unsigned x,unsigned y) noexcept {
    if(x>=800||y>=480||(x>=80&&x<720))return {};
    if(exit.contains(x,y))return {0,Action::ExitLibrary};
    if(pause.contains(x,y))return {0,Action::Settings};
    for(const auto& button:buttons)if(button.area.contains(x,y))return {button.mask,Action::None};
    return {};
}
constexpr unsigned chromeWidth=160,chromeHeight=480;
/**
 * @brief Переводит логический x боковой полосы в упакованный160-пиксельный буфер интерфейса.
 * @param logicalX Логический x бокового элемента; вызывающий уже исключил центральную область игры.
 */
constexpr unsigned packedX(unsigned logicalX) noexcept{return logicalX<80?logicalX:logicalX-640;}
} // namespace nes::app::gameControls
