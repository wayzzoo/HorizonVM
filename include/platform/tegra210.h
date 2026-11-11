
#ifndef HORIZONVM_TEGRA210_H
#define HORIZONVM_TEGRA210_H

#include <stdint.h>

/*
 * NVIDIA Tegra X1 (T210)
 * Original Nintendo Switch hardware platform.
 *
 * Physical addresses, not host virtual addresses.
 */

/* Memory */
#define T210_IRAM_BASE       UINT64_C(0x40000000)
#define T210_DRAM_BASE       UINT64_C(0x80000000)

/* Interrupt controller */
#define T210_GICD_BASE       UINT64_C(0x50041000)
#define T210_GICC_BASE       UINT64_C(0x50042000)

/* Timers */
#define T210_TIMER_BASE       UINT64_C(0x60005000)
#define T210_TIMER_SIZE       UINT64_C(0x400)
#define T210_TIMERUS_CNTR     UINT64_C(0x10)
#define T210_TIMERUS_USEC_CFG UINT64_C(0x14)
#define T210_TIMER_FREQUENCY  UINT64_C(1000000)

/* Serial controller */
#define T210_UARTA_BASE      UINT64_C(0x70006000)

#endif
