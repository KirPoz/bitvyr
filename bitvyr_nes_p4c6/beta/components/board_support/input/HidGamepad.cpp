#include "HidGamepad.h"

#include <algorithm>
#include <limits>

namespace nes::platform {
namespace {
/**
 * @brief Собирает беззнаковое little-endian значение HID short-item.
 * @details Вызывающий разборщик заранее проверяет доступный диапазон и длину не более 4 байтов.
 * @param data Буфер данных вызывающего кода достаточного размера.
 * @param count Количество элементов/отсчётов/байтов по контракту вызываемой функции.
 * @return Число из count байтов.
 */
std::uint32_t readUnsigned(const std::uint8_t* data, unsigned count) noexcept {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint32_t>(data[i]) << (i * 8);
    return value;
}
/**
 * @brief Расширяет ограниченное битовое HID-значение со знаком.
 * @details Нулевая ширина даёт 0; при 32 битах использует представление int32 без сдвига на ширину типа.
 * @param value Записываемое или преобразуемое значение.
 * @param bits Число значащих битов 0..32.
 * @return Знаковое 32-битное значение.
 */
std::int32_t signExtend(std::uint32_t value, unsigned bits) noexcept {
    if (!bits) return 0;
    if (bits >= 32) return static_cast<std::int32_t>(value);
    const auto sign = std::uint32_t{1} << (bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}
/**
 * @brief Извлекает поле HID из произвольного битового смещения.
 * @details Границы отчёта проверяет decode до вызова; размер поля ограничен 32 битами.
 * @param bytes Заимствованный массив входного HID-отчёта.
 * @param offset Битовое смещение поля от начала входного отчёта.
 * @param size Размер данных/поля; единица задаётся протоколом текущей функции.
 * @return Беззнаковое значение поля.
 */
std::uint32_t extract(const std::uint8_t* bytes, unsigned offset, unsigned size) noexcept {
    std::uint32_t value = 0;
    for (unsigned bit = 0; bit < size; ++bit) {
        const unsigned position = offset + bit;
        if (bytes[position / 8] & (1U << (position % 8))) value |= std::uint32_t{1} << bit;
    }
    return value;
}
}

/**
 * @brief Строит ограниченную схему входных HID-отчётов.
 * @details Сбрасывает старую схему; проверяет длины, стек Global, Report ID и предел 96 полей. Константы и неподдерживаемые Input всё равно двигают битовый указатель; Output/Feature не двигают его. Указатель descriptor не сохраняется.
 * @param descriptor Заимствованный HID-дескриптор либо запись GATT, не сохраняемая как указатель.
 * @param length Число доступных байтов входного массива.
 * @return true при законченной поддерживаемой схеме, иначе false.
 */
bool HidGamepad::parse(const std::uint8_t* descriptor, std::size_t length) noexcept {
    fieldCount_ = 0;
    maximumButtonUsage_ = 0;
    if (!descriptor || !length || length > 2048) return false;
    /**
     * @brief Локальный контекст HID Global и его ограниченный стек Push/Pop; данные не выходят за текущий разбор.
     */
    struct Global {
        std::uint16_t page{0};
        std::int32_t minimum{0};
        std::int32_t maximum{0};
        unsigned size{0};
        unsigned count{0};
        std::uint8_t reportId{0};
    } global, stack[8]{};
    unsigned depth = 0;
    std::uint32_t usages[32]{};
    unsigned usageCount = 0;
    std::uint32_t usageMinimum = 0, usageMaximum = 0;
    bool hasUsageRange = false;
    // Индекс — Report ID; только основные Input-поля продвигают входные смещения.
    std::uint16_t offsets[256]{};
    for (std::size_t position = 0; position < length;) {
        const auto prefix = descriptor[position++];
        if (prefix == 0xFE) {
            if (length - position < 2) return false;
            const unsigned count = descriptor[position];
            position += 2;
            if (count > length - position) return false;
            position += count;
            continue;
        }
        unsigned count = prefix & 3;
        if (count == 3) count = 4;
        if (count > length - position) return false;
        const auto value = readUnsigned(descriptor + position, count);
        const auto signedValue = signExtend(value, count * 8);
        position += count;
        const unsigned type = (prefix >> 2) & 3;
        const unsigned tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: global.page = static_cast<std::uint16_t>(value); break;
            case 1: global.minimum = signedValue; break;
            case 2: global.maximum = global.minimum < 0 ? signedValue : static_cast<std::int32_t>(value); break;
            case 7: if (value > 32) return false; global.size = value; break;
            case 8: if (!value || value > 255) return false; global.reportId = value; break;
            case 9: if (value > 256) return false; global.count = value; break;
            case 10: if (depth == 8) return false; stack[depth++] = global; break;
            case 11: if (!depth) return false; global = stack[--depth]; break;
            default: break;
            }
        } else if (type == 2) {
            const auto qualifiedUsage = count == 4 ? value : (static_cast<std::uint32_t>(global.page) << 16) | value;
            switch (tag) {
            case 0:
                if (usageCount == 32) return false;
                usages[usageCount++] = qualifiedUsage;
                break;
            case 1: usageMinimum = qualifiedUsage; hasUsageRange = true; break;
            case 2: usageMaximum = qualifiedUsage; break;
            default: break;
            }
        } else if (type == 0) {
            if (tag == 8) {
                auto& offset = offsets[global.reportId];
                const std::uint32_t total = static_cast<std::uint32_t>(global.size) * global.count;
                if (total > 8192 || offset + total > 8192) return false;
                const bool variable = (value & 2) != 0;
                const bool constant = (value & 1) != 0;
                for (unsigned i = 0; i < global.count; ++i) {
                    std::uint32_t usage = 0;
                    if (i < usageCount) usage = usages[i];
                    else if (hasUsageRange && usageMaximum >= usageMinimum && i <= usageMaximum - usageMinimum)
                        usage = usageMinimum + i;
                    else if (usageCount) usage = usages[usageCount - 1];
                    const unsigned page = usage >> 16;
                    const unsigned code = usage & 0xFFFF;
                    const bool button = page == 9 && code >= 1 && code <= 32;
                    const bool direction = page == 1 && (code == 0x30 || code == 0x31 || code == 0x39);
                    const bool guide = page == 0x0C && (code == 0x223 || code == 0x40);
                    if (!constant && variable && global.size && (button || direction || guide)) {
                        if (fieldCount_ == maxFields) return false;
                        auto& field = fields_[fieldCount_++];
                        field.bitOffset = offset + i * global.size;
                        field.usage = code;
                        field.page = page;
                        field.size = global.size;
                        field.reportId = global.reportId;
                        field.minimum = global.minimum;
                        field.maximum = global.maximum;
                        if (button) maximumButtonUsage_ = std::max(maximumButtonUsage_, code);
                    }
                }
                offset += total;
            }
            // Local-параметры действуют ровно на один Main item, включая Collection,
            // Output и Feature; последние не потребляют входной битовый поток.
            usageCount = 0;
            usageMinimum = usageMaximum = 0;
            hasUsageRange = false;
        }
    }
    if (!fieldCount_ || depth) return false;
    for (unsigned i = 0; i < fieldCount_; ++i)
        fields_[i].declaredBytes = static_cast<std::uint16_t>((offsets[fields_[i].reportId] + 7) / 8);
    return true;
}

/**
 * @brief Описывает пригодность одного Report ID для подписки.
 * @details Различает минимально читаемые и объявленные байты, игровые и меню-контролы; не изменяет схему.
 * @param reportId Идентификатор входного HID-отчёта.
 * @return Сводка HidReportInfo, нулевая при отсутствии полей.
 */
HidReportInfo HidGamepad::reportInfo(std::uint8_t reportId) const noexcept {
    HidReportInfo info{};
    const bool extended = maximumButtonUsage_ >= 13;
    for (unsigned i = 0; i < fieldCount_; ++i) {
        const auto& field = fields_[i];
        if (field.reportId != reportId) continue;
        info.requiredBytes = std::max(info.requiredBytes, (unsigned(field.bitOffset) + field.size + 7) / 8);
        info.declaredBytes = field.declaredBytes;
        if (field.page == 9) {
            if (field.usage == 1 || field.usage == 2 ||
                field.usage == (extended ? 11 : 7) || field.usage == (extended ? 12 : 8))
                info.nesControls = true;
            if (extended && field.usage == 13) info.menuControl = true;
        } else if (field.page == 0x0C) {
            info.menuControl = true;
        } else if (field.page == 1 && ((field.usage == 0x39 && field.maximum >= field.minimum) ||
                   (field.usage != 0x39 && field.maximum > field.minimum))) {
            // Учитываемые Desktop-поля — X/Y/hat. Вырожденная ось не меняет pad
            // и сама по себе не делает отчёт пригодным для игры.
            info.nesControls = true;
        }
    }
    return info;
}

/**
 * @brief Обновляет контролы одного Report ID из ограниченного пакета.
 * @details Проверяет границы каждого поля при обходе, сохраняет контролы других ID и нормализует оси/hat. При позднем отказе выход уже может содержать частичные изменения; для сохранения прежнего состояния вызывающий код декодирует в его копию. Данные пакета не сохраняет.
 * @param reportId Идентификатор входного HID-отчёта.
 * @param bytes Заимствованный массив входного HID-отчёта.
 * @param length Число доступных байтов входного массива.
 * @param state Обновляемые игровые контролы; поля остальных Report ID сохраняются.
 * @return true при найденных и допустимых полях данного ID, иначе false.
 */
bool HidGamepad::decode(std::uint8_t reportId, const std::uint8_t* bytes,
                        std::size_t length, HidPadState& state) const noexcept {
    if (!bytes || !length || length > 1024) return false;
    // Сохраняем контролы остальных Report ID, но перед применением новых значений
    // очищаем контролы, принадлежащие текущему отчёту.
    bool found = false;
    for (unsigned i = 0; i < fieldCount_; ++i) {
        const auto& field = fields_[i];
        if (field.reportId != reportId) continue;
        if (field.bitOffset + field.size > length * 8) return false;
        found = true;
        if (field.page == 9) state.buttons &= ~(std::uint32_t{1} << (field.usage - 1));
        else if (field.page == 0x0C) state.guide = false;
        else if (field.usage == 0x39) { state.directions = 0; }
    }
    if (!found) return false;
    for (unsigned i = 0; i < fieldCount_; ++i) {
        const auto& field = fields_[i];
        if (field.reportId != reportId) continue;
        const auto raw = extract(bytes, field.bitOffset, field.size);
        const auto value = field.minimum < 0 ? signExtend(raw, field.size) : static_cast<std::int32_t>(raw);
        if (field.page == 9) {
            if (value) state.buttons |= std::uint32_t{1} << (field.usage - 1);
        } else if (field.page == 0x0C) {
            state.guide = state.guide || value != 0;
        } else if (field.usage == 0x39) {
            const auto direction = static_cast<std::int64_t>(value) - field.minimum;
            if (value < field.minimum || value > field.maximum || direction < 0 || direction > 7) continue;
            constexpr std::uint8_t directions[]{1,9,8,10,2,6,4,5};
            state.directions = directions[direction];
        } else if (field.maximum > field.minimum) {
            const std::int64_t range = static_cast<std::int64_t>(field.maximum) - field.minimum;
            const auto normalized = std::clamp<std::int64_t>((static_cast<std::int64_t>(value) - field.minimum) * 65535 / range - 32768, -32768, 32767);
            if (field.usage == 0x30) state.x = normalized;
            if (field.usage == 0x31) state.y = normalized;
        }
    }
    // Hat независим от осей; мёртвую зону стика применяет вызывающий код.
    return true;
}

} // пространство имён nes::platform
