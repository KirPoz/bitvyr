/**
 * @file VideoRenderer.h
 * @brief Хранилище теней и контракт consumer, не читающего живую producer Machine.
 * @ingroup nes_ppu
 */
#pragma once
#include "VideoPacket.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace nes::core {
// Постоянная память вызывающего. Переживает закреплённый VideoRenderer и
// исключительно принадлежит этому consumer; не перемещать при привязке.
/**
 * @brief Постоянные caller-owned NT/CHR-тени и индексный кадр старого packet-consumer. Не перемещается и не меняет владельца, пока VideoRenderer привязан.
 * @ingroup nes_ppu
 */
struct VideoStorage final {
    std::array<std::uint8_t,4096> names{};
    std::array<std::uint8_t,1024> mmc5ExRam{}; // RUN-only consumer history, не primary.
    std::array<std::uint8_t,8192> chr{};
    std::array<std::uint8_t,256*240> indices{};
};
// Постоянный consumer только CPU0. Не обращается к живой Machine/Cartridge.
/**
 * @brief Потребитель VideoPacket, строящий индексное/RGB-изображение из опубликованных событий без второго исполнения CPU.
 * @ingroup nes_ppu
 */
class VideoRenderer final : public VideoObserver {
public:
    // Конструктор инициализирует заимствованные тени, в том числе повторно используемое хранилище.
    /**
     * @brief Создаёт потребитель индексных видеособытий и его собственное растровое состояние.
     * @details Изображение строится из опубликованного пакета, без повторного исполнения CPU производителя.
     * @param storage Постоянные изменяемые NT/CHR/индексные тени этого consumer; сохраняются по одному стабильному адресу.
     */
    explicit VideoRenderer(VideoStorage& storage) noexcept;
#if defined(NES_CORE_PPU_OUTPUT_ROW_SCRATCH) && NES_CORE_PPU_OUTPUT_ROW_SCRATCH
    /** @brief Явный выбор только полного sealed RGB-raster, без canonical framebuffer и Recording observer. */
    struct RgbRowScratch final {};
    /**
     * @brief Создаёт Output-only RGB-consumer с единственной рабочей индексной строкой.
     * @param storage Постоянные NT/CHR-тени; прежняя full indices capacity зарезервирована, но этим режимом не обслуживается.
     * @param mode Явный tag неизменяемого режима; не является сменой режима уже начатого consumer.
     * @details Renderer и его256B scratch должны принадлежать Main и находиться во внутренней памяти.
     * До первого render требуется неизменяемая RGB-привязка256x240, ограждённая от PPA.
     * Каждый sealed packet обязан дать ровно полный последовательный raster до FrameEnd.
     */
    VideoRenderer(VideoStorage& storage, RgbRowScratch mode) noexcept;
    /** @brief Сообщает наличие постоянной индексной плоскости; false не заменяется фиктивным указателем. */
    bool canonicalVideoAvailable() const noexcept { return !rowScratchMode_; }
    /**
     * @brief Подтверждает только полный успешно завершённый RGB-кадр нового режима.
     * @return true после sealed render с полным FrameEnd; partial/fault/retarget сбрасывают готовность.
     * @note Не заменяет аппаратный source fence, packet release или проверку ticket/cut вызывающего.
     */
    bool rgbRowScratchReady() const noexcept {
        return rowScratchMode_ && rgb_ && rasterComplete_ && !rasterPosition_ && frame_ &&
            !packetOpen_ && fault_ == VideoRenderResult::Ok;
    }
#endif
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    VideoRenderer(const VideoRenderer&) = delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    VideoRenderer& operator=(const VideoRenderer&) = delete;
    /**
     * @brief Запрещает копирование идентичности и заимствованных ресурсов этого владельца.
     * @details Удалённый метод не имеет исполняемого тела; перенос владения через обычное копирование недопустим.
     */
    VideoRenderer(VideoRenderer&&) = delete;
    /**
     * @brief Запрещает присваивание объекта с закреплённой шиной, идентичностью или заимствованными ресурсами.
     * @details Удалённый оператор не имеет исполняемого тела. Владение не передаётся обычным присваиванием.
     */
    VideoRenderer& operator=(VideoRenderer&&) = delete;
    /**
     * @brief Возвращает устройство в определённое исходное состояние.
     * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
     */
    void reset() noexcept;
    // Null-выход разрешает canonical-only status/observer-работу. Вызывающий
    // ставит PPA/LCD-fence ДО передачи ненулевого RGB-источника.
    /**
     * @brief Обслуживает дополнительный RGB565-вывод полного PPU.
     * @details Заимствованный выход и индексное хранилище имеют разные lifetime; приложение управляет аппаратными fences отдельно.
     * @param pixels Заимствованный RGB565 framebuffer256×240 либо nullptr для canonical-only работы; аппаратный читатель заранее ограждён.
     */
    void setRgb565Output(std::uint16_t* pixels) noexcept;
    /**
     * @brief Восстанавливает звук либо изображение из проверенного входного пакета.
     * @details Проверяет идентичность и временной порядок. Неполный или ошибочный пакет не является готовым результатом.
     * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
     * @param sealed Запечатанный полный видеопакет, сохраняемый до завершения всех чтений consumer.
     */
    VideoRenderResult render(const VideoPacket& sealed) noexcept;
    /**
     * @brief Доводит активный видеопакет и передаёт его наблюдателю.
     * @details При отказе наблюдателя защёлкивает ошибку; вызывающий не получает частичную картинку как успешный результат.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param recordingOrSealed Зафиксированный неизменяемый префикс Recording либо Sealed; producer не пишет наблюдаемый диапазон.
     */
    bool observeVideo(const VideoPacket& recordingOrSealed) noexcept override;
    /**
     * @brief Возвращает заимствованный результат наблюдателя видеопакета.
     * @details Данные допустимы только после успешного наблюдения; не обозначают владение физическим LCD-буфером.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    const std::uint8_t* indexedVideo() const noexcept override {
#if defined(NES_CORE_PPU_OUTPUT_ROW_SCRATCH) && NES_CORE_PPU_OUTPUT_ROW_SCRATCH
        if (rowScratchMode_) return nullptr;
#endif
        return indices_.data(); }
    /**
     * @brief Возвращает заимствованный результат наблюдателя видеопакета.
     * @details Данные допустимы только после успешного наблюдения; не обозначают владение физическим LCD-буфером.
     * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
     * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
     */
    std::uint8_t videoEmphasis(unsigned row) const noexcept override { return row < 240 ? emphasis_[row] : 0; }
    /**
     * @brief Предоставляет заимствованный индексный буфер текущего видеотракта.
     * @details У Machine сначала материализуются CPU/PPU-долги; observable-владелец возвращает nullptr. Указатель нельзя использовать параллельно с изменением владельца или после освобождения его хранилища.
     * @return Заимствованный указатель результата; срок жизни и допустимость определяются описанной привязкой.
     */
    const std::uint8_t* framebuffer() const noexcept {
#if defined(NES_CORE_PPU_OUTPUT_ROW_SCRATCH) && NES_CORE_PPU_OUTPUT_ROW_SCRATCH
        if (rowScratchMode_) return nullptr;
#endif
        return indices_.data(); }
    /**
     * @brief Возвращает идентификатор или временную границу текущего интервала.
     * @details Поколение, последовательность и гостевые такты нельзя подменять номером физической LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t generation() const noexcept { return generation_; }
    /**
     * @brief Возвращает идентификатор или временную границу текущего интервала.
     * @details Поколение, последовательность и гостевые такты нельзя подменять номером физической LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t sequence() const noexcept { return sequence_; }
    /**
     * @brief Возвращает номер PPU-кадра, которому соответствует текущий результат.
     * @details Не является номером LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t ppuFrame() const noexcept { return frame_; }
    /**
     * @brief Возвращает идентификатор или временную границу текущего интервала.
     * @details Поколение, последовательность и гостевые такты нельзя подменять номером физической LCD-подачи.
     * @return Вычисленное значение или состояние типа std::uint64_t согласно описанному контракту.
     */
    std::uint64_t endCycle() const noexcept { return cycle_; }
    // Редкое canonical-only наблюдение перед показом: материализовать
    // RGB из текущей тени без обращения к Machine или packet-истории.
    /**
     * @brief Обслуживает дополнительный RGB565-вывод полного PPU.
     * @details Заимствованный выход и индексное хранилище имеют разные lifetime; приложение управляет аппаратными fences отдельно.
     */
    void refreshRgb565() noexcept;
private:
#if defined(NES_CORE_PPU_OUTPUT_ROW_SCRATCH) && NES_CORE_PPU_OUTPUT_ROW_SCRATCH
    /**
     * @brief Проверяет очередной видимый span до записи RGB или scratch.
     * @param y Строка0..239, совпадающая с единственным текущим растровым курсором.
     * @param x Первый пиксель0..255; пропуски и повторные записи запрещены.
     * @param count Положительное число пикселей, не пересекающее границу строки.
     * @return true для прежнего canonical режима либо точного очередного RGB span; false запрещает публикацию.
     */
    bool beginRasterSpan(unsigned y, unsigned x, unsigned count) noexcept;
    const bool rowScratchMode_ = false;
    alignas(4) std::array<std::uint8_t,256> rowIndices_{};
    std::uint32_t rasterPosition_ = 0;
    bool rasterComplete_ = false;
#endif
    /**
     * @brief Проверяет и исполняет опубликованные записи текущей аренды.
     * @details Порядок времени, последовательность, реальные ответы чтений и конечный срез проверяются до готовности результата.
     * @param sealedOnly При true разрешает только уже запечатанный пакет; иначе действует предусмотренная проверка текущего состояния.
     * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
     */
    VideoRenderResult consume(const VideoPacket&, bool sealedOnly) noexcept;
    /**
     * @brief Исполняет одно хронологическое событие повторного PPU.
     * @details Для сохранённого чтения выполняет реальное чтение один раз и сравнивает ответ; ошибку нельзя исправлять повторной попыткой после побочных эффектов.
     * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
     */
    VideoRenderResult apply(const VideoEvent&) noexcept;

    /**
     * @brief Применяет уже проверенную четвёрку literal BG без повторного dispatch.
     * @details Старые next загружаются до новых; меняется только локальный renderer, не packet/producer.
     * @param shift Проверенный сдвиг атрибута:0,2,4,6; все shape/protocol guards остаются у caller.
     * @param attributeValue Проверенный literal byte в младших8битах; старшие биты не влияют на выбор2бит.
     * @param lowValue Проверенный low byte в младших8битах.
     * @param highValue Проверенный high byte в младших8битах.
     */
    inline void applyLiteralBackground(unsigned shift, std::uint32_t attributeValue,
        std::uint32_t lowValue, std::uint32_t highValue) noexcept {
        low_ = static_cast<std::uint16_t>((low_ & 0xFF00U) | nextLow_);
        high_ = static_cast<std::uint16_t>((high_ & 0xFF00U) | nextHigh_);
        attrLow_ = static_cast<std::uint16_t>((attrLow_ & 0xFF00U) |
            ((nextAttr_ & 1U) ? 255U : 0U));
        attrHigh_ = static_cast<std::uint16_t>((attrHigh_ & 0xFF00U) |
            ((nextAttr_ & 2U) ? 255U : 0U));
        nextAttr_ = static_cast<std::uint8_t>((attributeValue >> shift) & 3U);
        nextLow_ = static_cast<std::uint8_t>(lowValue);
        nextHigh_ = static_cast<std::uint8_t>(highValue);
    }
    /**
     * @brief Обслуживает последовательность полных фоновых тайлов в текущей строке.
     * @details Временной PPU может удержать только живой хвост при доказанном отсутствии нужного Sprite0-наблюдения; полный вывод сохраняет фактический выбор пикселей.
     * @param payload Неизменяемые raw-слоты после заголовка видеособытия; интерпретация и число задаются его форматом.
     * @return Ok после корректного исполнения операции либо конкретная ошибка формата/границ/источника; частичный кадр не публикуется.
     */
    VideoRenderResult backgroundRun(const VideoEvent&, const VideoEvent* payload) noexcept;
#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
    /**
     * @brief Рисует фон из опубликованных событий и сохранённого отображения страниц.
     * @details Проверяет физические ссылки и форму данных; потребитель не выполняет CPU-программу повторно.
     * @return Ok после полной проверки и установки нового физического view либо ошибка без частичной замены прежнего view.
     */
    VideoRenderResult backgroundView(const VideoEvent&,const VideoEvent*) noexcept;
    /**
     * @brief Рисует фон из опубликованных событий и сохранённого отображения страниц.
     * @details Проверяет физические ссылки и форму данных; потребитель не выполняет CPU-программу повторно.
     * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
     */
    VideoRenderResult backgroundSpan(const VideoEvent&,const VideoEvent&) noexcept;
#endif
    /**
     * @brief Применяет опубликованный sprite-тайл с его позицией, атрибутами и приоритетом.
     * @details Поздние события не меняют порядок OAM уже выбранных перекрывающихся спрайтов.
     * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
     * @param payload Неизменяемое тело этой операции; длина и интерпретация соответствуют проверенному заголовку.
     */
    VideoRenderResult spriteTile(const VideoEvent&, const VideoEvent& payload) noexcept;
    /**
     * @brief Разрешает опубликованную физическую ссылку на байт либо тайл.
     * @details ROM/RAM/буквальные виды и границы проверяются до чтения; текущая банковая карта производителя не заменяет сохранённую идентичность.
     * @param compactReference Компактная физическая ссылка low-плоскости; high=low+8 допустимо только доказанной полной паре.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param low Выходной исторический low-байт тайла, разрешённый по физической ссылке.
     * @param high Выходной исторический high-байт той же доказанной пары, без повторного выбора живого банка.
     */
    bool resolveTile(std::uint32_t compactReference, std::uint8_t& low, std::uint8_t& high) const noexcept;
    /**
     * @brief Разрешает опубликованную физическую ссылку на байт либо тайл.
     * @details ROM/RAM/буквальные виды и границы проверяются до чтения; текущая банковая карта производителя не заменяет сохранённую идентичность.
     * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
     * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
     * @param value Выходной байт фактического чтения; допустим только после успешного результата метода.
     */
    bool resolve(std::uint32_t reference, std::uint8_t& value) const noexcept;
    /**
     * @brief Доводит отложенное состояние спрайтов до текущего растрового среза.
     * @details Раздельные плоскости, flip и накопленные сдвиги относятся к моменту захвата, а не к новой метаинформации.
     */
    void materializeSprites() noexcept;
    /**
     * @brief Строит текущий кэш выбора спрайтов для видимой строки.
     * @details Порядок OAM и окклюзия ранними спрайтами сохраняются; отдельная маска Sprite0 не разрешает менять выигравший пиксель.
     */
    void buildSpriteRow() noexcept;
#if defined(NES_CORE_PPU_OUTPUT_SPRITE_ROWS) && NES_CORE_PPU_OUTPUT_SPRITE_ROWS
    /**
     * @brief Проверяет полный row-payload и заменяет только локальные sprite-единицы output-композитора.
     * @param header Заголовок7-слотовой команды с row/count/reserved/effectiveClocks.
     * @param payload Шесть уже проверенных на наличие raw-слотов, неизменяемых до возврата.
     * @return Ok после полного импорта; некорректный header/valid не меняет ни одной sprite-единицы.
     */
    VideoRenderResult importSpriteRow(const VideoEvent& header,const VideoEvent* payload) noexcept;
#endif
    /**
     * @brief Фиксирует метаданные emphasis соответствующей строки.
     * @details При mid-row изменении связанный RGB-выход исправляет уже выведенный префикс до наблюдаемой границы.
     * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
     * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
     */
    void setRowEmphasis(unsigned row, unsigned emphasis) noexcept;
    /**
     * @brief Сообщает привязанному RGB-выходу точный участок индексного вывода.
     * @details Контекст относится к тому же PPU и не удерживается через смену API/владельца.
     * @param row Номер строки видимого изображения, проверяемый или ограничиваемый вызывающим путём.
     * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     */
    void noteRgbSpan(unsigned row, unsigned x, unsigned count) noexcept;
    /**
     * @brief Обновляет локальные значения палитры после её фактической записи.
     * @details Инвалидирует связанные цветовые таблицы в исходном порядке, не перенося новое значение назад по времени.
     * @param mask Снимок PPUMASK для этого растрового интервала.
     */
    void cachePalette(unsigned mask) noexcept;
    /**
     * @brief Записывает предусмотренный текущим видеособытием участок вывода.
     * @details Сохраняет индексную плоскость, порядок перекрытия и ограничения видимых координат.
     * @param background Восемь уже выбранных фоновых индексов, по четыре бита на пиксель.
     * @param sprites Уже декодированная строка первых непрозрачных sprite-победителей; читается без изменения счётчиков OAM.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param mask Снимок PPUMASK для этого растрового интервала.
     * @param indexed Изменяемый участок индексного framebuffer; по одному байту на пиксель, срок жизни покрывает вызов.
     * @param rgb Буфер RGB565 вызывающего с согласованным сроком жизни и исключёнными аппаратными читателями.
     */
    template<bool Rgb> inline void writePixels(std::uint32_t background, const std::uint8_t* sprites,
        unsigned count, unsigned x, unsigned mask, std::uint8_t* indexed, std::uint16_t* rgb) const noexcept;
    /**
     * @brief Записывает предусмотренный текущим видеособытием участок вывода.
     * @details Сохраняет индексную плоскость, порядок перекрытия и ограничения видимых координат.
     * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
     * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
     * @param mask Снимок PPUMASK для этого растрового интервала.
     * @param fine Точный fine-X от 0 до 7 для выбора байтов текущего/следующего тайла.
     */
    template<bool Rgb> void draw(unsigned x, unsigned y, unsigned count, unsigned mask, unsigned fine) noexcept;
    /**
     * @brief Рисует фон из опубликованных событий и сохранённого отображения страниц.
     * @details Проверяет физические ссылки и форму данных; потребитель не выполняет CPU-программу повторно.
     * @return Ok после корректного рисования либо ошибка формата/границ/ссылки без права публикации частичного кадра.
     */
    template<bool Rgb> VideoRenderResult backgroundRunPixels(const VideoEvent&, const VideoEvent*
#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
        ,unsigned spanV=0,unsigned spanControl=0
#endif
        ) noexcept;
    /**
     * @brief Один выбранный OAM-слот: X/атрибуты/индекс, low/high-плоскости и признаки действительности. Порядок слотов сохраняет приоритет первого непрозрачного.
     * @ingroup nes_ppu
     */
    struct Sprite {std::uint8_t low=0,high=0,x=0,attributes=0,index=255;bool valid=false;};
    // Держать малое рабочее состояние вместе; большие тени — холодные потоки.
    std::uint16_t* rgb_ = nullptr;
    const VideoPacket* packet_ = nullptr;
    const std::uint8_t* rom_ = nullptr;
    std::uint32_t romBytes_ = 0, consumed_ = 0;
    std::uint64_t generation_ = 0, sequence_ = 0, frame_ = 0, cycle_ = 0;
    std::uint64_t startCycle_ = 0;
    std::uint16_t low_ = 0, high_ = 0, attrLow_ = 0, attrHigh_ = 0;
    std::uint8_t nextLow_ = 0, nextHigh_ = 0, nextAttr_ = 0;
    unsigned spriteClocks_ = 0;
    bool initialized_ = false, packetOpen_ = false, pal_ = false;
    bool spriteRowValid_ = false;
    bool mmc5Seeded_=false;
    VideoRenderResult fault_ = VideoRenderResult::Ok;
    bool paletteDirty_ = true;
    std::uint8_t paletteMask_ = 0;
    std::array<std::uint8_t,32> palette_{};
    std::array<Sprite,8> sprites_{};
    alignas(4) std::array<std::uint8_t,256> spriteRow_{};
    std::array<std::uint8_t,32> indexPalette_{};
    std::array<std::uint16_t,32> colourPalette_{};
    std::array<std::array<std::uint8_t,2>,256> backgroundPairs_{};
    alignas(4) std::array<std::array<std::uint16_t,2>,256> colourPairs_{};
    std::array<std::uint8_t,240> emphasis_{};
    std::array<std::uint16_t,240> rgbWritten_{};
#if defined(NES_CORE_PPU_BG_SPAN) && NES_CORE_PPU_BG_SPAN
    std::array<const std::uint8_t*,4> bgNames_{};
    std::array<const std::uint8_t*,8> bgChr_{};
    bool bgViewValid_=false;
#endif
    std::array<std::uint8_t,4096>& names_;
    std::array<std::uint8_t,8192>& chr_;
    std::array<std::uint8_t,256*240>& indices_;
    std::array<std::uint8_t,1024>& mmc5ExRam_;
    /** @brief Проверяет RUN grammar и восстанавливает tile RGB из consumer history. */
    VideoRenderResult mmc5SpanEvent(const VideoEvent&,const VideoEvent*,unsigned available,unsigned& slots) noexcept;
};
} // пространство имён nes::core
