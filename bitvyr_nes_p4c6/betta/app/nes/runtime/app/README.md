# Координатор Main/Worker

Private `.inl` включаются один раз из NesApp.cpp после общего owner-state.
Они не являются самостоятельными TU и не вводят дубликаты статических объектов.
VideoSetup/FrameVideo/CoupledVideo выбирают и обслуживают runtime; C6Audio хранит
binding/pump/PCM/drain; Worker и FrameDispatch владеют handoff; Session — поколением
и pacing; Library — SD/ZIP/ROM; Ui — NVS/меню; Console — bounded UART; Telemetry —
read-only counters и обычный игровой loop.

Внешние вызовы выполняет Main, за исключением workerMain CPU1. Только Worker
меняет primary Machine. Main не читает live state из-за доступности указателя.
ROM/context/code не освобождаются при неизвестном владельце. Stop сначала
присоединяет и принимает финальный Frame, затем drain/UNBIND аудио и завершает PPA.

Разбиение проверено раскрытием include и сравнением C++ tokens в прежнем порядке.
Изменились source locations; старые ELF/stack/FPS доказательства на beta не переносятся.
