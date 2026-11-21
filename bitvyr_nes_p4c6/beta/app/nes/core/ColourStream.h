/**
 * @file ColourStream.h
 * @brief Аренды ROM, срезы времени и состояния producer/consumer цветового журнала.
 * @ingroup nes_core
 */
#pragma once
#include "Types.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nes::core {
/**
 * @brief Единственный владелец гостевых часов, карты памяти, CPU/PPU/APU и состояния картриджа. ROM заимствован; ввод, хранение, задачи и физический вывод остаются вне ядра.
 * @ingroup nes_core
 */
class Machine;
/**
 * @brief Актор хронологического повтора полного PPU. Теневая Machine служит шиной; её CPU/APU clock запрещён.
 * @ingroup nes_core
 */
class PpuColourReplay;
// Новые значения используются только RUN ThinMMC5; прежние 0..4 сохранены.
enum class ColourKind:std::uint8_t{Read,RetainedRead,Write,CartWrite,Dma,Mmc5CpuStrobe,Mmc5DmcStrobe,Mmc5SignalWrite,Mmc5IdleReset,Mmc5IdleDmcStrobe};
/**
 * @brief Одна компактная реальная read/write/cart/DMA-операция с временным смещением и фактическим байтом.
 * @ingroup nes_core
 */
struct ColourEvent{std::uint32_t cycleDelta;std::uint16_t address;ColourKind kind;std::uint8_t value;};
static_assert(sizeof(ColourEvent)==8,"bounded live event");
enum class ColourFault:std::uint8_t{None,Binding,Capacity,Time,Event,Cancelled,Lease,Sequence,Cut,Replay,Producer};
/**
 * @brief Идентичность producer/ROM/эпохи/поколения/региона. Описывает заимствование, но не продлевает его и не управляет аппаратными DMA-fences.
 * @ingroup nes_core
 */
struct ColourLease{
    const Machine* producer=nullptr;
    const std::uint8_t* rom=nullptr;
    std::size_t romBytes=0;
    std::uint64_t epoch=0,generation=0;
    Region region=Region::Ntsc;
};
/**
 * @brief Один согласованный гостевой срез: CPU-cycle, PPU-frame/scanline/dot и дробный PAL-остаток.
 * @ingroup nes_core
 */
struct ColourCut{
    std::uint64_t cycle=0,frame=0;
    std::int16_t scanline=-1;
    std::uint16_t dot=0;
    std::uint8_t phase=0;
};
/**
 * @brief Сравнивает все поля идентичности двух цветовых аренд без чтения ROM.
 * @param a Первая уже опубликованная либо исключительно доступная аренда.
 * @param b Вторая аренда с обеспеченным вызывающим сроком жизни её объектов.
 * @return true при совпадении producer, ROM-адреса/размера, epoch, generation и региона.
 * @details Не проверяет допустимость аренды, не продлевает заимствование и не
 * выполняет acquire: синхронизация и проверки ненулевых полей остаются у caller.
 * Сравнение полей намеренно не зависит от padding представления C++-структуры.
 */
constexpr bool sameColourLease(const ColourLease& a,const ColourLease& b)noexcept{
    return a.producer==b.producer&&a.rom==b.rom&&a.romBytes==b.romBytes&&a.epoch==b.epoch&&a.generation==b.generation&&a.region==b.region;
}
/**
 * @brief Сравнивает точное гостевое CPU/PPU-время двух закрытых срезов.
 * @param a Первый неизменяемый срез: CPU-такт, кадр, строка, следующая PPU-точка и фаза.
 * @param b Второй срез с тем же обеспеченным вызывающим доступом к полям.
 * @return true только при совпадении всех пяти полей; близкое время не считается равным.
 * @details Это чистое сравнение, не проверка региона/диапазонов, не материализация
 * долга и не барьер публикации. Допустимость каждого среза проверяется отдельно.
 */
constexpr bool sameColourCut(const ColourCut& a,const ColourCut& b)noexcept{
    return a.cycle==b.cycle&&a.frame==b.frame&&a.scanline==b.scanline&&a.dot==b.dot&&a.phase==b.phase;
}
/**
 * @brief Ограниченный слот хронологических CPU-видеособытий с состояниями Free/Writing/Sealed/Reading/Consumed и неизменяемой после публикации арендой.
 * @ingroup nes_core
 */
class ColourPacket final{
public:
    static constexpr std::uint32_t maximumEvents=65536,maximumCycles=45000;
    enum class State:unsigned{Free,Writing,Sealed,Reading,Consumed};
    /**
     * @brief Публикуемые lease/base/end/sequence/count/fault цветового слота; после Sealed не меняются до освобождения читателем.
     * @ingroup nes_core
     */
    struct Header{ColourLease lease{};ColourCut base{},end{};std::uint64_t sequence=0;std::uint32_t count=0;ColourFault fault=ColourFault::None;};
    /**
     * @brief Создаёт ограниченное хранилище видеособытий.
     * @details Ёмкость и источник записей не растут во время гостевого исполнения; внешний буфер, если он передан, остаётся у вызывающего.
     * @param events Отдельный ограниченный массив событий вызывающего.
     * @param capacity Ёмкость целевого массива в единицах его элементов.
     */
    ColourPacket(ColourEvent* events,std::uint32_t capacity)noexcept:events_(events),capacity_(capacity){}
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    ColourPacket(const ColourPacket&)=delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    ColourPacket&operator=(const ColourPacket&)=delete;
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Вычисленное значение или состояние типа State согласно описанному контракту.
     */
    State state()const noexcept{return static_cast<State>(state_.load(std::memory_order_acquire));}
    /**
     * @brief Предоставляет неизменяемый заголовок опубликованного пакета.
     * @details Ссылка заимствована. Писатель не изменяет заголовок, пока пакет принадлежит читателю; внутренний объект нельзя использовать как самостоятельный снимок.
     * @return Ссылка на метаданные пакета; владение и срок жизни остаются у пакета.
     */
    const Header& header()const noexcept{return header_;} // Неизменяем только в состояниях Sealed/Reading/Consumed.
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    const ColourEvent* events()const noexcept{return events_;}
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
     */
    std::uint32_t capacity()const noexcept{return capacity_;}
    /**
     * @brief Переводит запечатанный, но не используемый результат в потреблённый.
     * @details Сравнение состояния атомарное; вызов не исполняет события повторно и сам по себе не освобождает внешнюю память.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool discard()noexcept{unsigned expected=unsigned(State::Sealed);return state_.compare_exchange_strong(expected,unsigned(State::Consumed),std::memory_order_acq_rel);}
    /**
     * @brief Возвращает уже потреблённый слот производителю.
     * @details Не даёт права освобождать ROM или другие буферы, которые всё ещё заимствует активный актор.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool release()noexcept{unsigned expected=unsigned(State::Consumed);return state_.compare_exchange_strong(expected,unsigned(State::Free),std::memory_order_acq_rel);}
private:
    friend class Machine;
    friend class ColourStreamReader;
    /**
     * @brief Проверяет размер, диапазон и ограничения заимствованного хранилища.
     * @details Не выделяет память; указатель и срок жизни остаются обязанностью владельца.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool storageValid()const noexcept;
    /**
     * @brief Переводит свободный слот в запись и фиксирует его аренду/начальный срез.
     * @details Публикация и чтение разрешены только после соответствующего перехода состояния.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool claim(const ColourLease&,std::uint64_t,const ColourCut&)noexcept;
    /**
     * @brief Добавляет одно событие с проверенным временем и формой.
     * @details При переполнении или нарушении порядка защёлкивает ошибку, а не теряет событие молча.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool append(ColourKind,std::uint64_t,std::uint16_t,std::uint8_t)noexcept;
    /**
     * @brief Защёлкивает первую ошибку пакета.
     * @details Не превращает ошибочный частичный результат в готовый кадр.
     * @param f Первая сохраняемая ошибка протокола.
     */
    void poison(ColourFault f)noexcept{if(header_.fault==ColourFault::None)header_.fault=f;}
    /**
     * @brief Фиксирует конечный срез и публикует завершённые записи.
     * @details После release-перехода писатель не изменяет данные, пока потребитель не освободит слот.
     * @param cut Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
     */
    void seal(const ColourCut& cut)noexcept{header_.end=cut;state_.store(unsigned(State::Sealed),std::memory_order_release);}
    ColourEvent* const events_;
    const std::uint32_t capacity_;
    Header header_{};
    std::atomic<unsigned> state_{unsigned(State::Free)};
};
static_assert(std::atomic<unsigned>::is_always_lock_free,"live slot publication requires lock-free32-bit atomics");
/**
 * @brief Проверяющий потребитель запечатанного ColourPacket, заимствующий один полный PPU-актор и текущую ROM-аренду.
 * @ingroup nes_core
 */
class ColourStreamReader final{
public:
    /**
     * @brief Создаёт потребитель, заимствующий единственный полный PPU-актор.
     * @details Не копирует актор и не переносит его владение; повторное чтение устройства вместо сохранённого ответа недопустимо.
     * @param actor Постоянный единственный полный PPU-replay; его CPU/APU не исполняются, ссылка переживает reader/sink.
     */
    explicit ColourStreamReader(PpuColourReplay& actor)noexcept:actor_(actor){}
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    ColourStreamReader(const ColourStreamReader&)=delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    ColourStreamReader&operator=(const ColourStreamReader&)=delete;
    /**
     * @brief Начинает новый интервал или привязку объекта.
     * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool begin(const ColourLease&)noexcept;
    /**
     * @brief Проверяет и исполняет опубликованные записи текущей аренды.
     * @details Порядок времени, последовательность, реальные ответы чтений и конечный срез проверяются до готовности результата.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param currentLease Текущая проверяемая аренда с теми же ROM/epoch/generation; не заменяет реальное присоединение владельца.
     */
    bool consume(ColourPacket&,const ColourLease& currentLease)noexcept;
    /**
     * @brief Возвращает текущую ошибку соответствующего тракта.
     * @details Первичная ошибка сохраняется до установленного reset/begin; отсутствие ошибки не означает завершения физического вывода.
     * @return Вычисленное значение или состояние типа ColourFault согласно описанному контракту.
     */
    ColourFault fault()const noexcept{return fault_;}
    /**
     * @brief Проверяет готовность согласованного результата без его исполнения.
     * @details Готовность зависит от завершённого протокола и отсутствия защёлкнутой ошибки; владение буферами автоматически не передаётся.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool outputReady()const noexcept{return outputReady_;}
private:
    /**
     * @brief Защёлкивает первую ошибку и запрещает готовность частичного результата.
     * @details Не откатывает уже исполненные побочные эффекты и не разрешает скрытую смену владельца.
     * @param f Первая сохраняемая ошибка протокола.
     * @return false после фиксации первой ошибки; успешный частичный результат не выдаётся.
     */
    bool fail(ColourFault f)noexcept{if(fault_==ColourFault::None)fault_=f;outputReady_=false;return false;}
    PpuColourReplay& actor_;
    ColourLease lease_{};
    ColourCut expected_{};
    std::uint64_t nextSequence_=0;
    ColourFault fault_=ColourFault::None;
    bool begun_=false,outputReady_=false;
};
}
