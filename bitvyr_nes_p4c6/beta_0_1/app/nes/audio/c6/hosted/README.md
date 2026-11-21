# Pinned Hosted SDIO overlay

`p4_sdio_drv.c` основан на Espressif esp-hosted-mcu2.13.1, commit и SHA закреплены
в provenance.json; SPDX/Apache-2.0 и LICENSE сохранены. Это upstream-derived
driver, а не собственная SDK реализация. `sdio_drv.patch` описывает изменения
от текущего pinned managed driver. Managed/global SDK этим переносом не менялись.

Root attach обязан проверить версию/commit/upstream SHA, исключить ровно один
stock sdio_drv.c и добавить эту копию с прежними include/definitions. Два driver
в одном target запрещены. P4RawRx требует --wrap=serial_ll_rx_handler. RawLimits
и RawPort включаются из единственных protocol/transport экземпляров.

SERIAL1 использует bounded request1400/reply1792 и header64, actual Hosted
header12 проверяется static_assert. RX проверяет remaining/offset/capacity до
dereference/checksum, copy block2048. До первого INIT/tasks cold резервируется
20×2048 bytes на stream (40960; два при double buffering), без hot realloc;
peer TX queue должен быть известен и <=20. TX содержит два envelope1536/align64.
На OOM освобождаются только известные cold allocations. HCI/SERIAL0/protobuf и
приоритет HCI сохраняются; raw TX идёт в lower OTHERS queue с timeout0.
sourceDone возвращает источник driver, не подтверждает remote command/join.
Нужны отдельные target HP/stack/IRQ/coexistence и actual wire/runtime проверки.
