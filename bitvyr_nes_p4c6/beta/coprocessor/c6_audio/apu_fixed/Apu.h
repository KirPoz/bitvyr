/**
 * @file Apu.h
 * @brief Звуковые каналы NES, DMC-шина, таблицы и постоянное состояние фильтра.
 * @ingroup nes_apu
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "FixedFilter.h"

namespace nes::core {

/**
 * @brief Единственный владелец гостевых часов, карты памяти, CPU/PPU/APU и состояния картриджа. ROM заимствован; ввод, хранение, задачи и физический вывод остаются вне ядра.
 * @ingroup nes_apu
 */
class Machine;
/**
 * @brief Независимый потребитель аудиопакетов со своим полным APU и PCM. Не исполняет CPU производителя повторно.
 * @ingroup nes_apu
 */
class AudioRenderer;

/**
 * @brief Контракт DMC-чтения и украденных CPU-тактов полного APU.
 * @ingroup nes_apu
 */
class ApuBus {
public:
    /**
     * @brief Завершает время жизни объекта с полиморфным либо владеющим контрактом.
     * @details Связанные внешние bus/ROM/буферы должны сохраняться до окончания всех активных пользователей.
     */
    virtual ~ApuBus() = default;
    // Эти callback не должны рекурсивно вызывать Apu::tick. Machine планирует
    // запрошенные украденные такты и читает через своё CPU-отображение.
    /**
     * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
     * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    virtual std::uint8_t dmcRead(std::uint16_t address) noexcept = 0;
    /**
     * @brief Учитывает украденные DMC такты в установленном порядке.
     * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
     * @param cycles Число прошедших или украденных CPU-тактов; гостевой порядок эффектов задаёт вызывающий.
     */
    virtual void dmcStall(unsigned cycles) noexcept = 0;
};

/**
 * @brief Полная модель звуковых каналов и реконструкции PCM; bus обеспечивает реальные DMC-ответы и CPU-задержки.
 * @ingroup nes_apu
 */
class Apu final {
public:
#if defined(NES_CORE_DIRECT_BUS) && NES_CORE_DIRECT_BUS && !(defined(NES_CORE_SPLIT_APU) && NES_CORE_SPLIT_APU)
    using Bus = Machine;
#else
    using Bus = ApuBus;
#endif
    static constexpr unsigned sampleRate = 48000;
    /**
     * @brief Устанавливает внешний DAC для ближайшего CPU tick до общего FIR.
     * @param value Номинальный signed уровень MMC5−95070..0,65535=1.0.
     * @details Сначала материализует только уже прошедшие pending clocks со
     * старым DAC, затем будит событие ближайшего tick. Не продвигает новый такт.
     * Неизменившийся вход ничего не делает; native APU с входом0 сохраняет путь.
     * Вызывающий даёт один итоговый уровень перед соответствующим tick, а не
     * пропускает промежуточные фронты через advanceRendererCpuCycles().
     */
    void setExpansionInput(std::int32_t value) noexcept;
    /**
     * @brief Вносит только внешний DAC-фронт в доказанно тихую фазу следующего такта.
     * @param value Подписанный уровень MMC5 от -95070 до0 перед общим FIR.
     * @return true, если вклад принят без нового native tick; false оставляет
     * состояние неизменным и требует обычного setExpansionInput.
     * @pre Вызывает только renderer immutable-пакета: в текущем timestamp нет
     * native Input и повторного Expansion, а следующий такт входит в пакет.
     * @details Внутренний deadline исключает sample/frame/DMC/native-фронт.
     * Материализуется лишь уже прошедшее время; FIR получает ту же фазу U+1,
     * но native countdown сохраняется. Цифровое время не продвигается вперёд.
     * PRIVATE-флаг меняет только тело; API и layout одинаковы в ON/OFF.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    [[nodiscard]] bool trySetExpansionInputQuiet(std::int32_t value, bool triangleMean = false) noexcept;
    /**
     * @brief Принимает MMC5 DAC из DMC-read callback внутри текущего tickEvent.
     * @param value Подписанный уровень MMC5 от -95070 до0 перед FIR.
     * @details Только синхронный DMC callback уже активного APU: pending завершён,
     * повторный synchronize запрещён. До следующего mixerOutput новых clocks нет.
     */
    void setExpansionInputAtDmcFetch(std::int32_t value) noexcept { expansionLevel_=value; }
    static constexpr std::size_t sampleCapacity = 2048;

    /**
     * @brief Создаёт звуковой владелец с заимствованным bus-контрактом.
     * @details Состояние каналов сбрасывается, а дальнейшие DMC-чтения/IRQ подчиняются выбранному полному либо временному тракту.
     * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
     */
    explicit Apu(Bus& bus) noexcept;
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
     */
    void reset(bool pal = false) noexcept;
    /**
     * @brief Продвигает устройство на одну его логическую единицу времени.
     * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
     */
    void tick() noexcept {
        if (--eventCountdown_) { ++pendingCycles_; return; }
        tickEvent();
    }
    /**
     * @brief Записывает байт через протокол устройства или шины.
     * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void write(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Читает CPU-видимый статус каналов и подтверждает предусмотренный frame-IRQ.
     * @details Семантика отличается от peekStatus: побочный эффект чтения должен попасть в соответствующий пакет.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint8_t readStatus() noexcept;
    /**
     * @brief Возвращает текущий электрический уровень IRQ устройства.
     * @details Проверка не подтверждает IRQ и не обслуживает его в CPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool irqLine() const noexcept { return frameIrq_ || dmc_.irq; }
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const std::int16_t* samples() const noexcept { return samples_.data(); }
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    [[nodiscard]] std::size_t sampleCount() const noexcept { return sampleCount_; }
    /**
     * @brief Начинает накопление следующего блока PCM без изменения гостевых регистров каналов.
     * @details В split-режиме соответствующая операция сохраняется для renderer, чтобы границы аудиоблоков совпали.
     */
    void clearSamples() noexcept { sampleCount_ = 0; }
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t totalSamples() const noexcept { return totalSamples_; }
    /**
     * @brief Возвращает заимствованный PCM либо счётчик его заполнения/потерь.
     * @details Буфер меняется при дальнейшем аудиоисполнении; Machine в split-режиме не предоставляет renderer-owned PCM через обычный accessor.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t droppedSamples() const noexcept { return droppedSamples_; }

    /**
     * @brief Значения выбранных внутренних звуковых регистров/счётчиков для детерминированных проверок; не полный save-state и не потокобезопасный снимок.
     * @ingroup nes_apu
     */
    struct DebugState {
        std::array<std::uint16_t, 2> pulsePeriod;
        std::array<std::uint8_t, 4> length;
        std::uint16_t dmcAddress, dmcRemaining, noiseShift;
        std::uint8_t dmcOutput, triangleLinear;
        std::uint32_t frameCycle;
        bool frameIrq, dmcIrq;
    };
    /**
     * @brief Возвращает снимок внутренних счётчиков и флагов устройства для диагностики.
     * @details Снимок не является полным сохраняемым состоянием всей машины.
     * @return Вычисленное значение или состояние типа DebugState согласно описанному контракту.
     */
    [[nodiscard]] DebugState debug() const noexcept;

private:
    friend class AudioRenderer;
#if defined(NES_CORE_CPU_READ_QUANTA) && NES_CORE_CPU_READ_QUANTA
    friend class Machine;
/**
 * @brief Независимый потребитель аудиопакетов со своим полным APU и PCM. Не исполняет CPU производителя повторно.
 * @ingroup nes_apu
 */
class AudioRenderer;
    /**
     * @brief Возвращает допустимый CPU-интервал до следующего аудиособытия.
     * @details Горизонт ограничивает отложенные clocks; не даёт права терять DMC-выборки или побочные эффекты.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned asyncQuietCpuCycles() const noexcept;
    /**
     * @brief Продвигает APU на точное число CPU-тактов через его текущие события.
     * @details Сохраняет делители таймеров, frame sequencer, DMC и границы накопления PCM.
     * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    void advanceCpuCycles(unsigned count, bool triangleMean = false) noexcept;
#if defined(NES_CORE_APU_TRIANGLE_SEGMENTS) && NES_CORE_APU_TRIANGLE_SEGMENTS
    /**
     * @brief Продвигает соответствующий аудиовладелец на заданное число CPU-тактов.
     * @details Объединение не пересекает наблюдаемое IRQ/DMC-событие и не исполняет CPU повторно.
     * @param count Длина допустимого интервала в гостевых CPU-тактах.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    void advanceRendererCpuCycles(unsigned count, bool triangleMean = false) noexcept;
#endif
#endif
    /**
     * @brief Состояние громкостной огибающей pulse/noise: делитель, decay, restart и постоянная громкость.
     * @ingroup nes_apu
     */
    struct Envelope {
        std::uint8_t control = 0, divider = 0, decay = 0;
        bool start = false;
        /**
         * @brief Выполняет один шаг часов соответствующего устройства.
         * @details Единица времени определяется классом владельца; метод изменяет внутренние делители и события, а не системное время ESP-IDF.
         */
        void clock() noexcept;
        /**
         * @brief Возвращает состояние или результат текущей привязки.
         * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
         * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
         */
        [[nodiscard]] std::uint8_t output() const noexcept {
            return (control & 0x10) ? (control & 15) : decay;
        }
    };
    /**
     * @brief Таймер, duty/sequence, length, envelope и sweep одного pulse-канала; различие negate carry определяется номером канала.
     * @ingroup nes_apu
     */
    struct Pulse {
        Envelope envelope{};
        std::uint16_t period = 0, timer = 0;
        std::uint8_t sweep = 0, sweepDivider = 0, sequence = 0, length = 0;
        bool enabled = false, sweepReload = false;
    };
    /**
     * @brief Таймер и 32-шаговая фаза triangle с length/linear-counter; выход DAC сохраняется при закрытом тактировании.
     * @ingroup nes_apu
     */
    struct Triangle {
        std::uint16_t period = 0, timer = 0;
        std::uint8_t control = 0, linear = 0, length = 0, sequence = 15;
        bool enabled = false, reload = false;
    };
    /**
     * @brief Таймер noise, length/envelope и LFSR с выбираемым feedback-отводом.
     * @ingroup nes_apu
     */
    struct Noise {
        Envelope envelope{};
        std::uint16_t timer = 0, shift = 1;
        std::uint8_t control = 0, length = 0;
        bool enabled = false;
    };
    /**
     * @brief Таймер, оставшиеся байты, адрес, sample-buffer/shift и IRQ DMC; чтение памяти выполняет Machine через callback.
     * @ingroup nes_apu
     */
    struct Dmc {
        std::uint16_t timer = 0, address = 0xC000, remaining = 0;
        std::uint8_t control = 0, start = 0, length = 0, output = 0;
        std::uint8_t buffer = 0, shift = 0, bits = 8;
        bool bufferEmpty = true, silent = true, irq = false;
    };

#if defined(NES_CORE_APU_DENSE_PHASES) && NES_CORE_APU_DENSE_PHASES
    // Явный экспериментальный режим сборки; сбросить/пересобрать всех APU-consumer.
    static constexpr unsigned impulseSize = 16;
    static constexpr unsigned impulsePhases = 512;
#else
    static constexpr unsigned impulseSize = 32;
    static constexpr unsigned impulsePhases = 64;
#endif
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    static_assert(impulseSize == 16 && impulsePhases == 512, "Hybrid rank4 requires Dense16/512");
#endif
    /**
     * @brief Общие неизменяемые таблицы нелинейных DAC и фазовых FIR-импульсов; создаются единожды и не выделяются покадрово.
     * @ingroup nes_apu
     */
    struct Tables {
        std::array<std::uint16_t, 31> pulse;
        std::array<std::uint16_t, 16 * 16 * 128> tnd;
        std::array<std::array<std::int16_t, impulseSize>, impulsePhases> impulse;
        /**
         * @brief Вычисляет неизменяемые коэффициенты смешивания и реконструкции APU.
         * @details Таблицы разделяются через sharedTables; расчёт не зависит от исполнения конкретного ROM.
         */
        Tables() noexcept;
    };
    /**
     * @brief Предоставляет общие неизменяемые таблицы синтеза APU.
     * @details Таблицы создаются один раз и используются всеми экземплярами; вызов не продвигает гостевые такты и не выделяет покадровое хранилище.
     * @return Ссылка на таблицы со статическим сроком жизни.
     */
    static const Tables& sharedTables() noexcept;
    /**
     * @brief Обслуживает четвертькадровые envelope и linear-counter события.
     * @details Меняет состояние каналов, но не означает выдачу готового блока PCM.
     */
    void quarterFrame() noexcept;
    /**
     * @brief Обслуживает полукадровые length/sweep события каналов.
     * @details Обновление делителей и запрещённых sweep-целей выполняется в исходном порядке.
     */
    void halfFrame() noexcept;
    /**
     * @brief Продвигает последовательность quarter/half-frame событий APU.
     * @details Сохраняет NTSC/PAL-расписание, режим четырёх/пяти шагов и семантику IRQ.
     */
    void frameCounter() noexcept;
    /**
     * @brief Продвигает внутренние таймеры звуковых каналов.
     * @details Чередование CPU-фазы pulse/noise и каждый такт triangle/DMC сохраняются.
     */
    void clockTimers() noexcept;
    /**
     * @brief Материализует накопленное время до следующего значимого события устройства.
     * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    void tickEvent(bool triangleMean = false) noexcept;
    /**
     * @brief Компонует интервал каналов APU, в котором не нужны отдельные внешние обращения.
     * @details Сохраняет те же конечные таймеры/уровни и реконструкционный вклад; регистровые события остаются границами.
     * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
     * @param startCycle Абсолютный гостевой CPU-такт начала уже разрешённого аудиоинтервала.
     */
    void advanceSkipped(unsigned count, std::uint64_t startCycle) noexcept;
    /**
     * @brief Материализует накопленное время до следующего значимого события устройства.
     * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
     */
    void synchronize() noexcept;
    /**
     * @brief Вычисляет расстояние до ближайшего события текущего временного APU.
     * @details Отсчёт выражен в CPU-тактах и ограничивает быстрый advance.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    [[nodiscard]] unsigned nextEventDistance(bool triangleMean = false) const noexcept;
    /**
     * @brief Компонует продвижение noise LFSR на заданное число шагов.
     * @details Применяется только в текущем доказанном интервале; регистровые записи остаются отдельными границами.
     * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
     * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
     * @param noise Изменяемое состояние noise-таймера/LFSR; переданное число CPU-тактов не пересекает исключённые события.
     */
    static void advanceNoise(Noise& noise, unsigned count, bool pal) noexcept;
    /**
     * @brief Запрашивает либо перезапускает фактическую выборку байта DMC.
     * @details Чтение и украденные CPU-такты согласуются через bus-контракт; значение не генерируется по памяти renderer произвольно.
     */
    void fetchDmc() noexcept;
    /**
     * @brief Запрашивает либо перезапускает фактическую выборку байта DMC.
     * @details Чтение и украденные CPU-такты согласуются через bus-контракт; значение не генерируется по памяти renderer произвольно.
     */
    void restartDmc() noexcept;
    /**
     * @brief Вычисляет целевой период pulse sweep с особенностью отрицания канала.
     * @details Результат используется проверкой допустимости, а не немедленной записью нового периода.
     * @param channel Индекс pulse-канала 0 или 1; определяет разницу negate carry в sweep.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    [[nodiscard]] unsigned sweepTarget(unsigned channel) const noexcept;
    /**
     * @brief Вычисляет текущий уровень pulse-канала из его таймера, duty и envelope.
     * @details Запрещённые длина и sweep-цель дают тот же заглушённый уровень; метод не читает CPU-память.
     * @param channel Индекс pulse-канала 0 или 1; определяет разницу negate carry в sweep.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    [[nodiscard]] unsigned pulseOutput(unsigned channel) const noexcept;
    /**
     * @brief Смешивает текущие уровни NES-каналов по используемым нелинейным таблицам.
     * @details Это модель цифрового звукового тракта; аппаратный DAC платы и громкость находятся вне ядра.
     * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    [[nodiscard]] std::int32_t mixerOutput(bool triangleMean = false) const noexcept;
    /**
     * @brief Проверяет renderer-only ультразвуковой native triangle после завершения старого длинного timer.
     * @details Политика неизменна с reset: pending-интервал не пересекает вход timer-period в усреднение.
     * Длина и linear counter должны разрешать звук; остановленный канал сохраняет свой DAC.
     */
    [[nodiscard]] bool triangleMeanEligible() const noexcept {
        return triangle_.period <= 1U && triangle_.length && triangle_.linear && triangle_.timer <= triangle_.period;
    }

    /** @brief Вносит широкий DAC-фронт с int64-умножением; обычный NES сохраняет прежний FIR-путь. */
    void depositWideDelta(std::int32_t delta,unsigned phase) noexcept;
    /**
     * @brief Вносит ступень изменения звукового уровня в фазовый импульсный накопитель.
     * @details Положение определяется CPU-временем и фазой дискретизации; вклад не меняет последовательность регистровых событий.
     * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    void depositDelta(std::int32_t delta, bool triangleMean = false) noexcept;
#if (defined(NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) && NES_CORE_APU_TRIANGLE_PHASE_RECURRENCE) || (defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4)
    /**
     * @brief Вносит ступень изменения звукового уровня в фазовый импульсный накопитель.
     * @details Положение определяется CPU-временем и фазой дискретизации; вклад не меняет последовательность регистровых событий.
     * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
     * @param phase Фаза FIR0..63; индексирует коэффициенты дробной задержки, не PPU-фазу.
     
     * @param triangleMean Неизменная после reset политика только AudioRenderer: усреднять
     * активный triangle с period0/1 после старого timer; CPU-владелец передаёт false.
     */
    inline void depositDeltaAtPhase(std::int32_t delta, unsigned phase, bool triangleMean = false) noexcept;
#endif
#if defined(NES_CORE_APU_TRIANGLE_SCRATCH) && NES_CORE_APU_TRIANGLE_SCRATCH
    /**
     * @brief Накапливает ограниченный вклад triangle в выбранной фазе звукового реконструктора.
     * @details Сохраняет моменты изменения уровня; временное хранилище не является отдельным аудиопотоком.
     * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
     * @param phase Фаза FIR-импульса 0..63 для дробного положения DAC-перехода между PCM-отсчётами.
     * @param scratch Изменяемый временный накопитель вклада сегмента; размер задан FIR-контрактом, заимствуется на вызов.
     */
    inline void depositTriangleScratch(std::int32_t delta, unsigned phase, std::int32_t* scratch) noexcept;
#endif
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    /**
     * @brief Проверяет применимость выбранной таблицы моментов к текущему звуковому событию.
     * @details Не меняет доступные каналы или их гостевые clocks.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool momentSelected() const noexcept { return triangle_.period <= 1 && triangle_.length && triangle_.linear; }
    /**
     * @brief Накапливает ограниченный вклад triangle в выбранной фазе звукового реконструктора.
     * @details Сохраняет моменты изменения уровня; временное хранилище не является отдельным аудиопотоком.
     * @param delta Знаковая разность целочисленных уровней mixer до FIR, не готовый PCM-отсчёт.
     * @param phase Фаза FIR0..63; индексирует коэффициенты дробной задержки, не PPU-фазу.
     */
    inline void depositMomentAtPhase(std::int32_t delta, unsigned phase) noexcept;
#endif
    /**
     * @brief Выдаёт очередной PCM-отсчёт в ограниченный внутренний буфер APU.
     * @details Переполнение учитывается, а частота дискретизации выводится из гостевого времени.
     */
    void emitSample() noexcept;

    Bus& bus_;
    const Tables& tables_;
    std::array<Pulse, 2> pulses_{};
    Triangle triangle_{};
    Noise noise_{};
    Dmc dmc_{};
    // Логический слот k — точная сумма половин k и k+impulseSize.
    std::array<std::int64_t, 2 * impulseSize> pendingImpulse_{};
    std::uint64_t cycles_ = 0, totalSamples_ = 0, droppedSamples_ = 0;
    std::size_t sampleCount_ = 0;
    std::uint32_t frameCycle_ = 0, samplePhase_ = 0, cpuRate_ = 1789773;
    std::int32_t lastMix_ = 0;
    std::int32_t expansionLevel_ = 0; // Инвертированный DAC MMC5 до общего FIR; обнуляется при reset.
    std::int64_t integrator_ = 0;
    unsigned impulsePosition_ = 0;
    std::uint8_t frameResetDelay_ = 0;
    std::uint8_t pendingCycles_ = 0, eventCountdown_ = 1;
    bool pal_ = false, fiveStep_ = false, irqInhibit_ = false, frameIrq_ = false;
    float highPass90_ = 0, highPass440_ = 0, lowPass14000_ = 0;
    float previousInput_ = 0, previousHighPass90_ = 0;
    // Layout одинаков при exact/fixed CPP-профилях; active цифровой APU это поле не читает.
    // Состояние принадлежит только render-владельцу; overflow отравляет весь DSP-пакет.
    nes::audio_probe::FixedState fixedFilter_{};
    bool fixedFilterFault_=false;
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    std::uint8_t momentCountdown_ = 0;
#endif
    std::array<std::int16_t, sampleCapacity> samples_{};
#if defined(NES_CORE_APU_HYBRID_RANK4) && NES_CORE_APU_HYBRID_RANK4
    std::array<std::array<std::int64_t, 16>, 4> momentHistory_{};
#endif
};

}  // пространство имён nes::core
