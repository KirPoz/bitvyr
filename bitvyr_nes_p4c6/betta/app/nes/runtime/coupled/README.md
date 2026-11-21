# MMC5 coupled video

`CoupledVideoPipeline` связывает CPU1 ThinSignals с CPU0 FullPPU consumer.
`ThinBridge` переносит completed prefixes, once-only MMIO reads и final commit;
публикация prefix ещё не подтверждает исполнение consumer. У каждого lease/cut
проверяются ROM/generation/region/slot/sequence/input/time, порядок MMC5 strobes
и ретенция MMIO значений. Не допускаются rollback или скрытый локальный fallback.

Thin ring фиксирован8slots; hot raster/control allocations выполняются до игры
с проверкой full extents/caps. Main обслуживает bounded poll и PPA overlap,
Worker единолично меняет primary Machine. PREACK audio token означает точный
immutable cut, а не право произвольного чтения Worker packet.

AsyncPresent разделяет PPA source completion и LCD handoff. Fault/timeout не
освобождает ROM/context/ring. Реальный join и checked C6 drain/UNBIND требуются
до новой ROM. Native Core/MMC5 tests проверяют CPU/IRQ/RAM/RGB/PCM semantics;
RV32 placement, стек и аппаратный progress проверяются после SDK-сборки.
