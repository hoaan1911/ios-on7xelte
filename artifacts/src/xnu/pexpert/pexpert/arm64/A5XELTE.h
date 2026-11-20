/* Samsung Galaxy A5 (2016), SM-A510F: Exynos 7580, Cortex-A53. */
#ifndef _PEXPERT_ARM64_A5XELTE_H
#define _PEXPERT_ARM64_A5XELTE_H

#define NO_MONITOR                    1
#define NO_ECORE                      1

#define A5XELTE                       1
/* Reuse donor's SoC-neutral early services until their guards are renamed. */
#define SWEET                         1
/* Secondary-CPU bring-up (PSCI CPU_ON + GICv2 SGIs) shared by HEROLTE and the 4+4 A53 boards. */
#define A5XELTE_SMP                   1

/* Firmware scanout buffer and the earliest XNU marker (physical). */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
/* Galaxy S7 SM-G930F: S-Boot leaves DECON_F scanning out this buffer. */
#define HEROLTE                       1
/* Logical 0..3: Cortex-A53 "Apollo" MPIDR 0x100..0x103 (boot cluster);
 * 4..7: Mongoose MPIDR 0x0..0x3. All started with PSCI CPU_ON. */
#define SWEET_SMP_MAX_CPUS            8
#define A5XELTE_FB_PA                 0xe2a00000
#define A5XELTE_FB_WIDTH              1440
#define A5XELTE_FB_HEIGHT             2560
#define A5XELTE_FB_STRIDE             5760
#define A5XELTE_FB_BLOCKS             8     /* 2 MiB VA=PA bootstrap blocks */
#define A5XELTE_FB_TEXT_SCALE         3
#define A5XELTE_FB_DOT_PITCH          458   /* iPhone X userland density */
#define A5XELTE_MARKER_PA             0x8ffff100
/* S-Boot starts on the Cortex-A53 "Apollo" cluster, Aff1 = 1. */
#define A5XELTE_BOOT_MPIDR            0x100
#else
/* Exynos7870/7580: logical 0..3 = MPIDR 0..3 (boot cluster), 4..7 = MPIDR 0x100..0x103. */
#define SWEET_SMP_MAX_CPUS            8
#define A5XELTE_FB_PA                 0x67000000
#define A5XELTE_FB_WIDTH              1080
#define A5XELTE_FB_HEIGHT             1920
#define A5XELTE_FB_STRIDE             4320
#define A5XELTE_FB_BLOCKS             5
#define A5XELTE_FB_TEXT_SCALE         2
#define A5XELTE_FB_DOT_PITCH          424   /* 1080 pixels across 5.2 inches */
#define A5XELTE_MARKER_PA             0x469ff100
#define A5XELTE_BOOT_MPIDR            0x0
#endif

#define ARM_ARCH_TIMER
#define __ARM_ARCH__                  8
#define __ARM_VMSA__                  8
#define __ARM_VFP__                   4
#define __ARM_COHERENT_CACHE__        1
#define __ARM_DEBUG__                 7
#define __ARM_ENABLE_SWAP__           1
#define __ARM_V8_CRYPTO_EXTENSIONS__  1
#define __ARM_PAN_AVAILABLE__         0

/* Exynos7580 has a GICv2 distributor and CPU interface, not GICv3. */
#define HAS_GIC_V3                    0
#define A5XELTE_VTIMER_INTID         27

#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif

#endif /* _PEXPERT_ARM64_A5XELTE_H */
