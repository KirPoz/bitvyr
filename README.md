# Bitvyr

Портативная NES-консоль для Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 с собственным ядром на C++17.

- Библиотека NES и ZIP на SD FAT32. В PSRAM загружается только выбранная игра.
- Сенсорные кнопки и Xbox Series по Bluetooth. Настройки громкости, управления и экрана сохраняются на плате.
- P4 исполняет 6502 и рассчитывает тайминг PPU/APU. Аппаратные PPA и DMA масштабируют кадр и подают его на экран.
- Редакция [P4+C6 Betta 0.1](bitvyr_nes_p4c6/betta/README.md#betta-01) рассчитывает PCM на сопроцессоре и перекрывает подготовку звука, эмуляцию и вывод. Все семь игр MMC3 из каталога уложились в целевой диапазон на проверенных сценах.

| Редакция | Сборка | Архитектура | Результаты |
|---|---|---|---|
| P4 · локальный звук | [Руководство](bitvyr_nes_p4/betta/docs/03-guide.md) | [Ядро и оборудование](bitvyr_nes_p4/betta/docs/02-architecture.md) | [FPS](bitvyr_nes_p4/betta/docs/04-results.md) |
| P4+C6 · звук на C6 | [Руководство](bitvyr_nes_p4c6/betta/docs/03-guide.md) | [Распределение вычислений](bitvyr_nes_p4c6/betta/docs/02-architecture.md) | [FPS](bitvyr_nes_p4c6/betta/docs/04-results.md) |

## Быстрый старт

В [рабочей области VS Code](Bitvyr.code-workspace) выберите редакцию и установленный [ESP-IDF 6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1). Команды **Full Clean Project** и **Build Your Project** работают с каталогом `build` выбранной редакции.

Поместите свои игры в папку `nes_rom` на SD FAT32. Редакции P4+C6 требуется [совместимый C6 worker](bitvyr_nes_p4c6/betta/docs/03-guide.md).

## Замеры и лицензия

Замеры и решения доступны через [журнал экспериментов](experiments/README.md) с хронологией и отчётами по SHA-256. [Инструменты](tools/README.md) описывают просмотр журнала и повтор маршрутов.

Собственный код Bitvyr распространяется по [GNU AGPLv3](LICENSE) (`AGPL-3.0-only`). [Уведомления о сторонних компонентах](THIRD_PARTY_NOTICES.md) содержат их условия и происхождение.
