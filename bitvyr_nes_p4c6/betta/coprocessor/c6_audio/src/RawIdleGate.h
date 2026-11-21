#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace nes::c6audio::adapter {
/**
 * @brief Закрывает RX-admission на время холодного reset без ожидания внутри callback.
 * @details Один CAS допускает writer либо отказывает; уже вошедший writer не даёт
 * worker получить barrier. Отдельный ACK loan запрещает исполнение новых команд
 * до actual sourceDone. Не заменяет полные ключи service и проверку всех Raw slots.
 */
class RawIdleGate final {
public:
    /** @brief Счётчики живут до reboot; копирование размножило бы право reset. */
    RawIdleGate() noexcept=default;
    /** @brief Не клонирует полномочия RX barrier и ACK loan. */
    RawIdleGate(const RawIdleGate&)=delete;
    /** @brief Присваивание не передаёт живые atomic admission/ACK владельцы. */
    RawIdleGate&operator=(const RawIdleGate&)=delete;
    /** @brief Однократно пытается войти в RX; false не требует leaveRx(). */
    bool enterRx() noexcept {
        auto value=admission_.load(std::memory_order_relaxed);
        return !(value&closed)&&value<closed-1U&&admission_.compare_exchange_strong(value,value+1U,std::memory_order_acquire);
    }
    /** @brief Завершает ровно один успешный enterRx после публикации Ready либо отказа. */
    void leaveRx() noexcept {admission_.fetch_sub(1U,std::memory_order_release);}
    /** @brief Worker получает barrier только при отсутствии вошедших RX writers. */
    bool closeIdle() noexcept {
        std::uint32_t empty=0;
        return admission_.compare_exchange_strong(empty,closed,std::memory_order_acq_rel);
    }
    /** @brief Парный leave после reset и control commit; не освобождает ACK source loan. */
    void open() noexcept {admission_.store(0U,std::memory_order_release);}
    /**
     * @brief Резервирует внутренний slot/ticket до reset; false не меняет старый loan.
     * @param slot Индекс существующего RawSlot, от0 до3; ticket полный uint32 arrival cursor.
     */
    bool armAck(std::uint32_t slot,std::uint32_t ticket) noexcept {
        if(slot>=4U)return false;
        std::uint32_t empty=0;
        if(!ackSlot_.compare_exchange_strong(empty,reserved,std::memory_order_acq_rel))return false;
        ackTicket_.store(ticket,std::memory_order_relaxed);
        ackSlot_.store(slot+1U,std::memory_order_release);return true;
    }
    /** @brief Проверяет удерживаемый ACK до вычисления следующей Ready команды. */
    bool ackHeld()const noexcept {return ackSlot_.load(std::memory_order_acquire)!=0U;}
    /**
     * @brief Снимает loan по сохранённым slot/ticket только после sourceDone либо precommit отказа.
     * @details Timeout и неудачная отправка не дают этого права. Driver обязан вызвать
     * callback ровно один раз: поздний повтор после reuse того же slot указателем
     * нельзя отличить без отдельного неизменяемого driver token.
     */
    bool releaseAck(std::uint32_t slot,std::uint32_t ticket) noexcept {
        auto expected=slot+1U;
        if(slot>=4U||ackSlot_.load(std::memory_order_acquire)!=expected||ackTicket_.load(std::memory_order_relaxed)!=ticket)return false;
        return ackSlot_.compare_exchange_strong(expected,0U,std::memory_order_acq_rel);
    }
private:
    static constexpr std::uint32_t closed=0x80000000U,reserved=std::numeric_limits<std::uint32_t>::max();
    std::atomic<std::uint32_t>admission_{0U},ackSlot_{0U},ackTicket_{0U};
};

/** @brief Ограничивает один успешный RX вход любым выходом из callback без heap/SDK. */
class RawRxLease final {
public:
    /** @brief Пытается войти однократно; caller проверяет held() до чтения/копии Raw input. */
    explicit RawRxLease(RawIdleGate&gate)noexcept:gate_(gate),held_(gate.enterRx()){}
    /** @brief Публикует завершение writer после Ready release либо отказа. */
    ~RawRxLease()noexcept {if(held_)gate_.leaveRx();}
    /** @brief Второй destructor не должен освобождать единственный writer вход. */
    RawRxLease(const RawRxLease&)=delete;
    /** @brief Присваивание не переносит парный leave между callback scopes. */
    RawRxLease&operator=(const RawRxLease&)=delete;
    /** @brief Возвращает полномочие читать и публиковать вход в этом scope. */
    bool held()const noexcept{return held_;}
private:
    RawIdleGate&gate_;
    bool held_{};
};
} // пространство имён nes::c6audio::adapter
