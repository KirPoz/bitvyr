/**
 * @file GameChrome.h
 * @brief Общий Main-painter боковых панелей: одинаковые стили, стрелки, нажатия и FPS.
 * @details Не обращается к Machine, RTOS, PPA или времени. BoardLike предоставляет
 * gameControlsFramebuffer(), drawGameText(), invalidateGameBars() и прямоугольный
 * invalidateGameBars(x,y,width,height). Caller владеет
 * панелями и соблюдает аппаратные fences; центральный ROM-viewport не записывается.
 */
#pragma once
#include "GameControls.h"
#include <algorithm>
#include <cstdint>
#include <initializer_list>

namespace nes::app::gameChrome {
inline constexpr std::uint16_t background=0x0841,buttonFill=0x18c5,buttonBorder=0x5aef;
inline constexpr std::uint16_t pressedFill=0x0451,pressedBorder=0xafff,ink=0xffff,mutedInk=0xad55;
inline constexpr auto button=buttonFill,border=buttonBorder,pressed=pressedFill,text=ink,mutedText=mutedInk;

/**
 * @brief Записывает один логический пиксель только в одну из боковых полос.
 * @param pixels Заимствованный160×480 RGB565-буфер; nullptr ничего не меняет.
 * @param x Логический x0..799; центр80..719 всегда пропускается.
 * @param y Логический y0..479; вне панели запись пропускается.
 * @param colour Цвет RGB565 без передачи владения.
 */
inline void pixel(std::uint16_t* pixels,int x,int y,std::uint16_t colour) noexcept {
    if(pixels&&y>=0&&y<480&&x>=0&&x<800&&(x<80||x>=720))
        pixels[unsigned(y)*160+gameControls::packedX(unsigned(x))]=colour;
}

/**
 * @brief Заливает отсечённый прямоугольник панелей без доступа к ROM-пикселям.
 * @param pixels Заимствованный буфер боковых полос160×480.
 * @param box Фиксированная UI-геометрия в логических пикселях800×480.
 * @param colour Цвет заливки RGB565.
 */
inline void fill(std::uint16_t* pixels,const gameControls::Box& box,std::uint16_t colour) noexcept {
    if(!pixels||box.width<=0||box.height<=0)return;
    for(int y=std::max(0,box.y);y<std::min(480,box.y+box.height);++y)
        for(int x=std::max(0,box.x);x<std::min(800,box.x+box.width);++x)pixel(pixels,x,y,colour);
}

/**
 * @brief Рисует одинаковую двухпиксельную рамку кнопки и её активную заливку.
 * @param pixels Заимствованные боковые RGB565-полосы.
 * @param area Прямоугольник текущего элемента.
 * @param pressed Реальное принятое UI-состояние; не изменяет NES-ввод.
 */
inline void box(std::uint16_t* pixels,const gameControls::Box& area,bool pressed) noexcept {
    fill(pixels,area,pressed?pressedBorder:buttonBorder);
    fill(pixels,{area.x+2,area.y+2,area.width-4,area.height-4},pressed?pressedFill:buttonFill);
}

/**
 * @brief Возвращает ширину ограниченной ASCII-подписи шрифта5×7.
 * @param text Заимствованная NUL-строка либо nullptr.
 * @param scale Целочисленный масштаб собственного шрифта.
 * @return Ширина видимых пятибитных глифов с шестипиксельным шагом.
 */
inline int textWidth(const char* text,unsigned scale) noexcept {
    unsigned count=0;if(text)while(text[count]&&count<32)++count;
    return count?int((count*6-1)*scale):0;
}

/**
 * @brief Центрирует подпись внутри кнопки через настоящий Board-текстовый интерфейс.
 * @param board Main-адаптер либо native fake с тем же текстовым контрактом.
 * @param area Граница элемента в логических координатах.
 * @param text Короткая заимствованная ASCII-подпись.
 * @param scale Масштаб глифа1..8; передаются только фиксированные UI-значения.
 */
template<class BoardLike>
void label(BoardLike& board,const gameControls::Box& area,const char* text,unsigned scale) noexcept {
    board.drawGameText(area.x+(area.width-textWidth(text,scale))/2,
        area.y+(area.height-int(7*scale))/2,text,ink,scale);
}

/**
 * @brief Рисует толстую33×33 стрелку без зависимости от Unicode-шрифта.
 * @param pixels Заимствованные боковые полосы.
 * @param area Одна из равных72×64 кнопок направления.
 * @param direction Одиночная NES-маска Up/Down/Left/Right.
 */
inline void arrow(std::uint16_t* pixels,const gameControls::Box& area,std::uint8_t direction) noexcept {
    const int cx=area.x+area.width/2,cy=area.y+area.height/2;
    for(int dy=-16;dy<=16;++dy)for(int dx=-16;dx<=16;++dx){
        int u=dx,v=dy;
        if(direction==core::Down)v=-dy;
        else if(direction==core::Left){u=dy;v=dx;}
        else if(direction==core::Right){u=dy;v=-dx;}
        const int magnitude=u<0?-u:u;
        if((v>=-16&&v<=0&&magnitude<=v+16)||(v>=0&&v<=16&&magnitude<=4))
            pixel(pixels,cx+dx,cy+dy,ink);
    }
}

/**
 * @brief Рисует одну NES-кнопку в общем стиле и отражает её нажатие.
 * @param board Исключительно доступный Main-адаптер панелей.
 * @param button Общая геометрия/маска GameControls; hit-test использует ту же запись.
 * @param pressed Принятое состояние нажатия этой кнопки.
 */
template<class BoardLike>
void paintButton(BoardLike& board,const gameControls::Button& button,bool pressed) noexcept {
    auto* pixels=board.gameControlsFramebuffer();if(!pixels)return;
    box(pixels,button.area,pressed);
    if(gameControls::isDirection(button.mask))arrow(pixels,button.area,button.mask);
    else label(board,button.area,button.label,button.scale);
}

/**
 * @brief Обновляет только изменившиеся NES-кнопки, без покадровой полной перерисовки.
 * @param board Main-адаптер; callback/Worker сюда не передаются.
 * @param before Ранее нарисованная NES-маска.
 * @param after Текущая принятая маска; несколько битов подсвечиваются одновременно.
 * @return true, если изменившиеся кнопки были нарисованы; false при том же mask/нет памяти.
 */
template<class BoardLike>
bool updateButtons(BoardLike& board,std::uint8_t before,std::uint8_t after) noexcept {
    if(!board.gameControlsFramebuffer()||before==after)return false;
    bool changed=false;
    for(const auto& button:gameControls::buttons)if((before^after)&button.mask){
        paintButton(board,button,(after&button.mask)!=0);
        const auto& area=button.area;
        board.invalidateGameBars(area.x,area.y,area.width,area.height);changed=true;
    }
    return changed;
}

/**
 * @brief Обновляет зарезервированную FPS-область готовой строкой Main-счётчика.
 * @param board Исключительно доступный Main-адаптер боковых полос.
 * @param value Заимствованная ASCII-строка до5 символов; nullptr показывает "--".
 * @param visible Пользовательская настройка; false только очищает прежнее значение.
 * @details Здесь нет чтения CPU/PPU или вычисления FPS; caller различает
 * принятые кадры, software submissions и физический VSYNC.
 */
template<class BoardLike>
void paintFps(BoardLike& board,const char* value,bool visible) noexcept {
    auto* pixels=board.gameControlsFramebuffer();if(!pixels)return;
    fill(pixels,gameControls::fps,background);
    if(visible){
        char text[6]{};const char* source=value?value:"--";
        for(unsigned i=0;i<5&&source[i];++i)text[i]=source[i];
        label(board,{4,82,72,16},"FPS",1);
        label(board,{4,101,72,30},text,2);
    }
    const auto& area=gameControls::fps;
    board.invalidateGameBars(area.x,area.y,area.width,area.height);
}

/**
 * @brief Рисует библиотеку и меню одинаковыми системными кнопками с пиктограммами.
 * @param board Main-адаптер; подписи используют существующий5×7 шрифт.
 */
template<class BoardLike>
void paintSystem(BoardLike& board) noexcept {
    auto* p=board.gameControlsFramebuffer();if(!p)return;
    box(p,gameControls::exit,false);box(p,gameControls::pause,false);
    // Книги и три строки меню читаются даже при мелкой вспомогательной подписи.
    fill(p,{25,20,7,22},ink);fill(p,{37,17,7,25},ink);fill(p,{49,21,7,21},ink);
    for(int y:{20,29,38})fill(p,{746,y,28,4},ink);
    label(board,{4,47,72,10},"LIBRARY",1);label(board,{724,47,72,10},"MENU",1);
}

/**
 * @brief Готовит одинаковые панели для обычного, Frame и Coupled-видеотрактов.
 * @param board Main-адаптер с отдельно выделенными боковыми полосами.
 * @param pressed Начальная маска подсветки; обычно0 после границы игры/возобновления.
 * @param fpsText Уже рассчитанное значение Main-счётчика либо nullptr для "--".
 * @param showFps Пользовательская видимость FPS.
 * @return false только при отсутствии хранилища панелей; память не выделяется.
 */
template<class BoardLike>
bool paint(BoardLike& board,std::uint8_t pressed=0,const char* fpsText=nullptr,bool showFps=true) noexcept {
    auto* pixels=board.gameControlsFramebuffer();if(!pixels)return false;
    std::fill_n(pixels,gameControls::chromeWidth*gameControls::chromeHeight,background);
    paintSystem(board);
    for(const auto& button:gameControls::buttons)paintButton(board,button,(pressed&button.mask)!=0);
    paintFps(board,fpsText,showFps);board.invalidateGameBars();return true;
}
/**
 * @brief Форматирует короткое завершённое FPS-значение либо отсутствие измерения.
 * @param value Выход ровно6 байт: до5 видимых ASCII-символов и NUL.
 * @param fps Измеренная частота завершённых software submissions, не период CPU-кернела.
 * @param valid Разрешение использовать значение; NaN/отрицательное/>999.9 также дают "--".
 */
inline void formatFps(char (&value)[6],double fps,bool valid) noexcept {
    value[0]='-';value[1]='-';value[2]=0;
    if(!valid||!(fps>=0.0&&fps<=999.9))return;
    // Только UI-округление до десятых: без тяжёлого float-printf на Main-стеке.
    const unsigned tenths=static_cast<unsigned>(fps*10.0+0.5),whole=tenths/10;
    unsigned out=0;
    if(whole>=100)value[out++]=char('0'+whole/100);
    if(whole>=10)value[out++]=char('0'+(whole/10)%10);
    value[out++]=char('0'+whole%10);value[out++]='.';
    value[out++]=char('0'+tenths%10);value[out]=0;
}
/**
 * @brief Перерисовывает только FPS-блок из готовой Main-метрики.
 * @param board Исключительно доступный Main-адаптер боковых полос.
 * @param showFps Пользовательская видимость; false удаляет прежнее значение.
 * @param fps Частота завершённых software submissions, предоставленная владельцем счётчика.
 * @param valid true только для законченного допустимого измерительного окна.
 */
template<class BoardLike>
void drawFps(BoardLike& board,bool showFps,double fps,bool valid) noexcept {
    char value[6]{};formatFps(value,fps,valid);paintFps(board,value,showFps);
}
/**
 * @brief Строит общую игровую рамку без привязки к способу исполнения кадра.
 * @param board Main-адаптер отдельно выделенных боковых полос.
 * @param pressedMask Принятая NES-маска для начальной подсветки, обычно0.
 * @param showFps Пользовательская видимость FPS.
 * @param fps Уже рассчитанная частота завершённых software submissions.
 * @param valid Признак законченного измерительного окна; иначе показывается "--".
 * @return false при отсутствии хранилища панелей; аллокации и аппаратный submit не выполняются.
 */
template<class BoardLike>
bool draw(BoardLike& board,std::uint8_t pressedMask,bool showFps,double fps,bool valid) noexcept {
    char value[6]{};formatFps(value,fps,valid);return paint(board,pressedMask,value,showFps);
}
} // пространство имён nes::app::gameChrome
