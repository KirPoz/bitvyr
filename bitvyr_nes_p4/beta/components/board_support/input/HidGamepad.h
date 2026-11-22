#pragma once

#include <cstddef>
#include <cstdint>

namespace nes::platform {

/**
 * @brief Накопленное состояние HID, сохраняющее контролы независимых Report ID; оси нормализованы, направления hat хранятся отдельно.
 */
struct HidPadState final {
    std::uint32_t buttons{0};
    std::int16_t x{0};
    std::int16_t y{0};
    std::uint8_t directions{0}; // вверх 1/вниз 2/влево 4/вправо 8
    bool guide{false};
};

/**
 * @brief Границы одного входного отчёта и признаки игровых/меню-контролов для безопасной GATT-подписки.
 */
struct HidReportInfo final {
    unsigned requiredBytes{0};
    unsigned declaredBytes{0}; // Включает постоянные/необрабатываемые Input-поля, но не Output/Feature.
    bool nesControls{false};
    bool menuControl{false};
};

// Ограниченный HID short-item парсер без зависимости от Bluetooth/ESP-IDF.
// Принимает кнопки, оси X/Y и hat. Постоянные и неизвестные поля тоже
// занимают биты отчёта: смещения задаёт дескриптор.
/**
 * @brief Независимый от SDK ограниченный парсер HID short-item. Хранит собственные поля, не сохраняет указатели на дескриптор/отчёты и не выделяет память; начальные счётчики нулевые.
 */
class HidGamepad final {
public:
    /**
     * @brief Строит ограниченную схему входных HID-отчётов.
     * @details Сбрасывает старую схему; проверяет длины, стек Global, Report ID и предел 96 полей. Константы и неподдерживаемые Input всё равно двигают битовый указатель; Output/Feature не двигают его. Указатель descriptor не сохраняется.
     * @param descriptor Заимствованный HID-дескриптор либо запись GATT, не сохраняемая как указатель.
     * @param length Число доступных байтов входного массива.
     * @return true при законченной поддерживаемой схеме, иначе false.
     */
    [[nodiscard]] bool parse(const std::uint8_t* descriptor, std::size_t length) noexcept;
    /**
     * @brief Обновляет контролы одного Report ID из ограниченного пакета.
     * @details Проверяет границы каждого поля при обходе, сохраняет контролы других ID и нормализует оси/hat. При позднем отказе выход уже может содержать частичные изменения; для сохранения прежнего состояния вызывающий код декодирует в его копию. Данные пакета не сохраняет.
     * @param reportId Идентификатор входного HID-отчёта.
     * @param bytes Заимствованный массив входного HID-отчёта.
     * @param length Число доступных байтов входного массива.
     * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
     * @return true при найденных и допустимых полях данного ID, иначе false.
     */
    [[nodiscard]] bool decode(std::uint8_t reportId, const std::uint8_t* bytes,
                              std::size_t length, HidPadState& state) const noexcept;
    /**
     * @brief Возвращает наибольший распознанный Button Usage.
     * @details Позволяет отличить расширенную Xbox-раскладку без повторного разбора дескриптора.
     * @return Максимальный номер кнопки или 0.
     */
    [[nodiscard]] unsigned maximumButtonUsage() const noexcept { return maximumButtonUsage_; }
    /**
     * @brief Возвращает число поддерживаемых полей текущей HID-схемы.
     * @details Не включает игнорируемые поля, хотя они учитываются в битовых смещениях.
     * @return Количество 0..96.
     */
    [[nodiscard]] unsigned fieldCount() const noexcept { return fieldCount_; }
    /**
     * @brief Описывает пригодность одного Report ID для подписки.
     * @details Различает минимально читаемые и объявленные байты, игровые и меню-контролы; не изменяет схему.
     * @param reportId Идентификатор входного HID-отчёта.
     * @return Сводка HidReportInfo, нулевая при отсутствии полей.
     */
    [[nodiscard]] HidReportInfo reportInfo(std::uint8_t reportId) const noexcept;
private:
    /**
     * @brief Закрытое описание поддерживаемого HID-поля: Report ID, битовые границы, Usage и логический диапазон. Поля остальных типов всё равно учитываются в объявленной длине отчёта.
     */
    struct Field {
        std::uint16_t bitOffset{0};
        std::uint16_t usage{0};
        std::uint16_t page{0};
        std::uint8_t size{0};
        std::uint8_t reportId{0};
        std::int32_t minimum{0};
        std::int32_t maximum{0};
        std::uint16_t declaredBytes{0};
    };
    static constexpr unsigned maxFields = 96;
    Field fields_[maxFields]{};
    unsigned fieldCount_{0};
    unsigned maximumButtonUsage_{0};
};

} // пространство имён nes::platform
