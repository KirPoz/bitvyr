/**
 * @file AudioPacket.h
 * @brief Ограниченные журналы регистров и реальных DMC-байтов на гостевом времени.
 * @ingroup nes_apu
 */
#pragma once
#include "../Types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nes::core {

enum class AudioFault : std::uint8_t { None, InputCapacity, DmcCapacity, Timestamp, DmcOrder, InvalidDestination
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    ,ExpansionCapacity,ExpansionOrder
#endif
};

// Ограниченное хранилище вызывающего. Machine пишет активный пакет только своим
// потоком владельца. Retire копирует использованные записи в другой неизменяемый пакет.
/**
 * @brief Ограниченные записи регистров и DMC для раздельного APU. Активный пакет пишет только CPU-владелец; приёмный экземпляр не должен перекрывать источник.
 * @ingroup nes_apu
 */
class AudioPacket final {
public:
    static constexpr unsigned inputCapacity=32768,dmcCapacity=512;
    static constexpr std::uint8_t readStatusOperation=0x95,clearSamplesOperation=0xFE;
    /**
     * @brief Вход аудиорендерера: timestamp в CPU-тактах от начала пакета, адрес регистра и записанный байт.
     * @ingroup nes_apu
     */
    struct Input {std::uint16_t offset;std::uint8_t operation,value;};
    /**
     * @brief Записанный настоящий DMC-байт с timestamp и адресом источника; renderer не повторяет чтение живого картриджа.
     * @ingroup nes_apu
     */
    struct Dmc {std::uint16_t offset,address;std::uint8_t value,stall;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        std::uint16_t reserved=0;std::int32_t expansionLevel=0;
#endif
    };
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    static constexpr unsigned expansionCapacity=65536;
    static constexpr std::uint16_t expansionVersion=1,externalDacQ16=1;
    /** @brief Изменение raw DAC на CPU boundary; prefix сохраняет порядок native events того же времени. */
    struct Expansion {std::uint16_t offset,nativePrefix;std::int32_t level;};
    static_assert(sizeof(Input)==4&&sizeof(Dmc)==12&&sizeof(Expansion)==8,"typed expansion representation");
    /** @brief Допускает только уже доказанный signed диапазон первого raw-Q16 provider. */
    static constexpr bool validExpansionLevel(std::int32_t level)noexcept{return level>=-95070&&level<=0;}
#else
    static_assert(sizeof(Input)==4&&sizeof(Dmc)==6,"compact event representation");
#endif
    /**
     * @brief Неизменяемые метаданные аудиоинтервала: поколение, sequence, начальный/конечный CPU-такт, границы журналов и fault.
     * @ingroup nes_apu
     */
    struct Header {
        std::uint64_t generation=0,sequence=0,ppuFrame=0,baseCycle=0,endCycle=0;
        std::uint32_t inputCount=0,dmcCount=0;
        Region region=Region::Ntsc;
        AudioFault fault=AudioFault::None;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        std::uint16_t formatVersion=expansionVersion,capabilities=0;
        std::uint32_t expansionCount=0;
#endif
    };
    /**
     * @brief Начинает новый интервал или привязку объекта.
     * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
     * @param generation Поколение данного протокола; не подменяется эпохой другого тракта.
     * @param sequence Последовательный номер интервала; не номер физической подачи LCD.
     * @param base Начало гостевого интервала либо эффективная адресная база согласно типу аргумента.
     * @param region Явно выбранный гостевой регион NTSC/PAL.
     */
    void begin(std::uint64_t generation,std::uint64_t sequence,std::uint64_t base,Region region
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
               ,bool external=false
#endif
               ) noexcept {
        header_={generation,sequence,0,base,base,0,0,region,AudioFault::None};
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        header_.capabilities=external?externalDacQ16:0;
#endif
    }
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    /** @brief Записывает changed raw level до следующего APU tick; первый fault остаётся sticky. */
    [[nodiscard]] bool addExpansion(std::uint64_t cycle,std::int32_t level)noexcept{
        if(header_.fault!=AudioFault::None)return false;
        if(!validTime(cycle))return false;
        if(header_.capabilities!=externalDacQ16||!validExpansionLevel(level)||header_.inputCount>inputCapacity){header_.fault=AudioFault::ExpansionOrder;return false;}
        if(header_.expansionCount>=expansionLimit_){header_.fault=AudioFault::ExpansionCapacity;return false;}
        if(header_.expansionCount&&expansion_[header_.expansionCount-1].offset>cycle-header_.baseCycle){header_.fault=AudioFault::ExpansionOrder;return false;}
        expansion_[header_.expansionCount++]={static_cast<std::uint16_t>(cycle-header_.baseCycle),static_cast<std::uint16_t>(header_.inputCount),level};return true;
    }
    /** @brief Возвращает immutable borrowed tape только после публикации/retire владельцем. */
    [[nodiscard]] const Expansion* expansions()const noexcept{return expansion_.data();}
#endif
    /**
     * @brief Добавляет регистровое аудиособытие с точным смещением CPU-времени.
     * @details Проверяет ёмкость и диапазон 16-битного offset; первая ошибка не теряется.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @param operation Код реальной регистровой операции, уже совершённой производителем.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool addInput(std::uint64_t cycle,std::uint8_t operation,std::uint8_t value) noexcept {
        if(header_.fault!=AudioFault::None)return false;
        if(!validTime(cycle))return false;
        if(header_.inputCount>=inputLimit_){header_.fault=AudioFault::InputCapacity;return false;}
        input_[header_.inputCount++]={static_cast<std::uint16_t>(cycle-header_.baseCycle),operation,value};return true;
    }
    /**
     * @brief Сохраняет реальный ответ памяти на запрос DMC.
     * @details Адрес, значение и момент нужны для повторного APU без второго CPU-чтения.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @param address Гостевой адрес в пространстве данного шинного контракта; не физический указатель ROM.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    [[nodiscard]] bool addDmc(std::uint64_t cycle,std::uint16_t address,std::uint8_t value
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
                            ,std::int32_t expansionLevel=0
#endif
                            ) noexcept {
        if(header_.fault!=AudioFault::None)return false;
        if(!validTime(cycle))return false;
        if(header_.dmcCount>=dmcLimit_){header_.fault=AudioFault::DmcCapacity;return false;}
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        if(!validExpansionLevel(expansionLevel)||(header_.capabilities==0&&expansionLevel!=0)){header_.fault=AudioFault::ExpansionOrder;return false;}
        dmc_[header_.dmcCount++]={static_cast<std::uint16_t>(cycle-header_.baseCycle),address,value,0,0,expansionLevel};return true;
#else
        dmc_[header_.dmcCount++]={static_cast<std::uint16_t>(cycle-header_.baseCycle),address,value,0};return true;
#endif
    }
    /**
     * @brief Учитывает украденные DMC такты в установленном порядке.
     * @details Сами данные и момент выборки не угадываются. Пакетный тракт привязывает задержку к последней подходящей записи DMC.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @param clocks Число гостевых CPU-тактов в данном шинном/аудиоинтервале.
     */
    void setDmcStall(std::uint64_t cycle,unsigned clocks) noexcept {
        if(header_.fault!=AudioFault::None)return;
        if(!header_.dmcCount||clocks>255||cycle<header_.baseCycle||cycle-header_.baseCycle>65535){header_.fault=AudioFault::DmcOrder;return;}
        auto& last=dmc_[header_.dmcCount-1];
        if(last.offset!=cycle-header_.baseCycle||last.stall){header_.fault=AudioFault::DmcOrder;return;}
        last.stall=static_cast<std::uint8_t>(clocks);
    }
    /**
     * @brief Закрывает аудиоинтервал и копирует только занятые записи в отдельный пакет.
     * @details CPU-владелец должен быть остановлен; функция не запускает AudioRenderer и не копирует неиспользуемую ёмкость.
     * @param end Конец уже исполненного гостевого интервала.
     * @param frame Номер согласованного гостевого PPU-кадра.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param out Отдельный незанятый пакет назначения; получает только использованные записи и заголовок интервала.
     */
    [[nodiscard]] bool retireTo(AudioPacket& out,std::uint64_t end,std::uint64_t frame) const noexcept {
        if(&out==this||header_.fault!=AudioFault::None||end<header_.baseCycle||end-header_.baseCycle>65535||
            header_.inputCount>out.inputLimit_||header_.dmcCount>out.dmcLimit_
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
            ||header_.expansionCount>out.expansionLimit_
#endif
            )return false;
        out.header_=header_;out.header_.endCycle=end;out.header_.ppuFrame=frame;
        std::memcpy(out.input_.data(),input_.data(),header_.inputCount*sizeof(Input));
        std::memcpy(out.dmc_.data(),dmc_.data(),header_.dmcCount*sizeof(Dmc));
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        std::memcpy(out.expansion_.data(),expansion_.data(),header_.expansionCount*sizeof(Expansion));
#endif
        return true;
    }
    /**
     * @brief Предоставляет неизменяемый заголовок опубликованного пакета.
     * @details Ссылка заимствована. Писатель не изменяет заголовок, пока пакет принадлежит читателю; внутренний объект нельзя использовать как самостоятельный снимок.
     * @return Ссылка на метаданные пакета; владение и срок жизни остаются у пакета.
     */
    [[nodiscard]] const Header& header() const noexcept{return header_;}
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const Input* inputs() const noexcept{return input_.data();}
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const Dmc* dmc() const noexcept{return dmc_.data();}
    /**
     * @brief Возвращает текущую ошибку соответствующего тракта.
     * @details Первичная ошибка сохраняется до установленного reset/begin; отсутствие ошибки не означает завершения физического вывода.
     * @return Вычисленное значение или состояние типа AudioFault согласно описанному контракту.
     */
    [[nodiscard]] AudioFault fault() const noexcept{return header_.fault;}
    // Меньший детерминированный лимит обеспечивает политику памяти вызывающего
    // и тесты отказов; физические массивы остаются полностью ограниченными.
    /**
     * @brief Ограничивает доступную ёмкость существующих массивов пакета.
     * @details Не меняет физический размер хранилища; используется политикой памяти и проверками переполнения.
     * @param input Лимит либо входная запись согласно типу параметра.
     * @param dmc Лимит или состояние выборок DMC согласно типу параметра.
     */
    void setLimits(unsigned input,unsigned dmc
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
                   ,unsigned expansion=expansionCapacity
#endif
                   ) noexcept {
        inputLimit_=input<inputCapacity?input:inputCapacity;dmcLimit_=dmc<dmcCapacity?dmc:dmcCapacity;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        expansionLimit_=expansion<expansionCapacity?expansion:expansionCapacity;
#endif
    }
private:
    friend class Machine;
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    /**
     * @brief Pure admission максимум двух changed-DAC записей на T-1/T; credits не списывает.
     * @param first Первая publish boundary до tick(T), в абсолютных CPU clocks.
     * @details Только активный owner packet; учитывает настоящий configured expansionLimit_, timestamp/prefix tails.
     * @return true подтверждает запас; false не меняет fault/credits и требует scalar fallback.
     */
    bool canAppendMmc5H2(std::uint64_t first) const noexcept {
        if(header_.fault!=AudioFault::None||header_.formatVersion!=expansionVersion||
           header_.capabilities!=externalDacQ16||header_.endCycle!=header_.baseCycle||
           inputLimit_>inputCapacity||dmcLimit_>dmcCapacity||
           header_.inputCount>inputLimit_||header_.dmcCount>dmcLimit_||
           header_.expansionCount>expansionLimit_||expansionLimit_>expansionCapacity||
           expansionLimit_-header_.expansionCount<2U||first==~std::uint64_t{0}||first<header_.baseCycle||
           first-header_.baseCycle>65534U)return false;
        const auto offset=first-header_.baseCycle;
        if(header_.inputCount&&input_[header_.inputCount-1].offset>offset)return false;
        if(header_.dmcCount&&dmc_[header_.dmcCount-1].offset>offset)return false;
        if(header_.expansionCount){
            const auto& tail=expansion_[header_.expansionCount-1];
            if(tail.offset>offset||tail.nativePrefix>header_.inputCount||!validExpansionLevel(tail.level))return false;
        }
        return true;
    }
        /**
     * @brief Проверяет pure запас до80 реальных M2 с максимум одной DAC-публикацией на M2.
     * @param first Первая publish boundary в CPU clocks, до последующего audio tick.
     * @param clocks Полный ограниченный интервал; записи CPU и DMC запрещает caller.
     * @return false не меняет credits/fault и требует прежнего scalar исполнения.
     */
    bool canAppendMmc5Quiet(std::uint64_t first,unsigned clocks) const noexcept {
        return clocks>=8U&&clocks<=80U&&canAppendMmc5H2(first)&&
            expansionLimit_-header_.expansionCount>=clocks&&
            first-header_.baseCycle<=65536U-clocks;
    }

#endif

    /**
     * @brief Проверяет представимость timestamp внутри текущего аудиоинтервала.
     * @details При выходе за допустимое смещение защёлкивает Timestamp вместо усечения времени.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool validTime(std::uint64_t cycle) noexcept {
        if(cycle<header_.baseCycle||cycle-header_.baseCycle>65535){header_.fault=AudioFault::Timestamp;return false;}return true;
    }
    Header header_{};
    unsigned inputLimit_=inputCapacity,dmcLimit_=dmcCapacity;
    std::array<Input,inputCapacity> input_{};
    std::array<Dmc,dmcCapacity> dmc_{};
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
    unsigned expansionLimit_=expansionCapacity;
    std::array<Expansion,expansionCapacity> expansion_{};
#endif
};
/**
 * @brief Сравнивает все девять полей идентичности AudioPacket header.
 * @param a Первый неизменный заголовок.
 * @param b Второй неизменный заголовок с сопоставляемыми generation, sequence, cut, событиями, регионом и fault.
 * @return true при равенстве полей; не сравнивает payload/DSP и не подтверждает park владельца.
 */
inline constexpr bool sameAudioHeader(const AudioPacket::Header&a,const AudioPacket::Header&b)noexcept{
    return a.generation==b.generation&&a.sequence==b.sequence&&a.ppuFrame==b.ppuFrame&&
        a.baseCycle==b.baseCycle&&a.endCycle==b.endCycle&&a.inputCount==b.inputCount&&
        a.dmcCount==b.dmcCount&&a.region==b.region&&a.fault==b.fault
#if defined(NES_CORE_EXPANSION_DAC_PACKET) && NES_CORE_EXPANSION_DAC_PACKET
        &&a.formatVersion==b.formatVersion&&a.capabilities==b.capabilities&&a.expansionCount==b.expansionCount
#endif
        ;
}
} // пространство имён nes::core
