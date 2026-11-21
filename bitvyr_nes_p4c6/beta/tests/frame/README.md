# Actual Frame ownership fixture

FrameOwnershipTests исполняет текущий FrameVideoPipeline и Core на четырёх
M0/M7 NTSC/PAL сценариях,24frames. Проверяются полный CPU/RAM/RGB, immutable
audio view, все поля ticket, ранний sourceDone, запрет double borrow/release,
удержание slots и отказ destroy до освобождения. Неверный ключ завершается73
через stderr/exit, без CRT assert dialog. SyntheticImage — собственный минимальный
генератор тестовой программы, не пользовательская ROM.

SDK mocks заменяют только allocator/caps/time/logging. Они не доказывают HP/DMA,
RTOS/SDIO/PPA, физический sourceDone или FPS. Ожидаемый SHA полного byte stream
является самостоятельным regression golden; файл из старого build для теста не нужен.
