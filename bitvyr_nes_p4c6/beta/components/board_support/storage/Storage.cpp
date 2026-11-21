#include "../BoardInternal.h"

#include <cstring>
#include <limits>
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

namespace nes::platform {
namespace {
constexpr const char* tag = "nes_storage";
/**
 * @brief Читает 32-битное little-endian поле метаданных SD.
 * @details Вызывающий код обеспечивает наличие четырёх байтов в прочитанном секторе.
 * @param p Указатель на четыре доступных байта; владение не передаётся.
 * @return Беззнаковое значение.
 */
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
/**
 * @brief Читает 64-битное little-endian поле метаданных SD.
 * @details Собирает два 32-битных слова без требования выравнивания адреса.
 * @param p Указатель на восемь доступных байтов; владение не передаётся.
 * @return Беззнаковое значение.
 */
std::uint64_t le64(const std::uint8_t* p) noexcept {
    return le32(p) | (std::uint64_t(le32(p + 4)) << 32);
}
/**
 * @brief Определяет только текстовую подсказку формата boot-сектора.
 * @details Сравнивает ограниченные сигнатуры; не проверяет целостность/пригодность всей файловой системы.
 * @param sector Буфер не менее 512 байтов для одного сектора.
 * @return Статическая строка FAT12/FAT16/FAT32/exFAT/NTFS либо unknown.
 */
const char* filesystemHint(const std::uint8_t* sector) noexcept {
    if (!std::memcmp(sector + 3, "EXFAT   ", 8)) return "exFAT";
    if (!std::memcmp(sector + 3, "NTFS    ", 8)) return "NTFS";
    if (!std::memcmp(sector + 82, "FAT32   ", 8)) return "FAT32";
    if (!std::memcmp(sector + 54, "FAT16   ", 8)) return "FAT16";
    if (!std::memcmp(sector + 54, "FAT12   ", 8)) return "FAT12";
    return "unknown";
}
/**
 * @brief Читает один 512-байтный сектор для ограниченной диагностики.
 * @details Проверяет LBA по ёмкости и размеру size_t; не пишет карту и не меняет монтирование.
 * @param card Инициализированная SD-карта; объект принадлежит вызывающему коду/SDK.
 * @param lba Номер сектора, проверяемый по ёмкости карты.
 * @param sector Буфер не менее 512 байтов для одного сектора.
 * @return true при успешном чтении, иначе false с числовой ошибкой в журнале.
 */
bool readMetadataSector(sdmmc_card_t& card, std::uint64_t lba, std::uint8_t* sector) noexcept {
    if (lba >= card.csd.capacity || lba > std::numeric_limits<std::size_t>::max()) {
        ESP_LOGW(tag, "SD_METADATA lba=%llu out_of_range=1", static_cast<unsigned long long>(lba));
        return false;
    }
    const auto error = sdmmc_read_sectors(&card, sector, static_cast<std::size_t>(lba), 1);
    if (error != ESP_OK) {
        ESP_LOGW(tag, "SD_METADATA lba=%llu read_error=0x%x", static_cast<unsigned long long>(lba), unsigned(error));
        return false;
    }
    return true;
}
/**
 * @brief Журналирует сигнатуру и подсказку формата одного boot-сектора.
 * @details Использует локальный выровненный буфер; не выводит содержимое сектора, метки или идентификатор карты.
 * @param card Инициализированная SD-карта; объект принадлежит вызывающему коду/SDK.
 * @param lba Номер сектора, проверяемый по ёмкости карты.
 */
void logBootSector(sdmmc_card_t& card, std::uint64_t lba) noexcept {
    alignas(4) std::uint8_t sector[512]{};
    if (readMetadataSector(card, lba, sector))
        ESP_LOGI(tag, "SD_METADATA lba=%llu signature55aa=%u fs_hint=%s",
                 static_cast<unsigned long long>(lba), unsigned(sector[510] == 0x55 && sector[511] == 0xaa),
                 filesystemHint(sector));
}
/**
 * @brief Читает ограниченные числовые метаданные MBR/GPT.
 * @details Не сканирует каталоги: не более четырёх записей, проверенные LBA и размеры. Функция нужна только для объяснения отказа монтирования.
 * @param card Инициализированная SD-карта; объект принадлежит вызывающему коду/SDK.
 */
void logPartitions(sdmmc_card_t& card) noexcept {
    // Только метаданные: не журналируем идентификатор карты, метки разделов и сырые сектора.
    alignas(4) std::uint8_t sector[512]{};
    ESP_LOGI(tag, "SD_METADATA sectors=%llu sector_bytes=%u sdk_exfat=0 sdk_gpt=0",
             static_cast<unsigned long long>(card.csd.capacity), unsigned(card.csd.sector_size));
    if (card.csd.sector_size != sizeof(sector) || !readMetadataSector(card, 0, sector)) return;
    const bool signature = sector[510] == 0x55 && sector[511] == 0xaa;
    const char* hint = filesystemHint(sector);
    ESP_LOGI(tag, "SD_METADATA lba=0 signature55aa=%u fs_hint=%s", unsigned(signature), hint);
    if (!signature || std::strcmp(hint, "unknown")) return;
    /**
     * @brief Ограниченная запись MBR для read-only диагностики: тип, начальный LBA и число секторов.
     */
    struct Partition { std::uint8_t type; std::uint32_t start, count; } partitions[4]{};
    bool protective = false;
    for (unsigned i = 0; i < 4; ++i) {
        const auto* entry = sector + 446 + 16 * i;
        partitions[i] = {entry[4], le32(entry + 8), le32(entry + 12)};
        const auto& p = partitions[i];
        if (!p.type) continue;
        ESP_LOGI(tag, "SD_METADATA mbr_entry=%u type=0x%02x start_lba=%lu sectors=%lu", i,
                 unsigned(p.type), static_cast<unsigned long>(p.start), static_cast<unsigned long>(p.count));
        protective |= p.type == 0xee;
    }
    if (!protective) {
        for (const auto& p : partitions) if (p.type && p.start && p.count) logBootSector(card, p.start);
        return;
    }
    if (!readMetadataSector(card, 1, sector)) return;
    const bool gpt = !std::memcmp(sector, "EFI PART", 8);
    ESP_LOGI(tag, "SD_METADATA protective_mbr=1 gpt_signature=%u", unsigned(gpt));
    if (!gpt) return;
    const auto table = le64(sector + 72);
    const auto count = le32(sector + 80), size = le32(sector + 84);
    ESP_LOGI(tag, "SD_METADATA gpt_table_lba=%llu entries=%lu entry_bytes=%lu scan_limit=4",
             static_cast<unsigned long long>(table), static_cast<unsigned long>(count), static_cast<unsigned long>(size));
    // Для диагностики достаточно четырёх ограниченных записей; каталоги не сканируем.
    if (!table || (size != 128 && size != 256)) return;
    for (unsigned i = 0; i < count && i < 4; ++i) {
        const auto offset = std::uint64_t(i) * size;
        if (table > std::numeric_limits<std::uint64_t>::max() - offset / 512 ||
            !readMetadataSector(card, table + offset / 512, sector)) break;
        const auto* entry = sector + offset % 512;
        bool used = false;
        for (unsigned j = 0; j < 16; ++j) used |= entry[j] != 0;
        if (!used) continue;
        const auto first = le64(entry + 32), last = le64(entry + 40);
        ESP_LOGI(tag, "SD_METADATA gpt_entry=%u start_lba=%llu end_lba=%llu", i,
                 static_cast<unsigned long long>(first), static_cast<unsigned long long>(last));
        if (first && first <= last) logBootSector(card, first);
    }
}
/**
 * @brief Повторно открывает SDMMC только для диагностики неудачного FATFS-mount.
 * @details Публичный mount уже освободил неуспешные card/slot; эта функция не регистрирует VFS повторно и освобождает свой slot после чтения.
 * @param host Штатная конфигурация SDMMC host; частоты и пины не перенастраиваются.
 * @param slot Конфигурация SDMMC slot0 текущей платы.
 */
void diagnoseMountFailure(const sdmmc_host_t& host, const sdmmc_slot_config_t& slot) noexcept {
    // Неуспешный публичный mount уже освободил свой card и slot.
    // Инициализируем один раз только для чтения, без повторной регистрации VFS.
    auto error = sdmmc_host_init();
    if (error != ESP_OK) { ESP_LOGW(tag, "SD_METADATA host_init_error=0x%x", unsigned(error)); return; }
    error = sdmmc_host_init_slot(host.slot, &slot);
    if (error == ESP_OK) {
        sdmmc_card_t card{};
        error = sdmmc_card_init(&host, &card);
        if (error == ESP_OK) logPartitions(card);
    }
    if (error != ESP_OK) ESP_LOGW(tag, "SD_METADATA init_error=0x%x", unsigned(error));
    const auto cleanup = sdmmc_host_deinit_slot(host.slot);
    if (cleanup != ESP_OK) ESP_LOGW(tag, "SD_METADATA deinit_error=0x%x", unsigned(cleanup));
}
} // пространство имён

/**
 * @brief Монтирует FATFS microSD через SDMMC slot0 без форматирования.
 * @details Использует штатный IO-MUX платы, LDO4 и 4 линии; slot1/C6 не меняет. При ESP_FAIL разрешена только read-only диагностика; частотных/пиновых экспериментов нет.
 * @param state Состояние узла, заимствованное у владельца на время вызова; дескрипторы остаются в нём.
 * @return ESP_OK либо исходная ошибка SDK; неосвобождённый LDO остаётся отмеченным в state.
 */
esp_err_t mountStorage(BoardState& state) noexcept {
    if (state.sdCard || state.sdPower) return ESP_ERR_INVALID_STATE;
    // Slot0 использует штатный IO-MUX платы, это не SDIO host контроллера C6.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sd_pwr_ctrl_ldo_config_t power{};
    power.ldo_chan_id = 4;
    auto result = sd_pwr_ctrl_new_on_chip_ldo(&power, &state.sdPower);
    if (result != ESP_OK) return result;
    host.pwr_ctrl_handle = state.sdPower;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.flags = 0;
    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = false;
    mount.max_files = 6;
    mount.allocation_unit_size = 16 * 1024;
    result = esp_vfs_fat_sdmmc_mount(Board::storagePath(), &host, &slot, &mount, &state.sdCard);
    if (result == ESP_OK) {
        ESP_LOGI(tag, "SD mounted %s capacity=%llu bytes; no format requested", Board::storagePath(),
                 static_cast<unsigned long long>(state.sdCard->csd.capacity) * state.sdCard->csd.sector_size);
    } else {
        state.sdCard = nullptr;
        // ESP_FAIL — наблюдавшийся отказ файловой системы вставленной карты.
        // Тайм-ауты протокола не запускают эксперименты с частотой/пинами.
        if (result == ESP_FAIL) diagnoseMountFailure(host, slot);
        const auto cleanup = sd_pwr_ctrl_del_on_chip_ldo(state.sdPower);
        if (cleanup == ESP_OK) state.sdPower = nullptr;
        else ESP_LOGW(tag, "SD power cleanup: %s", esp_err_to_name(cleanup));
    }
    return result;
}

} // пространство имён nes::platform
