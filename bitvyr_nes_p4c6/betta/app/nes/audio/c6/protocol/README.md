# Portable wire и batch client

Здесь единственный canonical wire contract P4/C6: RawLimits/RawWire/CRC32,
AudioPacketWire, FrameService/FrameClient/PartialClient, BatchProtocol/BatchClient
и RawAsyncAdapter. C++17; SDK/RTOS сюда не включаются. Core AudioPacketView
заимствует лишь used typed records; он не является publication authority.
ServiceReady использует прежние Core APU/renderer declarations с согласованными
PUBLIC definitions. P4 и C6 — разные ELF, wire layout задаётся LE bytes/CRC,
а не передачей native C++ structs.

Владелец client/adapter — один Main. Callback API неблокирующий; один poll
ограничивает число transport start/poll. Complete требует CRC/full-key и
фактического TX sourceDone, а не только arrival. INIT/BIND/CONSUMED сохраняют
прежнюю последовательность; timeout sticky poison не разрешает reuse.
Ресурсы фиксированы caller spans/arrays, нет heap/задач/ROM copies. Проверка —
source/token parity и bounded native codec/client oracle; target и реальный
remote PCM проверяются отдельно. Новые product lifecycle здесь не добавлены.
