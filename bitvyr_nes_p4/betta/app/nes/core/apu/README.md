# APU и PCM

`Apu` моделирует pulse/triangle/noise/DMC, envelope/sweep/length,
frame sequencer и IRQ NTSC/PAL. DMC получает настоящий байт и сообщает украденные
CPU-такты через bus; callback не продвигает APU рекурсивно.
`TimingApu` пишет timestamped `AudioPacket`; `AudioRenderer` потребляет sealed
пакет без доступа к изменяемому картриджу. I2S/DMA/кодек находятся вне Core.

Producer/consumer — разные владельцы. Полная identity/region/cut и непрерывность
пакетов проверяются до выдачи PCM; частичный/повторный пакет не становится новым
готовым кадром. Typed массивы ограничены, горячий renderer не выделяет память.
FIR сохраняет целочисленную дробную sample-фазу; `-ffp-contract=off` исключает
зависимость фильтра от неявного FMA. Ошибка capacity/key оставляет явный отказ.

Выбранный портативный профиль усредняет inaudible MMC5 carrier и native triangle;
это намеренное отличие PCM от точного raw-профиля, а не изменение CPU/APU IRQ/DMC.
Selected MMC5 DAC cache обновляется только при влияющих событиях.
Сроки real sample/frame/DMC остаются границами продвижения.

Независимые raw scalar fixtures и active packet/triangle/MMC5 fixtures проверяют
timing, sample count, переходы, invalidation и PCM. Слышимая непрерывность I2S,
громкость/mute и физическое качество проверяются отдельно. C6 DSP в P4-линии нет.
