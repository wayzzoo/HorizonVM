#include "hypervisor/vm.h"

hv_return_t hvm_vm_create(void) {
    return hv_vm_create(NULL);
}

hv_return_t hvm_vm_destroy(void) {
    return hv_vm_destroy();
}