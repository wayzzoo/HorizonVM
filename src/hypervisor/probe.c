#include "hypervisor/probe.h"
#include "hypervisor/vcpu.h"
#include "hypervisor/vm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool check(const char *operation, hv_return_t result) {
    if (result == HV_SUCCESS) {
        return true;
    }
    fprintf(stderr, "[HVM] %s failed: %#x\n", operation, (unsigned int)result);
    return false;
}

int hvm_probe(void) {
    bool el2;
    hv_ipa_granule_t granule;
    if (!check("query guest EL2", hv_vm_config_get_el2_supported(&el2)) ||
        !check("query default IPA granule", hv_vm_config_get_default_ipa_granule(&granule))) {
        return EXIT_FAILURE;
    }
    printf("[HVM] Host page size: %d bytes\n", getpagesize());
    printf("[HVM] Default stage-2 IPA granule: %s\n",
           granule == HV_IPA_GRANULE_16KB ? "16 KiB" :
           granule == HV_IPA_GRANULE_4KB ? "4 KiB" : "unknown");
    printf("[HVM] Guest EL2 supported: %s\n", el2 ? "yes" : "no");

    if (!check("create VM", hvm_vm_create())) {
        return EXIT_FAILURE;
    }
    hvm_vcpu_t cpu = {0};
    bool ok = check("create vCPU", hvm_vcpu_create(&cpu));
    if (ok) {
        static const struct {
            const char *name;
            hv_sys_reg_t reg;
        } regs[] = {
            {"MIDR_EL1", HV_SYS_REG_MIDR_EL1},
            {"MPIDR_EL1", HV_SYS_REG_MPIDR_EL1},
            {"ID_AA64MMFR0_EL1", HV_SYS_REG_ID_AA64MMFR0_EL1},
            {"ID_AA64PFR0_EL1", HV_SYS_REG_ID_AA64PFR0_EL1},
            {"ID_AA64ISAR0_EL1", HV_SYS_REG_ID_AA64ISAR0_EL1},
            {"SCTLR_EL1", HV_SYS_REG_SCTLR_EL1},
            {"TCR_EL1", HV_SYS_REG_TCR_EL1},
            {"TTBR0_EL1", HV_SYS_REG_TTBR0_EL1},
            {"TTBR1_EL1", HV_SYS_REG_TTBR1_EL1},
            {"MAIR_EL1", HV_SYS_REG_MAIR_EL1},
            {"VBAR_EL1", HV_SYS_REG_VBAR_EL1},
            {"TPIDR_EL1", HV_SYS_REG_TPIDR_EL1},
        };
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            uint64_t value;
            if (!check(regs[i].name, hv_vcpu_get_sys_reg(cpu.handle, regs[i].reg, &value))) {
                ok = false;
                continue;
            }
            printf("[HVM] %s = 0x%016" PRIx64 "\n", regs[i].name, value);
            if (regs[i].reg == HV_SYS_REG_ID_AA64MMFR0_EL1) {
                printf("[HVM] Guest stage-1 TGran4 field: 0x%" PRIx64 "\n",
                       (value >> 28) & UINT64_C(0xf));
            }
        }
        if (!check("destroy vCPU", hvm_vcpu_destroy(&cpu))) {
            ok = false;
        }
    }
    if (!check("destroy VM", hvm_vm_destroy())) {
        ok = false;
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
