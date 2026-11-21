/** @file esp_timer.h
 * @brief Детерминированные mock часы для deadline-ветвей native fixture.
 */
#pragma once
#include <cstdint>
/**
 * @brief Возвращает следующий mock тик: 101 при первом вызове, затем увеличение на 1.
 * @details Возврат соответствует типу SDK микросекунд, но walltime и FPS не
 * измеряются. Внутренний статический счётчик меняется без выделения ресурсов.
 */
std::int64_t esp_timer_get_time();
