#pragma once
#include "DonePollBatch.h"
// Типы handoff объявлены независимо от PRIVATE выбора backend: все consumers
// видят одинаковые сигнатуры, не включая SDK или реализацию remote-пакета.
namespace nes::c6audio::async {class AudioPublished;struct PublishedKey;struct JoinedFrame;}
#include "Machine.h"
#include "board/GameSourceLease.h"
#include <cstdint>
#include <cstddef>
namespace nes::core {class AudioRenderer;class AudioStageToken;}
namespace nes::app {
// Main владеет lifecycle/C0; Worker заимствует лишь стабильный endpoint задания.
// Любое неатомарное наблюдение ниже требует обычной joined-границы задания.

/**
 * @brief M4/MMC5 диагностический Signals/C0 pipeline с once-only PPU-read RPC.
 * @details Main владеет C0/lifecycle, Worker заимствует стабильный endpoint; free разрешён лишь после реального join.
 */
class CoupledVideoPipeline final {
public:
    
    /**
     * @brief Счётчики длительностей и работы этого benchmark.
     * @details Время consumer/producer либо ожидания может перекрываться; значения не суммируются в FPS.
     */
    struct Times {std::uint64_t consumer=0,read=0,maxRead=0,prefix=0,reads=0,prefixes=0,commit=0,workerDsp=0,ackReadyBeforeDsp=0,dspFrames=0;
        // Host wall, мкс: Worker пишет per-job, Main читает только после actual done.
        // Stage включает held-wait; эти интервалы не складываются с commit/FPS.
        std::uint64_t remoteStage=0,remoteStageMax=0,remoteHeld=0,remoteHeldMax=0,
            remoteHeldEntries=0,remoteHeldExits=0,remoteStageFrames=0,remoteClockErrors=0;};
    
    /**
     * @brief На joined/reset7 Main выделяет выровненные control/Impl и PSRAM staging, выполняет debt barrier, привязывает bridge, thin Signals и RGB.
     * @details При отказе сохраняет порядок detach/park до освобождения; ROM должен оставаться живым.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Joined producer Machine на reset7, с нужным split audio и drained debt.
     * @li 2 — Неизменное полное ROM image, живое до detach/обоих park.
     * @li 3 — Размер этого ROM image в байтах.
     * @li 4 — Регион NTSC/PAL, совпадающий с producer и C0.
     * @li 5 — Ненулевое App generation, записываемое в неизменный transport lease.
     */
    static CoupledVideoPipeline* create(core::Machine&,const std::uint8_t*,std::size_t,core::Region,std::uint64_t,std::uint16_t*alternate=nullptr)noexcept;
#if defined(NES_DIAGNOSTIC_MMC5_INDEX_MEMORY)
    /** @brief Для следующей сессии выбирает RGB-only строку в HP SRAM для MMC5. */
    static void setIndexedMemoryProbe(bool enableHp)noexcept;
#endif
    // Producer сброшен и явно detached, пока endpoint/ROM ещё живы;
    // его job loop joined. Main последовательно принимает простаивающую producer-роль.
    
    /**
     * @brief Уничтожает контекст после реального завершения Worker и явного detach producer.
     * @details Main последовательно паркует endpoints; неизвестное владение нельзя заменять истечением timeout.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Фабричный контекст после настоящего Worker done и producer detach; nullptr допустим.
     */
    // false сохраняет весь контекст и owned A при активном Reserved/Reading;
    // вызывающий не обнуляет handle и не освобождает ROM. true завершает teardown.
    static bool destroyJoined(CoupledVideoPipeline*)noexcept;
    
    /**
     * @brief Запрещает копирование владельца ресурсов.
     * @details Передача ordinary полей, атомарных controls и callback-регистрации копией не поддерживается.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Источник запрещённого копирования; передача его resource/endpoint ownership не выполняется.
     */
    CoupledVideoPipeline(const CoupledVideoPipeline&)=delete;
                /**
                 * @brief Запрещает копирование владельца ресурсов.
                 * @details Передача ordinary полей, атомарных controls и callback-регистрации копией не поддерживается.
                
                 * @par Безымянные параметры в порядке объявления
                 * @li 1 — Источник запрещённого копирования; передача его resource/endpoint ownership не выполняется.
                 */
                CoupledVideoPipeline&operator=(const CoupledVideoPipeline&)=delete;
    
    /**
     * @brief На joined Main проверяет producer, ROM, размер, регион, поколение и актуальную transport epoch.
     * @details Не предназначен для чтения producer во время его работы.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Joined producer Machine, чья актуальная epoch читается в этом вызове.
     * @li 2 — Адрес того же удерживаемого ROM image.
     * @li 3 — Полный размер ROM image в байтах.
     * @li 4 — Неизменный регион текущего сеанса.
     * @li 5 — Ожидаемое App generation; номер кадра сюда не передаётся.
     */
    bool matches(const core::Machine&,const std::uint8_t*,std::size_t,core::Region,std::uint64_t)const noexcept;
    
    /**
     * @brief До публикации start сбрасывает принятие/счётчики и задаёт крайний срок задания.
     * @details Все обычные поля в этот момент принадлежат Main.
     */
    void startJob()noexcept;
    /** @brief Неизменный feature admit: только MMC5; читается Worker либо joined Main. */
    bool preAckDspEnabled()const noexcept;
    /**
     * @brief Joined Main предлагает прежние packet/token и immutable ключ следующего задания.
     * @details Publication и источники живут до true done и снятия соответствующих
     * borrows; после start Main не читает primary и обычные поля RemoteLoan.
     * @return false при неподходящем состоянии/ключе либо выключенном backend/PREACK.
     * Успех не означает готовность PCM или завершение remote-транспорта.
     */
    bool offerRemoteAudio(core::Machine&,core::AudioPacket&,core::AudioStageToken&,::nes::c6audio::async::AudioPublished&,const ::nes::c6audio::async::PublishedKey&)noexcept;
    /**
     * @brief После actual done принимает точные joined metadata входного кадра N.
     * @details Caller уже проверил job result, generation, ROM lease, input и cut.
     * Выходной JoinedFrame заимствован только на вызов; PCM_N может быть pending.
     * @return true лишь при exact once-token/lease/bridge/sequence соответствии;
     * false не снимает publication/Raw/PCM borrows и не разрешает free.
     */
    bool acceptRemoteAudioJoined(core::Machine&,std::uint64_t,::nes::c6audio::async::JoinedFrame&)noexcept;
    /** @brief Joined Main отдаёт прежние packet/renderer/PCM/token одному следующему input; refs живы до true done. */
    bool offerPreAckDsp(core::Machine&,core::AudioPacket&,core::AudioRenderer&,std::int16_t*,std::size_t,core::AudioStageToken&,std::uint64_t)noexcept;
    /** @brief Worker после Machine.stepFrame/ACK завершает staged packet без второй copy. */
    bool finalizePreAckDsp(core::Machine&)noexcept;
    /** @brief Joined Main проверяет полный ключ и принимает PCM вместо повторного render. */
    bool acceptPreAckDspJoined(core::Machine&,std::uint64_t,std::uint32_t&)noexcept;
    /** @brief Только после true join и между C0 callbacks снимает stageborrow перед reset/detach. */
    bool abandonPreAckDspJoined(core::Machine&)noexcept; // Только Main, до публикации start.
    
    /**
     * @brief На Main обслуживает очередной доступный запрос receiver и накапливает его wall time.
     * @details Producer Machine не наблюдает; Empty также проходит через измеряемый вызов.
     */
    coupled_done::VideoResult pump()noexcept; // Только Main. Обрабатывает до одного запроса.
    
    /**
     * @brief Сравнивает монотонное время с deadline задания.
     * @details Не отменяет Worker и не доказывает park.
     */
    bool expired()const noexcept;
    
    /**
     * @brief Устанавливает sticky abort текущей transport session.
     * @details Во время задания разрешены только атомарные изменения остановки; освобождения здесь нет.
     */
    void stop()noexcept; // Атомарный abort координатора с session-тегом; безопасен во время задания.
    
    /**
     * @brief На Main между pump-вызовами паркует consumer, чтобы тот сам инвалидировал C0 актор после активного callback.
     */
    void parkMain()noexcept; // Только Main между pump-вызовами.
    
    /**
     * @brief На Worker проверяет transport/thin faults и его собственный producer Thin binding перед гостевой работой CPU1.
     * @details Не читает изменяемые Main lifecycle-флаги.
     */
    bool workerCanRun()const noexcept; // Только Worker: атомарная проверка stop до гостевой работы.
    
    /**
     * @brief На Worker проверяет его итоговый endpoint без чтения Main lifecycle.
     * @details Настоящий done публикуется внешним worker-протоколом App.
     */
    bool workerFinished()noexcept; // Только Worker; Main lifecycle-флаг не читается.
    
    /**
     * @brief На joined Main сверяет завершение обоих акторов, cut/frame и полный clock/APU witness.
     * @details Ошибка запрещает принятие RGB и связанного audio.
    
     * @param[in] cycle Абсолютное число завершённых CPU cycles производителя, не число PPU dots.
     * @param[in] frame Число завершённых producer кадров, сверяемое с thin commit counter.
     */
    bool jointReady(std::uint64_t cycle,std::uint64_t frame)noexcept;
    
    /**
     * @brief При joined RGB-границе подтверждает согласованный вывод.
     * @details Вызывающая сторона заранее завершает владение PPA и проверяет реальные DSP/аудио-свидетели.
     */
    bool accept()noexcept; // Joined RGB fence на Main; запрещён при владении Board со стороны PPA.
    
    /**
     * @brief Возвращает заимствованный staging RGB только после успешного принятия.
     * @details Указатель не даёт права обходить joined/PPA границы.
     */
    const std::uint16_t* pixels()const noexcept;
    /** @brief Указывает, зарегистрированы ли два точных источника A/B в этом сеансе. */
    bool rgbPoolEnabled()const noexcept;
    /** @brief Выдаёт принадлежащую Impl аллокацию A только для Board registration; не current writer. */
    const std::uint16_t*ownedRgb()const noexcept;
    /** @brief Возвращает принятый immutable ticket; пуст до accept, не изменяет reader credit. */
    platform::GameSourceTicket acceptedSource()const noexcept;
    /** @brief Резервирует accepted source до Board begin; false не меняет credit. */
    bool borrowSource(const platform::GameSourceTicket&ticket)noexcept;
    /** @brief После Board begin переводит Reserved в Reading, в том числе при раннем ISR completion. */
    bool confirmSource(const platform::GameSourceTicket&ticket)noexcept;
    /** @brief Отменяет Reserved только после отказа Board begin, когда SDK не удерживает источник. */
    bool cancelSource(const platform::GameSourceTicket&ticket)noexcept;
    /** @brief Возвращает Reading credit только после настоящего полного Board finish с тем же ticket. */
    bool completeSource(const platform::GameSourceTicket&ticket)noexcept;
    /** @brief На joined Main переводит RGB actor на другой Free A/B до допуска следующего Worker job. */
    bool prepareNextRgb(const platform::GameSourceTicket&ticket)noexcept;
    
    /**
     * @brief Возвращает Main-снимок измерений текущего задания.
     * @details Поля перекрываются с работой другого ядра и не складываются в FPS.
     */
    const Times& times()const noexcept;
private:
    
    /**
     * @brief Рабочий владелец coupled-сеанса: очередь bridge, отдельный C0 PPU/MMC5/audio actor и Main/Worker состояние.
     * @details Factory владеет INTERNAL Control и PSRAM Impl/RGB до joined teardown; ROM и внешние RGB остаются у caller, Worker передаёт события в bridge, Main исполняет actor.
     */
    struct Impl;Impl* state_;
    
    /**
     * @brief Сохраняет указатель на уже созданный Impl.
     * @details Ресурсы освобождает только специальный joined-путь фабрики.
    
     * @param[in] p Уже созданный Impl, ownership которого освобождается только destroyJoined.
     */
    explicit CoupledVideoPipeline(Impl* p)noexcept:state_(p){}
    
    /**
     * @brief Завершает оболочку после уничтожения её Impl фабричным joined-путём.
     * @details Сам по себе деструктор не заменяет park/detach.
     */
    ~CoupledVideoPipeline()=default;
};
}
