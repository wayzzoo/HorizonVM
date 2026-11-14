// SPDX-License-Identifier: MIT
// Freestanding Mesosphère IPC client.
.text
.global _start
_start:
    adr x1, port_name
    svc #0x1f                       // ConnectToNamedPort -> w1
    cbnz w0, fail
    mov w20, w1
    mrs x19, tpidrro_el0

    mov w8, #4                      // HIPC request with one raw word
    mov w9, #1
    stp w8, w9, [x19]
    movz w8, #0x5678
    movk w8, #0x1234, lsl #16
    str w8, [x19, #8]
    mov w0, w20
    svc #0x21                       // SendSyncRequest
    cbnz w0, fail

    ldp w8, w9, [x19]
    cbnz w8, fail
    cmp w9, #1
    b.ne fail
    ldr w1, [x19, #8]
    movz w9, #0xf3dd
    movk w9, #0xb791, lsl #16
    cmp w1, w9
    b.ne fail
    mov x0, #0x4950                 // IPC completion marker
    svc #7
    b .

fail:
    mov x1, x0
    mov x0, #0x4951
    svc #7
    b .

port_name:
    .asciz "hvm-ipc"
