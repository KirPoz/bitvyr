/** @file C6ProbeMain.cpp @brief Собственный entrypoint поверх неизменённого Hosted slave. */
#include "esp_err.h"
#include "esp_hosted_coprocessor.h"
#include "nvs_flash.h"

extern "C" esp_err_t c6_probe_endpoint_init() noexcept;

/** @brief Запускает Hosted и private endpoint; ошибки NVS никогда не вызывают erase. */
extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_hosted_coprocessor_init());
    ESP_ERROR_CHECK(c6_probe_endpoint_init());
}
