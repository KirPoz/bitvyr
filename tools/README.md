# Инструменты Bitvyr

Требуется Python 3. Для записи UART нужен `pyserial` из среды ESP-IDF.
Выполняйте команды из корня Bitvyr. Одновременно портом владеет одна программа.

Локальные `tools/Invoke-Idf.ps1` обеих редакций передают выбранную папку в
[общий запуск ESP-IDF](Invoke-Idf.ps1). Он использует окружение выбранного SDK.

## Ручное прохождение

Пассивный рекордер сохраняет полные строки UART, состояние записи и отметки.
Он выделяет сеансы по запуску, CRC и поколению игры.

```powershell
python -B tools/record_playthrough.py --execute-root --port COM4 --output bitvyr_nes_p4/betta/build/validation/my-play
python -B tools/record_playthrough.py --stop bitvyr_nes_p4/betta/build/validation/my-play
python -B tools/analyze_playthrough.py bitvyr_nes_p4/betta/build/validation/my-play --output bitvyr_nes_p4/betta/build/validation/my-play/report --profile p4
```

`export_playthrough_movies.py` экспортирует ввод и варианты сцен для проверки
повтора. [Банк ручного ввода](human-playthrough-movies.json) содержит маршруты
со статусом CANDIDATE. Пассивный UART передаёт фактические наблюдения и не
создаёт игровой ввод.

## Автоматический прогон

P4+C6 по умолчанию воспроизводит каталог из 26 измеренных сцен. Ввод и границы
восстанавливаются из [журнала](../experiments/README.md) по SHA-256 архива.
План до открытия COM показывает интервалы каждой сцены. P4 использует свой
закреплённый банк. Для сравнения двух редакций укажите одинаковый банк через
`--movies`.
Для P4+C6 есть дополнительная сцена Castlevania III в BLOCK 1-02. Выберите
`--movies tools/c6-later-cviii-scene.json --crcs 7cf70cde` при запуске из
корня репозитория. Проверяются три окна по 120 кадров. [Маршрут](c6-later-cviii-scene.json)
В банке есть CRC, регион, последовательность кнопок и измеряемый интервал.
Сначала посмотрите план без открытия COM:

```powershell
python -B tools/qualify_p4_device.py --expected-volume 28 --games c9ffbbdb
python -B tools/qualify_c6_device.py --expected-volume 28 --crcs c9ffbbdb
```

Для P4+C6 локальный ELF и журналы по умолчанию находятся в `bitvyr_nes_p4c6/beta/build`.
Для замера выпуска из `beta_0_1` укажите его ELF через `--local-elf` и папку журнала через `--output`.

Для прогона добавьте `--execute-root --attest-admitted-image-current`,
`--port COM4 --serial <USB-серийный-номер>`, `--elf-sha <полный-SHA256>` и
`--admission <boot.log>`. Лог загрузки и локальный ELF должны соответствовать
прошивке на устройстве. Громкость проверяется чтением. Во время прогона оставьте
экран и геймпад без нажатий.

Результат содержит фактический ввод, среднее и экстремумы FPS по окнам
120 кадров, ключ принятого снимка и итог завершения. В P4+C6 собираются
подтверждения возврата C6-звука и освобождения сеанса. В P4 проверяется локальный
DSP. Для сравнения редакций используйте один CRC, регион, набор кнопок,
измеряемые кадры и громкость.

## Сравнимые сцены P4+C6

Подготовка банка сверяет CRC32 и mapper с локальными ROM. Измеряемый участок
содержит 360 гостевых кадров. Длина вступления зависит от игры. Запуск одной
команды ниже готовит банк и рядом файл с покрытием.

```powershell
python -B tools/prepare_c6_routes.py --rom-dir ./roms/nes_rom --source catalog --windows 3 --output bitvyr_nes_p4c6/beta/build/routes-360.json
python -B tools/qualify_c6_device.py --movies bitvyr_nes_p4c6/beta/build/routes-360.json --expected-volume 28
```

Для дополнительного боя Battletoads & Double Dragon на 18720–19079
используйте наблюдённый ввод из варианта `53b02f3d-v01`. Три окна по
120 кадров начинаются внутри боя. Подготовка сохраняет 1308 изменений ввода
до 19200 и проверяет CRC локальной игры.

```powershell
python -B tools/prepare_c6_routes.py --rom-dir ./roms/nes_rom --source human --crcs 53b02f3d --variant 53b02f3d-v01 --roi-start 53b02f3d:18720 --windows 3 --output bitvyr_nes_p4c6/beta/build/bdd-later.json
python -B tools/qualify_c6_device.py --movies bitvyr_nes_p4c6/beta/build/bdd-later.json --expected-volume 28
```

Режим `catalog` собирает маршруты из [записи прохождения](c6-scene-recipe.json),
[банка боёв](c6-battle-scenes.json) и закреплённого общего ввода.
Для Just Breed и Lagrange Point в обеих версиях берутся окна из банка боёв. В рецепте указаны CRC, вариант ввода и
первый кадр сцены. В Battletoads & Double Dragon закреплён бой 2-1,
кадры 17640–17999. Для точечного повтора используйте `--crcs CRC`. Для ручного
выбора границ доступны `--source pinned` и `--source human` с `--roi-start CRC:кадр`.

Для просмотра сцены передайте `--native-runner` с путём к `nes_run.exe`,
`--native-max-edges 2048` и те же CRC в `--preview-crcs`. Получатся кадры
с начала, середины и конца участка. Для замера примените флаги допуска
устройства из предыдущего раздела. `roi_fps_range` проверяет каждое
120-кадровое окно, а `excess_us_per_guest_frame` показывает, сколько
времени на кадр нужно убрать для достижения 59,5 FPS. Если в UART есть
достаточная телеметрия, `hart`, `slowest_join` и `slowest_main_loop`
показывают затраты CPU1, ожидание результата и фазы самой долгой итерации CPU0.

Поле `frame_budget` хранит дефицит периода кадра и отдельно сравнивает время
задания CPU1 с бюджетом. Для Frame есть отдельное время ядра. В Coupled
производитель включает ожидания Thin и C6, которые учитываются внутри этого этапа.

Диагностическое поле `main_phases` содержит разбивку цикла Main для того же
окна Frame. Допуск следующего аудиозадания учитывается в
`predispatch_audio_us`. Поле `coupled_hart` содержит такты и инструкции CPU1
для полного окна Coupled с отметкой времени платы. В такты входят ожидания
и прерывания. Оба поля появляются только при полном совпадении границ.
FPS по-прежнему рассчитывается по границам подачи кадров.

В [банке боевых сцен P4+C6](c6-battle-scenes.json) версии Just Breed EN и JP
используют самостоятельные маршруты из загрузки до первого тактического боя.
Команда Auto запускает продвижение отрядов. Для EN первые кадры окон —
46440, 46560 и 46680, для JP — 28080, 28200 и 28320. По три окна из 120 кадров
на версию. Вступление занимает около 13 минут для EN и 8 минут для JP.
Для Lagrange Point EN и JP закреплены отдельные маршруты до первого боя.
Начала окон EN — 23880, 24000 и 24120, JP — 14160, 14280 и 14400.
Во всех окнах уже идёт боевой раунд.

```powershell
python -B tools/qualify_c6_device.py --movies tools/c6-battle-scenes.json --expected-volume 28 --crcs fcf46dbb 131b9946 9e13caa2 a1e48d00
```

Для аппаратного прогона используются те же параметры допуска устройства.
Для прогона сохранённого образа укажите `--local-elf` и его полный
`--expected-elf`. Сборщик сверит байты и запишет путь и SHA256 в отчёт.
Для аппаратного запуска также требуется журнал загрузки этого образа.

## Журнал и тесты

[experiments.json](../experiments.json) хранит текущие записи. [Хронология](../experiments/history.json) и [доказательства](../experiments/evidence.json) содержат накопленную историю. Команды ниже работают со всеми частями журнала.
Каждая запись имеет дату, идентификатор, условия проверки, результат и решение.
Перенесённый из `build` отчёт открывается по его исходному SHA256.

```powershell
python -B tools/experiments.py --date 2026-11-21
python -B tools/experiments.py --sha <SHA256-отчета>
python -B tools/experiments.py --id 2026-11-20-P16-full26-two-origin-final
python -B tools/experiments.py --event path/to/event.json
python -B -m unittest discover -s tools -p 'test_*.py'
```

Результаты программных тестов, аппаратный FPS и физическое качество звука и
экрана записываются с отдельными условиями и выводами.
