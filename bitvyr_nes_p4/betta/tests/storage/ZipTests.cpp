#include "ZipArchive.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace nes::storage;
/** @brief Собственное имя ZIP-записи и скопированные метаданные для проверяемого полного/префиксного чтения. */
struct Item { std::string name; ZipEntry entry; };
/**
 * @brief Копирует имя и метаданные ZIP-записи в вектор caller; исходные заимствованные строки не удерживаются.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
bool collect(const char* name, std::size_t length, const ZipEntry& entry, void* state) noexcept {
    static_cast<std::vector<Item>*>(state)->push_back({std::string(name, length), entry});
    return true;
}
/**
 * @brief Немедленно отменяет visitor для проверки штатного пути Cancelled без чтения содержимого записи.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
bool cancel(const char*, std::size_t, const ZipEntry&, void*) noexcept { return false; }
/**
 * @brief Сохраняет ровно size байтов в отдельный временный файл для независимого Python/zlib-сравнения.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
void save(const std::string& name, const std::uint8_t* data, std::size_t size) {
    std::ofstream stream(name, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data), size);
    if (!stream) std::abort();
}
/**
 * @brief Обрабатывает список synthetic архивов: сверяет CRC, префикс, полный образ, границы буферов и ошибки изменённых записей.
 * @details Выполняется только native-тестом; файлы SD и NVS устройства не затрагиваются.
 */
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::uint32_t missingCrc = 0xA5A5A5A5;
    std::uint64_t missingBytes = 99;
    const auto missing = std::string(argv[2]) + "/not-existing-archive.bin";
    if (ZipArchive::archiveCrc(missing.c_str(), missingCrc, missingBytes) != ZipError::Io ||
        missingCrc || missingBytes) return 4;
    std::ifstream list(argv[1]);
    std::string path;
    unsigned caseIndex = 0;
    while (std::getline(list, path)) {
        if (!path.empty() && path.back() == '\r') path.pop_back();
        std::uint32_t fileCrc = 0xA5A5A5A5;
        std::uint64_t fileBytes = 99;
        const auto crcError = ZipArchive::archiveCrc(path.c_str(), fileCrc, fileBytes);
        std::printf("CRC|%u|%u|%u|%llu\n", caseIndex, unsigned(crcError), fileCrc,
                    static_cast<unsigned long long>(fileBytes));
        std::vector<Item> items;
        const auto scan = ZipArchive::scan(path.c_str(), collect, &items);
        const auto cancelled = ZipArchive::scan(path.c_str(), cancel, nullptr);
        std::printf("SCAN|%u|%u|%zu|%u\n", caseIndex, unsigned(scan), items.size(), unsigned(cancelled));
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            const auto capacity = std::min<std::size_t>(item.entry.uncompressedSize, 4 * 1024 * 1024);
            std::vector<std::uint8_t> full(capacity + 32, 0xA7);
            std::array<std::uint8_t, 48> prefix{};
            prefix.fill(0xB6);
            std::size_t prefixCount = 999, fullCount = 999;
            const auto pe = ZipArchive::readPrefix(path.c_str(), item.entry, prefix.data() + 16, 16, prefixCount);
            const auto fe = ZipArchive::read(path.c_str(), item.entry, full.data() + 16, capacity, fullCount);
            bool guards = std::all_of(full.begin(), full.begin() + 16, /** @brief Возвращает true только для неизменённого защитного байта 0xA7 перед данными полного буфера. */ [](auto x) { return x == 0xA7; }) &&
                std::all_of(full.end() - 16, full.end(), /** @brief Возвращает true только для неизменённого защитного байта 0xA7 после данных полного буфера. */ [](auto x) { return x == 0xA7; }) &&
                std::all_of(prefix.begin(), prefix.begin() + 16, /** @brief Возвращает true только для неизменённого защитного байта 0xB6 перед данными буфера префикса. */ [](auto x) { return x == 0xB6; }) &&
                std::all_of(prefix.end() - 16, prefix.end(), /** @brief Возвращает true только для неизменённого защитного байта 0xB6 после данных буфера префикса. */ [](auto x) { return x == 0xB6; });
            // Callers receive zero usable bytes after any failure.
            guards &= (pe == ZipError::Ok || prefixCount == 0) && (fe == ZipError::Ok || fullCount == 0);
            for (const std::size_t cap : {std::size_t(0), std::size_t(1), std::size_t(15),
                                          std::size_t(17), std::size_t(33), std::size_t(512)}) {
                std::vector<std::uint8_t> partial(cap + 32, 0xC5);
                std::size_t n = 999;
                const auto e = ZipArchive::readPrefix(path.c_str(), item.entry,
                    cap ? partial.data() + 16 : nullptr, cap, n);
                guards &= std::all_of(partial.begin(), partial.begin() + 16, /** @brief Возвращает true только для неизменённого защитного байта 0xC5 перед данными частичного буфера. */ [](auto x) { return x == 0xC5; }) &&
                    std::all_of(partial.end() - 16, partial.end(), /** @brief Возвращает true только для неизменённого защитного байта 0xC5 после данных частичного буфера. */ [](auto x) { return x == 0xC5; });
                if (e == ZipError::Ok) {
                    guards &= n == std::min<std::size_t>(cap, item.entry.uncompressedSize);
                    if (fe == ZipError::Ok)
                        guards &= std::equal(partial.begin() + 16, partial.begin() + 16 + n, full.begin() + 16);
                } else guards &= n == 0;
            }
            if (!item.entry.uncompressedSize && fe == ZipError::Ok) {
                std::size_t n = 99;
                guards &= ZipArchive::read(path.c_str(), item.entry, nullptr, 0, n) == ZipError::Ok && n == 0;
            }
            bool mutations = true;
            for (unsigned field = 0; field < 7; ++field) {
                auto changed = item.entry;
                switch (field) {
                case 0: ++changed.localHeaderOffset; break;
                case 1: ++changed.compressedSize; break;
                case 2: ++changed.uncompressedSize; break;
                case 3: ++changed.crc32; break;
                case 4: ++changed.centralHeaderOffset; break;
                case 5: ++changed.method; break;
                case 6: changed.flags ^= 8; break;
                }
                std::size_t n = 99;
                mutations &= ZipArchive::readPrefix(path.c_str(), changed, prefix.data() + 16, 16, n) != ZipError::Ok && n == 0;
            }
            if (item.entry.uncompressedSize) {
                std::size_t n = 99;
                guards &= ZipArchive::read(path.c_str(), item.entry, full.data() + 16,
                    std::min<std::size_t>(capacity, item.entry.uncompressedSize - 1), n) == ZipError::OutputTooSmall && n == 0;
            }
            const auto base = std::string(argv[2]) + "/" + std::to_string(caseIndex) + "-" + std::to_string(i);
            if (pe == ZipError::Ok) save(base + ".prefix", prefix.data() + 16, prefixCount);
            if (fe == ZipError::Ok) save(base + ".full", full.data() + 16, fullCount);
            std::printf("ENTRY|%u|%zu|%u|%u|%u|%u|%u|%zu|%u|%zu|%d|%d|", caseIndex, i,
                unsigned(item.entry.method), unsigned(item.entry.flags), item.entry.crc32, item.entry.uncompressedSize,
                unsigned(pe), prefixCount, unsigned(fe), fullCount, int(mutations), int(guards));
            for (unsigned char c : item.name) std::printf("%02x", unsigned(c));
            std::putchar('\n');
        }
        ++caseIndex;
    }
    return list.eof() ? 0 : 3;
}
