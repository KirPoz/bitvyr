/**
 * @file PpuRgbOutput.h
 * @brief Caller-owned RGB-контекст с проверенной идентичностью единственного актора.
 * @ingroup nes_ppu
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nes::core {
/**
 * @brief Полный растровый PPU: конвейер, OAM, Sprite0/overflow, индексное изображение и необязательный RGB-выход. Шина заимствована, пиксели управляются выбранной политикой хранения.
 * @ingroup nes_ppu
 */
class Ppu;
/**
 * @brief Актор хронологического повтора полного PPU. Теневая Machine служит шиной; её CPU/APU clock запрещён.
 * @ingroup nes_ppu
 */
class PpuColourReplay;

// Только диагностическая аренда выхода: вызывающий владеет объектом, RGB и pair-хранилищем.
// Закреплённый replay-актор должен простаивать при bind/unbind и на границах жизни буфера.
/**
 * @brief Borrowed BG writer на один уже открытый qualified span без владения.
 * @details Нельзя сохранять за пределами вызова либо через palette/PPUMASK/row
 * invalidation. prepare вызывается только после rebuildBackgroundPairs.
 */
struct PpuBackgroundWriter {
    const std::uint8_t* pairs = nullptr;
    std::uint16_t* row = nullptr;
    /** @brief Показывает завершённую ленивую подготовку; не проверяет внешнюю аренду. */
    [[nodiscard]] bool ready() const noexcept { return pairs != nullptr; }
    /**
     * @brief Пишет четыре точные native-endian RGB пары без повторной подготовки.
     * @param start Абсолютная позиция 0..248 текущей строки, кратная 8.
     * @param background Восемь уже выбранных фоновых nibble-индексов.
     * @pre ready(), стабильные palette/grayscale/emphasis/row и 8 доступных pixels.
     */
    void write(unsigned start, std::uint32_t background) const noexcept {
        auto* out = row + start;
        std::memcpy(out, pairs + (background & 255U) * 4U, 4);
        std::memcpy(out + 2, pairs + ((background >> 8U) & 255U) * 4U, 4);
        std::memcpy(out + 4, pairs + ((background >> 16U) & 255U) * 4U, 4);
        std::memcpy(out + 6, pairs + (background >> 24U) * 4U, 4);
    }
};

/**
 * @brief Контекст заимствованных RGB565/Pair-буферов для точного выбранного PPU. Привязка и teardown требуют исключения активного использования.
 * @ingroup nes_ppu
 */
class PpuRgbOutput final {
public:
    /**
     * @brief Два RGB565-пикселя таблицы фоновых индексов; копируется alias-safe способом без требований uint32_t-алиасинга.
     * @ingroup nes_ppu
     */
    struct alignas(4) Pair { std::uint16_t first,second; };
    static constexpr std::size_t pairCount=8U*256U;
    static constexpr std::size_t pixelCount=256U*240U;
    static_assert(sizeof(Pair)==4,"native-endian two-word RGB pair");
    enum class Error:std::uint8_t {None,Busy,Boundary,Capacity,Alignment,Alias,Pixels};
    /**
     * @brief Накопленные диагностические счётчики данного владельца; чтение требует внешнего исключения активного писателя и не измеряет физическую частоту кадров.
     * @ingroup nes_ppu
     */
    struct Counters {
        std::uint64_t setupPixels=0,emittedPixels=0,spans=0,pairStores=0,scalarPixels=0,uniformPixels=0;
        std::uint64_t pairRowsBuilt=0,prefixRepairPixels=0,fenceRepairPixels=0,emphasisChanges=0;
    };
    /**
     * @brief Создаёт пока не привязанный RGB-контекст без владения внешними буферами.
     * @details Указатели на PPU, RGB и таблицу пар задаются только успешным bind.
     */
    PpuRgbOutput()noexcept=default;
    /**
     * @brief Отсоединяет RGB-контекст до разрушения объекта.
     * @details Не освобождает заимствованные RGB/Pair-буферы; внешний пользователь обязан закончить работу.
     */
    ~PpuRgbOutput()noexcept{unbind();}
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    PpuRgbOutput(const PpuRgbOutput&)=delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    PpuRgbOutput&operator=(const PpuRgbOutput&)=delete;
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
    bool bind(PpuColourReplay& replay,std::uint16_t* rgb,std::size_t words,Pair* table,std::size_t pairs)noexcept;
    /**
     * @brief Завершает соответствующую привязку после исключения активного использования.
     * @details Не освобождает память вызывающего и не заменяет ожидание фактического окончания другой стороны.
     */
    void unbind()noexcept;
    /**
     * @brief На reset7 регистрирует ровно второй заимствованный RGB-источник без его заполнения.
     * @param replay Тот же связанный actor, исключённый из исполнения.
     * @param rgb Полный второй диапазон, непересекающийся с первым и защищёнными объектами.
     * @param words Ёмкость в uint16_t; не менее pixelCount.
     * @return false без смены источника при invalid boundary/alias/alignment; память не освобождается.
     */
    bool registerAlternate(PpuColourReplay&replay,std::uint16_t*rgb,std::size_t words)noexcept;
    /**
     * @brief После принятого кадра переводит единственного RGB-писателя на заранее свободный A/B.
     * @details Внешний владелец доказывает DMA-credit. Метод не закрывает старые хвосты: pending/dirty
     * отвергаются до записи; предыдущий pixels() обязан завершиться ДО запуска аппаратного reader.
     * Таблицы/палитра сохраняются, RGB-плоскости не копируются и не инициализируются.
     * @param replay Тот же actor после настоящего commit/join и прежнего RGB fence.
     * @param rgb Точное начало зарегистрированного другого диапазона.
     * @param cycle Ожидаемое абсолютное гостевое CPU-время завершённого кадра.
     * @param frame Ожидаемый ненулевой завершённый кадр, а не следующий input frame.
     * @return true после единственной смены назначения; false оставляет прежнее назначение.
     */
    bool retarget(PpuColourReplay&replay,std::uint16_t*rgb,std::uint64_t cycle,std::uint64_t frame)noexcept;
    /**
     * @brief Возвращает состояние или результат текущей привязки.
     * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool bound()const noexcept{return owner_!=nullptr;}
    /**
     * @brief Возвращает состояние или результат текущей привязки.
     * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
     * @return Вычисленное значение или состояние типа Error согласно описанному контракту.
     */
    Error error()const noexcept{return error_;}
    /**
     * @brief Предоставляет счётчики работы текущего вспомогательного владельца.
     * @details Ссылка действительна при живом объекте; конкурентное чтение активного писателя требует внешней синхронизации.
     * @return Заимствованная ссылка на накопленные счётчики.
     */
    const Counters& counters()const noexcept{return counts_;}
    /**
     * @brief Предоставляет готовый заимствованный RGB-буфер после необходимого завершения хвоста.
     * @details Буфер и таблица пар остаются у вызывающего до unbind и завершения всех пользователей.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    const std::uint16_t* pixels()noexcept;

    // Lookup не разыменовывает контекст чужого актора. Исполнение и
    // teardown того же актора требуют обычного исключительного Ppu-владения.
    /**
     * @brief Возвращает RGB-контекст только для точной идентичности PPU.
     * @details Проверка атомарного владельца предшествует разыменованию контекста другого актора; lookup не даёт права на конкурентный teardown.
     * @param owner Точный PPU/машина, которому принадлежит текущая привязка.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    static PpuRgbOutput* activeFor(const Ppu* owner)noexcept;
    /**
     * @brief Завершает соответствующую привязку после исключения активного использования.
     * @details Не освобождает память вызывающего и не заменяет ожидание фактического окончания другой стороны.
     * @param owner Точный PPU/машина, которому принадлежит текущая привязка.
     */
    static void detachOwner(const Ppu* owner)noexcept;
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
    void beginSpan(const Ppu& p,unsigned row,unsigned start,unsigned count,unsigned emphasis,unsigned oldEmphasis)noexcept;
    /**
     * @brief Закрывает RGB-интервал с сохранением хвостового состояния строки.
     * @details Не переименовывает буфер и не выполняет физическое представление кадра.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void finishSpan(const Ppu& p)noexcept;
    /**
     * @brief Доводит отложенные RGB-хвосты и перекраску до наблюдаемой границы.
     * @details Это граница ядра, а не PPA/LCD fence; аппаратные читатели буфера управляются приложением.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void fence(const Ppu& p)noexcept;
    /**
     * @brief Сбрасывает RGB-хвосты для того же привязанного PPU.
     * @details Не переносит внешние буферы между объектами и не освобождает заимствованную память.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void resetFor(const Ppu& p)noexcept;
    /**
     * @brief Инвалидирует строки цветовой таблицы после перестройки палитры.
     * @details Следующий запрос пары восстановит нужную строку; чужой актор не получает этот контекст.
     */
    void paletteRebuilt()noexcept{validRows_=0;}
    /**
     * @brief Строит либо выдаёт строку таблицы RGB-пар для выбранного emphasis.
     * @details Таблица принадлежит вызывающему; маска validRows инвалидируется после изменения палитры.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    const Pair* pairRow(const Ppu& p,unsigned emphasis)noexcept{
        emphasis&=7U;
        if(!(validRows_&(1U<<emphasis)))buildPairRow(p,emphasis);
        return table_+emphasis*256U;
    }
    /**
     * @brief Возвращает адрес текущей строки заимствованного RGB-буфера.
     * @details Допустим только при действительной привязке и корректно открытом span; не передаёт владение памятью.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    std::uint16_t* rowData()const noexcept{return rgb_+row_*256U;}
    /**
     * @brief Заимствует готовую pair-row и текущую output-row до конца одного span.
     * @param p Закреплённый PPU после rebuildBackgroundPairs и beginSpan.
     * @param emphasis Три стабильных бита PPUMASK текущего span.
     * @return Невладеющий 8 Б view; не пересекает palette/row/MMIO/retarget boundary.
     */
    [[nodiscard]] PpuBackgroundWriter prepareBackgroundWriter(const Ppu& p,unsigned emphasis) noexcept {
        return {reinterpret_cast<const std::uint8_t*>(pairRow(p,emphasis)), rowData()};
    }
    /**
     * @brief Выдаёт адрес строки либо значение уже подготовленной RGB-палитры.
     * @details Предполагает действительную текущую привязку; не выполняет PPU-выбор пикселя.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     * @param index Индекс элемента текущей таблицы/буфера; диапазон задаётся этим типом.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t colour(unsigned emphasis,unsigned index)const noexcept{return colours_[emphasis&7U][index&63U];}
    // Только kernel-вызовы: закреплённый актор сохраняет индексное выделение
    // всю аренду. Indices — только что записанные palette/gray-значения.
    // Определения находятся в отдельном диагностическом объекте default-Flash.
    /**
     * @brief Записывает восемь фоновых RGB-пикселей из уже вычисленного набора индексов.
     * @details Таблица пар уменьшает число сохранений; выбор палитры, grayscale и границы остаются теми же.
     * @param start Начальная позиция ограниченного участка вывода.
     * @param background Восемь уже выбранных фоновых индексов, по четыре бита на пиксель.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void background8(const Ppu& p,unsigned start,std::uint32_t background,unsigned emphasis)noexcept;
    /**
     * @brief Преобразует фактически выбранные индексные пиксели в RGB.
     * @details Не повторяет селектор приоритетов и попадания Sprite0; смешанный и частичный участки сохраняют исходный выбор.
     * @param indices Только что записанные palette/grayscale-индексы текущего PPU; чтение не передаёт владение.
     * @param start Начальная позиция ограниченного участка вывода.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     */
    void selectedIndices(const std::uint8_t* indices,unsigned start,unsigned count,unsigned emphasis)noexcept;
    /**
     * @brief Заполняет RGB-участок одним уже выбранным цветом.
     * @details Использует тот же LUT/emphasis и не изменяет индексную плоскость.
     * @param start Начальная позиция ограниченного участка вывода.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @param index Индекс элемента текущей таблицы/буфера; диапазон задаётся этим типом.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     */
    void uniform(unsigned start,unsigned count,unsigned index,unsigned emphasis)noexcept;
    /**
     * @brief Обслуживает необязательные диагностические счётчики цветового ядра.
     * @details Счётчики описывают работу выбранного пути, а не частоту кадров или завершение аппаратного DMA.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     */
    void countPairs(unsigned count)noexcept{
        (void)count;
    }
    /**
     * @brief Обслуживает необязательные диагностические счётчики цветового ядра.
     * @details Счётчики описывают работу выбранного пути, а не частоту кадров или завершение аппаратного DMA.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     */
    void countScalars(unsigned count)noexcept{
        (void)count;
    }
private:
    /**
     * @brief Строит либо выдаёт строку таблицы RGB-пар для выбранного emphasis.
     * @details Таблица принадлежит вызывающему; маска validRows инвалидируется после изменения палитры.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void buildPairRow(const Ppu& p,unsigned emphasis)noexcept;
    /**
     * @brief Инициализирует цветовые таблицы и текущее содержимое связанного RGB-выхода.
     * @details Вызывается только в разрешённой привязке; это не кадр гостевого исполнения.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void initialise(const Ppu& p)noexcept;
    /**
     * @brief Пересчитывает указанный участок строки по итоговому emphasis.
     * @details Использует реальные индексные пиксели, чтобы mid-row PPUMASK не оставил префикс со старым цветом.
     * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
     * @param start Начальная позиция ограниченного участка вывода.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     * @param fence Отмечает восстановление на границе выхода, а не строковый префикс; влияет только на диагностический счётчик.
     * @param p Точный закреплённый PPU с живой индексной плоскостью; нельзя передавать копию/чужого актора.
     */
    void recolour(const Ppu& p,unsigned row,unsigned start,unsigned count,unsigned emphasis,bool fence)noexcept;
    const Ppu* owner_=nullptr;
    PpuColourReplay* replay_=nullptr;
    std::uint16_t* sources_[2]{nullptr,nullptr};
    std::uint16_t* rgb_=nullptr;
    Pair* table_=nullptr;
    std::array<std::array<std::uint16_t,64>,8> colours_{};
    Counters counts_{};
    unsigned row_=240,emittedEnd_=0,repairPrefix_=0;
    std::uint8_t validRows_=0;
    bool dirty_=false;
    Error error_=Error::None;
};
}
