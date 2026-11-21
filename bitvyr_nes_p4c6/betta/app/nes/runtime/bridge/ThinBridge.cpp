#include "ThinBridge.h"
#include <algorithm>
#include <limits>

namespace nes::bridge {
namespace {
constexpr auto max32=std::numeric_limits<std::uint32_t>::max();
constexpr auto max64=std::numeric_limits<std::uint64_t>::max();





/**
 * @brief Проверяет raster/phase пределы cut для PAL или NTSC.
 * @details Само соответствие cycle ожидаемой завершённой границе проверяется вызывающим протоколом.
 * @param[in] c Завершённый CPU-cycle/raster/phase cut для проверки полей.
 * @param[in] r Регион lease: PAL допускает scanline до310, NTSC до260; dot до340, phase меньше5.
 */
bool cut(const ColourCut&c,Region r)noexcept{return c.scanline>=-1&&c.scanline<=(r==Region::Pal?310:260)&&c.dot<=340&&c.phase<5;}

/**
 * @brief Проверяет ненулевой Span и отсутствие переполнения его верхней адресной границы.
 * @details Доступность памяти и срок жизни остаются контрактом caller.
 * @param[in] s Caller-owned диапазон с длиной в байтах, проверяемый до указательной арифметики.
 */
bool valid(Span s)noexcept{const auto p=reinterpret_cast<std::uintptr_t>(s.data);return p&&s.bytes&&s.bytes<=std::numeric_limits<std::uintptr_t>::max()-p;}

/**
 * @brief Проверяет пересечение двух уже арифметически валидных диапазонов.
 * @details Касание исключающих верхних границ не является пересечением.
 * @param[in] a Первый Span, предварительно прошедший valid.
 * @param[in] b Второй такой Span; размер в байтах.
 */
bool overlap(Span a,Span b)noexcept{const auto x=reinterpret_cast<std::uintptr_t>(a.data),y=reinterpret_cast<std::uintptr_t>(b.data);return x<y+b.bytes&&y<x+a.bytes;}

/**
 * @brief Допускает только PPU Write, CartWrite и OAM DMA записи prefix.
 * @details Чтения передаются отдельным once-only запросом, не в этом массиве.
 * @param[in] e Одно событие с kind/address; cycleDelta порядок проверяет prefixValid.
 */
bool event(const ColourEvent&e)noexcept{
    return(e.kind==ColourKind::Write&&e.address<8)||(e.kind==ColourKind::CartWrite&&e.address>=0x4020)||(e.kind==ColourKind::Dma&&e.address==0x2004)||
        (e.kind==ColourKind::Mmc5CpuStrobe&&(e.address==0xfffaU||e.address==0xfffbU||e.address==0x2002U||e.address==0x5204U))||
        (e.kind==ColourKind::Mmc5DmcStrobe&&(e.address==0xfffaU||e.address==0xfffbU))||
        (e.kind==ColourKind::Mmc5SignalWrite&&(e.address==0x2000U||e.address==0x2001U||e.address==0x4014U))||
        (e.kind==ColourKind::Mmc5IdleReset&&e.address==0U)||
        (e.kind==ColourKind::Mmc5IdleDmcStrobe&&(e.address==0xfffaU||e.address==0xfffbU));
}

/**
 * @brief Проверяет завершённый prefix относительно ожидаемого cut, лимита и региона до исполнения C0.
 * @details Учитывает sequence-внешние cut, PAL phase, порядок events и предел45000 CPU cycles.
 * @param[in] p Заимствованный prefix, event-буфер должен быть живым на время проверки.
 * @param[in] expected Ранее принятый завершённый base cut с абсолютными CPU cycles.
 * @param[in] capacity Максимальное число ColourEvent в слоте, не байтовая ёмкость.
 * @param[in] region Регион lease для raster и15/16-к5 phase-арифметики.
 */
bool prefixValid(const ThinPrefix&p,const ColourCut&expected,std::uint32_t capacity,Region region)noexcept{
    if(!nes::core::sameColourCut(p.base,expected)||!cut(p.end,region)||p.end.cycle<p.base.cycle||p.end.cycle-p.base.cycle>45000||p.end.frame<p.base.frame||p.end.frame-p.base.frame>1||p.count>capacity||(p.count&&!p.events))return false;
    if((p.base.phase+(p.end.cycle-p.base.cycle)*(region==Region::Pal?16U:15U))%5U!=p.end.phase)return false;
    std::uint32_t last=0;
    for(std::uint32_t i=0;i<p.count;++i){const auto&e=p.events[i];if(!event(e)||e.cycleDelta<last||e.cycleDelta>p.end.cycle-p.base.cycle)return false;last=e.cycleDelta;}
    return true;
}

/**
 * @brief Переводит внутреннюю sticky Fault в причину инвалидирования Core colour actor.
 * @details Не вызывает actor и не очищает fault.
 * @param[in] f Bridge Fault текущей session для сопоставления с ColourFault.
 */
ColourFault colour(Fault f)noexcept{
    if(f==Fault::Lease)return ColourFault::Lease;
    if(f==Fault::Time)return ColourFault::Time;
    if(f==Fault::Sequence||f==Fault::Wrap)return ColourFault::Sequence;
    if(f==Fault::Event)return ColourFault::Event;
    if(f==Fault::Receiver||f==Fault::Response)return ColourFault::Replay;
    return ColourFault::Cancelled;
}
}
// Методы endpoint явно снимают guards; внешний lock не удерживается
// во время caller wait. Битовые guards исключают initialize, но не peer.

/**
 * @brief CAS-захватом исключает reentrancy своего endpoint и гонку initialize.
 * @details Peer endpoint не блокирует; guard должен быть снят до caller wait.
 * @param[in] bit Бит собственной activity: producer, consumer либо abort.
 * @param[in] epoch Ожидаемый32-битный номер bridge session; не ColourLease::epoch и не номер гостевого кадра.
 * @param[in] allowStop Разрешает безопасный stop/park вход после stopping; ordinary publish его не использует.
 */
Result Channel::enter(std::uint32_t bit,std::uint32_t epoch,bool allowStop)noexcept{
    if(!storage_.control||reinterpret_cast<std::uintptr_t>(storage_.control)%alignof(Control))return Result::Fault;
    auto&c=*storage_.control;auto state=c.state.load(std::memory_order_acquire);
    for(;;){
        if(!(state&live)||(state&initializing)||((state&stopping)&&!allowStop))return c.fault.load(std::memory_order_acquire)?Result::Fault:Result::Stopped;
        if(state&bit)return Result::Busy;
        if(c.state.compare_exchange_weak(state,state|bit,std::memory_order_acq_rel,std::memory_order_acquire)){
            if(c.session.load(std::memory_order_acquire)!=epoch){leave(bit);return Result::Stale;}
            if(!allowStop&&c.fault.load(std::memory_order_acquire)){leave(bit);return Result::Fault;}return Result::Ready;
        }
    }
}

/**
 * @brief Release-записью снимает ранее захваченный activity-bit.
 * @details Не публикует payload и не подтверждает park другого владельца.
 * @param[in] bit Тот же activity-bit, который текущий метод успешно захватил enter.
 */
void Channel::leave(std::uint32_t bit)noexcept{storage_.control->state.fetch_and(~bit,std::memory_order_release);}

/**
 * @brief Acquire-чтением возвращает первую bridge Fault; плохая control-привязка возвращает Binding.
 * @details Это не разрешение освободить pending payload.
 */
Fault Channel::fault()const noexcept{if(!storage_.control||reinterpret_cast<std::uintptr_t>(storage_.control)%alignof(Control))return Fault::Binding;return static_cast<Fault>(storage_.control->fault.load(std::memory_order_acquire));}

/**
 * @brief Возвращает acquire32-bit session либо0 при плохой control-привязке.
 * @details Session tag защищает от позднего abort/receipt старого запуска.
 */
std::uint32_t Channel::session()const noexcept{if(!storage_.control||reinterpret_cast<std::uintptr_t>(storage_.control)%alignof(Control))return 0;return storage_.control->session.load(std::memory_order_acquire);}

/**
 * @brief Сохраняет первую причину и устанавливает stopping атомарно.
 * @details Память, endpoints и C0 callback остаются живыми до park/join.
 * @param[in] f Sticky-причина; None не перезаписывает fault, но stopping всё равно устанавливается.
 */
void Channel::stop(Fault f)noexcept{
    if(!storage_.control||reinterpret_cast<std::uintptr_t>(storage_.control)%alignof(Control))return;
    if(f!=Fault::None){std::uint32_t empty=0;storage_.control->fault.compare_exchange_strong(empty,static_cast<std::uint32_t>(f),std::memory_order_acq_rel);}
    storage_.control->state.fetch_or(stopping,std::memory_order_acq_rel);
}

/**
 * @brief Выполняет session-tagged sticky stop через собственный abort guard.
 * @details Stale session не может остановить новый запуск.
 * @param[in] epoch Ожидаемый bridge session из ticket/endpoint, не transport epoch lease.
 * @param[in] reason Причина остановки; default Cancelled не означает завершение Worker.
 */
Result Channel::abort(std::uint32_t epoch,Fault reason)noexcept{
    const auto r=enter(abortActive,epoch,true);if(r!=Result::Ready)return r;
    stop(reason);leave(abortActive);return Result::Ready;
}

/**
 * @brief Сохраняет sticky-причину через stop и возвращает Result::Fault для раннего выхода.
 * @details Не отменяет активный callback и не освобождает storage.
 * @param[in] f Причина первого протокольного отказа.
 */
Result Channel::fail(Fault f)noexcept{stop(f);return Result::Fault;}

/**
 * @brief Возвращает только advisory snapshot пустоты очереди/reply и отсутствия active guards.
 * @details Для reset/free дополнительно обязательны parked endpoints и реально завершённые owner loops.
 */
bool Channel::drained()const noexcept{
    if(!storage_.control||reinterpret_cast<std::uintptr_t>(storage_.control)%alignof(Control))return false;
    auto&c=*storage_.control;return c.head.load(std::memory_order_acquire)==c.tail.load(std::memory_order_acquire)&&!c.replyReady.load(std::memory_order_acquire)&&!(c.state.load(std::memory_order_acquire)&(producerActive|consumerActive|abortActive));
}

/**
 * @brief Проверяет размеры/выравнивание и попарное непересечение storage с ROM/self/extra.
 * @details Не подменяет SDK INTERNAL/caps проверки App и не знает неуказанные живые аллокации.
 * @param[in] lease Lease с ROM extent для проверки storage alias.
 * @param[in] extra Дополнительные caller-owned actor/output/table spans.
 * @param[in] count Число extra spans; при ненулевом count требуется живой массив.
 */
bool Channel::storageValid(const ColourLease&lease,const Span*extra,std::size_t count)const noexcept{
    const auto&s=storage_;if(!s.control||!s.slots||!s.events||!s.reply||s.slotCount<2||s.slotCount>1024||(s.slotCount&(s.slotCount-1))||!s.eventsPerSlot||s.eventsPerSlot>4096||std::uint64_t(s.slotCount)*s.eventsPerSlot>65536||(count&&!extra))return false;
    if(reinterpret_cast<std::uintptr_t>(s.control)%alignof(Control)||reinterpret_cast<std::uintptr_t>(s.slots)%alignof(Slot)||reinterpret_cast<std::uintptr_t>(s.events)%alignof(ColourEvent)||reinterpret_cast<std::uintptr_t>(s.reply)%alignof(Reply))return false;
    Span spans[]={{s.control,sizeof(Control)},{s.slots,sizeof(Slot)*s.slotCount},{s.events,sizeof(ColourEvent)*s.slotCount*s.eventsPerSlot},{s.reply,sizeof(Reply)}};
    Span rom{lease.rom,lease.romBytes},self{this,sizeof(*this)};
    if(!valid(rom))return false;
    for(unsigned i=0;i<4;++i){if(!valid(spans[i])||overlap(spans[i],rom)||overlap(spans[i],self))return false;for(unsigned j=0;j<i;++j)if(overlap(spans[i],spans[j]))return false;for(std::size_t k=0;k<count;++k)if(!valid(extra[k])||overlap(spans[i],extra[k]))return false;}
    return true;
}

/**
 * @brief Проверяет выравнивание и непересечение копируемого source-prefix с transport storage.
 * @details При count0 указатель не разыменовывается.
 * @param[in] events Живой массив ColourEvent, который будет скопирован до возврата publish.
 * @param[in] count Число событий; байтовый extent равен count*sizeof(ColourEvent).
 */
bool Channel::sourceValid(const ColourEvent*events,std::uint32_t count)const noexcept{
    if(!count)return true;
    Span input{events,sizeof(ColourEvent)*std::size_t(count)};
    if(!valid(input)||reinterpret_cast<std::uintptr_t>(events)%alignof(ColourEvent))return false;
    const auto&s=storage_;
    Span spans[]={{s.control,sizeof(Control)},{s.slots,sizeof(Slot)*s.slotCount},{s.events,sizeof(ColourEvent)*s.slotCount*s.eventsPerSlot},{s.reply,sizeof(Reply)},{this,sizeof(*this)}};
    for(const auto&span:spans)if(overlap(input,span))return false;
    return true;
}

/**
 * @brief На quiescent setup инициализирует новую session на reset7.
 * @details Отказывает при активных/неприпаркованных endpoints, wrap или alias; actors и ROM caller сбрасывает до нового bind.
 * @param[in] lease Неизменный producer/ROM/region/generation/epoch lease; память ROM здесь не приобретается.
 * @param[in] initial Уже materialized reset7 CPU/raster/phase cut для указанного региона.
 * @param[in] extra Массив всех дополнительных живых actor/output/table spans, с которыми transport storage не должен пересекаться; nullptr только при count0.
 * @param[in] count Число Span в extra, не число байтов; caller обязан перечислить свои объекты.
 */
Result Channel::initialize(const ColourLease&lease,const ColourCut&initial,const Span*extra,std::size_t count)noexcept{
    if(!lease.producer||!lease.rom||lease.romBytes<16||(lease.region!=Region::Ntsc&&lease.region!=Region::Pal)||!storageValid(lease,extra,count))return Result::Fault;
    if(initial.cycle!=7||initial.frame||initial.scanline!=-1||initial.dot!=(lease.region==Region::Pal?22:21)||initial.phase!=(lease.region==Region::Pal?2:0))return Result::Fault;
    auto&c=*storage_.control;auto state=c.state.load(std::memory_order_acquire);
    if(!(state&stopping)||(state&(producerActive|consumerActive|initializing|abortActive))||!c.producerParked.load(std::memory_order_acquire)||!c.consumerParked.load(std::memory_order_acquire))return Result::Busy;
    if(!c.state.compare_exchange_strong(state,initializing,std::memory_order_acq_rel))return Result::Busy;
    const auto epoch=c.session.load(std::memory_order_relaxed);
    if(epoch==max32||(initialized_&&lease.epoch==lease_.epoch&&lease.generation==lease_.generation)){c.state.store(state,std::memory_order_release);return Result::Stale;}
    lease_=lease;initial_=initial;initialized_=true;
    c.head.store(0,std::memory_order_relaxed);c.tail.store(0,std::memory_order_relaxed);c.replyReady.store(0,std::memory_order_relaxed);c.fault.store(0,std::memory_order_relaxed);c.retiredLow.store(0,std::memory_order_relaxed);c.retiredHigh.store(0,std::memory_order_relaxed);
    c.session.store(epoch+1,std::memory_order_relaxed);c.state.store(live,std::memory_order_release);return Result::Ready;
}

/**
 * @brief Копирует Slot/events и release-публикует head только при свободной очереди.
 * @details Tail acquire запрещает перезаписать ещё используемый C0 payload; Full не является успешной публикацией.
 * @param[in] slot Подготовленный header с уже назначенным Producer publication serial; Channel его не меняет.
 * @param[in] events Источник count событий, живой до окончания копирования.
 * @param[out] ticket Session/position/serial квитанция только при Published.
 */
Result Channel::publish(Slot&slot,const ColourEvent*events,Ticket&ticket)noexcept{
    auto&c=*storage_.control;const auto h=c.head.load(std::memory_order_relaxed),t=c.tail.load(std::memory_order_acquire);
    if(std::uint32_t(h-t)>storage_.slotCount)return fail(Fault::Sequence);
    if(std::uint32_t(h-t)==storage_.slotCount)return Result::Full;
    const auto index=h&(storage_.slotCount-1);
    if(slot.count)std::copy_n(events,slot.count,storage_.events+index*storage_.eventsPerSlot);
    storage_.slots[index]=slot;c.head.store(h+1,std::memory_order_release);ticket={session(),h+1,slot.serial};return Result::Published;
}

#define ENTER_PRODUCER(allow) const auto entered=channel_.enter(Channel::producerActive,session_,allow);if(entered!=Result::Ready)return entered
#define DONE_PRODUCER(value) do{const auto result=(value);channel_.leave(Channel::producerActive);return result;}while(false)

/**
 * @brief На owner-потоке присоединяет единственного producer к текущей session и сбрасывает локальные sequence/cut/request.
 * @details Повторный либо чужой attach отказывает.
 */
Result Producer::attach()noexcept{
    session_=channel_.session();ENTER_PRODUCER(false);auto&c=*channel_.storage_.control;
    if(attached_||!c.producerParked.exchange(0,std::memory_order_acq_rel))DONE_PRODUCER(Result::Busy);
    lease_=channel_.lease_;expected_=channel_.initial_;sequence_=request_=issued_=0;frame_=expected_.frame;begun_=outstanding_=false;attached_=true;DONE_PRODUCER(Result::Ready);
}

/**
 * @brief Публикует подготовленное сообщение через Channel и продвигает issued serial только при Published.
 * @details Работает под уже захваченным producer guard.
 * @param[in,out] slot Подготовленный локальный Slot с identity сообщения.
 * @param[in] events Живые events для немедленной копии либо nullptr при count0.
 * @param[out] ticket Квитанция именно успешной публикации.
 */
Result Producer::publish(Slot&slot,const ColourEvent*events,Ticket&ticket)noexcept{
    if(issued_==max64)return channel_.fail(Fault::Wrap);
    slot.serial=issued_+1;const auto result=channel_.publish(slot,events,ticket);
    if(result==Result::Published)++issued_;
    return result;
}

/**
 * @brief Публикует начальный begin без исполнения C0.
 * @details При Published caller ещё должен получить настоящий receipt ACK перед handoff/bootstrap завершением.
 * @param[in] lease Lease, точно совпадающий с initialize.
 * @param[in] initial Точный reset7 cut инициализации.
 * @param[out] ticket Квитанция begin при Published; Full/Pending не подтверждают callback.
 */
Result Producer::begin(const ColourLease&lease,const ColourCut&initial,Ticket&ticket)noexcept{
    ENTER_PRODUCER(false);if(!attached_||begun_||!nes::core::sameColourLease(lease,lease_)||!nes::core::sameColourCut(initial,expected_))DONE_PRODUCER(channel_.fail(Fault::Lease));
    Slot s{};s.kind=Message::Begin;s.lease=lease;s.base=s.end=initial;const auto r=publish(s,nullptr,ticket);if(r==Result::Published)begun_=true;DONE_PRODUCER(r);
}

/**
 * @brief Проверяет и копирует завершённый prefix, продвигая local expected/sequence только после публикации.
 * @details Не ждёт выполнения C0 и не передаёт future budget.
 * @param[in] p Prefix с тем же lease, текущим base и уже завершённым end; events живы до возврата.
 * @param[out] ticket Квитанция копии при Published.
 */
Result Producer::prefix(const ThinPrefix&p,Ticket&ticket)noexcept{
    ENTER_PRODUCER(false);if(!attached_||!begun_||outstanding_||!nes::core::sameColourLease(p.lease,lease_))DONE_PRODUCER(channel_.fail(Fault::Lease));
    if(p.sequence!=sequence_||sequence_==max64)DONE_PRODUCER(channel_.fail(Fault::Sequence));
    if(p.count>channel_.storage_.eventsPerSlot||!channel_.sourceValid(p.events,p.count)||!prefixValid(p,expected_,channel_.storage_.eventsPerSlot,lease_.region))DONE_PRODUCER(channel_.fail(Fault::Event));
    Slot s{};s.kind=Message::Prefix;s.lease=p.lease;s.base=p.base;s.end=p.end;s.sequence=p.sequence;s.count=p.count;
    const auto r=publish(s,p.events,ticket);if(r==Result::Published){expected_=p.end;++sequence_;}DONE_PRODUCER(r);
}

/**
 * @brief Публикует один read-mailbox запрос после всех предшествующих prefix.
 * @details Второй outstanding read запрещён; результат получают отдельно через reply.
 * @param[in] q Lease/cut/post-prefix sequence/request-id и регистр0..7.
 * @param[out] ticket Квитанция опубликованного запроса, не сам read-byte.
 */
Result Producer::read(const ThinReadRequest&q,Ticket&ticket)noexcept{
    ENTER_PRODUCER(false);if(!attached_||!begun_||outstanding_||!nes::core::sameColourLease(q.lease,lease_))DONE_PRODUCER(channel_.fail(Fault::Lease));
    if(q.sequence!=sequence_||q.request!=request_||request_==max64||q.reg>=8||!nes::core::sameColourCut(q.cut,expected_))DONE_PRODUCER(channel_.fail(Fault::Sequence));
    if(channel_.storage_.control->replyReady.load(std::memory_order_acquire))DONE_PRODUCER(channel_.fail(Fault::Response));
    Slot s{};s.kind=Message::Read;s.lease=q.lease;s.end=q.cut;s.sequence=q.sequence;s.request=q.request;s.reg=q.reg;
    const auto r=publish(s,nullptr,ticket);if(r==Result::Published){pending_=q;outstanding_=true;}DONE_PRODUCER(r);
}

/**
 * @brief Публикует окончание следующего кадра только при согласованных expected cut/frame/sequence.
 * @details До receipt ACK нельзя считать C0 output проверенным.
 * @param[in] lease Тот же неизменный lease.
 * @param[in] end Точный завершённый end cut, ожидаемый после последнего prefix.
 * @param[in] sequence Текущая post-prefix sequence.
 * @param[out] ticket Квитанция финального commit при Published.
 */
Result Producer::commit(const ColourLease&lease,const ColourCut&end,std::uint64_t sequence,Ticket&ticket)noexcept{
    ENTER_PRODUCER(false);if(!attached_||!begun_||outstanding_||!nes::core::sameColourLease(lease,lease_))DONE_PRODUCER(channel_.fail(Fault::Lease));
    if(sequence!=sequence_||!nes::core::sameColourCut(end,expected_)||frame_==max64||end.frame!=frame_+1)DONE_PRODUCER(channel_.fail(Fault::Sequence));
    Slot s{};s.kind=Message::Commit;s.lease=lease;s.end=end;s.sequence=sequence;
    const auto r=publish(s,nullptr,ticket);if(r==Result::Published)frame_=end.frame;DONE_PRODUCER(r);
}

/**
 * @brief Проверяет session/ticket и retired serial acquire-чтениями32-битных частей.
 * @details Ready подтверждает выполненный consumer callback; Pending/Full не разрешают reuse или публикацию RGB.
 * @param[in] ticket Квитанция конкретной публикации с session, позицией и монотонным64-битным serial.
 */
Result Producer::receipt(Ticket ticket)noexcept{
    ENTER_PRODUCER(false);auto&c=*channel_.storage_.control;
    if(!attached_||ticket.session!=session_||!ticket.serial||ticket.serial>issued_)DONE_PRODUCER(Result::Stale);
    std::uint32_t high,low,again;
    do{high=c.retiredHigh.load(std::memory_order_acquire);low=c.retiredLow.load(std::memory_order_acquire);again=c.retiredHigh.load(std::memory_order_acquire);}while(high!=again);
    // Low публикуется до high. При wrap old-high/new-low занижает
    // progress и не может подтвердить serial будущей публикации.
    const auto retired=(std::uint64_t(high)<<32)|low;
    DONE_PRODUCER(retired>=ticket.serial?Result::Ready:Result::Pending);
}

/**
 * @brief Acquire-получает ровно один ответ и сверяет всю pending identity до освобождения mailbox.
 * @details Повторное чтение ответа невозможно после успешного Ready.
 * @param[out] out Авторитетный ThinReadReply при Ready; Pending не содержит пригодного результата.
 */
Result Producer::reply(ThinReadReply&out)noexcept{
    ENTER_PRODUCER(false);if(!attached_||!outstanding_)DONE_PRODUCER(Result::Empty);
    auto&c=*channel_.storage_.control;if(!c.replyReady.load(std::memory_order_acquire))DONE_PRODUCER(Result::Pending);
    const auto value=channel_.storage_.reply->value;
    if(!nes::core::sameColourLease(value.lease,pending_.lease)||!nes::core::sameColourCut(value.cut,pending_.cut)||value.sequence!=pending_.sequence||value.request!=pending_.request)DONE_PRODUCER(channel_.fail(Fault::Response));
    out=value;outstanding_=false;++request_;c.replyReady.store(0,std::memory_order_release);DONE_PRODUCER(Result::Ready);
}

/**
 * @brief После stopping помечает producer parked и снимает outstanding ownership.
 * @details Caller всё равно завершает реальный owner loop; метод не освобождает storage.
 */
Result Producer::park()noexcept{
    ENTER_PRODUCER(true);auto&c=*channel_.storage_.control;if(!(c.state.load(std::memory_order_acquire)&Channel::stopping))DONE_PRODUCER(Result::Busy);
    attached_=false;outstanding_=false;c.producerParked.store(1,std::memory_order_release);DONE_PRODUCER(Result::Ready);
}

/**
 * @brief На собственном attached endpoint session-tagged останавливает Channel.
 * @details Не вызывает C0 invalidate и не отменяет исполняющийся receiver.
 * @param[in] reason Первая sticky-причина остановки, по умолчанию Cancelled.
 */
void Producer::stop(Fault reason)noexcept{
    const auto r=channel_.enter(Channel::producerActive,session_,true);
    if(r!=Result::Ready)return;
    if(attached_)channel_.stop(reason);
    channel_.leave(Channel::producerActive);
}
#undef ENTER_PRODUCER
#undef DONE_PRODUCER

#define ENTER_CONSUMER(allow) const auto entered=channel_.enter(Channel::consumerActive,session_,allow);if(entered!=Result::Ready)return entered
#define DONE_CONSUMER(value) do{const auto result=(value);channel_.leave(Channel::consumerActive);return result;}while(false)

/**
 * @brief На C0 owner-потоке присоединяется к текущей session и исходному cut.
 * @details Повторное присоединение занятого consumer отвергается.
 */
Result Consumer::attach()noexcept{
    session_=channel_.session();ENTER_CONSUMER(false);auto&c=*channel_.storage_.control;
    if(attached_||!c.consumerParked.exchange(0,std::memory_order_acq_rel))DONE_CONSUMER(Result::Busy);
    lease_=channel_.lease_;expected_=channel_.initial_;sequence_=request_=received_=0;frame_=expected_.frame;begun_=invalidated_=false;attached_=true;DONE_CONSUMER(Result::Ready);
}

/**
 * @brief Acquire-чтением берёт одно immutable сообщение и проверяет identity/order/extent до реального C0 callback.
 * @details Read выполняется once-only; reply и retired ACK публикуются лишь при успехе и отсутствии stop; ожиданий/heap здесь нет.
 */
Result Consumer::pump()noexcept{
    ENTER_CONSUMER(false);if(!attached_)DONE_CONSUMER(Result::Stopped);auto&c=*channel_.storage_.control;
    const auto t=c.tail.load(std::memory_order_relaxed),h=c.head.load(std::memory_order_acquire);
    if(std::uint32_t(h-t)>channel_.storage_.slotCount)DONE_CONSUMER(channel_.fail(Fault::Sequence));
    if(h==t)DONE_CONSUMER(Result::Empty);
    const auto index=t&(channel_.storage_.slotCount-1);const auto&s=channel_.storage_.slots[index];
    if(received_==max64||s.serial!=received_+1)DONE_CONSUMER(channel_.fail(Fault::Sequence));
    if(!nes::core::sameColourLease(s.lease,lease_))DONE_CONSUMER(channel_.fail(Fault::Lease));
    bool ok=false;
    switch(s.kind){
    case Message::Begin:
        if(begun_||!nes::core::sameColourCut(s.end,expected_)||!nes::core::sameColourCut(s.base,expected_)||s.count)DONE_CONSUMER(channel_.fail(Fault::Sequence));
        ok=receiver_.begin(lease_,s.end);if(ok)begun_=true;break;
    case Message::Prefix:{
        ThinPrefix p{s.lease,s.base,s.end,s.sequence,channel_.storage_.events+index*channel_.storage_.eventsPerSlot,s.count};
        if(!begun_||s.sequence!=sequence_||sequence_==max64||!prefixValid(p,expected_,channel_.storage_.eventsPerSlot,lease_.region))DONE_CONSUMER(channel_.fail(Fault::Event));
        ok=receiver_.prefix(p);if(ok){expected_=p.end;++sequence_;}break;}
    case Message::Read:{
        if(!begun_||s.sequence!=sequence_||s.request!=request_||request_==max64||!nes::core::sameColourCut(s.end,expected_)||s.reg>=8||s.count||c.replyReady.load(std::memory_order_acquire))DONE_CONSUMER(channel_.fail(Fault::Sequence));
        ThinReadRequest q{s.lease,s.end,s.sequence,s.request,s.reg};ThinReadReply reply{};
        ok=receiver_.read(q,reply);
        if(ok&&(!nes::core::sameColourLease(reply.lease,q.lease)||!nes::core::sameColourCut(reply.cut,q.cut)||reply.sequence!=q.sequence||reply.request!=q.request))DONE_CONSUMER(channel_.fail(Fault::Response));
        if(ok){
            if(c.state.load(std::memory_order_acquire)&Channel::stopping)DONE_CONSUMER(Result::Stopped);
            channel_.storage_.reply->value=reply;c.replyReady.store(1,std::memory_order_release);++request_;
        }break;}
    case Message::Commit:
        if(!begun_||s.sequence!=sequence_||!nes::core::sameColourCut(s.end,expected_)||frame_==max64||s.end.frame!=frame_+1||s.count)DONE_CONSUMER(channel_.fail(Fault::Sequence));
        ok=receiver_.commit(lease_,s.end,s.sequence);if(ok)frame_=s.end.frame;break;
    default:DONE_CONSUMER(channel_.fail(Fault::Event));
    }
    if(!ok)DONE_CONSUMER(channel_.fail(Fault::Receiver));
    if(c.state.load(std::memory_order_acquire)&Channel::stopping)DONE_CONSUMER(Result::Stopped);
    received_=s.serial;c.retiredLow.store(static_cast<std::uint32_t>(received_),std::memory_order_release);c.retiredHigh.store(static_cast<std::uint32_t>(received_>>32),std::memory_order_release);
    c.tail.store(t+1,std::memory_order_release);DONE_CONSUMER(Result::Ready);
}

/**
 * @brief На том же C0 owner после stopping инвалидирует receiver не более одного раза, затем публикует parked.
 * @details Вызывать лишь между завершёнными pump callback, до free/reset actors.
 */
Result Consumer::park()noexcept{
    ENTER_CONSUMER(true);auto&c=*channel_.storage_.control;if(!(c.state.load(std::memory_order_acquire)&Channel::stopping))DONE_CONSUMER(Result::Busy);
    if(attached_&&!invalidated_){receiver_.invalidate(colour(channel_.fault()));invalidated_=true;}
    attached_=false;c.consumerParked.store(1,std::memory_order_release);DONE_CONSUMER(Result::Ready);
}
#undef ENTER_CONSUMER
#undef DONE_CONSUMER


/**
 * @brief Вызывает только предоставленный wait callback.
 * @details Его false/отсутствие устанавливает sticky Timeout; сам adapter не выбирает tick или clock policy.
 */
bool Sink::pause()noexcept{if(wait_&&wait_(context_))return true;producer_.stop(Fault::Timeout);return false;}

/**
 * @brief Ожидает Ready именно данной публикации, повторяя лишь Pending через wait.
 * @details Поздняя/чужая квитанция не принимается.
 * @param[in] t Квитанция begin/commit с текущей session и publication serial.
 */
bool Sink::acknowledged(Ticket t)noexcept{for(;;){const auto r=producer_.receipt(t);if(r==Result::Ready)return true;if(r!=Result::Pending||!pause())return false;}}

/**
 * @brief Публикует begin и ждёт настоящий receipt ACK через caller wait.
 * @details Во время Main-bootstrap wait вправе pump C0; Worker wait этого делать не вправе.
 * @param[in] l Неизменный lease setup.
 * @param[in] c Завершённый reset7 CPU/raster/phase cut.
 */
bool Sink::begin(const ColourLease&l,const ColourCut&c)noexcept{Ticket t;for(;;){const auto r=producer_.begin(l,c,t);if(r==Result::Published)return acknowledged(t);if(r!=Result::Full||!pause())return false;}}

/**
 * @brief Повторяет попытку лишь при Full и возвращает после копии prefix, без C0 ACK.
 * @details Source events можно переиспользовать только после true.
 * @param[in] p Живой завершённый prefix без read-записей и будущего бюджета.
 */
bool Sink::prefix(const ThinPrefix&p)noexcept{Ticket t;for(;;){const auto r=producer_.prefix(p,t);if(r==Result::Published)return true;if(r!=Result::Full||!pause())return false;}}

/**
 * @brief Публикует запрос и ждёт единственный проверенный reply через caller wait.
 * @details Возвращает false при fault/deadline, не предсказывая прочитанный байт.
 * @param[in] q Запрос с текущими lease/cut/sequence/request-id и регистром0..7.
 * @param[out] out Авторитетный C0 ответ только при true.
 */
bool Sink::read(const ThinReadRequest&q,ThinReadReply&out)noexcept{Ticket t;for(;;){const auto r=producer_.read(q,t);if(r==Result::Published)break;if(r!=Result::Full||!pause())return false;}for(;;){const auto r=producer_.reply(out);if(r==Result::Ready)return true;if(r!=Result::Pending||!pause())return false;}}

/**
 * @brief Публикует финальный commit и ждёт реальный C0 ACK перед разрешением Core output/audio commit.
 * @details Отдельный App PPA lease остаётся обязательным.
 * @param[in] l Тот же неизменный lease.
 * @param[in] c Завершённая финальная CPU-cycle/raster/phase граница кадра.
 * @param[in] s Post-prefix sequence, которую подтверждает consumer.
 */
bool Sink::commit(const ColourLease&l,const ColourCut&c,std::uint64_t s)noexcept{Ticket t;for(;;){const auto r=producer_.commit(l,c,s,t);if(r==Result::Published)return acknowledged(t);if(r!=Result::Full||!pause())return false;}}
}
