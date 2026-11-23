# C6 пакетный APU consumer

P4 Main владеет `MainOwner`, BatchMainSession, PCM queue и transport. Worker
передаёт только immutable audio cut: PREACK loan для MMC5 Thin либо POSTJOIN
payload для Frame. Core/PPA epochs и C6 binding epochs сопоставляются явно,
slot0/1 Frame не является logical arena1 PublishedKey.

Warm profile принимает только ожидаемый C6 image/capabilities и полный HELLO.
INIT/BIND, phase/sample/cursor/CRC/full-key checks обязательны. Пауза сохраняет
историю. Новая ROM требует drain, настоящего sourceDone и checked UNBIND;
неподтверждённый timeout удерживает канал, ROM и буферы до join/reset.

`protocol` — single-source SDK-free wire/клиент; `transport` — P4 Hosted/SDIO;
`runtime` — Main ownership и очередь; `hosted` — отдельно лицензированный
Hosted2.13.1 raw adapter. SERIAL0/BLE остаётся обязательным соседом SERIAL1.
`qualification/C6BatchImage.h` — обязательный image admission, не отключаемый
golden observer. Остальные bounded observers не разрешают освобождение ресурсов.

Буферы и очереди fixed, heap выделяется до игры. C6 worker сохраняет текущие
capabilities совместимости; удаление неиспользуемого клиентом opcode из firmware
требует новой C6 identity/target/OTA проверки. HOST fixtures моделируют transport
и не доказывают реальный SDIO/HCI, RTOS scheduling или достижение60FPS.
