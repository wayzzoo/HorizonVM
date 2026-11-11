#ifndef HORIZONVM_MMIO_H
#define HORIZONVM_MMIO_H

#include <stdbool.h>
#include <stdint.h>

#include "devices/timer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hvm_timer_t timer;
} hvm_mmio_t;


bool hvm_mmio_init(hvm_mmio_t *mmio);


bool hvm_mmio_read32(
    hvm_mmio_t *mmio,
    uint64_t address,
    uint32_t *value
);

#ifdef __cplusplus
}
#endif

#endif