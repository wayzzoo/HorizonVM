# First Mesosphère user process on M1

Mesosphère loaded the open-source `HvmEl0Probe` KIP, created PID 1 and
scheduled its main thread on vCPU 0. The process executed
`mov x0, #0x4856` and `svc #7`. HorizonVM observed a saved EL0t PSTATE,
guest ESR `0x56000007`, and X0 `0x4856` at the AArch64 SVC handler.
It walked the live TTBR0 tables and checked both instruction words
in the process's mapped text. The trace stops at handler entry before
running the body of ExitProcess.

The guest kernel requests an instruction-cache operation on peer
vCPUs while setting up the user's executable pages. HorizonVM now kicks
running target vCPUs out of guest execution so they receive the SGI on
their next entry. The same mechanism stops peers after the probe reaches
its checkpoint. For this initial process the native trace backs both the
40 MiB boot region and the board's 12 MiB INI1/early allocation reserve.
The probe's code, stack and kernel allocations fit in those mappings.

## Repeat

Use the no-process image built in the
[scheduler startup guide](mesosphere-scheduler-startup.md), before its
empty INI1 header is added. The probe builder inserts one KIP into the
unused INI1 area and requires a separate output file. The KIP has no
external dependencies and requests only ExitProcess.

```sh
make all
python3 tools/build_el0_probe.py "$image" build/mesosphere-el0.bin
python3 tools/trace_mesosphere.py build/mesosphere-el0.bin --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --el0-svc-checkpoint 0x3097c \
  --semihost-write0 0x4d6c8
```

The verified image SHA-256 was
`c8436e9126990c4eb86a7b7e6d1e48630e476fd468f8ad0ab766f265ac53a204`.
The expected trace contains `KProcess::Run()` for `HvmEl0Probe` and
`observed EL0 SVC #7`. The user PC and stack address vary with ASLR.
