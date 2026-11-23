# Лицензия и сторонние компоненты

Copyright © 2026 Bitvyr contributors. Собственный код, инструменты и документация Bitvyr распространяются по [GNU AGPLv3](LICENSE) ([AGPL-3.0-only](https://spdx.org/licenses/AGPL-3.0-only.html)). У каждого стороннего компонента действуют указанные ниже условия.

| Компонент | Где используется | Условия и происхождение |
|---|---|---|
| ESP-IDF 6.1 | Обе редакции; RTOS, PPA/LCD, SD, I2S, Bluetooth | Внешний SDK. Apache-2.0 и лицензии отдельных компонентов; [официальный реестр](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/COPYRIGHT.html) |
| ESP-Hosted MCU 2.13.1 | SDIO/HCI; P4 overlay и C6 worker | Apache-2.0; commit `0e8d0e3f0c2723deaf15441200cb4d8c53fea230`. [Лицензия](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/LICENSES/ESP-Hosted.txt), [P4 provenance](bitvyr_nes_p4c6/beta_0_2/app/nes/audio/c6/hosted/provenance.json), [пометки изменений и патчи](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/provenance/license-notices.json) |
| Apache Mynewt mempool | C6 `project/main/common/mempool/mempool_ll.c` и `.h` | Apache-2.0. Copyright ASF и изменения Espressif сохранены в файлах; [точная upstream NOTICE](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/LICENSES/Apache-Mynewt-NOTICE.txt), [ревизия источника NOTICE](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/provenance/license-notices.json) |
| Hosted `.proto` и generated C/H | C6 RPC | `.proto` принадлежит Espressif, Apache-2.0. Generated C/H наследуют условия входного файла. |
| protobuf-c runtime | Внешний SDK/Hosted; поддержка generated RPC | BSD-2-Clause, Dave Benson и protobuf-c authors; [полный текст и copyright установленной версии](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/LICENSES/protobuf-c.txt) |
| Радиотранспорт C6 | Внешние входы сборки, автор Ernest Domaradzki | MIT для исходников проекта. Архив контроллера берётся из SDK. [Происхождение и условия](bitvyr_nes_p4c6/beta_0_2/THIRD_PARTY_NOTICES.md) · [Закреплённые входы](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/bluetooth_audio/sources.json) |
| Font5x7 | Меню обеих редакций и native-preview | Собственный битовый алфавит Bitvyr. |

Публикация содержит исходники, тесты и рецепты сборки. SDK, toolchain, прошивки и игры используются локально. Зависимости P4 закреплены в файлах `dependencies.lock`, входы worker — в [рецепте C6](bitvyr_nes_p4c6/beta_0_2/coprocessor/c6_audio/recipe). Отдельный выпуск прошивки сопровождается применимыми лицензиями, уведомлениями и исходниками в объёме соответствующих лицензий.
