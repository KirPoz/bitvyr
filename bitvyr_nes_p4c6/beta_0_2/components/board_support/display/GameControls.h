#pragma once
#include <cstdint>

namespace nes::platform {
// Две логические панели 80×480 упакованы рядом с шагом 160.
// Источник не содержит пиксели библиотеки/настроек. Владение LCD вызывающий код
// получает до записи выбранного физического заднего буфера.
/**
 * @brief Копирует две боковые панели в native-буфер с поворотом 270°.
 * @details Источник имеет шаг 160 и не содержит меню; вызывающий код заранее получает владение выбранным native-буфером. Границы заданы геометрией платы.
 * @param controls Заимствованный RGB565-источник двух панелей 80×480 с шагом 160.
 * @param output Выбранный native RGB565-буфер после получения владения LCD/PPA.
 */
inline void copyGameControlsToNative(const std::uint16_t* controls,std::uint16_t* output) noexcept {
    for(unsigned x=0;x<480;++x) {
        const auto* row=controls+(479-x)*160;
        for(unsigned bar=0;bar<80;++bar) {
            output[bar*480+x]=row[bar];
            output[(720+bar)*480+x]=row[80+bar];
        }
    }
}
} // пространство имён nes::platform
