/**
 * @file ZipArchive.h
 * @brief Собственный безопасный reader ZIP/DEFLATE для загрузки ROM в память вызывающего.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nes::storage {

/** @brief Результаты чтения/формата/CRC ZIP; неверный архив не принимается частично как готовая ROM. */
enum class ZipError : std::uint8_t {
    Ok, Io, InvalidArchive, Unsupported, Encrypted, TooLarge, NameTooLong,
    Cancelled, InvalidEntry, Truncated, InvalidDeflate, CrcMismatch,
    OutputTooSmall, NoMemory
};

/** @brief Идентичность участника ZIP с размерами, методом, CRC и смещениями для повторной проверки. */
struct ZipEntry {
    std::uint32_t localHeaderOffset = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t centralHeaderOffset = 0;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
};

/** @brief Собственный ограниченный ZIP/DEFLATE reader без извлечения игр на SD; выходная память принадлежит вызывающему. */
class ZipArchive final {
public:
    /**
     * @brief Считает CRC32 всех байтов файла для диагностики SD/локальной копии.
     * @param archivePath Заимствованный UTF-8 путь; файл открывается только на чтение.
     * @param crc Выходной CRC32 файла, действителен только при Ok; при ошибке0.
     * @param bytes Проверенный размер в байтах, при ошибке0.
     * @return Ok для полностью прочитанного файла, включая пустой; TooLarge
     * при размере больше64MiB, Io/Truncated при ошибке/недостатке входа.
     * @details Не разбирает ZIP и не распаковывает ROM. Использует512байт
     * на стеке и тот же CRC-полином, что проверка распакованной записи.
     * Вызывающий исключает изменение файла и одновременный доступ загрузчика.
     */
    static ZipError archiveCrc(const char* archivePath, std::uint32_t& crc,
                               std::uint64_t& bytes) noexcept;
    // Временное UTF-8/raw имя ZIP с NUL, не более1023байтов.
    // Записи каталогов пропускаются; расширения файлов выбирает потребитель.
    // false от потребителя отменяет scan с ZipError::Cancelled.
    using Visitor = bool (*)(const char* name, std::size_t nameLength,
                            const ZipEntry&, void*) noexcept;
    /**
     * @brief Перечисляет файловые записи центрального каталога ZIP, проверяя границы и формат. Каталоги пропускаются; расширение ROM фильтрует visitor. Файлы на SD не изменяются.
     * @param context Непрозрачный контекст вызывающего, без изменений передаётся visitor; scan его не освобождает.
     * @param archivePath Заимствованный путь ZIP для двоичного чтения.
     * @param visitor Callback индексации одной временной записи ZIP; false отменяет scan.
     */
    static ZipError scan(const char* archivePath, Visitor visitor, void* context) noexcept;
    // Prefix-чтение проверяет метаданные/границы, но намеренно останавливается до CRC
    // и поздних ошибок DEFLATE. Полное чтение требует точных размера/CRC.
    /**
     * @brief Читает ограниченный распакованный префикс выбранной записи для индексирования заголовка; это ещё не полная CRC-проверка ROM.
     * @param destination Буфер вызывающего для результата; функция не сохраняет владение после завершения.
     * @param capacity Вместимость выходной памяти в байтах.
     * @param archivePath Заимствованный путь ZIP для двоичного чтения.
     * @param bytesRead Выходное число успешно полученных распакованных байтов.
     */
    static ZipError readPrefix(const char* archivePath, const ZipEntry&,
                               std::uint8_t* destination, std::size_t capacity,
                               std::size_t& bytesRead) noexcept;
    /**
     * @brief Распаковывает выбранную запись в буфер вызывающего и проверяет размер/CRC. Объём и история LZ77 ограничены предоставленной памятью.
     * @param destination Буфер вызывающего для результата; функция не сохраняет владение после завершения.
     * @param capacity Вместимость выходной памяти в байтах.
     * @param archivePath Заимствованный путь ZIP для двоичного чтения.
     * @param bytesRead Выходное число успешно полученных распакованных байтов.
     */
    static ZipError read(const char* archivePath, const ZipEntry&,
                         std::uint8_t* destination, std::size_t capacity,
                         std::size_t& bytesRead) noexcept;
    /**
     * @brief Возвращает постоянное русифицируемое описание ZipError для пользовательского сообщения.
     * @param error Код результата ZIP; возвращаемая статическая строка не требует освобождения.
     */
    static const char* errorText(ZipError error) noexcept;
};

} // namespace nes::storage
