/**
 * @file ThinPpu.cpp
 * @brief Проверяемый синхронный канал закрытых префиксов и настоящих PPU-чтений.
 * @ingroup nes_core
 */
#include "ThinPpu.h"
#include "Machine.h"
#include "ppu/PpuColourReplay.h"
#include <limits>
#include <algorithm>
namespace nes::core {
namespace {
/**
 * @brief Возвращает состояние или результат текущей привязки.
 * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param l Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool valid(const ColourLease&l)noexcept{return l.producer&&l.rom&&l.romBytes>=16&&l.romBytes<=~std::uintptr_t{0}-reinterpret_cast<std::uintptr_t>(l.rom)&&l.epoch&&l.generation&&(l.region==Region::Ntsc||l.region==Region::Pal);}
/**
 * @brief Возвращает состояние или результат текущей привязки.
 * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
 * @param r Регион NTSC/PAL, задающий длину кадра и дробное отношение PPU/CPU.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool valid(const ColourCut&c,Region r)noexcept{return c.dot<=340&&c.scanline>=-1&&c.scanline<=(r==Region::Pal?310:260)&&c.phase<5&&(r==Region::Pal||!c.phase);}
/**
 * @brief Проверяет соответствие kind и адреса реальной видеозаписи.
 * @details Классификация не даёт права обходить фактические адресные, временные и жизненные проверки.
 * @param k Вид хронологического видеообращения, определяющий допустимый адрес и эффект.
 * @param a Адрес события: регистр 0..7, cartridge $4020..$FFFF либо DMA $2004 согласно kind.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool writeShape(ColourKind k,unsigned a)noexcept{return(k==ColourKind::Write&&a<8)||(k==ColourKind::CartWrite&&a>=0x4020&&a<=0xffff)||(k==ColourKind::Dma&&a==0x2004)||
    (k==ColourKind::Mmc5CpuStrobe&&(a==0xfffaU||a==0xfffbU||a==0x2002U||a==0x5204U))||
    (k==ColourKind::Mmc5DmcStrobe&&(a==0xfffaU||a==0xfffbU))||
    (k==ColourKind::Mmc5SignalWrite&&(a==0x2000U||a==0x2001U||a==0x4014U))||
    (k==ColourKind::Mmc5IdleReset&&a==0U)||
    (k==ColourKind::Mmc5IdleDmcStrobe&&(a==0xfffaU||a==0xfffbU));}
/**
 * @brief Проверяет пересечение диапазонов с защитой от переполнения адресной арифметики.
 * @details Нулевые и недопустимые диапазоны не считаются доказательством безопасного заимствования.
 * @param a Начало первого заимствованного диапазона; размер передаётся отдельно в байтах.
 * @param an Размер соответствующего диапазона в байтах; проверяется без переполнения.
 * @param b Начало второго заимствованного диапазона; размер передаётся отдельно в байтах.
 * @param bn Размер соответствующего диапазона в байтах; проверяется без переполнения.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool overlap(const void*a,std::size_t an,const void*b,std::size_t bn)noexcept{
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);return x<=y?y-x<an:x-y<bn;
}
}
/**
 * @brief Защёлкивает первую ошибку и запрещает готовность частичного результата.
 * @details Не откатывает уже исполненные побочные эффекты и не разрешает скрытую смену владельца.
 * @param f Первая сохраняемая ошибка протокола.
 * @return false после фиксации первой ошибки; успешный частичный результат не выдаётся.
 */
bool ThinReplaySink::fail(ColourFault f)noexcept{if(fault_==ColourFault::None)fault_=f;ready_=false;actor_.invalidateActual();return false;}
/**
 * @brief Терминально отменяет текущую привязку/сеанс после нарушения контракта.
 * @details До нового согласованного reset запрещает дальнейшую публикацию; память удерживается до фактической остановки пользователей.
 * @param f Первая сохраняемая ошибка протокола.
 */
void ThinReplaySink::invalidate(ColourFault f)noexcept{static_cast<void>(fail(f));}
/**
 * @brief Начинает новый интервал или привязку объекта.
 * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param l Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ThinReplaySink::begin(const ColourLease&l,const ColourCut&c)noexcept{
    if(begun_||fault_!=ColourFault::None||!valid(l)||!valid(c,l.region)||c.cycle!=7||c.frame||c.scanline!=-1||c.dot!=(l.region==Region::Pal?22:21)||c.phase!=(l.region==Region::Pal?2:0))return fail(ColourFault::Binding);
    if(!actor_.begin(l.rom,l.romBytes,l.region)||!actor_.endFrame(c.cycle,c.phase,c.frame,c.scanline,c.dot))return fail(ColourFault::Replay);
    lease_=l;expected_=c;begun_=true;return true;
}
/**
 * @brief Передаёт законченный временной префикс реальному полному PPU.
 * @details Потребитель обязан закончить заимствование записей согласно sink-контракту; предполагаемые будущие события не публикуются.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param p Закрытый неизменяемый Thin-префикс с непрерывными cut и sequence.
 */
bool ThinReplaySink::prefix(const ThinPrefix&p)noexcept{
    ready_=false;
    if(!begun_||fault_!=ColourFault::None)return fail(ColourFault::Binding);
    if(!sameColourLease(p.lease,lease_))return fail(ColourFault::Lease);
    if(p.sequence!=sequence_||sequence_==~std::uint64_t{0})return fail(ColourFault::Sequence);
    if(!sameColourCut(p.base,expected_)||!valid(p.end,lease_.region)||p.end.cycle<p.base.cycle||p.end.cycle-p.base.cycle>ColourPacket::maximumCycles||p.end.frame<p.base.frame||p.end.frame-p.base.frame>1)return fail(ColourFault::Cut);
    if(p.count>ColourPacket::maximumEvents||(p.count&&!p.events))return fail(ColourFault::Capacity);
    unsigned previous=0;
    for(unsigned i=0;i<p.count;++i){const auto&e=p.events[i];if(!writeShape(e.kind,e.address))return fail(ColourFault::Event);if(e.cycleDelta<previous||e.cycleDelta>p.end.cycle-p.base.cycle)return fail(ColourFault::Time);previous=e.cycleDelta;}
    if(!actor_.endFrame(p.base.cycle,p.base.phase,p.base.frame,p.base.scanline,p.base.dot))return fail(ColourFault::Cut);
    for(unsigned i=0;i<p.count;++i){const auto&e=p.events[i];if(!actor_.apply(p.base.cycle+e.cycleDelta,unsigned(e.kind),e.address,e.value))return fail(ColourFault::Replay);}
    if(!actor_.endFrame(p.end.cycle,p.end.phase,p.end.frame,p.end.scanline,p.end.dot)||!actor_.checkProgressWitness())return fail(ColourFault::Replay);
    expected_=p.end;++sequence_;return true;
}
/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param q Один execute-once запрос с lease/cut/sequence/request и нормализованным PPU-регистром.
 * @param r Выходной настоящий ответ с полным эхом запроса; допустим лишь при успешном read.
 */
bool ThinReplaySink::read(const ThinReadRequest&q,ThinReadReply&r)noexcept{
    ready_=false;
    if(!begun_||fault_!=ColourFault::None)return fail(ColourFault::Binding);
    if(!sameColourLease(q.lease,lease_))return fail(ColourFault::Lease);
    if(q.sequence!=sequence_||q.request!=request_||request_==~std::uint64_t{0})return fail(ColourFault::Sequence);
    if(!sameColourCut(q.cut,expected_)||q.reg>=8)return fail(ColourFault::Cut);
    std::uint8_t value;
    if(!actor_.readActual(q.cut.cycle,q.reg,value)||!actor_.checkProgressWitness())return fail(ColourFault::Replay);
    r={lease_,q.cut,q.sequence,q.request,value};++request_;return true;
}
/**
 * @brief Подтверждает полный кадр после достижения точного конечного среза.
 * @details Общий успех нужен до публикации RGB/аудио; один лишь producer-done не завершает всех потребителей.
 * @param sequence Последовательный номер интервала; не номер физической подачи LCD.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param l Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ThinReplaySink::commit(const ColourLease&l,const ColourCut&c,std::uint64_t sequence)noexcept{
    if(!begun_||fault_!=ColourFault::None||!sameColourLease(l,lease_))return fail(ColourFault::Lease);
    if(sequence!=sequence_||!sameColourCut(c,expected_)||frame_==~std::uint64_t{0}||c.frame!=frame_+1)return fail(ColourFault::Cut);
    if(!actor_.endFrame(c.cycle,c.phase,c.frame,c.scanline,c.dot)||!actor_.checkWitness())return fail(ColourFault::Replay);
    frame_=c.frame;ready_=true;return true;
}
/**
 * @brief Защёлкивает первую ошибку и запрещает готовность частичного результата.
 * @details Не откатывает уже исполненные побочные эффекты и не разрешает скрытую смену владельца.
 * @param f Первая сохраняемая ошибка протокола.
 * @return false после фиксации первой ошибки; успешный частичный результат не выдаётся.
 */
bool ThinPpuChannel::fail(ColourFault f)noexcept{
    ready_=false;
    if(fault_==ColourFault::None){fault_=f;++counters_.failures;if(!notifying_){notifying_=true;sink_.invalidate(f);notifying_=false;}}
    return false;
}
/**
 * @brief Проверяет размер, диапазон и ограничения заимствованного хранилища.
 * @details Не выделяет память; указатель и срок жизни остаются обязанностью владельца.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param m Точная Machine producer для проверок идентичности и неперекрытия хранилищ.
 * @param l Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool ThinPpuChannel::storageValid(const Machine&m,const ColourLease&l)const noexcept{
    const auto p=reinterpret_cast<std::uintptr_t>(events_);const auto n=std::size_t(capacity_)*sizeof(ColourEvent);
    return events_&&capacity_&&capacity_<=ColourPacket::maximumEvents&&!(p%alignof(ColourEvent))&&n<=~std::uintptr_t{0}-p&&
        !overlap(events_,n,this,sizeof(*this))&&!overlap(events_,n,&m,sizeof(m))&&!overlap(events_,n,l.rom,l.romBytes);
}
/**
 * @brief Привязывает заимствованный ресурс или временного получателя к текущему владельцу.
 * @details Точные ограничения размера, фазового среза и неперекрытия определяются этим типом; произвольное освобождение ресурса во время использования недопустимо.
 * @param mode Signals для mapper0/1/2/3/4/5/7; Legacy160 сохраняет реальные FD/FE и /RD только в Oracle.
 * @param audioGeneration Поколение звукового пакета, проверяемое независимо от lease цветового владельца.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param l Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ThinPpuChannel::bind(const ColourLease&l,const ColourCut&c,ThinMode mode,std::uint64_t audioGeneration)noexcept{
    if(audioStageOwner_||audioStageState_!=AudioStagePhase::Empty||begun_||busy_||fault_!=ColourFault::None||!valid(l)||!valid(c,l.region))return fail(ColourFault::Binding);
    mode_=mode;lease_=l;base_=c;audioGeneration_=audioGeneration;
    busy_=true;const bool ok=sink_.begin(l,c);busy_=false;
    if(!ok||fault_!=ColourFault::None)return fail(ColourFault::Replay);
    begun_=true;return true;
}
/**
 * @brief Передаёт накопленный завершённый префикс получателю.
 * @details Не пересекает будущий timestamp и не даёт права переиспользовать заимствованные записи раньше обещанного завершения.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ThinPpuChannel::flush(const ColourCut&c)noexcept{
    if(!begun_||busy_||fault_!=ColourFault::None)return fail(ColourFault::Binding);
    if(sequence_==~std::uint64_t{0}||!valid(c,lease_.region)||c.cycle<base_.cycle||c.cycle-base_.cycle>ColourPacket::maximumCycles)return fail(ColourFault::Time);
    const ThinPrefix prefix{lease_,base_,c,sequence_,events_,count_};
    busy_=true;const bool ok=sink_.prefix(prefix);busy_=false;
    if(!ok||fault_!=ColourFault::None)return fail(ColourFault::Replay);
    ++counters_.prefixes;++sequence_;base_=c;count_=0;return true;
}
/**
 * @brief Добавляет одно событие с проверенным временем и формой.
 * @details При переполнении или нарушении порядка защёлкивает ошибку, а не теряет событие молча.
 * @param kind Вид хронологического видеообращения; неподдерживаемое значение отвергается до исполнения.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ThinPpuChannel::append(ColourKind kind,std::uint64_t cycle,std::uint16_t address,std::uint8_t value,const ColourCut&c)noexcept{
    ready_=false;
    if(!begun_||busy_||fault_!=ColourFault::None)return fail(ColourFault::Binding);
    if(!writeShape(kind,address)||cycle!=c.cycle)return fail(ColourFault::Event);
    if(count_==capacity_&&!flush(c))return false;
    if(cycle<base_.cycle||cycle-base_.cycle>ColourPacket::maximumCycles||(count_&&cycle-base_.cycle<events_[count_-1].cycleDelta))return fail(ColourFault::Time);
    events_[count_++]={static_cast<std::uint32_t>(cycle-base_.cycle),address,kind,value};++counters_.records;
    counters_.maximumBuffered=std::max(counters_.maximumBuffered,count_);return true;
}
/**
 * @brief Читает байт соответствующего адресного пространства.
 * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 * @param value Выходной байт фактического чтения; допустим только после успешного результата метода.
 */
bool ThinPpuChannel::read(const ColourCut&c,std::uint16_t reg,std::uint8_t&value)noexcept{
    ready_=false;
    if(reg>=8||request_==~std::uint64_t{0})return fail(ColourFault::Event);
    if(!flush(c))return false;
    const ThinReadRequest request{lease_,c,sequence_,request_,static_cast<std::uint8_t>(reg)};ThinReadReply reply{};
    busy_=true;const bool ok=sink_.read(request,reply);busy_=false;
    if(!ok||fault_!=ColourFault::None)return fail(ColourFault::Replay);
    if(!sameColourLease(reply.lease,lease_)||!sameColourCut(reply.cut,c)||reply.sequence!=sequence_||reply.request!=request_)return fail(ColourFault::Lease);
    value=reply.value;++request_;++counters_.reads;++counters_.readRegisters[reg];return true;
}
/**
 * @brief Подтверждает полный кадр после достижения точного конечного среза.
 * @details Общий успех нужен до публикации RGB/аудио; один лишь producer-done не завершает всех потребителей.
 * @param audioGeneration Поколение звукового пакета, проверяемое независимо от lease цветового владельца.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ThinPpuChannel::commit(const ColourCut&c,std::uint64_t audioGeneration)noexcept{
    if(audioGeneration!=audioGeneration_||frame_==~std::uint64_t{0}||c.frame!=frame_+1)return fail(ColourFault::Producer);
    if(!flush(c))return false;
    busy_=true;const bool ok=sink_.commit(lease_,c,sequence_);busy_=false;
    if(!ok||fault_!=ColourFault::None)return fail(ColourFault::Replay);
    frame_=c.frame;ready_=true;++counters_.commits;return true;
}
/**
 * @brief Формирует текущий материализованный CPU/PPU-срез тонкого канала.
 * @details Не включает неисполненный будущий бюджет.
 * @return Вычисленное значение или состояние типа ColourCut согласно описанному контракту.
 */
ColourCut Machine::thinCut()const noexcept{return {cycles_,timingPpu_.frameNumber(),timingPpu_.scanline(),timingPpu_.dot(),ppuRemainder_};}
/**
 * @brief Подключает канал тонкого PPU на остановленном сбросном срезе.
 * @details Обе стороны должны быть согласованы по ROM, эпохе и фазе. Ошибка активного канала не разрешает скрытый переход к другому владельцу.
 * @param channel Заимствованный Thin-канал с тем же владельцем и сроком жизни сессии; nullptr снимает привязку.
 * @param mode Signals для mapper0/1/2/3/4/5/7; Legacy160 сохраняет реальные FD/FE и /RD только в Oracle.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param lease Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool Machine::configureThinPpu(ThinPpuChannel*channel,const ColourLease&lease,ThinMode mode)noexcept{
    if(outputOwner_)return false;
    if(cpu_.inStep_||cpuCycles()!=7||pendingCpuReads_||pendingPpuDots_)return false;
    if(!channel){if(thin_)thin_->cancel();thin_=nullptr;thinStickyFault_=false;return true;}
    if(thin_||colourCapture_||!audioPacket_||!videoObservableOnly_||!splitVideoEnabled()||videoPacket_||!cartridge_.loaded()||((cartridge_.mapper()!=0&&cartridge_.mapper()!=1&&cartridge_.mapper()!=2&&cartridge_.mapper()!=3&&cartridge_.mapper()!=4&&cartridge_.mapper()!=5&&cartridge_.mapper()!=7&&cartridge_.mapper()!=160)||(cartridge_.mapper()==160&&mode==ThinMode::Signals))||!valid(lease)||lease.producer!=this||lease.epoch!=colourEpoch_||lease.region!=region_||(mode!=ThinMode::Oracle&&mode!=ThinMode::Signals)||!channel->storageValid(*this,lease))return false;
    const auto image=reinterpret_cast<std::uintptr_t>(lease.rom),prg=reinterpret_cast<std::uintptr_t>(cartridge_.prg_);
    if(prg<image||prg-image>lease.romBytes||cartridge_.prgSize_>lease.romBytes-(prg-image)||cartridge_.chrSize_>lease.romBytes-(prg-image)-cartridge_.prgSize_)return false;
    thin_=channel;thinStickyFault_=false;
    if(!channel->bind(lease,thinCut(),mode,audioGeneration_)){thinStickyFault_=true;return false;}
    if(mode==ThinMode::Signals){timingPpu_.flushPixels();timingPpu_.backgroundAtoms_.fill(VideoByteRef::literal);timingPpu_.spriteCacheValid_=false;timingPpu_.spriteZeroRow_=false;timingPpu_.spriteCacheClocks_=0;}
    return true;
}
/**
 * @brief Терминально отменяет текущую привязку/сеанс после нарушения контракта.
 * @details До нового согласованного reset запрещает дальнейшую публикацию; память удерживается до фактической остановки пользователей.
 */
void Machine::invalidateThinPpu()noexcept{if(thin_){thin_->cancel();thinStickyFault_=true;}}
/**
 * @brief Сохраняет уже выполненное событие в ограниченном буфере тонкого канала.
 * @details Ёмкость и порядок проверяются; переполнение не превращается в молчаливую потерю события.
 * @param kind Вид хронологического видеообращения; неподдерживаемое значение отвергается до исполнения.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::recordThinEvent(ColourKind kind,std::uint64_t cycle,std::uint16_t address,std::uint8_t value)noexcept{
    if(thin_&&!thin_->append(kind,cycle,address,value,thinCut())){thinStickyFault_=true;videoFaultLatch_=VideoFault::InvalidEvent;}
}
/**
 * @brief Делегирует реальное CPU-чтение единственному полному PPU и сохраняет локальные эффекты сигналов.
 * @details Сначала закрывается текущий префикс; ответ соответствует точному post-clock срезу и не угадывается.
 * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t Machine::readThinPpu(std::uint16_t reg)noexcept{
    std::uint8_t value=openBus_;
    if(!thin_->read(thinCut(),reg,value)){thinStickyFault_=true;videoFaultLatch_=VideoFault::InvalidEvent;return openBus_;}
    if(thin_->mode()==ThinMode::Oracle){if(timingPpu_.cpuRead(reg)!=value){thin_->cancel(ColourFault::Replay);thinStickyFault_=true;videoFaultLatch_=VideoFault::InvalidEvent;}}
    else if(!timingPpu_.readThinSignals(reg,value)){thin_->cancel(ColourFault::Replay);thinStickyFault_=true;videoFaultLatch_=VideoFault::InvalidEvent;}
    return value;
}
/**
 * @brief Исполняет гостевую машину до границы кадра либо до ошибки.
 * @details Результат FrameReady относится к гостевому PPU. Он не означает завершения RGB, PCM, PPA или вывода на LCD у приложения.
 * @return Вычисленное значение или состояние типа RunStatus согласно описанному контракту.
 */
RunStatus Machine::stepFrame()noexcept{
    if(thinStickyFault_)return RunStatus::VideoFault;
    if(!thin_)return stepFrameImpl();
    if(thin_->fault()!=ColourFault::None)return RunStatus::VideoFault;
    if(thin_->audioStageOwner_||thin_->audioStageState_!=AudioStagePhase::Empty)return RunStatus::AudioFault;
    thin_->beginFrame();const auto result=stepFrameImpl();
    if(result!=RunStatus::FrameReady){invalidateThinPpu();return result;}
    observeCpuReads();
    // Уже выполненные чтения материализованы. Будущий quiet-credit не переносится
    // через закрытый frame cut и не является разрешением исполнять следующий кадр.
    cpuReadBudget_=0;
    flushPpu();
    if(videoHasFault()||audioFault()!=AudioFault::None||!thin_->commit(thinCut(),audioGeneration_)){invalidateThinPpu();return RunStatus::VideoFault;}
    return RunStatus::FrameReady;
}
}
