# Сборка P4+C6

`NesC6Audio.cmake` проверяет конфигурацию SDIO и SHA исходников Hosted 2.13.1.
Поддержка канала PCM добавляется при конфигурации в копию драйвера внутри `build`.
Правила размещения ядра подключает `app/nes/CMakeLists.txt`.

[Общие модули CMake](../../../cmake/README.md) · [Проект C6](../coprocessor/c6_audio/README.md) · [Руководство](../docs/03-guide.md)
