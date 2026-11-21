#pragma once
#include "GameControls.h"
#include <cstdint>

namespace nes::platform {
/** @brief Полуоткрытая область одной80×480 панели; нулевой размер означает отсутствие долга. */
struct GameBarsDirtyRect final {
    std::uint16_t left=0,top=0,right=0,bottom=0;
    /** @brief Проверяет наличие пикселей, ещё не скопированных в данный native-буфер. */
    bool empty() const noexcept {return left>=right||top>=bottom;}
    /**
     * @brief Объединяет отсечённую область с прежним долгом, не теряя более старые изменения.
     * @param x0,y0 Начало в пикселях одной панели; граница включена.
     * @param x1,y1 Конец в пикселях одной панели; граница исключена.
     * @pre Caller уже ограничил область размером80×480; пустая область ничего не меняет.
     */
    void include(unsigned x0,unsigned y0,unsigned x1,unsigned y1) noexcept {
        if(x0>=x1||y0>=y1)return;
        if(empty()){left=std::uint16_t(x0);top=std::uint16_t(y0);right=std::uint16_t(x1);bottom=std::uint16_t(y1);return;}
        if(x0<left)left=std::uint16_t(x0);
        if(y0<top)top=std::uint16_t(y0);
        if(x1>right)right=std::uint16_t(x1);
        if(y1>bottom)bottom=std::uint16_t(y1);
    }
};

/**
 * @brief Main-only sidecar трёх native-буферов, не меняющий BoardState или кадровые ресурсы.
 * @details Owner служит только скалярным ключом и не разыменовывается. Вызовы допускаются
 * после прежних Main/LCD/PPA guards; ISR и Worker хранилище не наблюдают.
 */
struct alignas(16) GameBarsDirtyState final {
    const void* owner=nullptr;
    GameBarsDirtyRect rects[3][2]{};
    /** @brief Помечает выбранный native полностью неизвестным после меню либо смены владельца. */
    void unknown(unsigned index) noexcept {
        if(index>=3)return;
        rects[index][0]={0,0,80,480};rects[index][1]={0,0,80,480};
    }
    /**
     * @brief Связывает долговечный Main owner; новый ключ требует полной копии всех полей.
     * @param value Заимствованный адрес BoardState, используемый только как идентификатор.
     * @return true только для прежнего ненулевого owner; false помечает все панели неизвестными.
     */
    bool bind(const void* value) noexcept {
        if(value&&owner==value)return true;
        owner=value;
        for(unsigned i=0;i<3;++i)unknown(i);
        return false;
    }
    /** @brief Сохраняет прежний полный invalidate для всех трёх native-буферов. */
    void full(const void* value) noexcept {
        bind(value);for(unsigned i=0;i<3;++i)unknown(i);
    }
    /**
     * @brief Добавляет логический800×480 прямоугольник в долг обоих полей каждого native.
     * @param value Неизменный ключ текущего BoardState, только Main.
     * @param x,y Начало в логических пикселях; отрицательные координаты отсекаются.
     * @param width,height Размер; неположительные значения ничего не меняют.
     * @return true только при пересечении хотя бы одной боковой полосы.
     */
    bool mark(const void* value,int x,int y,int width,int height) noexcept {
        bind(value);
        if(width<=0||height<=0)return false;
        const std::int64_t endX=std::int64_t(x)+width,endY=std::int64_t(y)+height;
        const auto y0=y<0?0:y;const auto y1=endY>480?480:endY;
        if(y0>=y1||y0>=480||y1<=0)return false;
        bool changed=false;
        for(unsigned bar=0;bar<2;++bar){
            const int origin=bar?720:0;
            const auto x0=x<origin?origin:x;
            const auto x1=endX>origin+80?origin+80:endX;
            if(x0>=x1||x0>=origin+80||x1<=origin)continue;
            for(unsigned i=0;i<3;++i)rects[i][bar].include(unsigned(x0-origin),unsigned(y0),unsigned(x1-origin),unsigned(y1));
            changed=true;
        }
        return changed;
    }
    /**
     * @brief Копирует накопленные области270° после прежнего аппаратного разрешения записи.
     * @details Неизвестный owner/index либо пустой долг при false gameBarsCleared вызывает
     * прежнюю полную копию; cache-mSync и gameBarsCleared остаются у prepareFrame.
     * @param value Ключ текущего Main-владельца без разыменования.
     * @param index Проверенный прежним кодом индекс native-буфера.
     * @param controls Заимствованные160×480 RGB565-полосы без центральной игры.
     * @param output Заимствованный native480×800 после исходного LCD/PPA fence.
     * @pre Оба RGB565-диапазона доступны целиком и живут до возврата; nullptr недопустим.
     * При пересечении диапазонов сохраняется порядок прежней полной скалярной копии.
     */
    void copy(const void* value,unsigned index,const std::uint16_t* controls,std::uint16_t* output) noexcept {
        const bool known=bind(value);
        // Только проверка раздельности диапазонов; uintptr subtraction не переполняет концы.
        const auto sourceAddress=reinterpret_cast<std::uintptr_t>(controls);
        const auto outputAddress=reinterpret_cast<std::uintptr_t>(output);
        const bool disjoint=sourceAddress<=outputAddress?outputAddress-sourceAddress>=160U*480U*2U:
                                                        sourceAddress-outputAddress>=800U*480U*2U;
        if(index>=3||!known||!disjoint||(rects[index][0].empty()&&rects[index][1].empty())){
            copyGameControlsToNative(controls,output);
        }else{
            for(unsigned bar=0;bar<2;++bar){
                const auto area=rects[index][bar];
                for(unsigned y=area.top;y<area.bottom;++y){
                    const auto* row=controls+y*160+bar*80;
                    for(unsigned x=area.left;x<area.right;++x)output[(bar*720+x)*480+479-y]=row[x];
                }
            }
        }
        if(index<3){rects[index][0]={};rects[index][1]={};}
    }
};
static_assert(sizeof(GameBarsDirtyRect)==8,"Два прямоугольника на три native требуют48Б");
static_assert(sizeof(GameBarsDirtyState)==64,"Main-only sidecar ограничен64Б в OFF и ON");
/** @brief Единственный долговечный Main-only sidecar; определён в Board.cpp в обоих режимах. */
extern GameBarsDirtyState gameBarsDirtyState;
} // пространство имён nes::platform
