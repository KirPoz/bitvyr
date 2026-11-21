/** @file main.cpp
 * @brief Единственный долгоживущий владелец консоли Bitvyr.
 * @details app_main вызывается ESP-IDF на CPU0 после инициализации RTOS.
 */
#include "NesApp.h"

/**
 * @brief Инициализирует консоль и передаёт CPU0 основному циклу приложения.
 * @details Статический объект живёт до reboot: Worker и PPA могут заимствовать
 * его ресурсы, поэтому возврат из run() не уничтожает активный контекст.
 */
extern "C" void app_main() {
    static nes::app::NesApp app;
    app.run();
}
