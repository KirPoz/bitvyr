/** @file esp_memory_utils.h
 * @brief Запросы mock capabilities по адресу внутри зарегистрированного HOST-блока, без разыменования.
 */
#pragma once
/** @brief Возвращает INTERNAL bit блока, содержащего заимствованный адрес; nullptr и неизвестный адрес дают false. */
bool esp_ptr_internal(const void*);
/** @brief Возвращает SPIRAM bit блока по заимствованному адресу; nullptr и неизвестный адрес дают false, физическая PSRAM не проверяется. */
bool esp_ptr_external_ram(const void*);
/** @brief Возвращает DMA bit блока по заимствованному адресу; nullptr и неизвестный адрес дают false, реальный DMA не проверяется. */
bool esp_ptr_dma_capable(const void*);
/** @brief Возвращает 8BIT bit блока по заимствованному адресу; nullptr и неизвестный адрес дают false, lifetime не продлевается. */
bool esp_ptr_byte_accessible(const void*);
