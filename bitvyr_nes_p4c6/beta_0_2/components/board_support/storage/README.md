# Аппаратная microSD

[Storage.cpp](Storage.cpp) монтирует `/sdcard`: SDMMC slot0, 4 линии,
CLK43/CMD44/D0–D3 GPIO39–42/LDO4. C6 использует отдельный SDIO slot1.
Пины/частота не меняются из-за ошибки FS.

Main вызывает mount/refresh после закрытия App-файлов. Board владеет slot/LDO/mount.
Ошибка cleanup удерживает ресурс, не разрешая повторную регистрацию.
`format_if_mount_failed=false`: нет автоформатирования/изменения ROM.

Failure metadata диагностика ограниченно читает секторные сигнатуры и освобождает
временный slot; это не fsck. MBR/FAT32 — проверенный формат, иные карты требуют
фактической проверки. SDK/mount/rescan и synthetic App ZIP проверяются раздельно.

[Архитектура](../../../docs/02-architecture.md) · [Сборка и использование](../../../docs/03-guide.md).
