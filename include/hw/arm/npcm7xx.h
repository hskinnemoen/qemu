/*
 * Nuvoton NPCM7xx SoC family.
 *
 * Copyright 2020 Google LLC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */
#ifndef NPCM7XX_H
#define NPCM7XX_H

#include "hw/boards.h"
#include "hw/cpu/a9mpcore.h"
#include "hw/misc/npcm7xx_clk.h"
#include "hw/misc/npcm7xx_gcr.h"
#include "hw/timer/npcm7xx_timer.h"
#include "target/arm/cpu.h"

#define NPCM7XX_MAX_NUM_CPUS    (2)

/* The first half of the address space is reserved for DDR4 DRAM. */
#define NPCM7XX_DRAM_BA         (0x00000000)
#define NPCM7XX_DRAM_SZ         (2 * GiB)

/* Magic addresses for setting up direct kernel booting and SMP boot stubs. */
#define NPCM7XX_LOADER_START            (0x00000000)  /* Start of SDRAM */
#define NPCM7XX_SMP_LOADER_START        (0xffff0000)  /* Boot ROM */
#define NPCM7XX_SMP_BOOTREG_ADDR        (0xf080013c)  /* GCR.SCRPAD */
#define NPCM7XX_GIC_CPU_IF_ADDR         (0xf03fe100)  /* GIC within A9 */

typedef struct NPCM7xxState {
    DeviceState         parent;

    ARMCPU              cpu[NPCM7XX_MAX_NUM_CPUS];
    A9MPPrivState       a9mpcore;

    MemoryRegion        sram;
    MemoryRegion        irom;
    MemoryRegion        ram3;
    MemoryRegion        *dram;

    NPCM7xxGCRState     gcr;
    NPCM7xxCLKState     clk;
    NPCM7xxTimerCtrlState tim[3];
} NPCM7xxState;

#define TYPE_NPCM7XX    "npcm7xx"
#define NPCM7XX(obj)    OBJECT_CHECK(NPCM7xxState, (obj), TYPE_NPCM7XX)

#define TYPE_NPCM730    "npcm730"
#define TYPE_NPCM750    "npcm750"

typedef struct NPCM7xxClass {
    DeviceClass         parent;

    /* Bitmask of modules that are permanently disabled on this chip. */
    uint32_t            disabled_modules;
    /* Number of CPU cores enabled in this SoC class (may be 1 or 2). */
    uint32_t            num_cpus;
} NPCM7xxClass;

#define NPCM7XX_CLASS(klass)                                            \
    OBJECT_CLASS_CHECK(NPCM7xxClass, (klass), TYPE_NPCM7XX)
#define NPCM7XX_GET_CLASS(obj)                                          \
    OBJECT_GET_CLASS(NPCM7xxClass, (obj), TYPE_NPCM7XX)

/**
 * npcm7xx_write_secondary_boot - Write stub for booting secondary CPU.
 * @cpu: The CPU to be booted.
 * @info: Boot info structure for the board.
 *
 * This will write a short code stub to the internal ROM that will keep the
 * secondary CPU spinning until the primary CPU writes an address to the SCRPAD
 * register in the GCR, after which the secondary CPU will jump there.
 */
extern void npcm7xx_write_secondary_boot(ARMCPU *cpu,
                                         const struct arm_boot_info *info);

#endif /* NPCM7XX_H */
