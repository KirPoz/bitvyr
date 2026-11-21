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
python -B tools/experiments.py --id bitvyr-public-audit-final-experiment-projections-20261121
python -B tools/experiments.py --sha <SHA256-исходного-отчёта>
```

Аппаратные FPS относятся к программной подаче кадров. Сравнение требует
одинаковых ROM, региона, ввода, интервала и звука. Для установленной
прошивки смотрите [P4](../bitvyr_nes_p4/betta/docs/04-results.md) и
[P4+C6](../bitvyr_nes_p4c6/beta_0_1/docs/04-results.md).
