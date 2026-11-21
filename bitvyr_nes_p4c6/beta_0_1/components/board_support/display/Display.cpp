#include "../BoardInternal.h"
#include "GameControls.h"
#include "GameBarsDirty.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace nes::platform {
namespace {
constexpr const char* tag = "nes_display";
constexpr std::size_t pixels = 480 * 800;
/**
 * @brief Одна собственная стартовая команда ST7701: регистр, полезная длина, задержка и до 16 байтов параметров.
 */
struct Command {
    std::uint8_t address;
    std::uint8_t length;
    std::uint16_t delayMs;
    std::uint8_t data[16];
};

// Регистры конкретной панели: гамма, драйвер затворов и последовательность питания;
// сверены с конфигурацией Waveshare ST7701 480×800. Чужой BSP не подключается.
constexpr Command panelSettings[]{
    {0x36,1,0,{0x00}}, {0x3A,1,0,{0x55}},
    {0xFF,5,0,{0x77,0x01,0x00,0x00,0x13}}, {0xEF,1,0,{0x08}},
    {0xFF,5,0,{0x77,0x01,0x00,0x00,0x10}},
    {0xC0,2,0,{0x63,0x00}}, {0xC1,2,0,{0x0D,0x02}},
    {0xC2,2,0,{0x17,0x08}}, {0xCC,1,0,{0x10}},
    {0xB0,16,0,{0x40,0xC9,0x94,0x0E,0x10,0x05,0x0B,0x09,0x08,0x26,0x04,0x52,0x10,0x69,0x6B,0x69}},
    {0xB1,16,0,{0x40,0xD2,0x98,0x0C,0x92,0x07,0x09,0x08,0x07,0x25,0x02,0x0E,0x0C,0x6E,0x78,0x55}},
    {0xFF,5,0,{0x77,0x01,0x00,0x00,0x11}},
    {0xB0,1,0,{0x5D}}, {0xB1,1,0,{0x4E}}, {0xB2,1,0,{0x87}},
    {0xB3,1,0,{0x80}}, {0xB5,1,0,{0x4E}}, {0xB7,1,0,{0x85}},
    {0xB8,1,0,{0x21}}, {0xB9,2,0,{0x10,0x1F}}, {0xBB,1,0,{0x03}},
    {0xBC,1,0,{0x00}}, {0xC1,1,0,{0x78}}, {0xC2,1,0,{0x78}},
    {0xD0,1,0,{0x88}}, {0xE0,3,0,{0x00,0x3A,0x02}},
    {0xE1,11,0,{0x04,0xA0,0x00,0xA0,0x05,0xA0,0x00,0xA0,0x00,0x40,0x40}},
    {0xE2,13,0,{0x30,0x00,0x40,0x40,0x32,0xA0,0x00,0xA0,0x00,0xA0,0x00,0xA0,0x00}},
    {0xE3,4,0,{0x00,0x00,0x33,0x33}}, {0xE4,2,0,{0x44,0x44}},
    {0xE5,16,0,{0x09,0x2E,0xA0,0xA0,0x0B,0x30,0xA0,0xA0,0x05,0x2A,0xA0,0xA0,0x07,0x2C,0xA0,0xA0}},
    {0xE6,4,0,{0x00,0x00,0x33,0x33}}, {0xE7,2,0,{0x44,0x44}},
    {0xE8,16,0,{0x08,0x2D,0xA0,0xA0,0x0A,0x2F,0xA0,0xA0,0x04,0x29,0xA0,0xA0,0x06,0x2B,0xA0,0xA0}},
    {0xEB,7,0,{0x00,0x00,0x4E,0x4E,0x00,0x00,0x00}}, {0xEC,2,0,{0x08,0x01}},
    {0xED,16,0,{0xB0,0x2B,0x98,0xA4,0x56,0x7F,0xFF,0xFF,0xFF,0xFF,0xF7,0x65,0x4A,0x89,0xB2,0x0B}},
    {0xEF,6,0,{0x08,0x08,0x08,0x45,0x3F,0x54}},
    {0xFF,5,0,{0x77,0x01,0x00,0x00,0x00}},
    {0x11,0,120,{}}, {0x29,0,20,{}}
};


/**
 * @brief Отмечает реальное завершение кадрового буфера LCD из ISR.
 * @details Отдаёт существующий frameDone; BoardState и семафор должны оставаться живы до остановки драйвера.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return true, если пробуждённой задаче требуется переключение контекста.
 */
bool IRAM_ATTR onFrameComplete(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void* context) {
    auto& state = *static_cast<BoardState*>(context);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(state.frameDone, &wake);
    return wake == pdTRUE;
}

/**
 * @brief Отмечает окончание чтения/записи игровой PPA-транзакции из ISR.
 * @details Без контекста меню ничего не делает; иначе фиксирует необязательную отметку и отдаёт gamePresentDone. Сам не вращает индексы LCD.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return Признак необходимости пробуждения задачи из ISR.
 */
bool IRAM_ATTR onGamePresentComplete(ppa_client_handle_t, ppa_event_data_t*, void* context) {
    // Поворот меню блокирующий и не использует контекст игровой транзакции.
    if (!context) return false;
    auto& state = *static_cast<BoardState*>(context);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(state.gamePresentDone, &wake);
    return wake == pdTRUE;
}
}

/**
 * @brief Создаёт DSI/DPI/ST7701, PPA и кадровые ресурсы платы.
 * @details Настраивает фиксированные пины/тайминги, два либо три native-буфера; только при нехватке третьего повторяет конструктор с двумя. Дескрипторы и кадры принадлежат BoardState, ошибки возвращаются без вымышленной готовности.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо ошибка GPIO/LDO/LCD/PPA/выделения памяти.
 */
esp_err_t initializeDisplay(BoardState& state) noexcept {
    gpio_config_t pins{};
    pins.pin_bit_mask = (1ULL << GPIO_NUM_26) | (1ULL << GPIO_NUM_27);
    pins.mode = GPIO_MODE_OUTPUT;
    auto result = gpio_config(&pins);
    if (result != ESP_OK) return result;
    gpio_set_level(GPIO_NUM_26, 1); // Подсветка этой платы активна низким уровнем.
    gpio_set_level(GPIO_NUM_27, 0);

    esp_ldo_channel_config_t power{};
    power.chan_id = 3;
    power.voltage_mv = 2500;
    result = esp_ldo_acquire_channel(&power, &state.mipiPower);
    if (result != ESP_OK) return result;
    esp_lcd_dsi_bus_config_t dsi{};
    dsi.bus_id = 0;
    dsi.num_data_lanes = 2;
    // Ноль выбирает опорную частоту PHY PLL в SDK с учётом ревизии чипа.
    dsi.lane_bit_rate_mbps = 500;
    result = esp_lcd_new_dsi_bus(&dsi, &state.dsi);
    if (result != ESP_OK) return result;
    esp_lcd_dbi_io_config_t commands{};
    commands.virtual_channel = 0;
    commands.lcd_cmd_bits = 8;
    commands.lcd_param_bits = 8;
    result = esp_lcd_new_panel_io_dbi(state.dsi, &commands, &state.panelIo);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_27, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (const auto& command : panelSettings) {
        result = esp_lcd_panel_io_tx_param(state.panelIo, command.address, command.data, command.length);
        if (result != ESP_OK) return result;
        if (command.delayMs) vTaskDelay(pdMS_TO_TICKS(command.delayMs));
    }

    esp_lcd_dpi_panel_config_t video{};
    video.virtual_channel = 0;
    video.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    video.dpi_clock_freq_mhz = 30;
    video.in_color_format = LCD_COLOR_FMT_RGB565;
    video.out_color_format = LCD_COLOR_FMT_RGB565;
    video.num_fbs = 3;
    video.video_timing.h_size = 480;
    video.video_timing.v_size = 800;
    video.video_timing.hsync_back_porch = 42;
    video.video_timing.hsync_pulse_width = 12;
    video.video_timing.hsync_front_porch = 42;
    video.video_timing.vsync_back_porch = 2;
    video.video_timing.vsync_pulse_width = 8;
    video.video_timing.vsync_front_porch = 56;
    result = esp_lcd_new_panel_dpi(state.dsi, &video, &state.panel);
    if(result == ESP_ERR_NO_MEM) {
        // Неуспешный конструктор SDK6.1 освобождает частичные FB/DMA/IRQ-ресурсы.
        // Повторяем только отказ памяти, сохраняя все тайминги и пины.
        ESP_LOGW(tag,"Third LCD framebuffer unavailable; falling back to two");
        video.num_fbs=2;state.panel=nullptr;
        result=esp_lcd_new_panel_dpi(state.dsi,&video,&state.panel);
    }
    if (result != ESP_OK) return result;
    state.nativeBufferCount=video.num_fbs;
    if(state.nativeBufferCount==3) {
        result=esp_lcd_dpi_panel_get_frame_buffer(state.panel,3,
            reinterpret_cast<void**>(&state.nativeFrames[0]),reinterpret_cast<void**>(&state.nativeFrames[1]),reinterpret_cast<void**>(&state.nativeFrames[2]));
    } else
    result = esp_lcd_dpi_panel_get_frame_buffer(state.panel, 2,
                reinterpret_cast<void**>(&state.nativeFrames[0]), reinterpret_cast<void**>(&state.nativeFrames[1]));
    if (result != ESP_OK) return result;
    state.logicalFrame = static_cast<std::uint16_t*>(heap_caps_aligned_calloc(128, pixels, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!state.logicalFrame) return ESP_ERR_NO_MEM;
    // Холодные локальные APU Tables размещены в PSRAM. Освобождённая HP RAM
    // держит RGB, который C0 записывает каждый кадр; PPA sourceDone/lease прежние.
    // При OOM возвращается явный отказ, скрытого возврата к PSRAM нет.
    state.gameFrame = static_cast<std::uint16_t*>(heap_caps_aligned_calloc(128, 256 * 240, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!state.gameFrame) return ESP_ERR_NO_MEM;
    state.gameControls = static_cast<std::uint16_t*>(heap_caps_aligned_calloc(128,160*480,2,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!state.gameControls) return ESP_ERR_NO_MEM;
    state.frameDone = xSemaphoreCreateBinary();
    if (!state.frameDone) return ESP_ERR_NO_MEM;
    state.gamePresentDone = xSemaphoreCreateBinaryStatic(&state.gamePresentDoneStorage);
    if (!state.gamePresentDone) return ESP_ERR_NO_MEM;
    state.displayCore = xPortGetCoreID();
    esp_lcd_dpi_panel_event_callbacks_t callbacks{};
    callbacks.on_frame_buf_complete = onFrameComplete;
    result = esp_lcd_dpi_panel_register_event_callbacks(state.panel, &callbacks, &state);
    if (result != ESP_OK) return result;

    ppa_client_config_t rotation{};
    rotation.oper_type = PPA_OPERATION_SRM;
    rotation.max_pending_trans_num = 1;
    result = ppa_register_client(&rotation, &state.rotation);
    if (result != ESP_OK) return result;
    ppa_event_callbacks_t rotationCallbacks{};
    rotationCallbacks.on_trans_done = onGamePresentComplete;
    result = ppa_client_register_event_callbacks(state.rotation, &rotationCallbacks);
    if (result != ESP_OK) return result;
    result = esp_lcd_panel_init(state.panel);
    if (result != ESP_OK) return result;
    gpio_set_level(GPIO_NUM_26, 0);
    ESP_LOGI(tag,"ST7701 480x800 RGB565 DPI30MHz DSI2x500Mbps; %u buffers; PPA landscape; IRQ core%u",state.nativeBufferCount,state.displayCore);
    return ESP_OK;
}

/**
 * @brief Готовит параметры масштабирования и безопасный целевой буфер PPA.
 * @details Требует ядро-владельца дисплея; в двухбуферном режиме ждёт LCD до записи. В трёхбуферном использует отдельный индекс; обновляет устаревшие панели управления только безопасно.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param game Выбор игрового источника вместо кадра меню.
 * @param operation Выходная конфигурация PPA; остаётся во владении вызывающего кода.
 * @return true при готовых параметрах, false при неверном ядре или тайм-ауте.
 */
static bool prepareFrame(BoardState& state, bool game, ppa_srm_oper_config_t& operation,const std::uint16_t*source=nullptr) noexcept {
    // Вывод остаётся на ядре-владельце IRQ. Ограждаем предыдущую подачу
    // до обращения к старому переднему буферу; NES может работать одновременно с развёрткой.
    if (xPortGetCoreID() != state.displayCore) {
        ESP_LOGE(tag, "present() must run on initialization core%u", state.displayCore);
        return false;
    }
    // В кольце из трёх цель отличается от развёртываемого и запрошенного FB.
    // Для проверенного двухбуферного отката сохраняем раннее ограждение.
    if(state.nativeBufferCount!=3) {
    if (state.framePending) {
        if (xSemaphoreTake(state.frameDone, pdMS_TO_TICKS(80)) != pdTRUE) {
            ESP_LOGE(tag, "LCD frame completion timeout");
            return false;
        }
        state.framePending = false;
    }
    }
    auto* output = state.nativeFrames[state.drawIndex];
    if (game && !state.gameBarsCleared[state.drawIndex]) {
        constexpr std::size_t barPixels = 480 * 80;
        // Как в меню, используем поворот PPA270°: логические левые/правые панели
        // попадают в физические верхнее/нижнее поля вокруг игрового изображения.
        gameBarsDirtyState.copy(&state,state.drawIndex,state.gameControls,output);
        esp_cache_msync(output, barPixels * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(output + 480 * 720, barPixels * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        state.gameBarsCleared[state.drawIndex] = true;
    } else if (!game) {
        gameBarsDirtyState.bind(&state);gameBarsDirtyState.unknown(state.drawIndex);
        state.gameBarsCleared[state.drawIndex] = false;
    }
    operation.in.buffer = game ? (source?source:state.gameFrame) : state.logicalFrame;
    operation.in.pic_w = game ? 256 : 800;
    operation.in.pic_h = game ? 240 : 480;
    operation.in.block_w = operation.in.pic_w;
    operation.in.block_h = operation.in.pic_h;
    operation.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    operation.out.buffer = output;
    operation.out.buffer_size = pixels * 2;
    operation.out.pic_w = 480;
    operation.out.pic_h = 800;
    operation.out.block_offset_y = game ? 80 : 0;
    operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    // SDK6.1 масштабирует по осям источника, затем переставляет размеры при
    // повороте 90/270°:256×2,5=640 и 240×2=480, результат native480×640.
    operation.scale_x = game ? 2.5F : 1.0F;
    operation.scale_y = game ? 2.0F : 1.0F;
    return true;
}

/**
 * @brief Подаёт готовый native-буфер LCD после предыдущего завершения.
 * @details Сохраняет отдельность PPA/LCD-ограждений; индекс рисования вращается только после успешного запроса драйверу. Успех не является доказательством физической развёртки/частоты.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param index Проверенный индекс готового физического буфера LCD.
 * @return true при принятом запросе, false при ошибке/тайм-ауте.
 */
static bool submitDisplay(BoardState& state, unsigned index) noexcept {
    if(state.nativeBufferCount==3) {
        // Одно реальное завершение между запросами LCD не даёт единственному
        // запрошенному индексу SDK вытеснить ещё не показанный кадр.
        if(state.framePending) {
            if(xSemaphoreTake(state.frameDone,pdMS_TO_TICKS(80))!=pdTRUE) {
                ESP_LOGE(tag,"LCD frame completion timeout before submit");
                return false;
            }
            state.framePending=false;
        }
    }
    auto* output = state.nativeFrames[index];
    if (esp_lcd_panel_draw_bitmap(state.panel, 0, 0, 480, 800, output) != ESP_OK) return false;
    // Завершения LCD и PPA являются разными ограждениями владения.
    while (xSemaphoreTake(state.frameDone, 0) == pdTRUE) {}
    state.framePending = true;
    state.drawIndex=state.nativeBufferCount==3?(index+1U)%3U:(index^1U);
    return true;
}

/**
 * @brief Запускает единственную неблокирующую игровую PPA-транзакцию.
 * @details До подачи публикует Main-owned pending/index, поскольку ISR может выполниться раньше возврата. Занятый источник не перехватывает; ошибка подачи сохраняет фактические правила SDK очистки.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return true при принятии транзакции, иначе false.
 */
bool beginGameDisplay(BoardState& state,const GameSourceTicket*ticket) noexcept {
    if (state.gamePresentPending||state.borrowedGameSource.valid()) return false; // Reader/native lease ещё жив.
    if(ticket&&(!ticket->valid()||!state.registeredGameSource||
       ticket->pixels!=(ticket->slot==0?state.registeredGameSource:state.gameFrame)))return false;
    ppa_srm_oper_config_t operation{};
    if (!prepareFrame(state, true, operation,ticket?ticket->pixels:nullptr)) return false;
    while (xSemaphoreTake(state.gamePresentDone, 0) == pdTRUE) {}
    // Публикуем состояние Main ДО подачи: ISR может завершиться раньше возврата.
    state.gamePresentIndex = state.drawIndex;
    state.borrowedGameSource=ticket?*ticket:GameSourceTicket{};
    state.gamePresentPending = true;
    state.gamePresentGpuReady=false;
    operation.mode = PPA_TRANS_MODE_NON_BLOCKING;
    operation.user_data = &state;
    const auto rotated = ppa_do_scale_rotate_mirror(state.rotation, &operation);
    if (rotated != ESP_OK) {
        // При ошибке подачи SDK6.1 удаляет/освобождает транзакцию перед возвратом;
        // ни один DMA после этой ошибки не владеет буфером (ppa_core.c/ppa_srm.c).
        state.gamePresentPending = false;
        state.borrowedGameSource={};
        ESP_LOGE(tag, "PPA rotation submit: %s", esp_err_to_name(rotated));
        return false;
    }
    return true;
}

/**
 * @brief Завершает игровой PPA и последующую подачу native-кадра LCD.
 * @details Использует реальный семафор; при трёх буферах запоминает готовность PPA и сохраняет output/index при позднем LCD. Тайм-аут не разрешает повторное использование ещё занятого ресурса.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return true после необходимых завершений либо отсутствия pending, иначе false.
 */
bool finishGameDisplay(BoardState& state, unsigned timeoutMs) noexcept {
    if (xPortGetCoreID() != state.displayCore) return false;
    if (!state.gamePresentPending) return true;
    if(state.nativeBufferCount==3) {
        if(!state.gamePresentGpuReady) {
            if(xSemaphoreTake(state.gamePresentDone,pdMS_TO_TICKS(timeoutMs))!=pdTRUE) {
                ESP_LOGE(tag,"PPA completion timeout; source/output remain owned");
                return false;
            }
            state.gamePresentGpuReady=true;
        }
        // Поздняя граница LCD или отказ draw сохраняет готовый output/источник.
        // Повторная попытка подаёт этот же кадр, не потребляя PPA-токен дважды.
        if(!submitDisplay(state,state.gamePresentIndex))return false;
        state.gamePresentGpuReady=false;
        state.gamePresentPending=false;
        return true;
    }
    if (xSemaphoreTake(state.gamePresentDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        ESP_LOGE(tag, "PPA completion timeout; source/output remain owned");
        return false;
    }
    // Только реальное завершение, включая позднее, возвращает владение GPU.
    const auto index = state.gamePresentIndex;
    state.gamePresentPending = false;
    return submitDisplay(state, index);
}

/**
 * @brief Подаёт готовый прежний native-кадр во время работы CPU1 без ожидания.
 * @details Не использует finishGameDisplay(0): его LCD-этап блокирующий. Здесь
 * прежний LCD-токен получен до submitDisplay, поэтому его wait80 не достижим.
 * Post-draw drain сохранён: callback до cur_fb_index store не доказывает принятие
 * нового индекса. Источник возвращается после PPA completion и принятой подачи,
 * отдельно от следующей физической развёртки. Fault не отменяет borrowed loan.
 */
GamePresentPollResult pollGameDisplay(BoardState&state)noexcept{
    if(xPortGetCoreID()!=state.displayCore)return GamePresentPollResult::Fault;
    if(state.nativeBufferCount!=3||!state.gamePresentPending||!state.borrowedGameSource.valid())
        return GamePresentPollResult::Fault;
    return detail::pollGamePresentation(state.gamePresentGpuReady,state.framePending,state.gamePresentPending,
        [&]()noexcept{return xSemaphoreTake(state.gamePresentDone,0)==pdTRUE;},
        [&]()noexcept{return xSemaphoreTake(state.frameDone,0)==pdTRUE;},
        [&]()noexcept{return submitDisplay(state,state.gamePresentIndex);});
}

/**
 * @brief Блокирующе поворачивает и подаёт кадр меню.
 * @details Сначала завершает старый игровой DMA, затем пишет отдельный native-буфер; отказ ограждения оставляет старый ресурс заимствованным.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return true при успехе PPA и LCD, иначе false.
 */
bool presentDisplay(BoardState& state) noexcept {
    // Меню может следовать за ошибкой аудио/вывода. Ограждаем старый игровой DMA
    // до поворота меню/записи native-буфера; тайм-аут сохраняет заимствование.
    if (!finishGameDisplay(state, 80)) return false;
    ppa_srm_oper_config_t operation{};
    if (!prepareFrame(state, false, operation)) return false;
    operation.mode = PPA_TRANS_MODE_BLOCKING;
    const auto rotated = ppa_do_scale_rotate_mirror(state.rotation, &operation);
    if (rotated != ESP_OK) {
        ESP_LOGE(tag, "PPA rotation: %s", esp_err_to_name(rotated));
        return false;
    }
    return submitDisplay(state, state.drawIndex);
}

/**
 * @brief Выполняет полный синхронный игровой цикл вывода.
 * @details Последовательность finish/begin/finish поддерживает позднее завершение предыдущей операции и не пропускает кадры автоматически.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return true, если все три этапа успешны.
 */
bool presentGameDisplay(BoardState& state) noexcept {
    // Сохраняем обычный синхронный API: сначала завершаем позднюю старую операцию,
    // затем начинаем новый кадр. Обычное исполнение по-прежнему подаёт каждый кадр.
    return finishGameDisplay(state, 80) && beginGameDisplay(state) && finishGameDisplay(state, 80);
}

/**
 * @brief Получает свободный RGB-источник по настоящему PPA-токену.
 * @details Поддерживает только три native-буфера; не подаёт LCD и не меняет output/index/pending. Повторное получение использует latch, не забирая токен дважды.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param source Выходной указатель: nullptr до успеха, при Ready разрешена запись только RGB-источника.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return Ready с указателем либо Unsupported/Timeout/InvalidState с nullptr.
 */
GameSourceAcquireResult acquireCompletedGameSourceDisplay(BoardState&state,std::uint16_t*&source,unsigned timeoutMs)noexcept{
    source=nullptr;
    if(xPortGetCoreID()!=state.displayCore||!state.gameFrame||state.registeredGameSource||state.borrowedGameSource.valid())return GameSourceAcquireResult::InvalidState;
    if(state.nativeBufferCount!=3)return GameSourceAcquireResult::Unsupported;
    if(state.gamePresentPending&&!state.gamePresentGpuReady){
        // Это тот же реальный PPA-семафор, что и в finishGameDisplay.
        // Не подаём LCD, не вращаем drawIndex и не снимаем старое заимствование output.
        if(xSemaphoreTake(state.gamePresentDone,pdMS_TO_TICKS(timeoutMs))!=pdTRUE)return GameSourceAcquireResult::Timeout;
        state.gamePresentGpuReady=true;
    }
    source=state.gameFrame;return GameSourceAcquireResult::Ready;
}

} // пространство имён nes::platform
