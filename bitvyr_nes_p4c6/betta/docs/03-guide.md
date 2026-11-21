# Сборка и использование P4+C6

1. Откройте [Bitvyr.code-workspace](../../../Bitvyr.code-workspace) в VS Code. Для сборки при отдельном открытии папки `betta` оставьте её в дереве Bitvyr: сборочные модули двух редакций находятся в корне репозитория. В **ESP-IDF: Pick a Workspace Folder** выберите **Bitvyr NES P4+C6**.
2. Выполните **ESP-IDF: Select Current ESP-IDF Version** и выберите установленный ESP-IDF 6.1. Проект задаёт цель `esp32p4`.
3. Выполните **ESP-IDF: Build Your Project**. Для пересборки удалите `build` через **ESP-IDF: Full Clean Project** и снова выполните **Build Your Project**.
4. Выберите UART платы. При первой установке P4 используйте **ESP-IDF: Flash (UART) Your Project**. Для обновления приложения используйте **ESP-IDF: Flash App Only (UART)**. Monitor работает на 115200 бод.

Эквивалент для терминала ESP-IDF из папки этой редакции:

```powershell
$env:ESP_IDF_VERSION = '6.1'
idf.py -B build fullclean
idf.py -B build build
idf.py -p COM4 app-flash monitor
```

`COM4` здесь пример. `NES_P4_CPU_OVERCLOCK=OFF` при конфигурации выбирает 360 МГц. По умолчанию используется профиль 400 МГц.

## Аудио и Bluetooth C6

Плата использует совместимый worker C6 с PCM 48 кГц и Hosted/HCI. При запуске игры P4 сверяет SHA-256 ELF и возможности worker через HELLO. Сборка worker описана в [проекте C6](../coprocessor/c6_audio/README.md).

Worker C6 устанавливается через разъём C6-UART по инструкции Waveshare. Считайте таблицу разделов и проверьте, какой OTA-слот выбран загрузчиком. `network_adapter.bin` — приложение worker для этого слота. Для пустого C6 сначала подготовьте совместимые загрузчик, таблицу разделов и данные OTA. [Режим загрузки C6](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/FAQ) · [Таблица разделов worker](../coprocessor/c6_audio/config/partitions.esp32c6.csv)

Для собственного образа вычислите SHA-256 `network_adapter.elf` и передайте его при сборке P4.

```powershell
$workerSha = (Get-FileHash build/c6_audio/release/network_adapter.elf -Algorithm SHA256).Hash
idf.py -B build -D "NES_C6_WORKER_SHA256=$workerSha" build
```

При сборке через расширение добавьте аналогичный параметр `-DNES_C6_WORKER_SHA256=<ваш SHA>` в пользовательский `idf.cmakeCompilerArgs` вместе со штатными аргументами. Для штатного проверенного worker используйте настройки проекта по умолчанию.

## Игра

Скопируйте игры `.nes` или ZIP с NES-образом в `nes_rom` на карте FAT32. В библиотеке крестовина Xbox выбирает игру, **A** запускает, **B** открывает настройки. В игре **Menu / ≡** соответствует Start, **View** соответствует Select. **View + Menu** или кнопка меню на экране открывают паузу. **Resume** возвращает в игру, **Library** завершает сеанс и открывает библиотеку.

Регулировки звука, контроллера и видео хранятся в NVS. Показатель FPS на экране отражает программную подачу кадров на дисплей.

## Тесты и замеры

Нативным тестам нужны CMake, Ninja, компилятор C++17 и Python. Запускайте из папки редакции после первой сборки ESP-IDF.

```powershell
cmake -S tests -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Пассивная запись и воспроизведение закреплённых маршрутов описаны в [инструментах](../../../tools/README.md). Результаты и контекст запусков сохраняются в [журнале экспериментов](../../../experiments.json).
