# Плата Waveshare4.3

Board — аппаратный фасад LCD/PPA, звука, SD и GT911; BluetoothInput/HidGamepad предоставляют Xbox-ввод.

Main/CPU0 владеет выводом, SD/I2C/I2S. Bluetooth-задача обслуживает HCI и публикует ограниченное состояние кнопок.

Стадии PPA sourceDone и LCD completion различны. Ресурсы создаются при инициализации; ошибки возвращаются явно, повторное использование ждёт аппаратного подтверждения.

ESP-IDF6.1 и Hosted2.13.1. Проверка — native HID/presentation и плата: экран, сенсор, SD, тихий звук, Xbox.
