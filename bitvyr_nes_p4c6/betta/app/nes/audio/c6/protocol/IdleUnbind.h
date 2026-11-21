#pragma once
#include "FrameService.h"

namespace nes::c6audio::service {
/**
 * @brief Полный ожидаемый старый binding и конечный уже CONSUMED cursor.
 * @details Поля в гостевых CPU cycles/кадрах не усечены до Raw uint32.
 * Это локальный DTO, не новый wire opcode или обещание транспортного ACK.
 */
struct IdleUnbindRequest final {
    Binding expectedBinding{};
    std::uint64_t completedFrames{},audioSequence{},nextPpuFrame{},finalAudioEndCycle{};
    std::uint32_t nextCommandSequence{};
};

/**
 * @brief Внешний barrier существующего Raw worker, удерживаемый через весь reset/control commit.
 * @details Успешный enter обязан закрыть новое RX-admission и доказать отсутствие
 * иных queued/RX/TX/late owners до leave. Моментальный snapshot этой гарантии не даёт.
 * Пока actual Raw implementation отсутствует: HOST callback — только модель.
 * Функции/контекст заимствованы caller до уничтожения lease; heap/SDK здесь нет.
 */
struct IdleAdmissionBarrier final {
    bool (*enter)(void*) noexcept=nullptr;
    void (*leave)(void*) noexcept=nullptr;
    void*context=nullptr;
};

/** @brief Некопируемый cold lease admission; успешный enter имеет ровно один matching leave. */
class IdleAdmissionLease final {
public:
    /** @brief Получает удерживаемый barrier; false enter не вызывает leave и не даёт полномочий reset. */
    static IdleAdmissionLease acquire(const IdleAdmissionBarrier&barrier) noexcept {return IdleAdmissionLease(barrier);}
    /** @brief Возвращает admission только после всех изменений service/control в scope caller. */
    ~IdleAdmissionLease() noexcept {if(held_)barrier_.leave(barrier_.context);}
    /** @brief Не клонирует полномочия и matching leave. */
    IdleAdmissionLease(const IdleAdmissionLease&)=delete;
    /** @brief Присваивание не переносит живой barrier. */
    IdleAdmissionLease&operator=(const IdleAdmissionLease&)=delete;
    /** @brief Стабильный scope не передаётся через move; acquire использует C++17 copy elision. */
    IdleAdmissionLease(IdleAdmissionLease&&)=delete;
    /** @brief Move-assignment также запрещён. */
    IdleAdmissionLease&operator=(IdleAdmissionLease&&)=delete;
    /** @brief Только успешный enter с парным leave считается удерживаемым admission barrier. */
    bool held()const noexcept{return held_;}
private:
    /** @brief Вызывает enter не более одного раза, после проверки наличия парных callbacks. */
    explicit IdleAdmissionLease(const IdleAdmissionBarrier&barrier)noexcept
        :barrier_(barrier),held_(barrier_.enter&&barrier_.leave&&barrier_.enter(barrier_.context)){}
    IdleAdmissionBarrier barrier_{};
    bool held_{};
};

/** @brief Сравнивает все логические поля Binding без чтения native padding. */
inline bool sameIdleBinding(const Binding&a,const Binding&b) noexcept {
    return a.appGeneration==b.appGeneration&&a.romLease==b.romLease&&a.romEpoch==b.romEpoch&&
        a.session==b.session&&a.audioGeneration==b.audioGeneration&&a.firstCommandSequence==b.firstCommandSequence&&
        a.region==b.region&&a.audioCapabilities==b.audioCapabilities;
}
} // пространство имён nes::c6audio::service
