#include "RawChannel.h"
#include <cstring>
namespace nes::c6raw {
/** @brief SDK больше не читает TX данного slot; release-флаг не доказывает remote execution или WorkerDone. */
void Channel::sourceDone(void* context) noexcept {static_cast<Slot*>(context)->txOwned.store(false,std::memory_order_release);}
/** @brief Main занимает slot и публикует immutable request до driver admission; непринятый slot возвращается. */
bool Channel::submit(Key key,const std::uint8_t* payload,std::size_t bytes,std::uint8_t flags) noexcept {
    if(!send_||key.slot>=slotCount)return false;
    auto& slot=slots_[key.slot];unsigned free=0;
    if(!slot.state.compare_exchange_strong(free,1,std::memory_order_acquire))return false;
    const auto count=encode(slot.tx.data(),slot.tx.size(),key,1,flags,payload,bytes);
    if(!count){slot.state.store(0,std::memory_order_release);return false;}
    slot.key=key;slot.bytes=0;slot.rx.store(0,std::memory_order_relaxed);slot.late.store(false,std::memory_order_relaxed);slot.txOwned.store(true,std::memory_order_relaxed);
    slot.state.store(2,std::memory_order_release);
    if(send_(key.slot,slot.tx.data(),count,&slot,sourceDone))return true;
    slot.txOwned.store(false,std::memory_order_relaxed);slot.state.store(0,std::memory_order_release);return false;
}
/** @brief RX сверяет CRC/full-key и публикует одну bounded reply-копию; stale/duplicate не меняют identity. */
bool Channel::receive(const std::uint8_t* packet,std::size_t bytes) noexcept {
    View view{};if(!decode(packet,bytes,view)||view.kind!=2)return false;auto& slot=slots_[view.key.slot];
    unsigned waiting=2;if(!slot.state.compare_exchange_strong(waiting,3,std::memory_order_acquire))return false;
    if(!equal(slot.key,view.key)){slot.state.store(2,std::memory_order_release);return false;}
    unsigned empty=0;if(!slot.rx.compare_exchange_strong(empty,1,std::memory_order_acquire)){slot.state.store(2,std::memory_order_release);return false;}
    std::memcpy(slot.reply.data(),packet,bytes);slot.bytes=bytes;slot.rx.store(2,std::memory_order_release);slot.state.store(2,std::memory_order_release);return true;
}
/** @brief Main читает после acquire RX-ready и снятого TX borrow; waits/alloc отсутствуют. */
bool Channel::poll(unsigned index,View& out) noexcept {
    if(index>=slotCount)return false;
    auto& slot=slots_[index];
    return slot.state.load(std::memory_order_acquire)==2&&slot.rx.load(std::memory_order_acquire)==2&&!slot.txOwned.load(std::memory_order_acquire)&&decode(slot.reply.data(),slot.bytes,out);
}
/** @brief Отмечает late у waiting slot; не освобождает источник и не приписывает completion. */
void Channel::timeout(unsigned index) noexcept {if(index<slotCount&&slots_[index].state.load(std::memory_order_acquire)==2)slots_[index].late.store(true,std::memory_order_release);}
/** @brief Main возвращает slot лишь при полном RX и actual TXdone; иначе waiting owner сохраняется. */
bool Channel::release(unsigned index) noexcept {
    if(index>=slotCount)return false;
    auto& slot=slots_[index];unsigned waiting=2;if(!slot.state.compare_exchange_strong(waiting,1,std::memory_order_acquire))return false;
    if(slot.rx.load(std::memory_order_acquire)!=2||slot.txOwned.load(std::memory_order_acquire)){slot.state.store(2,std::memory_order_release);return false;}
    slot.state.store(0,std::memory_order_release);return true;
}
/** @brief Возвращает acquire-снимок занятых slot; числовой ноль не заменяет lifecycle gates. */
unsigned Channel::occupied() const noexcept {unsigned count=0;for(const auto& slot:slots_)count+=slot.state.load(std::memory_order_acquire)!=0;return count;}
}
