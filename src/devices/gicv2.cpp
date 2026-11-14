#include "devices/gicv2.h"

#include <bit>

namespace {

bool word(uint32_t &reg, uint32_t mask, bool write, uint32_t &value) {
    if (write) {
        reg = value & mask;
    } else {
        value = reg;
    }
    return true;
}

bool bitmap(uint32_t &reg, bool set, bool write, uint32_t &value, uint32_t read_ones = 0) {
    if (write) {
        reg = set ? (reg | value) : (reg & ~value);
    } else {
        value = reg | read_ones;
    }
    return true;
}

bool bytes(uint8_t *regs, unsigned index, unsigned width, uint8_t mask,
           bool write, uint32_t &value) {
    if (write) {
        for (unsigned byte = 0; byte < width; ++byte) {
            regs[index + byte] = (value >> (8 * byte)) & mask;
        }
    } else {
        value = 0;
        for (unsigned byte = 0; byte < width; ++byte) {
            value |= uint32_t(regs[index + byte]) << (8 * byte);
        }
    }
    return true;
}

// This boot profile delivers group 0 local interrupts at one active priority level.
unsigned next_local_irq(const hvm_gicv2_t &gic, unsigned core) {
    const auto &cpu = gic.cpu[core];
    if (!gic.control || !cpu.control || cpu.active_irq) {
        return 1023;
    }
    unsigned best = 1023;
    unsigned priority = cpu.priority_mask;
    const auto &local = gic.local[core];
    const uint32_t pending_ppis = (cpu.ppi_level | local.pending) & local.enabled;
    for (unsigned id = 0; id < 32; ++id) {
        const auto mask = 1u << id;
        const bool pending = id < 16 ? cpu.sgi_sources[id] != 0 : (pending_ppis & mask) != 0;
        if (pending && !((local.group | local.active) & mask) && local.priority[id] < priority) {
            best = id;
            priority = local.priority[id];
        }
    }
    return best;
}

bool send_sgi(hvm_gicv2_t &gic, unsigned sender, uint32_t value) {
    const unsigned filter = (value >> 24) & 3;
    unsigned targets = (value >> 16) & 0xf;
    switch (filter) {
    case 1:
        targets = 0xf & ~(1u << sender);
        break;
    case 2:
        targets = 1u << sender;
        break;
    case 3:
        return false;
    }
    const unsigned id = value & 15;
    for (unsigned target = 0; target < HVM_GICV2_CPUS; ++target) {
        if (targets & (1u << target)) {
            gic.cpu[target].sgi_sources[id] |= 1u << sender;
        }
    }
    return true;
}

bool distributor(hvm_gicv2_t &gic, unsigned core, unsigned offset,
                 unsigned width, bool write, uint32_t &value) {
    if (offset >= 0x400 && offset + width <= 0x440) {
        auto &bank = offset < 0x420 ? gic.local[core] : gic.shared;
        return bytes(bank.priority, offset & 31, width, 0xf8, write, value);
    }
    if (offset >= 0x800 && offset + width <= 0x840) {
        if (offset < 0x820) {
            // The banked SGI/PPI target fields report the accessing CPU.
            if (!write) {
                value = (1u << core) * (width == 1 ? 1 : 0x01010101u);
            }
            return true;
        }
        return bytes(gic.shared_targets, offset - 0x820, width, 0xf, write, value);
    }
    if (width != 4) {
        return false;
    }
    switch (offset) {
    case 0:
        return word(gic.control, 1, write, value);
    case 4:
        if (!write) {
            // Four CPU interfaces and two 32-interrupt blocks.
            value = ((HVM_GICV2_CPUS - 1) << 5) | 1;
        }
        return true;
    case 0xf00:
        return write && send_sgi(gic, core, value);
    case 0xc00:
        if (!write) {
            value = 0xaaaaaaaau;  // SGIs are edge-triggered.
        }
        return true;
    case 0xc04:
        return word(gic.local[core].config[1], 0xaaaaaaaau, write, value);
    case 0xc08:
    case 0xc0c:
        return word(gic.shared.config[(offset - 0xc08) / 4], 0xaaaaaaaau, write, value);
    }

    const unsigned bank_index = (offset & 0x7f) / 4;
    if (bank_index > 1) {
        return false;
    }
    auto &bank = bank_index == 0 ? gic.local[core] : gic.shared;
    switch (offset & ~0x7f) {
    case 0x080:
        return word(bank.group, UINT32_MAX, write, value);
    case 0x100:
    case 0x180:
        return bitmap(bank.enabled, offset < 0x180, write, value,
                      bank_index == 0 ? 0xffffu : 0);
    case 0x200:
    case 0x280:
        // SGI source/target pending state is a separate interface.
        if (bank_index == 0 && write) {
            value &= 0xffff0000u;
        }
        return bitmap(bank.pending, offset < 0x280, write, value,
                      bank_index == 0 ? gic.cpu[core].ppi_level : 0);
    case 0x300:
    case 0x380:
        return bitmap(bank.active, offset < 0x380, write, value);
    default:
        return false;
    }
}

}

extern "C" bool hvm_gicv2_access(hvm_gicv2_t *gic, unsigned core, uint64_t ipa,
                                 unsigned width, bool write, uint32_t *value) {
    if (!gic || !value || core >= HVM_GICV2_CPUS ||
        (width != 1 && width != 4) || ipa % width) {
        return false;
    }
    if (ipa >= HVM_VIRT_GICD_BASE && ipa - HVM_VIRT_GICD_BASE < 0x1000) {
        return distributor(*gic, core, unsigned(ipa - HVM_VIRT_GICD_BASE), width, write, *value);
    }
    if (ipa < HVM_VIRT_GICC_BASE || width != 4) {
        return false;
    }
    auto &cpu = gic->cpu[core];
    switch (ipa - HVM_VIRT_GICC_BASE) {
    case 0:
        return word(cpu.control, 1, write, *value);
    case 4:
        return word(cpu.priority_mask, 0xf8, write, *value);
    case 8:
        return word(cpu.binary_point, 7, write, *value);
    case 0xc: {
        if (write) {
            return false;
        }
        const unsigned id = next_local_irq(*gic, core);
        if (id == 1023) {
            *value = id;
            return true;
        }
        *value = id;
        if (id < 16) {
            const unsigned sender = std::countr_zero(cpu.sgi_sources[id]);
            cpu.sgi_sources[id] &= ~(1u << sender);
            *value |= sender << 10;
        } else {
            gic->local[core].pending &= ~(1u << id);
        }
        gic->local[core].active |= 1u << id;
        cpu.active_irq = *value + 1;
        return true;
    }
    case 0x10:
        if (!write || !cpu.active_irq || (*value & 0x1fff) + 1 != cpu.active_irq) {
            return false;
        }
        gic->local[core].active &= ~(1u << ((*value) & 0x3ff));
        cpu.active_irq = 0;
        return true;
    default:
        return false;
    }
}

extern "C" bool hvm_gicv2_irq_pending(const hvm_gicv2_t *gic, unsigned core) {
    return gic && core < HVM_GICV2_CPUS && next_local_irq(*gic, core) != 1023;
}

extern "C" void hvm_gicv2_set_ppi(hvm_gicv2_t *gic, unsigned core, unsigned id, bool asserted) {
    if (gic && core < HVM_GICV2_CPUS && id >= 16 && id < 32) {
        auto &levels = gic->cpu[core].ppi_level;
        levels = asserted ? levels | (1u << id) : levels & ~(1u << id);
    }
}
