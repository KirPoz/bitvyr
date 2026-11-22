# Журнал экспериментов Bitvyr

Журнал содержит результаты проверок P4 и P4+C6. Записи фиксируют условия,
точный образ, ROM, фазу игры, измерения и принятое решение.

| Данные | Содержимое |
|---|---|
| [Текущие записи](../experiments.json) | Последние аппаратные прогоны, сравнения и решения |
| [Хронология](history.json) | События, измерения и разбор завершённых экспериментов |
| [Отчёты](evidence.json) | Привязанные к исходным SHA-256 данные измерений |

```powershell
python -B tools/experiments.py --date 2026-11-21
python -B tools/experiments.py --id 2026-11-19-sdk06-p4-all26-and-c6-m4-final
python -B tools/experiments.py --id p4c6-mmc3-crossline-production-20261121
python -B tools/experiments.py --sha <SHA256-исходного-отчёта>
```

По SHA-256 доступны сохранённый отчёт, сводка трассы или связанное событие.
Сводку сопровождает запись исходного индекса. Девять отчётов, перенесённых
из старой сборки, перечислены в событии
`bitvyr-legacy-build-evidence-preserved-20261121`.

Для просмотра вне репозитория выгрузите все даты, отчёты и индексы в один JSON.
Команда записывает файл целиком и сохраняет исходные части журнала.

```powershell
python -B tools/experiments.py --export .local/experiments-full.json
python -B tools/experiments.py --file .local/experiments-full.json --id p4c6-mmc3-crossline-production-20261121
```

Аппаратные FPS относятся к программной подаче кадров. Сравнение требует
одинаковых ROM, региона, ввода, интервала и звука. Для установленной
прошивки смотрите [P4](../bitvyr_nes_p4/beta/docs/04-results.md) и
[P4+C6](../bitvyr_nes_p4c6/beta_0_1/docs/04-results.md).
