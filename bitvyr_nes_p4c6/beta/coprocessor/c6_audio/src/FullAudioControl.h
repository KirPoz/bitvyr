#pragma once
#include "FrameService.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::c6audio::adapter {
namespace raw=::nes::c6raw;
/** @brief Только часы/stack query адаптера; Core и service не зависят от SDK. */
using Query=std::uint32_t(*)() noexcept;
/** @brief Результат подготовки Raw reply; PCM source остаётся до точного driver completion. */
struct Reply final {std::uint32_t status{},operationCycles{};std::size_t bytes{};bool pcmSource{};service::Envelope source{};};
/**
 * @brief Один статический full-DSP consumer существующего C6 worker.
 * @details Constructor не исполняет DSP. INIT12 placement-создаёт renderer ровно один раз;
 * ни timeout, ни HELLO не уничтожают заимствованные wire/typed/PCM. Все методы вызывает
 * только existing worker; driver callback публикует completion в RawSlot, не сюда.
 */
class FullAudioControl final {
public:
    /** @brief Сохраняет constant initialization storage; DSP создаётся только INIT12. */
    FullAudioControl() noexcept=default;
    /** @brief Byte storage содержит self-bound renderer/service; клонирование владельца запрещено. */
    FullAudioControl(const FullAudioControl&)=delete;
    /** @brief Присваивание не передаёт session и pending driver borrow. */
    FullAudioControl&operator=(const FullAudioControl&)=delete;
    /** @brief Перемещение нарушило бы адреса placement objects и external references. */
    FullAudioControl(FullAudioControl&&)=delete;
    /** @brief Запрещает переезд живого owner даже между свободными на вид contexts. */
    FullAudioControl&operator=(FullAudioControl&&)=delete;
    /** @brief INIT12/BIND13/0x40..44; output принадлежит RawSlot и не пересекает state. */
    Reply process(const raw::Key&key,const std::uint8_t*payload,std::size_t bytes,
                  std::uint8_t*output,std::size_t capacity,Query clock,Query stack) noexcept;
    /** @brief Worker применяет реальный TXdone; false удерживает слот и fault до reboot. */
    bool sourceReleased(const service::Envelope&key) noexcept;
    /** @brief Новый transport session после binding запрещён до reboot/join protocol. */
    bool permitsHello(std::uint64_t session)const noexcept;
    /** @brief Истина после session binding: colour/filter commands больше не допускаются. */
    bool bound()const noexcept{return bound_;}
    /** @brief Не выпускает следующую RX-команду пока driver держит PCM source. */
    bool sourceHeld() noexcept{return initialized_&&service()->snapshot().sourceHeld;}
private:
    /** @brief Возвращает только placement-created object, не создавая его неявно. */
    core::AudioRenderer*renderer() noexcept;
    /** @brief Возвращает единственный service после успешного INIT. */
    service::FrameService*service() noexcept;
    /** @brief Пишет числовой snapshot без native struct padding/pointers. */
    std::size_t snapshot(std::uint8_t*output,std::size_t capacity) noexcept;
    alignas(core::AudioRenderer)std::array<std::uint8_t,sizeof(core::AudioRenderer)>rendererStorage_{};
    alignas(service::FrameService)std::array<std::uint8_t,sizeof(service::FrameService)>serviceStorage_{};
    std::array<std::uint8_t,service::FrameService::maximumWireBytes>wire_{};
    UsedStorage<1024,128,512>typed_{};
    std::uint64_t session_{};
    std::uint32_t assemblyCycles_{},renderCycles_{};
    bool initialized_{},bound_{},fault_{};
};
} // namespace nes::c6audio::adapter
