# Runtime конвейеры

Пакет содержит действующую игровую реализацию, не benchmark harness.
`frame` — CPU1 Timing Output и CPU0 VideoRenderer для non5; `coupled` — CPU1 Thin
и CPU0 FullPPU для MMC5; `bridge` — bounded release/acquire протокол между ними.
`app` — private части единого NesApp TU. Выбор front-end фиксируется mapper при
создании joined сеанса, а не переключается в середине кадра.

Общий контракт: ROM и payload живут до полного join/освобождения, producer N+1
не подменяет ticket N. Fault/timeout не означают free. Ни packet completion,
ни диагностический counter не заменяет sourceDone, PCM release или LCD completion.

Ресурсы bounded: один Worker, прежние два Frame payload, Thin ring8, один текущий
ROM, существующие RGB/native/PPA буферы. Динамическое расширение в кадре запрещено.
Native проверяет протокол и guest output; SDK/HW проверяют ABI/размещение/стек/физические fences.
