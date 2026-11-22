#include "../BoardInternal.h"

#include <algorithm>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace nes::platform {
namespace {
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "I2S diagnostics require lock-free ISR counters");

/**
 * @brief Учитывает завершённые I2S DMA-кадры из ISR.
 * @details Использует только атомарные 32-битные счётчики внутреннего BoardState; вне активного диагностического окна ничего не прибавляет.
 * @param event Событие SDK; указатель действителен только внутри callback.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return false: дополнительное пробуждение задачи не запрашивается.
 */
bool IRAM_ATTR onAudioSent(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    auto& state = *static_cast<BoardState*>(context);
    if (state.audioActive.load(std::memory_order_acquire))
        state.audioDmaFrames.fetch_add(event->size / 4, std::memory_order_relaxed);
    return false;
}

/**
 * @brief Учитывает переполнение очереди свободных DMA-блоков I2S.
 * @details После 40 мс льготы активного окна регистрирует целые блоки, не пополненные вовремя. Это ограниченная диагностика, не полный счётчик аудиопровалов.
 * @param event Событие SDK; указатель действителен только внутри callback.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return false: дополнительное пробуждение задачи не запрашивается.
 */
bool IRAM_ATTR onAudioRefillMissed(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    auto& state = *static_cast<BoardState*>(context);
    if (!state.audioActive.load(std::memory_order_acquire)) return false;
    const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    if (now - state.audioMonitorStartMs.load(std::memory_order_relaxed) < 40) return false;
    // Очередь свободных буферов SDK (dma_desc_num-1 записей) переполнилась:
    // завершённый буфер не пополнили до следующего использования DMA.
    // При auto_clear он содержит нули. Это обнаруживает пропуск срока целого блока,
    // но не даёт точного числа всех потерянных PCM-отсчётов.
    state.audioMissedBlocks.fetch_add(1, std::memory_order_relaxed);
    state.audioMissedFrames.fetch_add(event->size / 4, std::memory_order_relaxed);
    return false;
}

/**
 * @brief Записывает один регистр ES8311 через принадлежащее плате I2C-устройство.
 * @details Передаёт два байта с ограниченным ожиданием 40 мс; ресурс кодека не передаётся вызывающему коду.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param reg Адрес аппаратного регистра выбранного устройства.
 * @param value Записываемое или преобразуемое значение.
 * @return Код ESP-IDF от i2c_master_transmit.
 */
esp_err_t writeRegister(BoardState& state, std::uint8_t reg, std::uint8_t value) noexcept {
    const std::uint8_t bytes[]{reg, value};
    return i2c_master_transmit(state.codec, bytes, sizeof(bytes), 40);
}

/**
 * @brief Преобразует процент в ограниченный код аттенюации ES8311.
 * @details Кусочно-линейная целочисленная кривая сильно ослабляет малые положения; код не превышает 191(0 дБ). Не меняет регистры.
 * @param percent Желаемый уровень 0..100; значения выше 100 ограничиваются.
 * @return Код DAC0..191.
 */
std::uint8_t volumeCode(unsigned percent) noexcept {
    // ES8311: дБ=(code-191)/2. 0xBF соответствует 0 дБ; положительное усиление DAC запрещено.
    // Целочисленная кусочная кривая сильно ослабляет первые положения регулятора.
    /**
     * @brief Узел целочисленной кривой громкости: процент интерфейса и ограниченный код аттенюации DAC.
     */
    struct Point { unsigned percent, code; };
    constexpr Point curve[]{{1,1},{5,47},{10,71},{25,111},{50,143},{75,167},{100,191}};
    if (!percent) return 0;
    percent = std::min(percent, 100U);
    for (unsigned i = 1; i < sizeof(curve)/sizeof(curve[0]); ++i) {
        if (percent <= curve[i].percent) {
            const auto& a = curve[i-1]; const auto& b = curve[i];
            return static_cast<std::uint8_t>(a.code + (percent-a.percent)*(b.code-a.code)/(b.percent-a.percent));
        }
    }
    return 0xbf;
}
/**
 * @brief Обрабатывает ошибку аудиоуправления с запросом выключения PA.
 * @details GPIO53 не зависит от успеха I2C; дополнительно пробует приглушить DAC, обнуляет сохранённый уровень и отключает учёт.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param error Заимствованный код/описание завершения SDK.
 * @return Всегда false для передачи исходной ошибки вверх.
 */
bool failQuiet(BoardState& state, esp_err_t error) noexcept {
    // PA не зависит от I2C: ошибка команды кодека не должна оставлять громкий выход.
    static_cast<void>(gpio_set_level(GPIO_NUM_53, 0));
    if (state.codec) static_cast<void>(writeRegister(state, 0x31, 0x60));
    state.volume = 0;
    state.audioActive.store(false, std::memory_order_release);
    ESP_LOGW("nes_audio", "Audio control failed: %s; PA-off requested", esp_err_to_name(error));
    return false;
}
/**
 * @brief Последовательно изменяет DAC и PA при безопасном выключении усилителя.
 * @details Отключение PA предшествует I2C; включение допустимо только после успешного уровня и снятия mute при active&&percent. Ошибка проходит через failQuiet.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param percent Желаемый уровень 0..100; значения выше 100 ограничиваются.
 * @param active Запрошенная активность воспроизведения/диагностики.
 * @return true после всех обязательных операций, иначе false.
 */
bool applyVolume(BoardState& state, unsigned percent, bool active) noexcept {
    const auto off = gpio_set_level(GPIO_NUM_53, 0); // До любой I2C-операции, включая нулевую громкость.
    if (off != ESP_OK) return failQuiet(state, off);
    if (!state.codec) return failQuiet(state, ESP_ERR_INVALID_STATE);
    state.volume = 0;
    percent = std::min(percent, 100U);
    auto error = writeRegister(state, 0x31, 0x60);
    if (error != ESP_OK) return failQuiet(state, error);
    error = writeRegister(state, 0x32, volumeCode(percent));
    if (error != ESP_OK) return failQuiet(state, error);
    if (active && percent) {
        error = writeRegister(state, 0x31, 0x00);
        if (error != ESP_OK) return failQuiet(state, error);
        error = gpio_set_level(GPIO_NUM_53, 1); // Последний шаг после проверки уровня и снятия mute с DAC.
        if (error != ESP_OK) return failQuiet(state, error);
    }
    state.volume = percent;
    return true;
}
}

/**
 * @brief Создаёт ES8311/I2S0 с PCM48 кГц и выключенным усилителем.
 * @details BoardState удерживает дескрипторы на время прошивки; настраивает шесть DMA-блоков по 256 кадров, ISR и два 16-битных слота. Начальное состояние — DAC mute/volume0, пользовательский уровень задаётся позже приложением.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо первая ошибка GPIO/I2C/I2S/поиска кодека.
 */
esp_err_t initializeAudio(BoardState& state) noexcept {
    state.volume = 0;
    state.audioPlaybackRequested = false;
    state.audioActive.store(false, std::memory_order_release);
    auto result = gpio_set_level(GPIO_NUM_53, 0); // Заранее устанавливаем низкий уровень до включения выходного режима.
    if (result != ESP_OK) return result;
    gpio_config_t amp{};
    amp.pin_bit_mask = 1ULL << GPIO_NUM_53;
    amp.mode = GPIO_MODE_OUTPUT;
    result = gpio_config(&amp);
    if (result != ESP_OK) return result;
    result = gpio_set_level(GPIO_NUM_53, 0);
    if (result != ESP_OK) return result;
    if (!state.i2c) return ESP_ERR_INVALID_STATE;
    if (i2c_master_probe(state.i2c, 0x18, 40) != ESP_OK) return ESP_ERR_NOT_FOUND;
    i2c_device_config_t codec{};
    codec.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    codec.device_address = 0x18;
    codec.scl_speed_hz = 400000;
    result = i2c_master_bus_add_device(state.i2c, &codec, &state.codec);
    if (result != ESP_OK) return result;

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 256;
    channel.auto_clear_after_cb = true;
    result = i2s_new_channel(&channel, &state.audioTx, nullptr);
    if (result != ESP_OK) return result;
    i2s_std_config_t serial{};
    serial.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(Board::audioSampleRate);
    serial.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    // Копируем моноотсчёт в оба 16-битных слота; это однозначно задаёт формат
    // на проводах независимо от выбранного кодеком левого/правого слота.
    serial.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    serial.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    serial.gpio_cfg.mclk = GPIO_NUM_13;
    serial.gpio_cfg.bclk = GPIO_NUM_12;
    serial.gpio_cfg.ws = GPIO_NUM_10;
    serial.gpio_cfg.dout = GPIO_NUM_9;
    serial.gpio_cfg.din = GPIO_NUM_NC;
    result = i2s_channel_init_std_mode(state.audioTx, &serial);
    if (result != ESP_OK) return result;
    i2s_event_callbacks_t callbacks{};
    callbacks.on_sent = onAudioSent;
    callbacks.on_send_q_ovf = onAudioRefillMissed;
    result = i2s_channel_register_event_callback(state.audioTx, &callbacks, &state);
    if (result != ESP_OK) return result;
    result = i2s_channel_enable(state.audioTx);
    if (result != ESP_OK) return result;

    result = writeRegister(state, 0x00, 0x1F);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(20));
    // ES8311 ведомый, Philips I2S/16 бит, MCLK=12,288 МГц=256×48 кГц.
    // Делители: pre1/mult1/ADC1/DAC1/LRCK256; передискретизация OSR16.
    const std::uint8_t settings[][2]{
        {0x00,0x00}, {0x00,0x80}, {0x01,0x3F}, {0x02,0x00},
        {0x03,0x10}, {0x04,0x10}, {0x05,0x00}, {0x06,0x03},
        {0x07,0x00}, {0x08,0xFF}, {0x09,0x0C}, {0x0A,0x0C},
        {0x0D,0x01}, {0x0E,0x02}, {0x12,0x00}, {0x13,0x10},
        {0x1C,0x6A}, {0x37,0x08}, {0x34,0x00}, {0x31,0x60}, {0x32,0x00}
    };
    for (const auto& setting : settings) {
        result = writeRegister(state, setting[0], setting[1]);
        if (result != ESP_OK) return result;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGI("nes_audio", "ES8311 mono48k/16bit MCLK12.288MHz I2S0; PA53 off, DAC muted, volume=0");
    return ESP_OK;
}

/**
 * @brief Применяет уровень с учётом уже запрошенного воспроизведения.
 * @details Не меняет audioPlaybackRequested; использует безопасную последовательность applyVolume.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param percent Желаемый уровень 0..100; значения выше 100 ограничиваются.
 * @return true при успехе, иначе false.
 */
bool changeVolume(BoardState& state, unsigned percent) noexcept {
    return applyVolume(state, percent, state.audioPlaybackRequested);
}

/**
 * @brief Сохраняет запрос воспроизведения и применяет существующий уровень.
 * @details Активность сама не увеличивает громкость; PA включается лишь при ненулевом сохранённом уровне.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param active Запрошенная активность воспроизведения/диагностики.
 * @return true при успешном управлении кодеком/PA, иначе false.
 */
bool setAudioOutputActive(BoardState& state, bool active) noexcept {
    state.audioPlaybackRequested = active;
    return applyVolume(state, state.volume, active);
}

} // пространство имён nes::platform
