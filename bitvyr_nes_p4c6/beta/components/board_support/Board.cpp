#include "BoardInternal.h"
#include "display/GameBarsDirty.h"

#include <algorithm>
#include <new>
#include "esp_heap_caps.h"
#include "esp_efuse.h"
#include "esp_memory_utils.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

namespace nes::platform {
// Main-only sidecar существует в обоих режимах и не принадлежит ISR/Worker.
GameBarsDirtyState gameBarsDirtyState{};
namespace {
constexpr const char* tag = "nes_board";
/**
 * @brief Записывает результат запуска одного узла в журнал платы.
 * @details ESP_OK означает готовность; ошибка журналируется без остановки остальных узлов.
 * @param device Статическое или временное имя узла для журнала, например LCD или GT911.
 * @param result Код результата проверяемой операции.
 * @return true при ESP_OK, иначе false.
 */
bool report(const char* device, esp_err_t result) noexcept {
    if (result == ESP_OK) {
        ESP_LOGI(tag, "%s: ready", device);
        return true;
    }
    ESP_LOGW(tag, "%s: %s (0x%x)", device, esp_err_to_name(result), static_cast<unsigned>(result));
    return false;
}
/**
 * @brief Обнуляет отдельное окно аудиодиагностики.
 * @details Вызывается владельцем Main при запуске слышимого вывода; устанавливает начало льготных 40 мс и не меняет PCM/DMA.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void resetAudioWindow(BoardState& state) noexcept {
    state.audioAcceptedFrames.store(0, std::memory_order_relaxed);
    state.audioDmaFrames.store(0, std::memory_order_relaxed);
    state.audioMissedBlocks.store(0, std::memory_order_relaxed);
    state.audioMissedFrames.store(0, std::memory_order_relaxed);
    state.audioMaxWriteGapUs.store(0, std::memory_order_relaxed);
    state.audioMaxWriteWaitUs.store(0, std::memory_order_relaxed);
    state.lastAudioWriteUs = 0;
    state.audioMonitorStartMs.store(static_cast<std::uint32_t>(esp_timer_get_time() / 1000), std::memory_order_relaxed);
}
}

/**
 * @brief Однократно создаёт состояние платы и запускает периферию.
 * @details BoardState размещается во внутренней RAM на время работы прошивки. Отсутствие SD или другого узла отражается отдельно в BoardStatus; повторный вызов возвращает сохранённый результат.
 * @return Ссылка на собственный BoardStatus; вызывающий код не освобождает её.
 */
const BoardStatus& Board::initialize() noexcept {
    if (state_) return status_;
    auto* memory = heap_caps_calloc(1, sizeof(BoardState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) {
        ESP_LOGE(tag, "No internal memory for board state");
        return status_;
    }
    state_ = new (memory) BoardState{};
    status_.psram = esp_psram_is_initialized();
    status_.freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(tag, "PSRAM initialized=%u free=%u", status_.psram, static_cast<unsigned>(status_.freePsram));

    i2c_master_bus_config_t i2c{};
    i2c.i2c_port = I2C_NUM_0;
    i2c.sda_io_num = GPIO_NUM_7;
    i2c.scl_io_num = GPIO_NUM_8;
    i2c.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c.glitch_ignore_cnt = 7;
    i2c.flags.enable_internal_pullup = true;
    const auto i2cResult = i2c_new_master_bus(&i2c, &state_->i2c);
    report("I2C", i2cResult);

    status_.display = report("LCD", initializeDisplay(*state_));
    if (i2cResult == ESP_OK) {
        status_.touch = report("GT911", initializeTouch(*state_));
    }
    // Инициализация аудио выключает PA даже при отказе запуска I2C.
    status_.audio = report("ES8311/I2S", initializeAudio(*state_));
    status_.storage = report("microSD", mountStorage(*state_));
    status_.freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    logConfiguration();
    return status_;
}

/**
 * @brief Повторно монтирует microSD после закрытия файлов приложением.
 * @details Допустим только из меню без заимствованных SD-файлов. Сначала снимает FATFS и освобождает LDO; ошибка очистки сохраняет оставшийся ресурс и прерывает повторное подключение.
 * @return true при успешном новом монтировании, иначе false.
 */
bool Board::refreshStorage() noexcept {
    if (!state_) return false;
    status_.storage = false;
    if (state_->sdCard) {
        const auto error = esp_vfs_fat_sdcard_unmount(storagePath(), state_->sdCard);
        if (!report("microSD unmount", error)) return false;
        state_->sdCard = nullptr; // Объект карты уже освобождён процедурой unmount SDK.
    }
    if (state_->sdPower) {
        const auto error = sd_pwr_ctrl_del_on_chip_ldo(state_->sdPower);
        if (!report("microSD power release", error)) return false;
        state_->sdPower = nullptr;
    }
    status_.storage = report("microSD", mountStorage(*state_));
    return status_.storage;
}

/**
 * @brief Предоставляет изменяемый кадр меню 800×480 в RGB565.
 * @details Буфер принадлежит плате; владение выводом и завершение предыдущей операции обеспечивает вызывающий код.
 * @return Указатель на кадр либо nullptr до успешного выделения.
 */
std::uint16_t* Board::framebuffer() noexcept {
    return state_ ? state_->logicalFrame : nullptr;
}

/**
 * @brief Предоставляет игровой RGB565-источник 256×240 только при свободном владельце.
 * @details Пока gamePresentPending установлен, DMA/PPA или отложенный вывод сохраняет заимствование. Тайм-аут не разрешает перезапись; требуется настоящий finishGamePresent. Зарегистрированный игровой источник также резервирует backing для CPU0 writer между PPA jobs.
 * @return Изменяемый указатель либо nullptr при неготовности, незавершённом выводе или действующей регистрации источника.
 */
std::uint16_t* Board::gameFramebuffer() noexcept {
    // При тайм-ауте DMA сохраняет заимствование RGB-источника. Поздний finish может
    // восстановиться после реального завершения; сам тайм-аут владение не возвращает.
    return state_ && !state_->gamePresentPending && !state_->registeredGameSource ? state_->gameFrame : nullptr;
}

/**
 * @brief Предоставляет отдельные боковые панели управления.
 * @details Две полосы 80×480 упакованы рядом со шагом 160; буфер не содержит меню. После изменения следует пометить физические поля недействительными.
 * @return Указатель на собственный буфер платы либо nullptr.
 */
std::uint16_t* Board::gameControlsFramebuffer() noexcept {
    return state_ ? state_->gameControls : nullptr;
}

/**
 * @brief Переключает необязательную регистрацию времён вывода кадра.
 * @details Владелец CPU0 задаёт режим до подачи транзакции. Без NES_APP_FRAME_TIMING вызов ничего не делает.
 * @param enabled Включить соответствующую необязательную возможность.
 */
void Board::setFrameTimingEnabled(bool enabled) noexcept {
    (void)enabled;
}
/**
 * @brief Копирует диагностические отметки PPA и LCD.
 * @details Читается CPU0 после завершения/ожидания; отметка ISR приобретается через атомарный признак. Младшие 32 бита esp_timer могут переполняться.
 * @return Копия отметок и маски действительных полей; без диагностики — нули.
 */
PresentationTiming Board::gamePresentationTiming() const noexcept {
    PresentationTiming result{};
    return result;
}

/**
 * @brief Синхронно выводит текущий игровой кадр через PPA.
 * @details Завершает предыдущее заимствование, начинает новое и дожидается его окончания на ядре инициализации дисплея.
 * @return true при успехе всех этапов, иначе false без разрешения игнорировать активный DMA.
 */
bool Board::presentGame() noexcept {
    return state_ && status_.display && !state_->registeredGameSource && presentGameDisplay(*state_);
}

/**
 * @brief Начинает асинхронное чтение игрового источника PPA.
 * @details CPU0 заимствует исходный и выбранный физический буферы. Каждый успешный begin должен получить настоящий finish, в том числе при последующей ошибке DSP.
 * @return true при принятой PPA-транзакции; false при ошибке или занятом источнике.
 */
bool Board::beginGamePresent() noexcept {
    return state_ && status_.display && !state_->registeredGameSource && !state_->borrowedGameSource.valid() && beginGameDisplay(*state_);
}
/** @brief Привязывает единственный A к существующему B, проверяя тип обоих концов без allocation. */
bool Board::registerGameSource(const std::uint16_t*source)noexcept{
    if(!state_||!status_.display||xPortGetCoreID()!=state_->displayCore||state_->gamePresentPending||
       state_->registeredGameSource||!distinctGameSources(source,state_->gameFrame)||esp_efuse_is_flash_encryption_enabled())return false;
    if(state_->nativeBufferCount!=3)return false;
    const auto*last=reinterpret_cast<const std::uint8_t*>(source)+122879U;
    if(!(esp_ptr_external_ram(source)&&esp_ptr_external_ram(last)))return false;
    state_->registeredGameSource=source;return true;
}
/** @brief Снимает только совпадающую регистрацию после reader/native drain. */
bool Board::unregisterGameSource(const std::uint16_t*source)noexcept{
    if(!state_||xPortGetCoreID()!=state_->displayCore||state_->gamePresentPending||state_->borrowedGameSource.valid()||
       state_->registeredGameSource!=source)return false;
    state_->registeredGameSource=nullptr;return true;
}
/** @brief Проверяет fixed A/B и сохраняет key до запуска, чтобы ранний ISR не опередил публикацию. */
bool Board::beginBorrowedGamePresent(const GameSourceTicket&ticket)noexcept{
    if(!state_||!status_.display||!ticket.valid()||!state_->registeredGameSource||
       ticket.pixels!=(ticket.slot==0?state_->registeredGameSource:state_->gameFrame))return false;
    return beginGameDisplay(*state_,&ticket);
}
/** @brief Возвращает borrow только по совпадающему полному ticket и настоящему PPA/LCD finish. */
bool Board::finishBorrowedGamePresent(const GameSourceTicket&ticket,unsigned timeoutMs)noexcept{
    if(!state_||!status_.display||!ticket.valid()||!sameGameSource(ticket,state_->borrowedGameSource))return false;
    if(!finishGameDisplay(*state_,timeoutMs))return false;
    state_->borrowedGameSource={};return true;
}
/** @brief Неблокирующий borrowed-poll проверяет полный ключ до чтения семафоров. */
GamePresentPollResult Board::pollBorrowedGamePresent(const GameSourceTicket&ticket)noexcept{
    if(!state_||!status_.display||!ticket.valid()||!sameGameSource(ticket,state_->borrowedGameSource))
        return GamePresentPollResult::Fault;
    const auto result=pollGameDisplay(*state_);
    if(result==GamePresentPollResult::Submitted)state_->borrowedGameSource={};
    return result;
}

/**
 * @brief Завершает ранее начатый игровой вывод.
 * @details Ожидает реальное завершение PPA и требуемую границу LCD; тайм-аут оставляет незавершённый ресурс заимствованным. Поддерживает позднее завершение.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return true при успешном завершении либо отсутствии pending-операции; false при ошибке.
 */
bool Board::finishGamePresent(unsigned timeoutMs) noexcept {
    return state_ && status_.display && !state_->borrowedGameSource.valid() && finishGameDisplay(*state_, timeoutMs);
}

/**
 * @brief Выводит кадр меню с поворотом в физический буфер LCD.
 * @details Перед записью меню ограждает более старый игровой DMA. Вызывается на том же ядре, что initializeDisplay.
 * @return true при успешных PPA и подаче LCD, иначе false.
 */
bool Board::present() noexcept {
    return state_ && status_.display && !state_->borrowedGameSource.valid() && presentDisplay(*state_);
}

/**
 * @brief Заливает логический кадр меню одним RGB565-цветом.
 * @details Не запускает вывод; также помечает игровые боковые поля для обновления при возвращении в игру.
 * @param color Цвет RGB565 без передачи владения буфером.
 */
void Board::clear(std::uint16_t color) noexcept {
    invalidateGameBars();
    if (auto* frame = framebuffer()) std::fill_n(frame, logicalPanel.width * logicalPanel.height, color);
}

/**
 * @brief Помечает все выделенные физические боковые поля устаревшими.
 * @details Следующий безопасный вывод скопирует новые панели управления; текущая развёртка здесь не перезаписывается.
 */
void Board::invalidateGameBars() noexcept {
    if(state_)gameBarsDirtyState.full(state_);
    if (state_) for(auto& flag:state_->gameBarsCleared)flag=false;
}

/**
 * @brief Отмечает отсечённую область панелей для каждого native, не записывая LCD.
 * @details OFF сохраняет полный invalidate; Main-only ON объединяет долг до безопасной копии.
 * @param x,y Логическое начало в пикселях800×480.
 * @param width,height Размер; центр игры и область вне панели не затрагиваются.
 */
void Board::invalidateGameBars(int x,int y,int width,int height) noexcept {
    if(state_&&gameBarsDirtyState.mark(state_,x,y,width,height)){
        for(auto& flag:state_->gameBarsCleared)flag=false;
    }
}

/**
 * @brief Вычисляет ёмкость успешно смонтированной карты в байтах.
 * @details Использует CSD и размер сектора существующего sdCard, без чтения новых секторов.
 * @return 64-битная ёмкость либо 0 при недоступной карте.
 */
std::uint64_t Board::storageCapacity() const noexcept {
    return state_ && status_.storage && state_->sdCard
        ? static_cast<std::uint64_t>(state_->sdCard->csd.capacity) * state_->sdCard->csd.sector_size : 0;
}

/**
 * @brief Читает первый контакт GT911 в логических координатах.
 * @details Результат предварительно обнуляется; отсутствие касания может быть успешным чтением с pressed=false.
 * @param point Выходная точка первого контакта либо обнулённое отсутствие нажатия.
 * @return true при достоверном опросе, false при ошибке или недоступном сенсоре.
 */
bool Board::pollTouch(TouchPoint& point) noexcept {
    point = {};
    return state_ && status_.touch && readTouch(*state_, point);
}

/**
 * @brief Читает до пяти достоверных контактов GT911.
 * @details Обнуляет выход перед обращением к сенсору; синхронный и фоновый режимы выбираются при сборке.
 * @param contacts Выходной ограниченный снимок контактов; при ошибке сбрасывается.
 * @return true при достоверном снимке, включая count=0; false при ошибке/устаревании.
 */
bool Board::pollTouchContacts(TouchContacts& contacts) noexcept {
    contacts={};
    return state_ && status_.touch && readTouchContacts(*state_,contacts);
}

/**
 * @brief Передаёт PCM16 mono48 кГц в два одинаковых I2S-слота.
 * @details Буфер вызывающего кода читается синхронно блоками не более 256 отсчётов. При ошибке/тайм-ауте возвращает фактически принятый префикс; принятие не равно завершению DMA или гарантии непрерывного звука.
 * @param samples Заимствованный массив знаковых 16-битных моноотсчётов 48 кГц.
 * @param count Количество элементов/отсчётов/байтов по контракту вызываемой функции.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return Число принятых моноотсчётов 0..count.
 */
std::size_t Board::writeAudio(const std::int16_t* samples, std::size_t count, unsigned timeoutMs) noexcept {
    if (!samples || !count || !state_ || !status_.audio) return 0;
    const auto begin = esp_timer_get_time();
    const bool monitor = state_->audioActive.load(std::memory_order_acquire);
    if (monitor) {
        if (state_->lastAudioWriteUs) {
            const auto gap = static_cast<std::uint32_t>(std::min<std::int64_t>(begin - state_->lastAudioWriteUs, UINT32_MAX));
            state_->audioMaxWriteGapUs.store(std::max(gap, state_->audioMaxWriteGapUs.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        }
        state_->lastAudioWriteUs = begin;
    }
    std::size_t accepted = 0;
    std::int16_t stereo[512];
    while (accepted < count) {
        const auto chunk = std::min<std::size_t>(256, count - accepted);
        for (std::size_t i = 0; i < chunk; ++i) stereo[i * 2] = stereo[i * 2 + 1] = samples[accepted + i];
        std::size_t bytes = 0;
        const auto result = i2s_channel_write(state_->audioTx, stereo, chunk * 4, &bytes, timeoutMs);
        accepted += bytes / 4;
        if (result != ESP_OK || bytes != chunk * 4) {
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT) ESP_LOGW(tag, "I2S write: %s", esp_err_to_name(result));
            break;
        }
    }
    if (monitor) {
        state_->audioAcceptedFrames.fetch_add(static_cast<std::uint32_t>(accepted), std::memory_order_relaxed);
        const auto elapsed = static_cast<std::uint32_t>(std::min<std::int64_t>(esp_timer_get_time() - begin, UINT32_MAX));
        state_->audioMaxWriteWaitUs.store(std::max(elapsed, state_->audioMaxWriteWaitUs.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    return accepted;
}

/**
 * @brief Применяет ограниченную громкость кодека без положительного усиления DAC.
 * @details Сначала запрашивает выключение PA; при ошибке повторно запрашивает тихий режим, не доказывая фактический уровень GPIO. Окно статистики возобновляется лишь при запрошенном воспроизведении и ненулевом уровне.
 * @param percent Желаемый уровень 0..100; значения выше 100 ограничиваются.
 * @return true при успешной настройке; false при отсутствии узла или аппаратной ошибке.
 */
bool Board::setVolume(unsigned percent) noexcept {
    if (!state_) return false;
    const bool wasActive = state_->audioActive.exchange(false, std::memory_order_acq_rel);
    if (!status_.audio) { static_cast<void>(setAudioOutputActive(*state_, false)); return false; }
    const bool okay = changeVolume(*state_, std::min(percent, 100U));
    if (okay && state_->audioPlaybackRequested && state_->volume) {
        if (!wasActive) resetAudioWindow(*state_);
        state_->audioActive.store(true, std::memory_order_release);
    }
    return okay;
}

/**
 * @brief Возвращает сохранённую громкость, не обращаясь к кодеку.
 * @return Последний успешный процент 0..100 либо 0 до инициализации.
 */
unsigned Board::volume() const noexcept {
    return state_ ? state_->volume : 0;
}

/**
 * @brief Задаёт намерение воспроизведения и отдельное окно аудиостатистики.
 * @details Отключает учёт перед аппаратным изменением PA/DAC; true с успешным ненулевым уровнем начинает новое окно с 40 мс льготы. Громкость пользователя этим методом не заменяется.
 * @param active Запрошенная активность воспроизведения/диагностики.
 */
void Board::setAudioActive(bool active) noexcept {
    if (!state_) return;
    state_->audioActive.store(false, std::memory_order_release);
    const bool okay = setAudioOutputActive(*state_, active && status_.audio);
    if (!active || !okay || !state_->volume) return;
    resetAudioWindow(*state_);
    state_->audioActive.store(true, std::memory_order_release);
}

/**
 * @brief Считывает подлинные счётчики Board/I2S текущего окна.
 * @details Поля читаются раздельно и могут отражать близкие моменты ISR; это не атомарный снимок всего тракта. uint32-счётчики переполняются. Пропуск пополнения не учитывает все возможные пропавшие отсчёты.
 * @return Копия счётчиков либо нулевые значения до инициализации.
 */
AudioStats Board::audioStats() const noexcept {
    AudioStats stats{};
    if (!state_) return stats;
    stats.active = state_->audioActive.load(std::memory_order_acquire);
    stats.acceptedFrames = state_->audioAcceptedFrames.load(std::memory_order_relaxed);
    stats.dmaFrames = state_->audioDmaFrames.load(std::memory_order_relaxed);
    stats.missedRefillBlocks = state_->audioMissedBlocks.load(std::memory_order_relaxed);
    stats.missedRefillFrames = state_->audioMissedFrames.load(std::memory_order_relaxed);
    stats.maxWriteGapUs = state_->audioMaxWriteGapUs.load(std::memory_order_relaxed);
    stats.maxWriteWaitUs = state_->audioMaxWriteWaitUs.load(std::memory_order_relaxed);
    return stats;
}

/**
 * @brief Выводит модель платы и сохранённую готовность узлов.
 * @details Не запускает повторную инициализацию и не доказывает физическое качество изображения или звука.
 */
void Board::logConfiguration() const noexcept {
    ESP_LOGI(tag, "Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3; native480x800 landscape800x480");
    ESP_LOGI(tag, "BOARD_READY display=%u touch=%u audio=%u sd=%u psram=%u free_psram=%u",
             status_.display, status_.touch, status_.audio, status_.storage, status_.psram,
             static_cast<unsigned>(status_.freePsram));
}

/**
 * @brief Разрешает повторную запись RGB после реального чтения PPA.
 * @details Только владелец дисплея: успешное получение источника не подаёт старый LCD-кадр и не освобождает его физический индекс. Обычный gameFramebuffer остаётся строгим.
 * @param source Выходной указатель: nullptr до успеха, при Ready разрешена запись только RGB-источника.
 * @param timeoutMs Максимальное ожидание одной операции в миллисекундах; тайм-аут не означает завершения DMA.
 * @return Ready/Unsupported/Timeout/InvalidState; при неуспехе source=nullptr.
 */
GameSourceAcquireResult Board::acquireCompletedGameSource(std::uint16_t*&source,unsigned timeoutMs)noexcept{
    source=nullptr;
    if(!state_||!status_.display)return GameSourceAcquireResult::InvalidState;
    return acquireCompletedGameSourceDisplay(*state_,source,timeoutMs);
}

} // пространство имён nes::platform
