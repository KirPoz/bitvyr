/**
 * @file GameLibrary.cpp
 * @brief Индексирование дерева SD и загрузка одной ROM с проверкой поколения и изменения файла.
 */
#include "GameLibrary.h"
#include "ZipArchive.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif
#if defined(_WIN32) && !defined(ESP_PLATFORM)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#if defined(NES_LIBRARY_TEST_ALLOCATOR)
extern "C" 
    /**
     * @brief Тестовый внешний аллокатор для воспроизводимого отказа выделения индекса; в прошивке заменён обычной ветвью выделения.
     */
    void* nesLibraryTestRealloc(void*, std::size_t) noexcept;
#endif

namespace nes::app {
namespace {
constexpr std::size_t pathLimit = sizeof(GameInfo::path);
constexpr unsigned depthLimit = 16;
/**
 * @brief Изменяет собственный блок индекса; на ESP использует PSRAM, в host — realloc. Нулевой размер освобождает блок, отказ сохраняет прежнюю память по контракту realloc.
 * @param memory Собственный ранее выделенный блок либо nullptr; успешный resize может изменить его адрес.
 * @param bytes Новый полный размер блока в байтах; ноль освобождает собственный блок.
 */
void* resize(void* memory, std::size_t bytes) noexcept {
#if defined(NES_LIBRARY_TEST_ALLOCATOR)
    return nesLibraryTestRealloc(memory, bytes);
#elif defined(ESP_PLATFORM)
    return heap_caps_realloc(memory, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    if (!bytes) { std::free(memory); return nullptr; }
    return std::realloc(memory, bytes);
#endif
}
/**
 * @brief Формирует ограниченное сообщение библиотеки с необязательным путём и сохраняет его в error-буфере.
 * @param path Необязательная завершённая строка пути; nullptr исключает суффикс сообщения.
 * @param error Выходной буфер минимум192 байта для snprintf с завершающим NUL.
 * @param text Заимствованная завершённая строка описания ошибки.
 */
void message(char* error, const char* text, const char* path = nullptr) noexcept {
    std::snprintf(error, 192, "%s%s%s", text, path ? ": " : "", path ? path : "");
#if defined(ESP_PLATFORM)
    ESP_LOGE("nes_library", "%s", error);
#endif
}
/** @brief Метка размера, времени и типа файла для повторной проверки перед/после загрузки. */
struct Stamp { std::uint64_t size = 0, modified = 0; bool directory = false, link = false; };
/**
 * @brief Сравнивает сохранённую и текущую метку файла, исключая замену ссылкой/каталогом.
 * @param a Первое уже проверенное значение/описание для точного сравнения.
 * @param b Второе уже проверенное значение/описание для точного сравнения.
 */
bool same(const Stamp& a, const Stamp& b) noexcept {
    return a.size == b.size && a.modified == b.modified && !b.link && !b.directory;
}
#if defined(_WIN32) && !defined(ESP_PLATFORM)
/**
 * @brief Преобразует UTF-8 путь в Windows UTF-16 и при необходимости добавляет префикс длинного пути. Проверяет вместимость без усечения.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param wide Выходной фиксированный UTF-16 путь Windows с проверенной вместимостью.
 */
bool widePath(const char* path, wchar_t (&wide)[pathLimit]) noexcept {
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, int(pathLimit))) return false;
    std::size_t length = 0;
    for (; wide[length]; ++length) if (wide[length] == L'/') wide[length] = L'\\';
    if (length >= 244 && length > 2 && wide[1] == L':' && wide[2] == L'\\') {
        if (length + 4 >= pathLimit) return false;
        std::memmove(wide + 4, wide, (length + 1) * sizeof(wchar_t));
        std::memcpy(wide, L"\\\\?\\", 4 * sizeof(wchar_t));
    }
    return true;
}
/**
 * @brief Читает размер/время/тип файла для защиты от изменения между индексированием и загрузкой.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param stamp Снимок размера/mtime/типа файла для обнаружения изменения при загрузке.
 */
bool statPath(const char* path, Stamp& stamp) noexcept {
    wchar_t wide[pathLimit]; WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!widePath(path, wide) || !GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        const auto e = GetLastError(); errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : EIO; return false;
    }
    stamp = {(std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
             (std::uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime,
             (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
             (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0};
    return true;
}
/**
 * @brief Открывает файл только для двоичного чтения с платформенным преобразованием пути.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 */
std::FILE* openFile(const char* path) noexcept {
    wchar_t wide[pathLimit]; return widePath(path, wide) ? _wfopen(wide, L"rb") : nullptr;
}
/** @brief Ограниченные данные открытого каталога/центрального каталога ZIP; OS-handle закрывает соответствующая RAII-цепочка. */
struct Directory { HANDLE handle = INVALID_HANDLE_VALUE; WIN32_FIND_DATAW data{}; bool first = true; };
/**
 * @brief Открывает перечисление каталога; возвращаемый ресурс нужно передать closeDirectory.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 */
Directory* openDirectory(const char* path) noexcept {
    auto* d = static_cast<Directory*>(resize(nullptr, sizeof(Directory))); if (!d) { errno = ENOMEM; return nullptr; }
    new (d) Directory; char pattern[pathLimit];
    if (std::strlen(path) + 3 > sizeof(pattern)) { resize(d, 0); errno = ENAMETOOLONG; return nullptr; }
    std::snprintf(pattern, sizeof(pattern), "%s/*", path); wchar_t wide[pathLimit];
    if (!widePath(pattern, wide)) { resize(d, 0); errno = EINVAL; return nullptr; }
    d->handle = FindFirstFileW(wide, &d->data);
    if (d->handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND) { resize(d, 0); errno = EIO; return nullptr; }
    return d;
}
/**
 * @brief Возвращает очередное имя записи и различает конец списка от ошибки перечисления.
 * @param d Собственный открытый ресурс перечисления каталога; закрывается closeDirectory.
 * @param name Выход имени UTF-8 ёмкостью pathLimit с NUL; память остаётся у вызывающего.
 * @param failed Выходной признак I/O-ошибки; false и nullptr вместе означают обычный конец каталога.
 */
const char* nextDirectory(Directory* d, char (&name)[pathLimit], bool& failed) noexcept {
    if (d->handle == INVALID_HANDLE_VALUE) return nullptr;
    if (!d->first && !FindNextFileW(d->handle, &d->data)) { failed = GetLastError() != ERROR_NO_MORE_FILES; return nullptr; }
    d->first = false;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, d->data.cFileName, -1, name, int(pathLimit), nullptr, nullptr)) { failed = true; return nullptr; }
    return name;
}
/**
 * @brief Закрывает собственный ресурс перечисления каталога и освобождает его обёртку.
 * @param d Собственный открытый ресурс перечисления каталога; закрывается closeDirectory.
 */
void closeDirectory(Directory* d) noexcept { if (d->handle != INVALID_HANDLE_VALUE) FindClose(d->handle); resize(d, 0); }
#else
using Directory = DIR;
/**
 * @brief Читает размер/время/тип файла для защиты от изменения между индексированием и загрузкой.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param stamp Снимок размера/mtime/типа файла для обнаружения изменения при загрузке.
 */
bool statPath(const char* path, Stamp& stamp) noexcept {
    struct stat data{};
#if defined(ESP_PLATFORM)
    if (stat(path, &data)) return false;
#else
    if (lstat(path, &data)) return false;
#endif
    if (data.st_size < 0) return false;
    stamp = {std::uint64_t(data.st_size), std::uint64_t(data.st_mtime), S_ISDIR(data.st_mode), false};
#if !defined(ESP_PLATFORM)
    stamp.link = S_ISLNK(data.st_mode);
#if defined(__linux__)
    stamp.modified = std::uint64_t(data.st_mtim.tv_sec) * 1000000000ULL + std::uint64_t(data.st_mtim.tv_nsec);
#endif
#endif
    return true;
}
/**
 * @brief Открывает файл только для двоичного чтения с платформенным преобразованием пути.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 */
std::FILE* openFile(const char* path) noexcept { return std::fopen(path, "rb"); }
/**
 * @brief Открывает перечисление каталога; возвращаемый ресурс нужно передать closeDirectory.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 */
Directory* openDirectory(const char* path) noexcept { return opendir(path); }
/**
 * @brief Возвращает очередное имя записи и различает конец списка от ошибки перечисления.
 * @param d Собственный открытый ресурс перечисления каталога; закрывается closeDirectory.
 * @param failed Выходной признак I/O-ошибки; false и nullptr вместе означают обычный конец каталога.
 */
const char* nextDirectory(Directory* d, char (&)[pathLimit], bool& failed) noexcept {
    errno = 0; const auto* entry = readdir(d); if (!entry) { failed = errno != 0; return nullptr; } return entry->d_name;
}
/**
 * @brief Закрывает собственный ресурс перечисления каталога и освобождает его обёртку.
 * @param d Собственный открытый ресурс перечисления каталога; закрывается closeDirectory.
 */
void closeDirectory(Directory* d) noexcept { closedir(d); }
#endif
/**
 * @brief Приводит ASCII-символ к единому регистру для сравнения расширений; не изменяет содержимое файла.
 * @param c Беззнаковый байт имени; меняются только ASCII A..Z.
 */
unsigned char folded(unsigned char c) noexcept { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
/**
 * @brief Проверяет расширение имени без зависимости от регистра ASCII.
 * @param name Завершённое имя файла; сравнивается только суффикс без изменения памяти.
 * @param suffix Проверяемое расширение с учётом нечувствительности к регистру ASCII.
 */
bool extension(const char* name, const char* suffix) noexcept {
    const auto a = std::strlen(name), b = std::strlen(suffix); if (a < b) return false;
    for (std::size_t i = 0; i < b; ++i) if (folded(name[a-b+i]) != folded(suffix[i])) return false;
    return true;
}
/**
 * @brief Возвращает заимствованный указатель на последний компонент пути.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 */
const char* basename(const char* path) noexcept {
    const char* name = path; for (; *path; ++path) if (*path == '/' || *path == '\\') name = path + 1; return name;
}
/**
 * @brief Создаёт ограниченное отображаемое имя без расширения; реальный путь остаётся отдельным полем.
 * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
 * @param name Выходной массив112 байт; UTF-8 усечён по границе символа и завершён NUL.
 */
void displayName(const char* path, char (&name)[112]) noexcept {
    const auto* text = reinterpret_cast<const unsigned char*>(basename(path));
    std::size_t length = std::strlen(reinterpret_cast<const char*>(text)); if (extension(reinterpret_cast<const char*>(text), ".nes")) length -= 4;
    std::size_t used = 0;
    for (std::size_t i = 0; i < length;) {
        unsigned bytes = text[i] < 0x80 ? 1 : (text[i] & 0xe0) == 0xc0 ? 2 : (text[i] & 0xf0) == 0xe0 ? 3 : (text[i] & 0xf8) == 0xf0 ? 4 : 0;
        bool valid = bytes && i + bytes <= length;
        for (unsigned j = 1; valid && j < bytes; ++j) valid = (text[i+j] & 0xc0) == 0x80;
        if (valid && bytes == 2) valid = text[i] >= 0xc2;
        if (valid && bytes == 3) valid = !(text[i] == 0xe0 && text[i+1] < 0xa0) && !(text[i] == 0xed && text[i+1] >= 0xa0);
        if (valid && bytes == 4) valid = text[i] <= 0xf4 && !(text[i] == 0xf0 && text[i+1] < 0x90) && !(text[i] == 0xf4 && text[i+1] >= 0x90);
        if (!valid) { bytes = 1; if (used + 1 >= sizeof(name)) break; name[used++] = '?'; }
        else { if (used + bytes >= sizeof(name)) break; std::memcpy(name + used, text + i, bytes); used += bytes; }
        i += bytes;
    }
    name[used] = 0;
}
/**
 * @brief Проверяет iNES-заголовок и объявленные PRG/CHR/trainer-размеры до принятия записи в каталог.
 * @param h Заимствованные байты заголовка iNES/NES2 для проверки размеров.
 * @param total Фактический полный размер ROM в байтах для проверки объявленного PRG/CHR.
 * @param mapper Выход номера mapper после разбора iNES/NES2.
 * @param pal Выход PAL-подсказки из заголовка/имени; читать только при true.
 * @param name Завершённое имя ROM для подсказки (E); для mapper160 эта подсказка исключена.
 */
bool sizes(const std::uint8_t* h, std::uint64_t total, std::uint16_t& mapper, bool& pal, const char* name) noexcept {
    if (total < 16 || std::memcmp(h, "NES\x1a", 4)) return false;
    const bool nes2 = (h[7] & 12) == 8;
    mapper = std::uint16_t((h[6] >> 4) | (h[7] & 0xf0) | (nes2 ? ((h[8] & 15) << 8) : 0));
    std::uint64_t prg = std::uint64_t(h[4] | (nes2 ? ((h[9] & 15) << 8) : 0)) * 16384;
    std::uint64_t chr = std::uint64_t(h[5] | (nes2 ? ((h[9] >> 4) << 8) : 0)) * 8192;
    if (nes2) {
        const auto exponent = /** @brief Декодирует exponent/multiplier NES2-размер с насыщением при непредставимом результате. */
                              [](unsigned value) noexcept -> std::uint64_t {
            const unsigned e = value >> 2, multiplier = (value & 3) * 2 + 1;
            return e >= 32 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t(1) << e) * multiplier;
        };
        if ((h[9] & 15) == 15) prg = exponent(h[4]);
        if ((h[9] >> 4) == 15) chr = exponent(h[5]);
    }
    const std::uint64_t start = 16 + ((h[6] & 4) ? 512 : 0);
    if (!prg || start > total || prg > total - start || chr > total - start - prg) return false;
    bool european = false;
    for (const char* p = name; p[0] && p[1] && p[2]; ++p) if (p[0] == '(' && folded(p[1]) == 'e' && p[2] == ')') european = true;
    pal = nes2 ? (h[12] & 3) == 1 : ((h[9] & 1) != 0 || (mapper != 160 && european)); return true;
}
/**
 * @brief Возвращает неизменяемую таблицу CRC32 для проверки загруженного ROM.
 */
constexpr std::array<std::uint32_t,256> crcTable() noexcept {
    std::array<std::uint32_t,256> table{};
    for (unsigned i = 0; i < 256; ++i) { auto c = std::uint32_t(i); for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0); table[i] = c; }
    return table;
}
/**
 * @brief Вычисляет CRC32 полного массива ROM; результат используется как идентичность загруженного содержимого.
 * @param bytes Заимствованный массив ROM из size байтов; память не изменяется.
 * @param size Число байтов, включаемых в CRC32.
 */
std::uint32_t checksum(const std::uint8_t* bytes, std::size_t size) noexcept {
    static constexpr auto table = crcTable(); std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 255];
    return ~crc;
}
} // namespace

/** @brief Закрытый владелец массивов записей/путей и очереди каталогов. Память освобождается без удаления файлов SD. */
struct GameLibrary::Index {
    /** @brief Компактная запись индекса с позицией строки пути/ZIP-участника и проверенными метаданными. */
    struct Entry {
        Stamp stamp{}; nes::storage::ZipEntry zip{};
        std::uint32_t path = 0, member = 0, size = 0, crc = 0;
        std::uint16_t mapper = 0;
        bool pal = false, archived = false, crcKnown = false;
        std::uint8_t header[16]{};
    };
    Entry* entries = nullptr; char* paths = nullptr;
    std::size_t count = 0, capacity = 0, used = 0, pathCapacity = 0, skipped = 0;
    /** @brief Элемент ограниченной очереди обхода подкаталогов с сохранением глубины. */
    struct Pending { std::uint32_t path; unsigned depth; };
    Pending* pending = nullptr;
    std::size_t pendingCount = 0, pendingCapacity = 0;
    char scanPath[pathLimit]{}, entryName[pathLimit]{}; char* error = nullptr;
    /**
     * @brief Освобождает собственные массивы и строки индекса, не удаляя исходные файлы.
     */
    ~Index() noexcept { resize(entries, 0); resize(paths, 0); resize(pending, 0); }
    /**
     * @brief Расширяет ограниченные массивы индекса с проверкой переполнения и OOM.
     * @param extra Дополнительное число элементов, для которых нужно зарезервировать память.
     */
    bool reserve(std::size_t extra) noexcept {
        if (count == std::numeric_limits<std::uint32_t>::max() || extra > std::numeric_limits<std::uint32_t>::max() - used) return false;
        if (count == capacity) {
            const auto next = capacity ? capacity + capacity / 2 + 1 : 32;
            if (next < capacity || next > std::numeric_limits<std::size_t>::max() / sizeof(Entry)) return false;
            auto* p = static_cast<Entry*>(resize(entries, next * sizeof(Entry))); if (!p) return false; entries = p; capacity = next;
        }
        if (extra > pathCapacity - used) {
            const auto grown = pathCapacity ? pathCapacity + pathCapacity / 2 + 1 : std::size_t(2048);
            if (grown < pathCapacity) return false;
            const auto next = std::max(used + extra, grown);
            auto* p = static_cast<char*>(resize(paths, next)); if (!p) return false; paths = p; pathCapacity = next;
        }
        return true;
    }
    /**
     * @brief Ставит каталог в ограниченный обход с проверкой длины пути и глубины.
     * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
     * @param depth Глубина обхода; ограничена depthLimit и не допускает неограниченной рекурсии.
     */
    bool queueDirectory(const char* path, unsigned depth) noexcept {
        const auto length = std::strlen(path) + 1;
        if (!reserve(length)) { message(error, "Not enough PSRAM to scan folders"); return false; }
        if (pendingCount == pendingCapacity) {
            const auto next = pendingCapacity ? pendingCapacity + pendingCapacity / 2 + 1 : 16;
            if (next < pendingCapacity || next > std::numeric_limits<std::size_t>::max() / sizeof(Pending)) return false;
            auto* memory = static_cast<Pending*>(resize(pending, next * sizeof(Pending)));
            if (!memory) { message(error, "Not enough PSRAM to scan folders"); return false; }
            pending = memory; pendingCapacity = next;
        }
        new (pending + pendingCount) Pending{static_cast<std::uint32_t>(used), depth};
        ++pendingCount; std::memcpy(paths + used, path, length); used += length; return true;
    }
    /**
     * @brief Добавляет проверенную запись ROM/ZIP в индекс после успешного резервирования всех данных.
     * @param path Заимствованный путь файла/каталога; открывается только разрешённая операция.
     * @param size Размер буфера/файла в байтах; не подменяет проверку формата.
     * @param member Имя выбранной ROM внутри ZIP; для обычного .nes пусто.
     * @param stamp Снимок размера/mtime/типа файла для обнаружения изменения при загрузке.
     * @param header Уже прочитанные байты iNES-заголовка для проверки размеров/mapper.
     * @param zip Необязательные метаданные записи ZIP; nullptr означает обычный файл.
     */
    bool append(const char* path, const char* member, const Stamp& stamp, const std::uint8_t* header,
                std::uint32_t size, const nes::storage::ZipEntry* zip = nullptr) noexcept {
        Entry entry{}; entry.stamp = stamp; entry.size = size; entry.archived = zip != nullptr;
        if (!sizes(header, size, entry.mapper, entry.pal, member ? member : path)) { ++skipped; return true; }
        const bool reuse = count && std::strcmp(paths + entries[count-1].path, path) == 0;
        const auto plen = reuse ? 0 : std::strlen(path) + 1, mlen = member ? std::strlen(member) + 1 : 1;
        if (!reserve(plen + mlen)) { message(error, "Not enough PSRAM to index games"); return false; }
        if (reuse) entry.path = entries[count-1].path;
        else { entry.path = static_cast<std::uint32_t>(used); std::memcpy(paths + used, path, plen); used += plen; }
        entry.member = static_cast<std::uint32_t>(used); if (member) std::memcpy(paths + used, member, mlen); else paths[used] = 0; used += mlen;
        if (zip) { entry.zip = *zip; entry.crc = zip->crc32; entry.crcKnown = true; }
        std::memcpy(entry.header, header, 16); new (entries + count) Entry(entry); ++count; return true;
    }
    /**
     * @brief Перечисляет дерево каталога, проверяет подходящие ROM/ZIP и учитывает пропуски/ошибки без извлечения игр на диск.
     * @param length Длина читаемого диапазона в байтах.
     * @param depth Глубина обхода; ограничена depthLimit и не допускает неограниченной рекурсии.
     */
    bool scan(unsigned depth, std::size_t length) noexcept {
        auto* directory = openDirectory(scanPath);
        if (!directory) { message(error, errno == ENOMEM ? "Not enough memory to scan games" : "Cannot open game folder", scanPath); return false; }
        bool failed = false, okay = true;
        while (const auto* name = nextDirectory(directory, entryName, failed)) {
            if (!std::strcmp(name, ".") || !std::strcmp(name, "..")) continue;
            const auto n = std::strlen(name);
            if (length + 1 + n >= pathLimit) { ++skipped; continue; }
            scanPath[length] = '/'; std::memcpy(scanPath + length + 1, name, n + 1); Stamp stamp{};
            if (!statPath(scanPath, stamp)) {
                if (errno == ENOENT) { ++skipped; scanPath[length] = 0; continue; }
                message(error, "Cannot inspect game file", scanPath); okay = false; break;
            }
            if (stamp.link) ++skipped;
            else if (stamp.directory) {
                if (depth >= depthLimit) ++skipped;
                else if (!queueDirectory(scanPath, depth + 1)) { okay = false; break; }
            }
            else if (extension(name, ".nes")) {
                if (stamp.size < 16 || stamp.size > std::numeric_limits<std::uint32_t>::max()) ++skipped;
                else {
                    auto* file = openFile(scanPath); std::uint8_t header[16]{};
                    if (!file) { message(error, "Cannot read game header", scanPath); okay = false; break; }
                    const auto got = std::fread(header, 1, sizeof(header), file); const auto closed = std::fclose(file);
                    if (got != sizeof(header) || closed) { message(error, "Cannot read game header", scanPath); okay = false; break; }
                    if (!append(scanPath, nullptr, stamp, header, static_cast<std::uint32_t>(stamp.size))) { okay = false; break; }
                }
            } else if (extension(name, ".zip")) {
                /** @brief Локальный контекст владельца ресурса; не передаётся за пределы описанной операции. */
                struct Context { Index* index; Stamp stamp; bool okay = true; } context{this, stamp};
                const auto startCount = count, startUsed = used;
                const auto visit = /** @brief Посетитель ZIP: отбирает .nes, читает только заголовок и добавляет проверенную запись в текущий индекс; false сообщает остановку/ошибку. */
                                   [](const char* member, std::size_t size, const nes::storage::ZipEntry& entry, void* opaque) noexcept -> bool {
                    auto& c = *static_cast<Context*>(opaque); if (!extension(member, ".nes")) return true;
                    if (size >= pathLimit || entry.uncompressedSize < 16) { ++c.index->skipped; return true; }
                    std::uint8_t header[16]{}; std::size_t read = 0;
                    const auto result = nes::storage::ZipArchive::readPrefix(c.index->scanPath, entry, header, sizeof(header), read);
                    if (result == nes::storage::ZipError::NoMemory || result == nes::storage::ZipError::Io) {
                        message(c.index->error, result == nes::storage::ZipError::NoMemory ? "Not enough memory to inspect archive" : "Cannot read game archive; check the card", c.index->scanPath);
                        c.okay = false; return false;
                    }
                    if (result != nes::storage::ZipError::Ok || read != sizeof(header)) { ++c.index->skipped; return true; }
                    c.okay = c.index->append(c.index->scanPath, member, c.stamp, header, entry.uncompressedSize, &entry); return c.okay;
                };
                const auto result = nes::storage::ZipArchive::scan(scanPath, visit, &context);
                if (!context.okay) { okay = false; break; }
                if (result != nes::storage::ZipError::Ok) {
                    count = startCount; used = startUsed;
                    if (result == nes::storage::ZipError::Io || result == nes::storage::ZipError::NoMemory) { message(error, "Cannot read game archive", scanPath); okay = false; break; }
                    ++skipped;
                }
            }
            scanPath[length] = 0;
        }
        closeDirectory(directory); scanPath[length] = 0;
        if (failed) { message(error, "Game folder read failed", scanPath); return false; } return okay;
    }
};

/**
 * @brief Освобождает принадлежащий библиотеке индекс и строки; активная ROM находится во владении приложения, а не индекса.
 */
GameLibrary::~GameLibrary() noexcept { clear(); }
/**
 * @brief Освобождает индекс и инвалидирует старые идентичности выбора; не форматирует и не удаляет файлы SD.
 */
void GameLibrary::clear() noexcept { if (index_) { index_->~Index(); resize(index_, 0); index_ = nullptr; } }
/**
 * @brief Возвращает фактическое число индексированных игр текущего поколения.
 */
std::size_t GameLibrary::count() const noexcept { return index_ ? index_->count : 0; }
/**
 * @brief Возвращает число пропущенных при сканировании записей; это не число несовместимых игровых сцен.
 */
std::size_t GameLibrary::skipped() const noexcept { return index_ ? index_->skipped : 0; }
/**
 * @brief Строит новый индекс каталога и вложенных директорий, проверяя пути и метаданные ROM/ZIP. При ошибке старая выборка не остаётся действующей.
 * @param root Заимствованный путь корня библиотеки; содержимое не изменяется.
 */
bool GameLibrary::open(const char* root) noexcept {
    clear(); ++generation_; if (!generation_) ++generation_; error_[0] = 0;
    if (!root || !*root || std::strlen(root) >= pathLimit - 2) { message(error_, "Game folder path is invalid or too long"); return false; }
    Stamp stamp{};
    if (!statPath(root, stamp) || !stamp.directory || stamp.link) { message(error_, "Game folder is unavailable", root); return false; }
    auto* memory = resize(nullptr, sizeof(Index)); if (!memory) { message(error_, "Not enough PSRAM to index games"); return false; }
    index_ = new (memory) Index; index_->error = error_;
    std::strcpy(index_->scanPath, root); auto length = std::strlen(root);
    while (length > 1 && (index_->scanPath[length-1] == '/' || index_->scanPath[length-1] == '\\')) index_->scanPath[--length] = 0;
    if (!index_->queueDirectory(index_->scanPath, 0)) { clear(); return false; }
    for (std::size_t directory = 0; directory < index_->pendingCount; ++directory) {
        const auto item = index_->pending[directory];
        std::strcpy(index_->scanPath, index_->paths + item.path);
        if (!index_->scan(item.depth, std::strlen(index_->scanPath))) { clear(); return false; }
    }
    resize(index_->pending, 0); index_->pending = nullptr; index_->pendingCount = index_->pendingCapacity = 0;
    const auto title = /** @brief Возвращает заимствованное отображаемое имя текущей записи индекса, не копируя путь. */
                       [this](const Index::Entry& entry) noexcept { return basename(index_->paths + (entry.archived ? entry.member : entry.path)); };
    if (index_->count > 1) std::sort(index_->entries, index_->entries + index_->count, /** @brief Упорядочивает игры по ASCII-folded имени, затем по пути/участнику для воспроизводимой навигации. */
                                                                                       [this, &title](const Index::Entry& a, const Index::Entry& b) noexcept {
        const char* x = title(a); const char* y = title(b); while (*x && *y && folded(*x) == folded(*y)) { ++x; ++y; }
        if (folded(*x) != folded(*y)) return folded(*x) < folded(*y);
        const auto path = std::strcmp(index_->paths + a.path, index_->paths + b.path); if (path) return path < 0;
        const auto member = std::strcmp(index_->paths + a.member, index_->paths + b.member); if (member) return member < 0;
        return a.zip.centralHeaderOffset < b.zip.centralHeaderOffset;
    });
#if defined(ESP_PLATFORM)
    ESP_LOGI("nes_library", "SD games: %u, skipped: %u", unsigned(index_->count), unsigned(index_->skipped));
#endif
    return true;
}
/**
 * @brief Копирует сведения выбранной записи и её generation/entry в GameInfo. Возвращает false для индекса вне текущего каталога.
 * @param info Идентичность выбранной ROM с generation/entry; при чтении должна принадлежать текущему каталогу.
 * @param position Нулевой индекс записи текущего поколения библиотеки.
 */
bool GameLibrary::game(std::size_t position, GameInfo& info) const noexcept {
    info = {}; if (!index_ || position >= index_->count) return false; const auto& entry = index_->entries[position];
    std::strcpy(info.path, index_->paths + entry.path); std::strcpy(info.member, index_->paths + entry.member);
    displayName(entry.archived ? info.member : info.path, info.name);
    info.size = entry.size; info.crc = entry.crc; info.mapper = entry.mapper; info.pal = entry.pal; info.archived = entry.archived;
    info.generation = generation_; info.entry = static_cast<std::uint32_t>(position); return true;
}
/**
 * @brief Читает выбранную ROM целиком в память вызывающего, перепроверяя generation, размер и изменения файла. Возвращает CRC только после успешного полного чтения.
 * @param info Идентичность выбранной ROM с generation/entry; при чтении должна принадлежать текущему каталогу.
 * @param destination Буфер вызывающего для результата; функция не сохраняет владение после завершения.
 * @param capacity Вместимость выходной памяти в байтах.
 * @param crc Выходной CRC32 полностью успешно прочитанной ROM.
 */
bool GameLibrary::read(const GameInfo& info, std::uint8_t* destination, std::size_t capacity, std::uint32_t& crc) const noexcept {
    crc = 0; error_[0] = 0;
    if (!index_ || info.generation != generation_ || info.entry >= index_->count) { message(error_, "Game selection is stale; rescan the card"); return false; }
    auto& entry = index_->entries[info.entry];
    if (!std::memchr(info.path, 0, sizeof(info.path)) || !std::memchr(info.member, 0, sizeof(info.member)) ||
        std::strcmp(info.path, index_->paths + entry.path) || std::strcmp(info.member, index_->paths + entry.member) ||
        info.size != entry.size || info.archived != entry.archived) { message(error_, "Game selection does not match the index"); return false; }
    if (!destination || capacity < entry.size) { message(error_, "Not enough memory to load this game"); return false; }
    Stamp before{}, after{};
    if (!statPath(info.path, before) || !same(entry.stamp, before)) { message(error_, "Game file changed or card removed; rescan", info.path); return false; }
    if (entry.archived) {
        std::size_t bytes = 0; const auto result = nes::storage::ZipArchive::read(info.path, entry.zip, destination, capacity, bytes);
        if (result != nes::storage::ZipError::Ok || bytes != entry.size) { message(error_, nes::storage::ZipArchive::errorText(result), info.path); return false; }
    } else {
        auto* file = openFile(info.path); if (!file) { message(error_, "Cannot open game; check the card", info.path); return false; }
        const auto bytes = std::fread(destination, 1, entry.size, file); const auto extra = std::fgetc(file);
        const bool failed = std::ferror(file) != 0; const auto closed = std::fclose(file);
        if (bytes != entry.size || extra != EOF || failed || closed) { message(error_, "Game read failed or size changed", info.path); return false; }
    }
    if (!statPath(info.path, after) || !same(before, after) || std::memcmp(destination, entry.header, 16)) { message(error_, "Game changed while reading; rescan the card", info.path); return false; }
    const auto actual = checksum(destination, entry.size);
    if ((entry.crcKnown && entry.crc != actual) || (info.crc && info.crc != actual)) { message(error_, "Game CRC check failed; rescan the card", info.path); return false; }
    entry.crc = actual; entry.crcKnown = true; crc = actual; return true;
}
} // namespace nes::app
