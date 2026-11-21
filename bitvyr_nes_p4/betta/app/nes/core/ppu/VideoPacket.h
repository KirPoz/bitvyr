/**
 * @file VideoPacket.h
 * @brief Пакет старого fetch/draw-протокола с компактными физическими ссылками.
 * @ingroup nes_ppu
 */
#pragma once
#if defined(NES_CORE_PPU_OUTPUT_SPRITE_ROWS) && NES_CORE_PPU_OUTPUT_SPRITE_ROWS
#endif
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nes::core {

enum class VideoFault : std::uint8_t {
    None, BadBinding, Capacity, InvalidEvent, InvalidSequence, InvalidReference,
    MissingSeed, ObserverFailure, UnsupportedMapper
#if defined(NES_CORE_LEGACY_PPU_PIXELS_HEAP) && NES_CORE_LEGACY_PPU_PIXELS_HEAP
    , NoMemory
#endif
};
enum class VideoRenderResult : std::uint8_t {
    Ok, InvalidPacket, InvalidSequence, InvalidReference, MissingSeed, OutputUnavailable
};

// Физические источники байтов: отображение разрешено на CPU1 ДО read-строба.
// CPU0 не вызывает Cartridge или живую Machine. RAM-ссылки разрешаются по
// постоянной consumer-тени в этой точной позиции потока событий.
/**
 * @brief Кодирование физического источника байта: ROM, CHR RAM, nametable или замороженный литерал. Смещение не переопределяется будущим банком.
 * @ingroup nes_ppu
 */
struct VideoByteRef {
    static constexpr std::uint32_t kindMask = 0xC0000000U;
    static constexpr std::uint32_t offsetMask = 0x3FFFFFFFU;
    static constexpr std::uint32_t rom = 0;
    static constexpr std::uint32_t chrRam = 0x40000000U;
    static constexpr std::uint32_t nametable = 0x80000000U;
    static constexpr std::uint32_t literal = 0xC0000000U;
};

enum class VideoOp : std::uint8_t {
    NameWrite, ChrWrite, PaletteWrite,
    BgAttribute, BgLow, BgHigh, BgLoad, BgShift,
    SpriteMeta, SpriteLow, SpriteHigh, VisibleLine,
    Draw, Fill, FrameEnd,
    BgRun,
    SpriteTile, PreloadTile,
    BgView, BgSpan
#if defined(NES_CORE_PPU_OUTPUT_SPRITE_ROWS) && NES_CORE_PPU_OUTPUT_SPRITE_ROWS
    , SpriteRow
#endif
    , Mmc5Seed = 241, Mmc5PhysicalWrite = 242, Mmc5Span = 243
    , PackedLiteralBg = 240 // Стабильный номер нового literal-протокола.
};

// Формат tag: op[7:0], arg0[15:8], arg1[23:16], arg2[31:24].
// Определяющим является хронологический порядок вставки, включая CPU-записи
// между low/high-выборками. CPU0 не выводит данные из более позднего live-образа памяти.
/**
 * @brief Восьмибайтовая запись старого видеопротокола: tag с op/тремя аргументами и value. Bulk-payload может содержать raw-слова вместо op.
 * @ingroup nes_ppu
 */
struct VideoEvent {
    std::uint32_t tag = 0, value = 0;
    /**
     * @brief Извлекает поле из упакованной записи видеособытия.
     * @details Формат и смысл поля определяются видом события; извлечение не исполняет обращение повторно.
     * @return Вычисленное значение или состояние типа VideoOp согласно описанному контракту.
     */
    VideoOp op() const noexcept { return static_cast<VideoOp>(tag & 255U); }
    /**
     * @brief Извлекает поле из упакованной записи видеособытия.
     * @details Формат и смысл поля определяются видом события; извлечение не исполняет обращение повторно.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned arg0() const noexcept { return (tag >> 8) & 255U; }
    /**
     * @brief Извлекает поле из упакованной записи видеособытия.
     * @details Формат и смысл поля определяются видом события; извлечение не исполняет обращение повторно.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned arg1() const noexcept { return (tag >> 16) & 255U; }
    /**
     * @brief Извлекает поле из упакованной записи видеособытия.
     * @details Формат и смысл поля определяются видом события; извлечение не исполняет обращение повторно.
     * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
     */
    unsigned arg2() const noexcept { return tag >> 24; }
};
static_assert(sizeof(VideoEvent) == 8, "bounded portable event format");

/**
 * @brief Ограниченный пакет старого раздельного видеоконвейера с физическими CHR-ссылками и метаданными; не контейнер физического LCD-DMA.
 * @ingroup nes_ppu
 */
class VideoPacket final {
public:
    static constexpr std::size_t eventCapacity = 131072;
    /** @brief Версия, разрешающая ровно один новый literal opcode. */
    static constexpr std::uint8_t literalProtocol = 1;
    enum class State : std::uint8_t { Free, Recording, Sealed };
    /**
     * @brief Заголовок старого видеопакета: поколение, последовательность, кадровый срез и число полностью опубликованных event-слотов.
     * @ingroup nes_ppu
     */
    struct Header {
        std::uint64_t generation = 0, sequence = 0;
        std::uint64_t startCycle = 0, endCycle = 0, ppuFrame = 0;
        const std::uint8_t* chrRom = nullptr;
        std::uint32_t chrRomBytes = 0, eventCount = 0;
        bool pal = false, seedPresent = false;
        VideoFault fault = VideoFault::None;
        std::uint8_t protocol = 0; // 0: прежние op; 1: разрешён PackedLiteralBg.
    };
    /**
     * @brief Небольшое изменяемое состояние writer без event-арены и CHR-RAM seed.
     * @details При VIDEO_HOT_METADATA вызывающий может разместить его во внутренней
     * памяти. Единственный writer меняет поля до Sealed; consumer читает Sealed
     * после прежнего release/acquire. owner защищает от двух одновременных заимствований,
     * но не является межпоточной блокировкой. Объект переживает пакет до actual join.
     */
    struct HotState final {
        Header header{};
        std::array<VideoEvent, 3> literalPrefix{};
        std::uint32_t legacyCount = 0;
        std::uint8_t literalPrefixCount = 0;
        bool mmc5SpanReady=false; // RUN-only immutable seed текущего sequence.
        State state = State::Free;
        const VideoPacket* owner = nullptr;
    };
    static_assert(sizeof(HotState) <= 192, "two video hot states must fit 384 bytes");
    /**
     * @brief Создаёт ограниченное хранилище видеособытий.
     * @details Ёмкость и источник записей не растут во время гостевого исполнения; внешний буфер, если он передан, остаётся у вызывающего.
     */
    VideoPacket() noexcept = default;
    /**
     * @brief Освобождает только запись владения заимствованных метаданных.
     * @details Вызывающий предварительно завершает worker/consumer и удерживает
     * внешний HotState живым до этого деструктора. Event- и ROM-буферы не освобождаются.
     */
    ~VideoPacket() noexcept {
    }
    /**
     * @brief Переключает Free-пакет на отдельное состояние writer либо обратно на локальное.
     * @param external Выравненный сконструированный HotState вне пакета; nullptr отменяет заимствование.
     * @return false при занятом пакете, alias, чужом owner или выключенном опыте с ненулевым external.
     * @details Переносит только метаданные один раз во время bind, без копии кадра/ROM
     * и без heap. Прежний Free-заголовок сохраняется. bind не выполняют параллельно
     * с читателем/писателем; begin/abandon сами аренду не отсоединяют.
     */
    bool bindHotState(HotState* external) noexcept {
        if (hot().state != State::Free) return false;
        return external == nullptr;
    }
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    VideoPacket(const VideoPacket&) = delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    VideoPacket& operator=(const VideoPacket&) = delete;
    /**
     * @brief Предоставляет неизменяемый заголовок опубликованного пакета.
     * @details Ссылка заимствована. Писатель не изменяет заголовок, пока пакет принадлежит читателю; внутренний объект нельзя использовать как самостоятельный снимок.
     * @return Ссылка на метаданные пакета; владение и срок жизни остаются у пакета.
     */
    [[nodiscard]] const Header& header() const noexcept { return hot().header; }
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Вычисленное значение или состояние типа State согласно описанному контракту.
     */
    [[nodiscard]] State state() const noexcept { return hot().state; }
    /**
     * @brief Возвращает текущее поле или заимствованное представление записи владельца.
     * @details Чтение не передаёт владение: Main читает опубликованные неизменяемые данные, а активный писатель работает только на своём потоке.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const VideoEvent* events() const noexcept { return events_.data(); }
    /** @brief Возвращает занятые старые capacity-credits, а не длину сжатого потока.
     * @return Число8-byte slots старого формата; физические slots находятся в Header. */
    unsigned legacyEventCount() const noexcept {
#if defined(NES_RESEARCH_MMC5_INPLACE_SPAN) && NES_RESEARCH_MMC5_INPLACE_SPAN
        // Proto0 span сохраняет старую capacity границу при меньшем physical payload.
        if(hot().header.protocol==0U)return hot().header.eventCount+hot().legacyCount;
#endif
        return hot().header.eventCount;
    }
    /**
     * @brief Материализует префикс перед чтением Recording-пакета на потоке владельца.
     * @details Не разрешает Main читать живой Worker; Sealed не изменяется.
     * Вызывать ДО передачи const reader; Sealed ветка не пишет ничего.
     * @return false при повреждённом scratch; старый sticky fault сохраняется.
     */
    bool readRecording() noexcept {
        return hot().state==State::Recording?flushLiteralPrefix():hot().literalPrefixCount==0U;
    }
    /** @brief Чисто проверяет наличие ещё не опубликованного producer-префикса. */
    bool literalPrefixPending() const noexcept {
        return hot().literalPrefixCount!=0U;
    }
    /**
     * @brief Задаёт либо выдаёт начальный образ CHR-RAM для повторного вывода.
     * @details Дальнейшие изменения отображаются событиями/литералами; seed нельзя заменять будущим состоянием RAM.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    [[nodiscard]] const std::uint8_t* chrRamSeed() const noexcept { return chrRamSeed_.data(); }
    // Только CPU0/владелец ПОСЛЕ окончания всех чтений рендерера; не при Recording.
    /**
     * @brief Возвращает уже потреблённый слот производителю.
     * @details Не даёт права освобождать ROM или другие буферы, которые всё ещё заимствует активный актор.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool release() noexcept { if (hot().state != State::Sealed) return false; hot().state = State::Free; return true; }

    // Только producer. begin повторно использует хранилище БЕЗ очистки большого массива.
#if defined(NES_CORE_PPU_OUTPUT_SPRITE_ROWS) && NES_CORE_PPU_OUTPUT_SPRITE_ROWS
    /**
     * @brief Записывает восемь уже выбранных literal sprite-единиц строки без новых шинных чтений.
     * @tparam Unit Тип записи с low/high/x/attributes/index/valid; представление C++-структуры не копируется.
     * @param row Видимая строка0..239 в прежней позиции VisibleLine.
     * @param units Восемь неизменяемых текущих единиц в исходном slot-порядке; low/high уже post-flip.
     * @param clocks Эффективный отложенный sprite-сдвиг, не более65535; долг не применяется к C1.
     * @return true после полной записи7 слотов; отказ сохраняет прежний eventCount и защёлкивает fault.
     */
    template<class Unit>
    bool addSpriteRow(unsigned row,const std::array<Unit,8>& units,unsigned clocks) noexcept {
        if(!flushLiteralPrefix())return false;
        if(hot().state!=State::Recording||hot().header.fault!=VideoFault::None)return false;
        if(row>=240U||clocks>65535U){fault(VideoFault::InvalidEvent);return false;}
        if(!reserveEvents(7U))return false;
        const unsigned at=hot().header.eventCount;
        events_[at]={static_cast<std::uint32_t>(VideoOp::SpriteRow)|(row<<8)|(8U<<16),clocks};
        auto* bytes=reinterpret_cast<unsigned char*>(events_.data()+at+1U);
        for(unsigned i=0;i<8;++i){const auto&u=units[i];auto* b=bytes+i*6U;
            b[0]=u.low;b[1]=u.high;b[2]=u.x;b[3]=u.attributes;b[4]=u.index;b[5]=u.valid?1U:0U;
        }
        hot().header.eventCount=at+7U;return true;
    }
#endif
    /**
     * @brief Начинает новый интервал или привязку объекта.
     * @details Сохраняет исходную идентичность и сбрасывает связанные счётчики/ошибки только в рамках явно разрешённого жизненного цикла.
     * @param generation Поколение данного протокола; не подменяется эпохой другого тракта.
     * @param sequence Последовательный номер интервала; не номер физической подачи LCD.
     * @param start Начальная позиция ограниченного участка вывода.
     * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
     * @param rom Заимствованные байты того же ROM, к которому относится текущая эпоха.
     * @param bytes Доступная длина переданного хранилища в байтах.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool begin(std::uint64_t generation, std::uint64_t sequence, std::uint64_t start,
               bool pal, const std::uint8_t* rom, std::uint32_t bytes) noexcept {
        if (hot().state != State::Free) return false;
        hot().literalPrefixCount=0;hot().legacyCount=0;hot().mmc5SpanReady=false;
        hot().header = {}; hot().header.generation = generation; hot().header.sequence = sequence;
        hot().header.startCycle = hot().header.endCycle = start; hot().header.pal = pal;
        hot().header.chrRom = rom; hot().header.chrRomBytes = bytes; hot().state = State::Recording;
        return true;
    }
    /**
     * @brief Задаёт либо выдаёт начальный образ CHR-RAM для повторного вывода.
     * @details Дальнейшие изменения отображаются событиями/литералами; seed нельзя заменять будущим состоянием RAM.
     * @param bytes Доступная длина переданного хранилища в байтах.
     */
    void setSeed(const std::uint8_t* bytes) noexcept {
        if(!flushLiteralPrefix())return;
        for (std::size_t i = 0; i < chrRamSeed_.size(); ++i) chrRamSeed_[i] = bytes[i];
        hot().header.seedPresent = true;
    }
    /**
     * @brief Обновляет текущий CPU/frame-курсор записываемого пакета.
     * @details Курсор отражает уже завершённую работу и используется проверками хронологии.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @param frame Номер согласованного гостевого PPU-кадра.
     */
    void cursor(std::uint64_t cycle, std::uint64_t frame) noexcept {
        if(!flushLiteralPrefix())return;
        hot().header.endCycle = cycle; hot().header.ppuFrame = frame;
    }
    /**
     * @brief Возвращает текущую ошибку соответствующего тракта.
     * @details Первичная ошибка сохраняется до установленного reset/begin; отсутствие ошибки не означает завершения физического вывода.
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     */
    void fault(VideoFault value) noexcept { static_cast<void>(flushLiteralPrefix()); if (hot().header.fault == VideoFault::None) hot().header.fault = value; }
    /**
     * @brief Добавляет или извлекает компактное видеособытие текущего пакета.
     * @details Контролирует формат и ограниченную ёмкость; переполнение является ошибкой, а не разрешением удалить запись.
     * @param op Уже декодированная CPU-операция либо вид упакованного события.
     * @param arg0 Младший восьмибитный аргумент события, упаковываемый в tag[15:8]; смысл задаёт op.
     * @param arg1 Второй восьмибитный аргумент события, упаковываемый в tag[23:16].
     * @param arg2 Третий восьмибитный аргумент события, упаковываемый в tag[31:24].
     * @param value Уже полученный либо записываемый байт; повторное шинное чтение здесь не подразумевается.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool add(VideoOp op, unsigned arg0 = 0, unsigned arg1 = 0,
             unsigned arg2 = 0, std::uint32_t value = 0) noexcept {
        if (hot().state != State::Recording || hot().header.fault != VideoFault::None) return false;
        if (legacyEventCount() == eventCapacity) { fault(VideoFault::Capacity); return false; }
        events_[hot().header.eventCount++] = {static_cast<std::uint32_t>(op) |
            ((arg0 & 255U) << 8) | ((arg1 & 255U) << 16) | ((arg2 & 255U) << 24), value};
        return true;
    }
    // Обычный компактный путь. Header arg0=1..32 тайлов; value=x|y<<8|
    // mask<<16|fineX<<24. Следующие ceil(count/2) слотов содержат сырые uint32-токены
    // первый в tag, второй в value, а НЕ опкоды. Младшие биты address-токена:
    // [28:0]=physical low-byte offset, [29]=CHR-RAM, иначе неизменяемый ROM,
    // [31:30]=двухбитный attribute. High-байт имеет ТО ЖЕ physical offset+8.
    // Ни CPU-запись, ни смена отображения не разделяет low/high-пару этого пути.
    /**
     * @brief Публикует фоновый run/span или описание его банков.
     * @details Координаты, fineX, исходный v и отображения относятся к одному согласованному интервалу.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param mask Снимок PPUMASK для этого растрового интервала.
     * @param fineX Тонкий горизонтальный сдвиг PPU, 0..7.
     * @param tokens Массив компактных физических ссылок выбранных тайлов; сырые слова не являются op-кодами.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool addBgRun(unsigned tiles, unsigned x, unsigned y, unsigned mask,
                  unsigned fineX, const std::uint32_t* tokens) noexcept {
        if(!flushLiteralPrefix())return false;
        if (hot().state != State::Recording || hot().header.fault != VideoFault::None) return false;
        if (!tiles || tiles > 32 || x + tiles * 8 > 256 || y >= 240 || fineX > 7 || !tokens) {
            fault(VideoFault::InvalidEvent); return false;
        }
        const unsigned slots = 1U + (tiles + 1U) / 2U;
        if(!reserveEvents(slots))return false;
        const auto at = hot().header.eventCount;
        events_[at] = {static_cast<std::uint32_t>(VideoOp::BgRun) | (tiles << 8),
            x | (y << 8) | ((mask & 255U) << 16) | (fineX << 24)};
        std::memcpy(static_cast<void*>(events_.data() + at + 1U), tokens, tiles * sizeof(std::uint32_t));
        if (tiles & 1U) events_[at + slots - 1U].value = 0;
        hot().header.eventCount += slots; // Опубликовать полный bulk-payload один раз.
        return true;
    }
#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
    // View содержит физические источники, не указатели внутрь живого маппера.
    // Шесть слотов: reserved-header, четыре packed CIRAM offsets, восемь CHR1K bases.
    // Поколению нужен первый view; следующие sequence пакетов сохраняют его.
    /**
     * @brief Публикует фоновый run/span или описание его банков.
     * @details Координаты, fineX, исходный v и отображения относятся к одному согласованному интервалу.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param names Четыре физических CIRAM-offset страниц имён с сохранением всех алиасов.
     * @param chr Восемь физических CHR1K-баз; живой mapper на CPU0 не запрашивается.
     */
    bool addBgView(const std::array<std::uint16_t,4>& names,
                   const std::array<std::uint32_t,8>& chr) noexcept {
        if(!flushLiteralPrefix())return false;
        if(hot().state!=State::Recording||hot().header.fault!=VideoFault::None)return false;
        for(auto page:names)if(page>3072U||(page&1023U)){
            fault(VideoFault::InvalidReference);return false;
        }
        for(auto ref:chr){
            const auto kind=ref&VideoByteRef::kindMask,offset=ref&VideoByteRef::offsetMask;
            const auto size=kind==VideoByteRef::chrRam?static_cast<std::uint32_t>(chrRamSeed_.size()):hot().header.chrRomBytes;
            if((kind!=VideoByteRef::rom&&kind!=VideoByteRef::chrRam)||(offset&1023U)||
               (kind==VideoByteRef::rom&&!hot().header.chrRom)||size<1024U||offset>size-1024U){
                fault(VideoFault::InvalidReference);return false;
            }
        }
        if(!reserveEvents(6U))return false;
        const auto at=hot().header.eventCount;
        events_[at]={static_cast<std::uint32_t>(VideoOp::BgView),0};
        events_[at+1]={std::uint32_t(names[0])|(std::uint32_t(names[1])<<16),
                       std::uint32_t(names[2])|(std::uint32_t(names[3])<<16)};
        for(unsigned i=0;i<4;++i)events_[at+2+i]={chr[i*2],chr[i*2+1]};
        hot().header.eventCount+=6; // Публиковать только полностью инициализированный view.
        return true;
    }
    // Полные phase0-видимые тайлы со стабильным отображением без CPU/наблюдателя
    // внутри. CPU0 читает упорядоченные NT/CHR-тени в этой позиции потока.
    /**
     * @brief Публикует фоновый run/span или описание его банков.
     * @details Координаты, fineX, исходный v и отображения относятся к одному согласованному интервалу.
     * @param tiles Число полных восьмипиксельных тайлов текущего ограниченного интервала.
     * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param mask Снимок PPUMASK для этого растрового интервала.
     * @param fineX Тонкий горизонтальный сдвиг PPU, 0..7.
     * @param v Сохранённый loopy-регистр v с coarse/fine-полями, а не произвольный CPU-адрес.
     * @param control Снимок PPUCTRL, в том числе выбор таблицы паттернов.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool addBgSpan(unsigned tiles,unsigned x,unsigned y,unsigned mask,
                   unsigned fineX,unsigned v,unsigned control) noexcept {
        if(!flushLiteralPrefix())return false;
        if(hot().state!=State::Recording||hot().header.fault!=VideoFault::None)return false;
        if(!tiles||tiles>32||x>255U||(x&7U)||x+tiles*8>256||y>=240||mask>255U||fineX>7||v>0x7FFFU||control>255U){
            fault(VideoFault::InvalidEvent);return false;
        }
        if(!reserveEvents(2U))return false;
        const auto at=hot().header.eventCount;
        events_[at]={static_cast<std::uint32_t>(VideoOp::BgSpan)|(tiles<<8),
                     x|(y<<8)|((mask&255U)<<16)|(fineX<<24)};
        events_[at+1]={v|(control<<16),0};
        hot().header.eventCount+=2;
        return true;
    }
#endif
    // Полная observer-free sprite-fetch-группа. Два слота: metadata-header,
    // затем {compact CHR low reference, reserved zero}. Сохранить каждый исходный
    // байт метаданных, включая invalid-единицы и index255. Fetch high=low+8;
    // записи/смены отображения или наблюдатель МЕЖДУ чтениями требуют
    // прежних SpriteMeta/Low/High-событий. Этот op не рисует.
    /**
     * @brief Публикует тайл спрайта либо предварительной загрузки с сохранённой физической идентичностью.
     * @details Порядок записей и место потребления должны совпасть с исходным конвейером.
     * @param slot Индекс текущего sprite-слота в исходном OAM-порядке.
     * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param attributes Байт атрибутов выбранного спрайта: palette, priority и отражения.
     * @param index Индекс текущего sprite-слота в исходном OAM-порядке.
     * @param valid Признак действительной sprite-записи; недействительные слоты всё равно сохраняют метаданные и выборки.
     * @param lowReference Приёмник физической идентичности младшей плоскости до её read-строба.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool addSpriteTile(unsigned slot, unsigned x, unsigned attributes,
                       unsigned index, bool valid, std::uint32_t lowReference) noexcept {
        if(!flushLiteralPrefix())return false;
        if (hot().state != State::Recording || hot().header.fault != VideoFault::None) return false;
        if (slot >= 8 || x > 255 || attributes > 255 || index > 255) {
            fault(VideoFault::InvalidEvent); return false;
        }
        std::uint32_t token = 0;
        if (!tileReference(lowReference, token)) { fault(VideoFault::InvalidReference); return false; }
        if(!reserveEvents(2U))return false;
        const auto at = hot().header.eventCount;
        events_[at] = {static_cast<std::uint32_t>(VideoOp::SpriteTile) | (slot << 8),
            x | (attributes << 8) | (index << 16) | (static_cast<unsigned>(valid) << 24)};
        events_[at + 1] = {token, 0};
        hot().header.eventCount += 2; // Не показывать header без payload.
        return true;
    }
    // Полный восьмиточечный background-preload: загрузить СТАРЫЙ next, shift8, сохранить
    // НОВЫЕ low/high/attribute. Без Draw и sprite-clock. Неполные группы сохраняют
    // исходные операции, включая любой наблюдаемый извне префикс.
    /**
     * @brief Публикует тайл спрайта либо предварительной загрузки с сохранённой физической идентичностью.
     * @details Порядок записей и место потребления должны совпасть с исходным конвейером.
     * @param lowReference Приёмник физической идентичности младшей плоскости до её read-строба.
     * @param attribute Уже выбранные атрибутные биты палитры фона, не позднее содержимое NT.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool addPreloadTile(std::uint32_t lowReference, unsigned attribute) noexcept {
        if(!flushLiteralPrefix())return false;
        if (hot().state != State::Recording || hot().header.fault != VideoFault::None) return false;
        if (attribute > 3) { fault(VideoFault::InvalidEvent); return false; }
        std::uint32_t token = 0;
        if (!tileReference(lowReference, token)) { fault(VideoFault::InvalidReference); return false; }
        if(!reserveEvents(1U))return false;
        events_[hot().header.eventCount] = {static_cast<std::uint32_t>(VideoOp::PreloadTile), token | (attribute << 30)};
        ++hot().header.eventCount;
        return true;
    }
    /**
     * @brief Фиксирует конечный срез и публикует завершённые записи.
     * @details После release-перехода писатель не изменяет данные, пока потребитель не освободит слот.
     * @param cycle Абсолютный гостевой CPU-такт соответствующего обращения.
     * @param frame Номер согласованного гостевого PPU-кадра.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     */
    bool seal(std::uint64_t cycle, std::uint64_t frame) noexcept {
        if(!flushLiteralPrefix())return false;
        if (hot().state != State::Recording || hot().header.fault != VideoFault::None || cycle < hot().header.startCycle) return false;
        cursor(cycle, frame); hot().state = State::Sealed; return true;
    }
    // Только owner reset/load при простое worker и отсутствии consumer этого слота.
    /**
     * @brief Отказывается от незавершённого содержимого пакета при согласованной смене режима.
     * @details Не выдаёт частичный кадр потребителю и не освобождает внешние ресурсы.
     */
    void abandon() noexcept { static_cast<void>(flushLiteralPrefix()); hot().state = State::Free; hot().header = {}; hot().literalPrefixCount=0;hot().legacyCount=0; }
private:
    /** @brief Готовность RUN-only Span/View только текущего Recording кадра. */
    bool mmc5SpanReady() const noexcept { return hot().mmc5SpanReady; }
    /** @brief Pure отказ seed оставляет старый fetch-путь и все credits/fault. */
    bool tryMmc5SpanSeed(const std::uint8_t* names,const std::uint8_t* exram) noexcept {
        constexpr unsigned slots=641U;
        const auto used=legacyEventCount();
        if(hot().state!=State::Recording||hot().header.fault!=VideoFault::None||
           hot().header.protocol||hot().literalPrefixCount||hot().mmc5SpanReady||
           !names||!exram||!hot().header.chrRom||hot().header.chrRomBytes<1024U||
           hot().header.chrRomBytes>1024U*1024U||
           (hot().header.chrRomBytes&(hot().header.chrRomBytes-1U))||
           used>eventCapacity||slots>eventCapacity-used)return false;
        const auto at=hot().header.eventCount;
        events_[at]={static_cast<std::uint32_t>(VideoOp::Mmc5Seed)|(1U<<8U),5120U};
        auto* out=reinterpret_cast<std::uint8_t*>(events_.data()+at+1U);
        std::memcpy(out,names,4096U);std::memcpy(out+4096U,exram,1024U);
        hot().header.eventCount=at+slots;hot().mmc5SpanReady=true;return true;
    }
    /** @brief Записывает только уже принятый физический байт CIRAM/ExRAM. */
    bool addMmc5PhysicalWrite(unsigned physical,std::uint8_t value) noexcept {
        if(!hot().mmc5SpanReady)return true;
        if(physical>=3072U){fault(VideoFault::InvalidEvent);return false;}
        return add(VideoOp::Mmc5PhysicalWrite,physical&255U,physical>>8U,0,value);
    }
    /** @brief Публикует immutable view вместо токенов, сохраняя пять credits на tile. */
    bool addMmc5ViewSpan(unsigned tiles,unsigned x,unsigned y,unsigned mask,unsigned fine,
                         const std::array<std::uint32_t,14>& view) noexcept {
        if(!hot().mmc5SpanReady||tiles<2U||tiles>31U||hot().header.protocol||hot().literalPrefixCount){fault(VideoFault::InvalidSequence);return false;}
        const auto used=legacyEventCount();
        if(used>eventCapacity||5U*tiles>eventCapacity-used){fault(VideoFault::Capacity);return false;}
        if(hot().state!=State::Recording||hot().header.fault!=VideoFault::None)return false;
        if(x+tiles*8U>256U||y>=240U||fine>7U){fault(VideoFault::InvalidEvent);return false;}
        const auto at=hot().header.eventCount;
        events_[at]={static_cast<std::uint32_t>(VideoOp::Mmc5Span)|(tiles<<8U)|(1U<<16U),x|(y<<8U)|((mask&255U)<<16U)|(fine<<24U)};
        std::memcpy(static_cast<void*>(events_.data()+at+1U),view.data(),56U);
        hot().header.eventCount=at+8U;hot().legacyCount+=5U*tiles-8U;return true;
    }

    /**
     * @brief Даёт writer активные метаданные; при выключенном опыте нет pointer indirection.
     * @return Локальное либо заимствованное состояние того же единственного владельца.
     */
    HotState& hot() noexcept {
        return localHotState_;
    }
    /**
     * @brief Даёт reader опубликованные метаданные без копирования и без смены владельца.
     * @return Заимствованная const-ссылка; чтение живого worker по-прежнему запрещено.
     */
    const HotState& hot() const noexcept {
        return localHotState_;
    }
    friend class Machine;
        /** @brief Публикует только уже успешные old ops до любого наблюдаемого среза.
     * @return false при повреждённом scratch/индексе; первая ошибка не затирается. */
    bool flushLiteralPrefix() noexcept {
        if(!hot().literalPrefixCount)return true;
        if(hot().literalPrefixCount>3U||hot().header.eventCount>eventCapacity||
           eventCapacity-hot().header.eventCount<hot().literalPrefixCount){
            if(hot().header.fault==VideoFault::None)hot().header.fault=VideoFault::InvalidSequence;
            return false;
        }
        for(unsigned i=0;i<hot().literalPrefixCount;++i)events_[hot().header.eventCount++]=hot().literalPrefix[i];
        hot().literalPrefixCount=0;return true;
    }
    friend class TimingPpu;
    friend struct VideoSegmentWriterTestAccess;
#if defined(NES_RESEARCH_MMC5_INPLACE_SPAN) && NES_RESEARCH_MMC5_INPLACE_SPAN
    /** @brief Публикует N законченных BgRun tiles со старыми пятью credits на tile.
     * @details Только certified observer-free proto0 caller; старый Draw head и
     * последний literal tile остаются снаружи. ROM refs принадлежат packet lease.
     * @return false со sticky fault при нарушении capacity/формата; retry запрещён. */
    bool addMmc5InplaceRun(unsigned tiles,unsigned x,unsigned y,unsigned mask,
                           unsigned fineX,const std::uint32_t* tokens) noexcept {
        if(!tiles||tiles>31U||hot().header.protocol!=0U||hot().literalPrefixCount){
            fault(VideoFault::InvalidSequence);return false;
        }
        const unsigned slots=1U+(tiles+1U)/2U;
        const unsigned saved=5U*tiles-slots;
        const unsigned used=legacyEventCount();
        if(used>eventCapacity||5U*tiles>eventCapacity-used){fault(VideoFault::Capacity);return false;}
        if(!addBgRun(tiles,x,y,mask,fineX,tokens))return false;
        hot().legacyCount+=saved;return true;
    }
    /**
     * @brief Записывает уже завершённый MMC5-сегмент за одну проверку старых credits.
     * @details Формат и момент списания совпадают с addMmc5InplaceRun. После
     * проверки proto0/пустого scratch не вызывает общий flush/reserve повторно.
     * Доступен только владельцу TimingPpu; пакет не публикуется до seal/release.
     * @param tiles Число законченных тайлов 1..31, последний pending tile снаружи.
     * @param x Левый пиксель run; вместе с tiles должен помещаться в 256 пикселей.
     * @param y Видимая строка 0..239.
     * @param mask Зафиксированный PPUMASK; в поток попадают младшие восемь бит.
     * @param fineX Горизонтальная тонкая прокрутка 0..7.
     * @param tokens Заимствованный массив tiles физических CHR/attribute токенов.
     * @return false с прежним sticky fault при неверном формате/ёмкости.
     */
    bool addMmc5SegmentRun(unsigned tiles,unsigned x,unsigned y,unsigned mask,
                          unsigned fineX,const std::uint32_t* tokens) noexcept {
        if(!tiles||tiles>31U||hot().header.protocol!=0U||hot().literalPrefixCount){
            fault(VideoFault::InvalidSequence);return false;
        }
        const unsigned used=legacyEventCount();
        if(used>eventCapacity||5U*tiles>eventCapacity-used){fault(VideoFault::Capacity);return false;}
        if(hot().state!=State::Recording||hot().header.fault!=VideoFault::None)return false;
        if(x+tiles*8U>256U||y>=240U||fineX>7U||!tokens){
            fault(VideoFault::InvalidEvent);return false;
        }
        const unsigned slots=1U+(tiles+1U)/2U;
        const auto at=hot().header.eventCount;
        events_[at]={static_cast<std::uint32_t>(VideoOp::BgRun)|(tiles<<8U),
            x|(y<<8U)|((mask&255U)<<16U)|(fineX<<24U)};
        std::memcpy(static_cast<void*>(events_.data()+at+1U),tokens,tiles*sizeof(std::uint32_t));
        if(tiles&1U)events_[at+slots-1U].value=0;
        hot().header.eventCount=at+slots;
        hot().legacyCount+=5U*tiles-slots;
        return true;
    }
#endif
    /** @brief Pure old-credit admission для proto0; не flush-ит scratch, не резервирует и не latch-ит fault.
     * @return true при запасе6старых slots; false оставляет state/fault/credits без изменений. */
    bool canAppendMmc5H2() const noexcept {
        return hot().state==State::Recording&&hot().header.fault==VideoFault::None&&hot().header.protocol==0U&&
            hot().literalPrefixCount==0U&&legacyEventCount()<=eventCapacity&&
            eventCapacity-legacyEventCount()>=6U;
    }
    /**
     * @brief Проверяет worst-case legacy credits двух BG ops на dot и четырёх хвостовых ops.
     * @param dots Уже допущенные visible BG dots, максимум240, без перехода строки.
     * @return Pure отказ сохраняет packet и исходную фазу возможного capacity fault.
     */
    bool canAppendMmc5Quiet(unsigned dots) const noexcept {
        return dots>=24U&&dots<=240U&&hot().state==State::Recording&&
            hot().header.fault==VideoFault::None&&hot().header.protocol==0U&&!hot().literalPrefixCount&&
            legacyEventCount()<=eventCapacity&&eventCapacity-legacyEventCount()>=2U*dots+4U;
    }
    
    /** @brief Списывает прежнее число слотов на исходной add-фазе, не заранее.
     * @param slots Число прежних8-byte records успешного scalar/bulk append.
     * @return false и sticky Capacity на прежней фазе при нехватке credits. */
    bool reserveEvents(unsigned slots) noexcept {
        if(hot().header.eventCount>eventCapacity||hot().literalPrefixCount>3U||
           (hot().header.protocol==literalProtocol&&(hot().header.eventCount>hot().legacyCount||
            hot().literalPrefixCount>hot().legacyCount-hot().header.eventCount))){
            fault(VideoFault::InvalidSequence);return false;
        }
        const unsigned used=legacyEventCount();
        if(used>eventCapacity||slots>eventCapacity-used){fault(VideoFault::Capacity);return false;}
        if(hot().header.protocol==literalProtocol)hot().legacyCount+=slots;
        return true;
    }
        /**
     * @brief Кодирует физическую ссылку тайла для последующего вывода.
     * @details Ссылка относится к моменту fetch и должна пережить банковые изменения по правилам пакета/COW.
     * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param token Выходной компактный tile-токен; использовать только при успешной проверке физической low/high-пары.
     */
    bool tileReference(std::uint32_t reference, std::uint32_t& token) const noexcept {
        const auto kind = reference & VideoByteRef::kindMask;
        const auto offset = reference & VideoByteRef::offsetMask;
        if ((kind != VideoByteRef::rom && kind != VideoByteRef::chrRam) || offset > 0x1FFFFFF7U) return false;
        const auto size = kind == VideoByteRef::chrRam ? static_cast<std::uint32_t>(chrRamSeed_.size()) : hot().header.chrRomBytes;
        if ((kind == VideoByteRef::rom && !hot().header.chrRom) || size < 9 || offset > size - 9) return false;
        token = offset | (kind == VideoByteRef::chrRam ? 0x20000000U : 0);
        return true;
    }
    // Флаг VIDEO_HOT_METADATA публичный: pointer изменяет layout всего VideoPacket.
    HotState localHotState_{};
    std::array<std::uint8_t, 8192> chrRamSeed_{};
    std::array<VideoEvent, eventCapacity> events_{};
};

// Необязательная синхронная точка наблюдения. Вызывающий владеет Machine и
// consumer, в приложении CPU0 только при простое worker. Обратно Machine не вызывает.
/**
 * @brief Полиморфный потребитель согласованного видеопакета; результат доступен лишь после успешного observeVideo.
 * @ingroup nes_ppu
 */
class VideoObserver {
public:
    /**
     * @brief Завершает время жизни объекта с полиморфным либо владеющим контрактом.
     * @details Связанные внешние bus/ROM/буферы должны сохраняться до окончания всех активных пользователей.
     */
    virtual ~VideoObserver() = default;
    /**
     * @brief Доводит активный видеопакет и передаёт его наблюдателю.
     * @details При отказе наблюдателя защёлкивает ошибку; вызывающий не получает частичную картинку как успешный результат.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param prefix Неизменяемый на время вызова завершённый префикс старого видеопротокола; Machine обратно не вызывается.
     */
    virtual bool observeVideo(const VideoPacket& prefix) noexcept = 0;
    /**
     * @brief Возвращает заимствованный результат наблюдателя видеопакета.
     * @details Данные допустимы только после успешного наблюдения; не обозначают владение физическим LCD-буфером.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    virtual const std::uint8_t* indexedVideo() const noexcept = 0;
    /**
     * @brief Возвращает заимствованный результат наблюдателя видеопакета.
     * @details Данные допустимы только после успешного наблюдения; не обозначают владение физическим LCD-буфером.
     * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    virtual std::uint8_t videoEmphasis(unsigned row) const noexcept = 0;
};
} // пространство имён nes::core
