# Аппаратный ввод

[Touch.cpp](Touch.cpp) читает GT911 в координатах 800×480.
[BluetoothInput.cpp](BluetoothInput.cpp) реализует Xbox BLE HID-over-GATT
через NimBLE/ESP-Hosted; [HidGamepad](HidGamepad.h) разбирает HID без SDK.
USB HID/Bluetooth Classic не реализованы.

Touch проверяет пять contact ID/координаты/status. Ошибка не сохраняет старое
нажатие как свежее. Async-touch имеет ровно одного I2C owner; stale снимки отвергаются.
Main получает snapshot, игровой Worker не опрашивает I2C.

BLE callbacks публикуют атомарные buttons/ready/menu, не SDK pointers. Ready
требует encryption/Report Map/HID subscription; initialize не означает связь.
Disconnect отпускает кнопки. NVS bonds не стираются массово. Проверки: host HID
и отдельные pairing/reconnect/input сценарии Xbox.

[Архитектура](../../../docs/02-architecture.md) · [Сборка и использование](../../../docs/03-guide.md).
