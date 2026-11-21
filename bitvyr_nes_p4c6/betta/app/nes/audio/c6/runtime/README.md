# Main session и Worker publication

AudioPublished — единственный release/acquire handoff; BatchMainSession и
MainOwner — Main-only, PcmQueue сохраняет два PCM ticket с точными zero/partial
acceptance offsets. RemoteLoan принадлежит Worker и сохраняет once stage/ACK/
finalize без local DSP. Он зависит от прежнего Coupled ThinBridge API; перенос
header не подключает и не изменяет этот API. Main не читает live primary.

Два caller PCM spans живут до actual drain; wire/дополнительный PCM выделяются
cold в PSRAM, control/Channel проверяются HP на обоих концах. Нет новых задач,
hot allocation или неограниченных очередей. Смена владельца требует joined N и
sourceReleased того же N; reply/TXdone/finished/timeout не заменяют completion.
Boot-long owner/session переиспользуется только после проверенного UNBIND
полного старого binding и пустых очередей. На fault он остаётся Poisoned
с сохранением ресурсов до реального завершения/reset.
Native client/publication проверки и actual remote PCM/lifecycle — разные gates.

`PostJoinFrameAudio` связывает прежние два Frame Payload с тем же
AudioPublished/BatchMainSession. Main сохраняет полный lease/ticket уже
завершённого кадра перед запуском следующего Worker job. Физический Frame slot
и логический номер публикации — разные поля; output epoch не подменяет C6 epoch.
Возврат источника разрешён после sourceDone/Joined и освобождения публикации.
Код не читает изменяемую Machine и не добавляет копию аудиопакета, задачу или
неограниченную очередь. Нативно проверены полные fixed PCM и отказные binding;
аппаратная приёмка относится к конкретному образу и маршруту отдельно.

## CPU0 FM: двухфазная публикация

[FrameFmBridge](FrameFmBridge.h) использует тот же единственный PostJoin loan-owner.
reserveJoined после настоящего Worker join лишь удерживает rawN и cached TapeN.
После dispatchN+1 incremental [FrameFmPacket](FrameFmPacket.h) воспроизводит FM
по [FrameFmTimeline](FrameFmTimeline.h): максимум512M2/128native records за квант,
pump между квантами и каждые128records preflight/copy. Исходный абсолютный deadline
FrameN не продлевается. Тайминг публикации, native-prefix и DMC expansionLevel
восстанавливаются до передачи через прежний C6 wire.

Main-owned Header/DMC/Expansion находятся в отдельной bounded PSRAM-арене
[allocator](FrameFmBridgeAllocation.h); native Inputs остаются borrowed из rawN.
Ready Payload не меняется. Building и Ready отличаются от Published: пустая
AudioPublished до публикации не разрешает recycle. Только exact published-key
после реальных DATA/TX sourceDone+matching joined освобождает raw ticket/Tape,
затем derived storage. PCM-ready, timeout и video accept этого права не дают.

Пауза сохраняет FM phase. Новая ROM/reset требуют полного существующего drain,
UNBIND и joined teardown до нового cold FM. Fault сохраняет все loans и модель;
локальный повтор/rollback частично пройденного кадра запрещён. Non85 публикует
исходный AudioPacket прежним путём без FM-арены. Новых задач и C6 wire-format нет.

App `dsp_us` для opt-in85 включает wall FM-build и cooperative pump; его нельзя
выдавать за exclusive synthesis time или суммировать с перекрывающимися этапами.
Native-тесты проверяют алгоритм и владение; actual SDK/ABI/стек, PCM/I2S и FPS
по-прежнему требуют аппаратной проверки конкретного образа.
