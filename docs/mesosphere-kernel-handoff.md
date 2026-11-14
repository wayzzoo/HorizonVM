# Mesosphère loader handoff on Apple M1

The later [four-core trace](mesosphere-four-core-entry.md) handles CPU_ON
and reaches the first multicore barrier.

The native trace now runs through the loader, verifies 4 KiB stage-1
translation, follows the relocated kernel handoff, and reaches
`InitializeCorePhase2`. A later run reached the kernel's first PSCI CPU_ON
request for affinity 1. Hypervisor.framework delivered the SMC as an exception
with syndrome `0x5e000000`. The trace leaves CPU_ON unhandled, so this run has
yet to reach `HorizonKernelMain` or start a user process.

The tested image has SHA-256
`ed1b5a68bcd1d1d7e804a5c7ebbe06f5f262024f68ddba230c51993f26a590de`.
At the checkpoint immediately after the loader's SCTLR write and ISB,
HorizonVM observed PC `0x400fb374`, SCTLR_EL1 `0x34d5d98d`
(MMU enabled) and TCR_EL1 `0x1b5193519` (4 KiB for both TTBRs).
PAR_EL1 reported a successful translation of the return site, and the
second breakpoint confirmed execution transferred there. The kernel then
executed through its relocated higher-half mapping. Physical page table
and return addresses vary between runs.

## Changes required by the native run

- The original loader faulted at its first 8-byte BSS store. The linker had
  defined `__bss_start__` at offset `0x36b1`, before the alignment of the BSS
  section. Moving the symbol inside the aligned section corrected the store.
  The trace captured guest ESR `0x96000061` and fault address `0x4027d6b1`
  using the original image placement.
- The next failure was an exclusive load in the early randomization spinlock.
  The pre-MMU QEMU board path now draws from the existing counter source
  without taking the lock. Only the primary core runs this path with the MMU
  off. Calls after MMU enablement use the existing synchronization.
- Hypervisor.framework trapped the guest's physical counter read with syndrome
  `0x6232f801`. HorizonVM services only reads of CNTPCT_EL0 during the extended
  trace and uses monotonic host uptime scaled to the board's 19.2 MHz tick
  rate. Each stage has a finite exit budget. Physical timer interrupt delivery
  is still required for a later stage.
- The extended trace places the image immediately after the board's 512 KiB
  early DRAM reservation, at `0x40080000`. At the QEMU reference address
  `0x40200000`, the gap before the kernel produced a separate pool partition
  and the QEMU board's phase 2 assertion stopped the guest before CPU_ON.
  The trace maps 40 MiB for the kernel and early allocator, plus one stage-2
  page for the final INI1 header near the end of the board's DRAM range.
  It expects an image without embedded initial processes.

## Repeat the trace

Use the source revision and toolchain in
[first execution](mesosphere-first-execution.md). Apply both patches
to a clean Atmosphère checkout. Build settings and symbol offsets below
refer to the debug image.

```sh
git -C "$ATMOSPHERE_SRC" apply --unidiff-zero "$PWD/tools/mesosphere-qemu-build.patch"
git -C "$ATMOSPHERE_SRC" apply --unidiff-zero "$PWD/tools/mesosphere-native-loader.patch"
DEVKITPRO="$DEVKITPRO" make -C "$ATMOSPHERE_SRC/mesosphere" -j3 qemu_virt_a57_debug
image="$ATMOSPHERE_SRC/mesosphere/out/qemu_virt_arm64_armv8a/debug/mesosphere.bin"
make all
python3 tools/trace_mesosphere.py "$image" --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 --kernel-checkpoint 0x1ea58
```

Kernel offset `0x1ea58` is the first executed `smc #0` in the PSCI CPU_ON
path. At the breakpoint, X0 is `0xc4000003`, X1 is the target affinity, and
X2/X3 are the physical secondary entry and context addresses. To observe the
unhandled SMC exit, select `--kernel-checkpoint 0x25ac0` for
`HorizonKernelMain`. That command should fail at the SMC with the current
backend. The next concrete milestone is to start the requested secondary
vCPU with those arguments and observe execution at `StartOtherCore`.
