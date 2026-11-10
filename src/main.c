#include <Hypervisor/hv_vcpu.h>
#include <Hypervisor/hv_vm.h>
#include <arm64/hv/hv_kern_types.h>
#include <stdio.h>
#include <Hypervisor/Hypervisor.h>
#include <stdint.h>

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