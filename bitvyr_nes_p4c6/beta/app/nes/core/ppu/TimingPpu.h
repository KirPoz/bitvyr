/**
 * @file TimingPpu.h
 * @brief Временное состояние PPU, Sprite0/overflow и контракты неизменяемого префикса.
 * @ingroup nes_ppu
 */
#pragma once
#include "VideoPacket.h"
#include "../cartridge/Mmc5RasterPlan.h"
#include <array>
#include <cstdint>

namespace nes::core {
/**
 * @brief Единственный владелец гостевых часов, карты памяти, CPU/PPU/APU и состояния картриджа. ROM заимствован; ввод, хранение, задачи и физический вывод остаются вне ядра.
 * @ingroup nes_ppu
 */
class Machine;
/** @brief Заимствованная цифровая часть конкретного canonical replay. */
class Mmc5;

// CPU-owner frontend времени/эффектов. Не имеет цветового framebuffer. Пакет
// записывает неизменяемые fetch-идентичности; рендерер владеет всей palette mux-работой.
/**
 * @brief Временной владелец CPU-наблюдаемых PPU-сигналов и исходных адресных эффектов. Не является самостоятельным физическим видеовыходом.
 * @ingroup nes_ppu
 */
class TimingPpu final {
public:
    /**
     * @brief Создаёт PPU, связанный с указанной шиной.
     * @details Срок жизни шины должен покрывать весь срок жизни PPU. Копия не получает автоматически RGB-контекст или полномочия основного временного владельца.
     * @param bus Заимствованная шина микросхемы; её объект переживает устройство и исключает недопустимый рекурсивный вызов.
     */
    explicit TimingPpu(Machine& bus) noexcept : bus_(bus) { reset(); }
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
     */
    void reset(bool pal = false) noexcept;
    /**
     * @brief Привязывает заимствованный ресурс или временного получателя к текущему владельцу.
     * @details Точные ограничения размера, фазового среза и неперекрытия определяются этим типом; произвольное освобождение ресурса во время использования недопустимо.
     * @param packet Заимствованный активный VideoPacket; nullptr отключает запись, не создавая нового владельца.
     */
    void bind(VideoPacket* packet) noexcept { materializeMmc5Lazy(); packet_ = packet; invalidateMmc5RasterPlan(); }
    /**
     * @brief Выбирает требуемое тактование и выставление адресов текущего PPU.
     * @details Флаги определяются режимом mapper; отключение ненужного провода не разрешает терять цветовые защёлки.
     * @param cycles Наличие подключённого наблюдателя PPU-тактов; не количество тактов.
     * @param addresses Наличие подключённого наблюдателя адресных линий; не число обращений.
     */
    void setBusTracking(bool cycles, bool addresses) noexcept { materializeMmc5Lazy(); trackBusCycles_ = cycles; trackBusAddresses_ = addresses; }
    /**
     * @brief Продвигает устройство на одну его логическую единицу времени.
     * @details Для PPU единица — точка растра, для APU — CPU-такт. Обслуживаются предусмотренные текущей фазой задержки и сигналы, а не время хоста.
     */
    void tick() noexcept;
    /**
     * @brief Продвигает PPU на указанное число уже прошедших точек.
     * @details Групповые пути сохраняют специальные точки, границы строк, delayed-mask/NMI и mapper-стробы; недопустимый участок остаётся скалярным.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    void tickDots(unsigned dots) noexcept;
    /** @brief Читает сохранённое поле ABI прежнего отложенного MMC5 режима.
     * @details Текущий producer этот долг не создаёт; layout и запрос сохранены. */
    bool hasMmc5Lazy() const noexcept { return mmc5Lazy_.dots != 0U; }
    /** @brief Совместимый no-op: текущий профиль не накапливает этот долг.
     * @details Метод не меняет PPU/mapper, память, часы CPU/APU или владение. */
    void materializeMmc5Lazy() noexcept;
    /**
     * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
     * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
     * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t cpuRead(std::uint16_t reg) noexcept;
    /**
     * @brief Проверяет либо обслуживает чтение STATUS без полного доведения лишь при доказанной стабильности.
     * @details Биты overflow/Sprite0, задержки, NMI, writeToggle и открытая шина требуют исходного точного помощника; ложный предикат оставляет обычный MMIO-путь.
     * @param dots Число уже прошедших точек PPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param value Выходной байт фактического чтения; допустим только после успешного результата метода.
     */
    bool tryReadDeferredStatus(unsigned dots, std::uint8_t& value) noexcept;
    /**
     * @brief Проверяет дополнительные условия идемпотентного свёрнутого STATUS-опроса.
     * @details Стабильных верхних битов недостаточно: учитываются toggle, NMI и открытая шина.
     * @param dots Число уже прошедших точек PPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool canRepeatStatusPoll(unsigned dots) const noexcept;
    /**
     * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
     * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
     * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void cpuWrite(std::uint16_t reg, std::uint8_t value) noexcept;
    /**
     * @brief Записывает очередной байт OAM DMA в текущий OAM-адрес.
     * @details Продвигает адрес и инвалидирует связанные sprite-кэши в исходном порядке; не заменяет источник DMA.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void dmaWrite(std::uint8_t value) noexcept;
    /**
     * @brief Забирает подготовленное событие NMI, предназначенное CPU.
     * @details Возврат события отличается от простого просмотра уровня линии и должен соблюсти исходный порядок синхронизации.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool takeNmi() noexcept;
    /**
     * @brief Проверяет готовность гостевой кадровой границы.
     * @details Не означает, что потребитель уже закончил цветовой повтор или физический вывод.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool frameReady() const noexcept { return frameReady_; }
    /**
     * @brief Снимает признак уже обработанной гостевой кадровой границы.
     * @details Не сбрасывает номер кадра и не создаёт новый физический кадр.
     */
    void clearFrameReady() noexcept { frameReady_ = false; }
    /**
     * @brief Возвращает счётчик гостевых кадров выбранного PPU.
     * @details Не является числом физических подач на LCD или игровым FPS.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t frameNumber() const noexcept { return frames_; }
    /**
     * @brief Возвращает растровую координату либо дробную фазу текущего владельца.
     * @details Значения относятся к одному уже материализованному срезу; номер строки не заменяет CPU-cycle или PAL-остаток.
     * @return Вычисленное значение или состояние типа std::int16_t согласно описанному контракту.
     */
    std::int16_t scanline() const noexcept { return scanline_; }
    /**
     * @brief Возвращает растровую координату либо дробную фазу текущего владельца.
     * @details Значения относятся к одному уже материализованному срезу; номер строки не заменяет CPU-cycle или PAL-остаток.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t dot() const noexcept { return dot_; }
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned deferredPrefix() const noexcept { return (1U - dot_) & 7U; }
    /**
     * @brief Проверяет низкий уровень A12 для точной текущей фоновой фазы.
     * @details Сохранённый адрес и возможный перенос +8 важнее простого предположения о выбранной таблице паттернов.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool backgroundA12Low() const noexcept { return (control_ & 0x10) == 0; }
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @param dots Число уже прошедших точек PPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool canDeferDots(unsigned dots) const noexcept {
        return renderingEnabled_ && !(renderDelay_ | nmiDelay_) &&
            static_cast<unsigned>(scanline_) < 240 && dot_ >= 1 && dot_ + dots <= 249;
    }
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned nonVisibleQuietDots() const noexcept;
    /** @brief Возвращает pure горизонт без cartridge RD; границы NMI/строк снаружи.
     * @return Число PPU dots до запретной точки, либо 0. Не тактирует устройства. */
    unsigned mmc5InplaceNoRdDots() const noexcept;
    /** @brief Проверяет stable active detector head и возвращает dots до 337.
     * @details Не выдаёт grant без внешних CPU/audio/lease/capacity guards Machine. */
    unsigned mmc5InplaceActiveDots() const noexcept;
    /** @brief Исполняет authoritative active dots без повторных per-M2 wrappers.
     * @param dots Фактический потреблённый NTSC prefix3..240, кратный 3.
     * @pre Предварительный certificate остаётся действителен, CPU/MMIO стоит. */
    void replayMmc5InplaceDots(unsigned dots) noexcept;
    /**
     * @brief Вычисляет безопасную длину отложенного/группового PPU-интервала.
     * @details Учитывает ближайшие растровые, NMI и mapper-границы. Возвращённый горизонт не означает уже выполненного события.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned quietDots() const noexcept;
    // Барьер неизменяемого префикса: выдать локальное draw-состояние до наблюдения consumer.
    /**
     * @brief Доводит отложенную очередь видимых пикселей до текущего среза.
     * @details Сначала сохраняет исходный индексный выбор и необходимые хвосты; материализация не даёт права переиспользовать аппаратно читаемый буфер.
     */
    void flushPixels() noexcept;
private:
    friend class Machine;
    friend struct MachineEventKernelTestAccess;
    /**
     * @brief Запрещает повторное использование mapper-дескрипторов после смены binding/reset.
     * @details Сам план не разыменовывается и не очищается. Нулевой tag гарантирует,
     * что непроинициализированные либо прежние заимствования не читаются до prepare.
     */
    void invalidateMmc5RasterPlan() noexcept { rasterPlanVersion_ = 0; }
    /**
     * @brief Получает подготовленную policy текущего sole-owner MMC5 на один синхронный вызов.
     * @param mapper Текущий живой mapper машины; caller уже исключил параллельный MMIO/load.
     * @return Заимствованный до ближайшей мутации план либо nullptr для старого fallback.
     * @details Ключ включает owner/version/control/CIRAM/CHR backing; версия 0 всегда miss.
     * Binding/reset/model-rebind отдельно инвалидируют ABA между ROM-арендами.
     */
    Mmc5RasterPlan* acquireMmc5RasterPlan(Mmc5& mapper) noexcept;
    /** @brief Совместимо отклоняет накопление отложенных PPU-точек.
     * @param dots Прежняя длина участка; текущий профиль её не потребляет.
     * @param clockFirstM2 Прежняя политика первого M2; текущий профиль её не исполняет.
     * @return Всегда false, без изменения guest state, pipeline или packet. */
    bool tryAppendMmc5Lazy(unsigned dots, bool clockFirstM2) noexcept;
    /** @brief Независимый прежний replay; materializer явно задаёт первый M2.
     * @param dots Ограниченный завершённый visible debt.
     * @param clockFirstM2 false для seed после настоящего scalar Cart.clockCpu. */
    void replayMmc5InplaceDotsEager(unsigned dots, bool clockFirstM2) noexcept;
    /** @brief Сохранённый layout прежнего отложенного visible интервала.
     * @details Текущий профиль его не заполняет. Безусловные поля оставлены,
     * чтобы сохранить размер класса и смещения у всех потребителей. */
    struct Mmc5LazyState {
        std::array<std::uint32_t,6> atoms{};
        Mmc5* mapper = nullptr;
        VideoPacket* packet = nullptr;
        const std::uint8_t* chr = nullptr;
        std::uint64_t generation = 0, sequence = 0;
        std::uint32_t version = 0;
        unsigned dots = 0, pixels = 0, pixelX = 0;
        std::uint16_t dot = 0, v = 0, address = 0, spriteClocks = 0;
        std::uint16_t lastAddress = 0, tileNtOffset = 0, pattern = 0;
        std::uint8_t shifts = 0, tile = 0, attribute = 0, paletteIndex = 0;
        std::uint8_t repeat = 0, idle = 0, tileAttribute = 0, splitAttribute = 0, splitY = 0;
        std::uint8_t control = 0, mask = 0, fineX = 0;
        bool coarse = false, scroll = false, fill = false, firstClock = false;
        bool readSinceM2 = false, extended = false, split = false;
    };
    /** @brief Пассивные C1-счётчики; shipping без C1 не исполняет их записи. */
    struct Mmc5LazyCounts {
        std::uint32_t chunks = 0, dots = 0, materializations = 0;
        std::uint32_t entryRejects = 0, boundaryRejects = 0, capacityRejects = 0;
        std::uint32_t scalarChunks = 0, gqChunks = 0;
    };
    /**
     * @brief Продвигает разрешённые 3/4 видимые точки через одну подготовленную policy.
     * @param dots Реальный уже прошедший M2-префикс длиной 3 либо 4 точки без строки/MMIO.
     * @param plan Подготовленный план этого mapper и текущего PPUCTRL.
     * @pre Caller проверил прежний visible-M2 домен, владельца пакета и отсутствие capture.
     * @details Сохраняет каждый clock/address/read и Sprite0; безопасный bookkeeping
     * интервал складывает debt один раз, остальные точки используют прежние pixel checks.
     */
    void runMmc5PlanVisibleM2(unsigned dots,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Исполняет bounded visible-интервал арифметически вместо поточечных фаз.
     * @param dots Уже разрешённые 1..24 PPU-точки без CPU/MMIO внутри.
     * @param plan Живой Normal/Extended план текущей sole-owner mapper-эпохи.
     * @return true после точного исполнения; false без мутации guest/packet, в том числе при OFF.
     * @details Не пересекает dot255/256, row или Sprite0-кандидата. Сохраняет
     * каждый /RD и прежние proto0 records/credits; count публикуется один раз.
     * Внешний pure OFF stub оставляет одинаковый private diagnostic API всех TU.
     */
    bool tryMmc5RasterInterval(unsigned dots,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Выполняет заранее проверенный interval в одной аппаратной policy.
     * @tparam Policy Normal0 либо Extended1; split намеренно остаётся прежним путём.
     * @param dots Число уже допущенных точек 1..24, не новый CPU grant.
     * @param plan План той же ROM/MMIO-эпохи; callbacks/наблюдатели исключены.
     * @details Caller доказал точную ёмкость и вызвал общий reserveEvents. Тело
     * держит cursor/debts локально; использует прежние VideoEvent без нового codec.
     */
    template<unsigned Policy> void runMmc5RasterInterval(unsigned dots,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Исполняет одну typed BG-фазу без Machine/Fetch-enum повторного выбора.
     * @tparam Policy Число 0..3, совпадающее с подготовленной normal/extended/split policy.
     * @tparam Phase Фаза 0..7 настоящего фонового тайла.
     * @param plan Текущий borrowed mapper-план; read-фазы создают прежний /RD.
     */
    template<unsigned Policy,unsigned Phase> void fetchMmc5PlanPhase(Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Исполняет видимую группу с compile-time policy/clock/bookkeeping.
     * @tparam Policy Подготовленный аппаратный режим 0..3.
     * @tparam Clocked true сохраняет bus ppuCycle перед каждой точкой.
     * @tparam Aggregate true допускается только без load/flush/Sprite0/end-line внутри группы.
     * @param dots Уже разрешённые точки 3/4, не пересекающие строку или MMIO.
     * @param plan Подготовленный план текущего sole-owner mapper.
     */
    template<unsigned Policy,bool Clocked,bool Aggregate>
    void runMmc5PlanVisibleGroup(unsigned dots,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Исполняет один видимый MMC5-отрезок до ближайшей границы тайла.
     * @param dots Уже разрешённые точки, от 1 до остатка текущей восьмиточечной фазы.
     * @return false без мутации при неизвестном состоянии, Sprite0 или нехватке credits.
     * @details Единственный production caller находится внутри owned in-place replay:
     * CPU/MMIO и Machine time неподвижны, ближайшие IRQ/NMI/DMC уже ограничили grant.
     * Read/address-фазы и порядок packet ops сохраняются, а pixel/shift/sprite debt
     * материализуются один раз. Dot255/256, sprite/dummy и строковые границы снаружи.
     * При выключенном PRIVATE-опыте всегда возвращает false; новых полей не вводит.
     */
    bool tryMmc5VisiblePhaseSegment(unsigned dots) noexcept;
    /**
     * @brief Выполняет прежний короткий endpoint с уже подготовленной policy.
     * @param dots Число точек до конца текущей восьмиточечной фазы.
     * @param plan Borrowed план, сохраняющий mapper-параметры до возврата.
     * @return true при исполнении; false без мутации для прежнего fallback.
     */
    bool tryMmc5PlanPhaseSegment(unsigned dots,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Специализация короткого endpoint без повторного policy dispatch по fetch.
     * @tparam Policy Аппаратная policy0..3 текущего плана.
     * @param dots Число точек 1..8 до ближайшей границы тайла.
     * @param plan Подготовленный sole-owner план.
     * @return true только после точного завершения прежнего endpoint.
     */
    template<unsigned Policy> bool tryMmc5PlanPhase(unsigned dots,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Проверяет только необходимую растровую геометрию completed-H2, без admission.
     * @param dots Полная длина в PPU dots:6/7 для H2 либо минимум 9 для trusted grant.
     * @details Только row/dot/phi/end; не читает packet/ROM, не flush-ит и не меняет fault/time/credits.
     * @return true не разрешает grant: все полные audio/PPU/owner проверки остаются у caller.
     */
    bool mmc5CompletedRasterGeometry(unsigned dots) const noexcept {
        return scanline_>=0&&scanline_<240&&dot_>=9U&&dot_<=57U&&
            ((dot_-1U)&7U)==0U&&dot_+dots<=64U;
    }
    /**
     * @brief Pure H2 domain до sprite evaluation65: phi0 и максимум 7dots, без S0/row/fault edge.
     * @param dots Полная длина двух real M2 из исходного remainder, только 6/7.
     * @details Не читает ROM, не разрешает atoms, не flush-ит packet и не списывает credits.
     * @return true допускает только данный H2 prefix; false сохраняет старый scalar путь.
     */
    bool canCompleteMmc5H2(unsigned dots) const noexcept;
    /**
     * @brief Повторяет целые группы 24 dots/8M2 внутри уже допущенного B3.
     * @param clocks Фактически завершённые CPU M2; не новый grant.
     * @return Число уже повторённых M2, кратное 8. При отказе 0 без мутации.
     * @details Состояние плоскостей call-local, материализуется до возврата.
     * Не меняет Machine time внутри run. Единственный caller немедленно добавляет
     * возвращённые M2 до полной IRQ/NMI проверки и любого scalar/observer пути.
     */
    unsigned replayMmc5QuietBlock(unsigned clocks) noexcept;
    /** @brief Локальный невидимый BG pipeline; не хранится в Machine/packet и не передаётся Main. */
    struct Mmc5TrustedState {
        std::array<std::uint32_t,6> atoms{};
        unsigned pixels=0,x=0,shifts=0,sprites=0;
    };
    /** @brief Выполняет один BG-тайл внутри run без per-dot Machine callbacks.
     * @details Читает реальные Name/Attr/CHR на прежних фазах. Текущие Machine
     * clocks не читает и не публикует; замыкание времени обеспечивает caller. */
    void runMmc5QuietTile(Mmc5TrustedState& state,Mmc5& mapper) noexcept;
    /** @brief Исполняет полный certified BG run над настоящим TimingPpu без seed/import.
     * @param tiles От 3 до 32 целых visible тайлов; старый grant уже исключил MMIO/S0/IRQ.
     * @param mapper Тот же sole-owner MMC5 до конца текущего borrowed ROM binding.
     * @details Сохраняет Name-latch, физические CHR refs, цифровой read-domain и
     * старый pending Draw последнего tile. Ранние planes при qualified elision
     * читает только цветной consumer; последние три пары нужны live PPU.
     * Первые tiles публикует существующим BgRun, с прежними logical credits. */
    void runMmc5InplaceBg(unsigned tiles,Mmc5& mapper) noexcept;
    /**
     * @brief Выполняет тот же packet/raster алгоритм через подготовленную аппаратную policy.
     * @param tiles Уже разрешённые 3..32 целых тайла; MMIO/Sprite0/IRQ ограничены caller.
     * @param mapper Живой sole-owner MMC5 текущей ROM.
     * @param plan Borrowed дескрипторы этого mapper до конца синхронного run.
     */
    void runMmc5PlanBg(unsigned tiles,Mmc5& mapper,Mmc5RasterPlan& plan) noexcept;
    /**
     * @brief Инстанцирует единый BG-run body с inline mapping вместо per-tile API вызовов.
     * @tparam Policy Аппаратная policy0..3, выбранная один раз на run.
     * @param tiles Число полных тайлов 3..32.
     * @param mapper Текущий mapper, чьи live latches обновляет план.
     * @param plan Дескрипторы текущей MMIO-эпохи; наружу не публикуются.
     */
    template<unsigned Policy> void runMmc5PlanBgPolicy(unsigned tiles,Mmc5& mapper,Mmc5RasterPlan& plan) noexcept;

    /** @brief Выполняет точный прежний literal shift/mix и загружает старый next до новых fetch. */
    void loadMmc5TrustedPlanes(Mmc5TrustedState& state) noexcept;

    /** @brief True только для MMC5 typed-/RD пути; не разрешает CPU/PPU debt. */
    bool mmc5Fetches() const noexcept;
    /** @brief Экранная колонка будущего BG fetch: visible2..33, prefetch0/1. */
    unsigned backgroundFetchTileX() const noexcept;
    friend class Machine;
    // Кодированный физический байт либо замороженный литерал. Копии не алиасят next-слот.
    enum Atom : unsigned { LowHigh,LowLow,LowNext,HighHigh,HighLow,HighNext };
    /**
     * @brief Вставляет следующие байты/идентичности фона в текущие сдвиговые регистры.
     * @details Частичные сдвиги и независимые high/low-ссылки не заменяются предположением о полном восьмибитном сдвиге.
     */
    void loadBackgroundAtoms() noexcept;
    /**
     * @brief Материализует ранее сохранённый байт паттерна по его виду и смещению.
     * @details Работает с буквальным значением, CHR-ROM либо защищённой CHR-RAM-идентичностью; недопустимая ссылка является ошибкой.
     * @param slot Индекс элемента текущей таблицы/буфера; диапазон задаётся этим типом.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t resolveAtom(unsigned slot) noexcept;
    /**
     * @brief Проверяет непрозрачность фоновой позиции, необходимой Sprite0.
     * @details Материализует лишь нужные плоскости; атрибут палитры не влияет на сам факт непрозрачности.
     * @param shift Смещение проверяемой фоновой точки 0..15 в текущей паре байтов; >=16 означает отсутствие непрозрачности.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool backgroundOpaque(unsigned shift) noexcept;
    /**
     * @brief Замораживает живые ссылки перед изменением CHR-RAM.
     * @details Старый байт сохраняется до записи, чтобы отложенный конвейер не увидел будущее значение.
     * @param offset Смещение внутри соответствующего физического диапазона.
     * @param oldValue Исторический CHR-RAM-байт до настоящей записи; COW фиксирует все ещё заимствованные версии.
     */
    void beforeChrRamWrite(unsigned offset,std::uint8_t oldValue) noexcept;
    /**
     * @brief Переводит отложенные ссылки конвейера в независимые значения перед сменой backing-памяти.
     * @details Порядок важен при загрузке, отказе загрузки, reset и CHR-RAM copy-on-write.
     */
    void materializeBackgroundRefs() noexcept;
    static constexpr unsigned width = 256, height = 240;
    /**
     * @brief Один выбранный OAM-слот: X/атрибуты/индекс, low/high-плоскости и признаки действительности. Порядок слотов сохраняет приоритет первого непрозрачного.
     * @ingroup nes_ppu
     */
    struct Sprite {
        std::uint8_t low = 0, high = 0, x = 0, attributes = 0, index = 0xFF;
        bool valid = false;
    };
    /**
     * @brief Проверяет текущую разрешённость фонового/спрайтового rendering.
     * @details Delayed-изменения PPUMASK материализуются исходной временной логикой.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool rendering() const noexcept { return renderingEnabled_; }
    /**
     * @brief Записывает предусмотренный текущим видеособытием участок вывода.
     * @details Сохраняет индексную плоскость, порядок перекрытия и ограничения видимых координат.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool renderLine() const noexcept { return scanline_ >= -1 && scanline_ < 240; }
    /**
     * @brief Публикует текущий адрес на шине PPU.
     * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void address(std::uint16_t value) noexcept;
    /**
     * @brief Читает байт соответствующего адресного пространства.
     * @details Декодирование и побочные эффекты принадлежат реализации шины. Доступы к MMIO нельзя переносить через наблюдаемую границу или заменять обычным чтением массива.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t read(std::uint16_t value) noexcept;
    /**
     * @brief Записывает байт через протокол устройства или шины.
     * @details Сохраняет порядок регистровых, RAM- и mapper-эффектов; запись не означает завершения физического вывода платы.
     * @param location Нормализованный физический индекс CIRAM с учётом nametable-алиасов, а не логический CPU-адрес.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void write(std::uint16_t location, std::uint8_t value) noexcept;
    /**
     * @brief Вычисляет физический индекс с правилами зеркалирования PPU.
     * @details Учитывает таблицы имён либо специальные зеркала палитры; сам адрес не выдаёт строб чтения.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    static unsigned paletteAddress(std::uint16_t value) noexcept;
    /**
     * @brief Продвигает coarse X и переключает горизонтальную таблицу имён при wrap.
     * @details Остальные поля loopy v сохраняются.
     */
    void incrementX() noexcept;
    /**
     * @brief Продвигает fine/coarse Y по правилам видимых и специальных строк PPU.
     * @details Строки 29/31 и переключение вертикальной таблицы имён не заменяются обычным сложением адреса.
     */
    void incrementY() noexcept;
    /**
     * @brief Продвигает v после обращения к PPUDATA по текущему режиму.
     * @details Правила rendering и обычного CPU-инкремента различаются; адресные эффекты сохраняются.
     */
    void incrementDataAddress() noexcept;
    /**
     * @brief Обновляет линию/задержку NMI из VBlank и PPUCTRL.
     * @details Сохраняет подавление и delayed-переходы около реального STATUS-чтения.
     */
    void updateNmi() noexcept;
    /**
     * @brief Читает CPU-видимый статус каналов и подтверждает предусмотренный frame-IRQ.
     * @details Семантика отличается от peekStatus: побочный эффект чтения должен попасть в соответствующий пакет.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t readStatus() noexcept;
    /** @brief Итог единственной проверки отложенного STATUS-чтения. */
    enum class StatusReadKind : std::uint8_t { Blocked, Stable, Eval };
    /** @brief Локальный результат запроса; evalUntil используется только при Eval. */
    struct StatusReadClassification {
        StatusReadKind kind;
        unsigned evalUntil;
    };
    /**
     * @brief Проверяет общие границы STATUS и необходимость отдельной OAM-оценки.
     * @details Метод не меняет PPU и не разрешает mutable-ветвь сам по себе.
     * @param dots Уже прошедшие, но ещё не доведённые точки PPU.
     */
    StatusReadClassification classifyDeferredStatus(unsigned dots) const noexcept;
    /**
     * @brief Проверяет право основного владельца не формировать ненаблюдаемые видеоданные.
     * @details Не расширяет поддерживаемые mapper и не относится к копиям, чужим PPU, временным захватам либо активному потребителю пакета.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool elideCaptureData() const noexcept;
    /** @brief Разрешает новый output-кодек только настоящему primary/current-packet владельцу без временного Capture.
     * @details Внешний predicate общий для event() всех TU; Machine в этом заголовке неполон.
     * @return true только для основного PPU с текущим пакетом и без Capture. */
    bool outputSpriteRows() const noexcept;
    /**
     * @brief Делегирует реальное CPU-чтение единственному полному PPU и сохраняет локальные эффекты сигналов.
     * @details Сначала закрывается текущий префикс; ответ соответствует точному post-clock срезу и не угадывается.
     * @param reg Регистр PPU: младшие три бита задают 0..7; readActual требует уже нормализованный номер, обычный MMIO допускает зеркала.
     * @param actual Байт, действительно возвращённый исходным PPU-чтением; используется для проверки, не повторяя чтение.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool readThinSignals(std::uint16_t reg,std::uint8_t actual) noexcept;
    /**
     * @brief Продвигает тонкий сигнальный PPU без формирования ненаблюдаемых цветовых данных.
     * @details Доступен только основному привязанному владельцу. Растр, NMI, loopy/OAM и mapper-провода остаются точными, а реальные чтения делегируются полному PPU.
     */
    void thinTick()noexcept;
    /**
     * @brief Продвигает тонкий сигнальный PPU без формирования ненаблюдаемых цветовых данных.
     * @details Доступен только основному привязанному владельцу. Растр, NMI, loopy/OAM и mapper-провода остаются точными, а реальные чтения делегируются полному PPU.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    void thinTickDots(unsigned dots)noexcept;
    /**
     * @brief Вставляет следующие байты/идентичности фона в текущие сдвиговые регистры.
     * @details Частичные сдвиги и независимые high/low-ссылки не заменяются предположением о полном восьмибитном сдвиге.
     */
    void loadBackground() noexcept;
    /**
     * @brief Выполняет предусмотренную текущей точкой загрузку фонового конвейера.
     * @details Не считает текущий адрес автоматически CHR: после переходов rendering может сохраняться иной шинный адрес.
     */
    void fetchBackground() noexcept;
    /**
     * @brief Обслуживает точную фазу фонового конвейера PPU.
     * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
     */
    template<unsigned Phase> inline void fetchBackgroundPhase() noexcept;
    /**
     * @brief Доводит текущие sprite-плоскости и счётчики X до нужной позиции.
     * @details Накопленные сдвиги и различающиеся моменты low/high-fetch учитываются до использования метаданных.
     */
    void synchronizeSprites() noexcept;
    /**
     * @brief Доводит аппаратный обход OAM до текущей растровой точки.
     * @details Сохраняет odd/even-защёлку, очистку secondary OAM, byte-позиции и диагональное переполнение, включая частичные срезы.
     * @param until Последняя включённая PPU-точка evaluation; ограничивается 256, частичная odd/even-защёлка сохраняется.
     */
    void synchronizeSpriteEvaluation(unsigned until) noexcept;
    /**
     * @brief Находит допустимую серию заведомо неподходящих OAM Y-пар.
     * @details Не перескакивает особые n=63/переполнение/частичную защёлку; последний прочитанный байт и secondary-позиция остаются точными.
     * @param n Начальный OAM-индекс 0..62; индекс 63 и оборачивание остаются скалярными.
     * @param limit Максимум завершённых Y-пар, доступных текущему вызову; последний/wrap-спрайт остаётся скалярным.
     * @param height Высота спрайта 8 или 16 PPU-строк, выбранная из текущего PPUCTRL.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned invalidYRun(unsigned n,unsigned limit,unsigned height) noexcept;
    /**
     * @brief Загружает спрайтовое состояние следующей строки в исходном порядке слотов.
     * @details Число видимых спрайтов не отменяет шинные обращения пустых слотов.
     */
    void fetchSprites() noexcept;
    /**
     * @brief Публикует метаданные конкретного sprite-слота после нужной фазы.
     * @details Не перепривязывает старую low/high-плоскость к новому flip, X либо номеру Sprite0.
     * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
     */
    void loadSpriteMetadata(unsigned slot) noexcept;
    /**
     * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
     * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
     * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
     * @param pattern Адрес паттерна текущей плоскости в момент fetch.
     */
    template<unsigned Phase, bool CachedPattern=false>
    inline void fetchSpritePhase(unsigned slot,std::uint16_t pattern=0) noexcept;
    /**
     * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
     * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    template<bool Clocked> void spriteFetchGroup(unsigned dots) noexcept;

    /**
     * @brief Заменяет полные sprite slots уже удерживаемого MMC5 GQ.
     * @param dots Исходный bounded prefix внутри 258..320, не более 321-dot.
     * @param mapper Заимствованный цифровой mapper текущей primary Machine.
     * @return false только до продвижения; true потребляет весь prefix старым
     * partial fallback и новыми canonical full8 slots без новых CPU clocks.
     * @pre Первый mapper M2 и CPU/APU время учтены прежним GQ replay.
     */
    bool tryMmc5HeldSpriteGroup(unsigned dots,Mmc5& mapper) noexcept;
    /**
     * @brief Исполняет ровно один canonical sprite slot1..7 без generic read chain.
     * @param mapper Тот же заимствованный mapper после admission внешнего group.
     * @return false без изменений при неполной геометрии/detector/backing.
     * @pre Exclusive held-group доказал owner/packet/rendering/no-observer domain;
     * pendingPixels равен 0, поэтому старый metadata loader не пишет packet.
     */
    bool tryMmc5HeldSpriteSlot(Mmc5& mapper) noexcept;

    /**
     * @brief Передаёт sprite prefix Flash-диспетчеру с прежней this+dots ABI.
     * @param dots Уже разрешённые PPU точки текущего sprite prefix.
     * @details Заимствует текущий mapper только до синхронного возврата. При
     * отсутствии владельца или отказе нового group исполняет старый полный
     * prefix; OFF также сохраняет прежний spriteFetchGroup. Не создаёт clocks.
     */
    void replayMmc5HeldSpriteGroup(unsigned dots) noexcept;


    /**
     * @brief Выполняет ограниченные фазы fetch-слотов спрайтов.
     * @details Dummy-адреса, 8x16 tile bit, flip и реальные low/high-стробы сохраняются даже для невидимого слота.
     * @param slot Текущий sprite-слот 0..7, обновляемый при переходе восьмиточечной группы.
     * @param cachedSlot Индекс слота локального pattern-кэша; не сохраняется через CPU/MMIO или смену OAM/CTRL.
     * @param pattern Локальный вычисленный pattern-адрес спрайта; действителен только для соответствующего cached-slot.
     */
    template<bool Clocked, unsigned Phase>
    inline void spriteFetchDot(unsigned& slot,unsigned& cachedSlot,std::uint16_t& pattern) noexcept;
    /**
     * @brief Обслуживает точную фазу фонового конвейера PPU.
     * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
     * @param dots Число уже разрешённых PPU-точек; caller ограничивает CPU/MMIO, delay и raster-границами.
     */
    template<bool Clocked, bool Visible> void backgroundGroup(unsigned dots) noexcept;
    /**
     * @brief Обслуживает точную фазу фонового конвейера PPU.
     * @details Порядок name/attribute/low/high и повторные адресные drive сохраняется; тактование mapper определяется параметрами варианта.
     */
    template<bool Clocked, bool Visible, unsigned Phase> inline void backgroundDot() noexcept;
    /**
     * @brief Строит текущий кэш выбора спрайтов для видимой строки.
     * @details Порядок OAM и окклюзия ранними спрайтами сохраняются; отдельная маска Sprite0 не разрешает менять выигравший пиксель.
     */
    void buildSpriteRow() noexcept;
    /**
     * @brief Доводит отложенное состояние спрайтов до текущего растрового среза.
     * @details Раздельные плоскости, flip и накопленные сдвиги относятся к моменту захвата, а не к новой метаинформации.
     */
    void materializeSprites() noexcept;
    /**
     * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
     * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
     */
    void renderPixel() noexcept;
    /**
     * @brief Обрабатывает текущий видимый пиксель с правилами масок, приоритета и Sprite0.
     * @details x255 и левый клип имеют особую семантику попадания; RGB-выход не должен изменять индексный результат или шинное время.
     */
    void visibleDot() noexcept;
    /**
     * @brief Обслуживает последовательность полных фоновых тайлов в текущей строке.
     * @details Временной PPU может удержать только живой хвост при доказанном отсутствии нужного Sprite0-наблюдения; полный вывод сохраняет фактический выбор пикселей.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     */
    void backgroundRun(unsigned tiles) noexcept;
    // Стековый sink одной полной observer-free fetch-группы. Сырые слова
    // избегают обнуления неиспользуемого event-хранилища; существуют лишь count инициализированных записей.
    /**
     * @brief Стековый ограниченный sink временной observer-free группы; видимы только count полностью инициализированных event-слов.
     * @ingroup nes_ppu
     */
    struct Capture {
        std::uint32_t words[24];
        unsigned count=0;
        bool overflow=false;
    };
    /**
     * @brief RAII-подмена временного capture с восстановлением прежнего указателя; не передаёт полномочия основного PPU.
     * @ingroup nes_ppu
     */
    struct CaptureScope {
        Capture*& target;
        Capture* previous;
        /**
         * @brief Временно подменяет указатель захвата с восстановлением прежнего значения.
         * @details RAII-граница не превращает временный или чужой PPU в основного владельца, имеющего право элиминировать данные.
         * @param slot Ссылка на указатель текущего capture-sink; RAII временно заменяет и затем восстанавливает его.
         * @param local Стековое capture-хранилище текущей группы; переживает CaptureScope и не публикуется асинхронно.
         */
        CaptureScope(Capture*& slot,Capture& local) noexcept:target(slot),previous(slot){target=&local;}
        /**
         * @brief Восстанавливает прежний указатель временного capture.
         * @details Объект и захват не должны переживать внешний исключительный PPU-вызов.
         */
        ~CaptureScope(){target=previous;}
        /**
         * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
         * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
         */
        CaptureScope(const CaptureScope&)=delete;
        /**
         * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
         * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
         */
        CaptureScope& operator=(const CaptureScope&)=delete;
    };
    /**
     * @brief Передаёт текущее событие временному захвату PPU.
     * @details Захват не является постоянным владельцем и не допускает элиминации данных по политике чужого основного PPU.
     * @param sprite Различает sprite/background-траекторию выборки; не заменяет проверку сигналов маппера.
     */
    void emitCapture(const Capture&,bool sprite) noexcept;
    /**
     * @brief Вычисляет адрес плоскости спрайта по текущей высоте, строке, тайлу и flip.
     * @details В 8x16 режиме младший бит tile выбирает таблицу; это влияет и на A12.
     * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t spritePattern(unsigned slot) const noexcept;
    /**
     * @brief Разворачивает порядок битов для горизонтального отражения либо индексирования таблицы.
     * @details Результат применяется к той плоскости и тому моменту fetch, для которых он был рассчитан.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    static std::uint8_t reverse(std::uint8_t value) noexcept;
    /**
     * @brief Добавляет или извлекает компактное видеособытие текущего пакета.
     * @details Контролирует формат и ограниченную ёмкость; переполнение является ошибкой, а не разрешением удалить запись.
     * @param op Уже декодированная CPU-операция либо вид упакованного события.
     * @param a Первый восьмибитный аргумент tag видеособытия; смысл определяется op.
     * @param b Второй восьмибитный аргумент tag видеособытия; смысл определяется op.
     * @param c Третий восьмибитный аргумент tag видеособытия; смысл определяется op.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void event(VideoOp op, unsigned a=0, unsigned b=0, unsigned c=0, std::uint32_t value=0) noexcept {
        if(capture_){
            if(capture_->count>=12){capture_->overflow=true;return;}
            auto* words=capture_->words+capture_->count*2;
            words[0]=static_cast<std::uint32_t>(op)|((a&255U)<<8)|((b&255U)<<16)|((c&255U)<<24);
            words[1]=value;++capture_->count;return;
        }
        if((op==VideoOp::SpriteMeta||op==VideoOp::SpriteLow||op==VideoOp::SpriteHigh)&&outputSpriteRows())return;
        if (packet_) packet_->add(op,a,b,c,value);
    }
    Machine& bus_;
    VideoPacket* packet_ = nullptr;
    Capture* capture_ = nullptr;
    bool trackBusCycles_ = false, trackBusAddresses_ = false;
    std::array<std::uint8_t,32> palette_{}, secondary_{};
    std::array<std::uint8_t,256> oam_{};
    std::array<std::uint8_t,8> secondaryIndex_{};
    std::array<Sprite,8> sprites_{};
    std::array<std::uint8_t,256> spriteRow_{};
    std::uint16_t spriteCacheClocks_ = 0;
    bool spriteCacheValid_ = false;
    bool spriteZeroRow_ = false;
    std::array<std::uint8_t,32> spriteZeroMask_{};
    std::uint8_t control_=0,mask_=0,status_=0,oamAddress_=0,openBus_=0,readBuffer_=0,fineX_=0;
    std::uint16_t v_=0,t_=0,busAddress_=0;
    /**
     * @brief Потребляет подготовленный E_EV sink горизонтального копирования на 257.
     * @details Active путь вызывает после BG-load и перед sprite-fetch, сохраняя
     * исходный порядок шины. Forced-blank путь вызывает после удержанного address(v).
     */
    void finishHorizontalScrollSignal() noexcept {
        if(scrollSignal_)v_=(v_&0x7BE0U)|(t_&0x041FU);
        scrollSignal_=false;
    }
    bool coarseXSignal_=false,scrollSignal_=false; // Защёлки F_TB и E_EV до последнего sink.
    bool writeToggle_=false,renderingEnabled_=false,renderingNext_=false;
    std::uint8_t renderDelay_=0;
    std::array<std::uint32_t,6> backgroundAtoms_{};
    std::uint8_t pendingBackgroundShifts_=0,nextTile_=0,nextAttribute_=0;
    unsigned pendingPixels_=0,pendingPixelX_=0;
    bool pendingFill_=false;
    std::uint8_t pendingPaletteIndex_=0;
    std::uint8_t evaluateN_=0,evaluateM_=0,secondaryPosition_=0,evaluateLatch_=0xFF,overflowFetches_=0;
    bool evaluationDone_=false;
    std::uint16_t evaluationNextDot_=257;
    std::int16_t scanline_=-1;
    std::uint16_t dot_=0;
    std::uint64_t frames_=0;
    bool pal_=false,odd_=false,frameReady_=false,nmiLine_=false,nmiPending_=false,suppressVblank_=false;
    std::uint8_t nmiDelay_=0;
    // Держать горячую row-маску на малых RV32-смещениях; динамические buckets следуют ниже.
    std::uint32_t oamYLow_=0,oamYHigh_=0;
    std::int16_t oamYRow_=-1;
    std::uint8_t oamYHeight_=0;
    bool oamYDirty_=true;
    std::array<std::array<std::uint32_t,2>,256> oamYBuckets_{};
    /** @brief Постоянный bounded кэш metadata; не ROM/кадр и не опубликованное состояние. */
    Mmc5RasterPlan rasterPlan_;
    /** @brief Ноль запрещает чтение старого либо ещё не подготовленного плана. */
    std::uint32_t rasterPlanVersion_ = 0;
    /** @brief Идентичность CIRAM нужна даже при четырёх fill/zero NT источниках. */
    const std::uint8_t* rasterPlanCiram_ = nullptr;
    /** @brief Весь PPUCTRL проверяется отдельно от наблюдаемых mapper адресов 2000. */
    std::uint8_t rasterPlanControl_ = 0;
    Mmc5LazyState mmc5Lazy_{}; ///< Сохранённое поле ABI; текущий профиль долг не накапливает.
    Mmc5LazyCounts mmc5LazyCounts_{}; ///< Сохранённый layout прежних счётчиков C1; текущий путь их не накапливает.
};
} // пространство имён nes::core
