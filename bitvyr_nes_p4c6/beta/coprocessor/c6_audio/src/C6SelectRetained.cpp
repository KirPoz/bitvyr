#include "C6SelectRetained.h"
#include "C6BootMeta.h"
#include "esp_app_desc.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
// Только SDK HAL header использует GNU typeof; own code остаётся strict C++17.
#ifdef typeof
#error "Unexpected outer typeof macro; local compatibility scope must be isolated"
#endif
#define typeof __typeof__
#include "esp_secure_boot.h"
#undef typeof

// SDK anti-rollback может eraseAPP при отказе: для этого фиксированного selector путь запрещён.
#ifdef CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
#error "Retained-a09 selector requires unchanged anti-rollback-disabled SDK"
#endif
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Retained-a09 selector requires existing undefined/FFFFFFFF OTA state profile"
#endif
namespace {
/** @brief Читает только fixed BootMeta328, не arbitrary addresses; один Worker исключает reentry. */
bool read(void*,std::uint8_t*out,std::size_t n)noexcept{std::size_t written=0;return c6BootMeta(out,n,written)==ESP_OK&&written==nes::c6rollback::metadataBytes;}
/** @brief Единственная persistent SDK операция фиксирует OTA0; не вызывает restart. */
bool selectZero(void*)noexcept{
    const auto*part=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
    return part&&part->address==0x10000U&&part->size==0x180000U&&esp_ota_set_boot_partition(part)==ESP_OK;
}
}
nes::c6rollback::Code c6SelectRetainedA09(const std::uint8_t*request,std::size_t bytes,
    std::uint8_t*reply,std::size_t capacity,std::size_t&written)noexcept{
    if(!nes::c6rollback::validRequest(request,bytes))return nes::c6rollback::Code::BadRequest;
    if(esp_secure_boot_enabled()||esp_efuse_is_flash_encryption_enabled()||
       esp_efuse_read_field_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD)||
       esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE)||esp_app_get_description()->secure_version)return nes::c6rollback::Code::Security;
    return nes::c6rollback::select(request,bytes,reply,capacity,written,{nullptr,read,selectZero});
}
