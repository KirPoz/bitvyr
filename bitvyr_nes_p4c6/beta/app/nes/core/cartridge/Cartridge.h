/**
 * @file Cartridge.h
 * @brief Аренда неизменяемого ROM, собственная RAM и физические PRG/CHR-представления.
 * @ingroup nes_cartridge
 */
#pragma once

#include "Legacy160.h"
#include "Sunsoft5BAudio.h"
#include "Vrc7Audio.h"
#include "Vrc7WriteTape.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::core {
class Mmc5;
class Mmc5Audio;

enum class Mirroring : std::uint8_t { Horizontal, Vertical, Single0, Single1, FourScreen };
enum class LoadResult : std::uint8_t {
    Ok, InvalidHeader, Truncated, UnsupportedMapper, UnsupportedFormat, MissingMapperStorage,
};

// Вызывающий сохраняет неизменяемые .nes-байты всю загруженную сессию.
// RAM картриджа и состояние mapper принадлежат объекту; без SDK и аллокаций.
/**
 * @brief Заимствованный образ iNES и собственное состояние банков/RAM/IRQ. Копирование запрещено; ROM не освобождается этим объектом.
 * @ingroup nes_cartridge
 */
class Cartridge final {
public:
    /** @brief Возможность timing-video модели mapper; не аппаратная квалификация ROM/платы. */
    static constexpr bool supportsTimingVideoMapper(std::uint16_t mapper) noexcept {
        switch (mapper) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 7: case 69: case 71: case 85: case 160: return true;
        default: return false;
        }
    }
    /** @brief Наличие expansion-DAC модели; не доказательство физического звука или точности FM. */
    static constexpr bool hasExpansionAudioMapper(std::uint16_t mapper) noexcept {
        return mapper <= 7 ? mapper == 5 : (mapper == 69 || mapper == 85);
    }
    /**
     * @brief Создаёт картридж без передачи владения ROM объекту.
     * @details Образ подключается отдельным load; изменение отображений не является копированием исходных байтов.
     */
    Cartridge() noexcept = default;
    /** @brief Связывает внешние MMC5-объекты на срок сессии после завершения Worker. Не выделяет память. */
    void bindMmc5State(Mmc5* digital, Mmc5Audio* audio) noexcept { mmc5_=digital; mmc5Audio_=audio; }
    /** @brief Передаёт настоящий CPU-read PPU-регистра MMC5; остальные чтения проходят cpuRead. */
    void observeMmc5Read(std::uint16_t address, std::uint8_t value) noexcept;
    /** @brief Передаёт MMC5 записи PPUCTRL/PPUMASK/OAMDMA с настоящим CPU-адресом. */
    void observeMmc5Write(std::uint16_t address, std::uint8_t value) noexcept;
    /** @brief Возвращает подписанный расширенный DAC до FIR, без громкости платы. */
    std::int32_t expansionSample() const noexcept;
    /**
     * @brief Возвращает расширенный DAC выбранного звукового профиля до общего FIR.
     * @return Raw DAC точного профиля либо ограниченное portable-усреднение;
     * без подключённого MMC5 возвращается 0. Единицы совпадают с AudioPacket.
     * @details Единственный Worker вызывает provider синхронно после настоящего
     * clock/read; метод не тактирует устройство, не меняет IRQ/PCM-read latch и
     * не продлевает заимствование. PRIVATE-флаг меняет только внешнее тело,
     * raw expansionSample и layout картриджа остаются общими для consumers.
     */
    /** @brief Pure actual capability после принятого load; mapper85sub1 не имеет FM. */
    [[nodiscard]] bool hasVrc7Fm()const noexcept{return prg_&&mapper_==85U&&(submapper_==0U||submapper_==2U);}
    /** @brief Pure owner-local режим; true запрещает тихий возврат к старому localFM. */
    [[nodiscard]] bool externalVrc7Enabled()const noexcept{return externalVrc7_;}
    /** @brief Первый sticky capture-отказ; getter не тактирует, не снимает и не сбрасывает fault. */
    [[nodiscard]] Vrc7WriteTape::Fault externalVrc7Fault()const noexcept{return vrc7TraceFault_;}
    /**
     * @brief Возвращает текущий DAC выбранного portable-провайдера без продвижения clocks.
     * @details Внешний FM85 и submapper1 возвращают ноль: FM85 Main восстанавливает из tape.
     * Для остальных поддержанных mapper уровень используется общими AudioPacket/DMC путями.
     */
    std::int32_t selectedExpansionSample() const noexcept;
    /**
     * @brief Ограничивает только пакетирование звукового выхода текущего профиля.
     * @return Положительное расстояние в CPU-тактах до возможной смены selected
     * DAC; при отсутствии MMC5 —1 для безопасного прежнего однотактового пути.
     * @details Это не CPU/PPU/IRQ/DMC quiet grant: существующие гостевые горизонты
     * остаются обязательными. Состояние генераторов и raw deadlines не меняются.
     */
    std::uint32_t selectedExpansionOutputCpuCycles() const noexcept;

    /**
     * @brief Завершает привязки и освобождает только собственное состояние объекта.
     * @details ROM и арены вызывающего не становятся собственностью машины; активных пользователей необходимо остановить до разрушения.
     */
    ~Cartridge() noexcept { detachCpuPrgTail(); }
    // Кэшированные CHR-RAM-страницы указывают внутрь владельца. Его перенос/копирование
    // инвалидировал бы указатели; сессия Machine имеет одного стабильного владельца.
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    Cartridge(const Cartridge&) = delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    Cartridge& operator=(const Cartridge&) = delete;
    /**
     * @brief Проверяет и подключает заимствованный образ ROM.
     * @details Владелец образа удерживает неизменённые байты на весь срок использования. Отказ загрузки нельзя считать сохранением всех старых указателей картриджа; ранее выданные ссылки и эпохи обрабатываются установленным жизненным циклом.
     * @param image Неизменяемый заимствованный образ ROM; вызывающий сохраняет его срок жизни.
     * @param size Размер переданного диапазона в байтах.
     * @return Код проверки iNES/поддержки mapper; только Ok означает принятую загрузку, отказ имеет описанные эффекты инвалидирования.
     */
    LoadResult load(const std::uint8_t* image, std::size_t size) noexcept;
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     */
    void reset() noexcept;
    /**
     * @brief Читает отображённый байт картриджа с учётом текущей открытой шины.
     * @details Mapper-режим, защита RAM и отсутствие источника обрабатываются отдельно от продвижения clocks.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t cpuRead(std::uint16_t address, std::uint8_t openBus = 0) const noexcept;
    /**
     * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
     * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     */
    void cpuWrite(std::uint16_t address, std::uint8_t value, std::uint64_t cycle) noexcept;
    /**
     * @brief Обслуживает отображённый доступ к памяти CHR картриджа.
     * @details CHR-ROM не становится изменяемой RAM; банковая логика и стробовые защёлки выполняются на своих исходных границах.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t chrRead(std::uint16_t address) const noexcept;
    /**
     * @brief Обслуживает отображённый доступ к памяти CHR картриджа.
     * @details CHR-ROM не становится изменяемой RAM; банковая логика и стробовые защёлки выполняются на своих исходных границах.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void chrWrite(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Продвигает временное состояние картриджа на одну точку PPU.
     * @details Сохраняет возраст A12 и фильтрацию фронтов MMC3, даже когда видеоданные не выводятся.
     */
    void ppuCycle() noexcept;
    /**
     * @brief Публикует текущий адрес на шине PPU.
     * @details Отдельное выставление адреса важно для A12 и защёлок mapper; повторное выставление того же адреса может быть частью исходной фазовой последовательности.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     */
    void ppuAddress(std::uint16_t address) noexcept;
    /**
     * @brief Продвигает CPU-зависимые счётчики mapper.
     * @details Эти IRQ-источники независимы от PPU A12 и не должны теряться при отложенном PPU.
     */
    void cpuCycle() noexcept {
        // Старые 0..7 сохраняют два сравнения и не вызывают новый expansion clock.
        if (mapper_ < 69) { if (mapper_ == 5) clockMmc5Cpu(1); }
        else if (mapper_ == 160) legacy_.cpuCycle();
        else if (mapper_ == 69 || mapper_ == 85) clockExpansionCpu(1);
    }
    /**
     * @brief Ограничивает CPU-интервал до возможного нового mapper-IRQ.
     * @details Горизонт не разрешает удалять реальные чтения или записи.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned cpuIrqQuietCycles() const noexcept { return mapper_ == 160 ? legacy_.cpuIrqQuietCycles() : ((mapper_ == 69 || mapper_ == 85) ? expansionIrqQuietCycles() : ~0U); }
    /**
     * @brief Ограничивает CPU-интервал до возможного нового mapper-IRQ.
     * @details Горизонт не разрешает удалять реальные чтения или записи.
     * @param count Длина допустимого интервала в гостевых CPU-тактах.
     */
    void cpuCyclesQuiet(unsigned count) noexcept { if (mapper_ == 160) legacy_.cpuCyclesQuiet(count); else if (mapper_ == 5) clockMmc5Cpu(count); else if (mapper_ == 69 || mapper_ == 85) clockExpansionCpu(count); }
    /**
     * @brief Обслуживает регистровую запись CPU и её отдельный mapper-строб.
     * @details Для некоторых Legacy-режимов запись RAM также является временным событием; такие эффекты нельзя терять в quiet-пути.
     */
    void cpuWriteStrobe() noexcept { if (mapper_ == 160) legacy_.cpuWriteStrobe(); }
    /**
     * @brief Проверяет, допустимо ли не выдавать отдельный CPU write-strobe в текущем режиме.
     * @details Предикат ограничивает оптимизацию записей; состояние направления/источника IRQ должно оставаться тем же в интервале.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool cpuWriteStrobeIsNoop() const noexcept {
        return mapper_ != 160 || legacy_.cpuWriteStrobeIsNoop();
    }
    /**
     * @brief Выполняет реальный строб чтения PPU без повторного чтения ненужного значения.
     * @details Не устраняет FD/FE-защёлки и не заменяет обязательное выставление сохранённого адреса.
     */
    void ppuReadStrobe() noexcept { if (mapper_ == 160 && legacy_.ppuRead()) refreshChrPages(); }
    /**
     * @brief Возвращает текущий электрический уровень IRQ устройства.
     * @details Проверка не подтверждает IRQ и не обслуживает его в CPU.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool irqLine() const noexcept { return mapper_ == 5 ? mmc5Irq() : (mapper_ == 160 ? legacy_.irqLine() : irqPending_); }
    // Консервативный горизонт без нового PPU IRQ для произвольных непрерывных точек.
    // MMC3 может иметь один уже квалифицированный подъём, затем требует восемь новых low-
    // тактов на каждый следующий квалифицированный подъём. Граница фронтов — 1+ceil(dots/8).
    /**
     * @brief Проверяет отсутствие нового наблюдаемого PPU-origin IRQ в заданном интервале.
     * @details Учитывает текущую фазу/A12/счётчик; не утверждает отсутствие CPU-clocked IRQ или иных устройств.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned ppuIrqQuietDotsAny() const noexcept {
        if (mapper_ == 160) return legacy_.ppuIrqQuietDotsAny();
        if (mapper_ != 4 || !irqEnabled_ || irqPending_) return 65535;
        const unsigned untilAssert = irqCounter_ != 0 && !irqReload_
            ? unsigned(irqCounter_) : unsigned(irqLatch_) + 1U;
        if (untilAssert <= 2) return 0;
        const unsigned dots = 8U * (untilAssert - 2U);
        return dots;
    }
    // Предикат действителен только в видимой phase0 со стабильным PPUCTRL,
    // непрерывными background-выборками, <=248 точками и без CPU MMIO внутри.
    // Гарантирует отсутствие нового PPU IRQ-фронта; CPU-тактируемые события учитываются отдельно.
    /**
     * @brief Проверяет отсутствие нового наблюдаемого PPU-origin IRQ в заданном интервале.
     * @details Учитывает текущую фазу/A12/счётчик; не утверждает отсутствие CPU-clocked IRQ или иных устройств.
     * @param dots Число уже прошедших точек PPU.
     * @param backgroundHigh Выбор верхней pattern-таблицы фона по PPUCTRL; влияет на настоящую траекторию A12.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool ppuIrqQuietFor(unsigned dots, bool backgroundHigh) const noexcept {
        if (mapper_ == 160) return legacy_.ppuIrqQuietFor(dots, backgroundHigh);
        if (mapper_ != 4 || !dots || !backgroundHigh || !irqEnabled_ || irqPending_) return true;
        // Phase0 выставляет низкий NT. Первый pattern-адрес следует через четыре такта;
        // у следующих тайлов только три низких такта, меньше восьмиточечного фильтра.
        // Поэтому не более первого подъёма может тактировать это окно.
        if (a12_ || a12LowDots_ < 4) return true;
        const auto next = irqCounter_ == 0 || irqReload_ ? irqLatch_ : unsigned(irqCounter_ - 1U);
        return next != 0;
    }
    // Видимые BG-фазы до dot249: произвольный неполный первый тайл может
    // дать не более одного квалифицированного подъёма A12 для обеих pattern-таблиц.
    // CPU $2006 может оставить +8 carry в phase6 и при BG0; не исключать BG0.
    /**
     * @brief Проверяет отсутствие нового наблюдаемого PPU-origin IRQ в заданном интервале.
     * @details Учитывает текущую фазу/A12/счётчик; не утверждает отсутствие CPU-clocked IRQ или иных устройств.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool ppuIrqQuietVisibleAny() const noexcept {
        if(mapper_!=4)return false;
        if(!irqEnabled_||irqPending_)return true;
        const unsigned next=irqCounter_==0||irqReload_?irqLatch_:unsigned(irqCounter_-1U);
        return next!=0;
    }
    /**
     * @brief Возвращает текущую характеристику подключённого картриджа.
     * @details Признак PAL из заголовка не подменяет явно выбранный режим Machine; loaded описывает действительность backing текущего картриджа.
     * @return Вычисленное значение или состояние типа Mirroring согласно описанному контракту.
     */
    Mirroring mirroring() const noexcept;
    /**
     * @brief Возвращает текущую характеристику подключённого картриджа.
     * @details Признак PAL из заголовка не подменяет явно выбранный режим Machine; loaded описывает действительность backing текущего картриджа.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t mapper() const noexcept { return mapper_; }
    /**
     * @brief Возвращает текущую характеристику подключённого картриджа.
     * @details Признак PAL из заголовка не подменяет явно выбранный режим Machine; loaded описывает действительность backing текущего картриджа.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool loaded() const noexcept { return prg_ != nullptr; }
    // Поддерживаемый mapper160 имеет IRQ от чтений и CHR-защёлки. IRQ mapper4
    // только от A12 управляются адресными/тактовыми линиями, а не этим read-стробом.
    /**
     * @brief Сообщает о mapper-эффектах реального чтения PPU.
     * @details При true нельзя заранее фиксировать все CHR-страницы на произвольный интервал.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool hasPpuReadSideEffects() const noexcept { return mapper_ == 160 || mapper_ == 5; }

    /** @brief Аппаратные свойства, требуемые повторяемым опросом STATUS без CPU-записей. */
    enum StatusPollCapability : unsigned {
        PureCpuPrgRead = 1U, CpuWritesOnlyPrgMapping = 2U,
        NoPpuReadLatch = 4U, NoCpuClockEvent = 8U, NoCpuWriteStrobe = 16U
    };
    /**
     * @brief Возвращает проверенные свойства модели платы, а не признак конкретной ROM.
     * @details В этих моделях чтение PRG чистое, а его mapping меняется только CPU-записью;
     * PPU-read и CPU-clock не переключают PRG и не создают отдельное событие картриджа.
     * Внутри допускаемого тела нет CPU-записей. Legacy160 и неизвестная плата отказываются.
     * @return Маска аппаратных свойств либо 0 при незагруженном/неподдержанном картридже.
     */
    unsigned statusPollCapabilities() const noexcept {
        if (!prg_) return 0;
        switch (mapper_) {
        case 0: case 1: case 2: case 3: case 7: case 71:
            return PureCpuPrgRead | CpuWritesOnlyPrgMapping | NoPpuReadLatch |
                   NoCpuClockEvent | NoCpuWriteStrobe;
        default: return 0;
        }
    }
    /**
     * @brief Возвращает текущую характеристику подключённого картриджа.
     * @details Признак PAL из заголовка не подменяет явно выбранный режим Machine; loaded описывает действительность backing текущего картриджа.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool palHint() const noexcept { return palHint_; }
    /**
     * @brief Проверяет поддерживаемую комбинацию заголовка и Legacy-подварианта.
     * @details Не является универсальным допуском любого mapper160-образа.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool legacyHeader() const noexcept { return legacyHeader_; }
    /**
     * @brief Возвращает состояние или результат текущей привязки.
     * @details Ошибка, наличие хранилища и готовность изображения различаются; эти признаки сами по себе не выполняют повтор или аппаратное ожидание.
     * @param result Код результата разбора iNES/допуска mapper; преобразуется в статическое диагностическое сообщение.
     * @return Указатель на статическую строку; память не освобождается вызывающим.
     */
    const char* errorText(LoadResult result) const noexcept;
    /**
     * @brief Возвращает заимствованное представление RAM картриджа.
     * @details Данные меняются при записях/загрузке; это не безусловно безопасный многопоточный снимок.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    /** @brief Заимствованный R/W диапазон $6000–$7FFF только одного quiet-вызова; полей Cartridge не добавляет. */
    struct QuietWram6000 {
        std::uint8_t* data = nullptr;
        static constexpr unsigned base = 0x6000U, bytes = 8192U;
        /** @brief Проверяет полный CPU-адрес до вычисления физического смещения 0..8191; $5Fxx всегда вне диапазона. */
        static constexpr bool contains(unsigned address) noexcept { return address-base<bytes; }
    };
    /** @brief Даёт полный физический WRAM на $6000 с действующим разрешением записи.
     * @details Для mapper 5 учитывает CE и регистры защиты. Запись mapper завершает аренду.
     */
    [[nodiscard]] QuietWram6000 quietWritableWram6000() noexcept;

    /**
     * @brief Даёт заимствованный mutable backing обычной PRG-RAM размером8192 байт.
     * @details Это не отображённое окно$6000 и не expansion WRAM69/85. Указатель не
     * отменяет банковку, защиту записи и исключительное владение изменяемым Cartridge.
     */
    std::uint8_t* ram() noexcept { return prgRam_.data(); }

private:
    friend class Machine;
    /** @brief Возвращает байт регистра AxROM с учётом конфликта на текущем PRG-отображении. */
    std::uint8_t axromLatchValue(std::uint16_t address, std::uint8_t value) const noexcept {
        return submapper_ == 2 ? static_cast<std::uint8_t>(value & cpuRead(address)) : value;
    }
    /** @brief Применяет защёлку AxROM после разрешения конфликта на шине. */
    void writeAxromLatch(std::uint8_t value) noexcept {
        const bool prgChanged = ((bank_ ^ value) & 7U) != 0;
        bank_ = value;
        if (prgChanged) refreshPrgPages();
    }
    /** @brief Декодирует FME-7 command/data; CPU-такты уже принадлежат общей Machine. */
    void writeFme7(std::uint16_t address, std::uint8_t value) noexcept;
    /** @brief Декодирует VRC7 по A3/A4 выбранной платы; audio получает canonical9010/9030. */
    void writeVrc7(std::uint16_t address,std::uint8_t value,std::uint64_t cycle)noexcept;
    /** @brief Cold arm только после fullreset7 с portableRemaining57; joined повтор phase не меняет. */
    bool armExternalVrc7(std::uint64_t cycle)noexcept;
    /** @brief Заимствует единственный Recording writer текущего owned-frame без async callback. */
    bool bindVrc7Writer(Vrc7WriteTape*)noexcept;
    /** @brief Единственный seal после metadata/audioCut; успешный seal отцепляет только writer. */
    bool sealVrc7Writer(Vrc7WriteTape*,std::uint64_t end)noexcept;
    /** @brief Сохраняет canonical write на actualM2, включая same-cycle и RMW duplicates. */
    void recordVrc7Write(std::uint64_t cycle,std::uint16_t address,std::uint8_t value)noexcept;
    /** @brief Сохраняет первый отказ, никогда не разрешает публикацию частичного trace. */
    void failVrc7Trace(Vrc7WriteTape::Fault fault)noexcept{if(vrc7TraceFault_==Vrc7WriteTape::Fault::None)vrc7TraceFault_=fault;}
    /** @brief Продвигает digital IRQ и expansion DAC ровно на count CPU M2 без bus accesses. */
    void clockExpansionCpu(unsigned count) noexcept;
    /** @brief Число CPU-тактов строго до следующего IRQ; фронт остаётся scalar boundary. */
    unsigned expansionIrqQuietCycles() const noexcept;

    /** @brief Выполняет mapper5 CPU-read, включая PCM и подтверждение IRQ. */
    std::uint8_t readMmc5Cpu(std::uint16_t address,std::uint8_t openBus) const noexcept;
    /** @brief Чистый digital certificate phi0: без pending IRQ/scanline/idle перехода внутри H2. */
    bool canCompleteMmc5H2() const noexcept;
    /** @brief Передаёт один ограниченный M2-интервал цифровому MMC5 и его звуку. */
    void clockMmc5Cpu(unsigned count) noexcept;
    /** @brief Объединяет scanline IRQ и PCM IRQ без подтверждения. */
    bool mmc5Irq() const noexcept;
    Mmc5* mmc5_ = nullptr;
    Mmc5Audio* mmc5Audio_ = nullptr;

    // Описание источника только на время вызова; не кэшируется через CPU-записи/такты.
    enum class Cpu6000Kind : std::uint8_t { OpenBus, Ram, Rom, LegacyPrg };
    /**
     * @brief Описание источника окна CPU $6000: открытая шина, внутренняя RAM либо Legacy PRG. Указатель не является копией изменяемых байтов.
     * @ingroup nes_cartridge
     */
    struct Cpu6000Source { const std::uint8_t* data; Cpu6000Kind kind; };
    /**
     * @brief Один раз выбирает описание источника для окна $6000.
     * @details Описание действительно пока тот же владелец не меняет регистры картриджа; это не право кэшировать изменяемые байты.
     * @return Вычисленное значение или состояние типа Cpu6000Source согласно описанному контракту.
     */
    Cpu6000Source selectCpu6000Source() const noexcept;
    /**
     * @brief Читает байт по уже выбранному описанию источника $6000.
     * @details Не выполняет повторный выбор mapper и сохраняет исходную семантику RAM, ROM либо открытой шины.
     * @param offset Смещение внутри соответствующего физического диапазона.
     * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t readCpu6000Source(const Cpu6000Source&, std::uint16_t offset,
                                 std::uint8_t openBus) const noexcept;
    // Исключительный спокойный вызывающий Machine предоставляет отдельные изменяемые 8 KiB.
    // Общая политика CPU-чтения; без тактов, стробов, записей, полей или передачи владения.
    /**
     * @brief Копирует окно $6000-$7FFF выбранного картриджа в память вызывающего.
     * @details Выбор источника соответствует обычному cpuRead; не должен сбрасывать IRQ или изменять mapper.
     * @param destination Приёмный диапазон вызывающего, не перекрывающий действующие заимствованные источники.
     * @param openBus Предшествующий байт открытой CPU-шины для неуправляемых или частично управляемых битов чтения.
     */
    void copyCpu6000Window(std::uint8_t* destination, std::uint8_t openBus) const noexcept;

    /**
     * @brief Продвигает только уже доказанные стабильные шинные сигналы PPU.
     * @details Не заменяет данные, которые наблюдает CPU, и сохраняет все необходимые clock/address переходы.
     * @param dots Число уже прошедших точек PPU.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param clocked Нужно ли сохранять каждый соответствующий mapper-clock.
     * @param addressed Нужно ли выставлять исходные адресные сигналы PPU.
     */
    void advanceStablePpuSignals(unsigned dots, std::uint16_t address,
                                 bool clocked, bool addressed) noexcept {
        if (!dots) return;
        // Сохранить первый физический переход clock/address, включая
        // возможный MMC3/Legacy IRQ-фронт. Повторные одинаковые адреса — неподвижная
        // точка; продолжает меняться лишь насыщаемый возраст low-A12.
        if (clocked) ppuCycle();
        if (addressed) ppuAddress(address);
        if (clocked && !a12_ && dots > 1) {
            const unsigned age = unsigned(a12LowDots_) + dots - 1U;
            a12LowDots_ = static_cast<std::uint16_t>(age < 65535U ? age : 65535U);
        }
    }
    /**
     * @brief Выполняет Legacy-доступ к фоновым или sprite-плоскостям с исходными защёлками.
     * @details Возвращаемый старый байт фиксируется до FD/FE-строба, который способен сменить страницы для следующего доступа.
     * @param pattern Адрес паттерна текущей плоскости в момент fetch.
     * @return Вычисленное значение или состояние типа std::uint16_t согласно описанному контракту.
     */
    std::uint16_t readLegacyBackgroundPlanes(std::uint16_t pattern) noexcept {
        // Оба байта используют прежнее отображение. FD/FE-защёлка срабатывает только ПОСЛЕ high.
        const auto low = chrPages_[(pattern >> 10) & 7U][pattern & 1023U];
        const auto highAddress = static_cast<std::uint16_t>(pattern + 8U);
        const auto high = chrPages_[(highAddress >> 10) & 7U][highAddress & 1023U];
        if (legacy_.backgroundTile(highAddress)) refreshChrPages();
        return static_cast<std::uint16_t>(low | (std::uint16_t(high) << 8));
    }
    // Точный восьмиточечный переход фоновых сигналов phase0. Только
    // конкретный мост Machine вызывает его для mapper4 без CPU-наблюдателя
    // внутри тайла. Чтения памяти остаются в исходном PPU-порядке.
    /**
     * @brief Применяет доказанную композицию MMC3-сигналов для тайла либо фонового span.
     * @details Возраст A12, счётчик, reload и pending должны совпасть со всеми исходными точками. Это не пропуск IRQ-наблюдения.
     * @param backgroundHigh Выбор верхней pattern-таблицы фона по PPUCTRL; влияет на настоящую траекторию A12.
     */
    void advanceMmc3TileSignals(bool backgroundHigh) noexcept {
        if (backgroundHigh) {
            if (!a12_ && a12LowDots_ >= 4) {
                if (irqCounter_ == 0 || irqReload_) irqCounter_ = irqLatch_;
                else --irqCounter_;
                irqReload_ = false;
                if (irqCounter_ == 0 && irqEnabled_) irqPending_ = true;
            }
            a12_ = true;
            a12LowDots_ = 0;
        } else {
            const unsigned lowDots = a12_ ? 7U : unsigned(a12LowDots_) + 8U;
            a12LowDots_ = static_cast<std::uint16_t>(lowDots > 0xFFFFU ? 0xFFFFU : lowDots);
            a12_ = false;
        }
    }
    // Точное повторение advanceMmc3TileSignals для проверенного вызывающим
    // phase0-интервала 0..32 тайлов. Первый high-тайл достигает неподвижной точки;
    // low-интервал лишь насыщает возраст A12 и не может тактировать IRQ.
    /**
     * @brief Применяет доказанную композицию MMC3-сигналов для тайла либо фонового span.
     * @details Возраст A12, счётчик, reload и pending должны совпасть со всеми исходными точками. Это не пропуск IRQ-наблюдения.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param backgroundHigh Выбор верхней pattern-таблицы фона по PPUCTRL; влияет на настоящую траекторию A12.
     */
    void advanceMmc3BgSpan(unsigned tiles,bool backgroundHigh) noexcept {
        if(!tiles)return;
        if(backgroundHigh){advanceMmc3TileSignals(true);return;}
        const unsigned dots=tiles*8U;
        const unsigned age=a12_?dots-1U:unsigned(a12LowDots_)+dots;
        a12LowDots_=static_cast<std::uint16_t>(age>65535U?65535U:age);
        a12_=false;
    }
    // Только доверенный мост Machine вызывает это после проверки loaded().
    /**
     * @brief Разрешает опубликованную физическую ссылку на байт либо тайл.
     * @details ROM/RAM/буквальные виды и границы проверяются до чтения; текущая банковая карта производителя не заменяет сохранённую идентичность.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t cachedChrByte(std::uint16_t address) const noexcept {
        return chrPages_[(address >> 10) & 7U][address & 1023U];
    }
    /**
     * @brief Вычисляет физическое смещение для текущей банковской раскладки.
     * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    std::size_t prgOffset(std::uint16_t address) const noexcept;
    /**
     * @brief Вычисляет физическое смещение для текущей банковской раскладки.
     * @details Само смещение не продлевает lifetime ROM и не является правом удерживать указатель после изменения mapper.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @return Вычисленное значение или состояние типа std::size_t согласно описанному контракту.
     */
    std::size_t chrOffset(std::uint16_t address) const noexcept;
    /**
     * @brief Декодирует запись регистров MMC3 и обновляет связанные отображения/IRQ.
     * @details Не заменяет последующие PPU-такты; CHR/PRG-страницы обновляются при изменении банков.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void mmc3Write(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Перестраивает прямые указатели банков после изменения отображения картриджа.
     * @details Содержимое ROM не копируется; обновление должно предшествовать следующему доступу с новой банковой раскладкой.
     */
    void refreshPrgPages() noexcept;
    /**
     * @brief Перестраивает прямые указатели банков после изменения отображения картриджа.
     * @details Содержимое ROM не копируется; обновление должно предшествовать следующему доступу с новой банковой раскладкой.
     */
    void refreshChrPages() noexcept;
    const std::uint8_t* prg_ = nullptr;
    const std::uint8_t* chr_ = nullptr;
    std::size_t prgSize_ = 0, chrSize_ = 0;
    std::array<const std::uint8_t*, 4> prgPages_{};
    // Временное представление данных потока владельца. Исходный prgPages_ остаётся физическим.
    // Без передачи владения, heap, checksum и проверки резидентности на каждом чтении.
    /**
     * @brief Заимствованное отображение неизменяемого хвоста PRG; исходные физические страницы сохраняются отдельно от резидентных копий.
     * @ingroup nes_cartridge
     */
    struct CpuPrgTailView final {
        /**
         * @brief Создаёт пустое описание резидентного хвоста PRG.
         * @details Исходные и резидентные страницы не действительны до проверенного bind/refresh.
         */
        CpuPrgTailView() noexcept = default;
        /**
         * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
         * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
         */
        CpuPrgTailView(const CpuPrgTailView&) = delete;
        /**
         * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
         * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
         */
        CpuPrgTailView& operator=(const CpuPrgTailView&) = delete;
        /**
         * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
         * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
         */
        CpuPrgTailView(CpuPrgTailView&&) = delete;
        /**
         * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
         * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
         */
        CpuPrgTailView& operator=(CpuPrgTailView&&) = delete;
        std::array<const std::uint8_t*,4> pages{};
        const std::uint8_t* source = nullptr;
        std::uint8_t* resident = nullptr;
        /**
         * @brief Пересчитывает резидентные PRG-указатели по исходным текущим банковым страницам.
         * @details Отображение источника сохраняется отдельно, чтобы внутренняя копия не подменяла физическую ROM-идентичность.
         * @param physical Четыре настоящих физических PRG-страницы текущего отображения; резидентные алиасы не заменяют их идентичность.
         */
        void refresh(const std::array<const std::uint8_t*,4>& physical) noexcept {
            pages=physical;
            if(!resident)return;
            for(unsigned i=0;i<4;++i) {
                if(physical[i]==source)pages[i]=resident;
                else if(physical[i]==source+8192U)pages[i]=resident+8192U;
            }
        }
        /**
         * @brief Завершает соответствующую привязку после исключения активного использования.
         * @details Не освобождает память вызывающего и не заменяет ожидание фактического окончания другой стороны.
         * @param physical Четыре настоящих физических PRG-страницы текущего отображения; резидентные алиасы не заменяют их идентичность.
         */
        void detach(const std::array<const std::uint8_t*,4>& physical) noexcept {
            pages=physical;source=nullptr;resident=nullptr;
        }
    } cpuPrgTail_;
    const std::uint8_t* cpuPrgSourceImage_ = nullptr;
    std::size_t cpuPrgSourceImageBytes_ = 0;
    /**
     * @brief Сбрасывает внутренние ссылки на резидентный PRG перед сменой источника.
     * @details Картридж не освобождает арену приложения; порядок вызова защищает её от дальнейшего использования.
     */
    void detachCpuPrgTail() noexcept { cpuPrgTail_.detach(prgPages_); }

    std::array<const std::uint8_t*, 8> chrPages_{};
    std::array<std::uint8_t*, 8> chrRamPages_{};
    std::uint16_t mapper_ = 0;
    Mirroring initialMirroring_ = Mirroring::Horizontal;
    bool fourScreen_ = false, palHint_ = false, legacyHeader_ = false;
    // Инвалидация mapping-view единственной Machine. Все изменения страниц
    // проходят refreshChrPages; сброс только после успешной публикации view.
    // CHR-RAM content меняется отдельно, без смены физической идентичности.
    bool chrMappingDirty_ = true;
    std::uint8_t bank_ = 0;
    // Первый адрес регистра выбора страницы CIRAM в варианте Camerica.
    std::uint16_t camericaMirrorStart_ = 0xa000;
    std::uint8_t mmc1Shift_ = 0x10, mmc1Control_ = 0x0c;
    std::uint8_t mmc1Chr0_ = 0, mmc1Chr1_ = 0, mmc1Prg_ = 0;
    std::uint64_t mmc1LastWrite_ = ~std::uint64_t{0};
    std::array<std::uint8_t, 8> mmc3Banks_{};
    std::uint8_t mmc3Select_ = 0, mmc3Mirror_ = 0, mmc3Ram_ = 0x80;
    std::uint8_t irqLatch_ = 0, irqCounter_ = 0;
    bool irqReload_ = false, irqEnabled_ = false, irqPending_ = false, a12_ = false;
    std::uint16_t a12LowDots_ = 16;
    // Одна активная плата использует эти малые digital-регистры; ROM/RAM не копируются.
    std::array<std::uint8_t, 8> expansionChr_{};
    std::array<std::uint8_t, 3> expansionPrg_{};
    std::uint8_t submapper_ = 0, expansionMirror_ = 0, expansionRamControl_ = 0;
    std::uint8_t fme7Command_ = 0, fme7IrqControl_ = 0, fme7Prg6000_ = 0x40;
    std::uint16_t expansionRamBytes_ = 8192, fme7Counter_ = 0, vrc7Prescaler_ = 341;
    std::uint8_t vrc7AddressMask_ = 0x18, vrc7Latch_ = 0, vrc7Counter_ = 0, vrc7IrqControl_ = 0;
    Sunsoft5BAudio sunsoft5bAudio_{};
    Vrc7Audio vrc7Audio_{};
    // Один ABI всех consumers; default localFM не удерживает внешние ресурсы.
    Vrc7WriteTape*vrc7Writer_=nullptr;
    Vrc7WriteTape::Fault vrc7TraceFault_=Vrc7WriteTape::Fault::None;
    std::uint8_t vrc7ShadowRemaining_=64;
    bool externalVrc7_=false;
    Legacy160 legacy_{};
    std::array<std::uint8_t, 8192> prgRam_{};
    std::array<std::uint8_t, 8192> chrRam_{};
};

} // пространство имён nes::core
