/* SPDX-License-Identifier: GPL-2.0-only
 * Samsung legacy Image wrapper for the isolated XNU entry experiment.
 * No storage accesses. Errors request the existing TWRP recovery path.
 */
.section .text.head, "ax"
.global _start
_start:
    b entry
    .long 0
    .quad 0x205000
    .quad 0
    .quad 0
    .quad 0, 0, 0
    .byte 0x41, 0x52, 0x4d, 0x64
    .long 0

entry:
    msr daifset, #15
    mov x19, x0
    mrs x20, CurrentEL
    cmp x20, #4
    b.ne halt
    mrs x21, SCTLR_EL1
    tbnz x21, #0, halt
    mov x13, #0x469f0000
    add x13, x13, #0xf000
    mov x14, x13
    add x15, x13, #4096
zero_log:
    stp xzr, xzr, [x14], #16
    cmp x14, x15
    b.lo zero_log
    ldr x14, =0x323030554e583541 /* "A5XNU002" */
    str x14, [x13]
    str x20, [x13, #8]
    str x21, [x13, #16]
    str x19, [x13, #24]
    adr x14, _start
    str x14, [x13, #32]
    mov x14, #1
    str x14, [x13, #80]
    adr x14, a5_boot_stack_top
    mov sp, x14
    mov x0, x19
    mov x1, x13
    bl a5_prepare_xnu
    mov x13, #0x469f0000
    add x13, x13, #0xf000
    cbz x0, fail
    mov x16, x0
    ldr x0, [x13, #136]       /* boot_args physical address */
    /* C cleaned copied ranges and the log. XNU's entry needs MMU off. */
    mrs x14, SCTLR_EL1
    bic x14, x14, #1
    bic x14, x14, #4
    bic x14, x14, #0x1000
    msr SCTLR_EL1, x14
    isb
    br x16
fail:
    mov x14, x13
    add x15, x13, #4096
flush_log:
    dc cvac, x14
    add x14, x14, #32
    cmp x14, x15
    b.lo flush_log
    dsb sy
    ldr x0, =0x10460000
    ldr w14, =0x12345678
    str w14, [x0, #0x808]
    ldr w14, =0x12345674
    str w14, [x0, #0x80c]
    dsb sy
    mov w14, #1
    str w14, [x0, #0x400]
    dsb sy
halt:
    wfe
    b halt
    .ltorg

.section .data, "aw"
.balign 16
a5_boot_stack:
    .space 4096
a5_boot_stack_top:
