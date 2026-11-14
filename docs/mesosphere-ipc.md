# IPC between two Mesosphère processes on M1

HorizonVM ran `HvmIpcServer` (PID 1) and `HvmIpcClient` (PID 2) under
Mesosphère on Apple M1. The server opened the named port `hvm-ipc` and
accepted the client's session. The client sent a synchronous HIPC request
with one raw word, `0x12345678`. The server checked the request, XORed
the word with `0xa5a5a5a5`, and sent `0xb791f3dd` back via the same
session. The client verified the result after `SendSyncRequest` returned.

The trace checked the request and reply words in the calling processes'
live TLS mappings at the syscall boundary. At the client's completion
marker it verified the reply again in client TLS and in a user register.
The server and client had distinct TTBR0 page-table roots.

The SVC trace temporarily places an HVC at the first instruction of the
AArch64 SVC handler. For ordinary calls, HorizonVM replays the replaced
stack subtraction and resumes Mesosphère's original handler. The
kernel performs the IPC, including the session and message transfer.
The trace stops at the client's final `SVC #7` before running its
ExitProcess body. The probes use the AArch64 syscall register ABI.
[libnx syscall wrappers](https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/svc.s)

## Repeat

Use the no-process image from the
[scheduler startup guide](mesosphere-scheduler-startup.md), before adding
the empty INI1 header. The probe builder uses devkitA64's assembler and
objcopy to build the two freestanding assembly programs.

```sh
make all
python3 tools/build_el0_probe.py --ipc \
  --tool-prefix "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-" \
  "$image" build/mesosphere-ipc.bin
python3 tools/trace_mesosphere.py build/mesosphere-ipc.bin --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --el0-svc-checkpoint 0x3097c \
  --semihost-write0 0x4d6c8 --ipc-probe
```

The tested image SHA-256 was
`83c0f6f7bb1ce583808c2676021326bddd9d4b0b24a9f361a7f9b6579c6eb52b`.
The expected trace reports `IPC round trip verified`. The probe uses a
raw named port. A service manager and service lookup are separate
milestones.
