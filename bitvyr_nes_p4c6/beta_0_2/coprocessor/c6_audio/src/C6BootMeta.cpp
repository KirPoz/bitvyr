#include "C6BootMeta.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <array>
#include <cstring>
namespace {
alignas(4) std::array<std::uint8_t,512> block;
/** @brief Пишет публичное32-битное LE поле, не экспортируя raw memory. */
void put(std::uint8_t* out,std::uint32_t value) noexcept {for(unsigned i=0;i<4;++i)out[i]=std::uint8_t(value>>(i*8U));}
/** @brief Хеширует ровно compile-time выбранный диапазон через SDK read; произвольные адреса не приходят с wire. */
bool hashRange(std::uint32_t offset,std::uint32_t bytes,std::uint8_t* digest,const esp_partition_t* partition=nullptr) noexcept {
    psa_hash_operation_t operation=PSA_HASH_OPERATION_INIT;bool ok=psa_hash_setup(&operation,PSA_ALG_SHA_256)==PSA_SUCCESS;
    for(std::uint32_t at=0;ok&&at<bytes;at+=block.size()){
        const auto count=bytes-at<block.size()?bytes-at:block.size();
        const auto rc=partition?esp_partition_read(partition,at,block.data(),count):esp_flash_read(esp_flash_default_chip,block.data(),offset+at,count);
        ok=rc==ESP_OK&&psa_hash_update(&operation,block.data(),count)==PSA_SUCCESS;
        if((at&0xffffU)==0)vTaskDelay(1);
    }
    std::size_t written=0;if(ok)ok=psa_hash_finish(&operation,digest,32,&written)==PSA_SUCCESS&&written==32;
    (void)psa_hash_abort(&operation);return ok;
}
/** @brief Читает little-endian OTA sequence/state после bounded512Б SDK read. */
std::uint32_t get(const std::uint8_t* p) noexcept {return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8U)|(std::uint32_t(p[2])<<16U)|(std::uint32_t(p[3])<<24U);}
/** @brief CRC только четырёх публичных seq bytes по исходной bootloader семантике. */
std::uint32_t seqCrc(const std::uint8_t* p) noexcept {std::uint32_t v=0;for(unsigned i=0;i<4;++i){v^=p[i];for(unsigned j=0;j<8;++j)v=(v>>1U)^(0xedb88320U&(0U-(v&1U)));}return ~v;}
}
/** @brief Выдаёт bounded публичную boot identity; flash ranges фиксированы кодом, не wire адресом. */
int c6BootMeta(std::uint8_t* out,std::size_t capacity,std::size_t& written) noexcept {
    written=0;if(!out||capacity<328)return ESP_ERR_INVALID_SIZE;
    const auto* running=esp_ota_get_running_partition();const auto* next=esp_ota_get_next_update_partition(nullptr);const auto* boot=esp_ota_get_boot_partition();
    const auto* zero=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
    const auto* one=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,nullptr);
    if(!running||!next||!boot||!zero||!one||zero->address!=0x10000||one->address!=0x190000||zero->size!=0x180000||one->size!=0x180000||next->address==running->address)return ESP_ERR_INVALID_STATE;
    std::uint32_t flash=0;const auto rc=esp_flash_get_size(esp_flash_default_chip,&flash);if(rc!=ESP_OK||flash!=0x400000)return ESP_ERR_INVALID_STATE;
    std::memset(out,0,328);put(out,1);put(out+4,1);put(out+8,running->address);put(out+12,running->size);put(out+16,next->address);put(out+20,next->size);put(out+24,boot->address);put(out+28,boot->size);put(out+32,flash);put(out+36,esp_efuse_read_secure_version());
    esp_chip_info_t chip{};esp_chip_info(&chip);put(out+40,chip.revision);put(out+44,esp_clk_cpu_freq());std::memcpy(out+48,esp_app_get_description()->app_elf_sha256,32);
    if(psa_crypto_init()!=PSA_SUCCESS||!hashRange(0,0x8000,out+80)||!hashRange(0x8000,0x1000,out+112)||!hashRange(0xd000,0x2000,out+144)||!hashRange(zero->address,zero->size,out+176,zero)||!hashRange(one->address,one->size,out+208,one))return ESP_FAIL;
    unsigned flags=1;if(esp_partition_get_sha256(running,out+240)==ESP_OK)flags|=2;if(esp_partition_get_sha256(next,out+272)==ESP_OK)flags|=4;put(out+4,flags);
    for(unsigned i=0;i<2;++i){if(esp_flash_read(esp_flash_default_chip,block.data(),0xd000+i*0x1000,512)!=ESP_OK)return ESP_FAIL;put(out+304+i*12,get(block.data()));put(out+308+i*12,get(block.data()+24));put(out+312+i*12,seqCrc(block.data())==get(block.data()+28));}
    written=328;return ESP_OK;
}
