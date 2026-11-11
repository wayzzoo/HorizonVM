#include "hypervisor/memory.h"

#include <sys/mman.h>
#include <unistd.h>

hv_return_t hvm_memory_create(
    hvm_memory_t *memory,
    hv_ipa_t guest_address,
    size_t size
) {
    if (memory == NULL || size == 0) {
        return HV_BAD_ARGUMENT;
    }

    size_t page_size = (size_t)getpagesize();

    if (size % page_size != 0 ||
        guest_address % page_size != 0) {
        return HV_BAD_ARGUMENT;
    }

    memory->host_address = NULL;
    memory->guest_address = guest_address;
    memory->size = 0;
    memory->mapped = false;

    void *buffer = mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON,
        -1,
        0
    );

    if (buffer == MAP_FAILED) {
        return HV_ERROR;
    }

    hv_return_t result = hv_vm_map(
        buffer,
        guest_address,
        size,
        HV_MEMORY_READ |
        HV_MEMORY_WRITE |
        HV_MEMORY_EXEC
    );

    if (result != HV_SUCCESS) {
        munmap(buffer, size);
        return result;
    }

    memory->host_address = buffer;
    memory->size = size;
    memory->mapped = true;

    return HV_SUCCESS;
}

hv_return_t hvm_memory_destroy(hvm_memory_t *memory) {
    if (memory == NULL || !memory->mapped) {
        return HV_BAD_ARGUMENT;
    }

    hv_return_t result = hv_vm_unmap(
        memory->guest_address,
        memory->size
    );

    if (result != HV_SUCCESS) {
        return result;
    }

    memory->mapped = false;

    if (munmap(memory->host_address, memory->size) != 0) {
        return HV_ERROR;
    }

    memory->host_address = NULL;
    memory->size = 0;

    return HV_SUCCESS;
}