# Mesosphère scheduler startup on M1

Four Hypervisor.framework vCPUs reached the final common
`HorizonKernelMain` checkpoint at image offset `0x25ccc`.
This follows main and idle thread initialization, memory-manager setup,
scheduler activation, the deferred procedure call (DPC) barrier, and core
0 setting the kernel state to `Initialized`. The test image has a valid
INI1 header with zero processes. A run beyond this checkpoint reached
the first unmasked physical timer request during scheduling.

HorizonVM implements the GICv2 register subset used by the QEMU virt
boot code, including banked state, group 0 SGIs, their acknowledgement
and EOI, and IRQ injection into the destination vCPU. The physical timer
model handles the counter and masked control/compare accesses. It
reports an explicit stop for an unmasked physical timer because PPI 30
delivery still needs implementation. The GIC model here covers boot
and SGIs. SPIs and nested interrupt priority handling are outside this
trace.

Mesosphère's SYS_WRITE0 debug stub normally executes `HLT #0xf000`,
which this VM delivers to the guest as an undefined instruction.
The optional trace redirects that exact instruction to an HVC and logs
bounded strings from guest RAM. It verifies the PC and operation and
supports no host file calls. This exposed the initial missing INI1
header and captures the kernel's boot diagnostics.

A separate guest patch disables the QEMU board's request to expose PMU
registers to EL0. Hypervisor.framework traps PMU accesses when guest
EL2 is unavailable on this host. The trace leaves PMU access disabled
and does not emulate those registers.

## Reproduce

Apply the previous build, native loader and SMP patches from the
[four-core entry trace](mesosphere-four-core-entry.md), then apply the
platform patch and rebuild. Use the debug target from that guide.

```sh
git -C "$ATMOSPHERE_SRC" apply --unidiff-zero "$PWD/tools/mesosphere-native-platform.patch"
DEVKITPRO="$DEVKITPRO" make -C "$ATMOSPHERE_SRC/mesosphere" -j3 qemu_virt_a57_debug
image="$ATMOSPHERE_SRC/mesosphere/out/qemu_virt_arm64_armv8a/debug/mesosphere.bin"
empty_image="$PWD/build/mesosphere-empty-ini.bin"
make all
python3 - "$image" "$empty_image" <<'PYTHON'
from pathlib import Path
import struct
import sys

image = bytearray(Path(sys.argv[1]).read_bytes())
assert image[4:8] == b"MSS1"
metadata, = struct.unpack_from("<I", image, 8)
ini = metadata + struct.unpack_from("<Q", image, metadata)[0]
loader = metadata + 8 + struct.unpack_from("<Q", image, metadata + 8)[0]
assert metadata + 0x48 <= ini <= loader - 16 and loader < len(image)
assert image[ini:ini + 16] == bytes(16)
struct.pack_into("<4sIII", image, ini, b"INI1", 16, 0, 0)
Path(sys.argv[2]).write_bytes(image)
PYTHON
python3 tools/trace_mesosphere.py "$empty_image" --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --smp-checkpoint 0x25ccc \
  --semihost-write0 0x4d6c8
```

The tested image SHA-256 was
`b10ed9e7d69c1c02d9ebddc2cae28e14bd346303112ce2d55cd698ecf7f44019`.
The user-process milestone requires an open-source KIP and enough backed
guest RAM for its allocation. The timer milestone can be measured
independently by observing the first PPI 30 delivery and acknowledgement.
