# Сборка и использование P4+C6

[Общее руководство](../../../bitvyr_nes_p4/betta/docs/03-guide.md) описывает сборку P4 в VS Code, установку игр, управление и тесты. Здесь приведена настройка C6 для этой редакции.

## Аудио и Bluetooth C6

Плата использует совместимый worker C6 с PCM 48 кГц и Hosted/HCI. При запуске игры P4 сверяет SHA-256 ELF и возможности worker через HELLO. Сборка worker описана в [проекте C6](../coprocessor/c6_audio/README.md).

Worker C6 устанавливается через разъём C6-UART по инструкции Waveshare. Считайте таблицу разделов и проверьте, какой OTA-слот выбран загрузчиком. `network_adapter.bin` — приложение worker для этого слота. Для пустого C6 сначала подготовьте совместимые загрузчик, таблицу разделов и данные OTA. [Режим загрузки C6](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/FAQ) · [Таблица разделов worker](../coprocessor/c6_audio/config/partitions.esp32c6.csv)

Для собственного образа вычислите SHA-256 `network_adapter.elf` и передайте его при сборке P4.

```powershell
$workerSha = (Get-FileHash build/c6_audio/release/network_adapter.elf -Algorithm SHA256).Hash
idf.py -B build -D "NES_C6_WORKER_SHA256=$workerSha" build
```

При сборке через расширение добавьте аналогичный параметр `-DNES_C6_WORKER_SHA256=<ваш SHA>` в пользовательский `idf.cmakeCompilerArgs` вместе со штатными аргументами. Для штатного проверенного worker используйте настройки проекта по умолчанию.
