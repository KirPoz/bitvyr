/**
 * @file NameMarkerRows.h
 * @brief Физический CIRAM-индекс FD/FE для консервативного допуска Legacy-интервалов.
 * @ingroup nes_ppu
 */
#pragma once
#include <array>
#include <cstdint>

namespace nes::core {
// Производный индекс физических CIRAM-байтов FD/FE. Включает строки
// атрибутов 30/31: ПО может выбирать их как tile-байты. Владелец обновляет
// индекс при КАЖДОЙ физической NameWrite и очищает ТОЛЬКО вместе с настоящей NT-
// памятью; после отказа загрузки с сохранением NT индекс тоже сохраняется.
/**
 * @brief Битовый индекс физических NT-байтов FD/FE, предотвращающий незаконное объединение Legacy-фона через смену CHR-защёлки.
 * @ingroup nes_ppu
 */
class NameMarkerRows final {
public:
    /**
     * @brief Сбрасывает внутренний индекс/маску вспомогательного объекта.
     * @details Не является очисткой всей памяти машины или подтверждением IRQ.
     */
    void clear() noexcept { rows_.fill(0); }
    /**
     * @brief Обновляет битовый индекс изменённой записи OAM/таблицы имён.
     * @details Вспомогательная маска должна соответствовать реальной записи, включая DMA и повторную запись того же значения.
     * @param physical Физическая позиция в индексируемой OAM/NT-памяти.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool update(unsigned physical,std::uint8_t value) noexcept {
        if(physical>=4096)return false;
        const auto bit=std::uint32_t(1)<<(physical&31U);
        auto& row=rows_[physical>>5];
        if(value==0xFD||value==0xFE)row|=bit;else row&=~bit;
        return true;
    }
    // True достаточно для отсутствия маркеров. Неверное представление/число вызывает отказ.
    // X может обернуться один раз при count<=32 с переключением logical nametable bit10. Физические
    // алиасы страниц остаются алиасами: индекс физический, а не логический.
    /**
     * @brief Проверяет отсутствие опасных маркеров в нужных физических частях строки.
     * @details Горизонтальное переключение таблицы имён и конец 32-битной маски учитываются явно.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param pages Четыре физических CIRAM-offset для логических nametable-страниц; алиасы сохраняются без копий памяти.
     */
    bool clearForSpan(std::uint16_t v,unsigned count,const std::array<std::uint16_t,4>& pages) const noexcept {
        if(!count||count>32||(v&0x8000U))return false;
        for(auto page:pages)if(page>3072U||(page&1023U))return false;
        const unsigned x=v&31U,y=(v>>5)&31U,nt=(v>>10)&3U;
        const unsigned first=count<32U-x?count:32U-x;
        if(((rows_[(pages[nt]>>5)+y]>>x)&lowBits(first))!=0)return false;
        const unsigned second=count-first;
        return !second||(rows_[(pages[nt^1U]>>5)+y]&lowBits(second))==0;
    }
private:
    /**
     * @brief Формирует маску младших битов без неопределённого сдвига на ширину слова.
     * @details Полная ширина обрабатывается отдельным случаем.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
     */
    static std::uint32_t lowBits(unsigned count) noexcept {
        return count==32?0xFFFFFFFFU:(std::uint32_t(1)<<count)-1U;
    }
    std::array<std::uint32_t,128> rows_{};
};
static_assert(sizeof(NameMarkerRows)==512,"one marker bit per physical nametable byte");
} // пространство имён nes::core
