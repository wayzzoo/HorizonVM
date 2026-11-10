#ifndef HORIZONVM_VM_H
#define HORIZONVM_VM_H

#include <Hypervisor/Hypervisor.h>
#include <arm64/hv/hv_kern_types.h>

hv_return_t hvm_vm_create(void);
hv_return_t hvm_vm_destroy(void);


#endif