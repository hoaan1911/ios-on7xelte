/* SPDX-License-Identifier: GPL-2.0-only
 * Samsung a5xelte 4 KiB MMU probe. Diagnostic candidate, not device tested.
 * Uses the Linux port's observed EL1/MMU-off Image handoff and reserved
 * ramoops page 0x469ff000. No storage, EFS or display accesses. On a
 * completed capture it requests recovery through the tested Exynos PMU
 * reset-mode registers so the working BOOT can be restored through TWRP.
 *
 * Log layout, little-endian u64:
 * 0 signature "A5XNU001"; 1 CurrentEL; 2 SCTLR_EL1; 3 incoming FDT;
 * 4 actual image address; 5 MPIDR_EL1; 6 ID_AA64MMFR0_EL1;
 * 7 ID_AA64PFR0_EL1; 8 CNTFRQ_EL0; 9 FDT magic (big endian source);
 * 10 result (1=entry, 2=registers captured, 3=valid FDT,
 *            4=4 KiB MMU enabled, negative=MMU setup rejected);
 * 11 TCR_EL1 after setup; 12 SCTLR_EL1 after setup.
 */
.section .text.head, "ax"
.global _start
_start:
    b entry
    .long 0
    .quad 0x205000           /* Samsung legacy Image text_offset */
    .quad 0                   /* legacy image_size, as in working wrapper */
    .quad 0
    .quad 0, 0, 0
    .byte 0x41, 0x52, 0x4d, 0x64
    .long 0

entry:
    msr daifset, #15
    mov x19, x0              /* FDT from bootloader */
    mrs x20, CurrentEL
    cmp x20, #4              /* EL1 only; no assumption about EL2 */
    b.ne halt
    mrs x21, sctlr_el1
    tbnz x21, #0, halt       /* MMU must be off */
    /* The proven Linux wrapper uses this last 4 KiB of reserved log_kernel. */
    mov x13, #0x469f0000
    add x13, x13, #0xf000
    mov x14, x13
    add x15, x13, #4096
inval:
    dc ivac, x14
    add x14, x14, #32
    cmp x14, x15
    b.lo inval
    dsb sy
    mov x14, x13
zero:
    stp xzr, xzr, [x14], #16
    cmp x14, x15
    b.lo zero
    ldr x14, =0x313030554e583541 /* "A5XNU001" */
    str x14, [x13, #0]
    str x20, [x13, #8]
    str x21, [x13, #16]
    str x19, [x13, #24]
    adr x14, _start
    str x14, [x13, #32]
    mov x14, #1
    str x14, [x13, #80]
    mrs x14, mpidr_el1
    str x14, [x13, #40]
    mrs x14, id_aa64mmfr0_el1
    str x14, [x13, #48]
    mrs x14, id_aa64pfr0_el1
    str x14, [x13, #56]
    mrs x14, cntfrq_el0
    str x14, [x13, #64]
    mov x14, #2
    str x14, [x13, #80]
    cbz x19, flush
    tst x19, #7
    b.ne flush
    ldr w14, [x19]
    str x14, [x13, #72]
    rev w14, w14
    ldr w15, =0xd00dfeed
    cmp w14, w15
    b.ne flush
    mov x14, #3
    str x14, [x13, #80]
    /* The table builder uses only this Image's loaded data and stack. */
    adr x14, a5_probe_stack_top
    mov sp, x14
    bl a5_enable_4k
    mov x13, #0x469f0000
    add x13, x13, #0xf000
    cbnz x0, mmu_rejected
    mov x14, #4
    str x14, [x13, #80]
    mrs x14, tcr_el1
    str x14, [x13, #88]
    mrs x14, sctlr_el1
    str x14, [x13, #96]
    b flush
mmu_rejected:
    str x0, [x13, #80]
flush:
    dmb sy
    mov x14, x13
    add x15, x13, #4096
flush_line:
    dc cvac, x14
    add x14, x14, #32
    cmp x14, x15
    b.lo flush_line
    dsb sy
    /* Exynos7580 Linux recovery path: INFORM2, INFORM3, then SWRESET. */
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
a5_probe_stack:
    .space 4096
a5_probe_stack_top:
.global a5_l1
.balign 4096
a5_l1:
    .space 4096
.global a5_l2
.balign 4096
a5_l2:
    .space 4096
.global a5_l3
.balign 4096
a5_l3:
    .space 4096
