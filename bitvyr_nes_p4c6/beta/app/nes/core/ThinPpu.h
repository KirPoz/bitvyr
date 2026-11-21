/**
 * @file ThinPpu.h
 * @brief Контракты сигнального владельца, аренды и execute-once ответа PPU.
 * @ingroup nes_core
 */
#pragma once
#include "ColourStream.h"
#include "AudioStageToken.h"
#include <array>
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
enum class ThinMode:std::uint8_t{Oracle,Signals};
/**
 * @brief Закрытый непрерывный префикс с lease, начальным/конечным срезом и порядковым номером. Массив events заимствован до завершения sink-вызова либо его явного копирования транспортом.
 * @ingroup nes_core
 */
struct ThinPrefix {
    ColourLease lease{};ColourCut base{},end{};
    std::uint64_t sequence=0;
    const ColourEvent* events=nullptr;std::uint32_t count=0;
};
/**
 * @brief Execute-once запрос PPU-чтения на точном закрытом срезе; lease, sequence и request связывают ответ с единственным обращением.
 * @ingroup nes_core
 */
struct ThinReadRequest {ColourLease lease{};ColourCut cut{};std::uint64_t sequence=0,request=0;std::uint8_t reg=0;};
/**
 * @brief Настоящий байт полного PPU и эхо lease/cut/sequence/request. Ответ нельзя подменить прогнозом или применить повторно.
 * @ingroup nes_core
 */
struct ThinReadReply {ColourLease lease{};ColourCut cut{};std::uint64_t sequence=0,request=0;std::uint8_t value=0;};
// Только синхронный native-прототип. Вызывающий сохраняет стабильную конечную точку и все
// непересекающиеся акторы/буферы до погашения и отвязки producer.
/**
 * @brief Заимствованный синхронный endpoint тонкого протокола; prefix/read/commit требуют точной аренды и подтверждения реального потребителя.
 * @ingroup nes_core
 */
class ThinPpuSink {
public:
    /**
     * @brief Завершает время жизни объекта с полиморфным либо владеющим контрактом.
     * @details Связанные внешние bus/ROM/буферы должны сохраняться до окончания всех активных пользователей.
     */
    virtual ~ThinPpuSink()=default;
    /**
     * @brief Начинает новый интервал или привязку объекта.
     * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    virtual bool begin(const ColourLease&,const ColourCut&)noexcept=0;
    /**
     * @brief Передаёт законченный временной префикс реальному полному PPU.
     * @details Потребитель обязан закончить заимствование записей согласно sink-контракту; предполагаемые будущие события не публикуются.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    virtual bool prefix(const ThinPrefix&)noexcept=0;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    virtual bool read(const ThinReadRequest&,ThinReadReply&)noexcept=0;
    /**
     * @brief Подтверждает полный кадр после достижения точного конечного среза.
     * @details Общий успех нужен до публикации RGB/аудио; один лишь producer-done не завершает всех потребителей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    virtual bool commit(const ColourLease&,const ColourCut&,std::uint64_t)noexcept=0;
    /**
     * @brief Терминально отменяет текущую привязку/сеанс после нарушения контракта.
     * @details До нового согласованного reset запрещает дальнейшую публикацию; память удерживается до фактической остановки пользователей.
     */
    virtual void invalidate(ColourFault)noexcept=0;
};
/**
 * @brief Адаптер тонкого sink к одному полному PpuColourReplay; реальные чтения исполняются ровно один раз.
 * @ingroup nes_core
 */
class ThinReplaySink final:public ThinPpuSink {
public:
    /**
     * @brief Создаёт потребитель, заимствующий единственный полный PPU-актор.
     * @details Не копирует актор и не переносит его владение; повторное чтение устройства вместо сохранённого ответа недопустимо.
     * @param actor Постоянный единственный полный PPU-replay; его CPU/APU не исполняются, ссылка переживает reader/sink.
     */
    explicit ThinReplaySink(PpuColourReplay& actor)noexcept:actor_(actor){}
    /**
     * @brief Начинает новый интервал или привязку объекта.
     * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool begin(const ColourLease&,const ColourCut&)noexcept override;
    /**
     * @brief Передаёт законченный временной префикс реальному полному PPU.
     * @details Потребитель обязан закончить заимствование записей согласно sink-контракту; предполагаемые будущие события не публикуются.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool prefix(const ThinPrefix&)noexcept override;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool read(const ThinReadRequest&,ThinReadReply&)noexcept override;
    /**
     * @brief Подтверждает полный кадр после достижения точного конечного среза.
     * @details Общий успех нужен до публикации RGB/аудио; один лишь producer-done не завершает всех потребителей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool commit(const ColourLease&,const ColourCut&,std::uint64_t)noexcept override;
    /**
     * @brief Терминально отменяет текущую привязку/сеанс после нарушения контракта.
     * @details До нового согласованного reset запрещает дальнейшую публикацию; память удерживается до фактической остановки пользователей.
     */
    void invalidate(ColourFault)noexcept override;
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
    bool ready()const noexcept{return ready_&&fault_==ColourFault::None;}
private:
    /**
     * @brief Защёлкивает первую ошибку и запрещает готовность частичного результата.
     * @details Не откатывает уже исполненные побочные эффекты и не разрешает скрытую смену владельца.
     * @return false после фиксации первой ошибки; успешный частичный результат не выдаётся.
     */
    bool fail(ColourFault)noexcept;
    PpuColourReplay& actor_;
    ColourLease lease_{};ColourCut expected_{};
    std::uint64_t sequence_=0,request_=0,frame_=0;
    ColourFault fault_=ColourFault::None;
    bool begun_=false,ready_=false;
};
/**
 * @brief Ограниченный буфер тонких событий и синхронных read-запросов; хранит terminal-fault и кадровые границы, но не владеет endpoint/ROM.
 * @ingroup nes_core
 */
class ThinPpuChannel final {
public:
    /**
     * @brief Накопленные диагностические счётчики данного владельца; чтение требует внешнего исключения активного писателя и не измеряет физическую частоту кадров.
     * @ingroup nes_core
     */
    struct Counters {
        std::uint64_t prefixes=0,records=0,reads=0,commits=0,failures=0;
        std::array<std::uint64_t,8> readRegisters{};
        std::uint32_t maximumBuffered=0;
    };
    /**
     * @brief Создаёт тонкий канал с заимствованным sink и отдельным ограниченным массивом событий.
     * @details Вызывающий удерживает адреса endpoint/буфера до остановки и отсоединения обеих сторон.
     * @param events Отдельный ограниченный массив событий вызывающего.
     * @param capacity Ёмкость целевого массива в единицах его элементов.
     * @param sink Стабильная конечная точка префиксов/настоящих чтений; сохраняется до отсоединения и завершения активных вызовов.
     */
    ThinPpuChannel(ThinPpuSink& sink,ColourEvent* events,std::uint32_t capacity)noexcept:sink_(sink),events_(events),capacity_(capacity){}
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    ThinPpuChannel(const ThinPpuChannel&)=delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    ThinPpuChannel&operator=(const ThinPpuChannel&)=delete;
    /**
     * @brief Возвращает выбранный режим протокола или отображения.
     * @details Проверка не переключает владельца и не продлевает текущую аренду.
     * @return Вычисленное значение или состояние типа ThinMode согласно описанному контракту.
     */
    ThinMode mode()const noexcept{return mode_;}
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
    bool outputReady()const noexcept{return ready_&&fault_==ColourFault::None;}
    /**
     * @brief Проверяет готовность согласованного результата без его исполнения.
     * @details Готовность зависит от завершённого протокола и отсутствия защёлкнутой ошибки; владение буферами автоматически не передаётся.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool audioReady()const noexcept{return outputReady()&&!audioRetired_;}
    /**
     * @brief Предоставляет счётчики работы текущего вспомогательного владельца.
     * @details Ссылка действительна при живом объекте; конкурентное чтение активного писателя требует внешней синхронизации.
     * @return Заимствованная ссылка на накопленные счётчики.
     */
    const Counters& counters()const noexcept{return counters_;}
    /**
     * @brief Терминально отменяет активный тонкий сеанс с указанной причиной.
     * @details Ожидание реальной остановки и удержание памяти остаются обязанностью вызывающего.
     * @param reason Причина терминальной отмены текущего сеанса.
     */
    void cancel(ColourFault reason=ColourFault::Cancelled)noexcept{static_cast<void>(fail(reason));}
private:
    friend class Machine;
    /**
     * @brief Привязывает заимствованный ресурс или временного получателя к текущему владельцу.
     * @details Точные ограничения размера, фазового среза и неперекрытия определяются этим типом; произвольное освобождение ресурса во время использования недопустимо.
     * @param audioGeneration Поколение звукового пакета, проверяемое независимо от lease цветового владельца.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool bind(const ColourLease&,const ColourCut&,ThinMode,std::uint64_t audioGeneration)noexcept;
    /**
     * @brief Добавляет одно событие с проверенным временем и формой.
     * @details При переполнении или нарушении порядка защёлкивает ошибку, а не теряет событие молча.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool append(ColourKind,std::uint64_t,std::uint16_t,std::uint8_t,const ColourCut&)noexcept;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool read(const ColourCut&,std::uint16_t,std::uint8_t&)noexcept;
    /**
     * @brief Передаёт накопленный завершённый префикс получателю.
     * @details Не пересекает будущий timestamp и не даёт права переиспользовать заимствованные записи раньше обещанного завершения.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool flush(const ColourCut&)noexcept;
    /**
     * @brief Подтверждает полный кадр после достижения точного конечного среза.
     * @details Общий успех нужен до публикации RGB/аудио; один лишь producer-done не завершает всех потребителей.
     * @param audioGeneration Поколение звукового пакета, проверяемое независимо от lease цветового владельца.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool commit(const ColourCut&,std::uint64_t audioGeneration)noexcept;
    /**
     * @brief Защёлкивает первую ошибку и запрещает готовность частичного результата.
     * @details Не откатывает уже исполненные побочные эффекты и не разрешает скрытую смену владельца.
     * @return false после фиксации первой ошибки; успешный частичный результат не выдаётся.
     */
    bool fail(ColourFault)noexcept;
    /**
     * @brief Проверяет размер, диапазон и ограничения заимствованного хранилища.
     * @details Не выделяет память; указатель и срок жизни остаются обязанностью владельца.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool storageValid(const Machine&,const ColourLease&)const noexcept;
    /**
     * @brief Начинает учёт следующего гостевого кадра внутри уже действительной привязки.
     * @details Снимает готовность прежнего результата, не сбрасывая ROM и не освобождая буферы.
     */
    void beginFrame()noexcept{ready_=false;audioRetired_=false;}
    ThinPpuSink& sink_;
    ColourEvent* const events_;
    const std::uint32_t capacity_;
    ColourLease lease_{};ColourCut base_{};
    std::uint64_t sequence_=0,request_=0,frame_=0,audioGeneration_=0;
    std::uint32_t count_=0;
    ThinMode mode_=ThinMode::Oracle;
    ColourFault fault_=ColourFault::None;
    bool begun_=false,busy_=false,notifying_=false,ready_=false,audioRetired_=false;
    /** @brief Безусловный owner live stage; fault/invalidate/begin не отзывает borrow до finalize/joined abandon. */
    AudioStageToken* audioStageOwner_=nullptr;
    /** @brief Protocol state дополняет owner; layout одинаков при PRIVATE option ON/OFF. */
    AudioStagePhase audioStageState_=AudioStagePhase::Empty;
    Counters counters_{};
};
}
