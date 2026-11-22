# Bitvyr

Портативная NES-консоль для Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3. Ядро на C++17 исполняет 6502, PPU, APU и логику картриджа. Библиотеку можно пополнять файлами `.nes` и ZIP на SD FAT32. Консоль загружает выбранную игру в PSRAM. Управлять можно с сенсорного экрана или геймпада Xbox Series по Bluetooth.

**Стабильный выпуск P4+C6 Beta 0.1.** P4 исполняет игру, C6 рассчитывает звук, PPA и DMA выводят изображение. На десяти проверенных сценах, включая семь игр MMC3, скорость подачи составила **59,938–60,143 FPS** в окнах по 120 кадров. [Таблица результатов по играм](bitvyr_nes_p4c6/beta_0_1/docs/04-results.md)

| Редакция | Вычисление звука | Документация |
|---|---|---|
| [P4+C6 Beta 0.1](bitvyr_nes_p4c6/beta_0_1) | C6 по Hosted SDIO | [Сборка и использование](bitvyr_nes_p4c6/beta_0_1/docs/03-guide.md) · [Схема ядра](bitvyr_nes_p4c6/beta_0_1/docs/02-architecture.md) |
| [P4](bitvyr_nes_p4/beta) | Локально на P4 | [Сборка и использование](bitvyr_nes_p4/beta/docs/03-guide.md) · [Схема ядра](bitvyr_nes_p4/beta/docs/02-architecture.md) · [Замеры](bitvyr_nes_p4/beta/docs/04-results.md) |

## Запуск

1. Поместите игры в папку `nes_rom` на SD FAT32.
2. Откройте [Bitvyr.code-workspace](Bitvyr.code-workspace) в VS Code. Выберите папку нужной редакции и ESP-IDF 6.1. У каждой папки свой каталог `build`.
3. Выполните **ESP-IDF: Build Your Project**. Для полной пересборки выполните **ESP-IDF: Full Clean Project**, затем **ESP-IDF: Build Your Project**. Для редакции P4+C6 установите совместимый C6 worker по [руководству платы](bitvyr_nes_p4c6/beta_0_1/docs/03-guide.md).

## Разработка и лицензия

Дальнейшие исследования P4+C6 ведутся в [каталоге beta](bitvyr_nes_p4c6/beta). [Журнал экспериментов](experiments/README.md) хранит даты, условия, результаты и решения. [Инструменты](tools/README.md) воспроизводят маршруты по вводу и измеряют FPS по времени платы.

Собственный код, документация и инструменты Bitvyr распространяются под GNU AGPLv3 (`AGPL-3.0-only`). [Текст лицензии](LICENSE) · [Сторонние компоненты и лицензии](THIRD_PARTY_NOTICES.md)
