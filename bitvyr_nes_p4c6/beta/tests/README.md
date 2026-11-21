# Проверки Bitvyr beta

Из корня Bitvyr настройте SDK-free native-проект обычным C++17 компилятором:

```
cmake -S bitvyr_nes_p4c6/beta/tests -B bitvyr_nes_p4c6/beta/build/native
cmake --build bitvyr_nes_p4c6/beta/build/native
ctest --test-dir bitvyr_nes_p4c6/beta/build/native --output-on-failure
```

Один CMake-вход подключает host Core/HID/UI, actual Frame, ZIP/library и NVS.
Host targets используют фиксированный профиль beta. `BitvyrTestProfile.h`
согласует PUBLIC metadata; ожидания PRIVATE-политик передаются тестам как
данные CTest и не создают дополнительный runtime-профиль прошивки.

Frame fixture исполняет настоящий pipeline с собственной synthetic program
и SDK mocks. Полный stream golden хранится в проверяющем коде и не требует
старого build. Negative завершается явным stderr/exit73 и ограничен timeout.
Allocation/OOM/stale metadata проверяются test seams, выключенными на SDK.

Python-проверки инструментов запускаются отдельно от CTest. Команды ниже
используют разные процессы: одноимённые root и edition-модули имеют разные
контракты и не должны подменять друг друга через import cache.
Edition-набор запускается из папки редакции; используйте Python среды ESP-IDF
с pyserial. Он импортирует транспортный модуль, но проверки подменяют порт.

```
python -B -m unittest discover -s tools -p "test_*.py"
Push-Location bitvyr_nes_p4c6/beta
python -B -m unittest discover -s tests/tools -p "test_*.py"
Pop-Location
```

Они проверяют parsing, capture, input/replay и observer boundaries на fake
транспортах; физический COM не открывается. Внешние test-ROM запускаются
отдельно через `nes_test_roms` или `nes_run`, не скачиваются и не входят в CTest.

Native не доказывает RV32 ABI/stack/HP placement, C6 SDIO/HCI, PPA/LCD,
надёжность физической SD или аппаратный FPS. Эти гейты выполняются отдельно
на конкретном образе; host-время не заменяет отметки времени платы.
