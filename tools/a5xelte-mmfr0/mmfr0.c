// SPDX-License-Identifier: GPL-2.0
/*
 * Read the raw EL1 memory-model feature register on a5xelte.
 * This module changes no CPU state or storage. Load and unload it for a
 * single measurement against the exact running Linux kernel.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static int __init a5xelte_mmfr0_init(void)
{
    u64 mmfr0;

    asm volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r" (mmfr0));
    pr_info("a5xelte-mmfr0: raw_el1=%#016llx tgran16=%llu\n",
            (unsigned long long)mmfr0,
            (unsigned long long)((mmfr0 >> 20) & 0xf));
    return 0;
}

static void __exit a5xelte_mmfr0_exit(void)
{
    pr_info("a5xelte-mmfr0: unloaded\n");
}

module_init(a5xelte_mmfr0_init);
module_exit(a5xelte_mmfr0_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only a5xelte EL1 MMFR0 probe");
