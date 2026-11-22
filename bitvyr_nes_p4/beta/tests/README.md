# Проверки Bitvyr

Единый вход — `cmake -S tests -B build/native`, затем `cmake --build build/native` и `ctest --test-dir build/native --output-on-failure`. Нужны C++17-компилятор и Python3 для синтетических ZIP-fixtures.

`host` проверяет ядро и независимые scalar bus-эталоны из тех же исходников. `storage` проверяет каталог, ошибки ZIP/DEFLATE и границы ввода с независимым Python/zlib. `settings` проверяет формат настроек и настоящий NVS-адаптер с fake API. HID проверяется без радиотранспорта.

Сборка тестов не открывает COM, не читает пользовательские ROM и не меняет SD/NVS. Выходы находятся только внутри `build/native`. Аппаратная проверка изображения, Xbox, звука и FPS выполняется отдельно.
