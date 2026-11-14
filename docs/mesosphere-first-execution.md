# First Mesosphère instructions on Apple Silicon

HorizonVM reached Mesosphère's loader at `0x4027a000` on an Apple M1 and
executed its first instruction, `b _main` (opcode `0x14000004`). A second
hardware breakpoint stopped the guest at `0x4027a010`. QEMU 11.1.3 reached
the same PCs and reported the same handoff registers for the same image.

| Value | QEMU TCG | Apple Hypervisor.framework |
| --- | --- | --- |
| Kernel entry PC | `0x40200000` | `0x40200000` |
| Loader entry PC | `0x4027a000` | `0x4027a000` |
| PC after the loader branch | `0x4027a010` | `0x4027a010` |
| PSTATE at loader entry (EL1h) | `0x600003c5` | `0x600003c5` |
| X0 (image base) | `0x40200000` | `0x40200000` |
| X1 (KernelLayout) | `0x4025b214` | `0x4025b214` |
| X2 (INI1 address) | `0x40279000` | `0x40279000` |
| X30 (kernel return PC) | `0x4025b068` | `0x4025b068` |

The two native stops reported exit reason 1 (exception) and syndrome
`0xc2000022` (hardware breakpoint). SCTLR_EL1 was `0x30100180` at both
stops, with translation off. The run maps the image and stops before
loader BSS initialization. The next run needs writable backing for the
loader BSS and stack, then a separate check for MMU enablement.

## Build and repeat

Use [Atmosphère revision 3e891501](https://github.com/Atmosphere-NX/Atmosphere/tree/3e8915011e1de89f595462d929c1cc3294470896)
and devkitA64 r29.2. The compiler and binutils were native arm64 macOS
packages obtained from the [devkitA64 archive](https://wii.leseratte10.de/devkitPro/devkitA64/r29%20%282026-01%29/).
The official package installer endpoint returned HTTP 403. The archives
were extracted into a temporary prefix, with the [official build rules
v1.1.1](https://github.com/devkitPro/devkita64-rules/releases/tag/v1.1.1)
installed under its `devkitA64/` directory.

| Package | SHA-256 |
| --- | --- |
| devkita64-gcc-15.2.0-7-osx_arm64 | `1917899845602cc5c43a74196032ed924bd03751aba2f6405532ec796fd65639` |
| devkita64-binutils-2.45.1-2-osx_arm64 | `ab164a57abbc8bef8423b5bfe1ce542d82bf9e8c7c40ddd6cee87dbd5d37e165` |
| devkita64-newlib-4.6.0.20260123-4-any | `fc97ba1009a94b68f8714c3cc548ef697ca82687b72fe0c05a7ca3df81beb53e` |

The stock QEMU target hit three build errors. The patch demotes GCC's
array bounds warning for the QEMU board's zero-entry I/O pool, supplies
the missing QEMU memory-pool alignment constants using the NX board
defaults, and includes the existing assembly macro used by the EL2
entry path. The run uses the EL1 path and the original loader instructions.
The warnings and the later behavior of the QEMU memory manager remain
outside the verified portion of execution.

From the HorizonVM checkout, set `ATMOSPHERE_SRC` to a clean checkout
of the pinned revision and `DEVKITPRO` to the toolchain prefix.

```sh
git -C "$ATMOSPHERE_SRC" apply --unidiff-zero "$PWD/tools/mesosphere-qemu-build.patch"
DEVKITPRO="$DEVKITPRO" make -C "$ATMOSPHERE_SRC/mesosphere" -j3 qemu_virt_a57_debug
image="$ATMOSPHERE_SRC/mesosphere/out/qemu_virt_arm64_armv8a/debug/mesosphere.bin"
make all
python3 tools/trace_mesosphere.py "$image"
python3 tools/trace_mesosphere.py "$image" --native ./horizonvm
make run
```

The trace tool hashes the same bytes it sends to each backend. QEMU
uses Cortex-A57, GICv2 and one CPU for this entry trace. The shared image
base leaves room for QEMU's device tree. The native run uses a two-second
vCPU cancellation deadline per breakpoint and prints the guest's PC,
PSTATE, EL1 registers and VM exit. The tested raw image SHA-256 was
`e412d5dfd2cd0a7a2b9e0e84f7f67cb34813b86c63461ac5adba0d8fb62d18be`.
