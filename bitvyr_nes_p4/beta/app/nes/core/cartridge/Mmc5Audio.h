/**
 * @file Mmc5Audio.h
 * @brief Переносимые pulse/PCM-регистры MMC5 без CPU, задач и вывода PCM.
 * @ingroup nes_cartridge
 */
#pragma once

#include <array>
#include <cstdint>

namespace nes::core {

/**
 * @brief Два pulse DAC и PCM DAC MMC5; объект принадлежит одному CPU-владельцу.
 * @details Собственная C++17-модель по NESdev MMC5_audio/APU_Pulse/APU_Envelope.
 * clockCpu получает ровно один источник M2-времени: APU quarter/half-frame сюда
 * не подаются. Делитель envelope И length равен7424 M2; это описанная NESdev
 * die-derived оценка, пока без подтверждённого аппаратного phase/rate теста.
 * Стартовая фаза и cold DAC=0 детерминированы для эмуляции; на железе power-on
 * PCM/часть pulse-состояния не определены. Здесь нет ROM, callback и аллокаций.
 */
class Mmc5Audio final {
public:
    /** @brief Цифровые DAC-уровни до инверсии, общего FIR и громкости платы. */
    struct Levels final {
        std::uint8_t pulse1 = 0; ///< Первый pulse:0..15.
        std::uint8_t pulse2 = 0; ///< Второй pulse:0..15.
        std::uint8_t pcm = 0;    ///< PCM:0..255; запись0 сохраняет прежний уровень.
    };

    static constexpr std::uint32_t framePeriodCpu = 7424; ///< CPU/M2-тактов на envelope/length clock.
    static constexpr std::int32_t pulseUnitQ16 = 653; ///< Номинально full15 близок к одному native pulse15.
    static constexpr std::int32_t pcmUnitQ16 = 296;   ///< Номинально127 близок к изолированному native DMC127.

    /**
     * @brief Создаёт определённое cold-состояние, не имитируя случайный power-on.
     * @param mmc5a true выбирает MMC5A revision bit0 при чтении5010; false — MMC5.
     */
    explicit Mmc5Audio(bool mmc5a = false) noexcept;
    /**
     * @brief Начинает новую сессию: все DAC/length/envelope и фаза обнулены.
     * @param mmc5a Аппаратная ревизия, не результат записи mode bit0.
     * @details Для обнаруженного M2-reset существующего устройства нужен
     * detectedReset(), поскольку pulse и PCM DAC от него не обнуляются.
     */
    void reset(bool mmc5a = false) noexcept;
    /**
     * @brief Обрабатывает обнаруженный MMC5 разрыв M2: control5010 становится0.
     * @details DAC, pulse, делители и IRQ-trip latch сохраняются. Снятый enable
     * убирает электрический IRQ; это не замена CPU reset и не clock advancement.
     */
    void detectedReset() noexcept;
    /**
     * @brief Продвигает только аудио на прошедшее CPU/M2-время.
     * @param cycles Число CPU-тактов, включая действительно прошедшие DMA/DMC.
     * @details Перед bus-стробом вызывающий сначала учитывает его CPU-такт.
     * Разбиение count на части сохраняет конечное состояние. Для регистрации
     * всех DAC-фронтов FIR вызывающий использует1 либо nextEventCpuCycles();
     * большой count сам не возвращает промежуточные аудиоотсчёты.
     */
    void clockCpu(std::uint32_t cycles = 1) noexcept;
    /**
     * @brief Выполняет один настоящий CPU read-strobe и возвращает bus-байт.
     * @param address Полный16-битный CPU-адрес, без зеркального декодирования.
     * @param value Фактически отображённый байт либо open bus для аудиорегистра.
     * @return Для5010 — IRQ/revision с acknowledge,5015 — length bits0/1;
     * остальные неуправляемые биты сохранены из value. PRG-байт не изменяется.
     * @details Вызывать также для opcode/dummy/DMA/DMC-чтений8000..BFFF;
     * PCM read-mode реагирует на каждый такой строб, а не только LDA. Peek/debug
     * не должен вызывать этот метод.5010 bit0 сообщает ревизию, не mode.
     */
    [[nodiscard]] std::uint8_t cpuRead(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Декодирует запись5000..5007/5010/5011/5015 без продвижения часов.
     * @param address Полный CPU-адрес;5001/5005 и прочие адреса игнорируются.
     * @param value Реальный записываемый байт;5011 в read-mode игнорируется.
     */
    void cpuWrite(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Возвращает точные текущие цифровые уровни, не меняя устройство.
     * @return Два4-битных pulse и8-битный PCM; это не48kHz PCM и не voltage.
     */
    [[nodiscard]] Levels levels() const noexcept;
    /**
     * @brief Даёт номинальный инвертированный уровень для общего APU FIR.
     * @return Знаковый уровень−95070..0 в шкале65535=1.0 существующего mixer.
     * @details Линейные653/pulse и296/PCM привязаны к standalone native pulse15
     * и DMC127; это инженерная аналоговая аппроксимация, НЕ измеренный gain платы.
     * MMC5 не входит в нелинейные native pulse/TND LUT. Сложение и DAC-delta
     * требуют signed-safe тракта до FIR/HPF, без предварительного clipping.
     */
    [[nodiscard]] std::int32_t sample() const noexcept;
    /**
     * @brief Возвращает портативный DAC с усреднением только pulse period0/1.
     * @return Signed Q16, в том же диапазоне−95070..0, что точный sample().
     * @details У активного ультразвукового pulse используется доля включённых
     * шагов duty. Gain умножается до единственного деления на8 общей суммы;
     * дробная Q16-часть округляется кнулю. Остальные pulse и PCM точны.
     * Геттер не материализует pending и не меняет фазы/IRQ; цифровой sample()
     * остаётся прежним. Изменение звука допускает только отдельный профиль.
     */
    [[nodiscard]] std::int32_t portableSample() const noexcept;
    /**
     * @brief Возвращает PCM IRQ level без acknowledge.
     * @return true только при trip latch И5010.enable; digital IRQ OR снаружи.
     */
    [[nodiscard]] bool irqLine() const noexcept { return pcmIrqTrip_ && (pcmControl_ & 0x80U); }
    /** @brief Разрешает прямые PRG reads только без PCM read-mode; часы и звук продолжают работать. */
    [[nodiscard]] bool purePrgReads() const noexcept { return (pcmControl_&1U)==0; }
    /**
     * @brief Возвращает расстояние до ближайшего внутреннего timer/frame edge.
     * @return Положительное число CPU-тактов; записи/read-mode могут дать фронт
     * раньше, поэтому через внешние bus-события этот горизонт не переносится.
     */
    [[nodiscard]] std::uint32_t nextEventCpuCycles() const noexcept;
    /**
     * @brief Возвращает ближайшую границу публикации raw output в CPU/M2 тактах.
     * @return Положительный остаток до точного visible DAC/frame события.
     * @details Не переносится через внешний CPU bus event. Каждый DAC edge
     * сохраняет прежнее время; PCM strobes остаются на настоящих CPU accesses.
     */
    [[nodiscard]] std::uint32_t nextOutputCpuCycles() const noexcept { return eventRemaining_; }
    /**
     * @brief Даёт срок следующего возможного изменения portableSample в M2.
     * @return Положительные1..7424такта: frame/envelope либо фронт неусреднённого
     * pulse. Текущий pending учтён, отброшенные carrier edges срок не сокращают.
     * @details Без bus/MMIO внутри интервала caller может вызвать clockCpu до
     * этой границы: raw фазы материализуются прежним способом. Настоящие PCM
     * reads/writes и все CPU-стробы по-прежнему внешние барьеры, не часть срока.
     * При отсутствии слышимого усредняемого pulse возвращается точный raw срок.
     */
    [[nodiscard]] std::uint32_t nextPortableOutputCpuCycles() const noexcept;


private:
    // Только постоянный native regression наблюдает slot; не даёт runtime API.
    friend struct Mmc5AudioCacheTestAccess;
    /** @brief Только постоянное состояние одного pulse, без sweep и указателей. */
    struct Pulse final {
        std::uint16_t period = 0; ///<11-битный reload;0..7 здесь НЕ заглушаются.
        std::uint16_t timer = 0;  ///< Оставшиеся APU half-rate clocks до reload.
        std::uint8_t control = 0;
        std::uint8_t sequence = 0;
        std::uint8_t length = 0;
        std::uint8_t decay = 0;
        std::uint8_t envelopeDivider = 0;
        bool envelopeStart = false;
        bool enabled = false;

        /** @brief Исполняет прошедшие half-rate timer clocks без CPU-стробов. */
        void advance(std::uint32_t ticks) noexcept;
        /** @brief Clock envelope и length на одном автономном MMC5 frame edge. */
        void frameClock() noexcept;
        /** @brief Возвращает gated DAC0..15; ни sweep, ни t<8 mute здесь нет. */
        [[nodiscard]] std::uint8_t output() const noexcept;
        /** @brief Проверяет вклад period0/1 с ненулевыми length и текущей громкостью; фазу не читает. */
        [[nodiscard]] bool portableCarrier() const noexcept;
    };

    /** @brief Применяет реальное DAC-событие:0 даёт trip, nonzero снимает trip. */
    void writePcm(std::uint8_t value) noexcept;
        /**
     * @brief Пересчитывает выбранный portable DAC после влияющего внутреннего события.
     * @details Вызывается только selected-cache политикой CPU-владельца: frame/length/
     * envelope, pulse MMIO или неусреднённый timer edge. Carrier period0/1 raw-фронты
     * его не вызывают. Не продвигает часы, не меняет IRQ, raw фазы и output deadlines.
     */
    void refreshSelectedSample() noexcept;


    /**
     * @brief Материализует прошедшее M2-время в timer/envelope состоянии.
     * @param cycles Число тактов без внешних CPU-стробов, включая UINT32_MAX.
     * @details Разбиение выполняется по frame edge. Промежуточные DAC-фронты
     * наблюдает внешний clockCpu(1) либо caller с nextEventCpuCycles().
     */
    void advanceTimers(std::uint32_t cycles) noexcept;
    /**
     * @brief Закрывает ограниченный долг до влияющей записи pulse-регистров.
     * @details Отложенный интервал строго раньше изменения наблюдаемого DAC
     * или length, поэтому его материализация не создаёт нового аудиофронта.
     */
    void flushPending() noexcept;
    /**
     * @brief Вычисляет ближайший clock, способный изменить видимый pulse DAC.
     * @details Frame edge обязателен для envelope/length. Timer выключенного
     * по length канала учитывается при flush, без вызова на каждом его фронте.
     */
    void refreshDeadline() noexcept;

    std::array<Pulse, 2> pulses_{};
    std::uint16_t frameRemaining_ = framePeriodCpu;
    std::uint8_t pcmControl_ = 0;
    std::uint8_t pcm_ = 0;
    bool halfPhase_ = false;
    bool pcmIrqTrip_ = false;
    bool mmc5a_ = false;
    // Один безусловный slot для взаимоисключающих raw/selected политик. Raw-cache
    // читает sample(), selected-cache — portableSample(); при двух OFF не читается.
    std::int32_t cachedSampleQ16_ = 0; ///< Signed Q16 выбранной политики, CPU-владелец pulses_.
    std::uint16_t pendingCpu_ = 0; ///< Уже прошедшие M2 до следующего влияющего события, меньше7424.
    std::uint16_t eventRemaining_ = framePeriodCpu; ///< Положительный остаток M2 до timer/frame edge.
};

} // namespace nes::core
