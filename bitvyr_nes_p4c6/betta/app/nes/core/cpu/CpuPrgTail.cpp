/**
 * @file CpuPrgTail.cpp
 * @brief Исключительные аренды неизменяемых физических PRG-окон.
 * @ingroup nes_cpu
 */
#include "Machine.h"
#include <cstring>


namespace nes::core {
namespace {
/**
 * @brief Проверяет границы живого хвоста и его пересечение с сохраняемыми данными.
 * @details Частично выведенный тайл нельзя переиспользовать как уже завершённую строку.
 * @param p Начало проверяемого диапазона или заимствованного источника.
 * @param n Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool tailSpan(const void* p,std::size_t n) noexcept {
    return p&&n&&n<=~std::uintptr_t{0}-reinterpret_cast<std::uintptr_t>(p);
}
/**
 * @brief Проверяет границы живого хвоста и его пересечение с сохраняемыми данными.
 * @details Частично выведенный тайл нельзя переиспользовать как уже завершённую строку.
 * @param a Начало первого заимствованного диапазона; размер передаётся отдельно в байтах.
 * @param an Размер соответствующего диапазона в байтах; проверяется без переполнения.
 * @param b Начало второго заимствованного диапазона; размер передаётся отдельно в байтах.
 * @param bn Размер соответствующего диапазона в байтах; проверяется без переполнения.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool tailOverlap(const void* a,std::size_t an,const void* b,std::size_t bn) noexcept {
    if(!b||!bn)return false;
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    return x<y?y-x<an:x-y<bn;
}
}

/**
 * @brief Привязывает отдельную арену неизменяемого хвоста PRG к текущей ROM-аренде.
 * @details Проверяет границу владения, размер, выравнивание и неперекрытие. Арену и исходный ROM нельзя менять до успешного unbind либо корректного окончания эпохи.
 * @param arena Отдельная writable-арена вызывающего; её нельзя освобождать до окончания заимствования.
 * @param bytes Доступная длина переданного хранилища в байтах.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param lease Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
 */
bool Machine::bindImmutableCpuPrgTail(const ColourLease& lease,void* arena,std::size_t bytes) noexcept {
    if(cartridge_.mapper()==5)return false;
    // Заимствование ограничено владельцем. Проверки состояния не заменяют
    // реальное исключение Worker вызывающим и не проверяют права доступа к памяти.
    // Каждый отказ сохраняет прежнее представление, арену и гостевое состояние.
    if(cartridge_.cpuPrgTail_.resident||bytes!=cpuPrgTailBytes||!tailSpan(arena,bytes)||
       (reinterpret_cast<std::uintptr_t>(arena)&(cpuPrgTailAlignment-1U))||
       !tailSpan(lease.rom,lease.romBytes)||lease.romBytes<16U||
       lease.rom!=cartridge_.cpuPrgSourceImage_||lease.romBytes!=cartridge_.cpuPrgSourceImageBytes_||
       lease.producer!=this||!lease.epoch||lease.epoch!=colourEpoch_||!lease.generation||
       (lease.region!=Region::Ntsc&&lease.region!=Region::Pal)||lease.region!=region_||
       cpu_.inStep_||cpu_.state().jammed||pendingCpuReads_||pendingPpuDots_||
       cpu_.state().cycles!=cycles_||ppuRemainder_>=5U||colourCapture_||
       !videoObservableOnly_||videoPacket_||videoObserver_||!splitVideoEnabled()||videoHasFault()||
       timingPpu_.packet_||timingPpu_.capture_||timingPpu_.dot()>340U||
       timingPpu_.scanline() < -1||timingPpu_.scanline()>(region_==Region::Pal?311:260)||
       !cartridge_.loaded()||cartridge_.mapper_!=4||cartridge_.prgSize_<cpuPrgTailBytes||
       (cartridge_.prgSize_&8191U))return false;
    if(thin_)return false;
    if(audioFault()!=AudioFault::None)return false;
    if(tailOverlap(arena,bytes,this,sizeof(*this))||tailOverlap(arena,bytes,&lease,sizeof(lease))||
       tailOverlap(arena,bytes,lease.rom,lease.romBytes)||
       tailOverlap(arena,bytes,ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
    if(tailOverlap(arena,bytes,audioPacket_,audioPacket_?sizeof(*audioPacket_):0U))return false;
    // Сверить реально разобранный источник, включая trainer и полный CHR-диапазон.
    // Const-указатель или другой содержащий диапазон не доказывает неизменяемую аренду.
    const auto* image=lease.rom;
    if(std::memcmp(image,"NES\x1a",4)||(image[7]&3U))return false;
    const bool nes2=(image[7]&0x0cU)==0x08U;
    if(nes2&&((image[8]>>4U)||(image[9]&15U)==15U||(image[9]>>4U)==15U))return false;
    const auto mapper=unsigned((image[6]>>4U)|(image[7]&0xf0U))|(nes2?unsigned(image[8]&15U)<<8U:0U);
    const std::size_t prgBytes=std::size_t(image[4]|(nes2?((image[9]&15U)<<8U):0U))*16384U;
    const std::size_t chrBytes=std::size_t(image[5]|(nes2?((image[9]>>4U)<<8U):0U))*8192U;
    const std::size_t offset=16U+((image[6]&4U)?512U:0U);
    if(mapper!=4U||offset>lease.romBytes||prgBytes>lease.romBytes-offset||
       chrBytes>lease.romBytes-offset-prgBytes||prgBytes!=cartridge_.prgSize_||
       chrBytes!=cartridge_.chrSize_||cartridge_.prg_!=image+offset||
       cartridge_.chr_!=(chrBytes?image+offset+prgBytes:nullptr))return false;
    for(unsigned i=0;i<4;++i){
        const auto off=cartridge_.prgOffset(static_cast<std::uint16_t>(0x8000U+i*8192U));
        if((off&8191U)||off>prgBytes-8192U||cartridge_.prgPages_[i]!=cartridge_.prg_+off)return false;
    }
    // Одно копирование при bind независимо от последующих bank/mode-переходов.
    auto& view=cartridge_.cpuPrgTail_;
    const auto* source=cartridge_.prg_+prgBytes-cpuPrgTailBytes;
    std::memcpy(arena,source,cpuPrgTailBytes);
    view.source=source;view.resident=static_cast<std::uint8_t*>(arena);
    view.refresh(cartridge_.prgPages_);
    return true;
}

/**
 * @brief Отсоединяет резидентный хвост PRG на исключительном срезе.
 * @details Успех завершает заимствование арены; отказ не даёт права освобождать память, ещё доступную владельцу.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool Machine::unbindImmutableCpuPrgTail() noexcept {
    if(!cartridge_.cpuPrgTail_.resident)return true;
    // Не угадывать завершение timeout: вызывающий без настоящего владения
    // сохраняет арену. Сам метод не добавляет наблюдение, такт или flush.
    if(cpu_.inStep_||pendingCpuReads_||pendingPpuDots_||colourCapture_)return false;
    cartridge_.detachCpuPrgTail();
    return true;
}
}
