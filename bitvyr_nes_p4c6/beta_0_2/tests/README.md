# Нативные проверки

Из папки выпуска соберите проект CMake с компилятором C++17. Выполняйте команды после первой сборки ESP-IDF, чтобы временные файлы попали в `build/native`.

```powershell
cmake -S tests -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Проверки охватывают CPU и PPU, мапперы, RGB565 и PCM, ввод, меню, ZIP, настройки NVS и правила обмена кадрами. `storage/zip_reference.py` создаёт образцы ZIP/DEFLATE стандартными `zipfile` и `zlib`. Для негативных сценариев используются синтетические ROM и контролируемые отказы.

Тесты инструментов записи и повтора запускаются отдельным процессом Python из среды ESP-IDF с `pyserial`.

```powershell
python -B -m unittest discover -s tests/tools -p "test_*.py"
```

Для тестов общих инструментов из корня репозитория запустите `python -B -m unittest discover -s tools -p "test_*.py"`. Они подменяют UART тестовым транспортом. Реальные скорость SDIO, расписание задач, доступную память и FPS измеряют отдельно на плате. [Файлы ядра](../app/nes/core/README.md) · [Замеры на устройстве](../docs/04-results.md)
