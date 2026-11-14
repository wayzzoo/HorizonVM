# Mesosphère kernel entry and first barrier on four vCPUs

On Apple M1, HorizonVM handled PSCI CPU_ON for affinities 1, 2 and 3.
All three secondary vCPUs executed `StartOtherCore` and reached
`InvokeMain` with their assigned affinity. All four vCPUs then executed
the first instruction of `HorizonKernelMain`, a stack-frame store,
with X0 equal to their core ID. All four also passed the first
multicore barrier while running concurrently.

The trace parks each vCPU at `InvokeMain`. The kernel clears its hardware
breakpoints in that function, so HorizonVM walks the guest's TTBR1 page
tables and places a temporary `HVC #0x480` at the second instruction of
`HorizonKernelMain`. The first instruction runs unchanged. The trace
verifies the HVC syndrome `0x5a000480`, the resume PC and X0 for each
vCPU, then restores the instruction while the vCPUs are stopped. The
trace then restores the resume PC to the second instruction and runs
all four vCPUs concurrently. A second marker, `HVC #0x481`, records
the return from the first `SynchronizeAllCores` call on each vCPU.
The trace stops before main and idle thread initialization.

When debug exception trapping was disabled, the guest's ordinary
debug register initialization ran. The framework still trapped
`OSLAR_EL1` and rejected a host write to that register. The additional
QEMU virt patch omits that OS lock write for this build. The other
debug initialization writes remain in place.

## Reproduce

Apply the build and native loader patches from
[the loader handoff](mesosphere-kernel-handoff.md), followed by the
SMP patch. The offsets below come from the ELF files of the resulting
debug build.

```sh
git -C "$ATMOSPHERE_SRC" apply --unidiff-zero "$PWD/tools/mesosphere-native-smp.patch"
DEVKITPRO="$DEVKITPRO" make -C "$ATMOSPHERE_SRC/mesosphere" -j3 qemu_virt_a57_debug
image="$ATMOSPHERE_SRC/mesosphere/out/qemu_virt_arm64_armv8a/debug/mesosphere.bin"
make all
python3 tools/trace_mesosphere.py "$image" --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --through-barrier 0x25af0
```

The tested raw image SHA-256 was
`777cd0e970bbc6a451033304db459ccb29aeac8805f4cf363275a5feaf146af1`.
The next section of kernel code initializes each core's main and idle
threads, then initializes core 0's memory manager. Later boot still
needs working interrupt delivery and open-source initial processes.
