/**
 * @file main.cpp
 * @brief Точка входа прошивки и единственный долгоживущий экземпляр приложения.
 */
#include "NesApp.h"
#include "sdkconfig.h"
#include <cstdio>
#if CONFIG_IDF_TARGET_ESP32P4
extern "C" std::uint32_t nes_p4_cache_handoff_calls(void);
extern "C" int nes_p4_cache_handoff_result(void);
#endif

extern "C" 
    /**
     * @brief Точка входа ESP-IDF: создаёт единственный долгоживущий NesApp и запускает Main-цикл. Статический объект живёт до перезапуска; Worker не может заимствовать уже уничтоженное приложение.
     */
    void app_main() {
#if CONFIG_IDF_TARGET_ESP32P4
    std::printf("NES_P4_CACHE_HANDOFF calls=%u result=%d source=spm_rom_icache_only\n",
                unsigned(nes_p4_cache_handoff_calls()),nes_p4_cache_handoff_result());
#endif
    static nes::app::NesApp app;
    app.run();
}
