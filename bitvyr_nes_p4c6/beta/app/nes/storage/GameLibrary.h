/**
 * @file GameLibrary.h
 * @brief Индексирование дерева SD и загрузка одной ROM с проверкой поколения и изменения файла.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nes::app {

/** @brief Копия сведений выбранной ROM: пути, ZIP-участник, размер, mapper и generation/entry. После rescan старая идентичность недействительна. */
struct GameInfo {
    char name[112]{};
    char path[1024]{};
    char member[1024]{}; // Для обычного файла пусто; иначе path указывает ZIP.
    std::uint32_t size = 0, crc = 0;
    std::uint16_t mapper = 0;
    bool pal = false;
    bool archived = false;
    std::uint64_t generation = 0; // Непрозрачная идентичность выбора, присваиваемая game().
    std::uint32_t entry = 0;
};

/** @brief Main-владелец динамического индекса SD. Возвращает копии выбора, читает одну ROM в память вызывающего и не хранит библиотеку во Flash. */
class GameLibrary final {
public:
    /**
     * @brief Создаёт пустой индекс библиотеки; ROM и SD при конструировании не открываются. Копирование запрещено из-за владения индексом.
     */
    GameLibrary() noexcept = default;
    /**
     * @brief Освобождает принадлежащий библиотеке индекс и строки; активная ROM находится во владении приложения, а не индекса.
     */
    ~GameLibrary() noexcept;
    /**
     * @brief Создаёт пустой индекс библиотеки; ROM и SD при конструировании не открываются. Копирование запрещено из-за владения индексом.
     */
    GameLibrary(const GameLibrary&) = delete;
    /**
     * @brief Запрещает копирующее присваивание индекса: память строк и generation принадлежат единственному экземпляру.
     */
    GameLibrary& operator=(const GameLibrary&) = delete;
    // Пересканирование заменяет индекс; при отказе библиотека остаётся пустой.
    /**
     * @brief Строит новый индекс каталога и вложенных директорий, проверяя пути и метаданные ROM/ZIP. При ошибке старая выборка не остаётся действующей.
     * @param root Заимствованный путь корня библиотеки; содержимое не изменяется.
     */
    bool open(const char* root = "/sdcard/nes_rom") noexcept;
    /**
     * @brief Возвращает фактическое число индексированных игр текущего поколения.
     */
    std::size_t count() const noexcept;
    /**
     * @brief Копирует сведения выбранной записи и её generation/entry в GameInfo. Возвращает false для индекса вне текущего каталога.
     * @param index Нулевой индекс записи текущего поколения каталога.
     * @param info Идентичность выбранной ROM с generation/entry; при чтении должна принадлежать текущему каталогу.
     */
    bool game(std::size_t index, GameInfo& info) const noexcept;
    // Индексирование заголовка оставляет crc=0 до полного успешного чтения файла.
    /**
     * @brief Читает выбранную ROM целиком в память вызывающего, перепроверяя generation, размер и изменения файла. Возвращает CRC только после успешного полного чтения.
     * @param info Идентичность выбранной ROM с generation/entry; при чтении должна принадлежать текущему каталогу.
     * @param destination Буфер вызывающего для результата; функция не сохраняет владение после завершения.
     * @param capacity Вместимость выходной памяти в байтах.
     * @param crc Выходной CRC32 полностью успешно прочитанной ROM.
     */
    bool read(const GameInfo& info, std::uint8_t* destination,
              std::size_t capacity, std::uint32_t& crc) const noexcept;
    /**
     * @brief Возвращает заимствованную строку последней ошибки; она живёт до следующей операции с библиотекой.
     */
    const char* errorText() const noexcept { return error_; }
    /**
     * @brief Возвращает число пропущенных при сканировании записей; это не число несовместимых игровых сцен.
     */
    std::size_t skipped() const noexcept;
private:
    struct Index;
    Index* index_ = nullptr;
    std::uint64_t generation_ = 0;
    mutable char error_[192]{};
    /**
     * @brief Освобождает индекс и инвалидирует старые идентичности выбора; не форматирует и не удаляет файлы SD.
     */
    void clear() noexcept;
};
} // namespace nes::app
