# Frame video pipeline

`FrameVideoPipeline` передаёт кадры non-MMC5 от единственного Worker к Main.
Factory создаёт два bounded Payload с RAM/AudioPacket, отдельные VideoPacket и INTERNAL
атомарный Control. Первичная Machine остаётся у CPU1; Main использует immutable
ticket/cut и VideoRenderer. Никакой live Machine доступ не следует из producer pointer.

`FrameOutputFactory.inl` отвечает за cold admission/выделение/отказ,
`FrameOutputTransport.inl` — за paired публикацию и принятие. Full Output lease
содержит свои romEpoch/transportEpoch; они не приравниваются к C6 binding epoch.
Board RGB допускается только полным128-aligned RGB565 span61440 words с caps
обоих концов и без alias primary Machine/ROM. PPA source lifetime остаётся у Board.

POSTJOIN audio заимствуется из того же Payload после настоящего Worker completion.
`audioHeld` запрещает повторную запись при TX/C6-владельце; `audioIssued` запрещает
повторное borrow после раннего sourceDone до C0 accept. Video accept не освобождает
audio. Destroy/rebind отказывает при held; release требует весь issued ticket.

Указатели/слоты fixed; горячих аллокаций нет. Timeout/fault удерживает заимствования.
Hart/RAM/usage DTO — наблюдаемость, не authority и не доказательство FPS.
HOST fixture проверяет M0/M7 NTSC/PAL,24frames, полный CPU/RAM/RGB, early release,
ошибочные ключи и reuse. PPA/SDK/SDIO/RTOS остаются отдельной аппаратной проверкой.

## Ленты FM в существующих слотах

Опциональный cold argument `cpu0Vrc7` разрешён только для actualFM85. Factory
создаёт отдельную PSRAM-арену двух Vrc7WriteTape и2×16384 событий по8байт
(262144байта событий плюс метаданные). Иные mapper этой арены не выделяют.
Все allocation extents проверяются до bind; отказ не включает скрытый fallback.

Worker после finalmetadata копирует sealed View в свой Payload до Ready.
`borrowJoinedVrc7Tape` на Main сверяет original JoinedAudioTicket и читает только
cachedN, даже пока Machine считает N+1. `pendingDeadlineUs` возвращает исходный
абсолютный срок ReadyN. Ни getter, ни Main FM-converter не наблюдают live Machine.

Tape освобождается внутри exact releaseJoinedAudio вместе с исходным audio loan;
video accept не означает reuse FM. Main меняет только phase отдельного Tape,
а cached Ready Payload не перезаписывает. Следующий Worker начинает только Empty
ленту. Actual joined destruction сначала terminal-detach в Core, затем free арены;
неизвестный Worker/C6/PPA или timeout сохраняет память.

Дополнительный native HOST-сценарий исполняет actual factory/Frame/Core и непустые
FM-записи на0/85 NTSC/PAL, проверяет24frames, stale-ticket и сохранение событий
соседнего слота. Это allocator/time mocks, не физическое RTOS/SDIO доказательство.
