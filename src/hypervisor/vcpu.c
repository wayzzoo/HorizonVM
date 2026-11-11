#include "hypervisor/vcpu.h"

#include <arm64/hv/hv_kern_types.h>
#include <mach/message.h>
#include <stddef.h>

hv_return_t hvm_vcpu_create(hvm_vcpu_t *cpu) {
    if (cpu == NULL) {
        return HV_BAD_ARGUMENT;
    }

    cpu->handle = 0;
    cpu->exit_info = NULL;

    return hv_vcpu_create(
        &cpu->handle,
        &cpu->exit_info,
        NULL
    );
}

hv_return_t hvm_vcpu_destroy(hvm_vcpu_t *cpu) {
    if (cpu == NULL) {
        return HV_BAD_ARGUMENT;
    }

    hv_return_t result = hv_vcpu_destroy(cpu->handle);

    if(result == HV_SUCCESS) {
        cpu->handle = 0;
        cpu->exit_info = NULL;
    }

    return result;
}