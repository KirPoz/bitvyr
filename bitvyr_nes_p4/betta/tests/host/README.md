# Переносимые проверки Core

Один CMake build содержит продуктовый Core и два test-only reference target:

- `nes_core`: фиксированные81 PUBLIC/21 PRIVATE definitions выбранного P4-профиля.
- `nes_scalar_reference`: те же исходники, независимые виртуальные CPU/PPU bus
  и raw packet-APU.
- `nes_fastcap_scalar_audio`: scalar CPU/PPU с качеством APU выбранного профиля;
  его canonical сравнивается с продуктовым результатом без смешения разных FIR-профилей.

Reference target не являются продуктовыми переключателями и не копируют исходное дерево Core.

Продуктовые fixtures проверяют lease/cut/capacity/alias, CPU/DMA/IRQ,
MMC5 event kernel/WRAM/raster, полный RGB, frame PCM/phase и выбранные portable
audio policies. Scalar fixtures независимо проверяют opcode/stack/dummy I/O,
PPUSTATUS/PPUDATA/OAM, batch trace/RGB, Legacy160 и mapper-fetch. Собственные
маленькие iNES-программы не являются gameplay или измерением FPS.

`nes_run` читает заданный ROM/movie и выдаёт диагностические hashes/PCM;
`nes_test_roms` использует предоставленные пользователем test-ROM данные.
Внешний nestest — необязательный аппаратный golden log, не код чужого ядра.
Тесты ROMdecode/lazy/readfold/cost-profile удалены вместе с неактивными реализациями.
Settings, ZIP/Library и HID имеют отдельные integration fixtures общего tests entrypoint.

Assertions включены в Release; C++17, строгие warnings, fp-contract=off.
Выходы находятся только в `build/tests` или `build/native` данной линии.
Пример из корня линии: `cmake -S tests/host -B build/tests`, затем
`cmake --build build/tests` и `ctest --test-dir build/tests --output-on-failure`.
На Windows явно выбирается проектный w64devkit/CMake environment.

Fixtures владеют своим backing, ROM и журналами до конца случая; изменяемый
эталон не разделяет память с проверяемым объектом. Ошибка прекращает executable
ненулевым кодом. Native PASS не заменяет SDK ABI/стек/HP/IRQ и moving hardware,
не объявляет SD исправленной и не переносит старые FPS на новый исходник.
