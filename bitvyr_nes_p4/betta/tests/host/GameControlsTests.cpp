/**
 * @file GameControlsTests.cpp
 * @brief Проверки общей сенсорной геометрии и настоящего chrome-painter без SDK/PPA.
 */
#include "../../app/nes/graphics/GameChrome.h"
#include "../../components/board_support/display/Font5x7.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace controls=nes::app::gameControls;
namespace chrome=nes::app::gameChrome;
namespace core=nes::core;

/** @brief Native-панели с guard-словами и реальными глифами Board, без аппаратного вывода. */
struct BoardFake {
    std::array<std::uint16_t,160*480+32> guarded{};
    std::vector<std::string> labels;
    unsigned invalidations=0;
    bool available=true;
    /** @brief Сохраняет guard-слова вокруг единственного тестового160×480 выделения. */
    BoardFake(){guarded.fill(0xdead);}
    /** @brief Возвращает только внутренний участок панелей либо моделирует отсутствие хранилища. */
    std::uint16_t* gameControlsFramebuffer() noexcept {return available?guarded.data()+16:nullptr;}
    /** @brief Учитывает dirty-публикацию, не создавая PPA/LCD completion. */
    void invalidateGameBars() noexcept {++invalidations;}
    /**
     * @brief Растеризует тот же Font5x7, который использует настоящий Text.cpp.
     * @param x Логическая горизонтальная координата0..799; центр не записывается.
     * @param y Логическая вертикальная координата0..479.
     * @param text Заимствованная ASCII-подпись.
     * @param colour Цвет RGB565.
     * @param scale Масштаб реального шрифта1..8.
     */
    void drawGameText(int x,int y,const char* text,std::uint16_t colour,unsigned scale) noexcept {
        if(!available||!text||scale==0||scale>8)return;
        labels.emplace_back(text);const int origin=x;
        for(;*text;++text){
            if(*text=='\n'){x=origin;y+=int(9*scale);continue;}
            for(unsigned row=0;row<7;++row){
                const auto bits=nes::platform::font5x7::glyph(*text,row);
                for(unsigned column=0;column<5;++column)if(bits&(16U>>column))
                    for(unsigned sy=0;sy<scale;++sy)for(unsigned sx=0;sx<scale;++sx)
                        chrome::pixel(gameControlsFramebuffer(),x+int(column*scale+sx),y+int(row*scale+sy),colour);
            }
            x+=int(6*scale);if(x>=800)break;
        }
    }
    /** @brief Подтверждает отсутствие записей за пределами тестового выделения. */
    void guards()const {for(unsigned i=0;i<16;++i)assert(guarded[i]==0xdead&&guarded[160*480+16+i]==0xdead);}
    /** @brief Проверяет, выдавалась ли точная ожидаемая подпись общим painter. */
    bool hasLabel(const char* value)const {for(const auto&s:labels)if(s==value)return true;return false;}
};

/** @brief Проверяет пересечение двух непустых фиксированных UI-прямоугольников. */
constexpr bool overlaps(const controls::Box&a,const controls::Box&b) {
    return a.x<b.x+b.width&&b.x<a.x+a.width&&a.y<b.y+b.height&&b.y<a.y+a.height;
}
/** @brief Проверяет, что изменение пикселя относится только к изменившимся NES-кнопкам. */
bool changedButtonAt(unsigned x,unsigned y,std::uint8_t changed) {
    for(const auto&b:controls::buttons)if((b.mask&changed)&&b.area.contains(x,y))return true;
    return false;
}
/** @brief Возвращает видимый RGB565-пиксель из упакованных боковых полос. */
std::uint16_t at(const BoardFake&b,unsigned x,unsigned y) {return b.guarded[16+y*160+controls::packedX(x)];}

/** @brief Проверяет границы, равенство размеров внутри групп и точное соответствие hit-test. */
void geometry() {
    for(unsigned i=0;i<8;++i){
        const auto&b=controls::buttons[i];assert(b.area.width==72&&b.area.height>=52);
        assert(b.area.x==4||b.area.x==724);assert(b.area.y>=0&&b.area.y+b.area.height<=480);
        assert(!overlaps(b.area,controls::exit)&&!overlaps(b.area,controls::pause)&&!overlaps(b.area,controls::fps));
        const auto hit=controls::hit(unsigned(b.area.x+b.area.width/2),unsigned(b.area.y+b.area.height/2));
        assert(hit.buttons==b.mask&&hit.action==controls::Action::None);
        assert(controls::hit(unsigned(b.area.x),unsigned(b.area.y)).buttons==b.mask);
        assert(!b.area.contains(unsigned(b.area.x+b.area.width),unsigned(b.area.y)));
        assert(!b.area.contains(unsigned(b.area.x),unsigned(b.area.y+b.area.height)));
        if(controls::isDirection(b.mask))assert(b.area.height==64&&b.label[0]==0);
        else assert(chrome::textWidth(b.label,b.scale)<=b.area.width-8);
        for(unsigned j=i+1;j<8;++j)assert(!overlaps(b.area,controls::buttons[j].area));
    }
    assert(controls::buttons[6].area.height==controls::buttons[7].area.height);
    assert(controls::buttons[4].area.height==controls::buttons[5].area.height);
    assert(controls::exit.height==controls::pause.height);
    for(unsigned y=0;y<480;++y)for(unsigned x=80;x<720;++x){
        const auto h=controls::hit(x,y);assert(!h.buttons&&h.action==controls::Action::None);
    }
    assert(!controls::hit(800,20).buttons&&!controls::hit(40,480).buttons);
    assert(controls::hit(40,30).action==controls::Action::ExitLibrary);
    assert(controls::hit(760,30).action==controls::Action::Settings);
    // То же OR нескольких контактов, что Main применяет до machine_.setController.
    const auto diagonal=controls::hit(40,200).buttons|controls::hit(40,344).buttons|controls::hit(760,392).buttons;
    assert(diagonal==(core::Up|core::Right|core::A));
    assert((controls::hit(760,264).buttons|controls::hit(760,392).buttons)==(core::A|core::B));
}

/** @brief Проверяет изоляцию viewport, направления стрелок, восстановление кнопок и FPS-only запись. */
void painting() {
    BoardFake b;assert(chrome::draw(b,0,true,0,false));b.guards();
    assert(b.hasLabel("LIBRARY")&&b.hasLabel("MENU")&&b.hasLabel("SEL")&&b.hasLabel("START"));
    assert(b.hasLabel("FPS")&&b.hasLabel("--")&&!b.hasLabel("UP")&&!b.hasLabel("DOWN"));
    assert(at(b,52,197)==chrome::ink&&at(b,52,203)!=chrome::ink);
    assert(at(b,37,284)==chrome::ink&&at(b,43,284)!=chrome::ink);
    const auto before=b.guarded;const auto mask=std::uint8_t(core::Up|core::Right|core::A);
    assert(chrome::updateButtons(b,0,mask));b.guards();
    for(unsigned y=0;y<480;++y)for(unsigned px=0;px<160;++px){const unsigned x=px<80?px:px+640;
        if(!changedButtonAt(x,y,mask))assert(before[16+y*160+px]==at(b,x,y));
    }
    const auto held=b.guarded;const auto dirty=b.invalidations;
    assert(!chrome::updateButtons(b,mask,mask)&&held==b.guarded&&dirty==b.invalidations);
    assert(chrome::updateButtons(b,mask,0)&&before==b.guarded);
    chrome::drawFps(b,true,59.8,true);assert(b.hasLabel("59.8"));
    for(unsigned y=0;y<480;++y)for(unsigned px=0;px<160;++px){const unsigned x=px<80?px:px+640;
        if(!controls::fps.contains(x,y))assert(before[16+y*160+px]==at(b,x,y));
    }
    chrome::drawFps(b,false,59.8,true);
    for(unsigned y=80;y<144;++y)for(unsigned x=4;x<76;++x)assert(at(b,x,y)==chrome::background);
    b.guards();
    BoardFake absent;absent.available=false;
    assert(!chrome::draw(absent,0,true,0,false)&&!chrome::updateButtons(absent,0,core::A));
    chrome::drawFps(absent,true,60,true);assert(absent.invalidations==0&&absent.labels.empty());absent.guards();
}

/** @brief Проверяет ограниченное форматирование, включая неизвестное/нечисловое значение. */
void fpsFormatting() {
    char value[6]{};chrome::formatFps(value,59.8,true);assert(!std::strcmp(value,"59.8"));
    chrome::formatFps(value,0,true);assert(!std::strcmp(value,"0.0"));
    chrome::formatFps(value,999.9,true);assert(!std::strcmp(value,"999.9"));
    for(double invalid:{-1.0,1000.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
        chrome::formatFps(value,invalid,true);assert(!std::strcmp(value,"--"));
    }
    chrome::formatFps(value,60,false);assert(!std::strcmp(value,"--"));
}

/**
 * @brief Записывает необязательный800×480 preview с настоящими Board-глифами.
 * @param path Путь нового PPM-артефакта; SD и устройство не используются.
 * @param mask Демонстрируемая одновременно нажатая NES-маска.
 * @param fpsVisible Показать или очистить FPS.
 */
void preview(const std::filesystem::path&path,std::uint8_t mask,bool fpsVisible) {
    BoardFake b;assert(chrome::draw(b,mask,fpsVisible,59.8,true));
    std::ofstream out(path,std::ios::binary);assert(out);out<<"P6\n800 480\n255\n";
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<800;++x){
        const std::uint16_t rgb=x<80||x>=720?at(b,x,y):((x/32+y/32)&1?0x18e3:0x2104);
        const unsigned char bytes[]{static_cast<unsigned char>(((rgb>>11)&31)*255/31),static_cast<unsigned char>(((rgb>>5)&63)*255/63),static_cast<unsigned char>((rgb&31)*255/31)};
        out.write(reinterpret_cast<const char*>(bytes),3);
    }
    assert(out.good());
}
} // локальные native-проверки

/** @brief Запускает чистые UI-проверки; необязательный аргумент задаёт только каталог preview. */
int main(int argc,char**argv) {
    geometry();painting();fpsFormatting();
    if(argc==2){std::filesystem::create_directories(argv[1]);const std::filesystem::path dir=argv[1];
        preview(dir/"controls-default.ppm",0,true);preview(dir/"controls-pressed.ppm",core::Up|core::Right|core::A,true);preview(dir/"controls-fps-off.ppm",0,false);
    }
}
