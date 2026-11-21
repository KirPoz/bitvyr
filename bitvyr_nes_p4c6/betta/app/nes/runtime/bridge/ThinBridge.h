#pragma once
#include "ThinPpu.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nes::bridge {
using namespace nes::core;

/**
 * @brief Результат операции очереди/endpoint: Published означает копию, Ready — подтверждённое завершение, Full/Empty/Pending не означают fault или право free.
 */
enum class Result:std::uint8_t {Published,Ready,Pending,Full,Empty,Stopped,Fault,Stale,Busy};

/**
 * @brief Sticky32-битная причина отказа сеанса bridge; первая ошибка сохраняется до quiescent initialize.
 */
enum class Fault:std::uint32_t {None,Binding,Lease,Time,Sequence,Event,Response,Receiver,Cancelled,Timeout,Wrap};

/**
 * @brief Caller-owned диапазон с точным байтовым extent для проверки alias.
 * @details Тип памяти и lifetime проверяет App отдельно.
 */
struct Span {const void* data=nullptr;std::size_t bytes=0;};

/**
 * @brief Квитанция копии:32-битные session/position и монотонный64-битный serial.
 * @details Не является RGB/PPA lease.
 */
struct Ticket {std::uint32_t session=0,position=0;std::uint64_t serial=0;};

/**
 * @brief Тип единственного сообщения кольца: Begin, завершённый Prefix, once-only Read либо финальный Commit.
 */
enum class Message:std::uint8_t {Begin,Prefix,Read,Commit};
// Обычный payload: чтение лишь после head acquire, перезапись лишь после tail acquire.

/**
 * @brief Обычный immutable payload header.
 * @details Читается только после head acquire и не перезаписывается до tail acquire; events находятся в соответствующем отдельном сегменте.
 */
struct Slot {
    ColourLease lease{};ColourCut base{},end{};
    std::uint64_t sequence=0,request=0,serial=0;
    std::uint32_t count=0;std::uint8_t reg=0;Message kind=Message::Begin;
};

/**
 * @brief Один mailbox ответ C0 на outstanding read.
 * @details Публикуется replyReady release и потребляется один раз producer acquire-путём.
 */
struct Reply {ThinReadReply value{};};
// App размещает control во внутренней RAM, не PSRAM.64-битных атомиков нет.

/**
 * @brief Caller размещает выровненный64-байтный control во внутренней RAM.
 * @details Используются только lock-free32-битные атомики; retire serial передаётся согласованными low/high частями, не atomic64.
 */
struct alignas(64) Control {
    std::atomic<std::uint32_t> state{2},session{0},head{0},tail{0},fault{0};
    std::atomic<std::uint32_t> producerParked{1},consumerParked{1},replyReady{0};
    std::atomic<std::uint32_t> retiredLow{0},retiredHigh{0};
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,"bridge requires lock-free32 controls");

/**
 * @brief Не владеющие памятью указатели на control, power-of-two slots, per-slot event-сегменты и reply.
 * @details Все allocations неподвижны и живы до обоих park/join.
 */
struct Storage {
    Control* control=nullptr;Slot* slots=nullptr;std::uint32_t slotCount=0;
    ColourEvent* events=nullptr;std::uint32_t eventsPerSlot=0;
    Reply* reply=nullptr;
};

/**
 * @brief Единственный producer endpoint своего owner-потока.
 * @details Публикует копии и once-only read, сохраняя local cut/sequence; обычные поля не читаются чужим owner.
 */
class Producer;
               /**
                * @brief Единственный C0 consumer своего потока.
                * @details Проверяет сообщения и вызывает реальный sink последовательно; после callback сам выполняет park/invalidate.
                */
               class Consumer;

/**
 * @brief Bounded SPSC copied-prefix канал с session-tagged остановкой, guards и проверкой declared spans.
 * @details Не выделяет heap, не тактирует гостя и не узнаёт неуказанные caller allocations.
 */
class Channel final {
public:
    
    /**
     * @brief Сохраняет caller-owned указатели storage без выделения памяти.
     * @details Storage остаётся неподвижным и живым до обоих park и завершения owner loops.
     * @param[in] storage Выровненные control/slots/events/reply; INTERNAL placement control обеспечивает App.
     */
    explicit Channel(Storage storage)noexcept:storage_(storage){}
    
    /**
     * @brief Запрещает копировать endpoint/channel вместе с его атомарным и ordinary ownership.
     * @details Caller создаёт отдельный объект только по lifecycle contract.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Источник запрещённого копирования; никакое владение не переносится.
     */
    Channel(const Channel&)=delete;
                                   /**
                                    * @brief Запрещает копировать endpoint/channel вместе с его атомарным и ordinary ownership.
                                    * @details Caller создаёт отдельный объект только по lifecycle contract.
                                    * @par Безымянные параметры в порядке объявления
                                    * @li 1 — Источник запрещённого копирования; никакое владение не переносится.
                                    */
                                   Channel&operator=(const Channel&)=delete;
    // Первичный setup либо после park ОБОИХ endpoints. Extra spans обязаны описывать
    // actor/output/table и другое caller storage, которое payload не может перекрывать.
    
    /**
     * @brief На quiescent setup инициализирует новую session на reset7.
     * @details Отказывает при активных/неприпаркованных endpoints, wrap или alias; actors и ROM caller сбрасывает до нового bind.
     * @param[in] extra Массив всех дополнительных живых actor/output/table spans, с которыми transport storage не должен пересекаться; nullptr только при count0.
     * @param[in] count Число Span в extra, не число байтов; caller обязан перечислить свои объекты.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Неизменный producer/ROM/region/generation/epoch lease; память ROM здесь не приобретается.
     * @li 2 — Уже materialized reset7 CPU/raster/phase cut для указанного региона.
     */
    Result initialize(const ColourLease&,const ColourCut&,const Span* extra=nullptr,std::size_t count=0)noexcept;
    
    /**
     * @brief Выполняет session-tagged sticky stop через собственный abort guard.
     * @details Stale session не может остановить новый запуск.
     * @param[in] session Ожидаемый bridge session из ticket/endpoint, не transport epoch lease.
     * @param[in] reason Причина остановки; default Cancelled не означает завершение Worker.
     */
    Result abort(std::uint32_t session,Fault reason=Fault::Cancelled)noexcept;
    
    /**
     * @brief Acquire-чтением возвращает первую bridge Fault; плохая control-привязка возвращает Binding.
     * @details Это не разрешение освободить pending payload.
     */
    Fault fault()const noexcept;
    // Только advisory-пустота; для free дополнительно нужны оба
    // parked owner и реальные joined/ended owner loops, а не один этот snapshot.
    
    /**
     * @brief Возвращает только advisory snapshot пустоты очереди/reply и отсутствия active guards.
     * @details Для reset/free дополнительно обязательны parked endpoints и реально завершённые owner loops.
     */
    bool drained()const noexcept;
    
    /**
     * @brief Возвращает acquire32-bit session либо0 при плохой control-привязке.
     * @details Session tag защищает от позднего abort/receipt старого запуска.
     */
    std::uint32_t session()const noexcept;
private:
    friend class Producer;friend class Consumer;
    static constexpr std::uint32_t live=1,stopping=2,producerActive=4,consumerActive=8,initializing=16,abortActive=32;
    
    /**
     * @brief CAS-захватом исключает reentrancy своего endpoint и гонку initialize.
     * @details Peer endpoint не блокирует; guard должен быть снят до caller wait.
     * @param[in] bit Бит собственной activity: producer, consumer либо abort.
     * @param[in] session Ожидаемый32-битный номер bridge session; не ColourLease::epoch и не номер гостевого кадра.
     * @param[in] allowStop Разрешает безопасный stop/park вход после stopping; ordinary publish его не использует.
     */
    Result enter(std::uint32_t bit,std::uint32_t session,bool allowStop=false)noexcept;
    
    /**
     * @brief Release-записью снимает ранее захваченный activity-bit.
     * @details Не публикует payload и не подтверждает park другого владельца.
     * @param[in] bit Тот же activity-bit, который текущий метод успешно захватил enter.
     */
    void leave(std::uint32_t bit)noexcept;
    
    /**
     * @brief Сохраняет sticky-причину через stop и возвращает Result::Fault для раннего выхода.
     * @details Не отменяет активный callback и не освобождает storage.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Причина первого протокольного отказа.
     */
    Result fail(Fault)noexcept;
    
    /**
     * @brief Сохраняет первую причину и устанавливает stopping атомарно.
     * @details Память, endpoints и C0 callback остаются живыми до park/join.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Sticky-причина; None не перезаписывает fault, но stopping всё равно устанавливается.
     */
    void stop(Fault)noexcept;
    
    /**
     * @brief Копирует Slot/events и release-публикует head только при свободной очереди.
     * @details Tail acquire запрещает перезаписать ещё используемый C0 payload; Full не является успешной публикацией.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Подготовленный header с уже назначенным Producer publication serial; Channel его не меняет.
     * @li 2 — Источник count событий, живой до окончания копирования.
     * @li 3 — Session/position/serial квитанция только при Published.
     */
    Result publish(Slot&,const ColourEvent*,Ticket&)noexcept;
    
    /**
     * @brief Проверяет размеры/выравнивание и попарное непересечение storage с ROM/self/extra.
     * @details Не подменяет SDK INTERNAL/caps проверки App и не знает неуказанные живые аллокации.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Lease с ROM extent для проверки storage alias.
     * @li 2 — Дополнительные caller-owned actor/output/table spans.
     * @li 3 — Число extra spans; при ненулевом count требуется живой массив.
     */
    bool storageValid(const ColourLease&,const Span*,std::size_t)const noexcept;
    
    /**
     * @brief Проверяет выравнивание и непересечение копируемого source-prefix с transport storage.
     * @details При count0 указатель не разыменовывается.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Живой массив ColourEvent, который будет скопирован до возврата publish.
     * @li 2 — Число событий; байтовый extent равен count*sizeof(ColourEvent).
     */
    bool sourceValid(const ColourEvent*,std::uint32_t)const noexcept;
    Storage storage_;ColourLease lease_{};ColourCut initial_{};bool initialized_=false;
};

/**
 * @brief Единственный producer endpoint своего owner-потока.
 * @details Публикует копии и once-only read, сохраняя local cut/sequence; обычные поля не читаются чужим owner.
 */
class Producer final {
public:
    
    /**
     * @brief Привязывает один SPSC producer endpoint к неподвижному Channel.
     * @details Объект не должен копироваться или освобождаться до своего park/join.
     * @param[in] channel Caller-owned живой Channel и его storage.
     */
    explicit Producer(Channel& channel)noexcept:channel_(channel){}
    
    /**
     * @brief Запрещает копировать endpoint/channel вместе с его атомарным и ordinary ownership.
     * @details Caller создаёт отдельный объект только по lifecycle contract.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Источник запрещённого копирования; никакое владение не переносится.
     */
    Producer(const Producer&)=delete;
                                     /**
                                      * @brief Запрещает копировать endpoint/channel вместе с его атомарным и ordinary ownership.
                                      * @details Caller создаёт отдельный объект только по lifecycle contract.
                                      * @par Безымянные параметры в порядке объявления
                                      * @li 1 — Источник запрещённого копирования; никакое владение не переносится.
                                      */
                                     Producer&operator=(const Producer&)=delete;
    
    /**
     * @brief На owner-потоке присоединяет единственного producer к текущей session и сбрасывает локальные sequence/cut/request.
     * @details Повторный либо чужой attach отказывает.
     */
    Result attach()noexcept;
    
    /**
     * @brief Публикует начальный begin без исполнения C0.
     * @details При Published caller ещё должен получить настоящий receipt ACK перед handoff/bootstrap завершением.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Lease, точно совпадающий с initialize.
     * @li 2 — Точный reset7 cut инициализации.
     * @li 3 — Квитанция begin при Published; Full/Pending не подтверждают callback.
     */
    Result begin(const ColourLease&,const ColourCut&,Ticket&)noexcept;
    
    /**
     * @brief Проверяет и копирует завершённый prefix, продвигая local expected/sequence только после публикации.
     * @details Не ждёт выполнения C0 и не передаёт future budget.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Prefix с тем же lease, текущим base и уже завершённым end; events живы до возврата.
     * @li 2 — Квитанция копии при Published.
     */
    Result prefix(const ThinPrefix&,Ticket&)noexcept;
    
    /**
     * @brief Публикует один read-mailbox запрос после всех предшествующих prefix.
     * @details Второй outstanding read запрещён; результат получают отдельно через reply.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Lease/cut/post-prefix sequence/request-id и регистр0..7.
     * @li 2 — Квитанция опубликованного запроса, не сам read-byte.
     */
    Result read(const ThinReadRequest&,Ticket&)noexcept;
    
    /**
     * @brief Публикует окончание следующего кадра только при согласованных expected cut/frame/sequence.
     * @details До receipt ACK нельзя считать C0 output проверенным.
     * @param[in] sequence Текущая post-prefix sequence.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Тот же неизменный lease.
     * @li 2 — Точный завершённый end cut, ожидаемый после последнего prefix.
     * @li 4 — Квитанция финального commit при Published.
     */
    Result commit(const ColourLease&,const ColourCut&,std::uint64_t sequence,Ticket&)noexcept;
    
    /**
     * @brief Проверяет session/ticket и retired serial acquire-чтениями32-битных частей.
     * @details Ready подтверждает выполненный consumer callback; Pending/Full не разрешают reuse или публикацию RGB.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Квитанция конкретной публикации с session, позицией и монотонным64-битным serial.
     */
    Result receipt(Ticket)noexcept;
    
    /**
     * @brief Acquire-получает ровно один ответ и сверяет всю pending identity до освобождения mailbox.
     * @details Повторное чтение ответа невозможно после успешного Ready.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Авторитетный ThinReadReply при Ready; Pending не содержит пригодного результата.
     */
    Result reply(ThinReadReply&)noexcept; // Поглощает ровно один готовый ответ.
    
    /**
     * @brief После stopping помечает producer parked и снимает outstanding ownership.
     * @details Caller всё равно завершает реальный owner loop; метод не освобождает storage.
     */
    Result park()noexcept; // Только после stop; caller-ресурсы не освобождает.
    
    /**
     * @brief На собственном attached endpoint session-tagged останавливает Channel.
     * @details Не вызывает C0 invalidate и не отменяет исполняющийся receiver.
     * @param[in] reason Первая sticky-причина остановки, по умолчанию Cancelled.
     */
    void stop(Fault reason=Fault::Cancelled)noexcept;
private:
    
    /**
     * @brief Публикует подготовленное сообщение через Channel и продвигает issued serial только при Published.
     * @details Работает под уже захваченным producer guard.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Подготовленный локальный Slot с identity сообщения.
     * @li 2 — Живые events для немедленной копии либо nullptr при count0.
     * @li 3 — Квитанция именно успешной публикации.
     */
    Result publish(Slot&,const ColourEvent*,Ticket&)noexcept;
    Channel&channel_;ColourLease lease_{};ColourCut expected_{};
    ThinReadRequest pending_{};
    std::uint64_t sequence_=0,request_=0,frame_=0,issued_=0;
    std::uint32_t session_=0;bool attached_=false,begun_=false,outstanding_=false;
};

/**
 * @brief Единственный C0 consumer своего потока.
 * @details Проверяет сообщения и вызывает реальный sink последовательно; после callback сам выполняет park/invalidate.
 */
class Consumer final {
public:
    
    /**
     * @brief Привязывает SPSC consumer к Channel и единственному C0 sink.
     * @details Callback исполняется только на этом owner-потоке, без reentrant pump.
     * @param[in] channel Тот же неподвижный Channel, что у producer.
     * @param[in] receiver Живой реальный C0 ThinPpuSink, не тактирующий producer CPU/APU и не меняющий его lease.
     */
    Consumer(Channel& channel,ThinPpuSink& receiver)noexcept:channel_(channel),receiver_(receiver){}
    
    /**
     * @brief Запрещает копировать endpoint/channel вместе с его атомарным и ordinary ownership.
     * @details Caller создаёт отдельный объект только по lifecycle contract.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Источник запрещённого копирования; никакое владение не переносится.
     */
    Consumer(const Consumer&)=delete;
                                     /**
                                      * @brief Запрещает копировать endpoint/channel вместе с его атомарным и ordinary ownership.
                                      * @details Caller создаёт отдельный объект только по lifecycle contract.
                                      * @par Безымянные параметры в порядке объявления
                                      * @li 1 — Источник запрещённого копирования; никакое владение не переносится.
                                      */
                                     Consumer&operator=(const Consumer&)=delete;
    
    /**
     * @brief На C0 owner-потоке присоединяется к текущей session и исходному cut.
     * @details Повторное присоединение занятого consumer отвергается.
     */
    Result attach()noexcept;
    
    /**
     * @brief Acquire-чтением берёт одно immutable сообщение и проверяет identity/order/extent до реального C0 callback.
     * @details Read выполняется once-only; reply и retired ACK публикуются лишь при успехе и отсутствии stop; ожиданий/heap здесь нет.
     */
    Result pump()noexcept; // Одно immutable сообщение prefix/control; без ожиданий/аллокаций.
    
    /**
     * @brief На том же C0 owner после stopping инвалидирует receiver не более одного раза, затем публикует parked.
     * @details Вызывать лишь между завершёнными pump callback, до free/reset actors.
     */
    Result park()noexcept; // Инвалидирует receiver на ЭТОМ owner до quiescence.
private:
    Channel&channel_;ThinPpuSink&receiver_;ColourLease lease_{};ColourCut expected_{};
    std::uint64_t sequence_=0,request_=0,frame_=0,received_=0;
    std::uint32_t session_=0;bool attached_=false,begun_=false,invalidated_=false;
};
// Необязательная совместимость с синхронным native sink. Переданный caller
// wait может spin/yield/pump и обязан вернуть false по своему deadline.
// В adapter нет RTOS rendezvous, sleep, guest clock increment или таймера.

/**
 * @brief Совместимый ThinPpuSink-адаптер: prefix асинхронный, begin/read/commit ожидают фактического consumer результата через caller wait.
 * @details Не является RTOS транспортом или обычным per-clock RPC.
 */
class Sink final:public ThinPpuSink {
public:
    using Wait=bool(*)(void*)noexcept;
    
    /**
     * @brief Создаёт синхронно совместимую оболочку над asynchronous producer.
     * @details Wait callback задаёт caller; собственных часов, RTOS sleep или гостевых clocks здесь нет.
     * @param[in] producer Живой единственный producer endpoint.
     * @param[in] wait Callback spin/yield либо Main-bootstrap pump; false означает собственный deadline/отказ caller.
     * @param[in] context Заимствованный wait context, живой до окончания всех синхронных методов.
     */
    Sink(Producer& producer,Wait wait,void* context)noexcept:producer_(producer),wait_(wait),context_(context){}
    
    /**
     * @brief Публикует begin и ждёт настоящий receipt ACK через caller wait.
     * @details Во время Main-bootstrap wait вправе pump C0; Worker wait этого делать не вправе.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Неизменный lease setup.
     * @li 2 — Завершённый reset7 CPU/raster/phase cut.
     */
    bool begin(const ColourLease&,const ColourCut&)noexcept override;
    
    /**
     * @brief Повторяет попытку лишь при Full и возвращает после копии prefix, без C0 ACK.
     * @details Source events можно переиспользовать только после true.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Живой завершённый prefix без read-записей и будущего бюджета.
     */
    bool prefix(const ThinPrefix&)noexcept override;
    
    /**
     * @brief Публикует запрос и ждёт единственный проверенный reply через caller wait.
     * @details Возвращает false при fault/deadline, не предсказывая прочитанный байт.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Запрос с текущими lease/cut/sequence/request-id и регистром0..7.
     * @li 2 — Авторитетный C0 ответ только при true.
     */
    bool read(const ThinReadRequest&,ThinReadReply&)noexcept override;
    
    /**
     * @brief Публикует финальный commit и ждёт реальный C0 ACK перед разрешением Core output/audio commit.
     * @details Отдельный App PPA lease остаётся обязательным.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Тот же неизменный lease.
     * @li 2 — Завершённая финальная CPU-cycle/raster/phase граница кадра.
     * @li 3 — Post-prefix sequence, которую подтверждает consumer.
     */
    bool commit(const ColourLease&,const ColourCut&,std::uint64_t)noexcept override;
    
    /**
     * @brief Останавливает producer как Cancelled и не вызывает C0 actor с чужого потока.
     * @details Реальный consumer invalidate выполняет только его park.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Core ColourFault-сигнал; этот adapter намеренно преобразует его в bridge Cancelled.
     */
    void invalidate(ColourFault)noexcept override{producer_.stop(Fault::Cancelled);}
private:
    
    /**
     * @brief Вызывает только предоставленный wait callback.
     * @details Его false/отсутствие устанавливает sticky Timeout; сам adapter не выбирает tick или clock policy.
     */
    bool pause()noexcept;
    
    /**
     * @brief Ожидает Ready именно данной публикации, повторяя лишь Pending через wait.
     * @details Поздняя/чужая квитанция не принимается.
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Квитанция begin/commit с текущей session и publication serial.
     */
    bool acknowledged(Ticket)noexcept;
    Producer&producer_;Wait wait_;void*context_;
};
}
