# Сборка и использование Bitvyr

1. Откройте [Bitvyr.code-workspace](../../../Bitvyr.code-workspace) в VS Code. В **ESP-IDF: Pick a Workspace Folder** выберите **Bitvyr NES P4** для локального звука или **Bitvyr NES P4+C6 Beta 0.2** для стабильного выпуска с аудио на C6. Можно открыть папку редакции отдельно внутри клонированного репозитория. Общие модули сборки находятся в корневом `cmake/`.
2. Выполните **ESP-IDF: Select Current ESP-IDF Version** и выберите установленный ESP-IDF 6.1. Проект задаёт цель `esp32p4`.
3. Выполните **ESP-IDF: Build Your Project**. Для пересборки удалите `build` через **ESP-IDF: Full Clean Project** и снова выполните **Build Your Project**.
4. Если выбрали редакцию P4+C6, проверьте [прошивку сопроцессора](../../../bitvyr_nes_p4c6/beta_0_2/docs/03-guide.md). Выберите UART платы. При первой установке P4 используйте **ESP-IDF: Flash (UART) Your Project**. Для обновления приложения используйте **ESP-IDF: Flash App Only (UART)**. Monitor работает на 115200 бод.

Эквивалент для терминала ESP-IDF из папки этой редакции:

```powershell
$env:ESP_IDF_VERSION = '6.1'
idf.py -B build fullclean
idf.py -B build build
idf.py -p COM4 app-flash monitor
```

`COM4` здесь пример. `NES_P4_CPU_OVERCLOCK=OFF` при конфигурации выбирает 360 МГц. По умолчанию используется профиль 400 МГц.

## Игра

Скопируйте игры `.nes` или ZIP с NES-образом в `nes_rom` на карте FAT32. В библиотеке крестовина Xbox выбирает игру, **A** запускает, **B** открывает настройки. В игре **Menu / ≡** соответствует Start, **View** соответствует Select. **View + Menu** или кнопка меню на экране открывают паузу. **Resume** возвращает в игру, **Library** завершает сеанс и открывает библиотеку.

Регулировки звука, контроллера и видео хранятся в NVS. Показатель FPS на экране отражает программную подачу кадров на дисплей.

## Тесты и замеры

Нативным тестам нужны CMake, Ninja, компилятор C++17 и Python. Запускайте из папки редакции после первой сборки ESP-IDF.

```powershell
cmake -S tests -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native --output-on-failure
```

Пассивная запись и воспроизведение закреплённых маршрутов описаны в [инструментах](../../../tools/README.md). Результаты и контекст запусков сохраняются в [журнале экспериментов](../../../experiments.json).
