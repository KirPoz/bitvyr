#include "board/BluetoothInput.h"
#include "HidGamepad.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <new>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_bt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gatt.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

// В NimBLE SDK 6.1 TinyCrypt на ESP32-P4 неверно формирует скаляр P-256:
// peer отклоняет DHKey при Secure Connections. Используем исправный mbedTLS,
// сохраняя Secure Connections, bonding и существующие ключи NVS.
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4 && \
    (!defined(CONFIG_BT_NIMBLE_CRYPTO_STACK_MBEDTLS) || !CONFIG_BT_NIMBLE_CRYPTO_STACK_MBEDTLS)
#error "ESP32-P4 NimBLE requires CONFIG_BT_NIMBLE_CRYPTO_STACK_MBEDTLS for Secure Connections"
#endif

extern "C"
/**
 * @brief Подключает штатное хранилище ключей сопряжения NimBLE.
 * @details Реализация предоставлена SDK; клиент вызывает её после инициализации host.
 */
void ble_store_config_init(void);

namespace nes::platform {
namespace {
constexpr const char* tag = "nes_xbox";
/**
 * @brief Фазы запуска, поиска, сопряжения и готовности BLE-клиента; Ready требует пригодную игровую подписку.
 */
enum class StateCode : unsigned { Starting, Scanning, Connecting, Pairing, Discovering, Ready, Failed };
/**
 * @brief Ограниченная запись одной найденной GATT-характеристики и её дескрипторов; handle принадлежат текущему соединению.
 */
struct Characteristic {
    std::uint16_t value{0}, uuid{0}, end{0}, cccd{0}, reference{0};
    std::uint8_t properties{0}, reportId{0}, reportType{0};
    bool subscribed{false}, nesControls{false};
};
}

/**
 * @brief Долговечное внутреннее состояние единственного BLE-клиента. Callback host владеют discovery/парсером; приложению публикуются атомарные кнопки, готовность и фронт меню.
 */
struct BluetoothState final {
    std::atomic<StateCode> status{StateCode::Starting};
    std::atomic<std::uint8_t> buttons{0};
    std::atomic<bool> connected{false};
    std::atomic<bool> menu{false};
    std::uint16_t connection{BLE_HS_CONN_HANDLE_NONE};
    std::uint8_t ownAddressType{0};
    std::uint16_t serviceStart{0}, serviceEnd{0}, reportMapHandle{0}, protocolHandle{0};
    Characteristic characteristics[24]{};
    unsigned characteristicCount{0}, reportIndex{0}, subscriptionCount{0}, gamepadSubscriptionCount{0};
    std::uint8_t descriptor[2048]{};
    std::size_t descriptorSize{0};
    HidGamepad parser{};
    HidPadState pad{};
    bool previousMenu{false};
    bool discovering{false};
    std::uint32_t reports{0};
    ble_addr_t preferredPeer{};
    bool hasPreferredPeer{false};
    ble_npl_callout scanRetry{};
    unsigned scanRetryDelayMs{250};
    bool scanRetryAvailable{false};
};

namespace {
BluetoothState* active = nullptr; // Единственный клиент радио/контроллера для этой платы.
/**
 * @brief Обрабатывает жизненный цикл соединения и уведомления GAP.
 * @details Соединяет, запускает SMP и ждёт ENC_CHANGE перед discovery; разрыв отпускает кнопки. Повторное pairing удаляет bond только проверенного текущего peer.
 * @param event Событие SDK; указатель действителен только внутри callback.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 либо код политики BLE_GAP_REPEAT_PAIRING_* для повторного сопряжения.
 */
int gapEvent(ble_gap_event* event, void* context);
/**
 * @brief Начинает активное сканирование подходящего контроллера.
 * @details Не конфликтует с активным соединением; дубликаты разрешены, поскольку имя может приходить только в scan response. Ошибка не запускает бесконечный немедленный цикл.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void startScan(BluetoothState& state);
/**
 * @brief Запускает единственную процедуру обнаружения HID-сервиса.
 * @details Повторный параллельный запуск не делает; очищает прошлые дескрипторы/счётчики после завершённого шифрования связи.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void discoverService(BluetoothState& state);
/**
 * @brief Продвигает последовательное обнаружение/подписку HID-отчётов.
 * @details Готовность наступает только при хотя бы одной пригодной игровой подписке; затем peer сохраняется в NVS.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void nextReport(BluetoothState& state);
/**
 * @brief Отменяет ожидающий повтор сканирования NimBLE.
 * @details Использует существующий callout только после успешной регистрации очереди событий.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void stopScanRetry(BluetoothState& state);
/**
 * @brief Планирует ограниченный по частоте повтор BLE-сканирования.
 * @details Не запускается при рассинхронизации host, активной связи/сканировании; задержка растёт 250..5000 мс. Таймер принадлежит BluetoothState.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param reason Числовая причина события/повторной попытки для журнала.
 */
void scheduleScanRetry(BluetoothState& state, int reason);

/**
 * @brief Извлекает короткий 16-битный UUID NimBLE.
 * @details Не пытается сокращать 128-битные UUID до короткого формата.
 * @param uuid UUID SDK, заимствованный на время преобразования.
 * @return UUID16 либо 0 для иного типа.
 */
std::uint16_t uuid16(const ble_uuid_any_t& uuid) noexcept {
    return uuid.u.type == BLE_UUID_TYPE_16 ? uuid.u16.value : 0;
}

/**
 * @brief Атомарно отпускает ввод при разрыве/ошибке BLE.
 * @details Также очищает сохранённый pad и фронт меню; соединение больше не считается готовым.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void releaseButtons(BluetoothState& state) noexcept {
    state.buttons.store(0, std::memory_order_relaxed);
    state.connected.store(false, std::memory_order_release);
    state.menu.store(false, std::memory_order_relaxed);
    state.pad = {};
    state.previousMenu = false;
}

/**
 * @brief Журналирует ошибку HID-процедуры и прекращает непригодное соединение.
 * @details Отпускает кнопки; уже закрытая связь допускает отложенное сканирование. Не стирает чужие данные NVS.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param operation Имя операции для диагностического сообщения либо заполняемые параметры PPA.
 * @param result Код результата проверяемой операции.
 */
void reject(BluetoothState& state, const char* operation, int result) {
    const char* detail = result == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_DHKEY)
        ? ": peer SMP DHKey check failed"
        : result == BLE_HS_SM_US_ERR(BLE_SM_ERR_DHKEY)
            ? ": local SMP DHKey check failed" : "";
    ESP_LOGW(tag, "%s failed rc=%d (0x%x)%s", operation, result, static_cast<unsigned>(result), detail);
    releaseButtons(state);
    state.status.store(StateCode::Failed, std::memory_order_relaxed);
    if (state.connection != BLE_HS_CONN_HANDLE_NONE) {
        const int terminated = ble_gap_terminate(state.connection, BLE_ERR_REM_USER_CONN_TERM);
        if (terminated == BLE_HS_ENOTCONN) {
            state.connection = BLE_HS_CONN_HANDLE_NONE;
            state.discovering = false;
            scheduleScanRetry(state, terminated);
        } else if (terminated && terminated != BLE_HS_EALREADY) {
            ESP_LOGW(tag, "Connection termination failed rc=%d", terminated);
        }
    }
}

/**
 * @brief Отбирает известный контроллер или подходящее Xbox HID-объявление.
 * @details Принимает сохранённого peer, имя Xbox либо Microsoft manufacturer вместе с HID service; один manufacturer недостаточен. Пакет заимствован только на время вызова.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param advertisement Разобранный SDK пакет объявления; указатель/данные не удерживаются.
 * @return true для кандидата на соединение, иначе false.
 */
bool xboxAdvertisement(const BluetoothState& state, const ble_gap_disc_desc& advertisement) noexcept {
    if (state.hasPreferredPeer && (advertisement.addr.type & 1) == (state.preferredPeer.type & 1) &&
        std::memcmp(advertisement.addr.val, state.preferredPeer.val, sizeof(advertisement.addr.val)) == 0) return true;
    ble_hs_adv_fields fields{};
    if (ble_hs_adv_parse_fields(&fields, advertisement.data, advertisement.length_data) != 0) return false;
    for (unsigned i = 0; i + 4 <= fields.name_len; ++i) {
        if ((fields.name[i] == 'X' || fields.name[i] == 'x') &&
            (fields.name[i + 1] == 'B' || fields.name[i + 1] == 'b') &&
            (fields.name[i + 2] == 'O' || fields.name[i + 2] == 'o') &&
            (fields.name[i + 3] == 'X' || fields.name[i + 3] == 'x')) return true;
    }
    // Microsoft manufacturer встречается и у ПК. Без имени дополнительно требуем
    // объявленный HID-сервис перед началом соединения.
    if (fields.mfg_data_len < 2 || fields.mfg_data[0] != 0x06 || fields.mfg_data[1] != 0x00) return false;
    for (unsigned i = 0; i < fields.num_uuids16; ++i) if (fields.uuids16[i].value == 0x1812) return true;
    return false;
}

/**
 * @brief Сохраняет идентификационный адрес успешно готового контроллера.
 * @details Читает текущую связь NimBLE и обновляет только собственный namespace nes_xbox; NVS-ошибка не удаляет существующие данные.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void rememberPeer(BluetoothState& state) {
    ble_gap_conn_desc connection{};
    if (ble_gap_conn_find(state.connection, &connection) != 0) return;
    if (state.hasPreferredPeer && std::memcmp(&state.preferredPeer, &connection.peer_id_addr, sizeof(ble_addr_t)) == 0) return;
    state.preferredPeer = connection.peer_id_addr;
    state.hasPreferredPeer = true;
    nvs_handle_t handle = 0;
    if (nvs_open("nes_xbox", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_blob(handle, "peer", &state.preferredPeer, sizeof(state.preferredPeer)) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
}

/**
 * @brief Декодирует уведомление HID и публикует NES-маску/фронт меню.
 * @details Ограничивает вход 128 байтами, учитывает подтверждённую подписку и Report ID, применяет dead zone14000 и исключает противоположные направления. os_mbuf не удерживается.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param handle GATT handle характеристики уведомления.
 * @param mbuf Цепочка входного пакета NimBLE, принадлежащая SDK.
 */
void publishReport(BluetoothState& state, std::uint16_t handle, os_mbuf* mbuf) {
    if (!state.connected.load(std::memory_order_acquire)) return;
    const auto length = OS_MBUF_PKTLEN(mbuf);
    if (!length || length > 128) return;
    std::uint8_t report[128]{};
    if (os_mbuf_copydata(mbuf, 0, length, report) != 0) return;
    for (unsigned i = 0; i < state.characteristicCount; ++i) {
        const auto& characteristic = state.characteristics[i];
        if (characteristic.value != handle || !characteristic.subscribed) continue;
        auto pad = state.pad;
        if (!state.parser.decode(characteristic.reportId, report, length, pad)) return;
        state.pad = pad;
        /**
         * @brief Проверяет одну кнопку уже декодированного локального pad.
         * @param usage Номер Button Usage 1..32; здесь передаются только проверенные константы.
         * @return true, если соответствующий бит нажат.
         */
        auto pressed = [&pad](unsigned usage) { return (pad.buttons & (std::uint32_t{1} << (usage - 1))) != 0; };
        const bool extended = state.parser.maximumButtonUsage() >= 13;
        const bool select = pressed(extended ? 11 : 7);
        const bool start = pressed(extended ? 12 : 8);
        const bool guide = pad.guide || (extended && pressed(13));
        std::uint8_t buttons = (pressed(1) ? 1 : 0) | (pressed(2) ? 2 : 0) |
                               (select ? 4 : 0) | (start ? 8 : 0);
        const bool up = (pad.directions & 1) || pad.y < -14000;
        const bool down = (pad.directions & 2) || pad.y > 14000;
        const bool left = (pad.directions & 4) || pad.x < -14000;
        const bool right = (pad.directions & 8) || pad.x > 14000;
        // Физическая крестовина NES не удерживает противоположные направления одновременно.
        if (up != down) buttons |= up ? 16 : 32;
        if (left != right) buttons |= left ? 64 : 128;
        state.buttons.store(buttons, std::memory_order_release);
        const bool menu = guide || (select && start);
        if (menu && !state.previousMenu) state.menu.store(true, std::memory_order_release);
        state.previousMenu = menu;
        if (++state.reports == 1) ESP_LOGI(tag, "XBOX_FIRST_INPUT report_id=%u bytes=%u", characteristic.reportId, length);
        return;
    }
}

/**
 * @brief Завершает запрос CCCD для текущего входного отчёта.
 * @details Игнорирует чужое соединение; успешная подписка учитывается отдельно для игровых контролов, затем начинается следующий отчёт.
 * @param connection Идентификатор соединения, проверяемый против текущего состояния.
 * @param error Заимствованный код/описание завершения SDK.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 для продолжения обработки NimBLE.
 */
int subscribed(std::uint16_t connection, const ble_gatt_error* error, ble_gatt_attr*, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    if (connection != state.connection) return 0;
    if (error->status == 0) {
        state.characteristics[state.reportIndex].subscribed = true;
        ++state.subscriptionCount;
        if (state.characteristics[state.reportIndex].nesControls) ++state.gamepadSubscriptionCount;
    } else ESP_LOGW(tag, "Report subscription failed rc=%d", error->status);
    ++state.reportIndex;
    nextReport(state);
    return 0;
}

/**
 * @brief Обрабатывает пару Report ID/type дескриптора 0x2908.
 * @details Подписывается лишь на Input с поддерживаемыми контролами и объявленной длиной до 128 байтов; при отказе переходит дальше.
 * @param connection Идентификатор соединения, проверяемый против текущего состояния.
 * @param error Заимствованный код/описание завершения SDK.
 * @param attribute Заимствованная запись/данные GATT-ответа.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 для продолжения обработки NimBLE.
 */
int reportReference(std::uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attribute, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    if (connection != state.connection) return 0;
    auto& characteristic = state.characteristics[state.reportIndex];
    std::uint8_t reference[2]{};
    if (!error->status && attribute && OS_MBUF_PKTLEN(attribute->om) >= 2 &&
        os_mbuf_copydata(attribute->om, 0, sizeof(reference), reference) == 0) {
        characteristic.reportId = reference[0];
        characteristic.reportType = reference[1];
        if (characteristic.reportType == 1 && characteristic.cccd) {
            const auto info = state.parser.reportInfo(characteristic.reportId);
            if (info.declaredBytes <= 128 && (info.nesControls || info.menuControl)) {
                characteristic.nesControls = info.nesControls;
                const std::uint8_t enable[]{1,0};
                const int result = ble_gattc_write_flat(connection, characteristic.cccd, enable, sizeof(enable), subscribed, &state);
                if (result == 0) return 0;
                ESP_LOGW(tag, "CCCD request failed rc=%d", result);
            } else ESP_LOGW(tag, "Unsupported HID input report id=%u required_bytes=%u declared_bytes=%u", characteristic.reportId, info.requiredBytes, info.declaredBytes);
        }
    }
    ++state.reportIndex;
    nextReport(state);
    return 0;
}

/**
 * @brief Собирает CCCD и Report Reference характеристики HID.
 * @details После окончания диапазона начинает чтение reference либо следующий отчёт; указатели SDK не сохраняются.
 * @param connection Идентификатор соединения, проверяемый против текущего состояния.
 * @param error Заимствованный код/описание завершения SDK.
 * @param descriptor Заимствованный HID-дескриптор либо запись GATT, не сохраняемая как указатель.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 для продолжения обработки NimBLE.
 */
int descriptorFound(std::uint16_t connection, const ble_gatt_error* error, std::uint16_t,
                    const ble_gatt_dsc* descriptor, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    if (connection != state.connection) return 0;
    auto& characteristic = state.characteristics[state.reportIndex];
    if (!error->status && descriptor) {
        const auto uuid = uuid16(descriptor->uuid);
        if (uuid == 0x2902) characteristic.cccd = descriptor->handle;
        if (uuid == 0x2908) characteristic.reference = descriptor->handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (characteristic.reference && characteristic.cccd &&
            ble_gattc_read(connection, characteristic.reference, reportReference, &state) == 0) return 0;
        ++state.reportIndex;
        nextReport(state);
    } else {
        ++state.reportIndex;
        nextReport(state);
    }
    return 0;
}

/**
 * @brief Продвигает последовательное обнаружение/подписку HID-отчётов.
 * @details Готовность наступает только при хотя бы одной пригодной игровой подписке; затем peer сохраняется в NVS.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void nextReport(BluetoothState& state) {
    while (state.reportIndex < state.characteristicCount) {
        auto& characteristic = state.characteristics[state.reportIndex];
        if (characteristic.uuid == 0x2A4D && (characteristic.properties & BLE_GATT_CHR_PROP_NOTIFY) && characteristic.end > characteristic.value) {
            const auto result = ble_gattc_disc_all_dscs(state.connection, characteristic.value, characteristic.end, descriptorFound, &state);
            if (!result) return;
        }
        ++state.reportIndex;
    }
    if (!state.gamepadSubscriptionCount) { reject(state, "No usable gamepad subscriptions", BLE_HS_ENOENT); return; }
    state.discovering = false;
    state.connected.store(true, std::memory_order_release);
    state.status.store(StateCode::Ready, std::memory_order_relaxed);
    rememberPeer(state);
    ESP_LOGI(tag, "XBOX_HID_READY fields=%u input_reports=%u gameplay_reports=%u; A/B View/Menu D-pad/stick", state.parser.fieldCount(), state.subscriptionCount, state.gamepadSubscriptionCount);
}

/**
 * @brief Собирает ограниченную длинную характеристику HID Report Map.
 * @details Проверяет непрерывность offset и вместимость 2048 байтов, затем вызывает собственный parser. Ошибка формы прекращает соединение.
 * @param connection Идентификатор соединения, проверяемый против текущего состояния.
 * @param error Заимствованный код/описание завершения SDK.
 * @param attribute Заимствованная запись/данные GATT-ответа.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 либо BLE_HS_EBADDATA при нарушенных границах.
 */
int reportMap(std::uint16_t connection, const ble_gatt_error* error, ble_gatt_attr* attribute, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    if (connection != state.connection) return 0;
    if (!error->status && attribute) {
        const std::size_t size = OS_MBUF_PKTLEN(attribute->om);
        if (attribute->offset != state.descriptorSize || size > sizeof(state.descriptor) - state.descriptorSize ||
            os_mbuf_copydata(attribute->om, 0, size, state.descriptor + state.descriptorSize) != 0) {
            reject(state, "HID report map bounds", BLE_HS_EBADDATA);
            return BLE_HS_EBADDATA;
        }
        state.descriptorSize += size;
    } else if (error->status == BLE_HS_EDONE) {
        if (!state.parser.parse(state.descriptor, state.descriptorSize)) {
            reject(state, "Unsupported HID report map", BLE_HS_EBADDATA);
            return 0;
        }
        state.reportIndex = 0;
        nextReport(state);
    } else reject(state, "Read HID report map", error->status);
    return 0;
}

/**
 * @brief Запоминает характеристики HID-сервиса и начинает чтение карты.
 * @details Ограничивает таблицу 24 элементами; переключает Report Protocol при наличии соответствующей характеристики.
 * @param connection Идентификатор соединения, проверяемый против текущего состояния.
 * @param error Заимствованный код/описание завершения SDK.
 * @param characteristic Заимствованная найденная характеристика GATT.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 либо BLE_HS_ENOMEM при переполнении таблицы.
 */
int characteristicFound(std::uint16_t connection, const ble_gatt_error* error, const ble_gatt_chr* characteristic, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    if (connection != state.connection) return 0;
    if (!error->status && characteristic) {
        if (state.characteristicCount == 24) { reject(state, "HID characteristic limit", BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
        if (state.characteristicCount) state.characteristics[state.characteristicCount - 1].end = characteristic->def_handle - 1;
        auto& target = state.characteristics[state.characteristicCount++];
        target = {};
        target.value = characteristic->val_handle;
        target.end = state.serviceEnd;
        target.uuid = uuid16(characteristic->uuid);
        target.properties = characteristic->properties;
        if (target.uuid == 0x2A4B) state.reportMapHandle = target.value;
        if (target.uuid == 0x2A4E) state.protocolHandle = target.value;
    } else if (error->status == BLE_HS_EDONE) {
        if (!state.reportMapHandle) { reject(state, "Missing HID report map", BLE_HS_ENOENT); return 0; }
        if (state.protocolHandle) {
            const std::uint8_t reportMode = 1;
            ble_gattc_write_no_rsp_flat(connection, state.protocolHandle, &reportMode, 1);
        }
        const int result = ble_gattc_read_long(connection, state.reportMapHandle, 0, reportMap, &state);
        if (result) reject(state, "HID map request", result);
    } else reject(state, "HID characteristics", error->status);
    return 0;
}

/**
 * @brief Обрабатывает обнаружение BLE HID service0x1812.
 * @details Сохраняет диапазон и по завершении начинает обнаружение характеристик; чужая связь игнорируется.
 * @param connection Идентификатор соединения, проверяемый против текущего состояния.
 * @param error Заимствованный код/описание завершения SDK.
 * @param service Заимствованный найденный GATT-сервис.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 для продолжения обработки NimBLE.
 */
int serviceFound(std::uint16_t connection, const ble_gatt_error* error, const ble_gatt_svc* service, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    if (connection != state.connection) return 0;
    if (!error->status && service) {
        state.serviceStart = service->start_handle;
        state.serviceEnd = service->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!state.serviceStart) { reject(state, "No BLE HID service", BLE_HS_ENOENT); return 0; }
        const int result = ble_gattc_disc_all_chrs(connection, state.serviceStart, state.serviceEnd, characteristicFound, &state);
        if (result) reject(state, "HID characteristic request", result);
    } else reject(state, "HID service", error->status);
    return 0;
}

/**
 * @brief Запускает единственную процедуру обнаружения HID-сервиса.
 * @details Повторный параллельный запуск не делает; очищает прошлые дескрипторы/счётчики после завершённого шифрования связи.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void discoverService(BluetoothState& state) {
    if (state.discovering) return;
    state.discovering = true;
    state.status.store(StateCode::Discovering, std::memory_order_relaxed);
    state.serviceStart = state.serviceEnd = state.reportMapHandle = state.protocolHandle = 0;
    state.characteristicCount = state.subscriptionCount = state.gamepadSubscriptionCount = state.reportIndex = 0;
    state.descriptorSize = 0;
    const ble_uuid16_t hid = BLE_UUID16_INIT(0x1812);
    const int result = ble_gattc_disc_svc_by_uuid(state.connection, &hid.u, serviceFound, &state);
    if (result) reject(state, "HID service request", result);
}

/**
 * @brief Отменяет ожидающий повтор сканирования NimBLE.
 * @details Использует существующий callout только после успешной регистрации очереди событий.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void stopScanRetry(BluetoothState& state) {
    if (state.scanRetryAvailable) ble_npl_callout_stop(&state.scanRetry);
}

/**
 * @brief Планирует ограниченный по частоте повтор BLE-сканирования.
 * @details Не запускается при рассинхронизации host, активной связи/сканировании; задержка растёт 250..5000 мс. Таймер принадлежит BluetoothState.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @param reason Числовая причина события/повторной попытки для журнала.
 */
void scheduleScanRetry(BluetoothState& state, int reason) {
    if (!state.scanRetryAvailable || !ble_hs_synced() || state.connection != BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_conn_active() || ble_gap_disc_active()) return;
    const unsigned delay = state.scanRetryDelayMs;
    const int result = ble_npl_callout_reset(&state.scanRetry, ble_npl_time_ms_to_ticks32(delay));
    if (result) {
        ESP_LOGW(tag, "Scan retry scheduling failed rc=%d", result);
        return;
    }
    state.scanRetryDelayMs = delay < 2000 ? delay * 2 : 5000;
    ESP_LOGW(tag, "XBOX_SCAN_RETRY rc=%d delay_ms=%u", reason, delay);
}

/**
 * @brief Обрабатывает отложенное событие повторного сканирования.
 * @details Получает заимствованный BluetoothState из аргумента callout и использует общие проверки startScan.
 * @param event Событие SDK; указатель действителен только внутри callback.
 */
void retryScan(ble_npl_event* event) {
    auto& state = *static_cast<BluetoothState*>(ble_npl_event_get_arg(event));
    startScan(state); // Ниже используются общие проверки host/соединения/discovery.
}

/**
 * @brief Проверяет, допускает ли ошибка сканирования отложенный повтор.
 * @details Список ограничен временной занятостью, нехваткой ресурсов, тайм-аутом и HCI command-disallowed.
 * @param result Код результата проверяемой операции.
 * @return true для перечисленных временных ошибок, иначе false.
 */
bool retryableScanError(int result) {
    return result == BLE_HS_EBUSY || result == BLE_HS_EPREEMPTED || result == BLE_HS_ENOMEM ||
        result == BLE_HS_EAGAIN || result == BLE_HS_ETIMEOUT ||
        result == BLE_HS_HCI_ERR(BLE_ERR_CMD_DISALLOWED);
}

/**
 * @brief Начинает активное сканирование подходящего контроллера.
 * @details Не конфликтует с активным соединением; дубликаты разрешены, поскольку имя может приходить только в scan response. Ошибка не запускает бесконечный немедленный цикл.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 */
void startScan(BluetoothState& state) {
    if (!ble_hs_synced() || state.connection != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_active()) return;
    if (ble_gap_disc_active()) {
        stopScanRetry(state);
        state.scanRetryDelayMs = 250;
        state.status.store(StateCode::Scanning);
        return;
    }
    ble_gap_disc_params scan{};
    scan.passive = 0;
    scan.itvl = 80;
    scan.window = 48;
    scan.filter_duplicates = 0; // Xbox может передавать имя только в ответе сканирования.
    const auto result = ble_gap_disc(state.ownAddressType, BLE_HS_FOREVER, &scan, gapEvent, &state);
    if (result) {
        state.status.store(StateCode::Failed);
        ESP_LOGW(tag, "BLE scan rc=%d", result);
        if (retryableScanError(result)) scheduleScanRetry(state, result);
        else stopScanRetry(state);
    } else {
        stopScanRetry(state);
        state.scanRetryDelayMs = 250;
        state.status.store(StateCode::Scanning);
        ESP_LOGI(tag, "XBOX_SCAN hold controller pairing button");
    }
}

/**
 * @brief Обрабатывает жизненный цикл соединения и уведомления GAP.
 * @details Соединяет, запускает SMP и ждёт ENC_CHANGE перед discovery; разрыв отпускает кнопки. Повторное pairing удаляет bond только проверенного текущего peer.
 * @param event Событие SDK; указатель действителен только внутри callback.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 * @return 0 либо код политики BLE_GAP_REPEAT_PAIRING_* для повторного сопряжения.
 */
int gapEvent(ble_gap_event* event, void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (state.connection == BLE_HS_CONN_HANDLE_NONE && state.status.load() == StateCode::Scanning && xboxAdvertisement(state, event->disc)) {
            const auto address = event->disc.addr;
            stopScanRetry(state);
            state.status.store(StateCode::Connecting);
            ble_gap_disc_cancel();
            ble_gap_conn_params parameters{};
            parameters.scan_itvl = 32;
            parameters.scan_window = 32;
            parameters.itvl_min = 6;
            parameters.itvl_max = 12;
            parameters.latency = 0;
            parameters.supervision_timeout = 300;
            const int result = ble_gap_connect(state.ownAddressType, &address, 10000, &parameters, gapEvent, &state);
            if (result) { state.status.store(StateCode::Scanning); startScan(state); }
            else ESP_LOGI(tag, "XBOX_CONNECT BLE HID candidate");
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (state.connection == BLE_HS_CONN_HANDLE_NONE && state.status.load() != StateCode::Connecting) startScan(state);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) { releaseButtons(state); startScan(state); return 0; }
        state.connection = event->connect.conn_handle;
        stopScanRetry(state);
        state.discovering = false;
        state.reports = 0;
        state.status.store(StateCode::Pairing);
        {
            const int result = ble_gap_security_initiate(state.connection);
            // NimBLE EALREADY означает, что SMP ещё выполняется; только callback
            // ENC_CHANGE, а не этот код возврата, разрешает discovery.
            if (result && result != BLE_HS_EALREADY) reject(state, "Pairing start", result);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle == state.connection) {
            ble_gap_conn_desc connection{};
            const int found = ble_gap_conn_find(state.connection, &connection);
            // SDK обновляет sec_state до успешного ENC_CHANGE; key_size задан в байтах.
            // Минимум стека FreeRTOS ESP-IDF также измеряется в байтах, а не словах.
            ESP_LOGI(tag, "XBOX_SECURITY status=%d lookup=%d encrypted=%u bonded=%u key_size=%u host_min_free=%u host_stack=%u",
                event->enc_change.status, found, static_cast<unsigned>(connection.sec_state.encrypted),
                static_cast<unsigned>(connection.sec_state.bonded), static_cast<unsigned>(connection.sec_state.key_size),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned>(CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE));
            if (event->enc_change.status) reject(state, "Pairing/encryption", event->enc_change.status);
            else if (found) reject(state, "Encryption state lookup", found);
            else if (!connection.sec_state.encrypted) reject(state, "Link not encrypted", BLE_HS_EENCRYPT);
            else if (connection.sec_state.key_size != BLE_SM_PAIR_KEY_SZ_MAX)
                reject(state, "Encryption key size", BLE_HS_EENCRYPT_KEY_SZ);
            else if (!state.connected.load()) discoverService(state);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(tag, "XBOX_DISCONNECTED reason=%d", event->disconnect.reason);
        state.connection = BLE_HS_CONN_HANDLE_NONE;
        state.discovering = false;
        releaseButtons(state);
        startScan(state);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.conn_handle == state.connection) publishReport(state, event->notify_rx.attr_handle, event->notify_rx.om);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        {
            if (event->repeat_pairing.conn_handle != state.connection) return BLE_GAP_REPEAT_PAIRING_IGNORE;
            ble_gap_conn_desc connection{};
            const int found = ble_gap_conn_find(event->repeat_pairing.conn_handle, &connection);
            if (found) {
                reject(state, "Repeat pairing peer lookup", found);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            const int deleted = ble_store_util_delete_peer(&connection.peer_id_addr);
            if (deleted) {
                reject(state, "Repeat pairing peer bond deletion", deleted);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            // SDK перечитывает bond именно этого проверенного peer перед продолжением SMP.
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
    default: return 0;
    }
}

/**
 * @brief Сбрасывает опубликованный ввод после сброса NimBLE host.
 * @details Отменяет callout, очищает идентификатор связи и регистрирует причину; активный объект имеет время жизни прошивки.
 * @param reason Числовая причина события/повторной попытки для журнала.
 */
void onReset(int reason) {
    if (!active) return;
    stopScanRetry(*active);
    active->scanRetryDelayMs = 250;
    releaseButtons(*active);
    active->connection = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGW(tag, "NimBLE host reset reason=%d", reason);
}

/**
 * @brief После синхронизации host определяет локальный тип адреса и начинает поиск.
 * @details Ошибка определения адреса оставляет статус Failed, не подделывая соединение.
 */
void onSync() {
    if (!active) return;
    const auto result = ble_hs_id_infer_auto(0, &active->ownAddressType);
    if (result) { active->status.store(StateCode::Failed); ESP_LOGW(tag, "BLE address rc=%d", result); return; }
    startScan(*active);
}

/**
 * @brief Выполняет цикл событий NimBLE в выделенной задаче SDK.
 * @details После остановки host отменяет повтор сканирования и завершает FreeRTOS-обвязку; сам не владеет ROM или эмулятором.
 */
void hostTask(void*) {
    nimble_port_run();
    if (active) stopScanRetry(*active);
    nimble_port_freertos_deinit();
}

/**
 * @brief Асинхронно запускает NVS, транспорт C6 и NimBLE.
 * @details Не стирает NVS при ошибке. Учитывает legacy boot-enabled C6 и более новый lifecycle RPC, регистрирует callback/хранилище bond и задачу host. Ошибка фиксирует Failed и удаляет задачу настройки.
 * @param context Заимствованный указатель на долговечное состояние, переданное SDK при регистрации.
 */
void setupTask(void* context) {
    auto& state = *static_cast<BluetoothState*>(context);
    const auto nvs = nvs_flash_init();
    if (nvs != ESP_OK) {
        ESP_LOGE(tag, "NVS init: %s; existing NVS was not erased", esp_err_to_name(nvs));
        state.status.store(StateCode::Failed);
        vTaskDelete(nullptr);
        return;
    }
    nvs_handle_t saved = 0;
    if (nvs_open("nes_xbox", NVS_READONLY, &saved) == ESP_OK) {
        std::size_t size = sizeof(state.preferredPeer);
        state.hasPreferredPeer = nvs_get_blob(saved, "peer", &state.preferredPeer, &size) == ESP_OK && size == sizeof(state.preferredPeer);
        nvs_close(saved);
    }
    const auto eventLoop = esp_event_loop_create_default();
    if (eventLoop != ESP_OK && eventLoop != ESP_ERR_INVALID_STATE) {
        state.status.store(StateCode::Failed);
        vTaskDelete(nullptr);
        return;
    }
    // Проект исключает ранний конструктор Hosted. Запускаем транспорт здесь,
    // когда main_task уже вернула стартовую SRAM стека аллокатору.
    const auto initialized = esp_hosted_init();
    if (initialized != ESP_OK) {
        ESP_LOGE(tag, "C6 initialization rc=%d", initialized);
        state.status.store(StateCode::Failed);
        vTaskDelete(nullptr);
        return;
    }
    const auto connected = esp_hosted_connect_to_slave();
    if (connected != ESP_OK) {
        ESP_LOGE(tag, "C6 transport rc=%d", connected);
        state.status.store(StateCode::Failed);
        vTaskDelete(nullptr);
        return;
    }
    esp_hosted_coprocessor_fwver_t firmware{};
    const bool knownVersion = esp_hosted_get_coprocessor_fwversion(&firmware) == ESP_OK;
    if (knownVersion) ESP_LOGI(tag, "C6_FIRMWARE %u.%u.%u", static_cast<unsigned>(firmware.major1), static_cast<unsigned>(firmware.minor1), static_cast<unsigned>(firmware.patch1));
    // До версии 2.5.2 BT-контроллер slave активен при загрузке. Более новые версии
    // предоставляют lifecycle RPC и ждут запроса host на включение.
    if (knownVersion && (firmware.major1 > 2 || (firmware.major1 == 2 &&
        (firmware.minor1 > 5 || (firmware.minor1 == 5 && firmware.patch1 >= 2))))) {
        const auto init = esp_hosted_bt_controller_init();
        const auto enable = esp_hosted_bt_controller_enable();
        if (init != ESP_OK || enable != ESP_OK) ESP_LOGW(tag, "C6 BT lifecycle init=%d enable=%d", init, enable);
    } else ESP_LOGI(tag, "Using legacy boot-enabled C6 BLE controller");

    const auto result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(tag, "NimBLE init rc=%d", result);
        state.status.store(StateCode::Failed);
        vTaskDelete(nullptr);
        return;
    }
    auto* hostEvents = nimble_port_get_dflt_eventq();
    state.scanRetryAvailable = hostEvents && ble_npl_callout_init(&state.scanRetry, hostEvents, retryScan, &state) == 0;
    if (!state.scanRetryAvailable) ESP_LOGW(tag, "Scan backoff unavailable; automatic scan retry disabled");
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
#if defined(CONFIG_BT_NIMBLE_CRYPTO_STACK_MBEDTLS) && CONFIG_BT_NIMBLE_CRYPTO_STACK_MBEDTLS
    constexpr const char* cryptoBackend = "mbedtls";
#else
    constexpr const char* cryptoBackend = "tinycrypt";
#endif
    ESP_LOGI(tag, "BLE_SECURITY backend=%s sc=%u bonding=%u mitm=%u io_cap=%u our_keys=0x%x peer_keys=0x%x host_stack=%u",
        cryptoBackend, static_cast<unsigned>(ble_hs_cfg.sm_sc), static_cast<unsigned>(ble_hs_cfg.sm_bonding),
        static_cast<unsigned>(ble_hs_cfg.sm_mitm), static_cast<unsigned>(ble_hs_cfg.sm_io_cap),
        static_cast<unsigned>(ble_hs_cfg.sm_our_key_dist), static_cast<unsigned>(ble_hs_cfg.sm_their_key_dist),
        static_cast<unsigned>(CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE));
    ble_store_config_init();
    nimble_port_freertos_init(hostTask);
    vTaskDelete(nullptr);
}
}

/**
 * @brief Однократно создаёт состояние и задачу настройки BLE-клиента.
 * @details Состояние располагается во внутренней RAM на время прошивки. Успех означает запуск задачи, а не уже установленное соединение.
 * @return true при существующем/запущенном клиенте, false при нехватке ресурсов.
 */
bool BluetoothInput::initialize() noexcept {
    if (state_) return true;
    if (active) return false;
    auto* memory = heap_caps_calloc(1, sizeof(BluetoothState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) return false;
    state_ = new (memory) BluetoothState{};
    active = state_;
    if (xTaskCreatePinnedToCore(setupTask, "xbox_setup", 6144, state_, 4, nullptr, 1) != pdPASS) {
        active = nullptr;
        state_->~BluetoothState();
        heap_caps_free(state_);
        state_ = nullptr;
        return false;
    }
    return true;
}

/**
 * @brief Получает опубликованную маску игровых кнопок NES.
 * @details Атомарное чтение не обращается к BLE/NVS; разрыв связи предварительно отпускает кнопки.
 * @return Биты A1/B2/Select4/Start8/Up16/Down32/Left64/Right128.
 */
std::uint8_t BluetoothInput::buttons() const noexcept {
    return state_ ? state_->buttons.load(std::memory_order_acquire) : 0;
}

/**
 * @brief Проверяет готовность хотя бы одной игровой HID-подписки.
 * @details Радиосоединение до завершения discovery/подписок ещё не считается готовым геймпадом.
 * @return true для готового игрового ввода, иначе false.
 */
bool BluetoothInput::connected() const noexcept {
    return state_ && state_->connected.load(std::memory_order_acquire);
}

/**
 * @brief Потребляет один опубликованный фронт Guide/Home или Select+Start.
 * @details Атомарный exchange очищает событие; удержание не создаёт повторных фронтов само по себе.
 * @return true при новом запросе меню.
 */
bool BluetoothInput::menuRequested() noexcept {
    return state_ && state_->menu.exchange(false, std::memory_order_acq_rel);
}

/**
 * @brief Возвращает короткое имя текущей фазы BLE-клиента.
 * @details Статическая строка не передаёт владение; это диагностический статус, не доказательство радиокачества.
 * @return Статическая строка состояния либо BLUETOOTH OFF до initialize.
 */
const char* BluetoothInput::statusText() const noexcept {
    if (!state_) return "BLUETOOTH OFF";
    switch (state_->status.load(std::memory_order_relaxed)) {
    case StateCode::Starting: return "C6 STARTING";
    case StateCode::Scanning: return "XBOX: POWER ON / PAIR";
    case StateCode::Connecting: return "XBOX CONNECTING";
    case StateCode::Pairing: return "XBOX PAIRING";
    case StateCode::Discovering: return "XBOX HID DISCOVERY";
    case StateCode::Ready: return "XBOX CONNECTED";
    case StateCode::Failed: return "BLUETOOTH ERROR: SEE SERIAL";
    }
    return "BLUETOOTH OFF";
}

} // пространство имён nes::platform
