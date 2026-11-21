/** @file PauseMenu.h @brief Единые прямоугольники быстрого меню и подтверждения для рисования/hit-test. */
#pragma once
#include "GameControls.h"
#include <cstddef>

namespace nes::app::pauseMenuLayout {
inline constexpr gameControls::Box rows[]{
    {160,116,480,60},{160,192,480,60},{160,268,480,60},{160,344,480,60}};
inline constexpr gameControls::Box cancel{160,230,480,64},confirm{160,314,480,64};
inline constexpr unsigned noItem=4;
/**
 * @brief Возвращает индекс видимого пункта либо noItem; зазоры не вызывают действие.
 * @param x Логическая координата касания 800×480 по горизонтали.
 * @param y Логическая координата касания по вертикали.
 * @param confirmation Два пункта CANCEL/CONFIRM вместо четырёх пунктов паузы.
 */
constexpr unsigned hit(unsigned x,unsigned y,bool confirmation) noexcept {
    if(confirmation)return cancel.contains(x,y)?0:confirm.contains(x,y)?1:noItem;
    for(unsigned index=0;index<4;++index)if(rows[index].contains(x,y))return index;
    return noItem;
}
} // namespace nes::app::pauseMenuLayout
