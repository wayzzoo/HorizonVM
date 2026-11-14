// SPDX-License-Identifier: MIT
.text
.global _start
_start:
    mrs x19, tpidrro_el0
    mov w21, #64
connect:
    adr x1, sm_port
    svc #0x1f
    cbz w0, connected
    subs w21, w21, #1
    b.eq fail
    movz x0, #0x9680
    movk x0, #0x98, lsl #16    // 10ms
    svc #0x0b
    b connect
connected:
    mov w20, w1
    stp xzr, xzr, [x19]
    stp xzr, xzr, [x19, #16]
    stp xzr, xzr, [x19, #32]
    mov w8, #4
    movz w9, #10
    movk w9, #0x8000, lsl #16
    stp w8, w9, [x19]
    mov w8, #1
    str w8, [x19, #8]          // Kernel inserts actual PID
    movz w8, #0x4653
    movk w8, #0x4943, lsl #16
    str w8, [x19, #32]         // SFCI, RegisterClient command zero
    mov w0, w20
    svc #0x21
    cbnz w0, fail
    ldr w0, [x19, #24]         // SFCO result
    cbnz w0, fail

    mov w8, #4
    mov w9, #12
    stp w8, w9, [x19]
    stp xzr, xzr, [x19, #8]
    movz w8, #0x4653
    movk w8, #0x4943, lsl #16
    str w8, [x19, #16]         // SFCI
    mov w9, #2
    stp w9, wzr, [x19, #24]    // RegisterService
    adr x8, service_name
    ldr x8, [x8]
    str x8, [x19, #32]
    mov w9, #1
    stp wzr, w9, [x19, #40]    // Normal session, limit 1
    mov w0, w20
    svc #0x21
    cbnz w0, fail
    ldr w0, [x19, #24]
    cbnz w0, fail
    ldr w22, [x19, #12]
    cbz w22, fail

    mov w8, #4
    mov w9, #10
    stp w8, w9, [x19]
    stp xzr, xzr, [x19, #8]
    movz w8, #0x4653
    movk w8, #0x4943, lsl #16
    str w8, [x19, #16]
    mov w9, #1
    stp w9, wzr, [x19, #24]    // GetServiceHandle
    adr x8, service_name
    ldr x8, [x8]
    str x8, [x19, #32]
    mov w0, w20
    svc #0x21
    cbnz w0, fail
    ldr w0, [x19, #24]
    cbnz w0, fail
    ldr w1, [x19, #12]
    cbz w1, fail
    cmp w1, w22
    b.eq fail
    mov w2, w22
    mov x0, #0x534d
    svc #7
    b .
fail:
    mov x1, x0
    mov x0, #0x534e
    svc #7
    b .
sm_port:
    .asciz "sm:"
.balign 8
service_name:
    .ascii "hvm:test"
