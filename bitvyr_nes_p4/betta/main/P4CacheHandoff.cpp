/**
 * @file P4CacheHandoff.cpp
 * @brief Согласует внутренний ICache при переходе из bootloader в приложение P4.
 * @details Локальный адаптер раннего старта для закреплённого ESP-IDF6.1.
 * Код исполняется из HP SPM, отсутствующего в загруженном bootloader, и использует
 * сильный ROM backend того же invalidate, что применяет cache_ll ESP32-P4.
 * Эта private startup точка нужна раньше доступного прикладного esp_cache_msync.
 * DCache не инвалидируется: loader уже завершил writeback загруженного IRAM,
 * а BSS и стек приложения могут содержать новые dirty-данные.
 */
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp32p4/rom/cache.h"
#include <cstdint>

extern "C" {
extern char _iram_start;
extern char _iram_end;
esp_err_t __real_spi_flash_enable_high_performance_mode(void);
}


/**
 * @brief Очищает устаревшие инструкции bootloader до первого раннего HPM вызова.
 * @return Прежний результат настоящей SDK HPM-функции после успешного invalidate.
 * @details Стек/GP и BSS уже готовы; функция и её константы не зависят от HP ICache.
 * При ошибке запрещено возвращаться в startup: его caller не проверяет код ошибки.
 * Используется только ROM reset, без исполняемых из HP SRAM abort/printf.
 */
extern "C" SPM_IRAM_ATTR esp_err_t __wrap_spi_flash_enable_high_performance_mode(void) {
    constexpr std::uintptr_t line = 64;
    const auto begin = reinterpret_cast<std::uintptr_t>(&_iram_start) & ~(line - 1U);
    const auto end = (reinterpret_cast<std::uintptr_t>(&_iram_end) + line - 1U) & ~(line - 1U);
    if (end <= begin || end - begin > 0xc0000U) {
        esp_rom_software_reset_system();
        __builtin_unreachable();
    }
    asm volatile("fence rw,rw" ::: "memory");
    // До start_other_core исполняется только CPU0; CPU1 ещё удерживается в reset.
    const int result = Cache_Invalidate_Addr(CACHE_MAP_L1_ICACHE_0,
                                             static_cast<std::uint32_t>(begin),
                                             static_cast<std::uint32_t>(end - begin));
    asm volatile("fence.i" ::: "memory");
    if (result != 0) {
        esp_rom_software_reset_system();
        __builtin_unreachable();
    }
    return __real_spi_flash_enable_high_performance_mode();
}

