/**
 * @file VideoRenderer.cpp
 * @brief Постоянный CPU0-consumer старого fetch/draw-протокола и RGB-материализация.
 * @ingroup nes_ppu
 */
#include "VideoRenderer.h"
#include "../Palette.h"
#include <algorithm>
#include <cstring>

namespace nes::core {

namespace {
/**
 * @brief Готовит таблицу цветов для предусмотренных сочетаний индекса и emphasis.
 * @details Использует общую палитру; изменение PPUMASK строки не подменяет прежний индексный выбор.
 * @return Вычисленное значение или состояние типа auto согласно описанному контракту.
 */
constexpr auto makeColours() noexcept {
    std::array<std::array<std::uint16_t, 64>, 8> table{};
    for (unsigned e = 0; e < 8; ++e) for (unsigned i = 0; i < 64; ++i) {
        const auto c = rgbPalette[i];
        unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        if (e & 1) { g = g * 3 / 4; b = b * 3 / 4; }
        if (e & 2) { r = r * 3 / 4; b = b * 3 / 4; }
        if (e & 4) { r = r * 3 / 4; g = g * 3 / 4; }
        table[e][i] = static_cast<std::uint16_t>(((r & 248) << 8) | ((g & 252) << 3) | (b >> 3));
    }
    return table;
}
constexpr auto colours = makeColours();
/**
 * @brief Формирует неизменяемые таблицы упакованных плоскостей или выбора пикселей.
 * @details Порядок битов соответствует исходному селектору; таблица не является изменяемой палитрой PPU.
 * @return Вычисленное значение или состояние типа auto согласно описанному контракту.
 */
constexpr auto makePlanes() noexcept {
    std::array<std::uint32_t,256> table{};
    for (unsigned byte=0;byte<256;++byte)for(unsigned bit=0;bit<8;++bit)
        if(byte&(0x80U>>bit))table[byte]|=1U<<(bit*4);
    return table;
}
constexpr auto expanded = makePlanes();
/**
 * @brief Формирует неизменяемые таблицы упакованных плоскостей или выбора пикселей.
 * @details Порядок битов соответствует исходному селектору; таблица не является изменяемой палитрой PPU.
 * @return Вычисленное значение или состояние типа auto согласно описанному контракту.
 */
constexpr auto makeSelections() noexcept {
    std::array<std::uint8_t,1024> table{};
    for(unsigned sprite=0;sprite<64;++sprite)for(unsigned bg=0;bg<16;++bg){
        const unsigned index=(sprite&3)&&(!(bg&3)||!(sprite&0x10))?16+(sprite&15):((bg&3)?bg:0);
        table[(sprite<<4)|bg]=static_cast<std::uint8_t>(index);
    }
    return table;
}
constexpr auto selection = makeSelections();
constexpr std::array<std::uint8_t,8> noSprites{};
/**
 * @brief Продвигает упакованную пару фоновых плоскостей на указанное число битов.
 * @details Сохраняет обе независимые плоскости и правила старшего/следующего фрагмента.
 * @param pair Восемь четырёхбитных фоновых индексов, сгруппированных в четыре пары для RGB-таблицы.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
inline std::uint32_t shiftPair(std::uint32_t pair,unsigned count) noexcept {
    return (((pair&0xFFFFU)<<count)&0xFFFFU)|((pair&0xFFFF0000U)<<count);
}
/**
 * @brief Предоставляет готовый заимствованный RGB-буфер после необходимого завершения хвоста.
 * @details Буфер и таблица пар остаются у вызывающего до unbind и завершения всех пользователей.
 * @param planes Упакованные low/high-байты текущего и следующего фоновых тайлов до выбора fine-X.
 * @param attrs Упакованные атрибутные сдвиговые регистры, соответствующие тем же фонoвым плоскостям.
 * @param fine Точный fine-X от 0 до 7 для выбора байтов текущего/следующего тайла.
 * @return Вычисленное значение или состояние типа std::uint32_t согласно описанному контракту.
 */
inline std::uint32_t pixels(std::uint32_t planes,std::uint32_t attrs,unsigned fine) noexcept {
    return expanded[(planes>>(8U-fine))&255U]|(expanded[(planes>>(24U-fine))&255U]<<1)|
        (expanded[(attrs>>(8U-fine))&255U]<<2)|(expanded[(attrs>>(24U-fine))&255U]<<3);
}
/**
 * @brief Выдаёт подготовленную цветовую строку для нужного emphasis.
 * @details Палитра и номер строки принадлежат текущему результату renderer, а не текущему банковому состоянию CPU.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 * @param pal Признак PAL для частот, фаз или цветового преобразования этого помощника.
 * @return Вычисленное значение или состояние типа unsigned согласно описанному контракту.
 */
unsigned colourRow(unsigned emphasis, bool pal) noexcept {
    return pal ? ((emphasis & 4) | ((emphasis & 1) << 1) | ((emphasis & 2) >> 1)) : emphasis;
}
/**
 * @brief Разворачивает порядок битов для горизонтального отражения либо индексирования таблицы.
 * @details Результат применяется к той плоскости и тому моменту fetch, для которых он был рассчитан.
 * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
 * @return Вычисленное значение или состояние типа std::uint8_t согласно описанному контракту.
 */
std::uint8_t reverse(std::uint8_t x) noexcept {
    x = static_cast<std::uint8_t>(((x & 0x55) << 1) | ((x >> 1) & 0x55));
    x = static_cast<std::uint8_t>(((x & 0x33) << 2) | ((x >> 2) & 0x33));
    return static_cast<std::uint8_t>((x << 4) | (x >> 4));
}
}

/**
 * @brief Создаёт потребитель индексных видеособытий и его собственное растровое состояние.
 * @details Изображение строится из опубликованного пакета, без повторного исполнения CPU производителя.
 * @param storage Постоянные изменяемые NT/CHR/индексные тени этого consumer; сохраняются по одному стабильному адресу.
 */
VideoRenderer::VideoRenderer(VideoStorage& storage) noexcept
    : names_(storage.names), chr_(storage.chr), indices_(storage.indices), mmc5ExRam_(storage.mmc5ExRam) {
    reset();
}

/**
 * @brief Закрепляет Output-only режим до сброса состояния, не создавая индексный canonical результат.
 * @details Тени и RGB-заём имеют прежних владельцев; новая рабочая строка является частью internal Renderer.
 * @param storage Неизменяемое по адресу caller-owned хранилище NT/CHR; full indices этого режима не читаются.
 * @param mode Явный tag выбора нового режима при создании объекта.
 */
VideoRenderer::VideoRenderer(VideoStorage& storage, RgbRowScratch mode) noexcept
    : rowScratchMode_(true), names_(storage.names), chr_(storage.chr), indices_(storage.indices), mmc5ExRam_(storage.mmc5ExRam) {
    static_cast<void>(mode);
    reset();
}

/**
 * @brief Проверяет точное продолжение raster до любых stores соответствующего span.
 * @details Scratch хранит resolved palette/grayscale indices, поэтому поздняя palette write не меняет старый prefix.
 * @param y Видимая строка 0..239.
 * @param x Начало текущего span внутри строки.
 * @param count Число пикселей без перехода через правый край.
 * @return false при отсутствии RGB, gap/reorder/overlap или выходе за 256x240; caller защёлкивает ошибку.
 */
bool VideoRenderer::beginRasterSpan(unsigned y, unsigned x, unsigned count) noexcept {
    if (!rowScratchMode_) return true;
    if (!rgb_ || fault_ != VideoRenderResult::Ok || y >= 240 || x >= 256 ||
        !count || count > 256U-x || rasterPosition_ >= 256U*240U ||
        rasterPosition_ != y*256U+x) return false;
    rasterComplete_ = false;
    return true;
}

/**
 * @brief Возвращает устройство в определённое исходное состояние.
 * @details Обнуляет соответствующие счётчики, защёлки и конвейеры; сброс Machine требует исключительного владения и не равен освобождению ROM или буферов приложения.
 */
void VideoRenderer::reset() noexcept {
    names_.fill(0); chr_.fill(0); palette_.fill(0);mmc5ExRam_.fill(0);mmc5Seeded_=false;
    sprites_.fill(Sprite{}); spriteRow_.fill(0); emphasis_.fill(0);
    rowIndices_.fill(0); rasterPosition_ = 0; rasterComplete_ = false;
    if (!rowScratchMode_) indices_.fill(0);
    rgbWritten_.fill(0);
    packet_ = nullptr; rom_ = nullptr; romBytes_ = consumed_ = 0;
    generation_ = sequence_ = frame_ = cycle_ = startCycle_ = 0;
    low_ = high_ = attrLow_ = attrHigh_ = 0;
    nextLow_ = nextHigh_ = nextAttr_ = 0; spriteClocks_ = 0;
    initialized_ = packetOpen_ = pal_ = spriteRowValid_ = false;
    fault_ = VideoRenderResult::Ok;
    paletteDirty_ = true; paletteMask_ = 0;
    bgViewValid_=false;
    // Reset не восстанавливает RGB из отсутствующей fullplane; нужен новый полный raster.
    if (rowScratchMode_) return;
    if (rgb_) refreshRgb565();
}

/**
 * @brief Обслуживает дополнительный RGB565-вывод полного PPU.
 * @details Заимствованный выход и индексное хранилище имеют разные lifetime; приложение управляет аппаратными fences отдельно.
 * @param pixels Заимствованный RGB565 framebuffer256×240 либо nullptr для canonical-only работы; аппаратный читатель заранее ограждён.
 */
void VideoRenderer::setRgb565Output(std::uint16_t* pixels) noexcept {
    // Смена или detach после первого consume требует явного exclusive reset при аппаратном fence.
    if (rowScratchMode_ && initialized_ && rgb_ != pixels) {
        fault_ = VideoRenderResult::InvalidSequence; rasterComplete_ = false; return;
    }
    if (rgb_ != pixels) {
        rgb_ = pixels;
        rgbWritten_.fill(0);
    }
}

/**
 * @brief Обслуживает дополнительный RGB565-вывод полного PPU.
 * @details Заимствованный выход и индексное хранилище имеют разные lifetime; приложение управляет аппаратными fences отдельно.
 */
void VideoRenderer::refreshRgb565() noexcept {
    if (rowScratchMode_) { fault_ = VideoRenderResult::InvalidSequence; rasterComplete_ = false; return; }
    if (!rgb_) return;
    for (unsigned y = 0; y < 240; ++y) {
        const auto& row = colours[colourRow(emphasis_[y], pal_)];
        for (unsigned x = 0; x < 256; ++x) rgb_[y * 256 + x] = row[indices_[y * 256 + x] & 63];
    }
    rgbWritten_.fill(256);
}

/**
 * @brief Разрешает опубликованную физическую ссылку на байт либо тайл.
 * @details ROM/RAM/буквальные виды и границы проверяются до чтения; текущая банковая карта производителя не заменяет сохранённую идентичность.
 * @param ref Кодированная физическая ссылка либо уже замороженный литерал; тип памяти задают старшие биты.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param value Выходной байт фактического чтения; допустим только после успешного результата метода.
 */
bool VideoRenderer::resolve(std::uint32_t ref, std::uint8_t& value) const noexcept {
    const unsigned offset = ref & VideoByteRef::offsetMask;
    switch (ref & VideoByteRef::kindMask) {
    case VideoByteRef::rom:
        if (!rom_ || offset >= romBytes_) return false;
        value = rom_[offset]; return true;
    case VideoByteRef::chrRam:
        if (offset >= chr_.size()) return false;
        value = chr_[offset]; return true;
    case VideoByteRef::nametable:
        if (offset >= names_.size()) return false;
        value = names_[offset]; return true;
    case VideoByteRef::literal:
        if (offset > 255) return false;
        value = static_cast<std::uint8_t>(offset); return true;
    }
    return false;
}

/**
 * @brief Разрешает опубликованную физическую ссылку на байт либо тайл.
 * @details ROM/RAM/буквальные виды и границы проверяются до чтения; текущая банковая карта производителя не заменяет сохранённую идентичность.
 * @param reference Сохранённая физическая либо буквальная идентичность байта; не текущий банковый адрес.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param low Выходной исторический low-байт тайла, разрешённый по физической ссылке.
 * @param high Выходной исторический high-байт той же доказанной пары, без повторного выбора живого банка.
 */
bool VideoRenderer::resolveTile(std::uint32_t reference, std::uint8_t& low, std::uint8_t& high) const noexcept {
    const auto offset = reference & 0x1FFFFFFFU;
    const bool ram = (reference & 0x20000000U) != 0;
    const auto* source = ram ? chr_.data() : rom_;
    const auto size = ram ? static_cast<std::uint32_t>(chr_.size()) : romBytes_;
    if (!source || size < 9 || offset > size - 9) return false;
    low = source[offset]; high = source[offset + 8];
    return true;
}

/**
 * @brief Применяет опубликованный sprite-тайл с его позицией, атрибутами и приоритетом.
 * @details Поздние события не меняют порядок OAM уже выбранных перекрывающихся спрайтов.
 * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
 * @param header Заголовок текущей операции старого видеопротокола с форматом и ограниченными координатами.
 * @param payload Неизменяемое тело этой операции; длина и интерпретация соответствуют проверенному заголовку.
 */
VideoRenderResult VideoRenderer::spriteTile(const VideoEvent& header, const VideoEvent& payload) noexcept {
    if (header.arg0() >= 8 || header.arg1() || header.arg2() || (header.value >> 25) ||
        (payload.tag & 0xC0000000U) || payload.value) return VideoRenderResult::InvalidPacket;
    std::uint8_t low = 0, high = 0;
    // Разрешить ОБА исторических байта до материализации или смены метаданных.
    if (!resolveTile(payload.tag, low, high)) return VideoRenderResult::InvalidReference;
    materializeSprites();
    auto& sprite = sprites_[header.arg0()];
    sprite.x = static_cast<std::uint8_t>(header.value);
    sprite.attributes = static_cast<std::uint8_t>(header.value >> 8);
    sprite.index = static_cast<std::uint8_t>(header.value >> 16);
    sprite.valid = (header.value >> 24) != 0;
    if (sprite.attributes & 0x40) { low = reverse(low); high = reverse(high); }
    sprite.low = low; sprite.high = high;
    return VideoRenderResult::Ok;
}

/**
 * @brief Доводит отложенное состояние спрайтов до текущего растрового среза.
 * @details Раздельные плоскости, flip и накопленные сдвиги относятся к моменту захвата, а не к новой метаинформации.
 */
void VideoRenderer::materializeSprites() noexcept {
    if (!spriteRowValid_) return;
    for (auto& sprite : sprites_) {
        if (spriteClocks_ <= sprite.x) sprite.x -= static_cast<std::uint8_t>(spriteClocks_);
        else {
            const unsigned shifts = spriteClocks_ - sprite.x;
            sprite.x = 0;
            if (shifts >= 8) sprite.low = sprite.high = 0;
            else { sprite.low <<= shifts; sprite.high <<= shifts; }
        }
    }
    spriteClocks_ = 0;
    spriteRowValid_ = false;
}

/**
 * @brief Строит текущий кэш выбора спрайтов для видимой строки.
 * @details Порядок OAM и окклюзия ранними спрайтами сохраняются; отдельная маска Sprite0 не разрешает менять выигравший пиксель.
 */
void VideoRenderer::buildSpriteRow() noexcept {
    spriteRow_.fill(0);
    for (const auto& sprite : sprites_) {
        if (!sprite.valid) continue;
        const unsigned meta = ((sprite.attributes & 3) << 2) |
            ((sprite.attributes & 0x20) ? 0x10 : 0) | (sprite.index == 0 ? 0x20 : 0);
        for (unsigned bit = 0; bit < 8 && sprite.x + bit < 256; ++bit) {
            const unsigned x = sprite.x + bit;
            if (spriteRow_[x]) continue;
            const unsigned mask = 0x80U >> bit;
            const unsigned pixel = ((sprite.low & mask) ? 1 : 0) | ((sprite.high & mask) ? 2 : 0);
            if (pixel) spriteRow_[x] = static_cast<std::uint8_t>(meta | pixel);
        }
    }
    spriteClocks_ = 0;
    spriteRowValid_ = true;
}

/** @brief Импортирует literal post-flip единицы и ровно один раз применяет эффективный долг, не трогая producer. */
VideoRenderResult VideoRenderer::importSpriteRow(const VideoEvent& header,const VideoEvent* payload) noexcept {
    if(header.arg0()>=240U||header.arg1()!=8U||header.arg2()||header.value>65535U)return VideoRenderResult::InvalidPacket;
    const auto* bytes=reinterpret_cast<const unsigned char*>(payload);
    for(unsigned i=0;i<8;++i)if(bytes[i*6U+5U]>1U)return VideoRenderResult::InvalidPacket;
    for(unsigned i=0;i<8;++i){const auto*b=bytes+i*6U;auto&u=sprites_[i];
        u.low=b[0];u.high=b[1];u.x=b[2];u.attributes=b[3];u.index=b[4];u.valid=b[5]!=0;
    }
    spriteClocks_=static_cast<std::uint16_t>(header.value);spriteRowValid_=spriteClocks_!=0;
    if(spriteRowValid_)materializeSprites();
    buildSpriteRow();return VideoRenderResult::Ok;
}

/**
 * @brief Фиксирует метаданные emphasis соответствующей строки.
 * @details При mid-row изменении связанный RGB-выход исправляет уже выведенный префикс до наблюдаемой границы.
 * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
 * @param emphasis Три бита цветового усиления для соответствующей строки/участка.
 */
void VideoRenderer::setRowEmphasis(unsigned y, unsigned emphasis) noexcept {
    if (rowScratchMode_) {
        if (emphasis_[y] == emphasis) return;
        emphasis_[y] = static_cast<std::uint8_t>(emphasis);
        const auto& row = colours[colourRow(emphasis, pal_)];
        // Только уже записанный prefix текущей строки; старый tail никогда не читается.
        const unsigned prefix = rasterPosition_ / 256U == y ? rasterPosition_ % 256U : 0U;
        for (unsigned x = 0; x < prefix; ++x) rgb_[y*256U+x] = row[rowIndices_[x] & 63U];
        return;
    }
    if (emphasis_[y] == emphasis) return;
    emphasis_[y] = static_cast<std::uint8_t>(emphasis);
    // Публичная baseline назначает всей строке ПОСЛЕДНИЙ emphasis, включая
    // неперерисованные пиксели прошлого кадра. Palette-indices уже
    // разрешили исторические palette/grayscale и не должны меняться.
    if (rgb_) {
        const auto& row = colours[colourRow(emphasis, pal_)];
        for (unsigned x = 0; x < 256; ++x) rgb_[y * 256 + x] = row[indices_[y * 256 + x] & 63];
        rgbWritten_[y] = 256;
    }
}

/**
 * @brief Обновляет локальные значения палитры после её фактической записи.
 * @details Инвалидирует связанные цветовые таблицы в исходном порядке, не перенося новое значение назад по времени.
 * @param mask Снимок PPUMASK для этого растрового интервала.
 */
void VideoRenderer::cachePalette(unsigned mask) noexcept {
    const unsigned key=mask&0xE1U;
    if(!paletteDirty_&&paletteMask_==key)return;
    const auto* colour=colours[colourRow(mask>>5,pal_)].data();
    const unsigned gray=(mask&1)?0x30U:0x3FU;
    for(unsigned i=0;i<32;++i){indexPalette_[i]=palette_[i]&gray;colourPalette_[i]=colour[indexPalette_[i]];}
    for(unsigned pair=0;pair<256;++pair){
        const unsigned a=pair&15U,b=pair>>4;
        const unsigned first=(a&3U)?a:0U,second=(b&3U)?b:0U;
        backgroundPairs_[pair]={indexPalette_[first],indexPalette_[second]};
        colourPairs_[pair]={colourPalette_[first],colourPalette_[second]};
    }
    paletteMask_=static_cast<std::uint8_t>(key);paletteDirty_=false;
}

/**
 * @brief Сообщает привязанному RGB-выходу точный участок индексного вывода.
 * @details Контекст относится к тому же PPU и не удерживается через смену API/владельца.
 * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
 * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 */
void VideoRenderer::noteRgbSpan(unsigned y, unsigned x, unsigned count) noexcept {
    if (rowScratchMode_) rasterPosition_ += count; // beginRasterSpan уже проверил весь диапазон.
    if (rgb_ && x <= rgbWritten_[y])
        rgbWritten_[y] = static_cast<std::uint16_t>(std::max<unsigned>(rgbWritten_[y], x + count));
}

template<bool Rgb>
#if defined(__GNUC__) || defined(__clang__)
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
[[gnu::always_inline]]
#endif
inline void VideoRenderer::writePixels(std::uint32_t background,const std::uint8_t* sprites,
    unsigned count,unsigned x,unsigned mask,std::uint8_t* indexed,std::uint16_t* rgb) const noexcept {
    if(!(mask&8U))background=0;
    else if(x<8U&&!(mask&2U)){
        const unsigned hidden=std::min(8U,8U-x);
        background=hidden==8?0:background&(~0U<<(hidden*4));
    }
    const unsigned hiddenSprites=!(mask&16U)?8U:((x<8U&&!(mask&4U))?8U-x:0U);
    bool empty=hiddenSprites>=count;
    if(!empty&&count==8){
        std::uint32_t first=0,second=0;
        std::memcpy(&first,sprites,4);std::memcpy(&second,sprites+4,4);
        empty=((first|second)&0x03030303U)==0;
    }
    if(empty&&count==8){
        if(!(background&0x33333333U)){
            std::memset(indexed,indexPalette_[0],8);
            if constexpr(Rgb)std::fill_n(rgb,8,colourPalette_[0]);
        }else for(unsigned pair=0;pair<4;++pair,background>>=8){
            const auto key=background&255U;
            std::memcpy(indexed+pair*2,backgroundPairs_[key].data(),2);
            if constexpr(Rgb)std::memcpy(rgb+pair*2,colourPairs_[key].data(),4);
        }
        return;
    }
    for(unsigned i=0;i<count;++i,background>>=4){
        const unsigned sprite=i<hiddenSprites?0U:sprites[i];
        const unsigned index=selection[(sprite<<4)|(background&15U)];
        indexed[i]=indexPalette_[index];
        if constexpr(Rgb)rgb[i]=colourPalette_[index];
    }
}

/**
 * @brief Записывает предусмотренный текущим видеособытием участок вывода.
 * @details Сохраняет индексную плоскость, порядок перекрытия и ограничения видимых координат.
 * @param x Координата внутри текущего растрового участка; не геометрия физического дисплея.
 * @param y Координата внутри текущего растрового участка; не геометрия физического дисплея.
 * @param count Число обрабатываемых элементов/позиций текущего ограниченного участка.
 * @param mask Снимок PPUMASK для этого растрового интервала.
 * @param fine Точный fine-X от 0 до 7 для выбора байтов текущего/следующего тайла.
 */
template<bool Rgb>
void VideoRenderer::draw(unsigned x,unsigned y,unsigned count,unsigned mask,unsigned fine) noexcept {
    setRowEmphasis(y,mask>>5);if(!spriteRowValid_)buildSpriteRow();cachePalette(mask);
    auto planes=unsigned(low_)|(unsigned(high_)<<16),attrs=unsigned(attrLow_)|(unsigned(attrHigh_)<<16);
    unsigned cursor=spriteClocks_,done=0;
    auto* const indexed=rowScratchMode_?rowIndices_.data()+x:indices_.data()+y*256+x;
    auto* const rgb=Rgb?rgb_+y*256+x:nullptr;
    while(done<count){
        const unsigned n=std::min(8U,count-done);
        const auto* row=noSprites.data();std::array<std::uint8_t,8> tail{};
        if(cursor<256){
            if(cursor+n<=256)row=spriteRow_.data()+cursor;
            else{std::copy_n(spriteRow_.data()+cursor,256U-cursor,tail.data());row=tail.data();}
        }
        writePixels<Rgb>(pixels(planes,attrs,fine),row,n,x+done,mask,indexed+done,Rgb?rgb+done:nullptr);
        planes=shiftPair(planes,n);attrs=shiftPair(attrs,n);cursor=std::min(256U,cursor+n);done+=n;
    }
    low_=static_cast<std::uint16_t>(planes);high_=static_cast<std::uint16_t>(planes>>16);
    attrLow_=static_cast<std::uint16_t>(attrs);attrHigh_=static_cast<std::uint16_t>(attrs>>16);
    spriteClocks_=cursor;noteRgbSpan(y,x,count);
}

/**
 * @brief Исполняет одно хронологическое событие повторного PPU.
 * @details Для сохранённого чтения выполняет реальное чтение один раз и сравнивает ответ; ошибку нельзя исправлять повторной попыткой после побочных эффектов.
 * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
 * @param event Следующая хронологическая запись старого видеопакета; порядок нельзя менять.
 */
VideoRenderResult VideoRenderer::apply(const VideoEvent& event) noexcept {
    const unsigned a = event.arg0(), b = event.arg1(), c = event.arg2();
    const unsigned value = event.value;
    const auto invalid = VideoRenderResult::InvalidPacket;
    std::uint8_t byte = 0;
    switch (event.op()) {
    case VideoOp::NameWrite:
    case VideoOp::ChrWrite: {
        const unsigned offset = a | (b << 8);
        const bool name = event.op() == VideoOp::NameWrite;
        if (c || value > 255 || offset >= (name ? names_.size() : chr_.size())) return invalid;
        if (name) names_[offset] = static_cast<std::uint8_t>(value);
        else chr_[offset] = static_cast<std::uint8_t>(value);
        break;
    }
    case VideoOp::PaletteWrite:
        if (a >= 32 || b || c || value > 63) return invalid;
        if(palette_[a]!=value)paletteDirty_=true;
        palette_[a] = static_cast<std::uint8_t>(value); break;
    case VideoOp::BgAttribute:
        if ((a != 0 && a != 2 && a != 4 && a != 6) || b || c) return invalid;
        if (!resolve(value, byte)) return VideoRenderResult::InvalidReference;
        nextAttr_ = static_cast<std::uint8_t>((byte >> a) & 3); break;
    case VideoOp::BgLow:
    case VideoOp::BgHigh:
        if (a || b || c) return invalid;
        if (!resolve(value, byte)) return VideoRenderResult::InvalidReference;
        if (event.op() == VideoOp::BgLow) nextLow_ = byte; else nextHigh_ = byte;
        break;
    case VideoOp::BgLoad:
        if (a || b || c || value) return invalid;
        low_ = static_cast<std::uint16_t>((low_ & 0xFF00) | nextLow_);
        high_ = static_cast<std::uint16_t>((high_ & 0xFF00) | nextHigh_);
        attrLow_ = static_cast<std::uint16_t>((attrLow_ & 0xFF00) | ((nextAttr_ & 1) ? 255 : 0));
        attrHigh_ = static_cast<std::uint16_t>((attrHigh_ & 0xFF00) | ((nextAttr_ & 2) ? 255 : 0));
        break;
    case VideoOp::BgShift:
        if (!a || b || c || value) return invalid;
        if (a >= 16) low_ = high_ = attrLow_ = attrHigh_ = 0;
        else { low_ <<= a; high_ <<= a; attrLow_ <<= a; attrHigh_ <<= a; }
        break;
    case VideoOp::PreloadTile: {
        if (a || b || c) return invalid;
        std::uint8_t low = 0, high = 0;
        if (!resolveTile(value, low, high)) return VideoRenderResult::InvalidReference;
        // Старый/current тайл загружается и сдвигается ДО перехода новых fetch-
        // байтов в next. Не сдвигать спрайты и не рисовать здесь.
        low_ = static_cast<std::uint16_t>(nextLow_ << 8);
        high_ = static_cast<std::uint16_t>(nextHigh_ << 8);
        attrLow_ = (nextAttr_ & 1) ? 0xFF00 : 0;
        attrHigh_ = (nextAttr_ & 2) ? 0xFF00 : 0;
        nextLow_ = low; nextHigh_ = high; nextAttr_ = static_cast<std::uint8_t>(value >> 30);
        break;
    }
    case VideoOp::SpriteMeta:
        if (a >= 8 || b || c || value >> 25) return invalid;
        materializeSprites();
        sprites_[a].x = static_cast<std::uint8_t>(value);
        sprites_[a].attributes = static_cast<std::uint8_t>(value >> 8);
        sprites_[a].index = static_cast<std::uint8_t>(value >> 16);
        sprites_[a].valid = (value >> 24) != 0;
        break;
    case VideoOp::SpriteLow:
    case VideoOp::SpriteHigh:
        if (a >= 8 || b || c) return invalid;
        if (!resolve(value, byte)) return VideoRenderResult::InvalidReference;
        materializeSprites();
        if (sprites_[a].attributes & 0x40) byte = reverse(byte);
        if (event.op() == VideoOp::SpriteLow) sprites_[a].low = byte;
        else sprites_[a].high = byte;
        break;
    case VideoOp::VisibleLine:
        if (a >= 240 || b || c || value) return invalid;
        materializeSprites(); buildSpriteRow(); break;
    case VideoOp::Draw: {
        const unsigned count = a ? a : 256;
        const unsigned x = value & 255, y = (value >> 8) & 255;
        const unsigned mask = (value >> 16) & 255, fineX = value >> 24;
        if (b || c || y >= 240 || x + count > 256 || fineX > 7) return invalid;
        if (!beginRasterSpan(y,x,count)) return invalid;
        if(rgb_)draw<true>(x,y,count,mask,fineX);else draw<false>(x,y,count,mask,fineX);
        break;
    }
    case VideoOp::Fill: {
        const unsigned count = a ? a : 256;
        const unsigned x = value & 255, y = (value >> 8) & 255;
        const unsigned mask = (value >> 16) & 255, index = value >> 24;
        if (b || c || y >= 240 || x + count > 256 || index >= 32) return invalid;
        if (!beginRasterSpan(y,x,count)) return invalid;
        setRowEmphasis(y, mask >> 5);
        const auto colour = static_cast<std::uint8_t>(palette_[index] & ((mask & 1) ? 0x30 : 0x3F));
        std::memset(rowScratchMode_?rowIndices_.data()+x:indices_.data()+y*256+x,colour,count);
        if(rgb_)std::fill_n(rgb_+y*256+x,count,colours[colourRow(mask>>5,pal_)][colour]);
        noteRgbSpan(y, x, count);
        break;
    }
    case VideoOp::FrameEnd:
        if (a || b || c || value) return invalid;
        if (rowScratchMode_) {
            if (!rgb_ || rasterPosition_ != 256U*240U) return invalid;
            ++frame_; rasterPosition_ = 0; rasterComplete_ = true;
            break;
        }
        ++frame_;
        // Вызывающий может привязать свежий RGB-буфер после canonical-only префикса.
        // Обычно каждая строка уже выдана напрямую; исправлять лишь редкий пробел.
        if (rgb_ && std::find_if(rgbWritten_.begin(), rgbWritten_.end(),
            [](std::uint16_t n) { return n != 256; }) != rgbWritten_.end()) refreshRgb565();
        break;
    default: return invalid;
    }
    return VideoRenderResult::Ok;
}

/**
 * @brief Обслуживает последовательность полных фоновых тайлов в текущей строке.
 * @details Временной PPU может удержать только живой хвост при доказанном отсутствии нужного Sprite0-наблюдения; полный вывод сохраняет фактический выбор пикселей.
 * @param payload Неизменяемые raw-слоты после заголовка видеособытия; интерпретация и число задаются его форматом.
 * @return Ok после корректного исполнения операции либо конкретная ошибка формата/границ/источника; частичный кадр не публикуется.
 * @param header Заголовок текущей операции старого видеопротокола с форматом и ограниченными координатами.
 */
VideoRenderResult VideoRenderer::backgroundRun(const VideoEvent& header, const VideoEvent* payload) noexcept {
    const unsigned count = header.arg0(), x = header.value & 255, y = (header.value >> 8) & 255;
    const unsigned fine = header.value >> 24;
    if (!count || count > 32 || header.arg1() || header.arg2() || y >= 240 || fine > 7 || x + count * 8 > 256 ||
        ((count & 1) && payload[count / 2].value)) return VideoRenderResult::InvalidPacket;
    return rgb_?backgroundRunPixels<true>(header,payload):backgroundRunPixels<false>(header,payload);
}

/**
 * @brief Рисует фон из опубликованных событий и сохранённого отображения страниц.
 * @details Проверяет физические ссылки и форму данных; потребитель не выполняет CPU-программу повторно.
 * @param payload Неизменяемые raw-слоты после заголовка видеособытия; интерпретация и число задаются его форматом.
 * @return Ok после полной проверки и установки нового физического view либо ошибка без частичной замены прежнего view.
 * @param header Заголовок текущей операции старого видеопротокола с форматом и ограниченными координатами.
 */
VideoRenderResult VideoRenderer::backgroundView(const VideoEvent& header,const VideoEvent* payload) noexcept {
    if(header.tag!=static_cast<std::uint32_t>(VideoOp::BgView)||header.value)return VideoRenderResult::InvalidPacket;
    std::array<const std::uint8_t*,4> names{};
    std::array<const std::uint8_t*,8> chr{};
    for(unsigned i=0;i<4;++i){
        const auto word=i<2?payload[0].tag:payload[0].value;
        const auto offset=(word>>((i&1U)*16))&65535U;
        if(offset>3072U||(offset&1023U))return VideoRenderResult::InvalidReference;
        names[i]=names_.data()+offset;
    }
    for(unsigned i=0;i<8;++i){
        const auto ref=(i&1U)?payload[1+i/2].value:payload[1+i/2].tag;
        const auto kind=ref&VideoByteRef::kindMask,offset=ref&VideoByteRef::offsetMask;
        const bool ram=kind==VideoByteRef::chrRam;
        const auto size=ram?static_cast<std::uint32_t>(chr_.size()):romBytes_;
        const auto* source=ram?chr_.data():rom_;
        if((kind!=VideoByteRef::rom&&!ram)||!source||(offset&1023U)||size<1024U||offset>size-1024U)
            return VideoRenderResult::InvalidReference;
        chr[i]=source+offset;
    }
    // Неверные заменяющие view не должны частично менять текущий view.
    bgNames_=names;bgChr_=chr;bgViewValid_=true;
    return VideoRenderResult::Ok;
}

/**
 * @brief Рисует фон из опубликованных событий и сохранённого отображения страниц.
 * @details Проверяет физические ссылки и форму данных; потребитель не выполняет CPU-программу повторно.
 * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
 * @param header Заголовок текущей операции старого видеопротокола с форматом и ограниченными координатами.
 * @param body Неизменяемый второй raw-слот фонового span, проверяемый вместе с заголовком.
 */
VideoRenderResult VideoRenderer::backgroundSpan(const VideoEvent& header,const VideoEvent& body) noexcept {
    const unsigned count=header.arg0(),x=header.value&255U,y=(header.value>>8)&255U,fine=header.value>>24;
    if(!bgViewValid_||!count||count>32||header.arg1()||header.arg2()||(x&7U)||
       y>=240||fine>7||x+count*8>256||body.value||(body.tag&0xFF008000U))
        return VideoRenderResult::InvalidPacket;
    const auto v=body.tag&0x7FFFU,control=(body.tag>>16)&255U;
    return rgb_?backgroundRunPixels<true>(header,nullptr,v,control):
                backgroundRunPixels<false>(header,nullptr,v,control);
}

template<bool Rgb>
#if 1 && 1 && defined(__GNUC__) && !defined(__clang__)
[[gnu::noinline,gnu::noclone]]
#elif 1 && 1 && defined(__clang__)
[[gnu::noinline]]
#endif
/**
 * @brief Рисует фон из опубликованных событий и сохранённого отображения страниц.
 * @details Проверяет физические ссылки и форму данных; потребитель не выполняет CPU-программу повторно.
 * @param payload Неизменяемые raw-слоты после заголовка видеособытия; интерпретация и число задаются его форматом.
 * @return Ok после корректного рисования либо ошибка формата/границ/ссылки без права публикации частичного кадра.
 * @param header Заголовок текущей операции старого видеопротокола с форматом и ограниченными координатами.
 */
VideoRenderResult VideoRenderer::backgroundRunPixels(const VideoEvent& header,const VideoEvent* payload
    ,unsigned spanV,unsigned spanControl
    ) noexcept {
    const unsigned count=header.arg0(),x=header.value&255U,y=(header.value>>8)&255U;
    const unsigned mask=(header.value>>16)&255U,fine=header.value>>24;
    if (!beginRasterSpan(y,x,count*8U)) return VideoRenderResult::InvalidPacket;
    auto planes=unsigned(low_)|(unsigned(high_)<<16),attrs=unsigned(attrLow_)|(unsigned(attrHigh_)<<16);
    unsigned nextLow=nextLow_,nextHigh=nextHigh_,nextAttr=nextAttr_,cursor=spriteClocks_,done=0;
    /**
     * @brief Фиксирует локальные planes/attrs/next/spriteClocks и уже нарисованный RGB-диапазон до возврата из фонового consumer-пути.
     * @note Не сохраняется после возврата внешнего метода; захваченные ссылки/указатели остаются у текущего владельца.
     */
    auto publish=[&]() noexcept {
        low_=static_cast<std::uint16_t>(planes);high_=static_cast<std::uint16_t>(planes>>16);
        attrLow_=static_cast<std::uint16_t>(attrs);attrHigh_=static_cast<std::uint16_t>(attrs>>16);
        nextLow_=static_cast<std::uint8_t>(nextLow);nextHigh_=static_cast<std::uint8_t>(nextHigh);nextAttr_=static_cast<std::uint8_t>(nextAttr);
        spriteClocks_=cursor;if(done)noteRgbSpan(y,x,done*8);
    };
    auto* const indexed=rowScratchMode_?rowIndices_.data()+x:indices_.data()+y*256+x;
    auto* const rgb=Rgb?rgb_+y*256+x:nullptr;
    for (unsigned tile = 0; tile < count; ++tile) {
        unsigned low,high,attribute;
        if(!payload){
            const auto* name=bgNames_[(spanV>>10)&3U];
            const unsigned tileId=name[spanV&1023U];
            const unsigned raw=name[0x3C0U|((spanV>>4)&0x38U)|((spanV>>2)&7U)];
            attribute=(raw>>(((spanV>>4)&4U)|(spanV&2U)))&3U;
            const unsigned pattern=((spanControl&0x10U)?0x1000U:0U)+tileId*16U+((spanV>>12)&7U);
            const auto* source=bgChr_[pattern>>10];
            low=source[pattern&1023U];high=source[(pattern&1023U)+8U];
            if((spanV&31U)==31U)spanV=(spanV&~31U)^0x400U;else ++spanV;
        }else
        {
        const auto token = (tile & 1) ? payload[tile / 2].value : payload[tile / 2].tag;
        const unsigned offset = token & 0x1FFFFFFFU;
        const bool ram=(token&0x20000000U)!=0;
        const unsigned size=ram?static_cast<unsigned>(chr_.size()):romBytes_;
        const auto* source=ram?chr_.data():rom_;
        if(!source||size<9||offset>size-9){publish();return VideoRenderResult::InvalidReference;}
        low=source[offset];high=source[offset+8];attribute=token>>30;
        }
        if(!tile){setRowEmphasis(y,mask>>5);if(!spriteRowValid_)buildSpriteRow();cachePalette(mask);cursor=spriteClocks_;}
        // Загрузить СТАРЫЕ next-значения, нарисовать тайл, затем сохранить новую выборку.
        planes=(planes&0xFF00FF00U)|nextLow|(nextHigh<<16);
        attrs=(attrs&0xFF00FF00U)|((nextAttr&1)?255U:0U)|((nextAttr&2)?0x00FF0000U:0U);
        const auto* row=noSprites.data();std::array<std::uint8_t,8> tail{};
        if(cursor<256){if(cursor<=248)row=spriteRow_.data()+cursor;else{std::copy_n(spriteRow_.data()+cursor,256U-cursor,tail.data());row=tail.data();}}
        writePixels<Rgb>(pixels(planes,attrs,fine),row,8,x+tile*8,mask,indexed+tile*8,Rgb?rgb+tile*8:nullptr);
        planes=(planes<<8)&0xFF00FF00U;attrs=(attrs<<8)&0xFF00FF00U;
        cursor=std::min(256U,cursor+8);nextLow=low;nextHigh=high;nextAttr=attribute;++done;
    }
    publish();
    return VideoRenderResult::Ok;
}

/**
 * @brief Проверяет и исполняет опубликованные записи текущей аренды.
 * @details Порядок времени, последовательность, реальные ответы чтений и конечный срез проверяются до готовности результата.
 * @param sealedOnly При true разрешает только уже запечатанный пакет; иначе действует предусмотренная проверка текущего состояния.
 * @return Вычисленное значение или состояние типа VideoRenderResult согласно описанному контракту.
 * @param packet Отдельный цветовой/видеослот в требуемом состоянии; чтение и release следуют единственному владельцу.
 */
#include "Mmc5SpanConsumer.inl"

/**
 * @brief Проверяет lease/время и исполняет только ещё не потреблённую часть packet.
 * @details sealedOnly требует Sealed; observer-путь допускает опубликованный immutable Recording-prefix.
 * Callback работает внутри заимствования и не сохраняется. Его отказ оставляет
 * ошибку и неполный результат; вызывающий сам завершает владельцев до teardown.
 */
VideoRenderResult VideoRenderer::consume(const VideoPacket& packet, bool sealedOnly,ProgressCallback progress,void*context) noexcept {
    // Только producer может опубликовать Recording scratch до передачи const reader.
    if(packet.literalPrefixPending())return VideoRenderResult::InvalidPacket;
    const auto& h = packet.header();
    if (rowScratchMode_) {
        if (fault_ != VideoRenderResult::Ok) return fault_;
        if (!sealedOnly || !rgb_ || h.sequence == UINT64_MAX || h.ppuFrame != h.sequence+1U)
            return VideoRenderResult::InvalidSequence;
    }
    if (packet.state() == VideoPacket::State::Free || (sealedOnly && packet.state() != VideoPacket::State::Sealed) ||
        h.fault != VideoFault::None || h.protocol>VideoPacket::literalProtocol || !h.generation || h.eventCount > VideoPacket::eventCapacity ||
        h.endCycle < h.startCycle || (h.chrRomBytes && !h.chrRom)) return VideoRenderResult::InvalidPacket;
    const bool newGeneration = !initialized_ || h.generation > generation_;
    if (newGeneration) {
        if (h.sequence != 0) return VideoRenderResult::InvalidSequence;
        if (!h.seedPresent) return VideoRenderResult::MissingSeed;
        reset();
        std::copy_n(packet.chrRamSeed(), chr_.size(), chr_.begin());
        generation_ = h.generation; sequence_ = 0; startCycle_ = h.startCycle;
        cycle_ = h.startCycle; rom_ = h.chrRom; romBytes_ = h.chrRomBytes; pal_ = h.pal;
        packet_ = &packet; initialized_ = packetOpen_ = true;
    } else {
        if (h.generation != generation_ || h.chrRom != rom_ || h.chrRomBytes != romBytes_ || h.pal != pal_)
            return VideoRenderResult::InvalidSequence;
        if (fault_ != VideoRenderResult::Ok) return fault_;
        if (h.sequence == sequence_) {
            if (packet_ != &packet || h.startCycle != startCycle_ || h.eventCount < consumed_ || h.endCycle < cycle_ ||
                (sealedOnly && !packetOpen_) || (!packetOpen_ && (h.eventCount != consumed_ ||
                h.endCycle != cycle_ || packet.state() != VideoPacket::State::Sealed)))
                return VideoRenderResult::InvalidSequence;
        } else {
            if (packetOpen_ || sequence_ == UINT64_MAX || h.sequence != sequence_ + 1 || h.startCycle != cycle_ || h.seedPresent)
                return VideoRenderResult::InvalidSequence;
            sequence_ = h.sequence; startCycle_ = h.startCycle; consumed_ = 0;mmc5Seeded_=false;
            packet_ = &packet; packetOpen_ = true;
        }
    }
    // Только завершённые IR-атомы; ни clock, ни callback внутри pixel/span-цикла.
    unsigned operations=0,lastPumpRow=0;
    const auto progressPoint=[&]()noexcept{
        if(!progress)return true;
        const auto row=rasterPosition_/256U;
        if(++operations<64U&&row<lastPumpRow+8U)return true;
        operations=0;lastPumpRow=row;return progress(context);
    };
    while (consumed_ < h.eventCount) {
        const auto& event = packet.events()[consumed_];
        // Четыре уже опубликованных scalar op образуют один exact next-tile update.
        // Никакая запись/другая op между ними не переставляется и не пропускается.
        if (event.tag == static_cast<std::uint32_t>(VideoOp::BgLoad) && !event.value &&
            h.eventCount - consumed_ >= 4U) {
            const auto* const group = packet.events() + consumed_;
            const auto& attribute = group[1];
            const auto& low = group[2];
            const auto& high = group[3];
            // Только arg0={0,2,4,6}; все остальные tag биты проверены сразу.
            const bool attributeShape = (attribute.tag & ~0x600U) ==
                static_cast<std::uint32_t>(VideoOp::BgAttribute);
            const bool byteShapes = low.tag == static_cast<std::uint32_t>(VideoOp::BgLow) &&
                high.tag == static_cast<std::uint32_t>(VideoOp::BgHigh);
            const bool literalBytes = (attribute.value & ~0xFFU) == VideoByteRef::literal &&
                (low.value & ~0xFFU) == VideoByteRef::literal &&
                (high.value & ~0xFFU) == VideoByteRef::literal;
            if (attributeShape && byteShapes && literalBytes) {
                // BgLoad читает ПРЕЖНИЕ next до присваивания новых literal fetch.
                applyLiteralBackground(attribute.arg0(), attribute.value, low.value, high.value);
                consumed_ += 4U; // Все четыре old apply гарантированно вернули бы Ok.
                if(!progressPoint()){fault_=VideoRenderResult::InvalidSequence;rasterComplete_=false;return fault_;}
                continue;
            }
        }

        unsigned slots = 1;
        VideoRenderResult result;
        if(event.op()==VideoOp::PackedLiteralBg){
            // Полная проверка до мутации. BgLoad обязан увидеть ПРЕЖНИЕ next.
            if(h.protocol!=VideoPacket::literalProtocol||event.arg0()>6U||(event.arg0()&1U)||event.arg1()||event.arg2()||event.value>0xFFFFFFU)
                result=VideoRenderResult::InvalidPacket;
            else{
                applyLiteralBackground(event.arg0(), event.value & 255U,
                    (event.value >> 8) & 255U, event.value >> 16);
                result=VideoRenderResult::Ok;
            }
        }else if(event.op()==VideoOp::Mmc5Seed||event.op()==VideoOp::Mmc5PhysicalWrite||event.op()==VideoOp::Mmc5Span){
            result=h.protocol?VideoRenderResult::InvalidPacket:mmc5SpanEvent(event,packet.events()+consumed_+1U,h.eventCount-consumed_,slots);
        }else if (event.op() == VideoOp::BgRun) {
            slots += (event.arg0() + 1) / 2;
            if (slots > h.eventCount - consumed_) result = VideoRenderResult::InvalidPacket;
            else result = backgroundRun(event, packet.events() + consumed_ + 1);
        } else if(event.op()==VideoOp::BgView){
            slots=6;
            if(slots>h.eventCount-consumed_)result=VideoRenderResult::InvalidPacket;
            else result=backgroundView(event,packet.events()+consumed_+1);
        } else if(event.op()==VideoOp::BgSpan){
            slots=2;
            if(slots>h.eventCount-consumed_)result=VideoRenderResult::InvalidPacket;
            else result=backgroundSpan(event,packet.events()[consumed_+1]);
        } else if(event.op()==VideoOp::SpriteRow){
            slots=7;
            if(slots>h.eventCount-consumed_)result=VideoRenderResult::InvalidPacket;
            else result=importSpriteRow(event,packet.events()+consumed_+1U);
        } else if (event.op() == VideoOp::SpriteTile) {
            slots = 2;
            if (slots > h.eventCount - consumed_) result = VideoRenderResult::InvalidPacket;
            else result = spriteTile(event, packet.events()[consumed_ + 1]);
        } else result = apply(event);
        if (result != VideoRenderResult::Ok) { fault_ = result; return result; }
        consumed_ += slots;
        if(!progressPoint()){fault_=VideoRenderResult::InvalidSequence;rasterComplete_=false;return fault_;}
    }
    if (h.ppuFrame != frame_) { fault_ = VideoRenderResult::InvalidSequence; return fault_; }
    cycle_ = h.endCycle;
    if (packet.state() == VideoPacket::State::Sealed) {
        packetOpen_ = false;
        if (rowScratchMode_) {
            if (!rasterComplete_ || rasterPosition_ != 0) return VideoRenderResult::InvalidSequence;
        } else
        // Canonical-only наблюдатель может потребить FrameEnd ДО привязки
        // владельцем нового защищённого fence RGB-target. Sealing может вообще
        // не добавить событий; завершить свежий target из постоянной тени здесь.
        if (rgb_ && std::find_if(rgbWritten_.begin(), rgbWritten_.end(),
            [](std::uint16_t n) { return n != 256; }) != rgbWritten_.end()) refreshRgb565();
    }
    return VideoRenderResult::Ok;
}

/**
 * @brief Потребляет полный Sealed видеопакет до проверенного конечного среза.
 * @param sealed Неизменяемый пакет; packet/ROM/RGB backing живут до возврата вызова.
 * @param progress Необязательный синхронный Main callback между целыми IR-группами; не сохраняется.
 * @param context Заимствованный аргумент callback на время вызова, без передачи владения.
 * @details Callback не меняет renderer, packet или их leases. false прекращает
 * потребление с ошибкой; частичная картинка не становится успешным результатом.
 * @return Ok подтверждает программный срез, но не PPA/LCD completion.
 */
VideoRenderResult VideoRenderer::render(const VideoPacket& sealed,ProgressCallback progress,void*context) noexcept {
    if (rowScratchMode_) {
        const auto result=consume(sealed,true,progress,context);
        if (result != VideoRenderResult::Ok) { fault_=result; rasterComplete_=false; }
        return result;
    }
    return consume(sealed, true,progress,context); }
/**
 * @brief Доводит активный видеопакет и передаёт его наблюдателю.
 * @details При отказе наблюдателя защёлкивает ошибку; вызывающий не получает частичную картинку как успешный результат.
 * @return Результат описанной проверки либо успешность перехода состояния; false обрабатывается вызывающим путём.
 * @param prefix Неизменяемый на время вызова завершённый префикс старого видеопротокола; Machine обратно не вызывается.
 */
bool VideoRenderer::observeVideo(const VideoPacket& prefix) noexcept {
    if (rowScratchMode_) return false; // Неподдержанный prefix не потребляется и не даёт canonical результата.
    return consume(prefix, false) == VideoRenderResult::Ok; }
} // пространство имён nes::core
