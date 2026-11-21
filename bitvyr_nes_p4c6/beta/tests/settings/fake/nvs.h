/** @file nvs.h
 * @brief Узкий интерфейс SDK NVS для проверки настоящего SettingsStore в native fake.
 */
#pragma once
#include <cstddef>
#include <cstdint>
/** @brief Минимальный тип кода ошибки SDK для native fake без зависимости от ESP-IDF. */
using esp_err_t = int;
/** @brief Непрозрачный идентификатор fake NVS; реализация теста выдаёт единственное значение 42. */
using nvs_handle_t = std::uint32_t;
/** @brief Режим заимствования fake NVS; чтение требует READONLY, запись требует READWRITE. */
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=0x1102;
constexpr esp_err_t ESP_ERR_NVS_NO_FREE_PAGES=0x110d, ESP_ERR_NVS_NEW_VERSION_FOUND=0x1110;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH=0x110c;
/**
 * @brief Возвращает постоянную строку FAKE_ERROR для диагностик SettingsStore.
 * @param error Код ошибки; fake намеренно игнорирует его значение.
 * @return Заимствованная статическая строка, не требующая освобождения.
 */
const char* esp_err_to_name(esp_err_t error);
/**
 * @brief Открывает единственное заимствование fake NVS для namespace nes_console.
 * @details Первый аргумент задаёт namespace, второй — режим доступа, третий обязан
 * указывать на выходной handle. При успехе он получает 42 до вызова nvs_close;
 * при предписанном отказе выходной handle не изменяется. Неверный namespace
 * или повторное открытие активного handle завершают тест через assert.
 * @return ESP_OK либо независимо заданный тестом openError.
 */
esp_err_t nvs_open(const char*,nvs_open_mode_t,nvs_handle_t*);
/**
 * @brief Читает зафиксированные байты cfg_v1 через активный READONLY handle.
 * @details Аргументы задают handle, ключ, выходной буфер и указатель на его
 * вместимость в байтах. Последний аргумент возвращает фактический размер blob;
 * если места мало, буфер не меняется, размер получает требуемую вместимость.
 * Предписанный getError возвращается до копирования и изменения размера.
 * @return ESP_OK, ESP_ERR_NVS_INVALID_LENGTH либо заданный getError.
 */
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,std::size_t*);
/**
 * @brief Копирует ровно 16 байтов cfg_v1 в pending через активный READWRITE handle.
 * @details Аргументы задают handle, ключ, заимствованный входной буфер и его
 * размер; assert проверяет ключ cfg_v1, режим и размер.
 * Копия принадлежит fake, а зафиксированный blob меняется только в nvs_commit.
 * При предписанном setError pending остаётся прежним.
 * @return ESP_OK либо заданный setError.
 */
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,std::size_t);
/**
 * @brief Делает pending видимым как зафиксированный blob активного handle.
 * @details Успех копирует pending в blob; предписанный commitError оставляет
 * blob прежним. Счётчик commit обновляется в обоих случаях, handle остаётся открыт.
 * @return ESP_OK либо заданный commitError.
 */
esp_err_t nvs_commit(nvs_handle_t);
/**
 * @brief Завершает единственное заимствование активного handle и считает close.
 * @details assert требует действующий handle 42; после вызова alive=false,
 * поэтому повторное закрытие или использование handle завершают тест ошибкой.
 */
void nvs_close(nvs_handle_t);
