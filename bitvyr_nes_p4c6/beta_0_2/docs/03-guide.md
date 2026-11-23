# Сборка, установка и игра

## Прошивка P4

1. Клонируйте репозиторий целиком. Откройте в VS Code папку `bitvyr_nes_p4c6/beta_0_2` внутри него. Общие файлы CMake лежат двумя уровнями выше. В расширении Espressif выполните **ESP-IDF: Select Current ESP-IDF Version** и выберите ESP-IDF 6.1.
2. Выполните **ESP-IDF: Build Your Project**. Для чистой пересборки выберите **ESP-IDF: Full Clean Project**, затем **ESP-IDF: Build Your Project**. Цель проекта `esp32p4`, выходная папка `build`.
3. Подключите разъём USB–UART платы. Выберите обнаруженный COM-порт и **ESP-IDF: Flash (UART) Your Project**. Для обновления только приложения на уже настроенной P4 выберите **ESP-IDF: Flash App Only (UART)**. Последовательный монитор работает на 115200 бод.

При нестандартной установке SDK в Windows укажите в пользовательской настройке VS Code `idf.eimIdfJsonPath` файл `tools/eim_idf.json` своей установки. После выбора ESP-IDF 6.1 расширение покажет команды сборки для папки выпуска.

Те же команды из папки выпуска в терминале ESP-IDF для Windows. Подставьте номер своего порта.

```powershell
$env:ESP_IDF_VERSION = '6.1'
idf.py -B build fullclean
idf.py -B build build
idf.py -p COM4 flash monitor
```

При проверке воспроизводимости на стенде перед Full Clean и Build задали `$env:SOURCE_DATE_EPOCH = '1795468792'`. С теми же путями и инструментами ELF и BIN побайтно совпали с образами аппаратного прогона. В обычной сборке SDK ставит текущее время. [SHA-256 проверенной пары](04-results.md)

## Совместимый C6

При запуске игры P4 сверяет возможности и SHA-256 ELF программы C6. Пара, которой соответствует таблица FPS, использует C6 `c2660fac`. Если на плате установлен этот worker, достаточно обновить P4. Для новой платы соберите worker и установите его согласно таблице разделов C6. Файл `network_adapter.bin` содержит приложение. При обновлении действующего устройства адрес приложения берут из его таблицы разделов и состояния OTA. На чистом C6 также требуются загрузчик, таблица разделов и начальные OTA-данные. [Сборка worker](../coprocessor/c6_audio/bluetooth_audio/README.md) · [Разделы](../coprocessor/c6_audio/config/partitions.esp32c6.csv) · [Разъём и режим загрузки Waveshare](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/FAQ)

Для собственноручно собранной пары укажите SHA-256 полученного `network_adapter.elf` при конфигурировании P4. Здесь путь соответствует командам сборки worker.

```powershell
$workerElf = Join-Path $env:TEMP 'bitvyr-c6-audio/beta02-release/network_adapter.elf'
$workerSha = (Get-FileHash -LiteralPath $workerElf -Algorithm SHA256).Hash
idf.py -B build -D "NES_C6_WORKER_SHA256=$workerSha" build
```

Для чистых пересборок через VS Code тот же `-DNES_C6_WORKER_SHA256=…` задайте в `idf.cmakeCompilerArgs` для этой папки. Настройки по умолчанию соответствуют контрольному worker.

## Игра

1. В корне SD FAT32 создайте `nes_rom`. Скопируйте туда файлы `.nes` или ZIP с NES-образом, вставьте карту и откройте библиотеку.
2. Выберите игру на экране или крестовиной Xbox и нажмите **A**. Для сопряжения включите Xbox Series, удерживайте его кнопку **Pair** до быстрого мигания и запустите сопряжение в настройках контроллера. При повторном подключении достаточно включить геймпад.
3. Во время игры **Menu (≡)** соответствует Start, **View (два окна)** соответствует Select. Одновременно нажмите **View + Menu** или коснитесь кнопки меню на экране, чтобы открыть паузу. **Resume** продолжает игру, **Library** возвращает к выбору картриджа. Громкость и счётчик FPS доступны в настройках.

## Проверки на ПК

Для нативных проверок нужны CMake, C++17, Python и генератор сборки. Следующие команды используют Ninja. Выполняйте их в папке выпуска после сборки прошивки.

```powershell
cmake -S tests -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Нативные сравнения проверяют состояния CPU, PPU, звук, обработку IRQ и правила владения буферами. План 38 аппаратных маршрутов можно посмотреть из корня репозитория командой `python -B tools/qualify_c6_device.py --expected-volume 7`. Она выводит CRC и измерительные интервалы. Для прохода на плате укажите установленный образ и его журнал загрузки. [Методика и замеры](04-results.md) · [Инструменты записи и повтора ввода](../../../tools/README.md)
