/**
 * @file SettingsStore.h
 * @brief Адаптер NVS настроек без автоматического стирания flash при ошибках.
 */
#pragma once

#include "ConsoleSettings.h"

namespace nes::app {

/** @brief Main-адаптер NVS для полного версионированного blob; ошибки не стирают чужие namespaces и BLE bonds. */
class SettingsStore final {
public:
    /**
     * @brief Читает версионированный NVS blob и проверяет CRC/диапазоны. При отказе остаются безопасные тихие настройки.
     * @param settings Выход настроек; до чтения установлен тихий default, сохраняемый при отказе.
     */
    bool load(ConsoleSettings& settings) noexcept;
    /**
     * @brief Сохраняет только валидный полный blob настроек и подтверждает NVS commit. Ошибка не объявляется успешным сохранением.
     * @param settings Неизменяемые настройки для проверки, кодирования и сохранения собственного NVS-ключа.
     */
    bool save(const ConsoleSettings& settings) noexcept;
    /**
     * @brief Возвращает заимствованный error-буфер хранилища до следующей операции.
     */
    const char* errorText() const noexcept { return error_; }
private:
    /**
     * @brief Сохраняет ограниченное описание NVS-ошибки для последующего вывода Main.
     * @param operation Заимствованное имя неуспешной операции NVS.
     * @param code Код ошибки NVS/ESP-IDF, преобразуемый в ограниченное сообщение.
     */
    void error(const char* operation, int code) noexcept;
    char error_[96]{};
};

} // namespace nes::app
