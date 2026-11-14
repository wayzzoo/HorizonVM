#ifndef HORIZONVM_GICV2_H
#define HORIZONVM_GICV2_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HVM_GICV2_CPUS = 4 };
#define HVM_VIRT_GICD_BASE UINT64_C(0x08000000)
#define HVM_VIRT_GICC_BASE UINT64_C(0x08010000)

typedef struct {
    uint32_t group;
    uint32_t enabled;
    uint32_t pending;
    uint32_t active;
    uint32_t config[2];
    uint8_t priority[32];
} hvm_gicv2_bank_t;

typedef struct {
    uint32_t control;
    uint32_t priority_mask;
    uint32_t binary_point;
    uint32_t active_irq;
    uint32_t ppi_level;
    uint8_t sgi_sources[16];
} hvm_gicv2_cpu_t;

typedef struct {
    uint32_t control;
    hvm_gicv2_cpu_t cpu[HVM_GICV2_CPUS];
    hvm_gicv2_bank_t local[HVM_GICV2_CPUS];
    hvm_gicv2_bank_t shared;
    uint8_t shared_targets[32];
} hvm_gicv2_t;

/* Group 0 boot register, SGI and level-triggered PPI model. Callers serialize concurrent accesses. */
bool hvm_gicv2_access(hvm_gicv2_t *gic, unsigned core, uint64_t ipa,
                      unsigned width, bool write, uint32_t *value);
void hvm_gicv2_set_ppi(hvm_gicv2_t *gic, unsigned core, unsigned id, bool asserted);
bool hvm_gicv2_irq_pending(const hvm_gicv2_t *gic, unsigned core);

#ifdef __cplusplus
}
#endif
#endif
