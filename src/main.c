#include <stdio.h>
#include <Hypervisor/Hypervisor.h>

int main(void) {
    printf("HorizonVM - Apple Silicon Hypervisor\n");

    hv_return_t result = hv_vm_create(NULL);

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] VM Creation Failed: %d\n", result);
        return 1;
    }

    printf("[HVM] Virtual machine created successufully\n");

    result = hv_vm_destroy();

    if (result != HV_SUCCESS) {
        fprintf(stderr, "[HVM] VM desctruction failed: %d\n", result);
        return 1;
    }

    printf("[HVM] Virtual machine destroyed successfully\n");

    return 0;
}