#ifndef HORIZONVM_MEMORY_H
#define HORIZONVM_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <Hypervisor/Hypervisor.h>

typedef struct {
    void *host_address;
    hv_ipa_t guest_address;
    size_t size;
    bool mapped;
} hvm_memory_t;

hv_return_t hvm_memory_create(
    hvm_memory_t *memory,
    hv_ipa_t guest_address,
    size_t size
);

hv_return_t hvm_memory_destroy(hvm_memory_t *memory);


#endif