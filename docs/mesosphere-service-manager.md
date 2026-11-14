# Atmosphère service manager on HorizonVM

HorizonVM booted the actual `sm.kip` from the open-source Atmosphère
1.12.2 release as PID 1 on vCPU 3. A separate probe process on vCPU 0
connected to `sm:`, registered as a client, registered the service
`hvm:test`, and resolved it through GetServiceHandle. The kernel
transferred the port and session handles. The trace checked that
the returned session handle was nonzero, distinct from the port
handle, and present in the client's successful SFCO reply.

This tests named-service discovery through the guest's service
manager. The probe stops before using the resulting session. The
earlier [two-process trace](mesosphere-ipc.md) separately verifies
request and reply traffic on a session. Neither trace loads a
Nintendo system applet.

## Repeat

Use the no-process image from the
[scheduler startup guide](mesosphere-scheduler-startup.md), before adding
the empty INI1 header. Get the Atmosphère 1.12.2 ZIP from the
[official release assets](https://github.com/Atmosphere-NX/Atmosphere/releases/expanded_assets/1.12.2).
Set `ATMOSPHERE_ZIP` to the local ZIP. The following extracts only the
open-source SM module from the release and checks its embedded digest.

```sh
make all
python3 - "$ATMOSPHERE_ZIP" build/sm.kip <<'PYTHON'
from pathlib import Path
import hashlib
import struct
import sys
import zipfile

archive = Path(sys.argv[1])
assert hashlib.sha256(archive.read_bytes()).hexdigest() == (
    "57d0019ff430af4b712899ff58fd341da8d9ed38159c51ade8e29fd42ecb2dda"
)
with zipfile.ZipFile(archive) as release:
    package = release.read("atmosphere/package3")
assert package[:4] == b"PK31"
count, = struct.unpack_from("<I", package, 0x10)
for entry in range(count + 1):
    program_id, offset, size, digest = struct.unpack_from(
        "<QII32s", package, 0x400 + 0x30 * entry
    )
    if program_id == 0x0100000000000004:
        kip = package[0x100000 + offset:0x100000 + offset + size]
        assert len(kip) == size and hashlib.sha256(kip).digest() == digest
        Path(sys.argv[2]).write_bytes(kip)
        break
else:
    raise RuntimeError("Atmosphère SM entry not found")
PYTHON
python3 tools/build_el0_probe.py --sm-kip build/sm.kip \
  --tool-prefix "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-" \
  "$image" build/mesosphere-sm.bin
python3 tools/trace_mesosphere.py build/mesosphere-sm.bin --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --el0-svc-checkpoint 0x3097c \
  --semihost-write0 0x4d6c8 --sm-probe
```

The tested `sm.kip` SHA-256 was
`469a82c377b0f12151787c6584faf1100aa6f642998ad5d1022fcacc5701ab5c`.
The packed guest image was
`8604f7efbb9907d62bc59d366954aac846da78a26e52d397685dc3737e610194`.

An original HOME Menu bring-up requires local Horizon OS system
content with `qlaunch` (program ID `0100000000001000`) for a known
firmware version. The QEMU board is an upstream kernel test platform.
The original applet also needs a compatible title loader and the system
services and devices it uses. The service manager is one prerequisite
for observing those service requests.
