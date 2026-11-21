#pragma once

#include <cstdint>

namespace nes::platform {

/**
 * @brief Долговечное внутреннее состояние единственного BLE-клиента. Callback host владеют discovery/парсером; приложению публикуются атомарные кнопки, готовность и фронт меню.
 */
struct BluetoothState;

// Собственный Xbox BLE HID-over-GATT клиент. ESP-IDF/NimBLE/ESP-Hosted
// предоставляют транспорт/стек; разбор HID и политика кнопок — наши.
/**
 * @brief Собственный асинхронный Xbox BLE HID-over-GATT клиент. NimBLE/ESP-Hosted предоставляют транспорт; разбор и политика кнопок принадлежат проекту. Состояние долговечно, копирование запрещено.
 */
class BluetoothInput final {
public:
    /**
     * @brief Создаёт пустой объект без аппаратного ввода-вывода.
     * @details Ресурсы ещё не выделены; запуск выполняется явным initialize().
     */
    BluetoothInput() noexcept = default;
    /**
     * @brief Запрещает копирование владельца аппаратных ресурсов.
     * @details Конструктор удалён; один объект сохраняет стабильное владение ресурсами и callback-контекстом.
     */
    BluetoothInput(const BluetoothInput&) = delete;
    /**
     * @brief Запрещает присваивание владельца аппаратных ресурсов.
     * @details Операция удалена: дескрипторы и долговечные буферы нельзя неявно передавать другому объекту.
     * @return Операция недоступна при компиляции.
     */
    BluetoothInput& operator=(const BluetoothInput&) = delete;

    // Начинает асинхронную настройку транспорта/сопряжения. true означает запуск задачи,
    // а не уже установленное соединение с контроллером.
    /**
     * @brief Однократно создаёт состояние и задачу настройки BLE-клиента.
     * @details Состояние располагается во внутренней RAM на время прошивки. Успех означает запуск задачи, а не уже установленное соединение.
     * @return true при существующем/запущенном клиенте, false при нехватке ресурсов.
     */
    [[nodiscard]] bool initialize() noexcept;
    // Порядок NES: A=1 B=2 Select=4 Start=8 Up=16 Down=32 Left=64 Right=128.
    /**
     * @brief Получает опубликованную маску игровых кнопок NES.
     * @details Атомарное чтение не обращается к BLE/NVS; разрыв связи предварительно отпускает кнопки.
     * @return Биты A1/B2/Select4/Start8/Up16/Down32/Left64/Right128.
     */
    [[nodiscard]] std::uint8_t buttons() const noexcept;
    /**
     * @brief Проверяет готовность хотя бы одной игровой HID-подписки.
     * @details Радиосоединение до завершения discovery/подписок ещё не считается готовым геймпадом.
     * @return true для готового игрового ввода, иначе false.
     */
    [[nodiscard]] bool connected() const noexcept;
    // Потребляет один фронт Guide/Home либо сочетания Select+Start.
    /**
     * @brief Потребляет один опубликованный фронт Guide/Home или Select+Start.
     * @details Атомарный exchange очищает событие; удержание не создаёт повторных фронтов само по себе.
     * @return true при новом запросе меню.
     */
    [[nodiscard]] bool menuRequested() noexcept;
    /**
     * @brief Возвращает короткое имя текущей фазы BLE-клиента.
     * @details Статическая строка не передаёт владение; это диагностический статус, не доказательство радиокачества.
     * @return Статическая строка состояния либо BLUETOOTH OFF до initialize.
     */
    [[nodiscard]] const char* statusText() const noexcept;

private:
    BluetoothState* state_{nullptr};
};

} // пространство имён nes::platform
