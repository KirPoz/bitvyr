# Сборка worker C6 для Beta 0.2

Worker принимает события APU по SDIO, возвращает PCM для динамика и обслуживает транспорт Bluetooth геймпада. У измеренной сборки 8 приёмных и 12 передающих буферов SDIO. Совместимость P4 с worker определяется SHA-256 ELF при обмене HELLO.

Из папки `beta_0_2` в терминале ESP-IDF 6.1 для Windows выполните команды. Нужны Git, Python среды SDK и инструменты, закреплённые в [рецепте worker](../recipe/external-lock.json).

```powershell
$radioSources = Join-Path $env:TEMP 'bitvyr-c6-radio'
$workerBuild = Join-Path $env:TEMP 'bitvyr-c6-audio/beta02-release'
git clone https://github.com/ernimen7/esp32-p4-c6-le-audio.git $radioSources
git -C $radioSources checkout --detach 0fafdd428ea26f8bb700d67d72e2dcf12241f4c5
python -B coprocessor/c6_audio/tools/build_bt_audio.py --upstream $radioSources --out $workerBuild
```

Выберите свободную папку для `--out`. Скрипт проверяет 36 внешних входов по SHA-256, готовит проект, собирает ELF и BIN и проверяет карту линковки. На диске рядом с образами остаются `result.json` и копии лицензии и уведомления upstream в `bt_audio_src`. Передайте SHA-256 своего `network_adapter.elf` в конфигурацию P4 по [руководству](../../../docs/03-guide.md). Эталонные отпечатки измеренной пары приведены в [результатах](../../../docs/04-results.md).

Исходники транспорта Ernest Domaradzki поступают из указанной ревизии под MIT. Архив контроллера `libble_app.a` предоставляет установленный ESP-IDF. Рецепт сверяет его SHA-256, создаёт изменённую копию в папке сборки и связывает её с worker. Уведомление Espressif указывает авторов компонентов NimBLE. [Манифест входов](sources.json) · [MIT и атрибуция upstream](https://github.com/ernimen7/esp32-p4-c6-le-audio/blob/0fafdd428ea26f8bb700d67d72e2dcf12241f4c5/NOTICE) · [Уведомление Espressif](https://github.com/espressif/esp32c6-bt-lib/blob/eee922eaa95770d567f0939cc3d56c712a6ecf65/NOTICE)
