/** @file esp_heap_caps.h
 * @brief Узкие HOST allocator/telemetry declarations; capabilities являются только метками fixture.
 */
#pragma once
#include <cstddef>
constexpr unsigned MALLOC_CAP_INTERNAL=1,MALLOC_CAP_DMA=2,MALLOC_CAP_8BIT=4,MALLOC_CAP_SPIRAM=8;
/**
 * @brief Выделяет HOST-блок с заданными выравниванием, размером в байтах и capability mask.
 * @details Первые два аргумента задают alignment/bytes, третий — mock flags.
 * Успешный указатель принадлежит вызывающему до heap_caps_free; nullptr означает
 * отказ _aligned_malloc или заполнение 64 записей. alignment обязан удовлетворять
 * _aligned_malloc после повышения до sizeof(void*).
 */
void*heap_caps_aligned_alloc(std::size_t,std::size_t,unsigned);
/**
 * @brief Освобождает точное начало зарегистрированного HOST-блока.
 * @details nullptr допустим. Неизвестный, внутренний или уже освобождённый адрес
 * вызывает abort; после успеха память и регистрация больше не действительны.
 */
void heap_caps_free(void*);
/**
 * @brief Выделяет заданное число байтов с выравниванием 8 и переданными mock flags.
 * @details Результат принадлежит вызывающему до heap_caps_free; nullptr означает
 * отказ HOST allocator или заполнение реестра, без изменения существующих блоков.
 */
void*heap_caps_malloc(std::size_t,unsigned);

/** @brief Возвращает постоянный free budget 1 MiB; аргумент flags игнорируется, живые блоки не учитываются. */
std::size_t heap_caps_get_free_size(unsigned);
/** @brief Возвращает постоянный largest-block budget 1 MiB; flags и реальная фрагментация не учитываются. */
std::size_t heap_caps_get_largest_free_block(unsigned);
/** @brief Возвращает постоянный minimum-free budget 1 MiB; flags и история выделений не учитываются. */
std::size_t heap_caps_get_minimum_free_size(unsigned);
