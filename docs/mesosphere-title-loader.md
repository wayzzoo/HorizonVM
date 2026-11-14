# Atmosphère title loader startup on HorizonVM

HorizonVM started the unmodified `loader.kip` from Atmosphère 1.12.2
alongside its `sm.kip`. Mesosphère created the two processes as PIDs 2
and 1, respectively. The title loader connected to SM and reached
its first CMIF `GetServiceHandle("fsp-srv")` call on vCPU 3.
The trace reads that request directly from the caller's live EL0 TLS
and stops on entry to SendSyncRequest. It does not complete the lookup
or start a title.

The test image has only SM and Loader. It has no filesystem service.
Atmosphère initializes filesystem and location services before it
registers `ldr:pm`, `ldr:shel`, and `ldr:dmnt`.
The request therefore marks a concrete dependency for launching an
original applet. [Atmosphère loader initialization](https://github.com/Atmosphere-NX/Atmosphere/blob/4e3f37038/stratosphere/loader/source/ldr_main.cpp)

## Repeat

Use the release ZIP from the
[service manager trace](mesosphere-service-manager.md). Extract the KIP
with program ID `0100000000000001` as `build/loader.kip` using the same
PK31 metadata procedure, substituting the Loader program ID for SM's ID.
The Loader KIP SHA-256 is
`82f95a5ac7fa67cdeabfb885183e27c9c46554708cebaa23a3f48d35d85a1513`.

```sh
make all
python3 tools/build_el0_probe.py --loader-kips build/sm.kip build/loader.kip \
  "$image" build/mesosphere-loader.bin
python3 tools/trace_mesosphere.py build/mesosphere-loader.bin --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --el0-svc-checkpoint 0x3097c \
  --semihost-write0 0x4d6c8 --loader-probe
```

The expected trace reports `Atmosphère Loader requested fsp-srv`.
The tested packed image SHA-256 was
`6cebb511b5e4327e76ae94d8e6ebe166dbb6228d294cc4ef1ac51252a33b9221`.

The original HOME Menu case still needs a local Horizon OS system
image containing `qlaunch` and its system services. The Atmosphère
release provides its own loader and SM, but does not provide the original
applet or Nintendo's filesystem service.
