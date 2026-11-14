#include "devices/gicv2.h"

#include <cassert>

int main() {
    hvm_gicv2_t gic{};
    const auto dist = HVM_VIRT_GICD_BASE;
    const auto cpu = HVM_VIRT_GICC_BASE;
    auto write = [&](unsigned core, uint64_t ipa, uint32_t value, unsigned width = 4) {
        assert(hvm_gicv2_access(&gic, core, ipa, width, true, &value));
    };
    auto read = [&](unsigned core, uint64_t ipa, unsigned width = 4) {
        uint32_t value = 0;
        assert(hvm_gicv2_access(&gic, core, ipa, width, false, &value));
        return value;
    };

    assert(read(0, dist + 4) == 0x61);
    write(1, dist + 0x800, 0, 1);
    assert(read(1, dist + 0x800, 1) == 2);
    assert(read(2, dist + 0x800, 1) == 4);
    write(1, dist + 0x100, 1u << 30);
    assert(read(1, dist + 0x180) == (1u << 30 | 0xffff));
    assert(read(2, dist + 0x100) == 0xffff);
    write(1, dist + 0x180, 1u << 30);
    assert(read(1, dist + 0x100) == 0xffff);

    write(0, dist, 1);
    write(1, cpu, 1);
    write(1, cpu + 4, 0x80);
    write(1, dist + 0x402, 0x40, 1);
    write(0, dist + 0xf00, (1u << 17) | 2);
    write(2, dist + 0xf00, (1u << 17) | 2);
    assert(hvm_gicv2_irq_pending(&gic, 1));
    assert(!hvm_gicv2_irq_pending(&gic, 2));
    assert(read(1, cpu + 0xc) == 2);
    assert(!hvm_gicv2_irq_pending(&gic, 1));
    assert(read(1, cpu + 0xc) == 1023);
    write(1, cpu + 0x10, 2);
    assert(hvm_gicv2_irq_pending(&gic, 1));
    const uint32_t second_sender = (2u << 10) | 2;
    assert(read(1, cpu + 0xc) == second_sender);
    write(1, cpu + 0x10, second_sender);
    assert(!hvm_gicv2_irq_pending(&gic, 1));

    write(1, cpu + 4, 0x40);
    write(1, dist + 0xf00, (2u << 24) | 2);
    assert(!hvm_gicv2_irq_pending(&gic, 1));
    write(1, cpu + 4, 0x48);
    assert(hvm_gicv2_irq_pending(&gic, 1));

    // A level PPI is banked, masked by ISENABLER, and reasserts after EOI
    // until the peripheral drops the line.
    write(2, cpu, 1);
    write(2, cpu + 4, 0x80);
    write(2, dist + 0x41e, 0x40, 1);
    hvm_gicv2_set_ppi(&gic, 2, 30, true);
    assert(!hvm_gicv2_irq_pending(&gic, 2));
    write(2, dist + 0x100, 1u << 30);
    assert(hvm_gicv2_irq_pending(&gic, 2));
    assert(read(2, cpu + 0xc) == 30);
    assert((read(2, dist + 0x300) & (1u << 30)) != 0);
    assert(!hvm_gicv2_irq_pending(&gic, 2));
    write(2, cpu + 0x10, 30);
    assert(hvm_gicv2_irq_pending(&gic, 2));
    assert(read(2, cpu + 0xc) == 30);
    hvm_gicv2_set_ppi(&gic, 2, 30, false);
    write(2, cpu + 0x10, 30);
    assert(!hvm_gicv2_irq_pending(&gic, 2));

    uint32_t ignored = 0;
    assert(!hvm_gicv2_access(&gic, 0, dist + 0x401, 4, true, &ignored));
    assert(!hvm_gicv2_access(&gic, 0, dist + 0xff0, 4, false, &ignored));
}
