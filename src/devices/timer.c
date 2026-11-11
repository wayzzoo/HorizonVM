#include "devices/timer.h"
#include "platform/tegra210.h"

#include <mach/kern_return.h>
#include <mach/mach_time.h>
#include <stddef.h>


bool hvm_timer_init(hvm_timer_t *timer) {
    if (timer == NULL) {
        return false;
    }

    timer->initialized = false;

    if (mach_timebase_info(&timer->timebase) != KERN_SUCCESS ||
        timer->timebase.denom == 0) {
        return false;
    } 

    timer->start_ticks = mach_absolute_time();
    timer->initialized = true;

    return true;
}

bool hvm_timer_read(
    const hvm_timer_t *timer,
    uint64_t address,
    uint32_t *value
) {
    if (timer == NULL || value == NULL ||
        !timer->initialized) {
        return false;
    }

    if (address != T210_TIMER_BASE + T210_TIMERUS_CNTR) {
        return false;
    }

    uint64_t elapsed_ticks =
        mach_absolute_time() - timer->start_ticks;

    __uint128_t nanoseconds =
        (__uint128_t)elapsed_ticks * timer->timebase.numer;

    nanoseconds /= timer->timebase.denom;

    uint64_t microseconds =
        (uint64_t)(nanoseconds / 1000);

    *value = (uint32_t)microseconds;

    return true;
}