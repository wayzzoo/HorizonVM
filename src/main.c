#include <Hypervisor/hv_vcpu.h>
#include <Hypervisor/hv_vm.h>
#include <arm64/hv/hv_kern_types.h>
#include <stdio.h>
#include <Hypervisor/Hypervisor.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(void) {
    printf("HorizonVM - Apple Silicon Hypervisor\n");

    hv_return_t result = hv_vm_create(NULL);
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit_info = NULL;

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] VM Creation Failed: %d\n", result);
        return 1;
    }

    printf("[HVM] Virtual machine created successufully\n");

    result = hv_vcpu_create(&vcpu, &exit_info, NULL);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] vCPU creation failed: %d\n", result);
        hv_vm_destroy();
        return 1;
    }

    printf("[HVM] Virtual CPU created successufully\n");

    uint64_t value = 42;

    result = hv_vcpu_set_reg(vcpu, HV_REG_X0, value);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to set X0: %d\n", result);
        hv_vcpu_destroy(vcpu);
        hv_vm_destroy();
        return 1;
    }

    result = hv_vcpu_get_reg(vcpu, HV_REG_X0, &value);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Failed to read X0: %d\n", result);
        hv_vcpu_destroy(vcpu);
        hv_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest register X0 = %llu\n",(unsigned long long)value);



    const size_t guest_ram_size = 16384;
    const hv_ipa_t guest_base = 0x40000000;

    void *guest_ram = mmap(NULL, guest_ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);


    if (guest_ram == MAP_FAILED) {
        fprintf(stderr, "[HVM] Guest RAM allocation failed\n");
        hv_vcpu_destroy(vcpu);
        hv_vm_destroy();
        return 1;
    }

    memset(guest_ram, 0, guest_ram_size);

    printf("[HVM] Allocated %zu bytes of guest RAM\n", guest_ram_size);

    result = hv_vm_map(guest_ram, guest_base, guest_ram_size, HV_MEMORY_READ | HV_MEMORY_WRITE);


    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Guest RAM mapping failed: %d\n", result);

        munmap(guest_ram, guest_ram_size);
        hv_vcpu_destroy(vcpu);
        hv_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest RAM mapped at 0x%llx\n", (unsigned long long)guest_base);

    result = hv_vm_unmap(guest_base, guest_ram_size);
    

    if(result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] Guest RAM unmapping failed: %d\n", result);
        hv_vcpu_destroy(vcpu);
        hv_vm_destroy();
        return 1;
    }

    printf("[HVM] Guest RAM unmapped successfully\n");

    munmap(guest_ram, guest_ram_size);

   


    result = hv_vcpu_destroy(vcpu);

    if(result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] vCPU destruction failed: %d\n", result);
        hv_vm_destroy();
        return 1;
    }

    printf("[HVM] Virtual CPU destroyed successfully\n");
    

    result = hv_vm_destroy();

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] VM desctruction failed: %d\n", result);
        return 1;
    }

    printf("[HVM] Virtual machine destroyed successfully\n");

    return 0;
}