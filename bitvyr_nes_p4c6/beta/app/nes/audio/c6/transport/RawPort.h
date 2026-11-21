#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Принимает ровно один raw SERIAL1 пакет, <=1400Б; false/0 не передаёт владение и не вызывает done.
 * @param slot Один из двух собственных слотов0/1; до done повторный вызов с этим slot запрещён.
 * @param done Вызывается ровно один раз после прекращения DMA/SDK чтения source либо его отброса.
 * @details true/1 не означает доставку peer. Все указатели/context остаются валидными до done.
 */
int nes_hosted_raw_try_tx(unsigned slot, const uint8_t* data, size_t bytes, void* context, void (*done)(void*));
/** @brief Короткий callback из Hosted RX: копирует пакет в собственное статическое хранилище; borrowed bytes нельзя сохранять. */
void nes_hosted_raw_rx(const uint8_t* data, size_t bytes);
#ifdef __cplusplus
}
#endif
