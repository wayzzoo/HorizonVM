#include "hypervisor/loader_trace.h"
#include "hypervisor/memory.h"
#include "hypervisor/vcpu.h"
#include "hypervisor/vm.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <inttypes.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const hv_ipa_t image_base = 0x40200000;
static const uint64_t el1h_masked = 0x3c5;

static bool check(const char *operation, hv_return_t result) {
    if (result == HV_SUCCESS) {
        return true;
    }
    fprintf(stderr, "[HVM] %s failed: %#x\n", operation, (unsigned int)result);
    return false;
}

/* Only the vCPU's owner runs it. The watchdog can request an asynchronous exit. */
static bool run_bounded(hvm_vcpu_t *cpu) {
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    dispatch_group_t watchdog = dispatch_group_create();
    dispatch_group_async(watchdog, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC))) {
            hv_vcpu_t handle = cpu->handle;
            fprintf(stderr, "[HVM] Guest deadline reached\n");
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
    return check("run guest", result);
}

static bool snapshot(hvm_vcpu_t *cpu, const char *label, uint64_t expected_pc,
                     uint64_t *pc, uint64_t *state, uint64_t args[4]) {
    const hv_reg_t arg_regs[] = {HV_REG_X0, HV_REG_X1, HV_REG_X2, HV_REG_X30};
    if (!check("read PC", hv_vcpu_get_reg(cpu->handle, HV_REG_PC, pc)) ||
        !check("read PSTATE", hv_vcpu_get_reg(cpu->handle, HV_REG_CPSR, state))) {
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        if (!check("read argument register", hv_vcpu_get_reg(cpu->handle, arg_regs[i], &args[i]))) {
            return false;
        }
    }
    printf("%s: PC=%#" PRIx64 " PSTATE=%#" PRIx64 " X0=%#" PRIx64
           " X1=%#" PRIx64 " X2=%#" PRIx64 " X30=%#" PRIx64 "\n",
           *pc == expected_pc ? label : "Unexpected guest stop",
           *pc, *state, args[0], args[1], args[2], args[3]);
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

static bool run_to_breakpoint(hvm_vcpu_t *cpu, const char *label, uint64_t address,
                              uint64_t args[4]) {
    uint64_t pc, state;
    if (!check("set breakpoint address", hv_vcpu_set_sys_reg(cpu->handle, HV_SYS_REG_DBGBVR0_EL1, address)) ||
        !run_bounded(cpu) || !snapshot(cpu, label, address, &pc, &state, args)) {
        return false;
    }
    const hv_vcpu_exit_t *exit = cpu->exit_info;
    if (exit == NULL) {
        fprintf(stderr, "[HVM] Missing vCPU exit information\n");
        return false;
    }
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

int hvm_trace_loader(char **args) {
    /* Image metadata and SHA-256 are checked by the trace script. The child
       receives exactly the bytes hashed by that script via stdin. */
    uint64_t values[5];
    for (size_t i = 0; i < 5; i++) {
        char *end;
        errno = 0;
        values[i] = strtoull(args[i], &end, 0);
        if (errno || end == args[i] || *end || args[i][0] == '-') {
            return EXIT_FAILURE;
        }
    }
    const uint64_t size = values[0], layout = values[1], ini = values[2];
    const uint64_t loader = values[3], target = values[4];
    if (size < 32 || size > 64 * 1024 * 1024 ||
        layout < image_base || layout >= ini || ini >= loader ||
        loader >= target || target > image_base + size - 4 ||
        ((loader | target) & 3)) {
        return EXIT_FAILURE;
    }
    const size_t page = (size_t)getpagesize();
    const size_t rounded_size = (size + page - 1) / page * page;
    hvm_vcpu_t cpu = {0};
    hvm_memory_t memory = {0};
    if (!check("create VM", hvm_vm_create())) {
        return EXIT_FAILURE;
    }
    const bool has_cpu = check("create vCPU", hvm_vcpu_create(&cpu));
    bool ok = false;
    if (!has_cpu || !check("map image", hvm_memory_create(&memory, image_base, rounded_size))) {
        goto cleanup;
    }
    if (fread(memory.host_address, 1, size, stdin) != size) {
        fprintf(stderr, "[HVM] Truncated trace image\n");
        goto cleanup;
    }
    sys_icache_invalidate(memory.host_address, size);

    uint64_t pc, state, registers[4], entry_registers[4];
    // MDSCR.MDE/KDE enable EL1 debug. BCR0 matches all instruction bytes at EL1.
    if (!check("enable debug traps", hv_vcpu_set_trap_debug_exceptions(cpu.handle, true)) ||
        !check("set PSTATE", hv_vcpu_set_reg(cpu.handle, HV_REG_CPSR, el1h_masked)) ||
        !check("set kernel PC", hv_vcpu_set_reg(cpu.handle, HV_REG_PC, image_base)) ||
        !check("set MDSCR", hv_vcpu_set_sys_reg(cpu.handle, HV_SYS_REG_MDSCR_EL1, (1 << 15) | (1 << 13))) ||
        !check("enable breakpoint", hv_vcpu_set_sys_reg(cpu.handle, HV_SYS_REG_DBGBCR0_EL1, (0xf << 5) | (1 << 1) | 1)) ||
        !snapshot(&cpu, "Kernel entry", image_base, &pc, &state, registers) ||
        !run_to_breakpoint(&cpu, "Loader entry", loader, entry_registers)) {
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
    ok = run_to_breakpoint(&cpu, "After first loader instruction", target, registers);
    if (ok && memcmp(registers, entry_registers, sizeof(registers)) != 0) {
        fprintf(stderr, "[HVM] Loader branch changed the argument registers\n");
        ok = false;
    }

cleanup:
    if (has_cpu && !check("destroy vCPU", hvm_vcpu_destroy(&cpu))) {
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
