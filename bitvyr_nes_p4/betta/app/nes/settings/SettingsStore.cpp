/**
 * @file SettingsStore.cpp
 * @brief Адаптер NVS настроек без автоматического стирания flash при ошибках.
 */
#include "SettingsStore.h"

#include <cstdio>
#include "nvs.h"
#include "nvs_flash.h"

namespace nes::app {
namespace {
constexpr const char* nameSpace = "nes_console";
constexpr const char* key = "cfg_v1";
}

/**
 * @brief Сохраняет ограниченное описание NVS-ошибки для последующего вывода Main.
 * @param operation Заимствованное имя неуспешной операции NVS.
 * @param code Код ошибки NVS/ESP-IDF, преобразуемый в ограниченное сообщение.
 */
void SettingsStore::error(const char* operation, int code) noexcept {
    std::snprintf(error_, sizeof(error_), "%s: %s", operation, esp_err_to_name(code));
}

/**
 * @brief Читает версионированный NVS blob и проверяет CRC/диапазоны. При отказе остаются безопасные тихие настройки.
 * @param settings Выход настроек; до чтения установлен тихий default, сохраняемый при отказе.
 */
bool SettingsStore::load(ConsoleSettings& settings) noexcept {
    settings = ConsoleSettings{};
    error_[0] = 0;
    auto result = nvs_flash_init(); // Идемпотентно: ошибки не разрешают стирать чужой namespace/ключи сопряжения.
    if (result != ESP_OK) { error("NVS init", result); return false; }
    nvs_handle_t handle{};
    result = nvs_open(nameSpace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) { error("Settings open", result); return false; }
    EncodedSettings bytes{};
    std::size_t size = bytes.size();
    result = nvs_get_blob(handle, key, bytes.data(), &size);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) { error("Settings read", result); return false; }
    if (!decodeSettings(bytes.data(), size, settings)) {
        std::snprintf(error_, sizeof(error_), "Invalid settings; quiet defaults");
        return false;
    }
    return true;
}

/**
 * @brief Сохраняет только валидный полный blob настроек и подтверждает NVS commit. Ошибка не объявляется успешным сохранением.
 * @param settings Неизменяемые настройки для проверки, кодирования и сохранения собственного NVS-ключа.
 */
bool SettingsStore::save(const ConsoleSettings& settings) noexcept {
    error_[0] = 0;
    EncodedSettings bytes{};
    if (!encodeSettings(settings, bytes)) {
        std::snprintf(error_, sizeof(error_), "Invalid settings; not saved");
        return false;
    }
    auto result = nvs_flash_init();
    if (result != ESP_OK) { error("NVS init", result); return false; }
    nvs_handle_t handle{};
    result = nvs_open(nameSpace, NVS_READWRITE, &handle);
    if (result != ESP_OK) { error("Settings open", result); return false; }
    result = nvs_set_blob(handle, key, bytes.data(), bytes.size());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK) { error("Settings save", result); return false; }
    return true;
}

} // namespace nes::app
