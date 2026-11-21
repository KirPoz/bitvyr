# Собственный Thin bridge

`ThinBridge.h/.cpp` — собственный bounded adapter поверх `nes::core::ThinPpu`,
ColourLease/Cut и событий. Он не содержит импортированную реализацию upstream PPU.
Его прежнее имя каталога vendor не являлось указанием сторонней лицензии.
Происхождение фиксируется source-preservation/миграционными hash; сторонние
copyright notices не удаляются и лицензия чужого кода здесь не переопределяется.

CPU1 producer и CPU0 consumer передают Begin/Prefix/Read/Commit. Обычный payload
доступен только после acquire, переиспользуется после противоположного release.
32-bit publication marker не заменяет полный64-bit serial/generation/time.
Published отличается от Ready; read response должен пройти exact receipt/key.

Caller выделяет ring и payload заранее, сохраняет ROM/арены до join и проверяет
HP/caps отдельно. Stop/fault/timeout удерживает неподтверждённое владение.
Native directed/model tests и Core MMIO regression дополняют SDK/HW validation;
adapter сам не выполняет allocation, RTOS wait, UART или чтение live Machine.
