# Инструменты P4+C6

`Invoke-Idf.ps1` вызывает ESP-IDF 6.1 для своего каталога `build`.
Ручная запись и парные прогоны описаны в [общих инструментах](../../../tools/README.md).

Локальные утилиты подключены к тестам UART, схемы кадров и точности ввода.
`build_games.py`, `test_games.py` и `render_game_grid.py` относятся к нативной
проверке локальной подборки ROM.

`audit_cpp_documentation.py` проверяет C++-комментарии и сравнивает токены.
Установите версии из `requirements-docs.txt` в отдельное окружение Python.
Запустите `python -B tools/audit_cpp_documentation.py --inventory --output build/cpp-comments.json`.
Подробные параметры используются для аудита изменённого пакета.
