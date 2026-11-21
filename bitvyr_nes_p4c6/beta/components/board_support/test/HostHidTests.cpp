#include "../input/HidGamepad.h"

#include <cassert>
#include <cstdio>

using nes::platform::HidGamepad;
using nes::platform::HidPadState;

/**
 * @brief Проверяет собственный HID-парсер на сохранённых описателях и отчётах.
 * @details Host-тест использует assert для Report ID, границ, осей, hat/Guide и сохранения независимых контролов; периферия и плата не запускаются.
 * @return 0 после всех успешных проверок; assert прерывает неверный случай.
 */
int main() {
    // Два независимых Input Report ID: геймпад и Consumer/Home.
    // Padding кнопок и Output-поля намеренно присутствуют в общем дескрипторе.
    constexpr std::uint8_t map[]{
        0x05,0x01, 0x09,0x05, 0xA1,0x01, 0x85,0x01,
        0x05,0x09, 0x19,0x01, 0x29,0x0F, 0x15,0x00, 0x25,0x01,
        0x75,0x01, 0x95,0x0F, 0x81,0x02,
        0x75,0x01, 0x95,0x01, 0x81,0x03,
        0x05,0x01, 0x09,0x39, 0x15,0x01, 0x25,0x08,
        0x75,0x04, 0x95,0x01, 0x81,0x42,
        0x75,0x04, 0x95,0x01, 0x81,0x03,
        0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0xFF,
        0x75,0x10, 0x95,0x02, 0x81,0x02,
        0x75,0x08, 0x95,0x04, 0x91,0x02,
        0x85,0x02, 0x05,0x0C, 0x0A,0x23,0x02,
        0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01, 0x81,0x02,
        0x75,0x07, 0x95,0x01, 0x81,0x03, 0xC0
    };
    HidGamepad parser;
    assert(parser.parse(map, sizeof(map)));
    assert(parser.maximumButtonUsage() == 15);
    const auto mainInfo = parser.reportInfo(1), guideInfo = parser.reportInfo(2), unknownInfo = parser.reportInfo(99);
    assert(mainInfo.requiredBytes == 7 && mainInfo.declaredBytes == 7 && mainInfo.nesControls);
    assert(guideInfo.requiredBytes == 1 && guideInfo.declaredBytes == 1 && !guideInfo.nesControls && guideInfo.menuControl);
    assert(unknownInfo.requiredBytes == 0 && unknownInfo.declaredBytes == 0 && !unknownInfo.nesControls && !unknownInfo.menuControl);
    HidPadState state{};
    const std::uint8_t neutral[]{0,0,0, 0x00,0x80, 0x00,0x80};
    assert(parser.decode(1, neutral, sizeof(neutral), state));
    assert(state.buttons == 0 && state.directions == 0);
    assert(state.x >= -1 && state.x <= 1 && state.y >= -1 && state.y <= 1);
    const std::uint8_t pressed[]{0x03,0x0C,0x02, 0x00,0x00, 0xFF,0xFF};
    assert(parser.decode(1, pressed, sizeof(pressed), state));
    assert(state.buttons == 0x0C03); // A/B и View/Menu, Usage11/12.
    assert(state.directions == 9); // Hat с нумерацией от 1: значение 2 означает северо-восток.
    assert(state.x == -32768 && state.y == 32767);
    const std::uint8_t home[]{1};
    assert(parser.decode(2, home, sizeof(home), state));
    assert(state.guide && state.buttons == 0x0C03); // Состояние другого отчёта сохранено.
    const std::uint8_t releaseHome[]{0};
    assert(parser.decode(2, releaseHome, sizeof(releaseHome), state));
    assert(!state.guide);
    assert(!parser.decode(1, pressed, 3, state)); // Усечённая полезная нагрузка отвергается.
    assert(!parser.decode(9, pressed, sizeof(pressed), state));

    constexpr std::uint8_t signedMap[]{
        0x05,0x01,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7F,
        0x75,0x08,0x95,0x02,0x81,0x02
    };
    assert(parser.parse(signedMap, sizeof(signedMap)));
    const std::uint8_t signedAxes[]{0x81,0x7F};
    state = {};
    assert(parser.decode(0, signedAxes, sizeof(signedAxes), state));
    assert(state.x == -32768 && state.y == 32767);
    constexpr std::uint8_t truncated[]{0x26,0xFF};
    constexpr std::uint8_t overflow[]{0x75,0x20,0x96,0xFF,0xFF,0x81,0x02};
    constexpr std::uint8_t underflow[]{0xB4};
    assert(!parser.parse(truncated, sizeof(truncated)));
    assert(!parser.parse(overflow, sizeof(overflow)));
    assert(!parser.parse(underflow, sizeof(underflow)));

    // Home и Menu — независимые Consumer-поля. Каждое должно запрашивать Guide;
    // позднее нейтральное поле не должно стирать предыдущее активное.
    // Отдельный отчёт геймпада без Consumer-полей сохраняет Guide.
    constexpr std::uint8_t dualGuideMap[]{
        0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x01,
        0x05,0x09,0x19,0x01,0x29,0x02,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x02,0x81,0x02,0x75,0x06,0x95,0x01,0x81,0x03,
        0x85,0x02,0x05,0x0C,0x0A,0x23,0x02,0x09,0x40,
        0x75,0x01,0x95,0x02,0x81,0x02,0x75,0x06,0x95,0x01,0x81,0x03,0xC0
    };
    assert(parser.parse(dualGuideMap, sizeof(dualGuideMap)));
    state = {};
    const std::uint8_t homeOnly[]{1}, menuOnly[]{2}, bothGuide[]{3}, neitherGuide[]{0};
    assert(parser.decode(2, homeOnly, 1, state) && state.guide);
    const std::uint8_t firstButton[]{1}, noButtons[]{0};
    assert(parser.decode(1, firstButton, 1, state) && state.guide && state.buttons == 1);
    assert(parser.decode(2, neitherGuide, 1, state) && !state.guide && state.buttons == 1);
    assert(parser.decode(2, menuOnly, 1, state) && state.guide);
    assert(parser.decode(1, noButtons, 1, state) && state.guide && state.buttons == 0);
    assert(parser.decode(2, bothGuide, 1, state) && state.guide);
    assert(parser.decode(2, neitherGuide, 1, state) && !state.guide);


    // Логический диапазон 32-битного hat может быть гораздо шире восьми направлений.
    // Разности диапазонов вычисляются без знакового переполнения;
    // недопустимая/null-позиция отпускает hat, не меняя другие ID.
    constexpr std::uint8_t wideHatMap[]{
        0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x01,
        0x05,0x09,0x19,0x01,0x29,0x02,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x02,0x81,0x02,0x75,0x06,0x95,0x01,0x81,0x03,
        0x85,0x02,0x05,0x0C,0x0A,0x23,0x02,0x75,0x01,0x95,0x01,0x81,0x02,
        0x75,0x07,0x95,0x01,0x81,0x03,
        0x85,0x03,0x05,0x01,0x09,0x39,
        0x17,0x00,0x00,0x00,0x80,0x27,0xFF,0xFF,0xFF,0x7F,
        0x75,0x20,0x95,0x01,0x81,0x42,0xC0
    };
    assert(parser.parse(wideHatMap, sizeof(wideHatMap)));
    state = {};
    const std::uint8_t twoButtons[]{3};
    assert(parser.decode(1, twoButtons, 1, state) && state.buttons == 3);
    assert(parser.decode(2, homeOnly, 1, state) && state.guide);
    const std::uint8_t wideUp[]{0,0,0,0x80}, wideNorthWest[]{7,0,0,0x80};
    const std::uint8_t wideNull[]{0,0,0,0}, wideOutside[]{8,0,0,0x80}, wideMaximum[]{0xFF,0xFF,0xFF,0x7F};
    assert(parser.decode(3, wideUp, 4, state) && state.directions == 1);
    assert(parser.decode(3, wideNull, 4, state) && state.directions == 0);
    assert(state.buttons == 3 && state.guide);
    assert(parser.decode(3, wideNorthWest, 4, state) && state.directions == 5);
    assert(parser.decode(3, wideOutside, 4, state) && state.directions == 0);
    assert(parser.decode(3, wideMaximum, 4, state) && state.directions == 0);
    assert(state.buttons == 3 && state.guide);

    // Кнопки, оси и Guide могут находиться в независимых Report ID.
    // Для полезной подписки одному ID не обязательно содержать все контролы.
    constexpr std::uint8_t splitMap[]{
        0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x01,
        0x05,0x09,0x19,0x01,0x29,0x02,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x02,0x81,0x02,0x75,0x06,0x95,0x01,0x81,0x03,
        0x85,0x02,0x05,0x01,0x09,0x30,0x09,0x31,
        0x16,0x00,0x80,0x26,0xFF,0x7F,0x75,0x10,0x95,0x02,0x81,0x02,
        0x85,0x03,0x05,0x0C,0x0A,0x23,0x02,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x01,0x81,0x02,0x75,0x07,0x95,0x01,0x81,0x03,0xC0
    };
    assert(parser.parse(splitMap, sizeof(splitMap)));
    const auto buttonsInfo = parser.reportInfo(1), axesInfo = parser.reportInfo(2), homeInfo = parser.reportInfo(3);
    assert(buttonsInfo.nesControls && !buttonsInfo.menuControl && buttonsInfo.declaredBytes == 1);
    assert(axesInfo.nesControls && !axesInfo.menuControl && axesInfo.declaredBytes == 4);
    assert(!homeInfo.nesControls && homeInfo.menuControl && homeInfo.declaredBytes == 1);
    state = {};
    assert(parser.decode(1, firstButton, 1, state));
    const std::uint8_t splitAxes[]{0x00,0x80,0xFF,0x7F}, centeredAxes[]{0,0,0,0};
    assert(parser.decode(2, splitAxes, 4, state) && state.x == -32768 && state.y == 32767 && state.buttons == 1);
    assert(parser.decode(3, homeOnly, 1, state) && state.guide && state.buttons == 1 && state.x == -32768);
    assert(parser.decode(1, noButtons, 1, state) && state.buttons == 0 && state.guide && state.y == 32767);
    assert(parser.decode(2, centeredAxes, 4, state) && state.x == 0 && state.y == 0 && state.guide);
    assert(parser.decode(3, neitherGuide, 1, state) && !state.guide && state.buttons == 0);

    // Необрабатываемый padding в конце всё равно задаёт объявленную длину пакета.
    constexpr std::uint8_t largeInputMap[]{
        0x05,0x09,0x09,0x01,0x15,0x00,0x25,0x01,
        0x75,0x01,0x95,0x01,0x81,0x02,0x75,0x08,0x95,0x81,0x81,0x03
    };
    assert(parser.parse(largeInputMap, sizeof(largeInputMap)));
    const auto largeInfo = parser.reportInfo(0);
    assert(largeInfo.nesControls && largeInfo.requiredBytes == 1 && largeInfo.declaredBytes == 130);
    constexpr std::uint8_t unusedButtonMap[]{0x05,0x09,0x09,0x03,0x75,0x01,0x95,0x01,0x81,0x02};
    assert(parser.parse(unusedButtonMap, sizeof(unusedButtonMap)));
    const auto unusedInfo = parser.reportInfo(0);
    assert(unusedInfo.requiredBytes == 1 && !unusedInfo.nesControls && !unusedInfo.menuControl);
    std::puts("HID parser/reportInfo/multipart tests passed");
}
