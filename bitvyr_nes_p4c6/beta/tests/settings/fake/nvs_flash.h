/** @file nvs_flash.h
 * @brief Инициализация NVS для native fake без интерфейса стирания.
 */
#pragma once
#include "nvs.h"
/** @brief Считает попытку init и возвращает предписанный тестом результат. */
esp_err_t nvs_flash_init();
