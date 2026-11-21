/**
 * @file PpuColourReplay.cpp
 * @brief Повтор настоящих PPU-доступов без исполнения CPU/APU теневой машины.
 * @ingroup nes_ppu
 */
#include "PpuColourReplay.h"
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
#include "PpuRgbOutput.h"
#endif
#include <atomic>
#include <cstring>

namespace nes::core {
namespace {
std::atomic<const Machine*> activeShadow{nullptr};
std::atomic<bool> forbiddenClock{false};
}
/**
 * @brief Снимает RGB-привязку выбранного PPU и собственную activeShadow identity перед разрушением.
 * @details Не освобождает внешний PPU/ROM/backing; вызывающий завершает всех пользователей до деструктора.
 */
PpuColourReplay::~PpuColourReplay()noexcept{
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    PpuRgbOutput::detachOwner(&targetPpu());
#endif
    if(bound_){const Machine* expected=&machine_;static_cast<void>(activeShadow.compare_exchange_strong(expected,nullptr,std::memory_order_relaxed));}
}
/**
 * @brief Проверяет запрет повторного CPU/APU-исполнения теневого актора.
 * @details Диагностический контроль отделяет продвижение PPU от исполнения основной машины.
 * @param machine Точная Machine-владелец контекста; указатель используется для проверки идентичности, не для переноса владения.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool PpuColourReplay::clockTrapped(const Machine* machine)noexcept{
    if(activeShadow.load(std::memory_order_relaxed)!=machine)return false;
    forbiddenClock.store(true,std::memory_order_relaxed);return true;
}
/** @details Только relaxed identity load: подтверждение запрета остаётся в прежнем clockTrapped. */
bool PpuColourReplay::isActiveShadow(const Machine* machine)noexcept{
    return activeShadow.load(std::memory_order_relaxed)==machine;
}
#if defined(NES_CORE_PPU_COLOUR_EXTERNAL_TARGET) && NES_CORE_PPU_COLOUR_EXTERNAL_TARGET
/**
 * @brief Выбирает свежий внешний PPU того же теневого bus до начала повторения.
 * @details Проверяет источник, неизменённость начального состояния и неперекрытие. Отказ отравляет актор; продолжать на другом объекте нельзя.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param target Свежий полный PPU той же shadow-шины на cycle0, без чужой активной привязки; остаётся живым всю replay-сессию.
 */
bool PpuColourReplay::bindExternalPpu(Ppu& target)noexcept{
    // Привязать ровно один раз до любой загрузки или продвижения владельца.
    if(error_!=Error::None||bound_||activeShadow.load(std::memory_order_relaxed)||selectedPpu_!=&machine_.ppu_||
       machine_.cycles_||machine_.cpu_.state().cycles||machine_.pendingCpuReads_||machine_.pendingPpuDots_||
       machine_.videoPacket_||machine_.audioPacket_||machine_.cartridge_.loaded())return fail(Error::Busy);
    if(&target==&machine_.ppu_||&target.bus_!=&machine_||target.frames_||target.scanline_!=-1||target.dot_||
       target.control_||target.mask_||target.v_||target.t_||target.pendingPixels_||target.spriteCacheValid_||
       target.writeToggle_||target.oamAddress_||target.fineX_||target.openBus_||target.readBuffer_||
       target.renderDelay_||target.renderingEnabled_||target.renderingNext_||target.nmiDelay_||
       target.nmiLine_||target.nmiPending_||target.frameReady_||target.status_)return fail(Error::Busy);
    if(!target.pixelsReady())return fail(Error::NoPixels);
    /**
     * @brief Проверяет пересечение заимствованных диапазонов через разности адресов без переполнения; память не читается и владение не передаётся.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    const auto overlaps=[](const void* a,std::size_t an,const void* b,std::size_t bn)noexcept{
        const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
        return x<=y?y-x<an:x-y<bn;
    };
    const auto* pixels=target.pixels_.data();
    if(overlaps(&target,sizeof(target),&machine_,sizeof(machine_))||
       overlaps(pixels,Ppu::width*Ppu::height,&machine_,sizeof(machine_))||
       overlaps(pixels,Ppu::width*Ppu::height,&target,sizeof(target))||
       (machine_.ppu_.pixels_.data()&&overlaps(pixels,Ppu::width*Ppu::height,machine_.ppu_.pixels_.data(),Ppu::width*Ppu::height)))return fail(Error::Busy);
    selectedPpu_=&target;
    return true;
}
#endif
/**
 * @brief Начинает новый интервал или привязку объекта.
 * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
 * @param rom Заимствованные байты того же ROM, к которому относится текущая эпоха.
 * @param bytes Доступная длина переданного хранилища в байтах.
 * @param region Явно выбранный гостевой регион NTSC/PAL.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool PpuColourReplay::begin(const std::uint8_t* rom,std::size_t bytes,Region region)noexcept{
#if defined(NES_CORE_PPU_COLOUR_EXTERNAL_TARGET) && NES_CORE_PPU_COLOUR_EXTERNAL_TARGET
    if(error_!=Error::None)return false;
#endif
    if(bound_)return fail(Error::Busy);
    if(machine_.cycles_||machine_.cpu_.state().cycles||machine_.pendingCpuReads_||machine_.pendingPpuDots_||
       machine_.videoPacket_||machine_.audioPacket_||machine_.cartridge_.loaded())return fail(Error::Busy);
#if defined(NES_CORE_PPU_COLOUR_EXTERNAL_TARGET) && NES_CORE_PPU_COLOUR_EXTERNAL_TARGET
    if(!pixelsReady())return fail(Error::NoPixels);
#else
    if(!machine_.legacyVideoReady())return fail(Error::NoPixels);
#endif
    const Machine* empty=nullptr;
    if(!activeShadow.compare_exchange_strong(empty,&machine_,std::memory_order_relaxed))return fail(Error::Busy);
    bound_=true;forbiddenClock.store(false,std::memory_order_relaxed);
    initialCpu_=machine_.cpu_.state();
    std::memcpy(initialApu_.data(),&machine_.apu_,sizeof(machine_.apu_));
    std::memcpy(initialTimingApu_.data(),&machine_.timingApu_,sizeof(machine_.timingApu_));
    if(machine_.cartridge_.load(rom,bytes)!=LoadResult::Ok)return fail(Error::Load);
#if defined(NES_CORE_PPU_FUSED_RGB_DIAG) && NES_CORE_PPU_FUSED_RGB_DIAG
    sourceImage_=rom;sourceBytes_=bytes;
#endif
    region_=region;machine_.region_=region;
    machine_.videoPacket_=nullptr;machine_.videoObserver_=nullptr;machine_.videoCapable_=false;machine_.audioPacket_=nullptr;
#if defined(NES_CORE_VIDEO_POLICY_CACHE) && NES_CORE_VIDEO_POLICY_CACHE
    machine_.refreshVideoPolicy(); // Независимый цветовой владелец никогда не использует Timing.
#endif
    machine_.pendingCpuReads_=machine_.cpuReadBudget_=0;machine_.pendingPpuDots_=machine_.ppuDeferBudget_=0;
    machine_.deferPpuAllowed_=machine_.deferIrqPpu_=false;
    machine_.ram_.fill(0);machine_.nametables_.fill(0);
#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN && defined(NES_CORE_PPU_BG_SPAN_LEGACY) && NES_CORE_PPU_BG_SPAN_LEGACY
    machine_.videoNameMarkers_.clear();
#endif
    machine_.cartridge_.reset();machine_.refreshNametableMapping();
    targetPpu().reset(region==Region::Pal);targetPpu().setBusTracking(false,machine_.cartridge_.mapper()==160);
    cycle_=0;phase_=0;
    return advance(7)&&checkWitness();
}
/**
 * @brief Доводит повторный PPU до абсолютного гостевого времени.
 * @details Не исполняет CPU/APU теневой машины и сохраняет NTSC/PAL-фазу.
 * @param target Абсолютный гостевой CPU-такт, до которого продвигается только выбранный PPU; движение назад запрещено.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool PpuColourReplay::advance(std::uint64_t target)noexcept{
    if(error_!=Error::None)return false;
    if(target<cycle_||target-cycle_>45000U)return fail(Error::Time);
    const auto q=phase_+(target-cycle_)*(region_==Region::Pal?16U:15U);
    const auto dots=static_cast<unsigned>(q/5U);phase_=static_cast<unsigned>(q%5U);
    if(dots)targetPpu().tickDots(dots);
    cycle_=target;return true;
}
/**
 * @brief Исполняет одно хронологическое событие повторного PPU.
 * @details Для сохранённого чтения выполняет реальное чтение один раз и сравнивает ответ; ошибку нельзя исправлять повторной попыткой после побочных эффектов.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @param kind Вид хронологического видеообращения; неподдерживаемое значение отвергается до исполнения.
 * @param address При kind0..2 регистр 0..7, при kind3 CPU-адрес$4020..$FFFF, при kind4 ровно$2004.
 * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool PpuColourReplay::apply(std::uint64_t cycle,unsigned kind,unsigned address,std::uint8_t value)noexcept{
    if(!bound_)return fail(Error::Busy);
    if(kind>4||((kind<=2)&&address>=8)||(kind==1&&address!=2)||(kind==3&&(address<0x4020||address>0xffff))||(kind==4&&address!=0x2004))return fail(Error::Event);
    if(!advance(cycle))return false;
    switch(kind){
    case 0:case 1:
        lastActualRead_=targetPpu().cpuRead(static_cast<std::uint16_t>(address));
        if(lastActualRead_!=value)return fail(Error::Read);
        break;
    case 2:targetPpu().cpuWrite(static_cast<std::uint16_t>(address),value);break;
    case 3:machine_.cartridge_.cpuWrite(static_cast<std::uint16_t>(address),value,cycle_);machine_.refreshNametableMapping();break;
    case 4:targetPpu().dmaWrite(value);break;
    default:return fail(Error::Event);
    }
    return true;
}
/**
 * @brief Завершает цветовой повтор на указанном CPU/PPU-срезе.
 * @details Проверяет согласованность фазы и готовности кадра; RGB/индексы становятся результатом только после полного успеха.
 * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
 * @param phase Остаток деления PPU-времени на 5: для NTSC0, для PAL0..4.
 * @param frame Номер согласованного гостевого PPU-кадра.
 * @param scanline PPU-строка среза: -1 означает pre-render; верхний предел определяется NTSC/PAL.
 * @param dot Следующая исполняемая PPU-точка среза, от 0 до 340 включительно.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool PpuColourReplay::endFrame(std::uint64_t cycle,unsigned phase,std::uint64_t frame,int scanline,unsigned dot)noexcept{
    if(!bound_)return fail(Error::Busy);
    if(!advance(cycle))return false;
    if(phase!=phase_||targetPpu().frameNumber()!=frame||targetPpu().scanline()!=scanline||targetPpu().dot()!=dot)return fail(Error::Cut);
    return true;
}
/**
 * @brief Проверяет согласованность полного актора с сохранённым временным свидетелем.
 * @details Успех не заменяет проверку всей последовательности событий и не является измерением физического вывода.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 */
bool PpuColourReplay::checkWitness()noexcept{
    if(!bound_)return fail(Error::Busy);
    if(forbiddenClock.load(std::memory_order_relaxed))return fail(Error::ForbiddenClock);
    const auto& c=machine_.cpu_.state();
    if(c.pc!=initialCpu_.pc||c.a!=initialCpu_.a||c.x!=initialCpu_.x||c.y!=initialCpu_.y||c.sp!=initialCpu_.sp||c.p!=initialCpu_.p||c.cycles!=initialCpu_.cycles||c.jammed!=initialCpu_.jammed||
       machine_.cycles_||machine_.pendingCpuReads_||machine_.pendingPpuDots_||machine_.audioPacket_||machine_.videoPacket_||
       std::memcmp(initialApu_.data(),&machine_.apu_,sizeof(machine_.apu_))||std::memcmp(initialTimingApu_.data(),&machine_.timingApu_,sizeof(machine_.timingApu_)))return fail(Error::CpuApuChanged);
    return error_==Error::None;
}
}
