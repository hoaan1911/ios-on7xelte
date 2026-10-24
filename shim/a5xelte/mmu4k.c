/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Single-core EL1 identity map for the isolated a5xelte MMU probe.
 * The first 4 GiB defaults to Device memory. Only the loaded Image pages
 * become executable Normal WB memory. No peripheral register is touched.
 */
#include <stdint.h>

#define PAGE_SHIFT 12U
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define L2_SHIFT 21U
#define L2_SIZE (1UL << L2_SHIFT)
#define L1_SHIFT 30U
#define ENTRIES 512U

#define DESC_BLOCK 1ULL
#define DESC_TABLE 3ULL
#define DESC_PAGE 3ULL
#define DESC_AF (1ULL << 10)
#define DESC_SH_INNER (3ULL << 8)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)
#define DESC_ATTR_WB (1ULL << 2)
#define DEVICE_FLAGS (DESC_AF | DESC_PXN | DESC_UXN)
#define CODE_FLAGS (DESC_ATTR_WB | DESC_SH_INNER | DESC_AF | DESC_UXN)

extern uint64_t a5_l1[ENTRIES], a5_l2[ENTRIES], a5_l3[ENTRIES];
extern char _start[], _image_end[];

static inline uint64_t read_mmfr0(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r" (value));
    return value;
}

static inline uint64_t read_sctlr(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r" (value));
    return value;
}

int a5_enable_4k(void)
{
    uintptr_t lo = (uintptr_t)_start & ~(PAGE_SIZE - 1);
    uintptr_t hi = ((uintptr_t)_image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t sctlr = read_sctlr();
    uint64_t mmfr0 = read_mmfr0();

    if (((mmfr0 >> 28) & 0xf) != 0)
        return -1; /* Cortex-A53 must report the 4 KiB granule */
    if (sctlr & 1)
        return -2; /* handoff must be MMU-off */
    if (!lo || hi > (1ULL << 32) || (lo >> L2_SHIFT) != ((hi - 1) >> L2_SHIFT))
        return -3; /* one 2 MiB L3 table covers the whole Image */
    if (((uintptr_t)a5_l1 & (PAGE_SIZE - 1)) ||
        ((uintptr_t)a5_l2 & (PAGE_SIZE - 1)) ||
        ((uintptr_t)a5_l3 & (PAGE_SIZE - 1)))
        return -4;

    /* 32-bit VA, 4 KiB granule: L1 -> L2 -> L3. */
    for (unsigned i = 0; i < 4; ++i)
        a5_l1[i] = ((uint64_t)i << L1_SHIFT) | DEVICE_FLAGS | DESC_BLOCK;
    uintptr_t l1_index = lo >> L1_SHIFT;
    uintptr_t one_gib = lo & ~((1UL << L1_SHIFT) - 1);
    for (unsigned i = 0; i < ENTRIES; ++i)
        a5_l2[i] = (one_gib + ((uint64_t)i << L2_SHIFT)) |
                   DEVICE_FLAGS | DESC_BLOCK;
    a5_l1[l1_index] = (uintptr_t)a5_l2 | DESC_TABLE;

    uintptr_t two_mib = lo & ~(L2_SIZE - 1);
    for (unsigned i = 0; i < ENTRIES; ++i) {
        uintptr_t pa = two_mib + ((uintptr_t)i << PAGE_SHIFT);
        a5_l3[i] = pa | ((pa >= lo && pa < hi) ? CODE_FLAGS : DEVICE_FLAGS) |
                   DESC_PAGE;
    }
    a5_l2[(lo >> L2_SHIFT) & (ENTRIES - 1)] = (uintptr_t)a5_l3 | DESC_TABLE;

    /* Cache lines are 4 << CTR_EL0.DminLine bytes. */
    uint64_t ctr;
    __asm__ volatile("mrs %0, CTR_EL0" : "=r" (ctr));
    uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
    for (uintptr_t p = (uintptr_t)a5_l1; p < (uintptr_t)a5_l1 + PAGE_SIZE; p += line)
        __asm__ volatile("dc cvac, %0" : : "r" (p) : "memory");
    for (uintptr_t p = (uintptr_t)a5_l2; p < (uintptr_t)a5_l2 + PAGE_SIZE; p += line)
        __asm__ volatile("dc cvac, %0" : : "r" (p) : "memory");
    for (uintptr_t p = (uintptr_t)a5_l3; p < (uintptr_t)a5_l3 + PAGE_SIZE; p += line)
        __asm__ volatile("dc cvac, %0" : : "r" (p) : "memory");
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" : : : "memory");

    /* MAIR attr0=Device-nGnRnE, attr1=Normal WB; TTBR1 walk disabled. */
    uint64_t tcr = 32ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |
                   (32ULL << 16) | (1ULL << 23) | (2ULL << 30);
    uint64_t mair = 0xff00ULL;
    __asm__ volatile("msr MAIR_EL1, %0" : : "r" (mair) : "memory");
    __asm__ volatile("msr TCR_EL1, %0" : : "r" (tcr) : "memory");
    __asm__ volatile("msr TTBR0_EL1, %0" : : "r" ((uintptr_t)a5_l1) : "memory");
    __asm__ volatile("isb; tlbi vmalle1; dsb sy; isb" : : : "memory");
    sctlr &= ~(1ULL << 19); /* WXN: executable writable probe pages */
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12); /* M, C, I */
    __asm__ volatile("msr SCTLR_EL1, %0; isb" : : "r" (sctlr) : "memory");
    return 0;
}
