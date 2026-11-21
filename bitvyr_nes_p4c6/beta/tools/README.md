# Инструменты P4+C6

`Invoke-Idf.ps1` вызывает ESP-IDF 6.1 для своего каталога `build`.
Ручная запись и парные прогоны описаны в [общих инструментах](../../../tools/README.md).

Локальные утилиты подключены к тестам UART, схемы кадров и точности ввода.
`build_games.py`, `test_games.py` и `render_game_grid.py` относятся к нативной
проверке локальной подборки ROM. Из каталога редакции:

```powershell
python -B tools/build_games.py --source ./Games --output build/games
python -B tools/test_games.py --manifest build/games/manifest.json --runner build/native/host/nes_run.exe --cmake-cache build/native/CMakeCache.txt
python -B tools/render_game_grid.py build/tests/games
```

Подготовка создаёт `roms/*.nes` и `manifest.json` формата `local_rom_manifest_v1`;
Flash pack не создаётся. Ввод smoke остаётся generic и не квалифицирует gameplay.
`--cmake-cache` необязателен: без него проверяются каталог runner и два родительских
каталога. Если cache отсутствует, `runner_core_cmake_options` равен `null`,
`runner_cmake_cache` хранит причину NA. Найденный cache фиксируется путём и SHA256,
но не доказывает фактические compile flags бинарника. Пустой словарь при доступном
cache означает отсутствие `NES_CORE_*:BOOL`, включая фиксированный CMake-профиль.

`audit_cpp_documentation.py` проверяет C++-комментарии и сравнивает токены.
Установите версии из `requirements-docs.txt` в отдельное окружение Python.
Запустите `python -B tools/audit_cpp_documentation.py --inventory --output build/cpp-comments.json`.
Подробные параметры используются для аудита изменённого пакета.
