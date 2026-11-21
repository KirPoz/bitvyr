#pragma once
#include <cstdint>

namespace nes::platform {
/**
 * @brief Результат неблокирующего продвижения PPA/native LCD.
 * @details Pending/Fault сохраняют ещё не доказанные владения. Submitted означает
 * окончание PPA-чтения и принятую native-подачу, но не физическую развёртку LCD.
 */
enum class GamePresentPollResult:std::uint8_t {Pending,Submitted,Fault};

namespace detail {
/**
 * @brief Продвигает один Main-owned вывод по фактическим completion без ожиданий.
 * @param ppaReady Защёлка полученного PPA-токена; Pending не потребляет его повторно.
 * @param lcdPending Прежний LCD-запрос; снимается только после tryLcd()==true.
 * @param gamePending Текущий native output; снимается только при успешном submit().
 * @param tryPpa,tryLcd Однократные неблокирующие получения аппаратных семафоров.
 * @param submit Подаёт сохранённый native index; может снова установить lcdPending.
 * @return Submitted после принятого запроса; Fault/Pending не разрешают снять source loan.
 * @pre Один владелец Main, валидный immutable ticket и три native-буфера.
 */
template<class TryPpa,class TryLcd,class Submit>
GamePresentPollResult pollGamePresentation(bool&ppaReady,bool&lcdPending,bool&gamePending,
    TryPpa&&tryPpa,TryLcd&&tryLcd,Submit&&submit)noexcept{
    if(!gamePending)return GamePresentPollResult::Fault;
    if(!ppaReady){
        if(!tryPpa())return GamePresentPollResult::Pending;
        ppaReady=true;
    }
    if(lcdPending){
        if(!tryLcd())return GamePresentPollResult::Pending;
        lcdPending=false;
    }
    if(!submit())return GamePresentPollResult::Fault;
    ppaReady=false;gamePending=false;
    return GamePresentPollResult::Submitted;
}
} // namespace detail
} // namespace nes::platform
