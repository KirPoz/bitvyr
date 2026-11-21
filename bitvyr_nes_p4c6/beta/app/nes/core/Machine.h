/**
 * @file Machine.h
 * @brief Публичный владелец NES-сессии и внутренние адаптеры конкретных шин.
 * @ingroup nes_core
 */
#pragma once

#include "Types.h"
#include "AudioStageToken.h"

#include "OutputFrame.h"
#include "ThinPpu.h"
#include "ColourStream.h"
#include "cpu/Cpu6502.h"
#include "ppu/Ppu.h"
#include "ppu/VideoPacket.h"
#include "ppu/TimingPpu.h"
#include "ppu/NameMarkerRows.h"
#include "apu/Apu.h"
#include "apu/TimingApu.h"
#include "apu/AudioPacket.h"
#include "cartridge/Cartridge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

// Единый внутренний compile-domain прежнего B3. Это не отдельная PUBLIC настройка.
#define NES_MMC5_EXACT_QUIET_DOMAIN 1
#define NES_MMC5_GENERALIZED_QUANTUM_DOMAIN 1

namespace nes::core {
struct RomWindow;

// Владеет эмулируемыми тактами, памятью, микросхемами и картриджем. Вызывающий
// сохраняет ROM; плата, накопитель, дисплей и HID остаются вне ядра.
class Machine final : private CpuBus, private PpuBus, private ApuBus
    , private ApuTimingBus
{
public:
    /** @brief Создаёт цифровые устройства с обычным выделением буфера legacy PPU; ROM загружается отдельно. */
    Machine() noexcept;
    // Только погашенный срез сброса. Null явно выбирает обычного владельца после
    // сброса обеих сторон; промежуточный отказ не включает fallback автоматически.
    /**
     * @brief Подключает канал тонкого PPU на остановленном сбросном срезе.
     * @details Обе стороны должны быть согласованы по ROM, эпохе и фазе. Ошибка активного канала не разрешает скрытый переход к другому владельцу.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool configureThinPpu(ThinPpuChannel*,const ColourLease& = {},ThinMode=ThinMode::Signals) noexcept;
    /**
     * @brief Проверяет выбранный режим или наличие нужного хранилища владельца.
     * @details Не меняет режим, не выделяет буферы и не доказывает завершение внешних пользователей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool thinPpuBound()const noexcept{return thin_!=nullptr;}
    /**
     * @brief Проверяет выбранный режим или наличие нужного хранилища владельца.
     * @details Не меняет режим, не выделяет буферы и не доказывает завершение внешних пользователей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool thinSignalMode()const noexcept{return thin_&&thin_->mode()==ThinMode::Signals;}
    /**
     * @brief Завершает время жизни объекта с полиморфным либо владеющим контрактом.
     * @details Связанные внешние bus/ROM/буферы должны сохраняться до окончания всех активных пользователей.
     */
    ~Machine() noexcept {
        cartridge_.detachCpuPrgTail();
        invalidateThinPpu();
        invalidateColourCapture();
    }
    /**
     * @brief Открывает запись точного CPU-видеопотока в свободный слот.
     * @details Проверяет аренду ROM, эпоху, режим владельца и неперекрытие, затем доводит текущие долги перед фиксацией исходного среза.
     * @param sequence Последовательный номер интервала; не номер физической подачи LCD.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool beginColourCapture(ColourPacket&,const ColourLease&,std::uint64_t sequence) noexcept;
    /**
     * @brief Закрывает видеослот на реальной готовой кадровой границе.
     * @details Проверяет цикл, фазу и отсутствие ошибок; публикует неизменяемый конец, а не будущий предполагаемый бюджет.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool sealColourCapture(ColourPacket&) noexcept;
    /**
     * @brief Прекращает захват и инвалидирует текущую эпоху цветового транспорта.
     * @details Завершает связанные заимствования. Вызывающий удерживает ROM и память слотов до фактического освобождения обоих владельцев.
     */
    void cancelColourCapture() noexcept { invalidateColourCapture(); }
    /**
     * @brief Возвращает текущую эпоху привязки цветового транспорта.
     * @details Изменение эпохи запрещает продолжать старые слоты или старые ROM-аренды.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t colourTransportEpoch()const noexcept{return colourEpoch_;}
    static constexpr std::size_t cpuPrgTailBytes=16384U,cpuPrgTailAlignment=64U;
    // Только владелец заимствует неизменяемый источник: вызывающий обеспечивает реальное
    // исключение исполнения, сохраняет отдельную изменяемую арену и неизменный ROM аренды
    // до успешной отвязки либо инвалидирования при reset/load/смене эпохи.
    // Существующее изменение caller-буфера при загрузке/без привязки сохранено.
    // Нет аллокаций, гостевых тактов, checksum источника или периодического копирования.
    /**
     * @brief Привязывает отдельную арену неизменяемого хвоста PRG к текущей ROM-аренде.
     * @details Проверяет границу владения, размер, выравнивание и неперекрытие. Арену и исходный ROM нельзя менять до успешного unbind либо корректного окончания эпохи.
     * @param arena Отдельная writable-арена вызывающего; её нельзя освобождать до окончания заимствования.
     * @param bytes Доступная длина переданного хранилища в байтах.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool bindImmutableCpuPrgTail(const ColourLease&,void* arena,std::size_t bytes) noexcept;
    /**
     * @brief Отсоединяет резидентный хвост PRG на исключительном срезе.
     * @details Успех завершает заимствование арены; отказ не даёт права освобождать память, ещё доступную владельцу.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool unbindImmutableCpuPrgTail() noexcept;
    /**
     * @brief Сообщает, подключена ли резидентная арена PRG.
     * @details Не подтверждает аппаратные возможности памяти и не изменяет её владение.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool immutableCpuPrgTailBound() const noexcept { return cartridge_.cpuPrgTail_.resident!=nullptr; }


    /** @brief Принимает политику размещения буфера legacy PPU; ROM и внешние пакеты передаются после создания машины. */
    explicit Machine(LegacyPixelAllocator allocator) noexcept;
    /**
     * @brief Проверяет выбранный режим или наличие нужного хранилища владельца.
     * @details Не меняет режим, не выделяет буферы и не доказывает завершение внешних пользователей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool legacyVideoReady() const noexcept { return ppu_.pixelsReady(); }
    // Только владелец: configure/reset/load требуют простоя worker и погашенных пакетов.
    /**
     * @brief Подключает заимствованный видеопакет и наблюдателя раздельного вывода.
     * @details Разрешён только при остановленном CPU-владельце и освобождённых пакетах; nullptr возвращает обычный путь.
     * @param storage Заимствованное хранилище выбранного протокола; nullptr обрабатывается контрактом метода.
     * @param observer Заимствованный наблюдатель опубликованных данных; не владелец CPU/ROM.
     */
    void configureSplitVideo(VideoPacket* storage, VideoObserver* observer = nullptr) noexcept;
    /**
     * @brief Завершает активный видеопакет и подключает другой свободный слот.
     * @details Вызывающий обеспечивает исключительное владение и отдельное хранилище. Сначала доводятся реальные долги и пиксели, затем публикуется старый пакет.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param nextFree Отдельный действительно Free-видеослот, предоставляемый только после освобождения прежнего consumer.
     * @param retired Выходной указатель на прежний запечатанный видеослот; освобождается лишь после завершения consumer.
     */
    bool swapVideoPacket(VideoPacket& nextFree, VideoPacket*& retired) noexcept;
    /**
     * @brief Выбирает Timing-владельца и две отдельные output-арены на reset7.
     * @details Только исключительный владелец после настоящего join. ROM и все
     * диапазоны binding сохраняются до успешного cancel. Настройка делает reset,
     * поэтому первый видеопакет начинается на 0, а первый гостевой JOB — на 7.
     * @param binding Проверенные внешним адаптером реальные выделения и аренды.
     * @param lease Выходная идентичность; валидационный отказ не меняет её,
     * runtime fault после привязки выдаёт lease для последующего joined stop/cancel.
     * @return false до мутации при неверном binding; runtime fault сохраняет аренды.
     */
    bool configureOutputFrames(const OutputFrameBinding& binding, OutputFrameLease& lease) noexcept;
    /**
     * @brief Подтверждает writer первого JOB либо начинает следующий свободный слот.
     * @details Только CPU-владелец после acquire внешнего Free-ack. Не сбрасывает
     * PPU/mapper/seed. Следующий JOB разрешён после ровно одного RAM snapshot.
     * @param ticket Полная идентичность задания с точным начальным CPU-срезом.
     * @return Отказ до изменения гостя или записи в не принадлежащий writer пакет.
     */
    bool beginOutputFrame(const OutputFrameTicket& ticket) noexcept;
    /**
     * @brief Материализует долги, запечатывает кадр и снимает только writer.
     * @details Не ждёт Main и не требует третьего/следующего свободного слота.
     * Timing-владение сохраняется при nullptr writer. Sealed пакет остаётся
     * заимствованным до внешнего Reading/Free release/acquire протокола.
     * @param ticket Идентичность действительного JOB; borrowed не владеет памятью.
     * @param borrowed Выходной неизменяемый пакет; nullptr при отказе.
     * @param cut Реальные CPU/PPU конец и исходный guest baseCut.
     * @return Успех только на настоящей новой FrameReady-границе без fault.
     */
    bool sealOutputFrame(const OutputFrameTicket& ticket, const VideoPacket*& borrowed, OutputFrameCut& cut) noexcept;
    /**
     * @brief Один раз копирует CPU-visible RAM в заранее зарегистрированный слот.
     * @details Только владелец в detached sealed cut. Const-чтение $6000 сохраняет
     * open bus; затем ровно один раз сбрасывает бюджет как старый debugRead witness.
     * Оба packet arena, ROM/Machine/audio/RGB и protected spans исключены.
     * @param ticket Идентичность только что завершённого JOB.
     * @param destination Тот же реальный writable диапазон binding.snapshot[slot].
     * @param bytes Ровно 10240 байт; отказ не пишет ни байта и не меняет бюджет.
     */
    bool copyOutputVisibleRam(const OutputFrameTicket& ticket, std::uint8_t* destination, std::size_t bytes) noexcept;
    /**
     * @brief Закрывает обязательную snapshot-фазу без формирования RAM witness.
     * @param ticket Точный уже sealed кадр текущего исключительного Worker-владельца.
     * @param destination Зарегистрированный snapshot-диапазон ticket.slot; не записывается.
     * @param bytes Ровно 10240; сохраняет прежние admission/alias проверки.
     * @return true подтверждает только metadata/budget/phaseack, НЕ наличие RAM bytes.
     * @details Прежний полный API остаётся эталоном для RAM witness. Нет heap,
     * новых полей, чтения RAM, guest clocks или Main-доступа к primary Machine.
     */
    bool finalizeOutputMetadata(const OutputFrameTicket& ticket, std::uint8_t* destination, std::size_t bytes) noexcept;
    /** @brief Worker копирует sealedView после metadata в свой Payload; не Main-read живой Machine. */
    bool outputVrc7Tape(const OutputFrameTicket& ticket,Vrc7WriteTape::View& view)const noexcept;

    /**
     * @brief Снимает output-аренду после join, освобождения consumer и writer.
     * @details Метод не отменяет Worker и не удостоверяет аппаратные fences.
     * Все 2 packet должны быть Free; при отказе состояние/заимствования сохраняются.
     */
    bool cancelOutputFrames(const OutputFrameLease& lease) noexcept;
    /**
     * @brief Отсоединяет незапечатанный writer после настоящего Worker completion.
     * @details Только исключительный владелец без внешнего читателя Recording.
     * Доводит уже возникшие долги, abandon только Recording, сохраняет Timing owner
     * и sticky fault. Продолжение гостя запрещено до cancel и явного reset.
     * Метод не доказывает join и не освобождает Sealed/Reading другого слота.
     */
    bool stopOutputWriter(const OutputFrameLease& lease) noexcept;
    /** @brief Сообщает явное Timing output-владение даже между writer JOB. */
    bool outputFramesEnabled() const noexcept { return outputOwner_; }
    /** @brief Проверяет действительный writer до первого гостевого изменения. */
    bool outputFrameExecutable() const noexcept;
    /**
     * @brief Возвращает текущую ошибку соответствующего тракта.
     * @details Первичная ошибка сохраняется до установленного reset/begin; отсутствие ошибки не означает завершения физического вывода.
     * @return Вычисленное значение или состояние типа VideoFault согласно описанному контракту.
     */
    VideoFault videoFault() const noexcept;
    // Хостовый прототип: явный владелец TimingPpu без цветового выхода.
    // Настройка допустима до загрузки либо на семитактовом срезе сброса.
    /**
     * @brief Выбирает временной PPU как явного владельца CPU-наблюдаемых эффектов.
     * @details Сам по себе этот режим не создаёт изображение. Поддержка mapper и сбросный срез проверяются до смены владельца.
     * @param enabled Запрошенное состояние режима на разрешённой остановленной границе.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool configureObservableVideo(bool enabled) noexcept;
    /**
     * @brief Проверяет выбранный режим или наличие нужного хранилища владельца.
     * @details Не меняет режим, не выделяет буферы и не доказывает завершение внешних пользователей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool observableVideoEnabled() const noexcept { return videoObservableOnly_; }
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    /**
     * @brief Проверяет выбранный режим или наличие нужного хранилища владельца.
     * @details Не меняет режим, не выделяет буферы и не доказывает завершение внешних пользователей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool splitVideoEnabled() const noexcept {
        return (videoPolicy_&1U)!=0;
    }

    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    Machine(const Machine&) = delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    Machine& operator=(const Machine&) = delete;

    /**
     * @brief Исполняет гостевую машину до границы кадра либо до ошибки.
     * @details Результат FrameReady относится к гостевому PPU. Он не означает завершения RGB, PCM, PPA или вывода на LCD у приложения.
     * @return Вычисленное значение или состояние типа RunStatus согласно описанному контракту.
     */
    [[nodiscard]] RunStatus stepFrame() noexcept;
    /**
     * @brief Проверяет и подключает заимствованный образ ROM.
     * @details Владелец образа удерживает неизменённые байты на весь срок использования. Отказ загрузки нельзя считать сохранением всех старых указателей картриджа; ранее выданные ссылки и эпохи обрабатываются установленным жизненным циклом.
     * @param image Неизменяемый заимствованный образ ROM; вызывающий сохраняет его срок жизни.
     * @param size Размер переданного диапазона в байтах.
     * @param region Явно выбранный гостевой регион NTSC/PAL.
     * @return Код проверки iNES/поддержки mapper; только Ok означает принятую загрузку, отказ имеет описанные эффекты инвалидирования.
     */
    [[nodiscard]] LoadResult load(const std::uint8_t* image, std::size_t size,
                                  Region region = Region::Ntsc) noexcept;
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     */
    void reset() noexcept;
    // Настройка потоком владельца; намеренно сбрасывает загруженную машину.
    // nullptr возвращает исходный синхронный аудиорежим. Хранилище заимствовано.
    /**
     * @brief Подключает заимствованное хранилище аудиособытий и сбрасывает машину.
     * @details Настраивается потоком-владельцем. nullptr восстанавливает синхронный APU; память пакета остаётся у вызывающего.
     * @param storage Заимствованное хранилище выбранного протокола; nullptr обрабатывается контрактом метода.
     * @param expansionDac В private feature включает полный bounded raw-DAC protocol; false оставляет native-only packet.
     */
    void configureSplitAudio(AudioPacket* storage
                             ,bool expansionDac=false
                             ) noexcept;
    /**
     * @brief Проверяет выбранный режим или наличие нужного хранилища владельца.
     * @details Не меняет режим, не выделяет буферы и не доказывает завершение внешних пользователей.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool splitAudio() const noexcept { return audioPacket_ != nullptr; }
    /** @brief Pure actualFM capability; Main читает только cold/joined owner, не живой Worker. */
    [[nodiscard]] bool hasVrc7Fm()const noexcept{return cartridge_.hasVrc7Fm();}
    /** @brief Pure owner-local flag; после terminal detach остаётся armed до fullreset. */
    [[nodiscard]] bool externalVrc7Enabled()const noexcept{return cartridge_.externalVrc7Enabled();}
    /**
     * @brief Возвращает текущую ошибку соответствующего тракта.
     * @details Первичная ошибка сохраняется до установленного reset/begin; отсутствие ошибки не означает завершения физического вывода.
     * @return Вычисленное значение или состояние типа AudioFault согласно описанному контракту.
     */
    [[nodiscard]] AudioFault audioFault() const noexcept {
        if(cartridge_.externalVrc7Fault()!=Vrc7WriteTape::Fault::None)return AudioFault::InvalidDestination;
        if (!audioPacket_) return AudioFault::None;
        if (audioPacket_->fault() != AudioFault::None) return audioPacket_->fault();
        const auto base = audioPacket_->header().baseCycle, time = cpuCycles();
        return time < base || time - base > 65535 ? AudioFault::Timestamp : AudioFault::None;
    }
    /**
     * @brief Возвращает поколение текущего аудиоинтервала.
     * @details Оно может отличаться от Appgeneration и эпохи цветового транспорта.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t audioGeneration() const noexcept { return audioGeneration_; }
    // Retire только при простое CPU-владельца. Копирует использованные записи, начинает
    // следующий интервал захвата; никогда не вызывает и не меняет AudioRenderer.
    /**
     * @brief Закрывает аудиоинтервал и копирует только занятые записи в отдельный пакет.
     * @details CPU-владелец должен быть остановлен; функция не запускает AudioRenderer и не копирует неиспользуемую ёмкость.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param destination Отдельный незанятый аудиопакет для копии использованных записей; не алиас активного пакета.
     */
    [[nodiscard]] bool retireAudioPacket(AudioPacket& destination) noexcept;
    /**
     * @brief Один раз копирует used audio в существующий destination до Thin commit ACK, не меняя источник/sequence.
     * @param destination Caller-owned отдельный пакет с достаточными limits, удерживаемый до actual join.
     * @param token Стабильная empty/completed self-bound квитанция; fresh second token также отвергается channel owner.
     * @return false до изменения token/ownership/destination при guard/limits отказе; true после единственной used-copy.
     * @pre Worker, completed CPU frame; доверенный App уже получил Producer.commit Published и ещё не ждёт ACK.
     */
    [[nodiscard]] bool stageAudioPacketForThinCommit(AudioPacket& destination, AudioStageToken& token) noexcept;
    /**
     * @brief Закрывает staged retirement без повторной копии после настоящего Thin ACK.
     * @param token Та же live staged квитанция и неизменный destination header.
     * @return true ровно один раз после sequence++/begin/audioRetired; false сохраняет borrow.
     * @pre Worker уже вернулся из stepFrame; прежний thin.audioReady обязателен, DSP завершён на том же владельце.
     */
    [[nodiscard]] bool finalizeAudioPacketForThinCommit(AudioStageToken& token) noexcept;
    /**
     * @brief Отцепляет только staged metadata после доказанного actual Worker join на Main.
     * @param token Live квитанция matching channel; channel/token живы до этого вызова.
     * @return true после abandon без sequence/begin/audioReady/PCM публикации; false ничего не освобождает.
     * @pre Main получил настоящий completion/join и находится вне consumer callback; timeout/fault недостаточны.
     */
    [[nodiscard]] bool abandonAudioPacketForThinCommitJoined(AudioStageToken& token) noexcept;

    /**
     * @brief Обновляет текущую битовую маску кнопок порта контроллера.
     * @details Последовательное считывание и защёлка строба остаются у шинного протокола; приложение задаёт маску на согласованной границе ввода.
     * @param port Индекс последовательного контроллерного порта.
     * @param buttons Битовая маска кнопок перед последующим протокольным считыванием.
     */
    void setController(unsigned port, std::uint8_t buttons) noexcept;
    /**
     * @brief Предоставляет заимствованный индексный буфер текущего видеотракта.
     * @details У Machine сначала материализуются CPU/PPU-долги; observable-владелец возвращает nullptr. Указатель нельзя использовать параллельно с изменением владельца или после освобождения его хранилища.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    const std::uint8_t* framebuffer() const noexcept {
        flushCpuReads();
        flushPpu();
        if(videoObservableOnly_)return nullptr; // Не создаёт фиктивное legacy-изображение или пустой кадр.
        if(outputOwner_)return nullptr; // Output-only цвет принадлежит отдельному compositor.
        if (videoPacket_) return observeVideo() ? videoObserver_->indexedVideo() : nullptr;
        return ppu_.framebuffer();
    }
    /**
     * @brief Возвращает метаданные усиления цвета строки.
     * @details Для Machine доступ может материализовать долги; в observable-режиме без цветового спутника данные изображения недоступны.
     * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t emphasis(unsigned row) const noexcept {
        flushCpuReads();
        flushPpu();
        if(videoObservableOnly_)return 0; // Цвет недоступен без парного повторного исполнителя.
        if(outputOwner_)return 0;
        if (videoPacket_) return observeVideo() ? videoObserver_->videoEmphasis(row) : 0;
        return ppu_.scanlineEmphasis(row);
    }
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    const std::int16_t* samples() const noexcept { observeCpuReads(); 
        if (audioPacket_) return nullptr; // Явный режим использует PCM, принадлежащий рендереру.
        return apu_.samples(); }
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    std::size_t sampleCount() const noexcept { observeCpuReads(); 
        if (audioPacket_) return 0;
        return apu_.sampleCount(); }
    /**
     * @brief Возвращает счётчик гостевых кадров выбранного PPU.
     * @details Не является числом физических подач на LCD или игровым FPS.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t frames() const noexcept {
        if (splitVideoEnabled()) return timingPpu_.frameNumber();
        return ppu_.frameNumber(); }
    /**
     * @brief Возвращает полное уже учтённое гостевое время CPU.
     * @details При read-quanta включает накопленные CPU-доступы без принудительного сброса долга.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t cpuCycles() const noexcept {
        return cycles_ + pendingCpuReads_;
    }
    /**
     * @brief Возвращает выбранный регион гостевой машины.
     * @details Это текущая конфигурация, а не доказательство частоты физической развёртки дисплея.
     * @return Вычисленное значение или состояние типа Region согласно описанному контракту.
     */
    Region region() const noexcept { return region_; }
    /**
     * @brief Связывает внешнее состояние MMC5 до load на остановленном владельце.
     * @param digital Заимствованная цифровая модель; nullptr отключает привязку.
     * @param audio Заимствованная звуковая модель, живущая до join/detach.
     * @details Активная Output-аренда запрещает замену. Сначала закрываются долги
     * старого mapper, затем отбрасывается prepared policy, в том числе при ABA
     * повторном использовании адреса объекта. Память моделей не освобождается.
     */
    void configureMmc5(Mmc5* digital,Mmc5Audio* audio) noexcept {
        if(outputOwner_)return;
        flushCpuReads();cpuReadBudget_=0;
        flushPpu();
        timingPpu_.invalidateMmc5RasterPlan();
        cartridge_.bindMmc5State(digital,audio); }
    /**
     * @brief Материализует отложенные PPU-эффекты и предоставляет картридж.
     * @details Публичный доступ является барьером наблюдения CPU/PPU; вызывать его из чужого потока во время рабочего задания нельзя.
     * @return Заимствованная ссылка на картридж текущей машины.
     */
    const Cartridge& cartridge() const noexcept {
        flushCpuReads();
        flushPpu();
        return cartridge_;
    }
    /**
     * @brief Доводит отложенную CPU-историю до границы наблюдения и возвращает состояние CPU.
     * @details Ссылка не является потокобезопасным снимком; Main читает её только после завершения задания владельца.
     * @return Ссылка на фактические регистры, флаги и счётчик CPU.
     */
    const Cpu6502::State& cpuState() const noexcept { return cpu_.state(); }
    /**
     * @brief Выполняет диагностическое чтение с реальной семантикой адресного пространства.
     * @details Может подтвердить IRQ, сдвинуть контроллер или изменить PPU. Не подходит для безэффектного снимка памяти.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t debugRead(std::uint16_t address) noexcept {
        // Detached MMIO не должно менять v/status/контроллер вне capture writer.
        if(outputOwner_&&!outputFrameExecutable())return openBus_;
        observeCpuReads();
        cpuReadBudget_ = 0; // Отладочный I/O может подтвердить IRQ или выставить адрес PPU.
        return memoryRead(address);
    }
    // Исключительный CPU-владелец на спокойном срезе аренды времени жизни; сброс допустим.
    // Вызывающий связывает RAM с настоящим запечатанным FrameCut; API
    // самостоятельно не удостоверяет FrameReady или конкретный номер кадра.
    // Вызывающий владеет настоящим изменяемым выделением результата. Здесь нет тактов, flush,
    // эффектов маппера, исполнения CPU/APU/PPU или реестра аллокаций.
    /**
     * @brief Копирует видимую CPU RAM на согласованном остановленном срезе.
     * @details Проверяет ёмкость и текущую аренду. Использует тот же выбор $6000-источника без повторного MMIO и не создаёт самостоятельно кадровый свидетель.
     * @param destination Приёмный диапазон вызывающего, не перекрывающий действующие заимствованные источники.
     * @param capacity Ёмкость целевого массива в единицах его элементов.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param lease Аренда точного producer/ROM/региона с ненулевыми epoch/generation; байты и идентичности сохраняются до отвязки.
     */
    bool copyCpuVisibleRam(std::uint8_t* destination, std::size_t capacity,
                           const ColourLease& lease) noexcept {
        constexpr std::size_t bytes=2048U+8192U;
        /**
         * @brief Проверяет ненулевой адрес/размер и отсутствие переполнения uintptr_t; не удостоверяет права памяти или реальное присоединение worker.
         * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
         */
        const auto validSpan=[](const void* pointer,std::size_t size)noexcept {
            return pointer&&size&&size<=~std::uintptr_t{0}-reinterpret_cast<std::uintptr_t>(pointer);
        };
        /**
         * @brief Проверяет пересечение заимствованных диапазонов через разности адресов без переполнения; память не читается и владение не передаётся.
         * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
         */
        const auto overlaps=[](const void* first,std::size_t firstBytes,
                               const void* second,std::size_t secondBytes)noexcept {
            if(!second||!secondBytes)return false;
            const auto a=reinterpret_cast<std::uintptr_t>(first);
            const auto b=reinterpret_cast<std::uintptr_t>(second);
            return a<b?b-a<firstBytes:a-b<secondBytes;
        };
        if(capacity!=bytes||!validSpan(destination,bytes)||!validSpan(lease.rom,lease.romBytes)||
           lease.producer!=this||!lease.generation||!lease.epoch||lease.epoch!=colourEpoch_||lease.region!=region_||
           (lease.region!=Region::Ntsc&&lease.region!=Region::Pal)||
           cpu_.inStep_||pendingCpuReads_||pendingPpuDots_||colourCapture_||
           !videoObservableOnly_||videoPacket_||!splitVideoEnabled()||videoHasFault()||!cartridge_.loaded())return false;
        if(thin_)return false;
        const auto image=reinterpret_cast<std::uintptr_t>(lease.rom);
        const auto prg=reinterpret_cast<std::uintptr_t>(cartridge_.prg_);
        if(prg<image||prg-image>lease.romBytes||cartridge_.prgSize_>lease.romBytes-(prg-image)||
           cartridge_.chrSize_>lease.romBytes-(prg-image)-cartridge_.prgSize_||
           overlaps(destination,bytes,this,sizeof(*this))||overlaps(destination,bytes,&lease,sizeof(lease))||
           overlaps(destination,bytes,lease.rom,lease.romBytes)||
           overlaps(destination,bytes,ppu_.pixels_.data(),Ppu::width*Ppu::height))return false;
        if(overlaps(destination,bytes,audioPacket_,audioPacket_?sizeof(*audioPacket_):0U))return false;
        // Неизменяемые резидентные байты — известный активный заимствованный источник чтения.
        // Результат снимка не должен повредить ни одну часть заимствованной арены.
        if(overlaps(destination,bytes,cartridge_.cpuPrgTail_.resident,
                    cartridge_.cpuPrgTail_.resident?cpuPrgTailBytes:0U))return false;
        // Эти адреса имеют чистое const-чтение Cartridge: сохранить PRG по
        // $6000 и отключённую MMC1/MMC3 RAM/открытую шину без копирования отображения.
        const auto seed=openBus_;
        std::memcpy(destination,ram_.data(),ram_.size());
        cartridge_.copyCpu6000Window(destination+2048U,seed);
        // Один раз воспроизвести окончательный учёт оптимизации старого цикла debugRead.
        // Гостевые поля, отложенный долг, IRQ, счётчики и защёлки не меняются.
        cpuReadBudget_=0;
        refreshPpuDeferBudget();
        return true;
    }
    /**
     * @brief Исполняет одну инструкцию с теми же шинными побочными эффектами, что и кадровый путь.
     * @details Гостевое время машины может включать задержки DMA сверх собственных тактов инструкции.
     */
    void stepInstruction() noexcept;

    // Необязательный внедряемый счётчик циклов для профиля платы. Без SDK-зависимости.

    /**
     * @brief Фактически выбранные MMC5-пути одного owner-кадра, без host-clock/CSR.
     * @details Поля безусловны: usage option не меняет layout между coherent lanes.
     * Насыщение UINT32_MAX означает непригодность кадра для точного суммирования.
     */
    struct Mmc5Usage {
        std::uint32_t h2AdmittedM2=0; ///< Реальные M2, завершившие eligible read path.
        std::uint32_t tile8Groups=0; ///< Полностью исполненные группы восьми PPU dots.
        std::uint32_t carryContinuations=0; ///< Следующие инструкции, начатые с pending debt.
    };
    /**
     * @brief Возвращает owner-local счётчики без materialization/reset и callback.
     * @return Ordinary copy; единицы полей различны и не являются временем.
     * @details Читает Worker после возврата stepFrame либо настоящий joined owner.
     * Main во время Worker использует только опубликованный SlotN/accepted copy.
     */
    [[nodiscard]] Mmc5Usage mmc5Usage() const noexcept {return mmc5Usage_;}

private:
    /** @brief Pure completed-frame guard; не flush-ит и не продвигает часы. beforeAck выбирает busy либо прежний audioReady. */
    [[nodiscard]] bool audioStageBoundary(bool beforeAck) const noexcept;
    // Независимый native fixture проверяет границы временного ядра без SDK;
    // friendship не добавляет полей, runtime hooks или нового владельца Machine.
    friend struct MachineEventKernelTestAccess;
    /**
     * @brief Сворачивает доказанный цикл обычной памяти без потери гостевых тактов.
     * @details Проверяет код, адреса, флаги и исходный горизонт. Counted-цикл способен изменить итоговый PC; вызывающий код не должен кэшировать прежний PC через этот метод.
     * @param bodyPc PC тела уже проверяемого обычного цикла.
     * @param branchPc PC завершающей ветви проверяемого цикла.
     * @param bodyCycles Реальное число тактов тела цикла.
     * @param branchCycles Реальное число тактов последней ветви, включая page crossing.
     * @param deadline Абсолютный предел гостевых CPU-тактов исходного quiet-интервала.
     */
    void collapseIdleLoop(std::uint16_t bodyPc, std::uint16_t branchPc, unsigned bodyCycles, unsigned branchCycles, std::uint64_t deadline) noexcept;
    /**
     * @brief Сворачивает только доказанный опрос STATUS с сохранением результата и шинного времени.
     * @details Проверка стабильности статуса и идемпотентности не разрешает чтения за будущей границей. Недопустимый цикл остаётся исходному интерпретатору.
     * @param branchPc PC завершающей ветви проверяемого цикла.
     * @param branchCycles Реальное число тактов последней ветви, включая page crossing.
     * @param deadline Абсолютный предел гостевых CPU-тактов исходного quiet-интервала.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned collapseStatusPollLoop(std::uint16_t branchPc, unsigned branchCycles, std::uint64_t deadline) noexcept;

    /**
     * @brief Проверяет аппаратный контракт и исключительное Timing-владение для нового STATUS loop fold.
     * @details Не продвигает clocks и не читает гостевую память; foreign observers остаются на fallback.
     */
    bool pureStatusPollContract() const noexcept;
    // Прямая шина даёт моделям микросхем известный конечный тип callback,
    // обобщённая сборка сохраняет независимо тестируемые интерфейсы шин.
    friend class Cpu6502;
    /**
     * @brief Выполняет внутренний цикл кадра с границами CPU, PPU, APU и DMA.
     * @details Объединённые интервалы допускаются только в пределах уже доказанного тихого горизонта; MMIO, ошибка и готовность кадра остаются обязательными границами.
     * @return Вычисленное значение или состояние типа RunStatus согласно описанному контракту.
     */
    RunStatus stepFrameImpl() noexcept;
    /**
     * @brief Формирует текущий материализованный CPU/PPU-срез тонкого канала.
     * @details Не включает неисполненный будущий бюджет.
     * @return Вычисленное значение или состояние типа ColourCut согласно описанному контракту.
     */
    ColourCut thinCut()const noexcept;
    /**
     * @brief Терминально отменяет текущую привязку/сеанс после нарушения контракта.
     * @details До нового согласованного reset запрещает дальнейшую публикацию; память удерживается до фактической остановки пользователей.
     */
    void invalidateThinPpu()noexcept;
    /**
     * @brief Сохраняет уже выполненное событие в ограниченном буфере тонкого канала.
     * @details Ёмкость и порядок проверяются; переполнение не превращается в молчаливую потерю события.
     */
    void recordThinEvent(ColourKind,std::uint64_t,std::uint16_t,std::uint8_t)noexcept;
    /**
     * @brief Делегирует реальное CPU-чтение единственному полному PPU и сохраняет локальные эффекты сигналов.
     * @details Сначала закрывается текущий префикс; ответ соответствует точному post-clock срезу и не угадывается.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t readThinPpu(std::uint16_t)noexcept;
    /**
     * @brief Продвигает только уже доказанные стабильные шинные сигналы PPU.
     * @details Не заменяет данные, которые наблюдает CPU, и сохраняет все необходимые clock/address переходы.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param high Уровень A12 либо выбор таблицы паттернов в данной композиции.
     */
    void advanceThinBackgroundSignals(unsigned tiles,bool high)noexcept{cartridge_.advanceMmc3BgSpan(tiles,high);}
    /**
     * @brief Записывает уже выполненное обращение в активный цветовой поток.
     * @details Сохраняет реальное значение и порядок одинаковых timestamps; не делает дополнительного чтения устройства.
     */
    void recordColourEvent(ColourKind,std::uint64_t,std::uint16_t,std::uint8_t) noexcept;
    /**
     * @brief Прекращает захват и инвалидирует текущую эпоху цветового транспорта.
     * @details Завершает связанные заимствования. Вызывающий удерживает ROM и память слотов до фактического освобождения обоих владельцев.
     */
    void invalidateColourCapture() noexcept;
    friend class PpuColourReplay;
    friend class Ppu;
    friend class PpuRgbOutput;
    friend class TimingPpu;
    /**
     * @brief Результат одной настоящей фоновой выборки: упакованные данные и физическая low-ссылка для текущего consumer-протокола.
     * @ingroup nes_core
     */
    struct VideoTileFetch { std::uint32_t packed, lowReference; };
    using VideoTileReader = VideoTileFetch (Machine::*)(std::uint16_t,unsigned,const TimingPpu*) noexcept;
    /**
     * @brief Выбирает конкретный обработчик тайла для текущих проводов и mapper.
     * @details Выбор делается для ограниченного интервала; неподдерживаемая комбинация возвращает прежний общий путь.
     * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
     * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @return Вычисленное значение или состояние типа VideoTileReader согласно описанному контракту.
     */
    VideoTileReader videoTileReader(bool clocked,bool addressed,unsigned tiles,unsigned control) noexcept;
    enum class VideoSpriteMode : std::uint8_t { None, Plain, Legacy, Mmc3 };
    /**
     * @brief Выбирает точный режим шинной композиции fetch-слота спрайта.
     * @details Clocked/addressed, таблица паттернов и mapper проверяются до замены обычной последовательности.
     * @param pattern Адрес паттерна текущей плоскости в момент fetch.
     * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
     * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
     * @return Вычисленное значение или состояние типа VideoSpriteMode согласно описанному контракту.
     */
    VideoSpriteMode videoSpriteMode(std::uint16_t pattern,bool clocked,bool addressed) const noexcept;
    /**
     * @brief Получает либо связывает две плоскости спрайта с исходным порядком стробов.
     * @details Старую физическую идентичность фиксируют до высокого Legacy-чтения; оптимизация не удаляет A12 и побочные эффекты.
     * @param pattern Адрес паттерна текущей плоскости в момент fetch.
     * @param mode Выбор вычисления sprite-данных для packet/capture; временные и шинные эффекты сохраняются.
     * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
     * @return Упакованные настоящие low/high-значения sprite-пары и её проверенная физическая ссылка.
     */
    VideoTileFetch videoSpritePlanes(std::uint16_t pattern,VideoSpriteMode mode,const TimingPpu* caller=nullptr) noexcept;
    /**
     * @brief Три уцелевшие low-идентичности и точные v/address/tile/attribute после полного фонового интервала; внутренние тайлы не становятся наблюдаемыми.
     * @ingroup nes_core
     */
    struct VideoBgTail {
        std::array<std::uint32_t,3> low{};
        std::uint16_t v=0,address=0;
        std::uint8_t tile=0,attribute=0;
    };
    /**
     * @brief Проверяет, допустима ли исходная композиция фонового интервала.
     * @details Учитывает mapper, часы, адресные сигналы, длину, таблицу паттернов и опасные маркеры; отказ не изменяет состояние.
     * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
     * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool canVideoBgSpan(bool clocked,bool addressed,unsigned tiles=0,unsigned control=0,std::uint16_t v=0) const noexcept;
    /**
     * @brief Применяет доказанную композицию тактов и A12 фонового MMC3-span.
     * @details Данные RGB не формируются, но счётчик, reload, pending и возраст A12 остаются теми же.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     */
    void completeVideoBgSpan(unsigned tiles,unsigned control) noexcept;
    /**
     * @brief Готовит метаданные банков только если они нужны потребителю видеопакета.
     * @details Observable-владелец не требует записи mapping-view; остальные пути сохраняют проверки физических CHR-ссылок и ёмкости.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool prepareVideoBgView() noexcept;
    /**
     * @brief Сохраняет живые идентичности последних фоновых тайлов интервала.
     * @details При трёх и более тайлах внутренние байты уже вытеснены; последние три ссылки, tile/attribute, v и конечный адрес сохраняются точно.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
     * @return Точные три уцелевшие low-идентичности и финальные v/address/tile/attribute ограниченного span.
     */
    VideoBgTail captureVideoBgTail(std::uint16_t v,unsigned tiles,unsigned control,const TimingPpu* caller=nullptr) const noexcept;
    /**
     * @brief Завершает разрешённый низкий Legacy-интервал с его конечным адресом.
     * @details Отказ не должен менять mapper; последующие FD/FE-запросы и CPU-наблюдаемые эффекты сохраняют прежний путь.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param lastHighAddress Реально удержанный конечный high-адрес интервала.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool completeLegacyVideoBgSpan(unsigned tiles,std::uint16_t lastHighAddress) noexcept;
    /**
     * @brief Получает один тайл с точно выбранными тактами, адресами и стробами.
     * @details Физические байты читаются до соответствующего Legacy-строба; высокое FD/FE-чтение может изменить только последующие страницы.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
     * @return Результат четырёх настоящих фоновых чтений с нужными данными и физической low-ссылкой.
     */
    template<bool Clocked,bool Addressed,bool ReadEffects,bool ComposeMmc3=false>
    VideoTileFetch directVideoBackgroundTile(std::uint16_t v,unsigned control,const TimingPpu* caller) noexcept;
    // Доверенная идентичность кэш-страницы: вызывающий проверил loaded() и получил
    // указатель текущей CHR-страницы этого картриджа. Без общего обхода диапазонов.
    /**
     * @brief Кодирует идентичность уже проверенного байта текущей CHR-страницы.
     * @details Не выполняет общий обход диапазонов; вызывающий обязан доказать loaded и происхождение указателя из этой страницы.
     * @param byte Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Кодированная физическая ROM/CHR-RAM-ссылка либо предусмотренный замороженный литерал; не указатель.
     */
    std::uint32_t cachedVideoReference(const std::uint8_t* byte) const noexcept {
        if(cartridge_.chr_)return static_cast<std::uint32_t>(byte-cartridge_.chr_);
        return VideoByteRef::chrRam|static_cast<std::uint32_t>(byte-cartridge_.chrRam_.data());
    }
    /**
     * @brief Выполняет реальный строб чтения PPU без повторного чтения ненужного значения.
     * @details Не устраняет FD/FE-защёлки и не заменяет обязательное выставление сохранённого адреса.
     */
    void videoReadStrobeOnly() noexcept { cartridge_.ppuReadStrobe(); }
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    /**
     * @brief Проверяет право основного владельца не формировать ненаблюдаемые видеоданные.
     * @details Не расширяет поддерживаемые mapper и не относится к копиям, чужим PPU, временным захватам либо активному потребителю пакета.
     * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool elideVideoCaptureData(const TimingPpu* caller=nullptr) const noexcept {
        // Идентичность вызывающего и временные приёмники не являются lifecycle-константами.
        return caller==&timingPpu_&&(videoPolicy_&2U)&&!timingPpu_.packet_&&!timingPpu_.capture_;
    }
    /**
     * @brief Пересчитывает неизменяемую для текущего режима часть видеополитики.
     * @details Идентичность вызывающего PPU, временный packet/capture и активное владение всё равно проверяются в месте использования.
     */
    void refreshVideoPolicy() noexcept;
    /**
     * @brief Сбрасывает выбранный видеотракт и обновляет допустимость mapper.
     * @details Отказ загрузки не должен неявно переносить старые ссылки на новый backing; политика владельца меняется только в разрешённом жизненном цикле.
     */
    void videoReset() noexcept;
    /**
     * @brief Доводит активный видеопакет и передаёт его наблюдателю.
     * @details При отказе наблюдателя защёлкивает ошибку; вызывающий не получает частичную картинку как успешный результат.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool observeVideo() const noexcept;
    /**
     * @brief Проверяет наличие защёлкнутой ошибки выбранного видеотракта.
     * @details Предикат не выдаёт RGB и не снимает ошибку.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool videoHasFault() const noexcept {
        if(outputOwner_)return videoFaultLatch_!=VideoFault::None||
            (videoPacket_&&videoPacket_->header().fault!=VideoFault::None);
        if(thinStickyFault_||(thin_&&thin_->fault()!=ColourFault::None))return true;
        if(videoObservableOnly_)return videoFaultLatch_!=VideoFault::None;
        return videoFaultLatch_!=VideoFault::None || (videoPacket_?
            videoPacket_->header().fault!=VideoFault::None:!ppu_.pixelsReady());
    }
    /**
     * @brief Определяет физическую либо буквальную ссылку для текущего адреса PPU.
     * @details При недопустимой идентичности защёлкивает ошибку; адрес и память не следует смешивать с текущим банком после mapper-строба.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
     */
    std::uint32_t videoReference(std::uint16_t address) const noexcept;
    /**
     * @brief Сохраняет ссылку либо немедленно замораживает нетипичное чтение паттерна.
     * @details Даже отложенное значение сохраняет реальный read-strobe. Сохранённый NT/палитровый адрес при смене rendering нельзя ошибочно считать CHR.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
     * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
     */
    std::uint32_t captureVideoPattern(std::uint16_t address,std::uint32_t reference) noexcept;
    /**
     * @brief Материализует ранее сохранённый байт паттерна по его виду и смещению.
     * @details Работает с буквальным значением, CHR-ROM либо защищённой CHR-RAM-идентичностью; недопустимая ссылка является ошибкой.
     * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t resolveVideoPattern(std::uint32_t reference) const noexcept;
    /**
     * @brief Замораживает живые ссылки перед изменением CHR-RAM.
     * @details Старый байт сохраняется до записи, чтобы отложенный конвейер не увидел будущее значение.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void freezeVideoChrWrite(std::uint16_t address,std::uint8_t value) noexcept;
    /**
     * @brief Продвигает выбранный PPU на заданное число уже прошедших точек.
     * @details Выбор полного или временного владельца не меняет гостевые clocks. Перенос сигналов CPU выполняется в предусмотренном вызывающим кодом месте.
     * @param dots Число уже прошедших точек PPU.
     */
    void videoTickDots(unsigned dots) const noexcept {
        if (splitVideoEnabled()) timingPpu_.tickDots(dots); else ppu_.tickDots(dots);
    }
    /**
     * @brief Забирает подготовленное событие NMI, предназначенное CPU.
     * @details Возврат события отличается от простого просмотра уровня линии и должен соблюсти исходный порядок синхронизации.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool videoTakeNmi() noexcept { return splitVideoEnabled() ? timingPpu_.takeNmi() : ppu_.takeNmi(); }
    /**
     * @brief Проверяет готовность гостевой кадровой границы.
     * @details Не означает, что потребитель уже закончил цветовой повтор или физический вывод.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool videoFrameReady() const noexcept { return splitVideoEnabled() ? timingPpu_.frameReady() : ppu_.frameReady(); }
    /**
     * @brief Снимает признак уже обработанной гостевой кадровой границы.
     * @details Не сбрасывает номер кадра и не создаёт новый физический кадр.
     */
    void videoClearFrameReady() noexcept { if (splitVideoEnabled()) timingPpu_.clearFrameReady(); else ppu_.clearFrameReady(); }
    /**
     * @brief Выполняет чтение выбранного видеофронтенда.
     * @details Этот внутренний диспетчер выбирает единственный активный PPU; внешний MMIO-путь отвечает за предшествующий гостевой такт и историю чтений.
     * @param reg Адрес регистра PPU, включая зеркала исходного MMIO-доступа.
     * @return Байт, возвращённый выбранным PPU с его реальными побочными эффектами.
     */
    std::uint8_t videoRead(std::uint16_t reg) noexcept {
        const auto value=
            thin_ ? readThinPpu(reg) :
            splitVideoEnabled() ? timingPpu_.cpuRead(reg) : ppu_.cpuRead(reg);
        recordColourEvent(ColourKind::Read,cycles_,reg,value);
        return value;
    }
    /**
     * @brief Маршрутизирует CPU-доступ к регистру выбранному владельцу PPU.
     * @details Сохраняет побочные эффекты STATUS/PPUDATA/OAMDATA и исходные границы синхронизации.
     * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void videoWrite(std::uint16_t reg,std::uint8_t value) noexcept {
        if(splitVideoEnabled()) timingPpu_.cpuWrite(reg,value); else ppu_.cpuWrite(reg,value);
        recordColourEvent(ColourKind::Write,cycles_,reg,value);
        if(thin_)recordThinEvent(ColourKind::Write,cycles_,reg,value);
    }
    /**
     * @brief Записывает очередной байт OAM DMA в текущий OAM-адрес.
     * @details Продвигает адрес и инвалидирует связанные sprite-кэши в исходном порядке; не заменяет источник DMA.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void videoDmaWrite(std::uint8_t value) noexcept {
        if(splitVideoEnabled()) timingPpu_.dmaWrite(value); else ppu_.dmaWrite(value);
        recordColourEvent(ColourKind::Dma,cycles_,0x2004,value);
        if(thin_)recordThinEvent(ColourKind::Dma,cycles_,0x2004,value);
    }
    /**
     * @brief Продвигает только уже доказанные стабильные шинные сигналы PPU.
     * @details Не заменяет данные, которые наблюдает CPU, и сохраняет все необходимые clock/address переходы.
     * @param dots Число уже прошедших точек PPU.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
     * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
     */
    void advanceStablePpuSignals(unsigned dots, std::uint16_t address, bool clocked, bool addressed) noexcept {
        cartridge_.advanceStablePpuSignals(dots, address, clocked, addressed);
    }
    friend class Apu;
    /**
     * @brief Call-local представление внутренней CPU RAM и физических PRG-страниц. Допуск quiet-адресов и срок жизни проверяются до использования.
     * @ingroup nes_core
     */
    struct QuietMemory {std::uint8_t* ram; const std::uint8_t* const* prg;};
    /**
     * @brief Выдаёт прямые страницы RAM/PRG для уже защищённого quiet-интервала.
     * @details Описание не делает MMIO обычной памятью; срок действия ограничен текущим владением и неизменной банковой раскладкой.
     * @return Вычисленное значение или состояние типа QuietMemory согласно описанному контракту.
     */
    QuietMemory quietMemory() noexcept { return {ram_.data(), cartridge_.prgPages_.data()}; }
#if defined(__GNUC__) || defined(__clang__)
    /**
     * @brief Возвращает текущие страницы прямого PRG-чтения, включая допустимый резидентный хвост.
     * @details Исходная физическая ROM-идентичность хранится отдельно; указатель нельзя удерживать через изменение mapper/эпохи.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[gnu::always_inline]]
#endif
    const std::uint8_t* const* quietCpuReadPages() const noexcept { return cartridge_.cpuPrgTail_.pages.data(); }

#if defined(__GNUC__) || defined(__clang__)
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[gnu::always_inline]]
#endif
    inline std::uint8_t read(std::uint16_t address) noexcept override {
        const bool ordinary = address < 0x2000 || address >= 0x8000;
        if (cpuReadBudget_ && !dmaPending_ && !dmcStalls_) {
            if(mmc5ReadQuantum_.active){
                // Vector strobe способен сбросить MMC5 frame и всегда идёт через flush.
                if(address>=0x2000U&&(address<0x6000U||(address&0xFFFEU)==0xFFFAU))return readSlow(address);
                ++pendingCpuReads_;--cpuReadBudget_;
                std::uint8_t value;
                if(address<0x2000U)value=ram_[address&0x7FFU];
                else if(address<0x8000U){
                    const auto* page=mmc5ReadQuantum_.ram6000;
                    value=page?page[address&8191U]:openBus_;
                }else value=cartridge_.cpuRead(address,openBus_);
                openBus_=value;
                return value;
            }
            if(ordinary){
                ++pendingCpuReads_; --cpuReadBudget_;
                const auto value = address < 0x2000 ? ram_[address & 0x7ff] : cartridge_.cpuRead(address, openBus_);
                openBus_ = value;
                return value;
            }
        }
        return readSlow(address);
    }
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t readSlow(std::uint16_t address) noexcept;
    /**
     * @brief Записывает байт через протокол устройства или шины.
     * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void write(std::uint16_t address, std::uint8_t value) noexcept override;
    /**
     * @brief Читает байт адресного пространства PPU с нужным выбором памяти.
     * @details Паттерны CHR, таблицы имён и палитра имеют разные правила зеркалирования и mapper-эффектов; чтение не заменяет отдельный адресный строб.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t ppuRead(std::uint16_t address) noexcept override;
    /** @brief Выполняет классифицированный fetch PPU; tileX сохраняет prefetch0/1 и будущие tiles2..33. */
    std::uint8_t ppuReadFetch(std::uint16_t address,PpuFetchKind kind,unsigned tileX) noexcept override {
        return cartridge_.mapper()==5?mmc5PpuFetch(address,kind,tileX):ppuRead(address);
    }
    /** @brief Выполняет только MMC5 fetch через отдельный TU; вызывающий уже проверил mapper5. */
    std::uint8_t mmc5PpuFetch(std::uint16_t address,PpuFetchKind kind,unsigned tileX) noexcept;

    // Метаданные только текущего вызова. Ppu использует их внутри уже прошедшего
    // видимого интервала до CPU/cart/MMIO-события, способного менять страницы.
    /**
     * @brief Call-local страницы имён, атрибутов и CHR полного PPU. Действительно только внутри доказанного интервала без CPU/MMIO/bank-смены.
     * @ingroup nes_core
     */
    struct FullPpuSpanView {
        const std::uint8_t* names[2];
        const std::uint8_t* attributes[2];
        const std::uint8_t* const* chr;
    };
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    /**
     * @brief Создаёт заимствованное описание физических страниц для одного полного фонового прохода.
     * @details Не читает тайлы заранее и не переживает CPU/MMIO/mapper-событие. Mapper с побочными эффектами чтения CHR сохраняет прежний путь.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param view Выходное call-local представление NT/attribute/CHR-страниц; использовать только после true и до следующего mapper/MMIO-события.
     */
    bool fullPpuSpanView(std::uint16_t v,unsigned control,FullPpuSpanView& view) const noexcept {
        // Явный отказ Legacy действует и для принудительно неотслеживаемых акторов.
        // Его FD/FE-чтения могут заменить физические страницы после возврата байта.
        if(!cartridge_.loaded() || cartridge_.mapper()==160 || cartridge_.hasPpuReadSideEffects())return false;
        const unsigned vertical=(v>>10U)&2U;
        for(unsigned x=0;x<2U;++x){
            const auto* page=nametables_.data()+nametablePages_[vertical|x];
            view.names[x]=page+(v&0x03E0U);
            view.attributes[x]=page+0x03C0U+((v>>4U)&0x38U);
        }
        view.chr=cartridge_.chrPages_.data()+((control&0x10U)?4U:0U);
        return true;
    }
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
#endif
    /**
     * @brief Выдаёт локальное отображение только для уже доказанного низкого Legacy-span.
     * @details Повторно использует существующие проверки режима и FD/FE-маркеров; не расширяет допуск элиминации C1.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param view Выходное call-local представление NT/attribute/CHR-страниц; использовать только после true и до следующего mapper/MMIO-события.
     */
    bool fullLegacyLowSpanView(std::uint16_t v,unsigned control,unsigned tiles,FullPpuSpanView& view) const noexcept {
        // Использовать прежнее доказательство физических FD/FE + source1/BG0, а не список игр
        // или CHR-режимов. Все цветовые байты остаются настоящими чтениями.
        if(tiles<3U || tiles>32U || !cartridge_.loaded() || cartridge_.mapper()!=160 ||
           !canVideoBgSpan(false,true,tiles,control,v))return false;
        const unsigned vertical=(v>>10U)&2U;
        for(unsigned x=0;x<2U;++x){
            const auto* page=nametables_.data()+nametablePages_[vertical|x];
            view.names[x]=page+(v&0x03E0U);
            view.attributes[x]=page+0x03C0U+((v>>4U)&0x38U);
        }
        view.chr=cartridge_.chrPages_.data(); // Допущена таблица паттернов 0.
        return true;
    }
    /**
     * @brief Записывает байт в отображённую память PPU.
     * @details Сохраняет зеркалирование, ограничения CHR-ROM и замораживание уже заимствованных CHR-RAM-значений.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void ppuWrite(std::uint16_t address, std::uint8_t value) noexcept override;
    /**
     * @brief Публикует адрес PPU, сохраняя A12 и адресные защёлки mapper.
     * @details Для MMC3 неизменный уровень A12 не требует повторной записи mapper; точки PPU и чтения всё равно исполняет caller. Остальные mapper, включая Legacy160 с FD/FE, получают полный адрес по прежнему пути.
     * @param address Гостевой адрес шины PPU, а не физический указатель ROM.
     */
    void ppuAddress(std::uint16_t address) noexcept override { if(cartridge_.mapper_==4&&(((address&0x1000U)!=0)==cartridge_.a12_))return;cartridge_.ppuAddress(address); }
    /**
     * @brief Продвигает временное состояние картриджа на одну точку PPU.
     * @details Сохраняет возраст A12 и фильтрацию фронтов MMC3, даже когда видеоданные не выводятся.
     */
    void ppuCycle() noexcept override { cartridge_.ppuCycle(); }
    /**
     * @brief Проверяет применимость конкретной шинной композиции к текущему mapper.
     * @details Разрешение зависит от режима и проводов, а не от имени игры.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool usesMmc3TileSignals() const noexcept { return cartridge_.mapper() == 4; }
    /**
     * @brief Проверяет применимость конкретной шинной композиции к текущему mapper.
     * @details Разрешение зависит от режима и проводов, а не от имени игры.
     * @param high Уровень A12 либо выбор таблицы паттернов в данной композиции.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool usesMmc3SpriteSignals(bool high) const noexcept {
        // Полные sprite-слоты имеют траекторию low/A12/стробов чтения фоновых тайлов
        // phase0; отвергнуть новый IRQ-фронт внутри слота.
        return cartridge_.loaded()&&cartridge_.mapper()==4&&cartridge_.ppuIrqQuietFor(8,high);
    }
    /**
     * @brief Применяет доказанную композицию MMC3-сигналов для тайла либо фонового span.
     * @details Возраст A12, счётчик, reload и pending должны совпасть со всеми исходными точками. Это не пропуск IRQ-наблюдения.
     * @param high Уровень A12 либо выбор таблицы паттернов в данной композиции.
     */
    void advanceMmc3TileSignals(bool high) noexcept { cartridge_.advanceMmc3TileSignals(high); }
    /**
     * @brief Проверяет применимость конкретной шинной композиции к текущему mapper.
     * @details Разрешение зависит от режима и проводов, а не от имени игры.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param high Уровень A12 либо выбор таблицы паттернов в данной композиции.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool usesLegacyTileSignals(unsigned tiles, bool high) const noexcept {
        return cartridge_.loaded() && cartridge_.mapper() == 160 &&
            cartridge_.ppuIrqQuietFor(tiles * 8U, high);
    }
    /**
     * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
     * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
     */
    std::uint32_t legacyBackgroundTile(std::uint16_t v, unsigned control) noexcept;
    /**
     * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
     * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @param lowReference Приёмник физической идентичности младшей плоскости до её read-строба.
     * @return Упакованные результаты настоящей NT/attribute/low/high-выборки; lowReference отдельно получает физическую идентичность.
     */
    template<bool CaptureReference>
    inline std::uint32_t legacyBackgroundTileFetch(std::uint16_t v, unsigned control,
                                                  std::uint32_t* lowReference) noexcept;
    /**
     * @brief Получает один Legacy-фоновый тайл через выбранный временной интерфейс.
     * @details Сохраняет фактические байты и порядок адресных/read-стробов.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @param caller Точная идентичность вызывающего PPU; временный/чужой экземпляр не получает допуск основного.
     * @return Значения выбранного тайла и физическая low-ссылка с сохранением post-read FD/FE-защёлки.
     */
    VideoTileFetch videoLegacyBackgroundTile(std::uint16_t v, unsigned control,const TimingPpu* caller) noexcept;
    /**
     * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
     * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
     * @param pattern Адрес паттерна текущей плоскости в момент fetch.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t legacySpritePlanes(std::uint16_t pattern) noexcept {
        return cartridge_.readLegacyBackgroundPlanes(pattern);
    }
    /**
     * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
     * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t dmcRead(std::uint16_t address, std::uint64_t cycle) noexcept override;
    /**
     * @brief Учитывает украденные DMC такты в установленном порядке.
     * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     */
    void dmcStall(unsigned count, std::uint64_t cycle) noexcept override;
    /**
     * @brief Сбрасывает аудиотракт и связанные временные счётчики.
     * @details Настройка режима и lifetime внешнего пакета остаются у Machine/приложения.
     */
    void audioReset() noexcept;
    /** @brief Публикует raw level на границе перед tick либо передаёт прежнему полному APU; provider не clock. */
    void audioPublishExpansion(std::int32_t level)noexcept;
    /**
     * @brief Передаёт запись регистра звуковому владельцу с её гостевым порядком.
     * @details В split-режиме сохраняется и вход renderer, и CPU-наблюдаемый эффект временного APU.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void audioWrite(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Считывает CPU-видимый статус APU с исходной семантикой очистки IRQ.
     * @details Это не peek: реальное чтение $4015 нельзя заменить безэффектным просмотром.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t audioReadStatus() noexcept;
    /**
     * @brief Начинает накопление следующего блока PCM без изменения гостевых регистров каналов.
     * @details В split-режиме соответствующая операция сохраняется для renderer, чтобы границы аудиоблоков совпали.
     */
    void audioClearSamples() noexcept;
    /**
     * @brief Продвигает выбранный APU-тракт на один CPU-такт.
     * @details В раздельном режиме TimingApu сохраняет IRQ/DMC-наблюдаемость, а полный DSP-APU исполняется отдельным потребителем событий.
     */
    void audioTick() noexcept {
 if (audioPacket_) timingApu_.tick(); else apu_.tick(); }
    /**
     * @brief Возвращает электрический IRQ выбранного звукового владельца.
     * @details Обслуживание CPU происходит отдельно с учётом маски и положения последнего такта.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool audioIrqLine() const noexcept { return audioPacket_ ? timingApu_.irqLine() : apu_.irqLine(); }
    /**
     * @brief Возвращает допустимый CPU-интервал до следующего аудиособытия.
     * @details Горизонт ограничивает отложенные clocks; не даёт права терять DMC-выборки или побочные эффекты.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned audioQuietCpuCycles() const noexcept {
        const unsigned native=audioPacket_?timingApu_.quietCpuCycles():apu_.asyncQuietCpuCycles();
        // Новый portable DAC меняется только на границе 64M2. Группа заканчивается
        // ДО неё: следующий scalar M2 публикует уровень с настоящим timestamp.
        // Это отдельный audio horizon; cart IRQ и PPU по-прежнему ограничивают budget.
        const auto mapper=cartridge_.mapper();
        if(mapper>7U&&(mapper==69U||mapper==85U)){
            const auto next=cartridge_.selectedExpansionOutputCpuCycles();
            return std::min(native,next?unsigned(next-1U):0U);
        }
        return native;
    }
    /**
     * @brief Продвигает соответствующий аудиовладелец на заданное число CPU-тактов.
     * @details Объединение не пересекает наблюдаемое IRQ/DMC-событие и не исполняет CPU повторно.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     */
    void audioAdvance(unsigned count) noexcept {
 if (audioPacket_) timingApu_.advanceCpuCycles(count); else apu_.advanceCpuCycles(count); }
    /**
     * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
     * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t dmcRead(std::uint16_t address) noexcept override;
    /**
     * @brief Учитывает украденные DMC такты в установленном порядке.
     * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
     * @param cycles Число прошедших или украденных CPU-тактов; гостевой порядок эффектов задаёт вызывающий.
     */
    void dmcStall(unsigned cycles) noexcept override;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t memoryRead(std::uint16_t address) noexcept;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t memoryReadIo(std::uint16_t address) noexcept;
    /**
     * @brief Вычисляет физический индекс с правилами зеркалирования PPU.
     * @details Учитывает таблицы имён либо специальные зеркала палитры; сам адрес не выдаёт строб чтения.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t nametableAddress(std::uint16_t address) const noexcept;
    /**
     * @brief Обновляет физические страницы таблиц имён по текущему mirroring.
     * @details Сохраняет фиксированную four-screen-проводку и не дублирует отдельные правила mapper.
     */
    void refreshNametableMapping() noexcept;
    /**
     * @brief Точный endpoint pure Signals-плана, без ссылок, OAM/RGB и исполнения clocks.
     * @details Тип и cache storage безусловны для единой ABI OFF/ON; право reuse
     * задаёт только CPP opt-in и совпадение исходного base/grant на owned frame.
     */
    struct ThinMmc5SignalPlan {
        std::uint16_t v=0,address=0,dot=0,evaluation=0,last=0; ///< Loopy, drive, raster и последний RD.
        std::int16_t row=0; ///< Scanline endpoint, включая pre-render−1.
        unsigned elapsed=0,idleOffset=0; ///< Завершённые M2 и единственный idle3 offset.
        std::uint8_t nextTile=0,oam=0,repeat=0,idle=0,scanline=0,splitY=0; ///< Fetch и MMC5 detector endpoint.
        bool coarse=false,scroll=false,read=false,inFrame=false,pending=false; ///< Signal/IRQ latches.
        bool extended=false,split=false,odd=false; ///< MMC5 tile flags и нечётность кадра.
    };

    /**
     * @brief Ограниченный контекст обычных MMC5 CPU accesses одного owned frame.
     * @details RAM6000 заимствуется только при выдаче budget. Null означает open bus,
     * а не фиксированный byte. Любая mapper/MMIO запись и внешний возврат закрывают долг;
     * опыт WRAM сохраняет grant только для обычных записей $6000-$7FFF.
     * Поля безусловны в OFF/ON для единой ABI. Не используют старое состояние H2.
     */
    struct Mmc5ReadQuantumState {
        std::uint64_t deadline=0; ///< Исключённая верхняя guest CPU граница frame watchdog.
        const std::uint8_t* ram6000=nullptr; ///< Текущая physical8KiB WRAM page, без копии.
        bool active=false; ///< Только текущий эксклюзивный stepFrame владеет допуском.
        bool frameQualified=false; ///< Только неизменяемые wiring/lease prereqs текущего вызова stepFrame.
        std::uint64_t previewBase=0; ///< Исходный materialized CPU-cycle этого grant.
        ThinMmc5SignalPlan preview{}; ///< Полностью собственный endpoint, не escaped stack pointer.
        std::uint8_t previewClocks=0; ///< 0 — invalid; иначе точный original grant8..240M2 выбранного профиля.
    };
    Mmc5ReadQuantumState mmc5ReadQuantum_{};
    std::uint64_t romMappingEpoch_=1; ///< Каждый load очищает cache до смены identity, включая wrap.
    /** @brief Закрывает generalized CPU debt и неиспользованные credits до любого frame return. */
    class Mmc5ReadQuantumFrameScope final {
    public:
        /** @brief Включает owned mapper5 допуск до deadline; nested execution не разрешается. */
        Mmc5ReadQuantumFrameScope(Machine& machine,std::uint64_t deadline) noexcept;
        /** @brief Материализует весь префикс до очистки borrowed pointer и контекста. */
        ~Mmc5ReadQuantumFrameScope() noexcept;
        /** @brief Копирование не допускает повторного закрытия одного frame debt. */
        Mmc5ReadQuantumFrameScope(const Mmc5ReadQuantumFrameScope&)=delete;
        /** @brief Присваивание не переносит stack lifetime и обязанность flush. */
        Mmc5ReadQuantumFrameScope& operator=(const Mmc5ReadQuantumFrameScope&)=delete;
    private:
        Machine& machine_;
        bool owns_=false;
    };
    /**
     * @brief Общий допуск B3/GQ без CPU/device mutation, с optional owned plan-cache.
     * @param remaining Максимум будущих M2 до текущего deadline.
     * @param insideStep Ожидаемое состояние CPU interpreter, true только для реального bus access.
     * @return Budget8..240 выбранного ThinFused профиля либо 0; прежние профили ограничены 80. Capacity/generation/IRQ/DMC/PPU проверяются до CPU ahead.
     */
    unsigned mmc5QuietCpuBudget(unsigned remaining,bool insideStep) noexcept;
    /**
     * @brief Pure вычисляет точный завершённый Signals-префикс до hard event.
     * @param requested Максимум уже разрешённых M2, не больше 240; legacy grant не больше 80.
     * @param out Невладеющий малый снимок для последующего commit фактического prefix.
     * @return Доказанные M2;0 означает fallback без изменения devices.
     */
    unsigned planThinMmc5Signals(unsigned requested,ThinMmc5SignalPlan&out)const noexcept;
    /**
     * @brief Общая pure материализация: false сохраняет прежний planner, true требует consumed KnownSafe proof.
     * @param requested Уже потреблённый prefix1..240M2; true не выполняет новый admission.
     * @param out Собственный 36 Б endpoint всех 22 полей, без primary mutation/heap/transport.
     * @pre Для true caller удостоверил active/frame/base/bound/elapsed0 и отозвал proof до callbacks.
     */
    template<bool KnownSafe>
    unsigned materializeThinMmc5Signals(unsigned requested,ThinMmc5SignalPlan&out)const noexcept;


    /**
     * @brief Дополняет frameQualified неизменяемой Output source identity один раз на frame.
     * @return false при чужой lease/packet/source; гостевых чтений и clocks нет.
     * @details Банки и IRQ сюда не входят: их прежний admission действует на каждый grant.
     */
    bool mmc5ReadFoldFrameSource() const noexcept;
    /**
     * @brief Проверяет fixed point только из доверенного strict MMC5 postlude.
     * @param pcAx targetPC16/A8/X8 по значению; State никогда не заимствуется.
     * @param baseOpenP savedBase16 в bits16..31, open8 в 8..15, P8 в 0..7.
     * @param branchInfo branchPC16, actual clocks8 в 16..23, jammed в bit24.
     * @param remaining Остаток исходного grant, не новый horizon; минимум 36M2.
     * @param ram,wram,dataPrg Заимствованные current owned read views только этого блока.
     * @return Clocks минимум четырёх целых пар, либо 0 без изменений.
     * @pre Только runQuietBlockWithPolicy<true> после admission, настоящей taken BNE
     * и pendingIrq==false. FrameScope lifetime фиксирует source identity, а прежний
     * grant исключает IRQ/NMI/DMA/DMC/time edges; helper не заменяет эти барьеры.
     */
#if defined(__GNUC__) || defined(__clang__)
    [[gnu::pure]]
#endif
    unsigned mmc5LocalReadFold(std::uint32_t pcAx,std::uint32_t baseOpenP,std::uint32_t branchInfo,
        unsigned remaining,const std::uint8_t* ram,const std::uint8_t* wram,
        const std::uint8_t* const* dataPrg) const noexcept;

    /**
     * @brief Предоставляет WRAM-read capability только уже выданного MMC5 grant.
     * @return Заимствованные 8192 байта либо nullptr для open bus; не разрешает запись.
     * @details Cpu6502 не получает доступ к приватной проводке Cartridge. Указатель
     * устанавливает владелец перед strict block, MMIO/возврат запрещают его reuse.
     */
    const std::uint8_t* quietMmc5WramPage() const noexcept {return mmc5ReadQuantum_.ram6000;}
    /**
     * @brief Квалифицирует физическую immutable ROM-страницу для текущего strict grant.
     * @param pc Настоящий гостевой PC; WRAM/PRG-RAM/MMIO не являются источником.
     * @return Текущая исходная ROM identity, не resident copy; нулевая аренда означает отказ.
     * @details Mapping не изменяется внутри strict block; смена окна требует нового вызова.
     * Физическая страница входит в ключ, поэтому отдельный mapper epoch не угадывается.
     */
    RomWindow quietRomWindow(std::uint16_t pc) const noexcept;
    /**
     * @brief Проверяет только неизменяемую конфигурацию одного эксклюзивного Output frame.
     * @details Вызывается после начального flush/clear. CPU/MMIO не изменяют ROM/region,
     * заимствованные владельцы пакетов и идентификаторы audio header; configure/retire/seal
     * разрешены лишь после возврата frame. IRQ/fault/capacity, packet state, clocks и mapping
     * сюда не входят и всегда проверяются каждым admission. Никаких гостевых эффектов.
     * @return true при пригодном wiring; false не выдаёт будущие CPU credits.
     */
    bool mmc5QuietFramePrerequisites() const noexcept;
    /**
     * @brief Воспроизводит уже завершённые чистые M2 общим audio-ahead/PPU24/FSM алгоритмом.
     * @param clocks Фактическое число 1..240, не выданный заранее horizon.
     * @details Только действительный сертификат, pending/budget уже обнулены. RAM writes
     * и 6000 reads коммутируют с устройствами только при исключённых DMA/DMC/observers.
     */
    void replayMmc5QuietCpuCycles(unsigned clocks) noexcept;

    /**
     * @brief Сохранённое поле ABI прежнего completed-M2 режима; текущий путь его не использует.
     * @details Поля оставлены безусловными для прежнего layout Machine; часы CPU/PPU не откатываются.
     */
    struct Mmc5CompletedDebt {
        std::uint64_t replayCycle=0,deadline=0;
        std::uint8_t remainder=0,count=0;
        bool active=false,replaying=false;
        /** @brief Сохранённое поле прежнего горизонта 3..8 M2; текущий путь его не читает. */
        std::uint8_t horizon=0;
    };
    Mmc5CompletedDebt mmc5Completed_{};
    /** @brief Заимствует конкретный mapper только synchronous PPU24 owner; lease не продлевает. */
    Mmc5* mmc5QuietMapper() noexcept { return cartridge_.mmc5_; }
    /**
     * @brief Продвигает один логический такт CPU и соответствующие устройства.
     * @details Для PAL сохраняет дробный остаток отношения 16/5, для NTSC — три точки PPU. Обработка NMI/IRQ и DMC не заменяется временем хоста.
     */
    void clockCpuCycle() noexcept;
    /**
     * @brief Исполняет допустимый MMC5 CPU-префикс одного ограниченного временного горизонта.
     * @param remaining Остаток кадрового CPU deadline, без переноса grant за frame.
     * @return Реально исполненные M2 либо 0, если decoder не исполнил инструкцию.
     * @details В обычном режиме debt закрыт до возврата. Retained-epoch сохраняет
     * остаток того же grant и pending M2 для owned frame; нулевой результат тогда
     * может оставить credits обычному interpreter, но не меняет CPU/RAM. Нет нового
     * admission до расходования либо явного закрытия исходного горизонта. MMIO,
     * budget0 и выход FrameScope материализуют долг до наблюдения устройств.
     */
    unsigned runMmc5QuietBlock(unsigned remaining) noexcept;
    /**
     * @brief Доводит отложенный PPU до текущей CPU-границы.
     * @details Внешний вид const не означает отсутствие временных побочных эффектов.
     * Сначала исполняются уже прошедшие точки; затем закрывается отложенный
     * MMC5 raster даже при нулевом pendingPpuDots_. Это обязательный барьер до
     * наблюдения регистров, изменения backing-памяти и публикации кадра.
     */
    void flushPpu() const noexcept {
        if (pendingPpuDots_) flushPpuSlow();
        if (timingPpu_.hasMmc5Lazy()) timingPpu_.materializeMmc5Lazy();
    }
    /**
     * @brief Обрабатывает ненулевой долг PPU с исходными временными границами.
     * @details Опциональное объединение применяется только к основному владельцу без пакета и захвата с разрешённой элиминацией данных; остальные режимы сохраняют прежнее разбиение.
     */
    void flushPpuSlow() const noexcept;
    /**
     * @brief Обновляет число точек PPU, которые можно отложить без наблюдаемого события.
     * @details Предикаты читают текущий материализованный курсор. После MMIO требуется повторная оценка, поскольку изменяются задержки, адреса и NMI.
     */
    void refreshPpuDeferBudget() const noexcept;
    template <bool RefreshBudget = true>
    /**
     * @brief Продвигает выбранный PPU на заданное число уже прошедших точек.
     * @details Выбор полного или временного владельца не меняет гостевые clocks. Перенос сигналов CPU выполняется в предусмотренном вызывающим кодом месте.
     * @param dots Число уже прошедших точек PPU.
     */
    inline void advancePpu(unsigned dots) const noexcept;
    /**
     * @brief Делает CPU-состояние наблюдаемым после обязательной обработки отложенных чтений.
     * @details Применяется перед внешним наблюдением или эффектом, который нельзя переносить через тихий интервал.
     */
    void observeCpuReads() const noexcept {
        flushCpuReads();
    }
    /**
     * @brief Материализует накопленные CPU-доступы до текущего гостевого среза.
     * @details Не является чистым getter: синхронизирует связанные устройства и запрещён при чужом активном владении машиной.
     */
    void flushCpuReads() const noexcept {
        if (pendingCpuReads_) const_cast<Machine*>(this)->commitCpuReads();
    }
    /**
     * @brief Передаёт накопленные такты чтения CPU в общий синхронизатор машины.
     * @details Сохраняет исходный порядок продвижения устройств и переноса IRQ/NMI; не создаёт будущего бюджета.
     */
    void commitCpuReads() noexcept;
    /**
     * @brief Пересчитывает безопасный горизонт отложенных CPU-чтений.
     * @details Граница определяется ближайшим наблюдаемым событием PPU/APU/картриджа; значение не даёт права пропускать сами гостевые такты.
     */
    void refreshCpuReadBudget() noexcept;
    /**
     * @brief Проверяет чистое окно PRG-RAM для использования уже выданного CPU-кванта.
     * @return true только внутри CPU-инструкции поддержанного non-MMC5 mapper без
     * внешнего наблюдателя, которому нужна материализация перед cartridge-доступом.
     * @details Не выдаёт новый budget, не кэширует mapping/защиту и не читает память.
     * Каждый реальный доступ по-прежнему выполняет Cartridge::cpuRead/cpuWrite;
     * DMA/DMC и положительный остаток проверяет вызывающий. Объявление безусловно:
     * PRIVATE cpp-флаг не меняет layout или inline-тела Machine у consumers.
     */
    bool canRetainPrgRamWindow() const noexcept;
    std::uint8_t pendingCpuReads_ = 0, cpuReadBudget_ = 0;
    /**
     * @brief Доводит ожидающие задержки DMC до наблюдаемой CPU-границы.
     * @details Не допускает переноса остановки чтения CPU на иной шинный цикл.
     */
    void drainDmcStalls() noexcept;
    /**
     * @brief Выполняет OAM DMA с исходными чередующимися чтениями и записями.
     * @details Сохраняет остановку RDY, начальный halt и выравнивание по CPU-фазе; быстрый blank/RAM-путь допустим только при доказанных условиях.
     */
    void dma() noexcept;
    std::array<std::uint16_t, 4> nametablePages_{0, 0, 1024, 1024};
    std::array<std::uint8_t, 2> buttons_{}, controllerShift_{};
    std::uint64_t cycles_ = 0;
    Region region_ = Region::Ntsc;
    std::uint8_t openBus_ = 0, dmaPage_ = 0, ppuRemainder_ = 0;
    mutable std::uint16_t pendingPpuDots_ = 0;
    // Максимальный суммарный долг от текущей физической позиции PPU.
    mutable std::uint16_t ppuDeferBudget_ = 0;
    bool deferPpuAllowed_ = false;
    bool deferIrqPpu_ = false;
    unsigned dmcStalls_ = 0;
    bool strobe_ = false, dmaPending_ = false;
    Mmc5Usage mmc5Usage_{}; ///< Один mutable owner; сброс только успешным beginOutputFrame.
    mutable Cartridge cartridge_{};
    Cpu6502 cpu_;
    mutable Ppu ppu_;
    Apu apu_;
    mutable TimingPpu timingPpu_;
    VideoPacket* videoPacket_ = nullptr;
    // Эти поля изменяет только первичный CPU-владелец. Внешний portable bridge
    // публикует отдельный immutable envelope; Main не читает поля во время JOB.
    OutputFrameBinding outputBinding_{};
    OutputFrameLease outputLease_{};
    OutputFrameTicket outputTicket_{};
    OutputFrameCut outputCut_{};
    std::uint64_t outputEpoch_=0,outputNextSequence_=0,outputNextInput_=0;
    bool outputOwner_=false,outputInitializing_=false,outputJob_=false;
    bool outputSealed_=false,outputSnapshot_=false,outputStarted_=false;
    /** @brief Исключает alias результата с любым действующим Core/transport займом. */
    /** @brief Pure cold alias/capability admission обоих caller-owned tape extents. */
    bool outputVrc7BindingValid(const OutputFrameBinding&,const OutputFrameLease&)const noexcept;
    /** @brief Привязывает свободный Tape slot того же actual owned ticket до первого M2. */
    bool beginOutputVrc7Tape(const OutputFrameTicket&)noexcept;
    /** @brief Закрывает trace после finalmetadata только при совпавшем retiredAudio cut. */
    bool sealOutputVrc7Tape(const OutputFrameTicket&)noexcept;
    /** @brief Подтверждает, что выходной диапазон целиком помещается в разрешённую память и не перекрывает заимствованные буферы. */
    bool outputWritableResult(const void* destination,std::size_t bytes) const noexcept;
    bool videoObservableOnly_ = false;
    VideoObserver* videoObserver_ = nullptr;
    std::uint64_t videoGeneration_ = 0, videoSequence_ = 0;
    bool videoCapable_ = false;
    std::uint8_t videoPolicy_ = 0;
    mutable VideoFault videoFaultLatch_ = VideoFault::None;
    std::array<std::uint16_t,4> videoBgNames_{};
    std::array<std::uint32_t,8> videoBgChr_{};
    bool videoBgViewValid_=false,videoBgViewDirty_=true;
    NameMarkerRows videoNameMarkers_{};
    TimingApu timingApu_;
    AudioPacket* audioPacket_ = nullptr;
    std::uint64_t audioGeneration_ = 0, audioSequence_ = 0;
    bool expansionAudio_=false;
    std::int32_t lastExpansionLevel_=0;
    std::array<std::uint8_t, 2048> ram_{};
    std::array<std::uint8_t, 4096> nametables_{};
    ColourPacket* colourCapture_=nullptr;
    std::uint64_t colourEpoch_=1;
    ThinPpuChannel* thin_=nullptr;
    bool thinStickyFault_=false;

};

}  // пространство имён nes::core
