# Прикладной runtime NES

`NesApp` — Main/CPU0 координатор меню, ROM и сеанса. Его private implementation
разделена на `runtime/app/*.inl`, включаемые ровно один раз из `NesApp.cpp`.
Это один TU: порядок методов, владельцы, аллокации и задачи не меняются из-за
разбиения. Заголовок содержит внешний контракт, `.inl` — алгоритм и причины отказа.

`runtime/frame` обслуживает non-MMC5 Frame Output, `runtime/coupled` — MMC5 Thin,
`runtime/bridge` — собственный bounded канал. CPU1 единолично исполняет primary
Machine; Main читает только опубликованные данные либо присоединённый срез.
`runtime/app/Worker.inl` управляет start/done semaphore, `Session.inl` — поколениями
и остановкой, `FrameDispatch.inl` — dispatch/retire и различением N/N+1.

Worker группирует чистые операции 6502 и оценку спрайтов по маске Y.
Main выполняет FM VRC7 и продвигает C6 между частями цветной обработки.
Для готовности кадра требуется полученный semaphore done. Ожидание ограничено
абсолютным сроком и продолжает обслуживать аудио предыдущего кадра.

C6 audio использует PREACK-пакет для Thin и POSTJOIN-пакет для Frame. Full-key,
sourceDone, PCM phase/count и checked UNBIND — обязательные проверки, а не
отключаемая диагностика. После pause сохраняется история; новая ROM допускается
только после настоящего join/drain и освобождения прежних заимствований.

Main владеет библиотекой SD/ZIP, настройками NVS, синхронным GT911, UART input
movie, Board RGB и PPA/LCD. RGB нельзя менять до source completion PPA; native
LCD-буфер имеет отдельное подтверждение. Горячий цикл не создаёт новые ROM,
framebuffer, задачи или неограниченные очереди. Main/Worker stack reservations
сохраняются16384/12288B и требуют actual watermark после SDK-сборки.

Проверки: portable Core/ZIP/UI/PCM suite и actual Frame fixture с полным CPU/RAM/RGB,
early sourceDone, slot reuse и неверным ticket. Heap/RTOS/SDIO/PPA на HOST моделируются;
аппаратные свойства и FPS устанавливаются отдельно на actual firmware.
