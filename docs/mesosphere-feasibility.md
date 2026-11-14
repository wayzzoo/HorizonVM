# Mesosphère on HorizonVM

Early Mesosphère execution at EL1 is plausible on this M1. Boot through
scheduler initialization is a separate milestone: the current QEMU board needs
GICv2, four cores and a real secondary CPU startup service. No Mesosphère
binary was built or executed during this audit.

## HorizonVM baseline

Audited HorizonVM at `07dcfef` on `main`. The C backend owns VM, vCPU and
anonymous guest RAM; the C++ MMIO dispatcher routes one Tegra timer register
to a Mach monotonic clock. `make -B run` rebuilt and signed the executable,
then passed the existing 32-bit timer load at `0x60005010`, followed by the
expected BRK. No other checked-in tests existed. Local editor artifacts were
untracked and left alone.

The exit path in `src/main.c` is specific to the two-instruction fixture.
It checks the IPA, PC and instruction, but it does not establish whether
the abort is a stage-2 translation fault for a data access rather than a
page-table walk or another abort cause. It is not a reusable MMIO decoder.
The final check verifies the BRK exception class and X4 but not the final PC
or BRK immediate. The two exits are counted implicitly; either call to
`hv_vcpu_run` can still block. The normal path destroys the vCPU directly,
bypassing the wrapper that clears the framework-owned exit pointer; no
current use-after-destroy was found. Several failure paths ignore cleanup
failures. If VM unmap fails, keeping the backing allocated is correct until
VM teardown; the current code then depends on process exit to reclaim it.
The Makefile also lacks header dependency tracking.

## Upstream boot contract

Inspected [Atmosphère 3e891501](https://github.com/Atmosphere-NX/Atmosphere/tree/3e8915011e1de89f595462d929c1cc3294470896),
one commit after tag `1.12.2`. The Mesosphère Makefiles select Nintendo NX
or QEMU virt; the latter still has `qemu_virt_a57_release`, `_debug` and
`_audit` targets. `make -C mesosphere qemu_virt_a57_debug` reaches the
AArch64 rules, then fails at the missing `DEVKITPRO` check on this workstation.
The rules include devkitA64's `base_rules`, use GCC specs and objcopy, compile
as GNU C++23, and run Python for image assembly. Apple Clang is not a drop-in
replacement for these rules. The supported source target is confirmed;
a reproducible build and QEMU boot at this revision are **unverified**.
Local QEMU 11.1.3 advertises TCG and HVF; that alone does not establish
that the selected machine, CPU and interrupt controller work together.

1. [Image assembly](https://github.com/Atmosphere-NX/Atmosphere/blob/3e8915011e1de89f595462d929c1cc3294470896/mesosphere/build_mesosphere.py)
   joins raw kernel and loader binaries into `mesosphere.bin`: kernel at
   offset zero, kernel BSS padded, optional INI1 and loader at 4 KiB boundaries.
   It patches two **field-relative** offsets in the `MSS1` metadata. The
   separate ELF outputs and linker maps are useful for symbolizing the raw
   image and checking each component's memory extent.
2. Start at the **kernel image's** `_start`, which branches to
   `StartCore0`. Start the VM at AArch64 EL1h, with DAIF masked and the MMU
   off, with identity-addressable writable backing for the full early
   footprint. EL2 is an alternate entry path that drops to EL1. The QEMU
   path does not derive the layout from a DTB or use the incoming X0/X1 to
   locate the loader; `StartCore0` sets the later debug-region argument
   to zero. It flushes caches, disables translation/caches, and calls the
   loader by the patched relative offset.
3. At [loader `_start`](https://github.com/Atmosphere-NX/Atmosphere/blob/3e8915011e1de89f595462d929c1cc3294470896/mesosphere/kernel_ldr/source/arch/arm64/start.s),
   X0 is the physical kernel image base, X1 points to its writable
   0x34-byte `KernelLayout` (section, relocation, init array, resource
   and sysreg offsets), X2 points to readable INI1 header storage, and X30
   is the physical return site in the kernel. The upstream name is
   `KernelLayout`, despite the older assembly comment naming `KernelMap`.
   The loader branches over `MLD0`, clears its linker-defined BSS, and
   sets SP from its own linker-defined 4 KiB stack before saving arguments.
   No caller-supplied loader stack is needed, but the BSS/stack must be
   backed even if absent from the raw file. It self-relocates, calls
   its init array, clears TPIDR_EL1 and CNTV_CVAL_EL0, and sets VBAR_EL1.
4. [Loader `Main`](https://github.com/Atmosphere-NX/Atmosphere/blob/3e8915011e1de89f595462d929c1cc3294470896/mesosphere/kernel_ldr/source/kern_init_loader.cpp)
   normalizes the layout, handles INI1, allocates page tables after the
   kernel resource area, and identity-maps kernel, loader and a 2 MiB
   table region. It sets TTBR0/1, MAIR (`0x44ff0400`), TCR
   (`0x11b5193519`, 4 KiB granules), then enables translation with
   SCTLR (`0x34d5d92d`). It creates randomized higher-half mappings,
   relocates and initializes the kernel, and reserves a kernel init stack.
   It returns X0 = allocator/INI state pointer, X1 = virtual minus
   physical return-site delta, with a new SP. It translates the relocated
   return site using `AT S1E1R` and PAR_EL1 and branches to that physical
   address; a translation failure spins.
5. [Kernel continuation](https://github.com/Atmosphere-NX/Atmosphere/blob/3e8915011e1de89f595462d929c1cc3294470896/mesosphere/kernel/source/arch/arm64/init/start.s)
   calls the relocated `InitializeCorePhase1`, installs the final per-core
   stack, calls `InitializeCorePhase2`, and invokes per-core `InvokeMain`.
   That initializes debug registers and exception vectors, then calls
   `HorizonKernelMain(core_id)`. Phase 2 requests other CPUs before
   returning to this assembly path. A bare image can reach loader
   milestones without processes; later INI handling requires a valid INI1
   magic and must be tested with legitimately built open-source inputs.

## Compatibility matrix

| Requirement | Status | Evidence and consequence |
| --- | --- | --- |
| Native AArch64, EL1 | Supported in principle | Tegra fixture proves native instructions execute. The SDK exposes CPSR and EL1 registers; an explicit EL1h entry test is still needed. |
| Guest EL2 | Unsupported on this M1 | `make probe` reports no guest EL2. Start at EL1; the QEMU board accepts it. No need for an EL2 guest in the first trace. |
| Guest stage-1 4 KiB | Advertised; runtime unknown | M1 guest MMFR0 reports TGran4 = 0 (supported). Host page size and default stage-2 IPA granule are independently 16 KiB. Put 4 KiB guest tables inside 16 KiB stage-2 mappings. Test the handoff's TTBR/TCR/MAIR/SCTLR, cache maintenance, DSB/ISB, and AT/PAR with a small guest before booting an image. |
| CPU feature assumptions | Partly supported | Probe reads guest MIDR, MPIDR and feature registers. MIDR implementer is Apple (0x61), so the Arm A53/A57-specific register branches should be skipped; do not spoof MIDR. Check AES, guest FP enablement, cache set/way and debug register execution on hardware. |
| RAM and raw image | Implementable with a workaround | The QEMU board hardcodes 4 GiB starting at `0x40000000` and reserves the first 512 KiB in its memory layout. Shared code places the INI destination near the top of that range. Map only the necessary early regions for the first trace and stop on unexpected accesses; full kernel initialization needs a measured memory budget or a documented smaller-memory board port. Confirm the image placement and resource footprint from actual linker output. |
| Generic timer | Requires validation/workaround | Early randomization reads the physical counter; the scheduler programs the physical timer and binds its nonsecure PPI. The board assumes 19.2 MHz. Measure CNTFRQ_EL0 and verify CNTP behavior; Apple's virtual timer exit is not evidence of working physical timer delivery. The Tegra MMIO timer is unrelated. |
| Interrupts | Apple GICv3 is incompatible as a direct replacement | QEMU board maps GIC distributor at `0x08000000` and MMIO CPU interface at `0x08010000`, with GICv2-style CPU interface and SGIs. Later boot needs either GICv2 emulation in HorizonVM or a guest GICv3 port. [Apple's API](https://developer.apple.com/documentation/hypervisor/hv_gic_create(_:)) exposes a GICv3 device. |
| Secondary CPUs | Implementable; PSCI conduit unknown on this host | Both board configurations set four cores; no one-core build target was found. Phase 2 calls PSCI 64-bit CPU_ON (`0xC4000003`) using `smc #0`, with target MPIDR, physical StartOtherCore PC and physical argument. Need owner threads, affinity IDs and actual secondary launch before returning success. Measure SMC exit behavior; a guest board port could use a documented host-handled conduit if needed. |
| Secure monitor | NX calls omitted early; later capabilities limited | Loader and kernel Exosphère firmware queries are NX-only. QEMU early entropy reads a timer; its guest user-SMC shim implements Exosphère API-version lookup only and panics on unhandled operations. Crypto and device-specific protected state cannot be assumed available. PSCI CPU_ON is a distinct, replaceable platform service. |
| Early debug | Requires workaround | QEMU log backend selects semihosting but PutChar is a TODO; several early failures spin in guest code. Use bounded execution and host PC/syndrome capture plus symbolization. |

## Next milestones

1. **Host capability probe (implemented).** `make probe` queries the real
   framework and exits without running guest instructions. On this M1 it
   completed successfully: host pages 16384 bytes, default IPA granule
   16 KiB, EL2 unavailable, MMFR0 `0x000010000f100001`. The existing
   Tegra regression also passed after the change.
2. **Build the reference image.** Pin devkitA64 and record GCC/binutils
   versions, build the above revision, and retain the two ELFs/maps plus
   the raw image and its SHA-256. Establish a QEMU reference trace with
   the [virt board](https://www.qemu.org/docs/master/system/arm/virt.html),
   GICv2, Cortex-A57 and the [generic raw loader](https://www.qemu.org/docs/master/system/generic-loader.html).
   Break on both entries and verify register/metadata values. Check the
   PSCI conduit before assuming a full QEMU boot. TCG can provide the
   Cortex-A57/GICv2 baseline even when host CPU or HVF device capabilities
   differ; it is a comparison tool, not a HorizonVM backend.
3. **Make early exits observable.** Add cancellation with
   `hv_vcpus_exit` on a deadline, capture exit state before destroying or
   resuming the vCPU, and prove 4 KiB stage-1 translation with a tiny native
   EL1 fixture. Then load the pinned raw image with validated ranges and
   break at loader entry and again after executing a loader instruction.
   Record image hash, actual PC, PSTATE, EL1 registers, exit reason,
   syndrome, VA/IPA where supplied, and symbol offsets.
4. **Work outward from the first real fault.** Validate translated return
   to the kernel, then tackle GIC, physical timer, secondary core launch,
   scheduler and open-source initial processes as separate milestones.
   Original Nintendo kernel and HOME Menu remain outside this experiment.
