#pragma once
#include "Machine.h"
#include "apu/AudioRenderer.h"
#include "FrameRamWitness.h"
#include "apu/AudioPacketView.h"
#include <cstddef>
#include <cstdint>
#include "FrameHartStats.h"


namespace nes::app {
// Роли владельцев передаются start/done-семафорами App. Одновременно общими
// остаются только stop/fault и явная публикация слотов.

/**
 * @brief Двухслотовый runtime non-MMC5: CPU1 публикует точный Output, Main потребляет RGB и POSTJOIN audio.
 * @details Публикация только paired; диагностические counters не являются game FPS.
 */
class FrameVideoPipeline final {
public:
    static constexpr unsigned slotsCount=2;
    static constexpr unsigned eventsPerSlot=core::ColourPacket::maximumEvents;
    /**
     * @brief Точная аренда уже joined аудио; адрес producer — только identity, не право чтения.
     * @details Header и output ticket копируются, used arrays остаются в прежнем Payload.
     * Первый audio base равен0, тогда как output base первого кадра равен reset7.
     */
    struct JoinedAudioTicket {
        core::OutputFrameTicket output{};
        core::AudioPacket::Header audio{};
        std::uint64_t appGeneration=0;
    };
    /**
     * @brief После настоящего done и joinedProducer заимствует immutable audio текущего N.
     * @details Выходы не меняются при отказе. Один borrow на slot; accept освобождает
     * видео как прежде, но audioSlotAvailable запрещает перезапись до sourceDone.
     */
    bool borrowJoinedAudio(core::AudioPacketView& view,JoinedAudioTicket& ticket)noexcept;
    /**
     * @brief Main возвращает точный borrow только после DATA+TX sourceDone и joined N.
     * @details inputSourceReleased — аттестация адаптера после AudioPublished reclaimJoined;
     * timeout/PCM ready/пустая очередь сами по себе не разрешают release.
     */
    bool releaseJoinedAudio(const JoinedAudioTicket& ticket,bool inputSourceReleased)noexcept;
    /** @brief Main читает cached sealed FM tape только под тем же actual joined audio ticket. Не читает Machine N+1. */
    bool borrowJoinedVrc7Tape(const JoinedAudioTicket&,core::Vrc7WriteTape::View&)const noexcept;
    /** @brief Неизменная cold capability; не разрешает чтение producer во время Worker. */
    bool cpu0Vrc7Enabled()const noexcept;
    /** @brief Main читает исходный абсолютный deadline cached ReadyN; новый срок FM-кванта не создаётся. */
    std::int64_t pendingDeadlineUs()const noexcept;
    /** @brief Main проверяет следующий ping-pong slot, не читая живой producer. */
    bool audioSlotAvailable(std::uint64_t nextInput)const noexcept;
    /**
     * @brief Cold Main копирует полный cached lease после createOutput, до первого dispatch.
     * @details romEpoch сохранён ДО reset configureOutputFrames; transportEpoch выдан Core.
     * Повторный live Machine getter здесь запрещён. Выход не меняется при отказе.
     */
    bool outputLeaseAtCreate(core::OutputFrameLease& out)const noexcept;
    
    /**
     * @brief Main-owned исторический accepted CPU/RAM/input/PCM/RGB снимок.
     * @details Сохраняется независимо от работающего producer N+1.
     */
    struct Snapshot {
        core::Cpu6502::State cpu{};
        std::uint64_t frame=0,cycle=0,appGeneration=0,inputFrame=0;
        std::uint64_t ramHash=0,pcmHash=0,rgbHash=0,readHash=0,readCount=0;
        std::uint64_t audioGeneration=0,audioSequence=0,colourSequence=0;
        std::size_t sampleCount=0;
        core::Region region=core::Region::Ntsc;
        std::uint8_t pad=0;
        FrameRamWitness ram{}; /**< @brief Отдельный RAM ключ/availability, не validity RGB/PCM. */
    };
    
    /**
     * @brief Длительности и объём работы рабочего конвейера Output; Main публикует их после paired accept.
     * @details Время consumer/producer либо ожидания может перекрываться; значения не суммируются в FPS.
     */
    struct Times {
        std::uint64_t core=0,producer=0,seal=0,audioRetire=0,snapshot=0;
        std::uint64_t replay=0,dsp=0,witness=0;
    };
    
    /**
     * @brief Коды первого отказа рабочего видеоконвейера; ошибка запрещает принятие частичного результата.
     */
    enum class Error:unsigned {None,Binding,Allocation,Owner,Slot,Producer,Colour,
        Audio,Cut,Lease,Sequence,Stopped,Deadline,Replay,Dsp,Output,Busy};
    /**
     * @brief На true joined reset7 создаёт только output compositor, без C0 Machine.
     * @details DIRECT_RGB/EARLY_DSP/FAST_SNAPSHOT обязательны. Все buffers/ROM
     * удерживаются до реального Worker/consumer/DSP/PPA завершения. Active Audio
     * передан явно: Core configure выполняет reset, фабрика затем фиксирует его
     * новый реальный header. nullptr возможен до binding либо после доказанного
     * cleanup; sticky handle означает retained/quarantined resources.
     * @param producer Исключительный владелец loaded reset7, без прежнего writer.
     * @param rom Полный неизменный образ с действительной lifetime lease.
     * @param bytes Размер полного ROM, не PRG-поддиапазона.
     * @param region Точный NTSC/PAL всех paired источников.
     * @param generation Ненулевая App generation исходного контекста.
     * @param activeAudio Реальный активный пакет producer; Header отдельно недостаточен.
     * @param borrowedRgb Свободный постоянный Board source после actual PPA drain.
     * @param borrowedWords Ровно61440 элементов RGB565.
     * @param cpu0Vrc7 Cold opt-in для actual FM85: только raw writes, без FM-синтеза CPU1.
     */
    static FrameVideoPipeline* createOutput(core::Machine& producer,const std::uint8_t* rom,std::size_t bytes,
        core::Region region,std::uint64_t generation,core::AudioPacket& activeAudio,
        std::uint16_t* borrowedRgb,std::size_t borrowedWords,
        bool cpu0Vrc7=false)noexcept;
    /**
     * @brief Main задаёт RAM-only политику один раз до первого dispatch.
     * @details Только исходный joined контекст фабрики; Full остаётся умолчанием.
     */
    bool setRamWitnessPolicyAtCreate(FrameRamPolicy policy)noexcept;
    // Нужны реальный Worker done/join и отсутствие активного Main consumer. Отказ
    // удерживает все аллокации; caller не освобождает контекст с неизвестным владельцем.
    
    /**
     * @brief Освобождает контекст только после реального Worker done и отсутствия активного Main consumer/DSP.
     * @details При неизвестном владении возвращает false и удерживает память/ROM; порядок detach предшествует free.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Фабричный контекст после настоящего Worker done; неизвестное владение возвращает false с удержанием всех ресурсов.
     */
    static bool destroyJoined(FrameVideoPipeline*)noexcept;
    
    /**
     * @brief На Main перед dispatch резервирует свободный producer-слот и фиксирует inputFrame/pad/App generation.
     * @details Не читает работающий producer и отказывает при незавершённой предыдущей передаче.
    
     * @param[in] inputFrame Исторический номер кадра ввода для следующего CPU job, не номер LCD submission.
     * @param[in] pad Фактическая восьмибитная маска pad, применяемая к этому job.
     * @param[in] appGeneration Текущее App generation; повторное использование старого сеанса запрещено.
     */
    bool prepareJob(std::uint64_t inputFrame,std::uint8_t pad,std::uint64_t appGeneration)noexcept;
    
    /**
     * @brief На Worker выполняет обычный observable CPU-кадр, seal colour/audio и неизменный CPU/RAM/input snapshot.
     * @details Публикует только полностью согласованный слот; локальный guard отмечает фактический выход Worker.
     */
    /** @brief Только на Worker выполняет кадр и seal/snapshot; освобождение аренды требует настоящего done. */
    core::RunStatus produce()noexcept;               // Только Worker.
    
    /**
     * @brief На Main после настоящего done проверяет producer epoch/region/cut, slot publication и отсутствие faults.
     * @details Только здесь разрешены соответствующие producer getters.
     */
    bool joinedProducer()noexcept;                  // Main после настоящего done.
    // Только Main: sealed slot и логический consumer lease удерживаются между
    // успешной подготовкой и colour/abort. Renderer должен оставаться живым.
    // Подготовка не трогает Ppu/RGB/Board; caller по-прежнему владеет PPA fence.
    
    /**
     * @brief На Main удерживает sealed слот и consumer lease, выполняя DSP до ожидания предыдущего PPA.
     * @details Ppu/RGB/Board не трогает; caller обязан сохранить renderer и проверить reentrant lifecycle после возврата.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Живой Main-owned AudioRenderer для sealed N; удерживается до colour/abort и не читается Main при Worker loan.
     */
    bool prepareDsp(core::AudioRenderer&)noexcept;
    
    /**
     * @brief После настоящего PPA drain на Main воспроизводит sealed colour N через единственный C0 actor.
     * @details Сверяет реальные read-byte/cut/witness и ранее подготовленный DSP; ошибка запрещает paired принятие.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Тот же renderer и неизменный DSP-stage N; Board/PPA fence обязан уже выполниться.
     */
    /** @brief Callback синхронный, не сохраняется; false запрещает accept и возвращается до внешнего fault-cleanup. */
    bool consumeColour(core::AudioRenderer&,bool(*progress)(void*)noexcept=nullptr,void*context=nullptr)noexcept;
    
    /**
     * @brief На Main безопасно закрывает подготовленный DSP-stage либо удерживает его при необратимом Worker-владении.
     * @details Не выдаёт отмену за завершение исполняющегося DSP.
     */
    bool abortPrepared()noexcept;
    
    /**
     * @brief На Main потребляет предыдущий sealed кадр с реальным DSP и C0 replay либо делегирует EarlyDSP-фазы.
     * @details Нельзя вызывать при владении PPA выбранным RGB или при чужом consumer.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Эксклюзивный реальный AudioRenderer для paired sealed кадра; указатель живёт до завершения consume.
     */
    bool consume(core::AudioRenderer&)noexcept;      // Main, только предыдущий кадр.
    
    /**
     * @brief После проверки обоих владельцев переносит согласованные CPU/RAM/input/audio/colour данные в Main-owned accepted snapshot.
     * @details Частичный или stale кадр не становится accepted.
    
     * @param[in] appGeneration Текущее App generation, совпадающее с подготовленным paired stage перед публикацией.
     */
    bool accept(std::uint64_t appGeneration)noexcept;// Main после всех проверок.
    
    /**
     * @brief В отдельном STATUS-наблюдателе обновляет хэши только Main-owned принятого RGB/PCM.
     * @details Не обращается к producer N+1 и не добавляет большие хэши незаметно в каждый измеряемый кадр.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Main-owned renderer именно accepted audio generation/sequence/cut; не producer N+1.
     */
    bool refreshWitness(core::AudioRenderer&)noexcept;// STATUS observer; только принятые Main-owned байты.
    
    /**
     * @brief На joined pause-границе обновляет App generation без подмены ROM/cut/transport epoch.
     * @details Требует завершённых владельцев и отсутствия непарного stage.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Новое ненулевое App generation на joined pause-границе; transport epoch/ROM не меняются.
     */
    bool rebindAppGeneration(std::uint64_t)noexcept; // Joined pause-граница.
    
    /**
     * @brief Сверяет неизменные App/ROM/region/generation данные контекста, допустимые во время Worker.
     * @details Актуальный producer epoch/cut дополнительно проверяет joinedProducer после реального done.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Адрес удерживаемого ROM текущего контекста.
     * @li 2 — Полный размер ROM image в байтах.
     * @li 3 — Ожидаемый неизменный NTSC/PAL регион.
     * @li 4 — Текущее App generation, без чтения mutable producer epoch во время job.
     */
    bool matches(const std::uint8_t*,std::size_t,core::Region,std::uint64_t)const noexcept;
    
    /**
     * @brief Атомарно помечает sticky-остановку и первую причину.
     * @details Borrowed context/slot/ROM остаются живыми до действительного joined teardown.
    
     * @par Безымянные параметры в порядке объявления
     * @li 1 — Sticky Error остановки; значение по умолчанию Stopped, без освобождения работающих владельцев.
     */
    void stop(Error=Error::Stopped)noexcept;
    
    /**
     * @brief Возвращает атомарно опубликованную первую ошибку контекста.
     * @details None не отменяет требований владельца для остальных getters.
     */
    Error error()const noexcept;
    
    /**
     * @brief Показывает наличие готового sealed слота для Main.
     * @details Признак не равен публикации на LCD и не разрешает чтение producer.
     */
    bool hasReady()const noexcept;
    
    /**
     * @brief Проверяет атомарное владение producer-заданием.
     * @details false само по себе не заменяет настоящий done-семафор App.
     */
    bool borrowedByWorker()const noexcept;
    
    
    
    /**
     * @brief Возвращает Main-счётчик подтверждённых producer-кадров.
     * @details Он может опережать accepted/submitted и не является FPS дисплея.
     */
    std::uint64_t producedFrames()const noexcept;
    
    /**
     * @brief Возвращает число принятых обоими владельцами кадров.
     * @details STATUS-drain может принять кадр без LCD submission.
     */
    std::uint64_t acceptedFrames()const noexcept;
    
    /**
     * @brief Возвращает cycle последнего Main-owned принятого снимка.
     * @details Работающий producer N+1 через getter не читается.
     */
    std::uint64_t acceptedCycle()const noexcept;
    
    /**
     * @brief Возвращает заимствованный неизменный Main-owned accepted snapshot.
     * @details Он сохраняет исторический input/CPU/RAM/PCM/RGB кадра N независимо от producer N+1.
     */
    const Snapshot& snapshot()const noexcept;
    
    /**
     * @brief Возвращает Main-owned времена последнего paired кадра.
     * @details C1 producer и C0/DSP интервалы могут перекрываться; их сумма не является длительностью общего цикла.
     */
    const Times& times()const noexcept;
    /**
     * @brief На Main печатает агрегат принятых кадров и очищает только своё окно.
     * @param game Номер игры в существующем диагностическом журнале.
     * @details Не читает Worker/producer и не означает завершение его следующего job.
     */
    void reportHartStats(unsigned game)noexcept;
    /** @brief На Main очищает своё окно; sealed/Worker Payload и гостевое состояние не меняет. */
    void clearHartStats()noexcept;
    
    /**
     * @brief Возвращает RGB только принятого и проверенного Main кадра.
     * @details Даже getter допустим лишь при выполненном caller PPA fence и действующем контексте.
     */
    const std::uint16_t* pixels()const noexcept;
private:
#if defined(NES_APP_FRAME_NATIVE_PROOF) && NES_APP_FRAME_NATIVE_PROOF
    // Native proof получает только явный friend; layout и рабочая логика не меняются.
    friend struct FrameTest;
#endif
    
    /**
     * @brief Состояние рабочего двухслотового конвейера: INTERNAL Control, PSRAM Payload и отдельные Main/Worker поля.
     * @details Main управляет сроком жизни; Worker заимствует свой слот до done, а backing и ROM живут до join Worker/consumer/DSP/PPA.
     */
    struct Impl;
    
    /**
     * @brief Создаёт лёгкую оболочку над уже сконструированным Impl.
     * @details Сам конструктор не приобретает ROM, PPA или слот.
    
     * @param[in] p Уже созданный стабильный Impl; оболочка не выделяет backing повторно.
     */
    explicit FrameVideoPipeline(Impl*p)noexcept:state_(p){}
    Impl*state_;
};
}
