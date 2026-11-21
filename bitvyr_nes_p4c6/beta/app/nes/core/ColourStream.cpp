/**
 * @file ColourStream.cpp
 * @brief Ограниченный журнал CPU-видеособытий и проверенный повтор завершённых кадров.
 * @ingroup nes_core
 */
#include "ColourStream.h"
#include "Machine.h"
#include "ppu/PpuColourReplay.h"
#include <limits>
namespace nes::core {
namespace {
/**
 * @brief Проверяет ненулевой диапазон памяти и отсутствие переполнения его конца.
 * @details Не проверяет физический тип памяти; этот уровень ядра не зависит от ESP-IDF capabilities.
 * @param p Начало непустого заимствованного диапазона; адрес проверяется на переполнение с n.
 * @param n Размер диапазона в байтах;0 и переполнение uintptr_t отвергаются.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool span(const void* p,std::size_t n)noexcept{return p&&n&&n<=~std::uintptr_t{0}-reinterpret_cast<std::uintptr_t>(p);}
/**
 * @brief Проверяет пересечение диапазонов с защитой от переполнения адресной арифметики.
 * @details Нулевые и недопустимые диапазоны не считаются доказательством безопасного заимствования.
 * @param a Начало первого заимствованного диапазона; размер передаётся отдельно в байтах.
 * @param an Размер соответствующего диапазона в байтах; проверяется без переполнения.
 * @param b Начало второго заимствованного диапазона; размер передаётся отдельно в байтах.
 * @param bn Размер соответствующего диапазона в байтах; проверяется без переполнения.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool overlap(const void* a,std::size_t an,const void* b,std::size_t bn)noexcept{
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);return x<=y?y-x<an:x-y<bn;
}
/**
 * @brief Классифицирует допустимую форму записи или инструкции для ограниченного быстрого пути.
 * @details Классификация не даёт права обходить фактические адресные, временные и жизненные проверки.
 * @param kind Вид хронологического видеообращения; неподдерживаемое значение отвергается до исполнения.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool shape(ColourKind kind,std::uint16_t address)noexcept{
    switch(kind){case ColourKind::Read:case ColourKind::Write:return address<8;
    case ColourKind::RetainedRead:return address==2;case ColourKind::CartWrite:return address>=0x4020;case ColourKind::Dma:return address==0x2004;default:return false;}
}
/**
 * @brief Проверяет структурную корректность ROM-аренды и её идентичностей.
 * @details Проверка указателя не доказывает аппаратные capabilities и не продлевает срок жизни памяти.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param l Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool leaseValid(const ColourLease& l)noexcept{return l.producer&&l.epoch&&l.generation&&span(l.rom,l.romBytes)&&(l.region==Region::Ntsc||l.region==Region::Pal);}
/**
 * @brief Проверяет растровый и временной срез перед его использованием.
 * @details CPU-cycle, frame, scanline, dot и PAL-остаток относятся к одной границе.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param c Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool cutValid(const ColourCut&c)noexcept{return c.phase<5&&c.dot<=340&&c.scanline>=-1&&c.scanline<=311;}
}
/**
 * @brief Проверяет размер, диапазон и ограничения заимствованного хранилища.
 * @details Не выделяет память; указатель и срок жизни остаются обязанностью владельца.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool ColourPacket::storageValid()const noexcept{
    return capacity_&&capacity_<=maximumEvents&&!(reinterpret_cast<std::uintptr_t>(events_)%alignof(ColourEvent))&&span(events_,std::size_t(capacity_)*sizeof(ColourEvent))&&!overlap(events_,std::size_t(capacity_)*sizeof(ColourEvent),this,sizeof(*this));
}
/**
 * @brief Переводит свободный слот в запись и фиксирует его аренду/начальный срез.
 * @details Публикация и чтение разрешены только после соответствующего перехода состояния.
 * @param sequence Последовательный номер интервала; не номер физической подачи LCD.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param lease Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 * @param cut Точный закрытый CPU/PPU-срез: абсолютный такт, кадр, строка, следующая точка и PAL-остаток.
 */
bool ColourPacket::claim(const ColourLease&lease,std::uint64_t sequence,const ColourCut&cut)noexcept{
    unsigned expected=unsigned(State::Free);if(!storageValid()||!state_.compare_exchange_strong(expected,unsigned(State::Writing),std::memory_order_acq_rel))return false;
    header_={};header_.lease=lease;header_.base=cut;header_.sequence=sequence;return true;
}
/**
 * @brief Добавляет одно событие с проверенным временем и формой.
 * @details При переполнении или нарушении порядка защёлкивает ошибку, а не теряет событие молча.
 * @param kind Вид хронологического видеообращения; неподдерживаемое значение отвергается до исполнения.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool ColourPacket::append(ColourKind kind,std::uint64_t cycle,std::uint16_t address,std::uint8_t value)noexcept{
    if(header_.fault!=ColourFault::None)return false;
    if(!shape(kind,address)){poison(ColourFault::Event);return false;}
    if(cycle<header_.base.cycle||cycle-header_.base.cycle>maximumCycles||(header_.count&&cycle-header_.base.cycle<events_[header_.count-1].cycleDelta)){poison(ColourFault::Time);return false;}
    if(header_.count>=capacity_){poison(ColourFault::Capacity);return false;}
    events_[header_.count++]={static_cast<std::uint32_t>(cycle-header_.base.cycle),address,kind,value};return true;
}
/**
 * @brief Записывает уже выполненное обращение в активный цветовой поток.
 * @details Сохраняет реальное значение и порядок одинаковых timestamps; не делает дополнительного чтения устройства.
 * @param kind Вид хронологического видеообращения; неподдерживаемое значение отвергается до исполнения.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 */
void Machine::recordColourEvent(ColourKind kind,std::uint64_t cycle,std::uint16_t address,std::uint8_t value)noexcept{
    if(colourCapture_&&!colourCapture_->append(kind,cycle,address,value))videoFaultLatch_=colourCapture_->header_.fault==ColourFault::Capacity?VideoFault::Capacity:VideoFault::InvalidEvent;
}
/**
 * @brief Прекращает захват и инвалидирует текущую эпоху цветового транспорта.
 * @details Завершает связанные заимствования. Вызывающий удерживает ROM и память слотов до фактического освобождения обоих владельцев.
 */
void Machine::invalidateColourCapture()noexcept{
    cartridge_.detachCpuPrgTail(); // Завершить заимствование исходника и арены до смены эпохи.
    if(colourCapture_){colourCapture_->poison(ColourFault::Cancelled);colourCapture_->seal(colourCapture_->header_.base);colourCapture_=nullptr;}
    if(colourEpoch_==~std::uint64_t{0})colourEpoch_=0;else if(colourEpoch_)++colourEpoch_;
}
/**
 * @brief Открывает запись точного CPU-видеопотока в свободный слот.
 * @details Проверяет аренду ROM, эпоху, режим владельца и неперекрытие, затем доводит текущие долги перед фиксацией исходного среза.
 * @param sequence Последовательный номер интервала; не номер физической подачи LCD.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param packet Отдельный цветовой/видеослот в требуемом состоянии; чтение и release следуют единственному владельцу.
 * @param lease Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool Machine::beginColourCapture(ColourPacket& packet,const ColourLease& lease,std::uint64_t sequence)noexcept{
    if(thin_)return false;
    if(colourCapture_||cpu_.inStep_||!videoObservableOnly_||videoPacket_||!splitVideoEnabled()||videoHasFault()||!cartridge_.loaded()||!leaseValid(lease)||lease.producer!=this||lease.epoch!=colourEpoch_||lease.region!=region_||!packet.storageValid()||packet.state()!=ColourPacket::State::Free)return false;
    const auto image=reinterpret_cast<std::uintptr_t>(lease.rom),prg=reinterpret_cast<std::uintptr_t>(cartridge_.prg_);
    if(prg<image||prg-image>lease.romBytes||cartridge_.prgSize_>lease.romBytes-(prg-image)||cartridge_.chrSize_>lease.romBytes-(prg-image)-cartridge_.prgSize_)return false;
    const auto bytes=std::size_t(packet.capacity_)*sizeof(ColourEvent);
    if(overlap(packet.events_,bytes,this,sizeof(*this))||overlap(packet.events_,bytes,lease.rom,lease.romBytes)||overlap(&packet,sizeof(packet),this,sizeof(*this))||overlap(&packet,sizeof(packet),lease.rom,lease.romBytes))return false;
    observeCpuReads();flushPpu();
    ColourCut cut{cycles_,timingPpu_.frameNumber(),timingPpu_.scanline(),timingPpu_.dot(),ppuRemainder_};
    if(!packet.claim(lease,sequence,cut))return false;
    colourCapture_=&packet;return true;
}
/**
 * @brief Закрывает видеослот на реальной готовой кадровой границе.
 * @details Проверяет цикл, фазу и отсутствие ошибок; публикует неизменяемый конец, а не будущий предполагаемый бюджет.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param packet Отдельный цветовой/видеослот в требуемом состоянии; чтение и release следуют единственному владельцу.
 */
bool Machine::sealColourCapture(ColourPacket& packet)noexcept{
    if(colourCapture_!=&packet||cpu_.inStep_)return false;
    observeCpuReads();flushPpu();
    ColourCut cut{cycles_,timingPpu_.frameNumber(),timingPpu_.scanline(),timingPpu_.dot(),ppuRemainder_};
    if(videoHasFault()||cpu_.state().jammed)packet.poison(ColourFault::Producer);
    if(audioFault()!=AudioFault::None)packet.poison(ColourFault::Producer);
    if(!videoObservableOnly_||!splitVideoEnabled()||!videoFrameReady()||cut.frame<=packet.header_.base.frame||cut.frame-packet.header_.base.frame!=1)packet.poison(ColourFault::Cut);
    if(cut.cycle<packet.header_.base.cycle||cut.cycle-packet.header_.base.cycle>ColourPacket::maximumCycles)packet.poison(ColourFault::Time);
    if(packet.header_.count&&packet.events_[packet.header_.count-1].cycleDelta>cut.cycle-packet.header_.base.cycle)packet.poison(ColourFault::Time);
    colourCapture_=nullptr;packet.seal(cut);return packet.header_.fault==ColourFault::None;
}
/**
 * @brief Начинает новый интервал или привязку объекта.
 * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param lease Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool ColourStreamReader::begin(const ColourLease& lease)noexcept{
    if(begun_||fault_!=ColourFault::None)return fail(ColourFault::Binding);
    if(!leaseValid(lease))return fail(ColourFault::Lease);
    if(!actor_.begin(lease.rom,lease.romBytes,lease.region))return fail(ColourFault::Replay);
    lease_=lease;expected_={7,0,-1,static_cast<std::uint16_t>(lease.region==Region::Pal?22:21),static_cast<std::uint8_t>(lease.region==Region::Pal?2:0)};
    if(!actor_.endFrame(expected_.cycle,expected_.phase,expected_.frame,expected_.scanline,expected_.dot))return fail(ColourFault::Cut);
    begun_=true;return true;
}
/**
 * @brief Проверяет и исполняет опубликованные записи текущей аренды.
 * @details Порядок времени, последовательность, реальные ответы чтений и конечный срез проверяются до готовности результата.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param packet Отдельный цветовой/видеослот в требуемом состоянии; чтение и release следуют единственному владельцу.
 * @param current Текущая проверяемая аренда с теми же ROM/epoch/generation; не читается из занятого чужого владельца.
 */
bool ColourStreamReader::consume(ColourPacket& packet,const ColourLease& current)noexcept{
    outputReady_=false;if(!begun_||fault_!=ColourFault::None)return fail(ColourFault::Binding);
    unsigned expectedState=unsigned(ColourPacket::State::Sealed);if(!packet.state_.compare_exchange_strong(expectedState,unsigned(ColourPacket::State::Reading),std::memory_order_acq_rel))return fail(ColourFault::Binding);
    /**
     * @brief Освобождает потреблённый colour-слот release-публикацией состояния Consumed и при ошибке терминально отравляет reader; захваченный packet жив до возврата consume.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto finish=[&](ColourFault f)noexcept{packet.state_.store(unsigned(ColourPacket::State::Consumed),std::memory_order_release);return f==ColourFault::None?true:fail(f);};
    const auto& h=packet.header_;
    if(h.fault!=ColourFault::None)return finish(h.fault);
    if(!sameColourLease(lease_,current)||!sameColourLease(h.lease,current))return finish(ColourFault::Lease);
    if(h.sequence!=nextSequence_||nextSequence_==~std::uint64_t{0})return finish(ColourFault::Sequence);
    if(!packet.storageValid()||h.count>packet.capacity_)return finish(ColourFault::Capacity);
    if(!sameColourCut(h.base,expected_)||!cutValid(h.base)||!cutValid(h.end)||h.end.cycle<h.base.cycle||h.end.cycle-h.base.cycle>ColourPacket::maximumCycles||h.base.frame==~std::uint64_t{0}||h.end.frame!=h.base.frame+1)return finish(ColourFault::Cut);
    std::uint32_t previous=0;
    for(unsigned i=0;i<h.count;++i){const auto&e=packet.events_[i];if(!shape(e.kind,e.address))return finish(ColourFault::Event);if(e.cycleDelta<previous||e.cycleDelta>h.end.cycle-h.base.cycle)return finish(ColourFault::Time);previous=e.cycleDelta;}
    if(actor_.cycle()!=h.base.cycle||actor_.phase()!=h.base.phase||!actor_.endFrame(h.base.cycle,h.base.phase,h.base.frame,h.base.scanline,h.base.dot))return finish(ColourFault::Cut);
    for(unsigned i=0;i<h.count;++i){const auto&e=packet.events_[i];if(!actor_.apply(h.base.cycle+e.cycleDelta,unsigned(e.kind),e.address,e.value))return finish(ColourFault::Replay);}
    if(!actor_.endFrame(h.end.cycle,h.end.phase,h.end.frame,h.end.scanline,h.end.dot)||!actor_.checkWitness())return finish(ColourFault::Replay);
    expected_=h.end;++nextSequence_;outputReady_=true;return finish(ColourFault::None);
}
}
