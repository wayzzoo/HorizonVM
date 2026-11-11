#ifndef HORIZONVM_VCPU_H
#define HORIZONVM_VCPU_H

#include <Hypervisor/Hypervisor.h>

typedef struct {
    hv_vcpu_t handle;
    hv_vcpu_exit_t *exit_info;
} hvm_vcpu_t;

hv_return_t hvm_vcpu_create(hvm_vcpu_t *cpu);
hv_return_t hvm_vcpu_destroy(hvm_vcpu_t *cpu);

#endif