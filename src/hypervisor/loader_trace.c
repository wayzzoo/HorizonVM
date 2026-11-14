#include "hypervisor/loader_trace.h"
#include "devices/gicv2.h"
#include "hypervisor/memory.h"
#include "hypervisor/vcpu.h"
#include "hypervisor/vm.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <inttypes.h>
#include <libkern/OSCacheControl.h>
#include <os/lock.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const hv_ipa_t reference_image_base = 0x40200000;
static const hv_ipa_t virt_kernel_base = 0x40080000;
static const int64_t guest_timeout_ns = 2000000000;
static const uint64_t el1h_masked = 0x3c5;
/* QEMU virt's 4 GiB DRAM starts at 0x40000000. Mesosphère places the
   initial-process header 12 MiB below its end. */
static const hv_ipa_t virt_ini_destination = 0x13f400000;
static const size_t virt_ini_reserve = 12 * 1024 * 1024;
static const size_t loader_working_ram = 40 * 1024 * 1024;

static bool check(const char *operation, hv_return_t result) {
    if (result == HV_SUCCESS) {
        return true;
    }
    fprintf(stderr, "[HVM] %s failed: %#x\n", operation, (unsigned int)result);
    return false;
}

/* Only the vCPU's owner runs it. The watchdog can request an asynchronous exit. */
static bool run_until(hvm_vcpu_t *cpu, int64_t delay_ns, bool timer_deadline, bool *timer_woke) {
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    dispatch_group_t watchdog = dispatch_group_create();
    const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, delay_ns);
    __block bool timer_fired = false;
    dispatch_group_async(watchdog, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (dispatch_semaphore_wait(finished, deadline)) {
            hv_vcpu_t handle = cpu->handle;
            timer_fired = timer_deadline;
            if (!timer_deadline) {
                fprintf(stderr, "[HVM] Guest deadline reached\n");
            }
            if (!check("cancel guest execution", hv_vcpus_exit(&handle, 1))) {
                _Exit(EXIT_FAILURE);
            }
        }
    });
    hv_return_t result = hv_vcpu_run(cpu->handle);
    dispatch_semaphore_signal(finished);
    dispatch_group_wait(watchdog, DISPATCH_TIME_FOREVER);
    dispatch_release(watchdog);
    dispatch_release(finished);
    if (timer_woke) {
        *timer_woke = timer_fired;
    }
    return check("run guest", result);
}

static bool run_bounded(hvm_vcpu_t *cpu) {
    return run_until(cpu, guest_timeout_ns, false, NULL);
}

static bool enable_breakpoints(hvm_vcpu_t *cpu) {
    /* MDSCR.MDE/KDE enable EL1 debug. BCR0 matches a full EL1 instruction. */
    return check("enable debug traps", hv_vcpu_set_trap_debug_exceptions(cpu->handle, true)) &&
           check("set MDSCR", hv_vcpu_set_sys_reg(cpu->handle, HV_SYS_REG_MDSCR_EL1, (1 << 15) | (1 << 13))) &&
           check("enable breakpoint", hv_vcpu_set_sys_reg(cpu->handle, HV_SYS_REG_DBGBCR0_EL1,
                                                         (0xf << 5) | (1 << 1) | 1));
}

static bool snapshot(hvm_vcpu_t *cpu, const char *label, uint64_t expected_pc,
                     uint64_t *pc, uint64_t *state, uint64_t args[4]) {
    const hv_reg_t arg_regs[] = {HV_REG_X0, HV_REG_X1, HV_REG_X2, HV_REG_X30};
    uint64_t x3;
    if (!check("read X3", hv_vcpu_get_reg(cpu->handle, HV_REG_X3, &x3)) ||
        !check("read PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, pc)) ||
        !check("read PSTATE", hv_vcpu_get_reg(cpu->handle, HV_REG_CPSR, state))) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!check("read argument register", hv_vcpu_get_reg(cpu->handle, arg_regs[i], &args[i]))) {
            return false;
        }
    }
    printf("%s: PC=%#" PRIx64 " PSTATE=%#" PRIx64 " X0=%#" PRIx64
           " X1=%#" PRIx64 " X2=%#" PRIx64 " X3=%#" PRIx64 " X30=%#" PRIx64 "\n",
           *pc == expected_pc ? label : "Unexpected guest stop",
           *pc, *state, args[0], args[1], args[2], x3, args[3]);
    const hv_sys_reg_t regs[] = {
        HV_SYS_REG_SCTLR_EL1, HV_SYS_REG_TCR_EL1, HV_SYS_REG_TTBR0_EL1,
        HV_SYS_REG_TTBR1_EL1, HV_SYS_REG_MAIR_EL1, HV_SYS_REG_VBAR_EL1,
        HV_SYS_REG_ESR_EL1, HV_SYS_REG_ELR_EL1, HV_SYS_REG_FAR_EL1,
    };
    const char *names[] = {"SCTLR", "TCR", "TTBR0", "TTBR1", "MAIR", "VBAR", "ESR", "ELR", "FAR"};
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint64_t value;
        if (!check(names[i], hv_vcpu_get_sys_reg(cpu->handle, regs[i], &value))) {
            return false;
        }
        printf(" %s_EL1=%#" PRIx64, names[i], value);
    }
    putchar('\n');
    return true;
}

/* The M1 framework traps reads of CNTPCT_EL0 to the host. QEMU virt's
   Mesosphère build expects a 19.2 MHz counter. Derive it from monotonic
   uptime so reads progress at the advertised board rate. */
static uint64_t physical_counter_ticks(void) {
    return (__uint128_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 19200000 / 1000000000;
}

static bool service_physical_counter(hvm_vcpu_t *cpu, uint64_t esr) {
    const unsigned rt = (esr >> 5) & 31;
    uint64_t pc;
    if (!check("read trapped instruction PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, &pc))) {
        return false;
    }
    const uint64_t ticks = physical_counter_ticks();
    if (rt != 31 &&
        !check("return CNTPCT_EL0 value", hv_vcpu_set_reg(cpu->handle, HV_REG_X0 + rt, ticks))) {
        return false;
    }
    return check("resume after CNTPCT_EL0 read", hv_vcpu_set_reg(cpu->handle, HV_REG_PC, pc + 4));
}

typedef struct {
    dispatch_semaphore_t ready;
    dispatch_semaphore_t release;
    dispatch_semaphore_t complete;
    dispatch_group_t owner;
    bool reached_checkpoint;
    bool reached_main;
    bool reached_smp_checkpoint;
    bool destroyed;
} secondary_vcpu_t;

typedef struct {
    secondary_vcpu_t secondary[3];
    uint64_t image_base;
    uint64_t kernel_end;
    uint64_t ram_end;
    uint64_t secondary_checkpoint;
    uint64_t main_checkpoint;
    uint64_t smp_checkpoint;
    uint64_t write0_pc;
    bool timer_smoke;
    uint64_t svc_checkpoint;
    uint64_t svc_offset;
    bool ipc_probe;
    uint64_t ipc_server_ttbr;
    uint64_t ipc_client_ttbr;
    bool ipc_reply_seen;
    atomic_bool stop_observed;
    const hvm_memory_t *memory;
    const hvm_memory_t *ini_memory;
    os_unfair_lock gic_lock;
    dispatch_semaphore_t interrupt_ready[HVM_GICV2_CPUS];
    struct {
        hv_vcpu_t handle;
        bool running;
        bool irq_kick;
    } irq_targets[HVM_GICV2_CPUS];
    hvm_gicv2_t gic;
    unsigned sgi_acks[HVM_GICV2_CPUS];
    unsigned timer_acks[HVM_GICV2_CPUS];
    unsigned timer_eois[HVM_GICV2_CPUS];
    struct {
        uint64_t compare;
        unsigned control;
    } physical_timer[HVM_GICV2_CPUS];
} smp_trace_t;

static bool service_cpu_on(smp_trace_t *smp, hvm_vcpu_t *primary);

static bool run_to_breakpoint(hvm_vcpu_t *cpu, const char *label, uint64_t address,
                              uint64_t args[4], bool allow_counter, smp_trace_t *smp) {
    uint64_t pc, state;
    if (!check("set breakpoint address", hv_vcpu_set_sys_reg(cpu->handle, HV_SYS_REG_DBGBVR0_EL1, address))) {
        return false;
    }
    size_t counter_reads = 0;
    while (true) {
        if (!run_bounded(cpu)) {
            return false;
        }
        const hv_vcpu_exit_t *current = cpu->exit_info;
        if (!current) {
            fprintf(stderr, "[HVM] Missing vCPU exit information\n");
            return false;
        }
        if (smp && current->reason == HV_EXIT_REASON_EXCEPTION &&
            current->exception.syndrome == UINT64_C(0x5e000000)) {
            if (!service_cpu_on(smp, cpu)) {
                snapshot(cpu, "Unhandled SMC", 0, &pc, &state, args);
                return false;
            }
            continue;
        }
        if (!allow_counter || current->reason != HV_EXIT_REASON_EXCEPTION ||
            (current->exception.syndrome & ~(UINT64_C(31) << 5)) != UINT64_C(0x6232f801)) {
            break;
        }
        if (++counter_reads > 4096) {
            fprintf(stderr, "[HVM] Physical counter exit budget exceeded\n");
            return false;
        }
        if (!service_physical_counter(cpu, current->exception.syndrome)) {
            return false;
        }
    }
    if (allow_counter) {
        printf("[HVM] CNTPCT_EL0 reads serviced: %zu\n", counter_reads);
    }
    if (!snapshot(cpu, label, address, &pc, &state, args)) {
        return false;
    }
    const hv_vcpu_exit_t *exit = cpu->exit_info;
    printf(" VM exit=%u", (unsigned)exit->reason);
    if (exit->reason == HV_EXIT_REASON_EXCEPTION) {
        printf(" syndrome=%#" PRIx64 " VA=%#" PRIx64 " IPA=%#" PRIx64,
               exit->exception.syndrome, exit->exception.virtual_address, exit->exception.physical_address);
    }
    putchar('\n');
    const uint64_t ec = exit->reason == HV_EXIT_REASON_EXCEPTION
        ? (exit->exception.syndrome >> 26) & 0x3f : 0;
    if ((ec != 0x30 && ec != 0x31) ||
        pc != address || (state & 0xf) != 5) {
        fprintf(stderr, "[HVM] Expected an EL1h hardware breakpoint at %#" PRIx64 "\n", address);
        return false;
    }
    return true;
}

/* The kernel clears its hardware debug registers in InvokeMain. Use a
   software breakpoint just for the final entry and let those writes execute. */
static bool run_to_main(hvm_vcpu_t *cpu, uint64_t address, uint64_t affinity) {
    uint64_t pc, state, regs[4];
    if (!check("disable debug exception traps", hv_vcpu_set_trap_debug_exceptions(cpu->handle, false)) ||
        !check("disable host hardware breakpoint",
               hv_vcpu_set_sys_reg(cpu->handle, HV_SYS_REG_DBGBCR0_EL1, 0)) ||
        !check("allow guest debug register access", hv_vcpu_set_trap_debug_reg_accesses(cpu->handle, false))) {
        return false;
    }
    if (!run_bounded(cpu)) {
        return false;
    }
    /* HVC exits report the next PC. The marker sits after the first
       unmodified HorizonKernelMain instruction. */
    const uint64_t resume_pc = address + 8;
    if (!snapshot(cpu, "After first HorizonKernelMain instruction", resume_pc, &pc, &state, regs)) {
        return false;
    }
    const hv_vcpu_exit_t *exit = cpu->exit_info;
    if (!exit || exit->reason != HV_EXIT_REASON_EXCEPTION) {
        return false;
    }
    printf(" VM exit=%u syndrome=%#" PRIx64 "\n", (unsigned)exit->reason, exit->exception.syndrome);
    return pc == resume_pc && (state & 0xf) == 5 && regs[0] == affinity &&
           exit->exception.syndrome == UINT64_C(0x5a000480);
}

/* The GIC lock protects the active handles from being destroyed or
   re-entered while a peer requests an asynchronous VM exit. */
static bool kick_running_core_locked(smp_trace_t *smp, unsigned core) {
    if (smp->irq_targets[core].running && !smp->irq_targets[core].irq_kick) {
        hv_vcpu_t target = smp->irq_targets[core].handle;
        if (!check("wake running vCPU", hv_vcpus_exit(&target, 1))) {
            return false;
        }
        smp->irq_targets[core].irq_kick = true;
    }
    return true;
}

static bool stop_smp_peers(smp_trace_t *smp, unsigned core) {
    bool ok = true;
    os_unfair_lock_lock(&smp->gic_lock);
    for (unsigned other = 0; other < HVM_GICV2_CPUS; ++other) {
        if (other != core) {
            dispatch_semaphore_signal(smp->interrupt_ready[other]);
            ok = kick_running_core_locked(smp, other) && ok;
        }
    }
    os_unfair_lock_unlock(&smp->gic_lock);
    return ok;
}

static bool service_gic(smp_trace_t *smp, hvm_vcpu_t *cpu, unsigned core,
                        const hv_vcpu_exit_exception_t *fault) {
    const uint64_t esr = fault->syndrome;
    const uint64_t ipa = fault->physical_address;
    /* Only emulate a direct stage-2 translation abort with a valid A64
       load/store syndrome. Host RAM and guest page table walks are separate. */
    const unsigned fault_status = esr & 0x3f;
    if (((esr >> 26) & 0x3f) != 0x24 || !(esr & (1u << 24)) ||
        fault_status < 4 || fault_status > 7 || !(esr & (1u << 25)) ||
        (esr & ((1u << 21) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 7)))) {
        return false;
    }
    const unsigned width = 1u << ((esr >> 22) & 3);
    const unsigned rt = (esr >> 16) & 31;
    const bool write = (esr & (1u << 6)) != 0;
    uint64_t reg = 0, pc;
    if (write && rt != 31 &&
        !check("read GIC write operand", hv_vcpu_get_reg(cpu->handle, HV_REG_X0 + rt, &reg))) {
        return false;
    }
    uint32_t value = (uint32_t)reg;
    os_unfair_lock_lock(&smp->gic_lock);
    const bool handled = hvm_gicv2_access(&smp->gic, core, ipa, width, write, &value);
    bool wake_ok = true;
    if (handled && write && ipa == HVM_VIRT_GICD_BASE + 0xf00) {
        for (unsigned target = 0; target < HVM_GICV2_CPUS; ++target) {
            if (hvm_gicv2_irq_pending(&smp->gic, target)) {
                dispatch_semaphore_signal(smp->interrupt_ready[target]);
                if (target != core) {
                    wake_ok = kick_running_core_locked(smp, target) && wake_ok;
                }
            }
        }
    }
    os_unfair_lock_unlock(&smp->gic_lock);
    if (handled && !write && ipa == HVM_VIRT_GICC_BASE + 0xc) {
        if ((value & 0x3ff) < 16) {
            ++smp->sgi_acks[core];
        } else if (value == 30) {
            ++smp->timer_acks[core];
        }
    }
    if (!handled || !wake_ok ||
        (!write && rt != 31 &&
         !check("return GIC read value", hv_vcpu_set_reg(cpu->handle, HV_REG_X0 + rt, value))) ||
        !check("read GIC access PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, &pc)) ||
        !check("resume after GIC access", hv_vcpu_set_reg(cpu->handle, HV_REG_PC, pc + 4))) {
        return false;
    }
    if (write && ipa == HVM_VIRT_GICC_BASE + 0x10 && value == 30) {
        ++smp->timer_eois[core];
        if (smp->timer_smoke && !atomic_exchange(&smp->stop_observed, true)) {
            printf("[HVM] Core %u completed PPI 30, IAR acknowledgements=%u EOI=%u\n",
                   core, smp->timer_acks[core], smp->timer_eois[core]);
            return stop_smp_peers(smp, core);
        }
    }
    return true;
}

static bool service_physical_timer(smp_trace_t *smp, hvm_vcpu_t *cpu,
                                   unsigned core, uint64_t esr) {
    const uint64_t access = esr & ~(UINT64_C(31) << 5);
    if (access == UINT64_C(0x6232f801)) {
        return service_physical_counter(cpu, esr);
    }
    const bool read = (access & 1) != 0;
    const bool control = (access & ~UINT64_C(1)) == UINT64_C(0x6232f804);
    const bool compare = (access & ~UINT64_C(1)) == UINT64_C(0x6234f804);
    if (!control && !compare) {
        return false;
    }
    const unsigned rt = (esr >> 5) & 31;
    uint64_t value = 0, pc;
    if (read) {
        if (control) {
            value = smp->physical_timer[core].control;
            if ((value & 1) && physical_counter_ticks() >= smp->physical_timer[core].compare) {
                value |= 4;  /* ISTATUS */
            }
        } else {
            value = smp->physical_timer[core].compare;
        }
        if (rt != 31 && !check("return physical timer value",
                               hv_vcpu_set_reg(cpu->handle, HV_REG_X0 + rt, value))) {
            return false;
        }
    } else {
        if (rt != 31 && !check("read physical timer operand",
                               hv_vcpu_get_reg(cpu->handle, HV_REG_X0 + rt, &value))) {
            return false;
        }
        if (control) {
            smp->physical_timer[core].control = value & 3;
        } else {
            smp->physical_timer[core].compare = value;
        }
    }
    return check("read trapped timer PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, &pc)) &&
           check("resume after physical timer access", hv_vcpu_set_reg(cpu->handle, HV_REG_PC, pc + 4));
}

static void *translate_guest_va(hvm_vcpu_t *cpu, const hvm_memory_t *memory,
                                const hvm_memory_t *ini_memory, uint64_t va, size_t width);

static bool service_write0(smp_trace_t *smp, hvm_vcpu_t *cpu, uint64_t esr) {
    if (!smp->write0_pc || esr != UINT64_C(0x5a00f000)) {
        return false;
    }
    uint64_t pc, operation, address;
    if (!check("read semihosting PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, &pc)) ||
        !check("read semihosting operation", hv_vcpu_get_reg(cpu->handle, HV_REG_X0, &operation)) ||
        !check("read semihosting string", hv_vcpu_get_reg(cpu->handle, HV_REG_X1, &address)) ||
        pc != smp->write0_pc + 4 || operation != 4 || address < UINT64_C(0xffffff8000000000)) {
        return false;
    }
    char line[1024];
    size_t count = 0;
    while (count < sizeof(line)) {
        if (address > UINT64_MAX - count) {
            return false;
        }
        const uint64_t cursor = address + count;
        size_t chunk = 4096 - (cursor & 4095);
        if (chunk > sizeof(line) - count) {
            chunk = sizeof(line) - count;
        }
        const char *source = translate_guest_va(cpu, smp->memory, smp->ini_memory, cursor, chunk);
        if (!source) {
            return false;
        }
        const char *end = memchr(source, '\0', chunk);
        const size_t length = end ? (size_t)(end - source) : chunk;
        memcpy(line + count, source, length);
        count += length;
        if (end) {
            flockfile(stdout);
            fputs("[Mesosphère] ", stdout);
            for (size_t i = 0; i < count; ++i) {
                const unsigned char c = (unsigned char)line[i];
                putchar(c == '\n' || c == '\t' || (c >= 32 && c < 127) ? c : '?');
            }
            if (!count || line[count - 1] != '\n') {
                putchar('\n');
            }
            funlockfile(stdout);
            return true;
        }
    }
    fprintf(stderr, "[HVM] Unterminated semihosting string\n");
    return false;
}

/* Zero denotes an expired compare. Callers refresh the level line rather
   than scheduling another timer wakeup for an already expired compare. */
static int64_t timer_delay_ns(const smp_trace_t *smp, unsigned core) {
    if (smp->physical_timer[core].control != 1) {
        return INT64_MAX;
    }
    const uint64_t now = physical_counter_ticks();
    const uint64_t compare = smp->physical_timer[core].compare;
    if (compare <= now) {
        return 0;
    }
    const __uint128_t ns = ((__uint128_t)(compare - now) * 1000000000 + 19199999) / 19200000;
    return ns < INT64_MAX ? (int64_t)ns : INT64_MAX;
}

static bool irq_pending_locked(smp_trace_t *smp, unsigned core) {
    const bool timer_asserted = smp->physical_timer[core].control == 1 &&
                                physical_counter_ticks() >= smp->physical_timer[core].compare;
    hvm_gicv2_set_ppi(&smp->gic, core, 30, timer_asserted);
    return hvm_gicv2_irq_pending(&smp->gic, core);
}

static bool irq_pending(smp_trace_t *smp, unsigned core) {
    os_unfair_lock_lock(&smp->gic_lock);
    const bool pending = irq_pending_locked(smp, core);
    os_unfair_lock_unlock(&smp->gic_lock);
    return pending;
}

static bool enter_smp_guest(smp_trace_t *smp, hvm_vcpu_t *cpu, unsigned core,
                            int64_t delay_ns, bool timer_deadline, bool *irq_exit) {
    os_unfair_lock_lock(&smp->gic_lock);
    if (atomic_load(&smp->stop_observed)) {
        os_unfair_lock_unlock(&smp->gic_lock);
        *irq_exit = true;
        return true;
    }
    const bool pending = irq_pending_locked(smp, core);
    const bool configured = check("set GIC IRQ line",
                                  hv_vcpu_set_pending_interrupt(cpu->handle, HV_INTERRUPT_TYPE_IRQ, pending));
    smp->irq_targets[core].handle = cpu->handle;
    smp->irq_targets[core].running = configured;
    os_unfair_lock_unlock(&smp->gic_lock);
    if (!configured) {
        return false;
    }

    bool timer_woke;
    const bool ok = run_until(cpu, delay_ns, timer_deadline, &timer_woke);
    os_unfair_lock_lock(&smp->gic_lock);
    smp->irq_targets[core].running = false;
    const bool canceled = ok && cpu->exit_info && cpu->exit_info->reason == HV_EXIT_REASON_CANCELED;
    *irq_exit = canceled && (timer_woke || smp->irq_targets[core].irq_kick);
    if (canceled) {
        smp->irq_targets[core].irq_kick = false;
    }
    os_unfair_lock_unlock(&smp->gic_lock);
    return ok;
}

static bool service_wfi(smp_trace_t *smp, hvm_vcpu_t *cpu, unsigned core, uint64_t esr) {
    if (esr != UINT64_C(0x07e00000)) {
        return false;
    }
    /* WFI wakes on an asserted IRQ even if guest PSTATE masks exceptions. */
    const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, guest_timeout_ns);
    while (!irq_pending(smp, core)) {
        if (atomic_load(&smp->stop_observed)) {
            return false;
        }
        int64_t until_timer = timer_delay_ns(smp, core);
        if (until_timer == 0) {
            if (irq_pending(smp, core)) {
                break;
            }
            until_timer = INT64_MAX;
        }
        const dispatch_time_t timer_wakeup = dispatch_time(DISPATCH_TIME_NOW, until_timer);
        const dispatch_time_t wakeup = timer_wakeup < deadline ? timer_wakeup : deadline;
        if (dispatch_semaphore_wait(smp->interrupt_ready[core], wakeup) &&
            wakeup == deadline && !irq_pending(smp, core)) {
            fprintf(stderr, "[HVM] Core %u waited for an interrupt at WFI\n", core);
            return false;
        }
    }
    uint64_t pc;
    return check("read WFI PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, &pc)) &&
           check("advance after WFI", hv_vcpu_set_reg(cpu->handle, HV_REG_PC, pc + 4));
}

/* The marker replaced "sub sp, sp, #0x120" at the handler entry.
   Reproduce that one instruction and let Mesosphère dispatch ordinary SVCs. */
static bool resume_ipc_syscall(hvm_vcpu_t *cpu) {
    uint64_t sp;
    return check("read exception stack", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_SP_EL1, &sp)) &&
           sp >= UINT64_C(0xffffff8000000120) && !(sp & 15) &&
           check("restore SVC frame setup", hv_vcpu_set_sys_reg(cpu->handle, HV_SYS_REG_SP_EL1, sp - 0x120));
}

static const uint32_t *ipc_tls_words(smp_trace_t *smp, hvm_vcpu_t *cpu, uint64_t *tls) {
    if (!check("read IPC TLS", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_TPIDRRO_EL0, tls)) ||
        !*tls || (*tls & 0xf) || *tls >= (UINT64_C(1) << 39)) {
        return NULL;
    }
    return translate_guest_va(cpu, smp->memory, smp->ini_memory, *tls, 3 * sizeof(uint32_t));
}

static bool verify_ipc_reply(smp_trace_t *smp, hvm_vcpu_t *cpu) {
    uint64_t tls, reply_register, ttbr;
    if (!check("read reply word", hv_vcpu_get_reg(cpu->handle, HV_REG_X1, &reply_register)) ||
        !check("read client page table", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_TTBR0_EL1, &ttbr)) ||
        reply_register != UINT64_C(0xb791f3dd)) {
        return false;
    }
    const uint32_t *message = ipc_tls_words(smp, cpu, &tls);
    os_unfair_lock_lock(&smp->gic_lock);
    const uint64_t server = smp->ipc_server_ttbr, client = smp->ipc_client_ttbr;
    const bool observed = smp->ipc_reply_seen && server && client == ttbr &&
                          ((server ^ client) & UINT64_C(0xfffffffff000));
    os_unfair_lock_unlock(&smp->gic_lock);
    if (!observed || !message || message[0] != 0 || message[1] != 1 ||
        message[2] != UINT32_C(0xb791f3dd)) {
        return false;
    }
    printf("[HVM] IPC round trip verified, server TTBR0=%#" PRIx64 " client TTBR0=%#" PRIx64
           " client TLS=%#" PRIx64 " reply=0xb791f3dd\n", server, client, tls);
    return true;
}

static bool observe_ipc_syscall(smp_trace_t *smp, hvm_vcpu_t *cpu, unsigned syscall) {
    if (syscall != 0x71 && syscall != 0x21 && syscall != 0x43) {
        return true;
    }
    uint64_t ttbr, reply_target = 0;
    if (!check("read IPC process page table", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_TTBR0_EL1, &ttbr)) ||
        (syscall == 0x43 && !check("read IPC reply handle", hv_vcpu_get_reg(cpu->handle, HV_REG_X3, &reply_target)))) {
        return false;
    }
    if (syscall == 0x21 || reply_target) {
        uint64_t tls;
        const uint32_t *message = ipc_tls_words(smp, cpu, &tls);
        const bool request = syscall == 0x21;
        if (!message || message[0] != (request ? 4u : 0u) || message[1] != 1 ||
            message[2] != (request ? UINT32_C(0x12345678) : UINT32_C(0xb791f3dd))) {
            fprintf(stderr, "[HVM] Unexpected IPC %s payload\n", request ? "request" : "reply");
            return false;
        }
    }
    os_unfair_lock_lock(&smp->gic_lock);
    if (syscall == 0x71) {
        smp->ipc_server_ttbr = ttbr;
    } else if (syscall == 0x21) {
        smp->ipc_client_ttbr = ttbr;
    } else if (reply_target && ttbr == smp->ipc_server_ttbr && smp->ipc_client_ttbr) {
        smp->ipc_reply_seen = true;
    }
    os_unfair_lock_unlock(&smp->gic_lock);
    return true;
}

static bool handle_el0_svc(smp_trace_t *smp, hvm_vcpu_t *cpu, unsigned core, uint64_t esr) {
    if (!smp->svc_checkpoint || esr != UINT64_C(0x5a000482)) {
        return false;
    }
    uint64_t pc, elr, spsr, guest_esr, marker, sp;
    if (!check("read SVC checkpoint PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, &pc)) ||
        !check("read SVC guest syndrome", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_ESR_EL1, &guest_esr)) ||
        !check("read SVC return PC", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_ELR_EL1, &elr)) ||
        !check("read SVC saved PSTATE", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_SPSR_EL1, &spsr)) ||
        !check("read user stack", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_SP_EL0, &sp)) ||
        !check("read EL0 marker", hv_vcpu_get_reg(cpu->handle, HV_REG_X0, &marker))) {
        return false;
    }
    if (pc != smp->svc_checkpoint + 4 || (guest_esr >> 16) != 0x5600 ||
        (spsr & 0x1f) || elr < 8 || elr >= (UINT64_C(1) << 39) || (elr & 3) ||
        !sp || (sp & 15) || sp >= (UINT64_C(1) << 39)) {
        fprintf(stderr, "[HVM] Unexpected EL0 SVC state\n");
        return false;
    }
    const unsigned syscall = guest_esr & 0xffff;
    const uint64_t expected_marker = smp->ipc_probe ? 0x4950 : 0x4856;
    if (smp->ipc_probe && (syscall != 7 || marker != expected_marker)) {
        if (syscall == 7 && (marker == 0x4951 || marker == 0x4952)) {
            uint64_t result;
            if (check("read failed IPC result", hv_vcpu_get_reg(cpu->handle, HV_REG_X1, &result))) {
                fprintf(stderr, "[HVM] IPC %s validation failed, result=%#" PRIx64 "\n",
                        marker == 0x4951 ? "client" : "server", result);
            }
            return false;
        }
        return observe_ipc_syscall(smp, cpu, syscall) && resume_ipc_syscall(cpu);
    }
    if (syscall != 7 || marker != expected_marker || (!smp->ipc_probe && (elr & 0xfff) != 8)) {
        fprintf(stderr, "[HVM] Unexpected EL0 SVC state\n");
        return false;
    }
    const uint32_t *code = translate_guest_va(cpu, smp->memory, smp->ini_memory, elr - 8,
                                               2 * sizeof(uint32_t));
    const uint32_t mov_marker = UINT32_C(0xd2800000) | ((uint32_t)expected_marker << 5);
    if (!code || code[0] != mov_marker || code[1] != UINT32_C(0xd40000e1) ||
        (smp->ipc_probe && !verify_ipc_reply(smp, cpu))) {
        fprintf(stderr, "[HVM] EL0 completion validation failed\n");
        return false;
    }
    printf("[HVM] Core %u observed EL0 SVC #7, guest PC=%#" PRIx64 " SP_EL0=%#" PRIx64
           " SPSR_EL1=%#" PRIx64 " ESR_EL1=%#" PRIx64 " X0=%#" PRIx64 "\n",
           core, elr - 4, sp, spsr, guest_esr, marker);
    atomic_store(&smp->stop_observed, true);
    return stop_smp_peers(smp, core);
}

static bool run_to_smp_checkpoint(hvm_vcpu_t *cpu, smp_trace_t *smp, uint64_t affinity) {
    uint64_t pc, state, regs[4];
    if (!check("resume at restored kernel instruction",
               hv_vcpu_set_reg(cpu->handle, HV_REG_PC, smp->main_checkpoint + 4))) {
        return false;
    }
    size_t gic_accesses = 0, timer_accesses = 0;
    bool stopped_after_event = false;
    for (unsigned exits = 0; ; ++exits) {
        if (atomic_load(&smp->stop_observed)) {
            stopped_after_event = true;
            break;
        }
        const int64_t until_timer = timer_delay_ns(smp, (unsigned)affinity);
        const bool timer_deadline = until_timer > 0 && until_timer < guest_timeout_ns;
        bool irq_exit;
        if (!enter_smp_guest(smp, cpu, (unsigned)affinity, timer_deadline ? until_timer : guest_timeout_ns,
                             timer_deadline, &irq_exit)) {
            return false;
        }
        const hv_vcpu_exit_t *current = cpu->exit_info;
        if (irq_exit && exits < 4096) {
            continue;
        }
        if (!current || current->reason != HV_EXIT_REASON_EXCEPTION || exits == 4096) {
            break;
        }
        if (handle_el0_svc(smp, cpu, (unsigned)affinity, current->exception.syndrome)) {
            continue;
        }
        if (service_gic(smp, cpu, (unsigned)affinity, &current->exception)) {
            ++gic_accesses;
            continue;
        }
        if (service_physical_timer(smp, cpu, (unsigned)affinity, current->exception.syndrome)) {
            ++timer_accesses;
            continue;
        }
        if (service_write0(smp, cpu, current->exception.syndrome)) {
            continue;
        }
        if (service_wfi(smp, cpu, (unsigned)affinity, current->exception.syndrome)) {
            continue;
        }
        stopped_after_event = current->exception.syndrome == UINT64_C(0x07e00000) &&
                              atomic_load(&smp->stop_observed);
        break;
    }
    const hv_vcpu_exit_t *exit = cpu->exit_info;
    const uint64_t expected_pc = smp->smp_checkpoint + 4;
    flockfile(stdout);
    printf("[HVM] Core %" PRIu64 " checkpoint trace, GIC accesses=%zu, timer accesses=%zu, SGI acknowledgements=%u, PPI 30 acknowledgements=%u EOI=%u\n",
           affinity, gic_accesses, timer_accesses, smp->sgi_acks[affinity],
           smp->timer_acks[affinity], smp->timer_eois[affinity]);
    if (stopped_after_event) {
        funlockfile(stdout);
        return true;
    }
    const bool read_state = snapshot(cpu, "SMP checkpoint reached", expected_pc,
                                     &pc, &state, regs);
    if (exit && exit->reason == HV_EXIT_REASON_EXCEPTION) {
        printf(" syndrome=%#" PRIx64 " VA=%#" PRIx64 " IPA=%#" PRIx64 "\n",
               exit->exception.syndrome, exit->exception.virtual_address, exit->exception.physical_address);
    }
    funlockfile(stdout);
    return read_state && pc == expected_pc && (state & 0xf) == 5 &&
           exit && exit->reason == HV_EXIT_REASON_EXCEPTION &&
           exit->exception.syndrome == UINT64_C(0x5a000481);
}

static bool service_cpu_on(smp_trace_t *smp, hvm_vcpu_t *primary) {
    uint64_t function, affinity, entry, context, pc;
    if (!check("read PSCI function", hv_vcpu_get_reg(primary->handle, HV_REG_X0, &function)) ||
        !check("read PSCI affinity", hv_vcpu_get_reg(primary->handle, HV_REG_X1, &affinity)) ||
        !check("read PSCI entry", hv_vcpu_get_reg(primary->handle, HV_REG_X2, &entry)) ||
        !check("read PSCI context", hv_vcpu_get_reg(primary->handle, HV_REG_X3, &context)) ||
        !check("read SMC PC", hv_vcpu_get_reg(primary->handle, HV_REG_PC, &pc))) {
        return false;
    }
    if (function != UINT64_C(0xc4000003) || affinity < 1 || affinity > 3 ||
        entry < smp->image_base || entry > smp->kernel_end - 4 || entry % 4 ||
        context < smp->image_base || context > smp->ram_end - 40 || context % 8) {
        fprintf(stderr, "[HVM] Unsupported PSCI call or invalid CPU_ON parameters\n");
        return false;
    }
    secondary_vcpu_t *slot = &smp->secondary[affinity - 1];
    if (slot->owner) {
        fprintf(stderr, "[HVM] Repeated CPU_ON for affinity %" PRIu64 "\n", affinity);
        return false;
    }
    printf("[HVM] CPU_ON affinity=%" PRIu64 " entry=%#" PRIx64 " context=%#" PRIx64 "\n",
           affinity, entry, context);
    slot->ready = dispatch_semaphore_create(0);
    slot->release = dispatch_semaphore_create(0);
    slot->complete = dispatch_semaphore_create(0);
    slot->owner = dispatch_group_create();
    /* A vCPU is created, run and destroyed on the same owner thread. The
       worker parks at its checkpoint until the trace releases all vCPUs. */
    dispatch_group_async(slot->owner, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        hvm_vcpu_t secondary = {0};
        uint64_t regs[4], mpidr;
        const bool created = check("create secondary vCPU", hvm_vcpu_create(&secondary));
        if (created) {
            slot->reached_checkpoint =
                check("set secondary MPIDR", hv_vcpu_set_sys_reg(secondary.handle, HV_SYS_REG_MPIDR_EL1, affinity)) &&
                check("set secondary PSTATE", hv_vcpu_set_reg(secondary.handle, HV_REG_CPSR, el1h_masked)) &&
                check("set secondary entry", hv_vcpu_set_reg(secondary.handle, HV_REG_PC, entry)) &&
                check("set secondary context", hv_vcpu_set_reg(secondary.handle, HV_REG_X0, context)) &&
                enable_breakpoints(&secondary) &&
                run_to_breakpoint(&secondary, "Secondary passed StartOtherCore", smp->secondary_checkpoint,
                                  regs, true, NULL) &&
                check("read secondary MPIDR", hv_vcpu_get_sys_reg(secondary.handle, HV_SYS_REG_MPIDR_EL1, &mpidr)) &&
                (mpidr & 0xff) == affinity && regs[0] == affinity;
        }
        dispatch_semaphore_signal(slot->ready);
        if (created) {
            dispatch_semaphore_wait(slot->release, DISPATCH_TIME_FOREVER);
            if (smp->main_checkpoint) {
                slot->reached_main = run_to_main(&secondary, smp->main_checkpoint, affinity);
                dispatch_semaphore_signal(slot->complete);
                dispatch_semaphore_wait(slot->release, DISPATCH_TIME_FOREVER);
                if (smp->smp_checkpoint || smp->timer_smoke || smp->svc_checkpoint) {
                    slot->reached_smp_checkpoint = run_to_smp_checkpoint(&secondary, smp, affinity);
                    dispatch_semaphore_signal(slot->complete);
                    dispatch_semaphore_wait(slot->release, DISPATCH_TIME_FOREVER);
                }
            }
            slot->destroyed = check("destroy secondary vCPU", hvm_vcpu_destroy(&secondary));
        } else {
            slot->destroyed = true;
        }
    });
    dispatch_semaphore_wait(slot->ready, DISPATCH_TIME_FOREVER);
    if (!slot->reached_checkpoint) {
        fprintf(stderr, "[HVM] Secondary did not reach its checkpoint\n");
        return false;
    }
    return check("return PSCI success", hv_vcpu_set_reg(primary->handle, HV_REG_X0, 0)) &&
           check("advance past SMC", hv_vcpu_set_reg(primary->handle, HV_REG_PC, pc + 4));
}

static bool destroy_secondaries(smp_trace_t *smp) {
    bool ok = true;
    for (size_t i = 0; i < sizeof(smp->secondary) / sizeof(smp->secondary[0]); i++) {
        secondary_vcpu_t *slot = &smp->secondary[i];
        if (slot->owner) {
            dispatch_semaphore_signal(slot->release);
            dispatch_group_wait(slot->owner, DISPATCH_TIME_FOREVER);
            ok = slot->destroyed && ok;
            dispatch_release(slot->owner);
            dispatch_release(slot->complete);
            dispatch_release(slot->release);
            dispatch_release(slot->ready);
        }
    }
    return ok;
}

static void *ram_pointer(const hvm_memory_t *memory, uint64_t ipa, size_t width) {
    if (!memory || !memory->mapped || ipa < memory->guest_address || width > memory->size ||
        ipa - memory->guest_address > memory->size - width) {
        return NULL;
    }
    return (uint8_t *)memory->host_address + (ipa - memory->guest_address);
}

static void *guest_backing(const hvm_memory_t *memory, const hvm_memory_t *ini_memory,
                           uint64_t ipa, size_t width) {
    void *pointer = ram_pointer(memory, ipa, width);
    return pointer ? pointer : ram_pointer(ini_memory, ipa, width);
}

/* Walk the 39-bit, 4 KiB regime used by the relocated kernel and probe. */
static void *translate_guest_va(hvm_vcpu_t *cpu, const hvm_memory_t *memory,
                                const hvm_memory_t *ini_memory, uint64_t va, size_t width) {
    const uint64_t address_mask = UINT64_C(0xfffffffff000);
    const bool high = va >= UINT64_C(0xffffff8000000000);
    if (!high && va >= (UINT64_C(1) << 39)) {
        return NULL;
    }
    uint64_t tcr, table;
    if (!check("read translation control", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_TCR_EL1, &tcr)) ||
        !check("read translation table", hv_vcpu_get_sys_reg(cpu->handle,
               high ? HV_SYS_REG_TTBR1_EL1 : HV_SYS_REG_TTBR0_EL1, &table))) {
        return NULL;
    }
    const bool compatible = high ? ((tcr >> 16) & 0x3f) == 25 && ((tcr >> 30) & 3) == 2
                                 : (tcr & 0x3f) == 25 && ((tcr >> 14) & 3) == 0;
    if (!compatible) {
        return NULL;
    }
    for (int shift = 30; shift >= 12; shift -= 9) {
        const uint64_t index = (va >> shift) & 0x1ff;
        const uint64_t *entry = guest_backing(memory, ini_memory, (table & address_mask) + index * 8, 8);
        if (!entry || !(*entry & 1) || (shift == 12 && !(*entry & 2))) {
            return NULL;
        }
        table = *entry;
        if (shift == 12 || !(table & 2)) {
            const uint64_t offset_mask = (UINT64_C(1) << shift) - 1;
            const uint64_t offset = va & offset_mask;
            const uint64_t ipa = (table & address_mask & ~offset_mask) | offset;
            return width <= offset_mask + 1 - offset ? guest_backing(memory, ini_memory, ipa, width) : NULL;
        }
    }
    return NULL;
}

static uint32_t *main_instruction(hvm_vcpu_t *cpu, const hvm_memory_t *memory, uint64_t va) {
    return translate_guest_va(cpu, memory, NULL, va, sizeof(uint32_t));
}

static void print_kernel_fault_instruction(hvm_vcpu_t *cpu, const hvm_memory_t *memory) {
    uint64_t elr, esr;
    if (!check("read guest fault syndrome", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_ESR_EL1, &esr)) ||
        !check("read guest fault PC", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_ELR_EL1, &elr)) ||
        !esr || elr < UINT64_C(0xffffff8000000000) || (elr & 3)) {
        return;
    }
    const uint32_t *instruction = main_instruction(cpu, memory, elr);
    if (instruction) {
        printf("[HVM] Primary guest fault instruction at %#" PRIx64 ": %#010" PRIx32 "\n",
               elr, *instruction);
    }
}

static bool trace_main_all(hvm_vcpu_t *cpu, smp_trace_t *smp,
                           const hvm_memory_t *memory, uint64_t address, uint64_t checkpoint, uint64_t write0) {
    for (size_t i = 0; i < sizeof(smp->secondary) / sizeof(smp->secondary[0]); i++) {
        if (!smp->secondary[i].reached_checkpoint) {
            fprintf(stderr, "[HVM] All secondary vCPUs must reach the pre-main checkpoint\n");
            return false;
        }
    }
    const uint32_t *first = main_instruction(cpu, memory, address);
    uint32_t *instruction = main_instruction(cpu, memory, address + 4);
    if (!instruction) {
        fprintf(stderr, "[HVM] Could not translate HorizonKernelMain to stage-2 RAM\n");
        return false;
    }
    if (!first || *first != UINT32_C(0xa9bd7bfd)) {
        fprintf(stderr, "[HVM] Unexpected HorizonKernelMain prologue\n");
        return false;
    }
    /* HVC #0x480 at the second instruction. Every vCPU is parked. */
    const uint32_t original = *instruction;
    *instruction = UINT32_C(0xd4009002);
    sys_icache_invalidate(instruction, sizeof(*instruction));
    smp->main_checkpoint = address;
    bool ok = run_to_main(cpu, address, 0);
    for (size_t i = 0; i < sizeof(smp->secondary) / sizeof(smp->secondary[0]); i++) {
        secondary_vcpu_t *slot = &smp->secondary[i];
        dispatch_semaphore_signal(slot->release);
        dispatch_semaphore_wait(slot->complete, DISPATCH_TIME_FOREVER);
        ok = slot->reached_main && ok;
    }
    *instruction = original;
    sys_icache_invalidate(instruction, sizeof(*instruction));
    if (!ok || (!checkpoint && !smp->timer_smoke && !smp->svc_checkpoint)) {
        return ok;
    }

    const uint64_t marker = smp->svc_checkpoint ? smp->svc_checkpoint : checkpoint;
    uint32_t *checkpoint_instruction = marker ? main_instruction(cpu, memory, marker) : NULL;
    if (marker && !checkpoint_instruction) {
        fprintf(stderr, "[HVM] Could not translate the SMP checkpoint\n");
        return false;
    }
    uint32_t *console = NULL;
    if (write0) {
        console = main_instruction(cpu, memory, write0);
        if (!console || *console != UINT32_C(0xd45e0000) || console == checkpoint_instruction) {
            fprintf(stderr, "[HVM] Invalid semihosting instruction\n");
            return false;
        }
        *console = UINT32_C(0xd41e0002);  /* HVC #0xf000 */
        sys_icache_invalidate(console, sizeof(*console));
        smp->write0_pc = write0;
    }
    uint32_t saved = 0;
    if (checkpoint_instruction) {
        saved = *checkpoint_instruction;
        if (smp->svc_checkpoint && saved != UINT32_C(0xd10483ff)) {
            if (console) {
                *console = UINT32_C(0xd45e0000);
                sys_icache_invalidate(console, sizeof(*console));
            }
            fprintf(stderr, "[HVM] Unexpected AArch64 SVC handler prologue\n");
            return false;
        }
        *checkpoint_instruction = smp->svc_checkpoint ? UINT32_C(0xd4009042) : UINT32_C(0xd4009022);
        sys_icache_invalidate(checkpoint_instruction, sizeof(*checkpoint_instruction));
    }
    smp->smp_checkpoint = checkpoint;
    for (unsigned core = 0; core < HVM_GICV2_CPUS; ++core) {
        smp->interrupt_ready[core] = dispatch_semaphore_create(0);
    }
    for (size_t i = 0; i < sizeof(smp->secondary) / sizeof(smp->secondary[0]); i++) {
        dispatch_semaphore_signal(smp->secondary[i].release);
    }
    ok = run_to_smp_checkpoint(cpu, smp, 0);
    for (size_t i = 0; i < sizeof(smp->secondary) / sizeof(smp->secondary[0]); i++) {
        secondary_vcpu_t *slot = &smp->secondary[i];
        dispatch_semaphore_wait(slot->complete, DISPATCH_TIME_FOREVER);
        ok = slot->reached_smp_checkpoint && ok;
    }
    for (unsigned core = 0; core < HVM_GICV2_CPUS; ++core) {
        dispatch_release(smp->interrupt_ready[core]);
    }
    if (!ok) {
        print_kernel_fault_instruction(cpu, memory);
    }
    if (checkpoint_instruction) {
        *checkpoint_instruction = saved;
        sys_icache_invalidate(checkpoint_instruction, sizeof(*checkpoint_instruction));
    }
    if (console) {
        *console = UINT32_C(0xd45e0000);
        sys_icache_invalidate(console, sizeof(*console));
    }
    return ok && ((!smp->timer_smoke && !smp->svc_checkpoint) || atomic_load(&smp->stop_observed));
}

static bool trace_handoff(hvm_vcpu_t *cpu, uint64_t handoff, uint64_t link,
                          uint64_t image_base, uint64_t image_end, uint64_t kernel_offset,
                          uint64_t secondary_offset, uint64_t main_offset, uint64_t smp_offset, uint64_t write0_offset,
                          smp_trace_t *smp,
                          const hvm_memory_t *memory) {
    uint64_t before[4], after[4], destination, par, sp;
    if (!run_to_breakpoint(cpu, "Loader handoff branch", handoff, before, true, NULL) ||
        !check("read translated return address", hv_vcpu_get_reg(cpu->handle, HV_REG_X3, &destination)) ||
        !check("read PAR_EL1", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_PAR_EL1, &par)) ||
        !check("read kernel stack", hv_vcpu_get_sys_reg(cpu->handle, HV_SYS_REG_SP_EL1, &sp))) {
        return false;
    }
    printf("[HVM] Kernel handoff destination=%#" PRIx64 " PAR_EL1=%#" PRIx64
           " SP_EL1=%#" PRIx64 "\n", destination, par, sp);
    const uint64_t translated = (par & UINT64_C(0xfffffffff000)) | (link & 0xfff);
    if ((par & 1) || destination != translated || destination < image_base ||
        destination >= image_end || !sp || (sp & 15)) {
        fprintf(stderr, "[HVM] Invalid translated kernel handoff\n");
        return false;
    }
    if (!run_to_breakpoint(cpu, "Returned to kernel code", destination, after, false, NULL)) {
        return false;
    }
    if (memcmp(before, after, sizeof(before)) != 0) {
        fprintf(stderr, "[HVM] Kernel handoff branch changed argument registers\n");
        return false;
    }
    if (kernel_offset) {
        /* X1 is the difference between the relocated virtual and physical
           return PCs. The return site's image offset is invariant. */
        const uint64_t virtual_base = destination + after[1] - (link - image_base);
        const uint64_t checkpoint = virtual_base + kernel_offset;
        if (virtual_base < UINT64_C(0xffffff8000000000) || checkpoint < virtual_base) {
            fprintf(stderr, "[HVM] Invalid relocated kernel address\n");
            return false;
        }
        printf("[HVM] Relocated kernel base=%#" PRIx64 "\n", virtual_base);
        if (secondary_offset) {
            smp->secondary_checkpoint = virtual_base + secondary_offset;
        }
        if (!run_to_breakpoint(cpu, "Relocated kernel checkpoint", checkpoint, after, true,
                               secondary_offset ? smp : NULL)) {
            return false;
        }
        if (smp->svc_offset) {
            smp->svc_checkpoint = virtual_base + smp->svc_offset;
        }
        return !main_offset || trace_main_all(cpu, smp, memory, virtual_base + main_offset,
                                              smp_offset ? virtual_base + smp_offset : 0,
                                              write0_offset ? virtual_base + write0_offset : 0);
    }
    return true;
}

int hvm_trace_loader(char **args) {
    /* Image metadata and SHA-256 are checked by the trace script. The child
       receives exactly the bytes hashed by that script via stdin. */
    uint64_t values[15];
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char *end;
        errno = 0;
        values[i] = strtoull(args[i], &end, 0);
        if (errno || end == args[i] || *end || args[i][0] == '-') {
            return EXIT_FAILURE;
        }
    }
    const uint64_t size = values[0], layout = values[1], ini = values[2];
    const uint64_t loader = values[3], target = values[4], mmu_stop = values[5];
    const uint64_t handoff = values[6], kernel_offset = values[7], secondary_offset = values[8], main_offset = values[9];
    const uint64_t smp_offset = values[10], write0_offset = values[11];
    const bool timer_smoke = values[12] == 1;
    const uint64_t svc_offset = values[13];
    const bool ipc_probe = values[14] == 1;
    const hv_ipa_t image_base = mmu_stop ? virt_kernel_base : reference_image_base;
    if (size < 32 || size > 64 * 1024 * 1024 ||
        layout < image_base || layout >= ini || ini >= loader ||
        loader >= target || target > image_base + size - 4 ||
        ((loader | target | mmu_stop | handoff | kernel_offset | secondary_offset | main_offset | smp_offset | write0_offset | svc_offset) & 3) ||
        (mmu_stop && (mmu_stop <= target || mmu_stop > image_base + size - 4)) ||
        (handoff && (!mmu_stop || handoff <= target || handoff > image_base + size - 4)) ||
        (kernel_offset && (!handoff || kernel_offset > ini - image_base - 4)) ||
        (secondary_offset && (!kernel_offset || secondary_offset > ini - image_base - 4)) ||
        (main_offset && (!secondary_offset || secondary_offset != kernel_offset ||
                         main_offset > ini - image_base - 8)) ||
        (smp_offset && (!main_offset || smp_offset <= main_offset + 4 ||
                            smp_offset > ini - image_base - 4)) ||
        (write0_offset && ((!smp_offset && !timer_smoke && !svc_offset) || write0_offset > ini - image_base - 4)) ||
        values[12] > 1 || (timer_smoke && (!main_offset || smp_offset || svc_offset)) ||
        (svc_offset && (!main_offset || smp_offset || svc_offset > ini - image_base - 4)) ||
        values[14] > 1 || (ipc_probe && !svc_offset)) {
        return EXIT_FAILURE;
    }
    const size_t page = (size_t)getpagesize();
    size_t rounded_size = (size + page - 1) / page * page;
    if (mmu_stop && rounded_size < loader_working_ram) {
        rounded_size = loader_working_ram;
    }
    hvm_vcpu_t cpu = {0};
    hvm_memory_t memory = {0}, ini_header = {0};
    smp_trace_t smp = {.image_base = image_base, .kernel_end = ini,
                       .ram_end = image_base + rounded_size, .gic_lock = OS_UNFAIR_LOCK_INIT,
                       .memory = &memory, .ini_memory = &ini_header,
                       .timer_smoke = timer_smoke, .svc_offset = svc_offset, .ipc_probe = ipc_probe};
    atomic_init(&smp.stop_observed, false);
    if (!check("create VM", hvm_vm_create())) {
        return EXIT_FAILURE;
    }
    const bool has_cpu = check("create vCPU", hvm_vcpu_create(&cpu));
    bool ok = false;
    if (!has_cpu || !check("map image", hvm_memory_create(&memory, image_base, rounded_size))) {
        goto cleanup;
    }
    if (mmu_stop && !check("map INI1 destination header",
                          hvm_memory_create(&ini_header, virt_ini_destination, svc_offset ? virt_ini_reserve : page))) {
        goto cleanup;
    }
    if (fread(memory.host_address, 1, size, stdin) != size) {
        fprintf(stderr, "[HVM] Truncated trace image\n");
        goto cleanup;
    }
    if (mmu_stop) {
        const uint8_t *bytes = memory.host_address;
        const uint8_t *header = bytes + (ini - image_base);
        static const uint8_t empty_ini[] = {
            'I', 'N', 'I', '1', 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        if (svc_offset) {
            uint32_t ini_size, processes;
            if (memcmp(header, "INI1", 4) != 0 || loader - ini < sizeof(empty_ini)) {
                fprintf(stderr, "[HVM] EL0 trace requires a valid INI1 header\n");
                goto cleanup;
            }
            memcpy(&ini_size, header + 4, 4);
            memcpy(&processes, header + 8, 4);
            const uint32_t expected_processes = ipc_probe ? 2 : 1;
            if (processes != expected_processes || ini_size <= sizeof(empty_ini) || ini_size > page || ini_size > loader - ini) {
                fprintf(stderr, "[HVM] EL0 trace requires its expected KIP count in one INI1 page\n");
                goto cleanup;
            }
        } else if (memcmp(header, "INI1", 4) == 0 &&
                   (loader - ini < sizeof(empty_ini) || memcmp(header, empty_ini, sizeof(empty_ini)) != 0)) {
            fprintf(stderr, "[HVM] Expected an empty INI1 for this trace\n");
            goto cleanup;
        }
    }
    sys_icache_invalidate(memory.host_address, size);

    uint64_t pc, state, registers[4], entry_registers[4];
    if (!enable_breakpoints(&cpu) ||
        !check("set PSTATE", hv_vcpu_set_reg(cpu.handle, HV_REG_CPSR, el1h_masked)) ||
        !check("set kernel PC", hv_vcpu_set_reg(cpu.handle, HV_REG_PC, image_base)) ||
        !snapshot(&cpu, "Kernel entry", image_base, &pc, &state, registers) ||
        !run_to_breakpoint(&cpu, "Loader entry", loader, entry_registers, false, NULL)) {
        goto cleanup;
    }
    const uint64_t link = entry_registers[3];
    uint32_t call = 0;
    if (link >= image_base + sizeof(call) && link < loader && !(link & 3)) {
        memcpy(&call, (uint8_t *)memory.host_address + (link - image_base) - sizeof(call), sizeof(call));
    }
    if (entry_registers[0] != image_base || entry_registers[1] != layout ||
        entry_registers[2] != ini || call != 0xd63f0060) {
        fprintf(stderr, "[HVM] Loader ABI mismatch\n");
        goto cleanup;
    }
    ok = run_to_breakpoint(&cpu, "After first loader instruction", target, registers, false, NULL);
    if (ok && memcmp(registers, entry_registers, sizeof(registers)) != 0) {
        fprintf(stderr, "[HVM] Loader branch changed the argument registers\n");
        ok = false;
    }

    if (ok && mmu_stop) {
        ok = run_to_breakpoint(&cpu, "After enabling the loader MMU", mmu_stop, registers, true, NULL);
        uint64_t sctlr, tcr;
        if (ok) {
            ok = check("read SCTLR_EL1", hv_vcpu_get_sys_reg(cpu.handle, HV_SYS_REG_SCTLR_EL1, &sctlr)) &&
                 check("read TCR_EL1", hv_vcpu_get_sys_reg(cpu.handle, HV_SYS_REG_TCR_EL1, &tcr));
            if (ok && (!(sctlr & 1) || ((tcr >> 14) & 3) != 0 || ((tcr >> 30) & 3) != 2)) {
                fprintf(stderr, "[HVM] Expected the MMU and 4 KiB translation for both TTBRs\n");
                ok = false;
            }
        }
    }
    if (ok && handoff) {
        ok = trace_handoff(&cpu, handoff, link, image_base, ini, kernel_offset, secondary_offset, main_offset, smp_offset, write0_offset, &smp, &memory);
    }

cleanup:
    if (!destroy_secondaries(&smp)) {
        ok = false;
    }
    if (has_cpu && !check("destroy vCPU", hvm_vcpu_destroy(&cpu))) {
        ok = false;
    }
    if (ini_header.mapped && !check("unmap INI1 destination header", hvm_memory_destroy(&ini_header))) {
        ok = false;
    }
    if (memory.mapped && !check("unmap image", hvm_memory_destroy(&memory))) {
        ok = false;
    }
    if (!check("destroy VM", hvm_vm_destroy())) {
        ok = false;
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
