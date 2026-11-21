#pragma once
#include "../protocol/RawLimits.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Проверяет наличие Hosted header и widened extents ДО checksum/copy/header dereference.
 * @param data Borrowed aggregate cursor; при available<12 функция не читает data.
 * @param available Оставшиеся bytes текущего SDIO aggregate, не полная capacity.
 * @param ordinary_limit Прежний stock payload cap; увеличивается только SERIAL1/flags0.
 * @param serial_if Значение ESP_SERIAL_IF actual Hosted header, не guessed ABI struct.
 * @param len_out,offset_out Изменяются только при успехе; destination capacity фиксирована2048.
 * @return 1 для bounded packet; ошибка не выполняет checksum и не меняет output.
 * @details Header byte0 содержит low4 if_type/high4 if_num, LE16 len@2/offset@4.
 * Caller дополнительно обязан проверить sizeof(actual esp_payload_header)==12.
 */
static inline int nes_raw_rx_bounds(const uint8_t*data,size_t available,
    size_t ordinary_limit,unsigned serial_if,uint16_t*len_out,uint16_t*offset_out)
{
    if(!data||!len_out||!offset_out||available<NES_RAW_HOSTED_HEADER_BYTES)return 0;
    const uint16_t len=(uint16_t)((uint16_t)data[2]|((uint16_t)data[3]<<8));
    const uint16_t offset=(uint16_t)((uint16_t)data[4]|((uint16_t)data[5]<<8));
    const int extended=(data[0]&15U)==serial_if&&(data[0]>>4)==1U&&data[1]==0U;
    const size_t limit=extended?NES_RAW_REPLY_PACKET_BYTES:ordinary_limit;
    /* size_t сумма исключает wrap16; available и copy block проверяются отдельно. */
    const size_t total=(size_t)offset+(size_t)len;
    if(!len||(size_t)len>limit||offset!=NES_RAW_HOSTED_HEADER_BYTES||
       total>available||total>NES_RAW_RX_BLOCK_BYTES)return 0;
    *len_out=len;*offset_out=offset;return 1;
}
