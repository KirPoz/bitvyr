/** @file StatusPollSearch.h @brief Поиск максимального доказанного префикса полных STATUS-итераций. */
#pragma once
namespace nes::core::statuspollsearch {
/**
 * @brief Находит тот же предел, что нисходящая проверка upper..2, за логарифмическое число проб.
 * @param upper Целая верхняя граница числа итераций; у STATUS helper не превосходит 36.
 * @param fits Чистый предикат неизменного состояния: допустимые n образуют начальный префикс.
 * @return Максимальное допустимое n >= 2; 1 при отказе всех n >= 2; исходные 0/1 без проб.
 * @note Монотонность обязана быть доказана вызывающим кодом. Q(0) здесь не предполагается.
 * Предикат не вправе продвигать clocks, выполнять guest read или менять проверяемое состояние.
 */
template<class Predicate>
unsigned maximumIterations(unsigned upper,Predicate& fits) noexcept {
    if(upper<2||fits(upper))return upper;
    if(upper==2||!fits(2))return 1;
    unsigned low=2,high=upper;
    while(high-low>1){
        const unsigned middle=low+(high-low)/2;
        if(fits(middle))low=middle;else high=middle;
    }
    return low;
}
}
