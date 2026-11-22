/** @file nvs.h
 * @brief Узкий контракт SDK NVS для проверки настоящего SettingsStore без платы.
 */
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
using nvs_handle_t = std::uint32_t;
/** @brief Режим доступа, проверяемый fake при каждом чтении/записи blob. */
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=0x1102;
constexpr esp_err_t ESP_ERR_NVS_NO_FREE_PAGES=0x110d, ESP_ERR_NVS_NEW_VERSION_FOUND=0x1110;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH=0x110c;
/** @brief Стабильное имя fake-ошибки для errorText адаптера. */
const char* esp_err_to_name(esp_err_t error);
/** @brief Открывает namespace в заданном режиме; handle заимствуется до nvs_close. */
esp_err_t nvs_open(const char* space,nvs_open_mode_t mode,nvs_handle_t* handle);
/** @brief Читает blob; size — входная вместимость/выходной размер в байтах. */
esp_err_t nvs_get_blob(nvs_handle_t handle,const char* key,void* data,std::size_t* size);
/** @brief Копирует size байтов в pending; видимость требует отдельного commit. */
esp_err_t nvs_set_blob(nvs_handle_t handle,const char* key,const void* data,std::size_t size);
/** @brief Фиксирует pending при отсутствии имитируемого отказа. */
esp_err_t nvs_commit(nvs_handle_t handle);
/** @brief Возвращает единственное заимствование handle и обновляет счётчик close. */
void nvs_close(nvs_handle_t handle);
