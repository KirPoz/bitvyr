/**
 * @file TimingApu.h
 * @brief Временные IRQ/DMC-состояния без генераторов, mixer и PCM-фильтра.
 * @ingroup nes_apu
 */
#pragma once

#include <array>
#include <cstdint>

namespace nes::core {

/**
 * @brief Контракт DMC-запросов с точным CPU-timestamp для временного APU.
 * @ingroup nes_apu
 */
class ApuTimingBus {
public:
    /**
     * @brief Завершает время жизни объекта с полиморфным либо владеющим контрактом.
     * @details Связанные внешние bus/ROM/буферы должны сохраняться до окончания всех активных пользователей.
     */
    virtual ~ApuTimingBus() = default;
    // Вызывающий владеет настоящим CPU-отображением и записывает возвращённый байт для
    // независимого рендерера. Ни один callback не должен рекурсивно тактировать этот объект.
    /**
     * @brief Получает фактический байт выборки DMC через соответствующий шинный контракт.
     * @details У временного APU адрес и гостевой момент передаются CPU-владельцу; renderer использует сохранённый ответ, не исполняя CPU второй раз.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    virtual std::uint8_t dmcRead(std::uint16_t address, std::uint64_t cycle) noexcept = 0;
    /**
     * @brief Учитывает украденные DMC такты в установленном порядке.
     * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
     * @param cycles Число прошедших или украденных CPU-тактов; гостевой порядок эффектов задаёт вызывающий.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     */
    virtual void dmcStall(unsigned cycles, std::uint64_t cycle) noexcept = 0;
};

// Исходная CPU-наблюдаемая проекция Apu. Без состояния генераторов, DAC, mixer и DSP.
/**
 * @brief CPU-наблюдаемая временная часть APU без повторного DSP; хранит IRQ, DMC и точные границы звуковых событий.
 * @ingroup nes_apu
 */
class TimingApu final {
public:
    /**
     * @brief Создаёт звуковой владелец с заимствованным bus-контрактом.
     * @details Состояние каналов сбрасывается, а дальнейшие DMC-чтения/IRQ подчиняются выбранному полному либо временному тракту.
     * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
     */
    explicit TimingApu(ApuTimingBus& bus) noexcept : bus_(bus) { reset(); }
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
     * @brief Продвигает APU на точное число CPU-тактов через его текущие события.
     * @details Сохраняет делители таймеров, frame sequencer, DMC и границы накопления PCM.
     * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
     */
    void advanceCpuCycles(unsigned count) noexcept;
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
     * @brief Просматривает статус без подтверждения IRQ и других эффектов чтения.
     * @details Используется только там, где вызывающему требуется наблюдение, а не гостевой bus read.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint8_t peekStatus() const noexcept;
    /**
     * @brief Возвращает текущий электрический уровень IRQ устройства.
     * @details Проверка не подтверждает IRQ и не обслуживает его в CPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool irqLine() const noexcept { return frameIrq_ || dmc_.irq; }
    /**
     * @brief Возвращает уже учтённое гостевое время соответствующего устройства.
     * @details Временные владельцы могут включать накопленные, но ещё не материализованные такты; это не время хоста.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    [[nodiscard]] std::uint64_t cycles() const noexcept { return cycles_ + pendingCycles_; }
    /**
     * @brief Возвращает число CPU-тактов до ближайшей обязательной аудиограницы.
     * @details Значение отсчитывается от текущего состояния и используется до следующего регистрового события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    [[nodiscard]] unsigned quietCpuCycles() const noexcept { return eventCountdown_ - 1U; }

    /**
     * @brief Значения выбранных внутренних звуковых регистров/счётчиков для детерминированных проверок; не полный save-state и не потокобезопасный снимок.
     * @ingroup nes_apu
     */
    struct DebugState {
        std::array<std::uint8_t, 4> length;
        std::array<bool, 4> halt, enabled;
        std::uint64_t cycle;
        std::uint32_t frameCycle;
        std::uint16_t dmcTimer, dmcAddress, dmcRemaining;
        std::uint8_t frameResetDelay, dmcControl, dmcStart, dmcLength, dmcBits;
        bool fiveStep, irqInhibit, frameIrq, dmcBufferEmpty, dmcIrq;
    };
    /**
     * @brief Возвращает снимок внутренних счётчиков и флагов устройства для диагностики.
     * @details Снимок не является полным сохраняемым состоянием всей машины.
     * @return Вычисленное значение или состояние типа DebugState согласно описанному контракту.
     */
    [[nodiscard]] DebugState debug() const noexcept;

private:
    /**
     * @brief CPU-наблюдаемое состояние length-counter с разрешением канала; звуковой генератор и DAC сюда не входят.
     * @ingroup nes_apu
     */
    struct Length {
        std::uint8_t value = 0;
        bool halt = false, enabled = false;
    };
    /**
     * @brief Таймер, оставшиеся байты, адрес, sample-buffer/shift и IRQ DMC; чтение памяти выполняет Machine через callback.
     * @ingroup nes_apu
     */
    struct Dmc {
        std::uint16_t timer = 0, address = 0xC000, remaining = 0;
        std::uint8_t control = 0, start = 0, length = 0, bits = 8;
        bool bufferEmpty = true, irq = false;
    };
    /**
     * @brief Продвигает таймер и счётчики DMC без потери выходного бита или запроса байта.
     * @details Условия IRQ, loop, окончания буфера и адресного wrap остаются исходными.
     * @param count Число CPU-тактов допустимого интервала; исключённые event/sample/IRQ/DMC-границы не пересекаются.
     * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
     * @param dmc Изменяемая CPU-наблюдаемая DMC-модель; реальные запросы памяти и украденные такты сохраняют исходную границу.
     */
    static void advanceDmc(Dmc& dmc, unsigned count, bool pal) noexcept;
    /**
     * @brief Материализует накопленное время до следующего значимого события устройства.
     * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
     */
    void synchronize() noexcept;
    /**
     * @brief Материализует накопленное время до следующего значимого события устройства.
     * @details Отложенные clocks обрабатываются хронологически; не происходит перескока через IRQ или запрос данных.
     */
    void tickEvent() noexcept;
    /**
     * @brief Продвигает последовательность quarter/half-frame событий APU.
     * @details Сохраняет NTSC/PAL-расписание, режим четырёх/пяти шагов и семантику IRQ.
     */
    void frameCounter() noexcept;
    /**
     * @brief Обслуживает полукадровые length/sweep события каналов.
     * @details Обновление делителей и запрещённых sweep-целей выполняется в исходном порядке.
     */
    void halfFrame() noexcept;
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
     * @brief Вычисляет расстояние до ближайшего события текущего временного APU.
     * @details Отсчёт выражен в CPU-тактах и ограничивает быстрый advance.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    [[nodiscard]] unsigned nextEventDistance() const noexcept;

    ApuTimingBus& bus_;
    std::array<Length, 4> lengths_{};
    Dmc dmc_{};
    std::uint64_t cycles_ = 0;
    std::uint32_t frameCycle_ = 0;
    std::uint16_t pendingCycles_ = 0, eventCountdown_ = 1;
    std::uint8_t frameResetDelay_ = 0;
    bool pal_ = false, fiveStep_ = false, irqInhibit_ = false, frameIrq_ = false;
};

} // пространство имён nes::core
