# Bitvyr

Портативная NES-консоль для Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 с собственным ядром на C++17.

- Библиотека NES и ZIP на SD FAT32. В PSRAM загружается выбранная игра.
- Сенсорные кнопки и Xbox Series по Bluetooth. Настройки громкости, управления и экрана сохраняются на плате.
- P4 исполняет 6502 и рассчитывает тайминг PPU/APU. Аппаратные PPA и DMA масштабируют кадр и подают его на экран.
- Редакция P4+C6 передаёт синтез PCM аудиосопроцессору. На плате 23 из 26 игр выдержали 59,5–60,5 FPS во всех закреплённых 120-кадровых окнах. [Условия и результаты](bitvyr_nes_p4c6/betta/docs/04-results.md)

| Редакция | Сборка | Архитектура | Результаты |
|---|---|---|---|
| P4 · локальный звук | [Руководство](bitvyr_nes_p4/betta/docs/03-guide.md) | [Ядро и оборудование](bitvyr_nes_p4/betta/docs/02-architecture.md) | [FPS](bitvyr_nes_p4/betta/docs/04-results.md) |
| P4+C6 · звук на C6 | [Руководство](bitvyr_nes_p4c6/betta/docs/03-guide.md) | [Распределение вычислений](bitvyr_nes_p4c6/betta/docs/02-architecture.md) | [FPS](bitvyr_nes_p4c6/betta/docs/04-results.md) |

В [рабочей области VS Code](Bitvyr.code-workspace) выберите редакцию и установленный [ESP-IDF 6.1](https://github.com/espressif/esp-idf/releases/tag/v6.1). Команды **Full Clean Project** и **Build Your Project** работают с каталогом `build` выбранной редакции.

[Журнал экспериментов](experiments.json) хранит параметры, результаты и решения по датам. [Инструменты](tools/README.md) описывают воспроизведение игровых маршрутов и чтение журнала.

Собственный код Bitvyr доступен по [GNU AGPLv3](LICENSE). [Уведомления о сторонних компонентах](THIRD_PARTY_NOTICES.md) содержат их условия и происхождение.
