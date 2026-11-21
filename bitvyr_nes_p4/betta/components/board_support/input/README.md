# GT911 и Xbox HID

Touch.cpp опрашивает сенсор; BluetoothInput.cpp выполняет HCI/BLE сопряжение, HidGamepad.cpp разбирает HID-дескрипторы и отчёты.

I2C у Main; Bluetooth — отдельный транспортный владелец, приложение читает опубликованные кнопки.

Привязка хранится в NVS. Пакеты и поля проверяются по длине; disconnect сбрасывает игровой ввод без освобождения чужого контекста.

Hosted/Bluetooth SDK и BoardState; native HostHidTests и аппаратное переподключение Xbox.
