/**
 * @file BufferedUart.h
 * @brief Ограниченный UART0/VFS proxy: пачки TX, точный CRLF-префикс, завершение без рекурсивного лога.
 */
#pragma once

// Этот startup-hook не меняет полей App/Board/Core и layout публичных объектов.
// Добавление комментариев меняет номера __LINE__; старые binary/source-хеши остаются историческими.
namespace nes::app::console {
/**
 * @brief Однократно подключает проверенный UART0/VFS proxy после RTOS. Неподдерживаемый SDK/состояние оставляет исходный путь без небезопасной подмены.
 */
void initializeBufferedUart() noexcept;
}
#define NES_INITIALIZE_BUFFERED_UART() ::nes::app::console::initializeBufferedUart()
