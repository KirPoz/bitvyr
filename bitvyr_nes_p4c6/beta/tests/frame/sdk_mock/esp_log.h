/** @file esp_log.h @brief HOST logging без RTOS/SDK; arguments сохраняются для строгих warnings. */
#pragma once
#include <cstdio>
#define ESP_LOGI(tag,...) do{(void)(tag);std::printf(__VA_ARGS__);std::putchar('\n');}while(false)
