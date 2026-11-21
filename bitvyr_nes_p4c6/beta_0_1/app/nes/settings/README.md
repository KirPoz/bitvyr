# Настройки консоли

[ConsoleSettings.h](ConsoleSettings.h) содержит переносимые volume/mute,
swap A/B, FPS и Auto/NTSC/PAL. [SettingsStore](SettingsStore.h) — Main-only NVS
адаптер; Core не зависит от persistence.

Формат 16 байт задаёт signature/version/reserved/CRC32, не raw C++ структуру.
Неверный payload даёт безопасный volume 1/mute. Пользовательский NVS не
переписывается ради defaults или документации.

Сохранение явно применяется, не пишется каждый кадр. Ошибка не разрешает
erase чужих namespaces/BLE bonds. Регион меняется через restart; forcedNTSC
ускоряет PAL. Проверки: codec/CRC/ranges и fake NVS save/reload/failure/lifetime.

[Архитектура](../../../docs/02-architecture.md) · [Сборка и использование](../../../docs/03-guide.md).
