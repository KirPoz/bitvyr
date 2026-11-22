#pragma once
/**
 * @file ColourReplayTrace.h
 * @brief Host-хуки проверки порядка видеособытий и запрета исполнения CPU/APU теневой машиной.
 * @ingroup nes_ppu
 *
 * Этот интерфейс обслуживает проверочные трассы. Он не заменяет ограниченный
 * транспорт ColourPacket и не выполняет синхронизацию потоков приложения.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace nes::core {
/**
 * @brief Единственный владелец гостевых часов, карты памяти, CPU/PPU/APU и состояния картриджа. ROM заимствован; ввод, хранение, задачи и физический вывод остаются вне ядра.
 * @ingroup nes_core
 */
class Machine;
namespace colourtrace {
/** @brief Вид уже совершённого обращения CPU к видеосистеме или картриджу. */
enum class Kind : std::uint8_t { Read, RetainedRead, Write, CartWrite, Dma };
/** @brief Компактная запись: смещение гостевого времени, адрес, вид и реальное значение обращения. */
struct Event {
    std::uint32_t cycleDelta;
    std::uint16_t address;
    Kind kind;
    std::uint8_t value;
};
static_assert(sizeof(Event)==8,"host proof uses the proposed compact record");
using Hook=void(*)(Kind,std::uint64_t,std::uint16_t,std::uint8_t) noexcept;
inline Machine* producer=nullptr;
inline const Machine* shadow=nullptr;
inline Hook hook=nullptr;
inline std::uint64_t forbiddenExecutionCalls=0;
/**
 * @brief Передаёт событие только от зарегистрированной машины-производителя.
 * @param who Машина, уже выполнившая обращение; её состояние здесь не читается.
 * @param kind Тип реального обращения, включая сохранённый ответ STATUS.
 * @param cycle Абсолютный гостевой такт после предусмотренного исходным путём clock.
 * @param address Гостевой адрес обращения; нормализация остаётся обязанностью вызывающего пути.
 * @param value Фактически прочитанный или записанный байт, без повторного чтения устройства.
 * @note Хук не владеет ROM/машиной; вызывающий host-тест обеспечивает срок их жизни.
 */
inline void emit(Machine* who,Kind kind,std::uint64_t cycle,std::uint16_t address,std::uint8_t value) noexcept {
    if(who==producer&&hook)hook(kind,cycle,address,value);
}
/**
 * @brief Аварийно останавливает host-проверку, если теневая машина получила вызов CPU/APU clock.
 * @param who Проверяемая машина; сравнивается только её идентичность с shadow.
 * @details Цветовой повтор вправе двигать PPU, но не повторно исполнять CPU или APU.
 * Счётчик и abort фиксируют нарушение тестового контракта вместо скрытого двойного исполнения.
 */
inline void forbidExecution(const Machine* who) noexcept {
    if(who==shadow){
        ++forbiddenExecutionCalls;
        std::fprintf(stderr,"COLOUR_REPLAY_FORBIDDEN_SHADOW_CPU_APU_CLOCK\n");
        std::abort();
    }
}
}
}
