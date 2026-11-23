/**
 * @file PpuRgbOutput.cpp
 * @brief Fused RGB565-выход, таблица пар и точное восстановление строкового emphasis.
 * @ingroup nes_ppu
 */
#include "PpuRgbOutput.h"
#include "PpuColourReplay.h"
#include "../Palette.h"
#include <atomic>
#include <cstring>
#include <limits>

namespace nes::core {
/**
 * @brief Доводит отложенные RGB-хвосты и перекраску до наблюдаемой границы.
 * @details Это граница ядра, а не PPA/LCD fence; аппаратные читатели буфера управляются приложением.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void Ppu::fenceRgbOutput() const noexcept { if(auto* rgb=PpuRgbOutput::activeFor(this))rgb->fence(*this); }
namespace {
std::atomic<const Ppu*> rgbOwner{nullptr};
std::atomic<PpuRgbOutput*> rgbContext{nullptr};
std::atomic_flag configuration=ATOMIC_FLAG_INIT;
/**
 * @brief Проверенный полуоткрытый адресный диапазон [first,last) без переполнения uintptr_t; не владеет памятью.
 * @ingroup nes_ppu
 */
struct Span {std::uintptr_t first,last;};
/**
 * @brief Проверяет ненулевой диапазон памяти и отсутствие переполнения его конца.
 * @details Не проверяет физический тип памяти; этот уровень ядра не зависит от ESP-IDF capabilities.
 * @param p Начало непустого заимствованного диапазона; адрес проверяется на переполнение с n.
 * @param n Размер диапазона в байтах;0 и переполнение uintptr_t отвергаются.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param out Выходное хранилище результата; при успехе заполнено полностью, исходный источник не усваивается.
 */
bool span(const void* p,std::size_t n,Span& out)noexcept{
    const auto b=reinterpret_cast<std::uintptr_t>(p);
    if(!p||!n||n>std::numeric_limits<std::uintptr_t>::max()-b)return false;
    out={b,b+n};return true;
}
/**
 * @brief Проверяет пересечение диапазонов с защитой от переполнения адресной арифметики.
 * @details Нулевые и недопустимые диапазоны не считаются доказательством безопасного заимствования.
 * @param a Первый предварительно проверенный полуоткрытый адресный диапазон.
 * @param b Второй предварительно проверенный полуоткрытый адресный диапазон.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool overlap(Span a,Span b)noexcept{return a.first<b.last&&b.first<a.last;}
}
/**
 * @brief Возвращает RGB-контекст только для точной идентичности PPU.
 * @details Проверка атомарного владельца предшествует разыменованию контекста другого актора; lookup не даёт права на конкурентный teardown.
 * @param p Тот же PPU, которому принадлежат текущие конвейер и привязка.
 * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
 */
PpuRgbOutput* PpuRgbOutput::activeFor(const Ppu* p)noexcept{
    // Чужие акторы сравнивают атомарную идентичность без обращения к context-
    // хранилищу, которое настоящий владелец может параллельно отвязать и освободить.
    if(rgbOwner.load(std::memory_order_acquire)!=p)return nullptr;
    return rgbContext.load(std::memory_order_relaxed);
}
/**
 * @brief Завершает соответствующую привязку после исключения активного использования.
 * @details Не освобождает память вызывающего и не заменяет ожидание фактического окончания другой стороны.
 * @param p Тот же PPU, которому принадлежат текущие конвейер и привязка.
 */
void PpuRgbOutput::detachOwner(const Ppu* p)noexcept{
    if(auto* c=activeFor(p))c->unbind();
}
/**
 * @brief Привязывает заимствованный ресурс или временного получателя к текущему владельцу.
 * @details Точные ограничения размера, фазового среза и неперекрытия определяются этим типом; произвольное освобождение ресурса во время использования недопустимо.
 * @param rgb Буфер RGB565 вызывающего с согласованным сроком жизни и исключёнными аппаратными читателями.
 * @param words Число доступных элементов uint16_t, а не число байтов.
 * @param table Отдельная таблица RGB-пар, сохраняемая на весь срок привязки.
 * @param pairs Число доступных элементов таблицы Pair.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param replay Постоянный актор, из которого берутся точный выбранный PPU и ROM-lease; находится в простое при bind.
 */
bool PpuRgbOutput::bind(PpuColourReplay& replay,std::uint16_t* rgb,std::size_t words,Pair* table,std::size_t pairs)noexcept{
    if(owner_||configuration.test_and_set(std::memory_order_acquire)){error_=Error::Busy;return false;}
    /**
     * @brief Сохраняет ошибку RGB-bind и release-снимает configuration-lock до возврата false; ресурсы нового владельца не публикуются.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto fail=[&](Error e)noexcept{error_=e;configuration.clear(std::memory_order_release);return false;};
    if(rgbOwner.load(std::memory_order_acquire))return fail(Error::Busy);
    auto& m=replay.machine_;auto& p=replay.targetPpu();
    if(!replay.bound_||replay.error_!=PpuColourReplay::Error::None||replay.cycle_!=7||
       p.frames_!=0||p.scanline_!=-1||p.dot_>(p.pal_?22U:21U)||p.pendingPixels_||!replay.sourceImage_||!replay.sourceBytes_)
        return fail(Error::Boundary);
    if(!p.pixels_.data()||p.pixels_.rowOnly()!=replay.rgbOnly_)return fail(Error::Pixels);
    if(words<pixelCount||pairs<pairCount||!rgb||!table)return fail(Error::Capacity);
    if((reinterpret_cast<std::uintptr_t>(rgb)&3U)||(reinterpret_cast<std::uintptr_t>(table)&3U))return fail(Error::Alignment);
    Span output,arena,indexed,context,image,actor,target,port;
    if(!span(rgb,pixelCount*sizeof(*rgb),output)||!span(table,pairCount*sizeof(*table),arena)||
       !span(p.pixels_.data(),p.pixels_.size(),indexed)||!span(this,sizeof(*this),context)||
       !span(replay.sourceImage_,replay.sourceBytes_,image)||!span(&m,sizeof(m),actor)||!span(&p,sizeof(p),target)||!span(&replay,sizeof(replay),port))return fail(Error::Alias);
    const Span forbidden[]={indexed,context,image,actor,target,port};
    if(overlap(output,arena))return fail(Error::Alias);
    for(auto s:forbidden)if(overlap(output,s)||overlap(arena,s))return fail(Error::Alias);
    for(auto s:{indexed,image,actor,target,port})if(overlap(context,s))return fail(Error::Alias);
    owner_=&p;replay_=&replay;sources_[0]=rgb;sources_[1]=nullptr;rgb_=rgb;table_=table;error_=Error::None;counts_={};
    initialise(p);
    rgbContext.store(this,std::memory_order_relaxed);rgbOwner.store(&p,std::memory_order_release);
    if(replay.rgbOnly_)replay.rgbOnlyReady_=true;
    configuration.clear(std::memory_order_release);return true;
}
/**
 * @brief Завершает соответствующую привязку после исключения активного использования.
 * @details Не освобождает память вызывающего и не заменяет ожидание фактического окончания другой стороны.
 */
void PpuRgbOutput::unbind()noexcept{
    if(!owner_)return;
    while(configuration.test_and_set(std::memory_order_acquire)){}
    if(rgbOwner.load(std::memory_order_relaxed)==owner_&&rgbContext.load(std::memory_order_relaxed)==this){
        rgbOwner.store(nullptr,std::memory_order_release);rgbContext.store(nullptr,std::memory_order_relaxed);
    }
    if(replay_->rgbOnly_)replay_->rgbOnlyReady_=false;
    owner_=nullptr;replay_=nullptr;sources_[0]=sources_[1]=nullptr;rgb_=nullptr;table_=nullptr;row_=240;validRows_=0;dirty_=false;
    configuration.clear(std::memory_order_release);
}
/** @brief Проверяет второй диапазон на reset7; не читает и не заполняет его пиксели. */
bool PpuRgbOutput::registerAlternate(PpuColourReplay&replay,std::uint16_t*rgb,std::size_t words)noexcept{
    if(!owner_||replay_!=&replay||sources_[1]||!replay.bound_||replay.error_!=PpuColourReplay::Error::None||
       replay.cycle_!=7||owner_->frames_||owner_->scanline_!=-1||owner_->pendingPixels_||dirty_)return false;
    if(words<pixelCount||!rgb||(reinterpret_cast<std::uintptr_t>(rgb)&3U))return false;
    Span output,first,arena,indexed,context,image,actor,target,port;
    if(!span(rgb,pixelCount*sizeof(*rgb),output)||!span(sources_[0],pixelCount*sizeof(*rgb),first)||
       !span(table_,pairCount*sizeof(*table_),arena)||!span(owner_->pixels_.data(),owner_->pixels_.size(),indexed)||
       !span(this,sizeof(*this),context)||!span(replay.sourceImage_,replay.sourceBytes_,image)||
       !span(&replay.machine_,sizeof(replay.machine_),actor)||!span(owner_,sizeof(*owner_),target)||
       !span(&replay,sizeof(replay),port))return false;
    for(auto protectedRange:{first,arena,indexed,context,image,actor,target,port})
        if(overlap(output,protectedRange))return false;
    sources_[1]=rgb;return true;
}
/** @brief Меняет только адрес назначения на подтверждённой невидимой границе, не трогая старый RGB. */
bool PpuRgbOutput::retarget(PpuColourReplay&replay,std::uint16_t*rgb,std::uint64_t cycle,std::uint64_t frame)noexcept{
    if(!owner_||replay_!=&replay||!sources_[1]||rgb==rgb_||(rgb!=sources_[0]&&rgb!=sources_[1])||
       !replay.bound_||replay.error_!=PpuColourReplay::Error::None||&replay.targetPpu()!=owner_||
       replay.cycle_!=cycle||!frame||owner_->frames_!=frame||owner_->scanline_<241||
       owner_->pendingPixels_||dirty_||!replay.checkWitness())return false;
    rgb_=rgb;row_=240;emittedEnd_=repairPrefix_=0;return true;
}

/**
 * @brief Инициализирует цветовые таблицы и текущее содержимое связанного RGB-выхода.
 * @details Вызывается только в разрешённой привязке; это не кадр гостевого исполнения.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::initialise(const Ppu& p)noexcept{
    for(unsigned e=0;e<8;++e)for(unsigned c=0;c<64;++c)colours_[e][c]=displayRgb565(static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(e),p.pal_);
    const auto* indices=p.pixels_.data();
    if (p.pixels_.rowOnly()) std::fill_n(rgb_,pixelCount,colours_[0][0]);
    else for(unsigned y=0;y<240;++y){const auto& lut=colours_[p.lineEmphasis_[y]&7U];for(unsigned x=0;x<256;++x)rgb_[y*256+x]=lut[indices[y*256+x]&63U];}
    counts_.setupPixels+=pixelCount;validRows_=0;row_=240;emittedEnd_=repairPrefix_=0;dirty_=false;
}
/**
 * @brief Сбрасывает RGB-хвосты для того же привязанного PPU.
 * @details Не переносит внешние буферы между объектами и не освобождает заимствованную память.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::resetFor(const Ppu& p)noexcept{
    if(owner_==&p)initialise(p);
}
/**
 * @brief Начинает ограниченный RGB-интервал и учитывает возможную смену emphasis строки.
 * @details Смена PPUMASK может потребовать исправить уже выведенный префикс; окончательные индексные значения сохраняются.
 * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
 * @param start Начальная позиция ограниченного участка вывода.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 * @param oldEmphasis Предыдущее усиление строки для решения о перекраске префикса.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::beginSpan(const Ppu& p,unsigned row,unsigned start,unsigned count,unsigned emphasis,unsigned oldEmphasis)noexcept{
    if(owner_!=&p||row>=240||start>=256||!count||count>256-start)return;
    if(p.pixels_.rowOnly()){
        const auto expectedRow=row_>=239U?0U:row_+1U;
        if(row!=row_&&(start||row!=expectedRow||(row_!=240U&&emittedEnd_!=256U)))
            error_=Error::Boundary;
        if(row==row_&&start!=emittedEnd_)error_=Error::Boundary;
    }
    if(row_!=row){row_=row;emittedEnd_=repairPrefix_=0;dirty_=false;}
    if((emphasis&7U)!=(oldEmphasis&7U)){dirty_=true;repairPrefix_=start;
    }
    if(start+count>emittedEnd_)emittedEnd_=start+count;
}
/**
 * @brief Строит либо выдаёт строку таблицы RGB-пар для выбранного emphasis.
 * @details Таблица принадлежит вызывающему; маска validRows инвалидируется после изменения палитры.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::buildPairRow(const Ppu& p,unsigned emphasis)noexcept{
        auto* output=table_+emphasis*256U;const auto& lut=colours_[emphasis];
        for(unsigned n=0;n<256;++n)output[n]={lut[p.backgroundPairs_[n][0]&63U],lut[p.backgroundPairs_[n][1]&63U]};
        validRows_=static_cast<std::uint8_t>(validRows_|(1U<<emphasis));
}
/**
 * @brief Записывает восемь фоновых RGB-пикселей из уже вычисленного набора индексов.
 * @details Таблица пар уменьшает число сохранений; выбор палитры, grayscale и границы остаются теми же.
 * @param start Начальная позиция ограниченного участка вывода.
 * @param background Восемь уже выбранных фоновых индексов, по четыре бита на пиксель.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::background8(const Ppu& p,unsigned start,std::uint32_t background,unsigned emphasis)noexcept{
    const auto* pairs=pairRow(p,emphasis);auto* out=rowData()+start;
    // Четыре alias-safe32-битные pair-записи без RGB-ветви на каждом пикселе.
    std::memcpy(out,&pairs[background&255U],sizeof(Pair));
    std::memcpy(out+2,&pairs[(background>>8)&255U],sizeof(Pair));
    std::memcpy(out+4,&pairs[(background>>16)&255U],sizeof(Pair));
    std::memcpy(out+6,&pairs[background>>24],sizeof(Pair));
    countPairs(4);
}
/**
 * @brief Преобразует фактически выбранные индексные пиксели в RGB.
 * @details Не повторяет селектор приоритетов и попадания Sprite0; смешанный и частичный участки сохраняют исходный выбор.
 * @param indices Только что записанные palette/grayscale-индексы текущего PPU; чтение не передаёт владение.
 * @param start Начальная позиция ограниченного участка вывода.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 */
void PpuRgbOutput::selectedIndices(const std::uint8_t* indices,unsigned start,unsigned count,unsigned emphasis)noexcept{
    auto* out=rowData()+start;const auto& lut=colours_[emphasis&7U];
    for(unsigned i=0;i<count;++i)out[i]=lut[indices[i]&63U];
    countScalars(count);
}
/**
 * @brief Заполняет RGB-участок одним уже выбранным цветом.
 * @details Использует тот же LUT/emphasis и не изменяет индексную плоскость.
 * @param start Начальная позиция ограниченного участка вывода.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @param index Индекс элемента текущей таблицы/буфера; диапазон задаётся этим типом.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 */
void PpuRgbOutput::uniform(unsigned start,unsigned count,unsigned index,unsigned emphasis)noexcept{
    const auto c=colour(emphasis,index);auto* out=rowData()+start;
    for(unsigned n=0;n<count;++n)out[n]=c;
}
/**
 * @brief Пересчитывает указанный участок строки по итоговому emphasis.
 * @details Использует реальные индексные пиксели, чтобы mid-row PPUMASK не оставил префикс со старым цветом.
 * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
 * @param start Начальная позиция ограниченного участка вывода.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 * @param isFence Отмечает восстановление на границе выхода, а не строковый префикс; влияет только на диагностический счётчик.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::recolour(const Ppu& p,unsigned row,unsigned start,unsigned count,unsigned emphasis,bool isFence)noexcept{
    const auto* in=p.pixels_.at(row,start);auto* out=rgb_+row*256U+start;const auto& lut=colours_[emphasis&7U];
    for(unsigned i=0;i<count;++i)out[i]=lut[in[i]&63U];
    (void)isFence;
}
/**
 * @brief Закрывает RGB-интервал с сохранением хвостового состояния строки.
 * @details Не переименовывает буфер и не выполняет физическое представление кадра.
 * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
 */
void PpuRgbOutput::finishSpan(const Ppu& p)noexcept{
    if(owner_!=&p||row_>=240||emittedEnd_!=256)return;
    if(dirty_&&repairPrefix_)recolour(p,row_,0,repairPrefix_,p.lineEmphasis_[row_],false);
    dirty_=false;repairPrefix_=0;
}
/**
 * @brief Доводит отложенные RGB-хвосты и перекраску до наблюдаемой границы.
 * @details Это граница ядра, а не PPA/LCD fence; аппаратные читатели буфера управляются приложением.
 */
void PpuRgbOutput::fence(const Ppu& p)noexcept{
    if(owner_!=&p||row_>=240||!dirty_)return;
    // RGB-only не публикует средину кадра. Сохраняем dirty; suffix ещё не принадлежит строке.
    if(p.pixels_.rowOnly()&&emittedEnd_<256U)return;
    if(emittedEnd_<256)recolour(p,row_,0,256,p.lineEmphasis_[row_],true);
    else if(repairPrefix_)recolour(p,row_,0,repairPrefix_,p.lineEmphasis_[row_],false);
    dirty_=false;repairPrefix_=0;
}
/**
 * @brief Предоставляет готовый заимствованный RGB-буфер после необходимого завершения хвоста.
 * @details Буфер и таблица пар остаются у вызывающего до unbind и завершения всех пользователей.
 * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
 */
const std::uint16_t* PpuRgbOutput::pixels()noexcept{
    if(!owner_||error_!=Error::None)return nullptr;
    if(owner_->pixels_.rowOnly()&&
       (!owner_->frames_||owner_->scanline_<241||row_!=239U||emittedEnd_!=256U))return nullptr;
    owner_->flushPixels();fence(*owner_);return rgb_;
}
}
