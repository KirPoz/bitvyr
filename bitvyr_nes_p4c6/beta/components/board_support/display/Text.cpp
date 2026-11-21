#include "board/Board.h"
#include "Font5x7.h"

namespace nes::platform {
namespace {
/** @brief Bounds только реально записанных glyph-пикселей; сложный текст использует полный fallback. */
struct GameTextDirtyBounds final {
    int left=800,top=480,right=0,bottom=0;bool fallback=false;
    /** @brief Добавляет уже отсечённый пиксель исходного renderer без второго прохода по строке. */
    void include(int x,int y) noexcept {
        if(x<left)left=x;
        if(y<top)top=y;
        if(x+1>right)right=x+1;
        if(y+1>bottom)bottom=y+1;
    }
};

/**
 * @brief Рисует ограниченный текст в меню либо упакованных боковых полосах.
 * @details Нулевые указатели и масштаб вне 1..8 игнорируются. Отсечение предотвращает выход за логические размеры; функция не передаёт кадр PPA.
 * @param frame Доступный для записи RGB565-буфер вызывающего кода.
 * @param controls Выбор упакованных боковых панелей вместо полного кадра меню.
 * @param x Логическая горизонтальная координата в пикселях.
 * @param y Логическая вертикальная координата в пикселях.
 * @param text Заимствованная строка с завершающим нулём; указатель не сохраняется.
 * @param color Цвет RGB565 без передачи владения буфером.
 * @param scale Целый масштаб шрифта 1..8; вне диапазона рисование не выполняется.
 */
void drawTextTo(std::uint16_t* frame,bool controls,int x,int y,const char* text,std::uint16_t color,unsigned scale
    ,GameTextDirtyBounds* dirty=nullptr
) noexcept {
    if (!frame || !text || scale == 0 || scale > 8) {
        if(dirty)dirty->fallback=true;
        return;
    }
    const int origin = x;
    for (; *text; ++text) {
        // Это ограничение оптимизации, а не второй источник растрового алфавита.
        if(dirty&&!((*text>='A'&&*text<='Z')||(*text>='a'&&*text<='z')||
                     (*text>='0'&&*text<='9')||*text==' '||*text=='.'||*text=='-'))dirty->fallback=true;
        if (*text == '\n') { x = origin; y += 9 * scale; continue; }
        for (unsigned row = 0; row < 7; ++row) {
            const auto bits = font5x7::glyph(*text, row);
            for (unsigned column = 0; column < 5; ++column) {
                if (!(bits & (16U >> column))) continue;
                for (unsigned sy = 0; sy < scale; ++sy) {
                    const int py = y + row * scale + sy;
                    if (py < 0 || py >= Board::logicalPanel.height) continue;
                    for (unsigned sx = 0; sx < scale; ++sx) {
                        const int px = x + column * scale + sx;
                        if (px < 0 || px >= Board::logicalPanel.width) continue;
                        if(controls) {
                            if(px<80)frame[py*160+px]=color;
                            else if(px>=720)frame[py*160+px-640]=color;
                            if(dirty&&(px<80||px>=720))dirty->include(px,py);
                        } else frame[py*Board::logicalPanel.width+px]=color;
                    }
                }
            }
        }
        x += 6 * scale;
        if (x >= Board::logicalPanel.width) break;
    }
}
}

/**
 * @brief Рисует текст собственным растровым шрифтом в кадре меню.
 * @details Масштаб 1..8, отсечение по границам 800×480; буфер и ресурс вывода остаются у платы.
 * @param x Логическая горизонтальная координата в пикселях.
 * @param y Логическая вертикальная координата в пикселях.
 * @param text Заимствованная строка с завершающим нулём; указатель не сохраняется.
 * @param color Цвет RGB565 без передачи владения буфером.
 * @param scale Целый масштаб шрифта 1..8; вне диапазона рисование не выполняется.
 */
void Board::drawText(int x,int y,const char* text,std::uint16_t color,unsigned scale) noexcept {
    drawTextTo(framebuffer(),false,x,y,text,color,scale);
}

/**
 * @brief Рисует текст только в боковых игровых панелях.
 * @details Использует логические координаты 800×480, отсекает центр и отмечает физические поля для последующего обновления. Не запускает PPA.
 * @param x Логическая горизонтальная координата в пикселях.
 * @param y Логическая вертикальная координата в пикселях.
 * @param text Заимствованная строка с завершающим нулём; указатель не сохраняется.
 * @param color Цвет RGB565 без передачи владения буфером.
 * @param scale Целый масштаб шрифта 1..8; вне диапазона рисование не выполняется.
 */
void Board::drawGameText(int x,int y,const char* text,std::uint16_t color,unsigned scale) noexcept {
    GameTextDirtyBounds dirty{};
    drawTextTo(gameControlsFramebuffer(),true,x,y,text,color,scale,&dirty);
    if(dirty.fallback)invalidateGameBars();
    else if(dirty.left<dirty.right&&dirty.top<dirty.bottom)
        invalidateGameBars(dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top);
}

} // пространство имён nes::platform
