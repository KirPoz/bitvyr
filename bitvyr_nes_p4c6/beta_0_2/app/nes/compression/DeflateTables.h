#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nes::compression {

/** @brief Базовые длины и числа дополнительных битов кодов 257..285 по RFC 1951. */
inline constexpr std::uint16_t lengthBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
inline constexpr std::uint8_t lengthBits[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
/** @brief Базовые расстояния и числа дополнительных битов кодов 0..29 по RFC 1951. */
inline constexpr std::uint16_t distanceBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
inline constexpr std::uint8_t distanceBits[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
/** @brief Порядок передачи 19 длин алфавита длин в динамическом блоке DEFLATE. */
inline constexpr unsigned codeLengthOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

/**
 * @brief Каноническая таблица Хаффмана для ограниченного алфавита DEFLATE.
 * @details Не владеет вводом, выводом, буферами истории или политикой ошибок.
 * Хранилище и алгоритм совпадают у файлового ZIP и пошагового replay-декодера.
 */
struct Huffman {
    std::uint16_t counts[16]{}, firstCode[16]{}, firstSymbol[16]{}, symbols[288]{};
    unsigned maximum = 0;

    /**
     * @brief Строит канонические коды, сохраняя прежние правила полноты дерева.
     * @param lengths Неизменный массив не менее size длин; каждая длина не больше 15.
     * @param size Число символов, не превышающее вместимость 288; проверяется владельцем формата.
     * @param emptyAllowed Разрешает пустой алфавит, например отсутствующие расстояния.
     * @param complete Требует полное дерево; неполное односимвольное допускается только при false.
     * @return true при допустимом дереве; false при недопустимых длинах/полноте. Ошибка не откатывает таблицу.
     */
    bool build(const std::uint8_t* lengths, unsigned size, bool emptyAllowed, bool complete) noexcept {
        std::memset(counts, 0, sizeof(counts));
        maximum = 0;
        for (unsigned i = 0; i < size; ++i) {
            if (lengths[i] > 15) return false;
            if (lengths[i]) { ++counts[lengths[i]]; maximum = std::max(maximum, unsigned(lengths[i])); }
        }
        if (!maximum) return emptyAllowed;
        int freeCodes = 1;
        for (unsigned i = 1; i <= 15; ++i) {
            freeCodes = freeCodes * 2 - counts[i];
            if (freeCodes < 0) return false;
        }
        if (freeCodes && (complete || maximum != 1 || counts[1] != 1)) return false;
        unsigned code = 0, index = 0;
        std::uint16_t next[16]{};
        for (unsigned i = 1; i <= 15; ++i) {
            code = (code + counts[i - 1]) << 1;
            firstCode[i] = static_cast<std::uint16_t>(code);
            next[i] = firstSymbol[i] = static_cast<std::uint16_t>(index);
            index += counts[i];
        }
        for (unsigned i = 0; i < size; ++i) if (lengths[i]) symbols[next[lengths[i]]++] = static_cast<std::uint16_t>(i);
        return true;
    }

    /**
     * @brief Декодирует один символ через существующий интерфейс readBits владельца.
     * @param input Заимствованный ввод с readBits(unsigned,uint32_t&) и полем error; ошибка чтения сохраняется.
     * @param result Выходной символ; изменяется только при найденном коде.
     * @param invalidError Ошибка формата владельца, записываемая только при исчерпании поиска кода.
     * @return true при найденном символе; false при ошибке ввода либо отсутствующем коде.
     */
    template<class Input, class Error>
    bool decode(Input& input, unsigned& result, Error invalidError) const noexcept {
        unsigned code = 0;
        for (unsigned length = 1; length <= maximum; ++length) {
            std::uint32_t bit;
            if (!input.readBits(1, bit)) return false;
            code = (code << 1) | bit;
            if (code >= firstCode[length] && code - firstCode[length] < counts[length]) {
                result = symbols[firstSymbol[length] + code - firstCode[length]];
                return true;
            }
        }
        input.error = invalidError;
        return false;
    }
};

} // namespace nes::compression
