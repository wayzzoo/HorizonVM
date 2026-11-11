#ifndef HORIZONVM_TIMER_H
#define HORIZONVM_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <mach/mach_time.h>

typedef struct {
    uint64_t start_ticks;
    mach_timebase_info_data_t timebase;
    bool initialized;
} hvm_timer_t;


bool hvm_timer_init(hvm_timer_t *timer);

bool hvm_timer_read(
    const hvm_timer_t *timer,
    uint64_t address,
    uint32_t *value
);


#endif