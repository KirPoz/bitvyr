/**
 * @file Font5x7.h
 * @brief Единый 5x7 ASCII шрифт Board/native-preview.
 */
#pragma once
#include <cstdint>
namespace nes::platform::font5x7 {
// Собственный маленький алфавит меню 5×7; один байт соответствует строке.
inline constexpr std::uint8_t letters[][7]{
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};
inline constexpr std::uint8_t digits[][7]{
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
};
/**
 * @brief Возвращает пять бит одной строки собственного символа 5×7.
 * @details Строчные латинские буквы приводятся к верхнему регистру; неподдерживаемый символ получает знак вопроса. row обязан быть 0..6.
 * @param character Байт символа собственного ограниченного алфавита.
 * @param row Строка символа 0..6.
 * @return Маска пикселей текущей строки.
 */
inline std::uint8_t glyph(char character, unsigned row) noexcept {
    if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'][row];
    if (character >= '0' && character <= '9') return digits[character - '0'][row];
    switch (character) {
    case ' ': return 0;
    case '.': return row == 6 ? 4 : 0;
    case ',': return row == 6 ? 8 : row == 5 ? 4 : 0;
    case ':': return row == 2 || row == 5 ? 4 : 0;
    case '-': return row == 3 ? 14 : 0;
    case '_': return row == 6 ? 31 : 0;
    case '/': return 1U << (row * 4 / 6);
    case '\\': return 16U >> (row * 4 / 6);
    case '!': return row < 4 || row == 6 ? 4 : 0;
    case '>': return 1U << (row < 4 ? 3 - row : row - 3);
    case '<': return 1U << (row < 4 ? row + 1 : 7 - row);
    case '%': { constexpr std::uint8_t percent[]{25,25,2,4,8,19,19}; return percent[row]; }
    case '|': return 4;
    case '+': return row == 3 ? 31 : (row >= 1 && row <= 5 ? 4 : 0);
    case '=': return row == 2 || row == 4 ? 31 : 0;
    case '[': return row == 0 || row == 6 ? 14 : 8;
    case ']': return row == 0 || row == 6 ? 14 : 2;
    case '(': return row == 0 || row == 6 ? 2 : (row == 1 || row == 5 ? 4 : 8);
    case ')': return row == 0 || row == 6 ? 8 : (row == 1 || row == 5 ? 4 : 2);
    case '\'': return row < 2 ? 4 : 0;
    case '&': { constexpr std::uint8_t amp[]{12,18,20,8,21,18,13}; return amp[row]; }
    default: { constexpr std::uint8_t unknown[]{14,17,1,2,4,0,4}; return unknown[row]; }
    }
}

} // пространство имён nes::platform::font5x7
