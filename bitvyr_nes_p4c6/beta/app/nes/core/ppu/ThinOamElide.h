#pragma once
/**
 * @file ThinOamElide.h
 * @brief Частный предикат ненаблюдаемой локальной OAM в основном сигнальном MMC5.
 * @details Реальные MMIO/DMA и PPUSTATUS обслуживает полный PPU. Чужой PPU,
 * packet/capture и остальные мапперы сохраняют прежнюю локальную OAM.
 */
#define NES_PRIMARY_MMC5_OAM_UNUSED() \
    (this==&bus_.timingPpu_&&bus_.thinSignalMode()&&bus_.cartridge_.mapper()==5U&&!packet_&&!capture_)
