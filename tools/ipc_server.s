// SPDX-License-Identifier: MIT
// Freestanding Mesosphère IPC server. All addresses are PC-relative.
.text
.global _start
_start:
    sub sp, sp, #16
    adr x1, port_name
    mov w2, #1
    svc #0x71                       // ManageNamedPort -> w1
    cbnz w0, fail
    str w1, [sp]

    mov x1, sp
    mov w2, #1
    mov w3, #0
    mov x4, #-1
    svc #0x43                       // Wait for the client connection
    cbnz w0, fail

    ldr w1, [sp]
    svc #0x41                       // AcceptSession -> w1
    cbnz w0, fail
    mov w20, w1
    str w20, [sp]

    mrs x19, tpidrro_el0
    stp wzr, wzr, [x19]            // No receive descriptors
    mov x1, sp
    mov w2, #1
    mov w3, #0
    mov x4, #-1
    svc #0x43                       // Receive request into TLS
    cbnz w0, fail

    ldp w8, w9, [x19]
    cmp w8, #4
    b.ne fail
    cmp w9, #1
    b.ne fail
    ldr w8, [x19, #8]
    movz w9, #0x5678
    movk w9, #0x1234, lsl #16
    cmp w8, w9
    b.ne fail
    movz w9, #0xa5a5
    movk w9, #0xa5a5, lsl #16
    eor w8, w8, w9
    mov w9, #1
    stp wzr, w9, [x19]
    str w8, [x19, #8]

    mov x1, #0
    mov w2, #0
    mov w3, w20
    mov x4, #0
    svc #0x43                       // Reply; zero timeout then returns
    svc #7                          // ExitProcess
    b .

fail:
    mov x1, x0
    mov x0, #0x4952
    svc #7
    b .

port_name:
    .asciz "hvm-ipc"
