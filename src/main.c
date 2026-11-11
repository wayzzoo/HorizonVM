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
    const hv_ipa_t guest_base = 0x40000000;

    void *guest_ram = mmap(NULL, guest_ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);


    if (guest_ram == MAP_FAILED) {
        fprintf(stderr, "[HVM] Guest RAM allocation failed\n");
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    memset(guest_ram, 0, guest_ram_size);

    const uint32_t guest_code[] = {
        0xB9400064, // ldr w4, [x3]
        0xD4200000  // brk #0   
    };

    memcpy(guest_ram, guest_code, sizeof(guest_code));

    printf("[HVM] Loaded %zu bytes of ARM64 code\n", sizeof(guest_code));

    printf("[HVM] Allocated %zu bytes of guest RAM\n", guest_ram_size);

    result = hv_vm_map(guest_ram, guest_base, guest_ram_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);


    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Guest RAM mapping failed: %d\n", result);

        munmap(guest_ram, guest_ram_size);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest RAM mapped at 0x%llx\n", (unsigned long long)guest_base);

    result = hv_vcpu_set_trap_debug_exceptions(cpu.handle, true);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to enable debug traps: %d\n", result);
        hv_vm_unmap(guest_base, guest_ram_size);
        munmap(guest_ram, guest_ram_size);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    result = hv_vcpu_set_reg(cpu.handle, HV_REG_PC, guest_base);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to set PC: %d\n", result);
        hv_vm_unmap(guest_base, guest_ram_size);
        munmap(guest_ram, guest_ram_size);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    const uint64_t mmio_base = 0x50000000;

    result = hv_vcpu_set_reg(cpu.handle, HV_REG_X3, mmio_base);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to set MMIO address: %d\n", result);
        hv_vm_unmap(guest_base, guest_ram_size);
        munmap(guest_ram, guest_ram_size);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] MMIO target: 0x%llx\n",(unsigned long long)mmio_base);

    printf("[HVM] Starting guest execution at 0x%llx\n", (unsigned long long)guest_base);


    result = hv_vcpu_run(cpu.handle);


    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Guest executuion failed: %d\n", result);
        hv_vm_unmap(guest_base, guest_ram_size);
        munmap(guest_ram, guest_ram_size);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    uint64_t x2 = 0;

    result = hv_vcpu_get_reg(cpu.handle, HV_REG_X2, &x2);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to read X2: %d\n", result);
        hv_vm_unmap(guest_base, guest_ram_size);
        munmap(guest_ram, guest_ram_size);
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

        if ((ec == 0x24 || ec == 0x25) && physical == mmio_base) {
            printf("[HVM] MMIO trap PASSED\n");
            mmio_passed = true;
        } else {
            printf("[HVM] Unexpected exception\n");
        }
    } else {
        printf("[HVM] Unexpected exit reason: %d\n",cpu.exit_info->reason);
    }

    result = hv_vm_unmap(guest_base, guest_ram_size);
    

    if(result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Guest RAM unmapping failed: %d\n", result);
        hvm_vcpu_destroy(&cpu);
        hvm_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest RAM unmapped successfully\n");

    munmap(guest_ram, guest_ram_size);

   


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