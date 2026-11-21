/**
 * @file ZipArchive.cpp
 * @brief Собственный безопасный reader ZIP/DEFLATE для загрузки ROM в память вызывающего.
 */
#include "ZipArchive.h"
#include "../compression/DeflateTables.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace nes::storage {
namespace {
// Реализация по RFC1951 и PKWARE APPNOTE, без стороннего
// inflater: история LZ77 хранится только в памяти вызывающего, файлы не извлекаются.
/**
 * @brief Читает little-endian16 из уже проверенного диапазона без невыровненного целочисленного доступа.
 * @param p Указатель на уже проверенный входной little-endian диапазон.
 */
std::uint16_t u16(const std::uint8_t* p) noexcept {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
/**
 * @brief Читает little-endian32 из уже проверенного диапазона без зависимости от порядка байтов CPU.
 * @param p Указатель на уже проверенный входной little-endian диапазон.
 */
std::uint32_t u32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

/** @brief RAII-владелец FILE архива и проверенного размера; любые range-чтения остаются в пределах файла. */
struct File {
    std::FILE* stream = nullptr;
    std::uint64_t size = 0;
    /**
     * @brief Закрывает принадлежащий обёртке FILE при любом выходе из разбора ZIP.
     */
    ~File() noexcept { if (stream) std::fclose(stream); }
    /**
     * @brief Позиционирует открытый архив по64-битному смещению, отказывая при непредставимом off_t.
     * @param offset Абсолютное байтовое смещение от начала открытого ZIP.
     */
    bool seek(std::uint64_t offset) noexcept {
#if defined(_WIN32)
        return _fseeki64(stream, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
        const auto value = static_cast<off_t>(offset);
        return value >= 0 && static_cast<std::uint64_t>(value) == offset &&
               fseeko(stream, value, SEEK_SET) == 0;
#endif
    }
    /**
     * @brief Открывает ZIP для чтения и устанавливает проверенный размер; неподдерживаемый большой архив возвращает TooLarge.
     * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
     */
    ZipError open(const char* path) noexcept {
        if (!path || !*path) return ZipError::Io;
#if defined(_WIN32)
        wchar_t wide[1024];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024))
            return ZipError::Io;
        stream = _wfopen(wide, L"rb");
        if (!stream || _fseeki64(stream, 0, SEEK_END) != 0) return ZipError::Io;
        const auto end = _ftelli64(stream);
#else
        stream = std::fopen(path, "rb");
        if (!stream || fseeko(stream, 0, SEEK_END) != 0) return ZipError::Io;
        const auto end = ftello(stream);
#endif
        if (end < 0) return ZipError::Io;
        size = static_cast<std::uint64_t>(end);
        return size > 0xFFFFFFFFULL ? ZipError::TooLarge : ZipError::Ok;
    }
    /**
     * @brief Читает точно заданный диапазон ZIP после проверки переполнений и конца файла; различает I/O и усечение.
     * @param offset Абсолютное байтовое смещение начала диапазона от начала ZIP.
     * @param destination Буфер вызывающего для результата; функция не сохраняет владение после завершения.
     * @param length Длина читаемого диапазона в байтах.
     */
    ZipError readAt(std::uint64_t offset, void* destination, std::size_t length) noexcept {
        if (offset > size || length > size - offset) return ZipError::Truncated;
        if (!seek(offset)) return ZipError::Io;
        if (length && std::fread(destination, 1, length, stream) != length)
            return std::ferror(stream) ? ZipError::Io : ZipError::Truncated;
        return ZipError::Ok;
    }
};

/** @brief Ограниченные данные открытого каталога/центрального каталога ZIP; OS-handle закрывает соответствующая RAII-цепочка. */
struct Directory {
    std::uint32_t start = 0, size = 0;
    std::uint16_t entries = 0;
    /**
     * @brief Находит и проверяет EOCD в разрешённом хвосте архива, не доверяя смещениям до проверки границ.
     */
    std::uint64_t end() const noexcept { return std::uint64_t(start) + size; }
};

/**
 * @brief Проверяет расположение/число записей центрального каталога и отказывает неподдерживаемым вариантам ZIP.
 * @param file RAII-обёртка открытого ZIP; дескриптор не передаётся во владение помощника.
 * @param result Выход разбора/декодирования; результат считается действительным только при успешном статусе.
 */
ZipError directory(File& file, Directory& result) noexcept {
    if (file.size < 22) return ZipError::InvalidArchive;
    const auto first = file.size > 65557 ? file.size - 65557 : 0;
    std::uint64_t end = file.size;
    std::uint8_t bytes[512];
    // Перекрытие21байт сохраняет разорванный фиксированный EOCD-header. Длина комментария
    // должна вести точно к EOF, включая ZIP-сигнатуры внутри комментариев.
    while (end > first) {
        const auto begin = end - first > sizeof(bytes) ? end - sizeof(bytes) : first;
        const auto length = static_cast<std::size_t>(end - begin);
        auto error = file.readAt(begin, bytes, length);
        if (error != ZipError::Ok) return error;
        if (length >= 22) {
            for (std::size_t pos = length - 22 + 1; pos-- > 0;) {
                const auto* p = bytes + pos;
                if (u32(p) != 0x06054B50 || begin + pos + 22 + u16(p + 20) != file.size)
                    continue;
                if (u16(p + 4) || u16(p + 6) || u16(p + 8) != u16(p + 10))
                    return ZipError::Unsupported;
                if (u16(p + 10) == 0xFFFF || u32(p + 12) == 0xFFFFFFFF || u32(p + 16) == 0xFFFFFFFF)
                    return ZipError::Unsupported;
                result = {u32(p + 16), u32(p + 12), u16(p + 10)};
                if (result.end() > begin + pos || (result.entries == 0 && result.size != 0))
                    return ZipError::InvalidArchive;
                return ZipError::Ok;
            }
        }
        if (begin == first) break;
        end = begin + 21;
    }
    return ZipError::InvalidArchive;
}

/**
 * @brief Разбирает ограниченные дополнительные поля записи, не пропуская усечение или неподдерживаемый ZIP64.
 * @param length Длина читаемого диапазона в байтах.
 * @param file RAII-обёртка открытого ZIP; дескриптор не передаётся во владение помощника.
 * @param start Проверенное начальное байтовое смещение диапазона в архиве.
 */
ZipError extraFields(File& file, std::uint64_t start, std::size_t length) noexcept {
    const auto end = start + length;
    while (start < end) {
        if (end - start < 4) return ZipError::InvalidEntry;
        std::uint8_t header[4];
        auto error = file.readAt(start, header, sizeof(header));
        if (error != ZipError::Ok) return error;
        if (u16(header) == 1) return ZipError::Unsupported; // Поле ZIP64.
        const auto step = std::size_t(4) + u16(header + 2);
        if (step > end - start) return ZipError::InvalidEntry;
        start += step;
    }
    return ZipError::Ok;
}

/** @brief Проверенная запись центрального ZIP-каталога; её смещения не используются до валидации local-header. */
struct Central {
    ZipEntry entry;
    std::uint16_t nameLength = 0, extraLength = 0, commentLength = 0;
    std::uint32_t attributes = 0;
    std::uint64_t next = 0;
};

/**
 * @brief Разбирает одну запись центрального каталога и проверяет имя, метод, размеры и расположение данных.
 * @param offset Байтовое смещение заголовка относительно начала ZIP, внутри проверенного central directory.
 * @param file RAII-обёртка открытого ZIP; дескриптор не передаётся во владение помощника.
 * @param dir Проверенные границы центрального каталога ZIP.
 * @param c Выход метаданных одной записи; использовать только при ZipError::Ok.
 */
ZipError central(File& file, const Directory& dir, std::uint64_t offset, Central& c) noexcept {
    if (offset < dir.start || offset > dir.end() || dir.end() - offset < 46)
        return ZipError::InvalidEntry;
    std::uint8_t header[46];
    auto error = file.readAt(offset, header, sizeof(header));
    if (error != ZipError::Ok) return error;
    if (u32(header) != 0x02014B50) return ZipError::InvalidEntry;
    if (u16(header + 34)) return ZipError::Unsupported;
    c.entry = {u32(header + 42), u32(header + 20), u32(header + 24), u32(header + 16),
               static_cast<std::uint32_t>(offset), u16(header + 10), u16(header + 8)};
    if (c.entry.compressedSize == 0xFFFFFFFF || c.entry.uncompressedSize == 0xFFFFFFFF ||
        c.entry.localHeaderOffset == 0xFFFFFFFF) return ZipError::Unsupported;
    c.nameLength = u16(header + 28);
    c.extraLength = u16(header + 30);
    c.commentLength = u16(header + 32);
    c.attributes = u32(header + 38);
    c.next = offset + 46 + c.nameLength + c.extraLength + c.commentLength;
    if (c.next > dir.end() || !c.nameLength) return ZipError::InvalidEntry;
    if (c.entry.localHeaderOffset >= dir.start) return ZipError::InvalidEntry;
    return extraFields(file, offset + 46 + c.nameLength, c.extraLength);
}

/**
 * @brief Сравнивает идентичность выбранной ZIP-записи с повторно прочитанными метаданными.
 * @param a Первое уже проверенное значение/описание для точного сравнения.
 * @param b Второе уже проверенное значение/описание для точного сравнения.
 */
bool equal(const ZipEntry& a, const ZipEntry& b) noexcept {
    return a.localHeaderOffset == b.localHeaderOffset && a.compressedSize == b.compressedSize &&
        a.uncompressedSize == b.uncompressedSize && a.crc32 == b.crc32 &&
        a.centralHeaderOffset == b.centralHeaderOffset && a.method == b.method && a.flags == b.flags;
}

/**
 * @brief Сверяет local-header и central-directory выбранного участника перед распаковкой.
 * @param file RAII-обёртка открытого ZIP; дескриптор не передаётся во владение помощника.
 * @param dir Проверенные границы центрального каталога ZIP.
 * @param entry Повторно проверяемая выбранная ZIP-запись; исходный архив не должен незаметно измениться.
 * @param dataOffset Выходное абсолютное положение сжатых данных внутри проверенного архива.
 */
ZipError validateEntry(File& file, const Directory& dir, const ZipEntry& entry,
                       std::uint64_t& dataOffset) noexcept {
    Central c;
    auto error = central(file, dir, entry.centralHeaderOffset, c);
    if (error != ZipError::Ok) return error;
    if (!equal(entry, c.entry)) return ZipError::InvalidEntry;
    if (entry.flags & (1u | 64u)) return ZipError::Encrypted;
    if (entry.flags & ~std::uint16_t(2u | 4u | 8u | 2048u)) return ZipError::Unsupported;
    if (entry.method != 0 && entry.method != 8) return ZipError::Unsupported;
    if (entry.method == 0 && entry.compressedSize != entry.uncompressedSize) return ZipError::InvalidEntry;
    if (std::uint64_t(entry.localHeaderOffset) + 30 > dir.start) return ZipError::InvalidEntry;
    std::uint8_t header[30];
    error = file.readAt(entry.localHeaderOffset, header, sizeof(header));
    if (error != ZipError::Ok) return error;
    if (u32(header) != 0x04034B50 || u16(header + 6) != entry.flags || u16(header + 8) != entry.method)
        return ZipError::InvalidEntry;
    if (u32(header + 18) == 0xFFFFFFFF || u32(header + 22) == 0xFFFFFFFF)
        return ZipError::Unsupported;
    const bool descriptor = (entry.flags & 8) != 0;
    if ((!descriptor && (u32(header + 14) != entry.crc32 || u32(header + 18) != entry.compressedSize ||
                         u32(header + 22) != entry.uncompressedSize)) ||
        (descriptor && ((u32(header + 14) && u32(header + 14) != entry.crc32) ||
                        (u32(header + 18) && u32(header + 18) != entry.compressedSize) ||
                        (u32(header + 22) && u32(header + 22) != entry.uncompressedSize))))
        return ZipError::InvalidEntry;
    const auto nameLength = u16(header + 26), extraLength = u16(header + 28);
    if (nameLength != c.nameLength || nameLength > 1023) return ZipError::InvalidEntry;
    dataOffset = std::uint64_t(entry.localHeaderOffset) + 30 + nameLength + extraLength;
    const auto payloadEnd = dataOffset + entry.compressedSize;
    if (payloadEnd > dir.start) return ZipError::InvalidEntry;
    std::uint8_t local[128], archived[128];
    for (std::size_t pos = 0; pos < nameLength;) {
        const auto count = std::min(sizeof(local), std::size_t(nameLength) - pos);
        error = file.readAt(std::uint64_t(entry.localHeaderOffset) + 30 + pos, local, count);
        if (error != ZipError::Ok) return error;
        error = file.readAt(std::uint64_t(entry.centralHeaderOffset) + 46 + pos, archived, count);
        if (error != ZipError::Ok) return error;
        if (std::memcmp(local, archived, count) || std::memchr(local, 0, count)) return ZipError::InvalidEntry;
        if (pos + count == nameLength && (local[count - 1] == '/' || local[count - 1] == '\\'))
            return ZipError::InvalidEntry;
        pos += count;
    }
    if ((c.attributes & 0x10) || ((c.attributes >> 16) & 0xF000) == 0x4000)
        return ZipError::InvalidEntry;
    error = extraFields(file, std::uint64_t(entry.localHeaderOffset) + 30 + nameLength, extraLength);
    if (error != ZipError::Ok) return error;
    if (descriptor) {
        std::uint8_t values[16];
        if (payloadEnd + 12 > dir.start) return ZipError::InvalidEntry;
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(16, dir.start - payloadEnd));
        error = file.readAt(payloadEnd, values, available);
        if (error != ZipError::Ok) return error;
        const bool plain = u32(values) == entry.crc32 && u32(values + 4) == entry.compressedSize &&
                           u32(values + 8) == entry.uncompressedSize;
        const bool signedRecord = available >= 16 && u32(values) == 0x08074B50 &&
            u32(values + 4) == entry.crc32 && u32(values + 8) == entry.compressedSize &&
            u32(values + 12) == entry.uncompressedSize;
        if (!plain && !signedRecord) return ZipError::InvalidEntry;
    }
    return ZipError::Ok;
}

/**
 * @brief Возвращает таблицу полинома CRC32 для ZIP; состояние CRC принадлежит конкретному чтению.
 */
constexpr std::array<std::uint32_t, 256> crcTable() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xEDB88320u : 0u);
        table[i] = value;
    }
    return table;
}
constexpr auto crcLookup = crcTable();
/**
 * @brief Продолжает неинвертированное состояние CRC32 общим полиномом ZIP.
 * @param crc Состояние предыдущих блоков, для первого0xFFFFFFFF.
 * @param data Заимствованный входной диапазон; функция не освобождает его.
 * @param length Длина читаемого диапазона в байтах.
 */
std::uint32_t updateCrc(std::uint32_t crc, const std::uint8_t* data, std::size_t length) noexcept {
    for (std::size_t i = 0; i < length; ++i) crc = (crc >> 8) ^ crcLookup[(crc ^ data[i]) & 255];
    return crc;
}

/** @brief Завершает стандартный ZIP CRC32 для одного проверенного диапазона. */
std::uint32_t crc32(const std::uint8_t* data, std::size_t length) noexcept {
    return updateCrc(0xFFFFFFFF, data, length) ^ 0xFFFFFFFF;
}

/** @brief Ограниченный битовый вход DEFLATE; чтение за сжатый диапазон возвращает ошибку. */
struct Input {
    std::FILE* file = nullptr;
    std::uint32_t length = 0, consumed = 0;
    std::size_t cursor = 0, available = 0;
    std::uint8_t bytes[512]{};
    std::uint32_t bits = 0;
    unsigned bitCount = 0;
    ZipError error = ZipError::Ok;
    /**
     * @brief Берёт следующий ограниченный байт сжатого входа, фиксируя конец/ошибку без чтения за диапазоном.
     * @param result Выход разбора/декодирования; результат считается действительным только при успешном статусе.
     */
    bool getByte(std::uint32_t& result) noexcept {
        if (consumed == length) { error = ZipError::Truncated; return false; }
        if (cursor == available) {
            available = std::fread(bytes, 1, std::min<std::uint32_t>(sizeof(bytes), length - consumed), file);
            cursor = 0;
            if (!available) {
                error = std::ferror(file) ? ZipError::Io : ZipError::Truncated;
                return false;
            }
        }
        result = bytes[cursor++];
        ++consumed;
        return true;
    }
    /**
     * @brief Читает требуемое число младших битов DEFLATE из ограниченного входа.
     * @param count Число элементов/байтов в переданном диапазоне.
     * @param result Выход разбора/декодирования; результат считается действительным только при успешном статусе.
     */
    bool readBits(unsigned count, std::uint32_t& result) noexcept {
        while (bitCount < count) {
            std::uint32_t byte;
            if (!getByte(byte)) return false;
            bits |= byte << bitCount;
            bitCount += 8;
        }
        result = bits & ((1u << count) - 1);
        bits >>= count;
        bitCount -= count;
        return true;
    }
    /**
     * @brief Отбрасывает остаток битов до следующей байтовой границы stored-блока.
     */
    void align() noexcept {
        const unsigned discard = bitCount & 7;
        bits >>= discard;
        bitCount -= discard;
    }
};

/** @brief Общая каноническая таблица; ввод и ошибки остаются у владельца декодера. */
using Huffman = ::nes::compression::Huffman;

/** @brief Рабочее состояние распаковки DEFLATE; LZ77-история хранится в выходной памяти вызывающего. */
struct InflateWorkspace {
    Input input;
    Huffman literals, distances, codeLengths;
    std::uint8_t lengths[320]{}, shortLengths[19]{};
};

using ::nes::compression::lengthBase;
using ::nes::compression::lengthBits;
using ::nes::compression::distanceBase;
using ::nes::compression::distanceBits;

/**
 * @brief Строит fixed/dynamic Huffman-таблицы DEFLATE с проверкой повторов и пределов алфавита.
 * @param w Рабочий контекст Huffman/DEFLATE, принадлежащий текущей распаковке.
 * @param type Тип блока DEFLATE: stored/fixed/dynamic; недопустимый код вызывает отказ.
 */
ZipError tables(InflateWorkspace& w, unsigned type) noexcept {
    if (type == 1) {
        for (unsigned i = 0; i < 288; ++i) w.lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        if (!w.literals.build(w.lengths, 288, false, true)) return ZipError::InvalidDeflate;
        std::memset(w.lengths, 5, 32);
        return w.distances.build(w.lengths, 32, false, true) ? ZipError::Ok : ZipError::InvalidDeflate;
    }
    auto& input = w.input;
    std::uint32_t literalCount, distanceCount, codeCount;
    if (!input.readBits(5, literalCount) || !input.readBits(5, distanceCount) || !input.readBits(4, codeCount))
        return input.error;
    literalCount += 257; distanceCount += 1; codeCount += 4;
    if (literalCount > 286) return ZipError::InvalidDeflate;
    using ::nes::compression::codeLengthOrder;
    std::memset(w.shortLengths, 0, sizeof(w.shortLengths));
    for (unsigned i = 0; i < codeCount; ++i) {
        std::uint32_t bits;
        if (!input.readBits(3, bits)) return input.error;
        w.shortLengths[codeLengthOrder[i]] = static_cast<std::uint8_t>(bits);
    }
    if (!w.codeLengths.build(w.shortLengths, 19, false, true)) return ZipError::InvalidDeflate;
    const unsigned total = literalCount + distanceCount;
    for (unsigned pos = 0; pos < total;) {
        unsigned symbol;
        if (!w.codeLengths.decode(input, symbol, ZipError::InvalidDeflate)) return input.error;
        if (symbol <= 15) { w.lengths[pos++] = static_cast<std::uint8_t>(symbol); continue; }
        unsigned count, value = 0;
        std::uint32_t bits;
        if (symbol == 16) {
            if (!pos) return ZipError::InvalidDeflate;
            if (!input.readBits(2, bits)) return input.error;
            count = bits + 3; value = w.lengths[pos - 1];
        } else if (symbol == 17) {
            if (!input.readBits(3, bits)) return input.error;
            count = bits + 3;
        } else if (symbol == 18) {
            if (!input.readBits(7, bits)) return input.error;
            count = bits + 11;
        } else return ZipError::InvalidDeflate;
        if (count > total - pos) return ZipError::InvalidDeflate;
        std::memset(w.lengths + pos, value, count);
        pos += count;
    }
    if (!w.lengths[256] || !w.literals.build(w.lengths, literalCount, false, false) ||
        !w.distances.build(w.lengths + literalCount, distanceCount, true, false)) return ZipError::InvalidDeflate;
    return ZipError::Ok;
}

/**
 * @brief Распаковывает stored/fixed/dynamic DEFLATE и выполняет проверенные обратные LZ77-копирования в память вызывающего.
 * @param file RAII-обёртка открытого ZIP; дескриптор не передаётся во владение помощника.
 * @param entry Повторно проверяемая выбранная ZIP-запись; исходный архив не должен незаметно измениться.
 * @param output Выходной буфер вызывающего; также хранит LZ77-историю, не записывается на SD.
 * @param goal Требуемое число распакованных байтов; для prefix не означает полную CRC-проверку.
 * @param prefix Разрешить завершение после заголовочного префикса; поздний CRC проверяет только полное чтение.
 * @param produced Выходное число байтов, реально записанных декодером.
 */
ZipError inflate(File& file, const ZipEntry& entry, std::uint8_t* output,
                 std::size_t goal, bool prefix, std::size_t& produced) noexcept {
    auto workspace = std::unique_ptr<InflateWorkspace>(new (std::nothrow) InflateWorkspace);
    if (!workspace) return ZipError::NoMemory;
    auto& w = *workspace;
    auto& input = w.input;
    input.file = file.stream;
    input.length = entry.compressedSize;
    produced = 0;
    if (prefix && !goal) return ZipError::Ok;
    bool last = false;
    while (!last) {
        std::uint32_t final, type;
        if (!input.readBits(1, final) || !input.readBits(2, type)) return input.error;
        last = final != 0;
        if (type == 0) {
            input.align();
            std::uint32_t length, inverse;
            if (!input.readBits(16, length) || !input.readBits(16, inverse)) return input.error;
            if ((length ^ inverse) != 0xFFFF) return ZipError::InvalidDeflate;
            for (unsigned i = 0; i < length; ++i) {
                std::uint32_t value;
                if (!input.readBits(8, value)) return input.error;
                if (produced == entry.uncompressedSize) return ZipError::InvalidDeflate;
                output[produced++] = static_cast<std::uint8_t>(value);
                if (prefix && produced == goal) return ZipError::Ok;
            }
            continue;
        }
        if (type == 3) return ZipError::InvalidDeflate;
        auto error = tables(w, type);
        if (error != ZipError::Ok) return error;
        for (;;) {
            unsigned symbol;
            if (!w.literals.decode(input, symbol, ZipError::InvalidDeflate)) return input.error;
            if (symbol < 256) {
                if (produced == entry.uncompressedSize) return ZipError::InvalidDeflate;
                output[produced++] = static_cast<std::uint8_t>(symbol);
                if (prefix && produced == goal) return ZipError::Ok;
            } else if (symbol == 256) break;
            else {
                if (symbol > 285) return ZipError::InvalidDeflate;
                const auto index = symbol - 257;
                std::uint32_t extra;
                if (!input.readBits(lengthBits[index], extra)) return input.error;
                const auto length = lengthBase[index] + extra;
                unsigned distanceCode;
                if (!w.distances.decode(input, distanceCode, ZipError::InvalidDeflate)) return input.error;
                if (distanceCode >= 30) return ZipError::InvalidDeflate;
                if (!input.readBits(distanceBits[distanceCode], extra)) return input.error;
                const auto distance = distanceBase[distanceCode] + extra;
                if (distance > produced || length > entry.uncompressedSize - produced)
                    return ZipError::InvalidDeflate;
                for (unsigned i = 0; i < length; ++i) {
                    output[produced] = output[produced - distance];
                    ++produced;
                    if (prefix && produced == goal) return ZipError::Ok;
                }
            }
        }
    }
    if (produced != entry.uncompressedSize || input.consumed != entry.compressedSize)
        return ZipError::InvalidDeflate;
    return ZipError::Ok;
}

/**
 * @brief Связывает выбранную ZIP-запись с её ограниченным входом/выходом и выбранной глубиной проверки.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param capacity Вместимость выходной памяти в байтах.
 * @param entry Повторно проверяемая выбранная ZIP-запись; исходный архив не должен незаметно измениться.
 * @param output Выходной буфер вызывающего; также хранит LZ77-историю, не записывается на SD.
 * @param bytesRead Выходное число успешно полученных распакованных байтов.
 * @param prefix Разрешить завершение после заголовочного префикса; поздний CRC проверяет только полное чтение.
 */
ZipError readMember(const char* path, const ZipEntry& entry, std::uint8_t* output,
                    std::size_t capacity, std::size_t& bytesRead, bool prefix) noexcept {
    bytesRead = 0;
    if ((!output && capacity) || (!prefix && capacity < entry.uncompressedSize)) return ZipError::OutputTooSmall;
    File file;
    auto error = file.open(path);
    if (error != ZipError::Ok) return error;
    Directory dir;
    error = directory(file, dir);
    if (error != ZipError::Ok) return error;
    std::uint64_t start;
    error = validateEntry(file, dir, entry, start);
    if (error != ZipError::Ok) return error;
    if (!file.seek(start)) return ZipError::Io;
    const auto goal = prefix ? std::min<std::size_t>(capacity, entry.uncompressedSize) : entry.uncompressedSize;
    std::size_t produced = 0;
    if (entry.method == 0) {
        if (goal && std::fread(output, 1, goal, file.stream) != goal)
            return std::ferror(file.stream) ? ZipError::Io : ZipError::Truncated;
        produced = goal;
    } else {
        error = inflate(file, entry, output, goal, prefix, produced);
        if (error != ZipError::Ok) return error;
    }
    if (!prefix && crc32(output, produced) != entry.crc32) return ZipError::CrcMismatch;
    bytesRead = produced;
    return ZipError::Ok;
}
} // namespace

/**
 * @brief Читает исходные байты файла небольшими блоками без выделения ROM-буфера.
 * @details Размер фиксируется File::open; до успеха результаты остаются нулевыми.
 * Поздний короткий read отличается от ошибки FILE, RAII закрывает файл на любом
 * выходе. Ограничение64MiB относится только к этой диагностической операции.
 */
ZipError ZipArchive::archiveCrc(const char* path, std::uint32_t& crc,
                                std::uint64_t& bytes) noexcept {
    crc = 0; bytes = 0;
    File file;
    const auto opened = file.open(path);
    if (opened != ZipError::Ok) return opened;
    if (file.size > 64ULL * 1024 * 1024) return ZipError::TooLarge;
    if (!file.seek(0)) return ZipError::Io;
    std::uint8_t block[512];
    std::uint32_t running = 0xFFFFFFFF;
    std::uint64_t consumed = 0;
    while (consumed < file.size) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(block), file.size - consumed));
        if (std::fread(block, 1, count, file.stream) != count)
            return std::ferror(file.stream) ? ZipError::Io : ZipError::Truncated;
        running = updateCrc(running, block, count);
        consumed += count;
    }
    crc = running ^ 0xFFFFFFFF;
    bytes = consumed;
    return ZipError::Ok;
}

/**
 * @brief Перечисляет файловые записи центрального каталога ZIP, проверяя границы и формат. Каталоги пропускаются; расширение ROM фильтрует visitor. Файлы на SD не изменяются.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param context Непрозрачный контекст вызывающего; scan передаёт его visitor без изменения и без владения.
 * @param visitor Callback индексации одной временной записи ZIP; false отменяет scan.
 */
ZipError ZipArchive::scan(const char* path, Visitor visitor, void* context) noexcept {
    if (!visitor) return ZipError::InvalidEntry;
    File file;
    auto error = file.open(path);
    if (error != ZipError::Ok) return error;
    Directory dir;
    error = directory(file, dir);
    if (error != ZipError::Ok) return error;
    auto name = std::unique_ptr<char[]>(new (std::nothrow) char[1024]);
    if (!name) return ZipError::NoMemory;
    std::uint64_t offset = dir.start;
    for (unsigned i = 0; i < dir.entries; ++i) {
        Central c;
        error = central(file, dir, offset, c);
        if (error != ZipError::Ok) return error;
        if (c.nameLength > 1023) return ZipError::NameTooLong;
        error = file.readAt(offset + 46, name.get(), c.nameLength);
        if (error != ZipError::Ok) return error;
        if (std::memchr(name.get(), 0, c.nameLength)) return ZipError::InvalidEntry;
        name[c.nameLength] = 0;
        const char last = name[c.nameLength - 1];
        const bool directoryEntry = last == '/' || last == '\\' || (c.attributes & 0x10) ||
            ((c.attributes >> 16) & 0xF000) == 0x4000;
        if (!directoryEntry && !visitor(name.get(), c.nameLength, c.entry, context)) return ZipError::Cancelled;
        offset = c.next;
    }
    // После записей допускается классическая цифровая подпись центрального каталога.
    if (offset != dir.end()) {
        std::uint8_t signature[6];
        if (dir.end() - offset < 6) return ZipError::InvalidArchive;
        error = file.readAt(offset, signature, sizeof(signature));
        if (error != ZipError::Ok) return error;
        if (u32(signature) != 0x05054B50 || offset + 6 + u16(signature + 4) != dir.end())
            return ZipError::InvalidArchive;
    }
    return ZipError::Ok;
}

/**
 * @brief Читает ограниченный распакованный префикс выбранной записи для индексирования заголовка; это ещё не полная CRC-проверка ROM.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param capacity Вместимость выходной памяти в байтах.
 * @param entry Повторно проверяемая выбранная ZIP-запись; исходный архив не должен незаметно измениться.
 * @param output Выходной буфер вызывающего; также хранит LZ77-историю, не записывается на SD.
 * @param bytesRead Выходное число успешно полученных распакованных байтов.
 */
ZipError ZipArchive::readPrefix(const char* path, const ZipEntry& entry, std::uint8_t* output,
                               std::size_t capacity, std::size_t& bytesRead) noexcept {
    return readMember(path, entry, output, capacity, bytesRead, true);
}
/**
 * @brief Распаковывает выбранную запись в буфер вызывающего и проверяет размер/CRC. Объём и история LZ77 ограничены предоставленной памятью.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param capacity Вместимость выходной памяти в байтах.
 * @param entry Повторно проверяемая выбранная ZIP-запись; исходный архив не должен незаметно измениться.
 * @param output Выходной буфер вызывающего; также хранит LZ77-историю, не записывается на SD.
 * @param bytesRead Выходное число успешно полученных распакованных байтов.
 */
ZipError ZipArchive::read(const char* path, const ZipEntry& entry, std::uint8_t* output,
                         std::size_t capacity, std::size_t& bytesRead) noexcept {
    return readMember(path, entry, output, capacity, bytesRead, false);
}
/**
 * @brief Возвращает постоянное русифицируемое описание ZipError для пользовательского сообщения.
 * @param error Код результата ZIP; возвращаемая статическая строка не требует освобождения.
 */
const char* ZipArchive::errorText(ZipError error) noexcept {
    switch (error) {
    case ZipError::Ok: return "ok";
    case ZipError::Io: return "ZIP read error";
    case ZipError::InvalidArchive: return "invalid ZIP directory";
    case ZipError::Unsupported: return "unsupported ZIP format";
    case ZipError::Encrypted: return "encrypted ZIP member";
    case ZipError::TooLarge: return "ZIP exceeds classic limits";
    case ZipError::NameTooLong: return "ZIP member name too long";
    case ZipError::Cancelled: return "ZIP scan cancelled";
    case ZipError::InvalidEntry: return "invalid ZIP member metadata";
    case ZipError::Truncated: return "truncated ZIP data";
    case ZipError::InvalidDeflate: return "invalid DEFLATE stream";
    case ZipError::CrcMismatch: return "ZIP CRC mismatch";
    case ZipError::OutputTooSmall: return "ZIP output buffer too small";
    case ZipError::NoMemory: return "ZIP workspace allocation failed";
    }
    return "ZIP error";
}
} // namespace nes::storage
