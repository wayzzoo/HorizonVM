# Physical timer interrupts in Mesosphère

On Apple M1, Mesosphère programmed the nonsecure physical timer and
received PPI 30 through HorizonVM's GICv2 model. The kernel read
`GICC_IAR` with interrupt ID 30 and wrote ID 30 to `GICC_EOIR`.
The smoke run stops all four vCPUs after that acknowledgement cycle.
The image has a zero-process INI1 header.

The virtual counter uses monotonic host uptime at 19.2 MHz.
`CNTP_CTL_EL0` supplies enable, mask and status bits, and
`CNTP_CVAL_EL0` supplies the compare value. An enabled, unmasked,
expired compare asserts a level-triggered interrupt on the local
GIC CPU interface. A masked or reprogrammed timer drops the level.
The guest can re-acknowledge a still-asserted level after EOI.
Host deadlines wake a vCPU waiting in WFI or request a VM exit
so the IRQ can be injected on the vCPU's next entry.

The GIC model supports local group 0 delivery with one active priority
level. The timer code leaves the guest's virtual timer registers
available to Mesosphère. In this build, Mesosphère uses CNTV_CVAL_EL0
for its exception stack.

## Repeat

Build the image and add the empty INI1 header as documented in the
[scheduler startup trace](mesosphere-scheduler-startup.md).
Replace that trace's `--smp-checkpoint` argument with `--timer-smoke`.

```sh
python3 tools/trace_mesosphere.py "$empty_image" --native ./horizonvm \
  --through-mmu 0x1374 --handoff 0xf8 \
  --kernel-checkpoint 0x201c --secondary-checkpoint 0x201c \
  --all-cores-entry 0x25ac0 --timer-smoke --semihost-write0 0x4d6c8
```

A successful run reports `completed PPI 30, IAR acknowledgements=1 EOI=1`.
The follow-up [EL0 trace](mesosphere-first-user-process.md) runs a standalone open-source KIP.
