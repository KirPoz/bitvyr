#pragma once
#include "ClientTypes.h"
#include "../transport/RawChannel.h"
#include "IdleUnbindWire.h"

#include <array>

namespace nes::c6audio::async {
/** @brief Fixed current-C6 image qualification; поля не являются OTA разрешением. */
struct RawProfile final {
    raw::Key hello{};
    /** @brief Caller обязан pin actual нового ELF; zero не квалифицирует старый a09. */
    std::array<std::uint8_t,32>elf{};
    /** @brief HELLO должен совпасть целиком; дополнительные неизвестные capability не принимаются. */
    std::uint32_t expectedCapabilities=raw::helloCapabilities;
    /** @brief Cold opt-in только для нового qualified image с lifecycle::capability; default сохраняет legacy. */
    bool warmIdleUnbind=false;
};
/** @brief Последний immutable HELLO resource sample, числа в bytes/Hz; не completion App кадра. */
struct RawHello final {std::uint32_t freeBytes{},minimumBytes{},largestBytes{},workerStackBytes{},hz{};};
/** @brief Только Main clock в микросекундах, вызываемый при start/completion, никогда на Pending. */
using ClockUs=std::uint64_t(*)() noexcept;
/**
 * @brief Адаптер BatchClient для двух слотов RawChannel.
 * @details Один Main owner, одна session до checked UNBIND, максимум один запрос. RX/driver callbacks
 * работают лишь через Channel. Нет waits/alloc/log/новых tasks, нет OTA/boot/CLI.
 * Channel живёт до reboot; timeout не cancel/release. Stable reply копируется до release.
 */
class RawAsyncAdapter final {
public:
    /** @brief Заимствует boot-long Channel и monotonic clock; ctor не вызывает транспорт. */
    RawAsyncAdapter(raw::Channel&channel,const RawProfile&profile,ClockUs clock) noexcept:channel_(channel),profile_(profile),clock_(clock){}
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    RawAsyncAdapter(const RawAsyncAdapter&)=delete;
    /** @brief Запрещает копирование владельца и дублирование borrowed источников. */
    RawAsyncAdapter&operator=(const RawAsyncAdapter&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    RawAsyncAdapter(RawAsyncAdapter&&)=delete;
    /** @brief Запрещает перенос ownership/callback адреса живого session. */
    RawAsyncAdapter&operator=(RawAsyncAdapter&&)=delete;
    /** @brief HELLO100 проверяет actual ELF/1ff/profile2/request1400/reply1792 в exact session. */
    bool beginHello() noexcept;
    /** @brief Проверяет cold profile/key/ELF и поддерживаемый exact capability set до allocation/RPC. */
    static bool validProfile(const RawProfile&profile) noexcept;
    /** @brief StartFn: INIT/BIND/service40..46;47 только после explicit qualified warm HELLO и полного request88. */
    static bool start(void*context,const raw::Key&key,const std::uint8_t*data,std::size_t bytes) noexcept;
    /** @brief PollFn: Complete требует matching reply+TXdone, а не только RX или готовыйresult. */
    static TransportState poll(void*context,ReplyView&reply) noexcept;
    /** @brief Пропущенный caller deadline навсегда удерживает неизвестный request до внешнего reset. */
    void noteTimeout() noexcept;
    /** @brief Только проверенный HELLO и неpoison разрешают INIT; это не доказательство cold/BIND. */
    bool qualified()const noexcept{return qualified_&&!poisoned_;}
    /** @brief Новый checked capability разрешён только exact HELLO/profile, а не по одному op47 reply. */
    bool warmQualified()const noexcept{return qualified()&&profile_.warmIdleUnbind&&(profile_.expectedCapabilities&lifecycle::capability);}
    /** @brief Проверенный ACK и завершённый P4 TX; C6 ACK-sourceDone отдельно обеспечивает firmware gate. */
    bool unboundAckVerified()const noexcept{return warmQualified()&&unbindVerified_&&!pending_&&!channel_.occupied();}
    /** @brief Проверяет новое HELLO identity без изменения old profile; session должна отличаться, другие fresh domains проверяет client. */
    bool canWarmRebind(const raw::Key&nextHello)const noexcept;
    /**
     * @brief После exact UNBIND ACK меняет только session metadata для нового HELLO без allocation.
     * @details Image/capability остаются прежними. C6 обязан поставить ранний HELLO в bounded Ready,
     * но не исполнять его до actual ACK-sourceDone; P4 не подменяет это таймером.
     */
    bool reprofileWarm(const raw::Key&nextHello) noexcept;
    /** @brief Возвращает собственный числовой снимок после read-only HELLO. */
    const RawHello&hello()const noexcept{return hello_;}
private:
    /** @brief Проверяет immutable binding и один transport request до первой submit. */
    bool submit(const raw::Key&key,const std::uint8_t*data,std::size_t bytes,bool isHello) noexcept;
    /** @brief Проверяет HELLO100 body без зависимости от C6 pointer ABI. */
    bool verifyHello(const raw::View&reply) noexcept;
    raw::Channel&channel_;RawProfile profile_{};ClockUs clock_{};raw::Key request_{};
    std::array<std::uint8_t,raw::packetBytes>reply_{};RawHello hello_{};
    std::uint64_t started_{};bool pending_{},helloPending_{},qualified_{},poisoned_{},unbindVerified_{};
};
} // namespace nes::c6audio::async
