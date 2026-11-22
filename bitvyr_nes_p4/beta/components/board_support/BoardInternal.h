#pragma once

#include <atomic>

#include "board/Board.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

namespace nes::platform {

/**
 * @brief Внутренний владелец дескрипторов периферии и долговечных буферов платы. Main владеет выводом; ISR используют ограниченные атомарные поля/семафоры, фоновая задача сенсора — только свой снимок.
 */
struct BoardState final {
    i2c_master_bus_handle_t i2c{nullptr};
    i2c_master_dev_handle_t touch{nullptr};
    i2c_master_dev_handle_t codec{nullptr};
    esp_ldo_channel_handle_t mipiPower{nullptr};
    esp_lcd_dsi_bus_handle_t dsi{nullptr};
    esp_lcd_panel_io_handle_t panelIo{nullptr};
    esp_lcd_panel_handle_t panel{nullptr};
    SemaphoreHandle_t frameDone{nullptr};
    StaticSemaphore_t gamePresentDoneStorage{};
    SemaphoreHandle_t gamePresentDone{nullptr};
    std::uint16_t* nativeFrames[3]{nullptr, nullptr, nullptr};
    std::uint16_t* logicalFrame{nullptr};
    std::uint16_t* gameFrame{nullptr};
    std::uint16_t* gameControls{nullptr};
    bool gameBarsCleared[3]{false, false, false};
    unsigned nativeBufferCount{2}; // Неизменно после успешной инициализации.
    bool gamePresentGpuReady{false}; // PPA завершён, но подача LCD ещё ожидается.
    unsigned drawIndex{1};
    bool framePending{false};
    bool gamePresentPending{false}; // Владелец CPU0; сброс только после настоящего завершения PPA.
    unsigned gamePresentIndex{0};
    unsigned displayCore{0};
    ppa_client_handle_t rotation{nullptr};
    i2s_chan_handle_t audioTx{nullptr};
    sd_pwr_ctrl_handle_t sdPower{nullptr};
    sdmmc_card_t* sdCard{nullptr};
    unsigned volume{0};
    bool audioPlaybackRequested{false}; // Поле Main; намерение воспроизведения отлично от слышимости и диагностической активности.
    TouchContacts lastTouches{};
    bool touchSampleValid{true};
    std::atomic<bool> audioActive{false};
    std::atomic<std::uint32_t> audioMonitorStartMs{0};
    std::atomic<std::uint32_t> audioAcceptedFrames{0};
    std::atomic<std::uint32_t> audioDmaFrames{0};
    std::atomic<std::uint32_t> audioMissedBlocks{0};
    std::atomic<std::uint32_t> audioMissedFrames{0};
    std::atomic<std::uint32_t> audioMaxWriteGapUs{0};
    std::atomic<std::uint32_t> audioMaxWriteWaitUs{0};
    std::int64_t lastAudioWriteUs{0};
};

/**
 * @brief Создаёт DSI/DPI/ST7701, PPA и кадровые ресурсы платы.
 * @details Настраивает фиксированные пины/тайминги, два либо три native-буфера; только при нехватке третьего повторяет конструктор с двумя. Дескрипторы и кадры принадлежат BoardState, ошибки возвращаются без вымышленной готовности.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо ошибка GPIO/LDO/LCD/PPA/выделения памяти.
 */
esp_err_t initializeDisplay(BoardState& state) noexcept;
/**
 * @brief Находит GT911 по одному из штатных I2C-адресов.
 * @details Не переключает RST/INT; до пяти ограниченных попыток с паузой. Неуспешно прочитанное устройство удаляется перед следующей попыткой.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо последняя/неустранимая ошибка I2C.
 */
esp_err_t initializeTouch(BoardState& state) noexcept;
/**
 * @brief Создаёт ES8311/I2S0 с PCM48 кГц и выключенным усилителем.
 * @details BoardState удерживает дескрипторы на время прошивки; настраивает шесть DMA-блоков по 256 кадров, ISR и два 16-битных слота. Начальное состояние — DAC mute/volume0, пользовательский уровень задаётся позже приложением.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо первая ошибка GPIO/I2C/I2S/поиска кодека.
 */
esp_err_t initializeAudio(BoardState& state) noexcept;
/**
 * @brief Монтирует FATFS microSD через SDMMC slot0 без форматирования.
 * @details Использует штатный IO-MUX платы, LDO4 и 4 линии; slot1/C6 не меняет. При ESP_FAIL разрешена только read-only диагностика; частотных/пиновых экспериментов нет.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо исходная ошибка SDK; неосвобождённый LDO остаётся отмеченным в state.
 */
esp_err_t mountStorage(BoardState& state) noexcept;
/**
 * @brief Блокирующе поворачивает и подаёт кадр меню.
 * @details Сначала завершает старый игровой DMA, затем пишет отдельный native-буфер; отказ ограждения оставляет старый ресурс заимствованным.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return true при успехе PPA и LCD, иначе false.
 */
bool presentDisplay(BoardState& state) noexcept;
/**
 * @brief Выполняет полный синхронный игровой цикл вывода.
 * @details Последовательность finish/begin/finish поддерживает позднее завершение предыдущей операции и не пропускает кадры автоматически.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return true, если все три этапа успешны.
 */
bool presentGameDisplay(BoardState& state) noexcept;
/**
 * @brief Запускает единственную неблокирующую игровую PPA-транзакцию.
 * @details До подачи публикует Main-owned pending/index, поскольку ISR может выполниться раньше возврата. Занятый источник не перехватывает; ошибка подачи сохраняет фактические правила SDK очистки.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return true при принятии транзакции, иначе false.
 */
bool beginGameDisplay(BoardState& state) noexcept;
/**
 * @brief Завершает игровой PPA и последующую подачу native-кадра LCD.
 * @details Использует реальный семафор; при трёх буферах запоминает готовность PPA и сохраняет output/index при позднем LCD. Тайм-аут не разрешает повторное использование ещё занятого ресурса.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return true после необходимых завершений либо отсутствия pending, иначе false.
 */
bool finishGameDisplay(BoardState& state, unsigned timeoutMs) noexcept;
/**
 * @brief Выбирает первый контакт из полного проверенного снимка.
 * @details Не обещает наличие нажатия при успешном чтении; при отсутствии контактов возвращает обнуленную точку.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param point Выходная точка первого контакта либо обнулённое отсутствие нажатия.
 * @return Успех чтения снимка, а не признак pressed.
 */
bool readTouch(BoardState& state, TouchPoint& point) noexcept;
/**
 * @brief Получает проверенный снимок контактов в координатах 800×480.
 * @details Синхронный вариант подтверждает статус GT911 и проверяет координаты/дубликаты ID. Фоновый вариант копирует публикацию под коротким lock и отвергает снимок старше 40 мс.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param contacts Выходной ограниченный снимок контактов; при ошибке сбрасывается.
 * @return true для достоверного снимка, в том числе пустого; false при ошибке.
 */
bool readTouchContacts(BoardState& state, TouchContacts& contacts) noexcept;
/**
 * @brief Применяет уровень с учётом уже запрошенного воспроизведения.
 * @details Не меняет audioPlaybackRequested; использует безопасную последовательность applyVolume.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param percent Желаемый уровень 0..100; значения выше 100 ограничиваются.
 * @return true при успехе, иначе false.
 */
bool changeVolume(BoardState& state, unsigned percent) noexcept;
/**
 * @brief Сохраняет запрос воспроизведения и применяет существующий уровень.
 * @details Активность сама не увеличивает громкость; PA включается лишь при ненулевом сохранённом уровне.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param active Запрошенная активность воспроизведения/диагностики.
 * @return true при успешном управлении кодеком/PA, иначе false.
 */
bool setAudioOutputActive(BoardState& state, bool active) noexcept;

/**
 * @brief Получает свободный RGB-источник по настоящему PPA-токену.
 * @details Поддерживает только три native-буфера; не подаёт LCD и не меняет output/index/pending. Повторное получение использует latch, не забирая токен дважды.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param source Выходной указатель: nullptr до успеха, при Ready разрешена запись только RGB-источника.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return Ready с указателем либо Unsupported/Timeout/InvalidState с nullptr.
 */
GameSourceAcquireResult acquireCompletedGameSourceDisplay(BoardState&state,std::uint16_t*&source,unsigned timeoutMs)noexcept;

} // пространство имён nes::platform
