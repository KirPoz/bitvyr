/**
 * @file Mmc5.h
 * @brief Собственная цифровая модель MMC5 без SDK, звука и выделения памяти.
 * @details Аппаратные регистры описаны в https://www.nesdev.org/wiki/MMC5 .
 * Реализация написана по описанию сигналов, без переноса класса чужого эмулятора.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::core {

class Mmc5RasterPlan;

/**
 * @brief Цифровая часть MMC5: banking, ExRAM, подстановка PPU, multiplier и IRQ.
 * @details Один владелец исполняет все методы в порядке настоящих CPU/PPU bus edges.
 * Объект принадлежит сессии mapper5, а не каждому Cartridge. ROM и WRAM заимствованы
 * до detach/join; внутри находятся только 1024 байта ExRAM и небольшое состояние.
 * Звук $5000-$5015 и PCM read-strobe обслуживает отдельный Mmc5Audio.
 */
class Mmc5 {
public:
    /** @brief Назначение настоящего чтения PPU; Cpu означает доступ через $2007. */
    enum class Fetch : std::uint8_t {
        Cpu, BackgroundName, BackgroundAttribute, BackgroundPattern, SpritePattern, Dummy
    };
    /** @brief Результат одной выборки: значение шины и физическое смещение CHR-ROM. */
    struct PpuRead {
        std::uint8_t value{};
        std::size_t chrOffset{InvalidChrOffset};
    };
    /** @brief Чтение nametable не ссылается на CHR-ROM. */
    static constexpr std::size_t InvalidChrOffset = static_cast<std::size_t>(-1);
    /** @brief Максимум внешней WRAM модели; память выделяет вызывающий до загрузки. */
    static constexpr std::size_t MaxPrgRamBytes = 128U * 1024U;

    /**
     * @brief Подключает неизменяемые ROM и внешнюю WRAM без копирования/очистки RAM.
     * @param prg PRG-ROM с размером степени двух от 8 KiB до 1 MiB.
     * @param prgBytes Число байтов PRG-ROM.
     * @param chr CHR-ROM размером от 1 КиБ до 1 МиБ, кратным 1 КиБ. Незаполненная часть адресного окна читается как $FF.
     * @param chrBytes Число байтов CHR-ROM.
     * @param ram WRAM; nullptr допустим только при ramBytes=0.
     * @param ramBytes 0, 8, 16, 32, 64 либо 128 KiB; выбирает проводку ExROM.
     * @param splitSlMode true для редкой SL-проводки; обычные платы используют false/CL.
     * @return false при неверных backing; прежние заимствования отключены.
     */
    bool attach(const std::uint8_t* prg, std::size_t prgBytes,
                const std::uint8_t* chr, std::size_t chrBytes,
                std::uint8_t* ram, std::size_t ramBytes, bool splitSlMode = false) noexcept;
    /** @brief Удаляет заимствования; вызывающий сначала завершает использующий их Worker. */
    void detach() noexcept;
    /** @brief Начальное состояние цифровой модели; внешняя WRAM не стирается. */
    void powerOn() noexcept;
    /**
     * @brief Аппаратный reset M2 восстанавливает только reset-detected регистры.
     * @details Отключает scanline IRQ без acknowledge/counter reset; сохраняет WRAM,
     * ExRAM, CHR banks, $5203 и прочие регистры без reset detection.
     */
    void resetSignal() noexcept;

    /**
     * @brief Выполняет настоящий доступ CPU к mapper/PRG, включая acknowledge $5204.
     * @param address Адрес CPU; неотображённый адрес возвращает openBus.
     * @param openBus Предыдущее значение шины для неподключённых разрядов/областей.
     * @return Значение CPU data bus. Не вызывать дополнительно cpuReadStrobe для этого чтения.
     */
    std::uint8_t cpuRead(std::uint16_t address, std::uint8_t openBus = 0) noexcept;
    /** @brief Читает отображение без acknowledge, NMI-vector strobe и изменения mapper. */
    std::uint8_t cpuPeek(std::uint16_t address, std::uint8_t openBus = 0) const noexcept;
    /** @brief Записывает регистр/WRAM; также наблюдает точные адреса $2000/$2001/$4014. */
    void cpuWrite(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Наблюдает чтение, которое выполнил другой bus-путь: векторы или PPUSTATUS.
     * @param address Настоящий CPU-адрес без приведения зеркал $2000-$3FFF.
     * @param value Прочитанное значение, в частности бит 7 настоящего $2002.
     */
    void cpuReadStrobe(std::uint16_t address, std::uint8_t value) noexcept;
    /**
     * @brief Обрабатывает M2 rising edges после предшествующих выборок PPU.
     * @param cycles Число CPU-тактов без промежуточных PPU-read; при наличии чтений
     * вызывающий делит интервал и передаёт strobes в хронологическом порядке.
     */
    void clockCpu(unsigned cycles = 1) noexcept;
    /** @brief Возвращает цифровой IRQ; вызывающий объединяет его с APU/MMC5Audio. */
    bool irqLine() const noexcept { return irqPending_ && irqEnabled_; }
    /** @brief True между подтверждённым началом строки и тремя пустыми M2. */
    bool inFrame() const noexcept { return inFrame_; }
    /** @brief Возвращает текущий аппаратный 8-битный счётчик строк, без побочных эффектов. */
    unsigned scanline() const noexcept { return scanline_; }
    /**
     * @brief Подтверждает начальный CHR-разрыв детектора для ограниченного BG-интервала.
     * @return true только внутри кадра, с false IRQ и последним настоящим CHR-read.
     * @details Геометрию и отсутствие CPU/MMIO изменений отдельно доказывает Machine.
     */
    bool canRunQuietBackground() const noexcept {
        return inFrame_&&!irqLine()&&nameRepeat_==0&&lastPpuAddress_<0x2000U&&
            ppuReadSinceM2_&&cpuIdle_<=1U;
    }

    /**
     * @brief Возвращает начало текущей 8 KiB PRG-страницы либо nullptr для open bus.
     * @param page Номер 0..3 для $8000/$A000/$C000/$E000.
     * @details Указатель заимствован до следующей mapper-записи или detach. Наличие
     * страницы не разрешает пропуск CPU strobes/PCM; первая интеграция их не обходит.
     */
    const std::uint8_t* prgPage(unsigned page) const noexcept;
    /**
     * @brief Возвращает фактическую непрерывную WRAM-страницу CPU $6000-$7FFF.
     * @return Заимствованные 8192 байта либо nullptr при отсутствии RAM/неподключённом CE.
     * @details Чистое отображение $5113; PRG mode и защита записи не меняют чтение.
     * Указатель действителен до смены $5113/detach; не удостоверяет владение или join.
     */
    const std::uint8_t* cpu6000Page() const noexcept;
    /** @brief Возвращает WRAM-адрес только для разрешённой записи в текущем mapping. */
    std::uint8_t* writablePrg(std::uint16_t address) noexcept;
    /** @brief Консервативно запрещает прямой CPU bypass, теряющий vector/PCM strobes. */
    bool canUseDirectPrg() const noexcept { return false; }

    /**
     * @brief Выполняет одну настоящую PPU-read, IRQ-strobe и нужную подстановку.
     * @param address Настоящий PPU-адрес до подстановки MMC5.
     * @param kind Фаза fetch; Dummy также создаёт IRQ-strobe.
     * @param ciram Заимствованная CIRAM 2048 байт; nullptr читает как нули.
     * @param tileX Номер BG-fetch по экрану, 0..33, для вертикального split.
     * @return Значение и физический CHR offset для snapshot/декодера; NT offset invalid.
     * @details BackgroundName фиксирует per-tile ExRAM/split latch. Attribute и обе
     * pattern-выборки того же тайла используют latch. Не повторять ppuReadStrobe.
     */
    PpuRead ppuRead(std::uint16_t address, Fetch kind, const std::uint8_t* ciram,
                    unsigned tileX = 0) noexcept;
    /** @brief Записывает отображённую CIRAM/ExRAM; CHR-ROM/fill не изменяются. */
    void ppuWrite(std::uint16_t address, std::uint8_t value, std::uint8_t* ciram) noexcept;
    /** @brief Передаёт только настоящий PPU /RD edge, если значение читает другой путь. */
    void ppuReadStrobe(std::uint16_t address) noexcept;
    /**
     * @brief Вычисляет адрес CHR без нового строба. Адрес может попасть в недостающий хвост образа.
     * @details Для BG-pattern вызывающий предварительно выполняет BackgroundName.
     */
    std::size_t chrOffset(std::uint16_t address, Fetch kind = Fetch::Cpu) const noexcept;
    /** @brief Читает обычное NT-отображение без BG-latch/strobe/extended override. */
    std::uint8_t nametablePeek(std::uint16_t address, const std::uint8_t* ciram) const noexcept;
    /** @brief Предоставляет read-only ExRAM для сохранения/диагностики владельца. */
    const std::array<std::uint8_t, 1024>& exRam() const noexcept { return exRam_; }

private:
    friend class Cartridge;
    friend class Machine;
    friend class TimingPpu;
    friend class Ppu;
    friend struct Mmc5TestAccess;
    friend struct MachineEventKernelTestAccess;
    friend class Mmc5RasterPlan;
    /** @brief Принимаемый CPU ExRAM offset либо~0U, без записи/clock. */
    unsigned cpuExRamWriteTarget(std::uint16_t address) const noexcept;
    /** @brief Принятый физический CIRAM0..2047/ExRAM2048..3071 либо~0U. */
    unsigned ppuWriteTarget(std::uint16_t address,bool ciram) const noexcept;
    /**
     * @brief Подготавливает неизменяемую адресную policy следующего синхронного участка.
     * @param plan Выходные заимствованные дескрипторы; при false не изменяются.
     * @param ciram Живые 2048 байт CIRAM либо nullptr для нулевого источника.
     * @param control Текущий PPUCTRL для адреса полного фонового тайла.
     * @return true при подключённом корректном CHR backing и допустимых режимах.
     * @details Не меняет mapper, bus strobes или время. Caller отдельно доказывает
     * временную границу; подготовка не является quiet-сертификатом. Plan нельзя
     * переносить через смену ROM, policy-регистров или CIRAM. Version0 разрешает
     * свежую подготовку, но запрещает повторное использование закешированного plan.
     */
    bool prepareRasterPlan(Mmc5RasterPlan& plan, const std::uint8_t* ciram,
                           std::uint8_t control) noexcept;
    /**
     * @brief Возвращает тег policy;0 означает, что кеш не имеет права получить hit.
     * @details Тег не возвращается к 1 на reset/attach; после uint32 wrap остаётся 0.
     */
    std::uint32_t rasterPlanVersion() const noexcept { return rasterVersion_; }
    /** @brief Инвалидирует все заимствованные policy без ABA после переполнения. */
    void invalidateRasterPlan() noexcept { if (rasterVersion_) ++rasterVersion_; }
    /**
     * @brief Проверяет вход целого C0 BG span без побочного эффекта.
     * @return true только после pattern /RD с R0; это не общий guest horizon.
     * @pre Внутри span нет M2, CPU/MMIO и изменения backing/policy/region.
     */
    bool canReadFullQuietBackgroundSpan() const noexcept;
    /**
     * @brief Материализует точный detector endpoint C0 span, не меняя cpuIdle.
     * @param lastHigh Последний настоящий high-pattern PPU address0..0x1fff.
     * @pre Guard true, один или более полных BG tiles или NT/NT/Low/High sprite slots исполнены;
     * owner не допускал M2/MMIO/наблюдение до завершения этой операции.
     * @details Sparse M2 принадлежат replay: primary finishQuietBackground здесь
     * использовать нельзя, поскольку он дополнительно обнуляет cpuIdle_.
     */
    void finishFullQuietBackgroundSpan(std::uint16_t lastHigh) noexcept;
    /** @brief Полные данные одного BG-тайла; offset относится к младшей CHR plane. */
    struct BackgroundTile {
        std::uint8_t tile{}, attribute{}, low{}, high{};
        std::size_t lowChrOffset{InvalidChrOffset};
    };
    /**
     * @brief Получает полный тайл с четырьмя настоящими упорядоченными /RD.
     * @param scroll Loopy v до Name, включая fine Y и nametable bits.
     * @param control PPUCTRL перед тайлом; bit4 выбирает таблицу pattern.
     * @param ciram Заимствованная CIRAM, живая до возврата; nullptr читает нули.
     * @param tileX Экранная fetch-колонка, включая будущие visible tiles2..33.
     * @return Прежние Name/attribute/planes и physical low offset; нет backing — planes0.
     * @pre Caller — неподвижный C0 whole-tile replay без CPU/MMIO/bank mutation.
     * @details Растр/RGB принадлежат caller. Каждый /RD предшествует данным своей
     * фазы; Name/Attr используют прежнюю data policy после возможного IRQ edge.
     * Дескриптор CHR не сохраняется между тайлами, указатели не усваиваются.
     */
    BackgroundTile ppuReadFullBackgroundTile(std::uint16_t scroll,std::uint8_t control,
                                           const std::uint8_t* ciram,unsigned tileX) noexcept;
    /**
     * @brief Получает Name, attribute и обе CHR planes в одном разрешённом BG-тайле.
     * @param scroll Loopy v перед Name, включая fine Y и nametable bits.
     * @param control PPUCTRL; бит 4 выбирает таблицу фоновых pattern.
     * @param ciram Заимствованные 2048 байт CIRAM либо nullptr для нулевого чтения.
     * @param tileX Настоящая экранная колонка fetch, включая prefetch 0/1.
     * @param needPlanes true читает обе CHR planes; false возвращает только Name/attribute/offset.
     * При needPlanes=false байты low/high равны нулю; все Name-latch и digital fields прежние.
     * @return Данные тайла и physical offset low; отключённый backing даёт нулевые planes.
     * @details Только certificate прежнего quiet BG разрешает объединить четыре data-read.
     * Внутри нет CPU/MMIO наблюдения и записи банков. Метод сохраняет настоящий Name
     * latch; /RD и M2 материализует caller тем же finishQuietBackground, что и раньше.
     */
    BackgroundTile ppuReadQuietBackgroundTile(std::uint16_t scroll, std::uint8_t control,
                                             const std::uint8_t* ciram, unsigned tileX, bool needPlanes = true) noexcept;
    /**
     * @brief Читает прежнее отображение после уже доказанного canonical BG strobed-domain.
     * @details Только private PPU24 с R0/lastCHR/Ptrue/I<=1 и без наблюдателей.
     * Name выполняет прежний ExRAM/split latch; IRQ detector материализуется до tail.
     * @param address Настоящий уже нормализованный Name/Attribute/Pattern адрес.
     * @param kind Настоящая typed фаза; CPU/Dummy здесь не допускаются caller.
     * @param ciram Заимствованные 2048 байт CIRAM до завершения synchronous replay.
     * @param tileX Экранная колонка настоящего fetch.
     * @return Прежнее значение и физическое CHR смещение.
     */
    PpuRead ppuReadQuietBackground(std::uint16_t address, Fetch kind,
                                  const std::uint8_t* ciram, unsigned tileX) noexcept;
    /** @brief Единая read-data часть обычного и qualified доступа, без нового /RD. */
    PpuRead readPpuData(std::uint16_t address, Fetch kind,
                        const std::uint8_t* ciram, unsigned tileX) noexcept;
    /**
     * @brief Материализует доказанный конец canonical 24dot run до любого observer/tail.
     * @param lastHigh Настоящий адрес последнего выполненного CHR high read, ниже $2000.
     * @details R0, Ptrue, I0. IRQ/inFrame/counter/splitY и per-tile latch сохранены.
     */
    void finishQuietBackground(std::uint16_t lastHigh) noexcept;

    /**
     * @brief Читает целый C0 sprite-slot, сохраняя detector и наборA обеих planes.
     * @param nameAddress Настоящий одинаковый адрес двух DummyName /RD, $2000..$2fff.
     * @param lowAddress Настоящий SpritePattern low ниже $2000, bit3 равен 0.
     * @param ciram Живые 2048 байт CIRAM либо nullptr; не усваивается.
     * @return Raw low в младшем байте, raw high в старшем; нет CHR — нулевые planes.
     * @pre Whole8-dot C0 replay без M2, CPU/MMIO, bank/backing mutation и observers.
     * @details Небезопасный head исполняет прежние четыре /RD в исходном порядке.
     * R0/lastCHR допускает их точный endpoint без изменения cpuIdle, IRQ и BG latch.
     * Horizontal flip, metadata/OAM и растровое время остаются у Ppu caller.
     */
    std::uint16_t ppuReadFullSpriteSlot(std::uint16_t nameAddress,
                                      std::uint16_t lowAddress,
                                      const std::uint8_t* ciram) noexcept;

    /**
     * @brief Получает две настоящие sprite planes из неизменного набораA без /RD.
     * @param lowAddress Канонический адрес low plane ниже$2000, bit3 равен 0.
     * @param low Выход raw low byte; при false остаётся прежним.
     * @param high Выход raw high byte; при false остаётся прежним.
     * @return true после проверки обоих ROM диапазонов и получения low/high.
     * @pre Caller удерживает один quiet interval без CPU/MMIO/CHR-bank изменений.
     * @details Не меняет detector или BG latch. Caller отдельно доказывает
     * dummy/digital sequence и материализует её прежний canonical endpoint.
     */
    bool readQuietSpritePlanes(std::uint16_t lowAddress,std::uint8_t& low,
                               std::uint8_t& high) const noexcept;

    /** @brief Pure prefix certificate; реальные read-strobes и M2 clocks всё равно исполняются. */
    bool canCompleteH2() const noexcept {
        return inFrame_&&!irqPending_&&nameRepeat_==0&&ppuReadSinceM2_&&cpuIdle_<=3U;
    }

    /**
     * @brief Пересчитывает физические базы восьми страниц normal-CHR выбранного набора.
     * @param set Набор A=0 либо B=1; вызывают только записи банков/режима и powerOn.
     * @details Базы в байтах заимствованного CHR-ROM не содержат копий данных и не
     * заменяют PPU read-strobe. Без подключённого ROM таблица заполняется нулями.
     */
    void rebuildNormalChrCache(unsigned set) noexcept;
    /** @brief Возвращает смещение внешней WRAM с учётом CE-проводки платы. */
    std::size_t ramOffset(unsigned bank, unsigned within) const noexcept;
    /** @brief Возвращает bank8k и ROM/RAM для CPU-адреса; false означает open bus. */
    bool decodePrg(std::uint16_t address, unsigned& bank, bool& rom) const noexcept;
    /** @brief Начинает tile-latch extended attributes/split по настоящему NT-fetch. */
    void beginBackground(std::uint16_t address, unsigned tileX) noexcept;
    /** @brief Обновляет frame/counter/IRQ при подтверждённом начале следующей строки. */
    void detectScanline() noexcept;
    /** @brief Подтверждает IRQ и сбрасывает frame/read-detector после явного сигнала. */
    void endFrame() noexcept;

    const std::uint8_t* prg_{};
    const std::uint8_t* chr_{};
    std::uint8_t* ram_{};
    std::size_t prgBytes_{}, chrBytes_{}, ramBytes_{};
    // Адресные линии образуют степень двух. Пустой хвост за chrBytes_ читается как $FF.
    std::uint32_t chrAddressMask_{};
    /** @brief Разрешает пути с прямым доступом, когда все адреса подкреплены ROM. */
    bool chrFullyBacked() const noexcept { return chrBytes_ == std::size_t(chrAddressMask_) + 1U; }
    /** @brief Читает байт CHR; пустой хвост отдаёт $FF, а отключённый источник — 0. */
    std::uint8_t readChrByte(std::size_t offset) const noexcept {
        if (offset < chrBytes_) return chr_[offset];
        return chr_ && offset <= chrAddressMask_ ? 0xffU : 0U;
    }
    std::array<std::uint8_t, 1024> exRam_{};
    std::array<std::uint8_t, 5> prgBanks_{};
    std::array<std::uint16_t, 12> chrBanks_{};
    std::uint8_t prgMode_{3}, chrMode_{}, protect1_{1}, protect2_{2};
    std::uint8_t exMode_{3}, ntMap_{}, fillTile_{}, fillAttribute_{}, upperChr_{};
    std::uint8_t splitControl_{}, splitScroll_{}, splitBank_{}, splitY_{};
    std::uint8_t irqCompare_{}, scanline_{}, multiplierA_{}, multiplierB_{};
    std::uint8_t ppuControl_{}, ppuMask_{}, lastChrSet_{};
    std::uint8_t nameRepeat_{}, cpuIdle_{}, tileAttribute_{}, tileSplitAttribute_{}, tileSplitY_{};
    std::uint16_t lastPpuAddress_{0xffff}, tileNtOffset_{};
    bool irqEnabled_{}, irqPending_{}, inFrame_{}, ppuReadSinceM2_{};
    bool tileExtended_{}, tileSplit_{}, splitSlMode_{};
    /** @brief Два normal-CHR набора по восемь физических 1 KiB баз; всегда одинаковый layout OFF/ON. */
    std::array<std::array<std::uint32_t, 8>, 2> normalChrBases_{};
    /** @brief Монотонная policy-эпоха; поле безусловно одинаково при OFF/ON E3. */
    std::uint32_t rasterVersion_{1};
    static_assert(sizeof(decltype(normalChrBases_)) == 64, "Normal CHR cache must contain exactly 64 bytes");
};

} // namespace nes::core
