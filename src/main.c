#include <Hypervisor/hv_vcpu.h>
#include <Hypervisor/hv_vcpu_types.h>
#include <Hypervisor/hv_vm.h>
#include <arm64/hv/hv_kern_types.h>
#include <stdio.h>
#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdbool.h>

#include <hypervisor/vm.h>
#include <hypervisor/vcpu.h>
#include <hypervisor/memory.h>
#include <platform/tegra210.h>
#include <devices/mmio.h>

int main(void) {
    printf("HorizonVM - Apple Silicon Hypervisor\n");

    hv_return_t result = hvm_vm_create();
    hvm_vcpu_t cpu = {0};

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] VM Creation Failed: %d\n", result);
        return 1;
    }

    printf("[HVM] Virtual machine created successufully\n");

    result = hvm_vcpu_create(&cpu);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] vCPU creation failed: %d\n", result);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] Virtual CPU created successufully\n");

    uint64_t value = 42;

    result = hv_vcpu_set_reg(cpu.handle, HV_REG_X0, value);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to set X0: %d\n", result);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    result = hv_vcpu_get_reg(cpu.handle, HV_REG_X0, &value);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to read X0: %d\n", result);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest register X0 = %llu\n",(unsigned long long)value);



    const size_t guest_ram_size = 16384;
    const hv_ipa_t guest_base = T210_IRAM_BASE;

    hvm_memory_t memory = {0};

    result = hvm_memory_create(
        &memory,
        guest_base,
        guest_ram_size
    );

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Memory creation failed: %d\n", result);

        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    const uint32_t guest_code[] = {
        0xB9400064, // ldr w4, [x3]
        0xD4200000  // brk #0   
    };

    memcpy(memory.host_address, guest_code, sizeof(guest_code));


    printf("[HVM] Guest RAM mapped at 0x%llx\n",
        (unsigned long long)guest_base);

    result = hv_vcpu_set_trap_debug_exceptions(cpu.handle, true);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to enable debug traps: %d\n", result);
        hvm_memory_destroy(&memory);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    result = hv_vcpu_set_reg(cpu.handle, HV_REG_PC, guest_base);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to set PC: %d\n", result);
        hvm_memory_destroy(&memory);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    const uint64_t mmio_base = T210_TIMER_BASE + T210_TIMERUS_CNTR;

    result = hv_vcpu_set_reg(cpu.handle, HV_REG_X3, mmio_base);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to set MMIO address: %d\n", result);
        hvm_memory_destroy(&memory);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] MMIO target: 0x%llx\n",(unsigned long long)mmio_base);

    printf("[HVM] Starting guest execution at 0x%llx\n", (unsigned long long)guest_base);


    hvm_mmio_t mmio = {0};

    if(!hvm_mmio_init(&mmio)) {
        fprintf(stderr, "[HVM] MMIO initialization failed\n");
        hvm_memory_destroy(&memory);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    result = hv_vcpu_run(cpu.handle);


    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Guest executuion failed: %d\n", result);
        hvm_memory_destroy(&memory);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    uint64_t x2 = 0;

    result = hv_vcpu_get_reg(cpu.handle, HV_REG_X2, &x2);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to read X2: %d\n", result);
        hvm_memory_destroy(&memory);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    bool mmio_passed = false;

    if(cpu.exit_info->reason == HV_EXIT_REASON_EXCEPTION) {
        uint64_t syndrome = cpu.exit_info->exception.syndrome;
        uint64_t physical = cpu.exit_info->exception.physical_address;

        unsigned int ec = (unsigned int)((syndrome >> 26) & 0x3F);

        printf("[HVM] Exception class: 0x%02X\n", ec);
        printf("[HVM] Fault IPA: 0x%llx\n",(unsigned long long)physical);

        if ((ec == 0x24 || ec == 0x25) &&
            physical == mmio_base) {

            uint32_t value = 0;

            if (hvm_mmio_read32(&mmio, physical, &value)) {
                printf("[HVM] Tegra timer read: %u us\n", value);
                printf("[HVM] Timer MMIO dispatch PASSED\n");
                mmio_passed = true;
            } else {
                printf("[HVM] Timer MMIO read failed\n");
            }
            }
    } else {
        printf("[HVM] Unexpected exception\n");
    }

    result = hvm_memory_destroy(&memory);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Memory cleanup failed: %d\n", result);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest RAM released successfully\n");

   


    result = hv_vcpu_destroy(cpu.handle);

    if(result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] vCPU destruction failed: %d\n", result);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] Virtual CPU destroyed successfully\n");
    

    result = hvm_vm_destroy();

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] VM desctruction failed: %d\n", result);
        return 1;
    }

    printf("[HVM] Virtual machine destroyed successfully\n");

    if (!mmio_passed) {
        return 1;
    }

    return 0;
}