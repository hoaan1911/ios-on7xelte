/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
 */

#include <stdint.h>
#include <libkern/section_keywords.h>
#include <arm64/proc_reg.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>


#if HAS_GIC_V3
#define GICR_WAKE_TIMEOUT_NS (1000000000ULL) // timeout for redistributor wakeup (default 1s)
MACHINE_TIMEOUT(gicr_wake_timeout_ns, "gicr-wake-timeout", GICR_WAKE_TIMEOUT_NS, MACHINE_TIMEOUT_UNIT_NSEC, NULL);

#if defined(SWEET)
/* Phone default; QEMU virt wires the architected virtual timer to INTID 27. */
SECURITY_READ_ONLY_LATE(uint32_t) sweet_vtimer_intid = SWEET_VTIMER_INTID;
#endif

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

static uint32_t
_gic_read32(vm_offset_t addr)
{
	return *((volatile uint32_t *) addr);
}

static uint64_t
_gic_read64(vm_offset_t addr)
{
	return *((volatile uint64_t *) addr);
}

static void
_gic_write32(vm_offset_t addr, uint32_t value)
{
	*((volatile uint32_t *) addr) = value;
}

#define gicd_read32(offset) (_gic_read32(gicd_base + (offset)))
#define gicd_write32(offset, data) (_gic_write32(gicd_base + (offset), (data)))
#define gicr_read32(offset) (_gic_read32(gicr_pe_base + (offset)))
#define gicr_write32(offset, data) (_gic_write32(gicr_pe_base + (offset), (data)))
#define gicr_read64(offset) (_gic_read64(gicr_pe_base + (offset)))

static vm_offset_t
find_gicr_pe_base()
{
	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

	for (vm_offset_t offset = 0; offset < gicr_size; offset += GICR_PE_SIZE) {
		vm_offset_t gicr_pe_base = gicr_base + offset;
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

		if (phys_id == aff_value) {
			return gicr_pe_base;
		}

		if (gicr_typer & GICR_TYPER_LAST) {
			break;
		}
	}

	panic("%s: cannot find GICR base for core %u", __func__, ml_get_cpu_number(phys_id));
}

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	// gicd_base is 0x0 only before it's initialized by the boot processor.
	// Avoid calling SecureDT* routines on secondary processors to avoid
	// race conditions because they are not thread-safe.
	if (!gicd_base) {
		// Find GIC DT node
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}

		// Find "reg" property
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess) {
			panic("%s: cannot find GIC MMIO regions in DT", __func__);
		}

		// Need at least GICD base, GICD size, GICR base and GICR size
		if (prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: incorrect reg property size in GIC DT node; expecting 32 bytes but got %u bytes", __func__, prop_size);
		}

#if defined(SWEET)
		/* Read only on the boot CPU, before the GIC and RO-late lockdown. */
		void const *timer_prop;
		unsigned int timer_size;
		if (SecureDTGetProperty(entry, "virtual-timer-intid", &timer_prop, &timer_size) == kSuccess) {
			if (timer_size != sizeof(uint32_t)) {
				panic("%s: invalid virtual-timer-intid size %u", __func__, timer_size);
			}
			uint32_t intid = *(const uint32_t *)timer_prop;
			if (intid < 16 || intid >= 32) {
				panic("%s: virtual timer INTID %u is not a PPI", __func__, intid);
			}
			sweet_vtimer_intid = intid;
		}
		kprintf("SWEET GIC: virtual timer INTID %u\n", sweet_vtimer_intid);
#endif

		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();

		uint64_t const gicd_base_prop = ((uint64_t const *) prop)[0];
		uint64_t const gicd_size_prop = ((uint64_t const *) prop)[1];
		uint64_t const gicr_base_prop = ((uint64_t const *) prop)[2];
		uint64_t const gicr_size_prop = ((uint64_t const *) prop)[3];

		// Find GICD base address
		gicd_base = ml_io_map(soc_base_phys + gicd_base_prop, gicd_size_prop);

		if (!gicd_base) {
			panic("%s: cannot map GICD region", __func__);
		}

		// Find GICR base address
		gicr_base = ml_io_map(soc_base_phys + gicr_base_prop, gicr_size_prop);

		if (!gicr_base) {
			panic("%s: cannot map GICR region", __func__);
		}

		gicr_size = gicr_size_prop;
	}

	// Find the redistributor for this processor
	vm_offset_t gicr_pe_base = find_gicr_pe_base();

	// Mark this PE to be awake
	uint32_t gicr_waker = gicr_read32(GICR_WAKER);
	if (gicr_waker & GICR_WAKER_CHILDRENASLEEP) {
		gicr_waker &= ~GICR_WAKER_PROCESSORSLEEP;

		gicr_write32(GICR_WAKER, gicr_waker);

		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		while (gicr_read32(GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) {
			// Spin
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				panic("%s: core %u timed out waiting for redistributor to wake up",
				    __func__, ml_get_cpu_number_local());
			}
		}
	}

#if defined(SWEET)
	/*
	 * Real hardware with two security states: Group 0 is TrustZone's and
	 * non-secure writes to it are ignored. Everything we own is Group 1 NS
	 * and arrives as an IRQ (see sleh_irq). GICR_IGROUPR0 reads as zero
	 * from the non-secure side even after this write; delivery still works.
	 */
	gicr_write32(GICR_IGROUPR0, 0xFFFFFFFF);
	for (unsigned int i = 0; i < 32; i += 4) {
		gicr_write32(GICR_IPRIORITYR0 + i, 0xA0A0A0A0);
	}
	gicr_write32(GICR_ISENABLER0, (1U << sweet_vtimer_intid) | (1U << SWEET_IPI_SGI));
#else
	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));
#endif /* SWEET */

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

#if defined(SWEET)
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
#else
	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
#endif /* SWEET */
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

#if defined(SWEET)
	// ABL leaves only ARE_NS set; Linux writes ARE_NS | EnableGrp1A | EnableGrp1
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ARE_NS | GICD_CTLR_ENABLEGRP1A | GICD_CTLR_ENABLEGRP1;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
		;
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#else
	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
	__builtin_arm_isb(ISB_SY);
#endif /* SWEET */
}
#else
#if defined(A5XELTE)
/* Exynos7580 GIC-400: distributor and CPU interface, both non-secure Group 1. */
static vm_offset_t a5_gicd_base;
static vm_offset_t a5_gicc_base;

static uint32_t
a5_gic_read32(vm_offset_t base, unsigned int offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
a5_gic_write32(vm_offset_t base, unsigned int offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

uint32_t
a5xelte_irq_ack(void)
{
	return a5_gic_read32(a5_gicc_base, 0x00c);
}

void
a5xelte_irq_eoi(uint32_t iar)
{
	a5_gic_write32(a5_gicc_base, 0x010, iar);
}

void
a5xelte_send_self_ipi(void)
{
	__builtin_arm_dsb(DSB_ISHST);
	a5_gic_write32(a5_gicd_base, 0xf00, (2U << 24) | 1U);
}

#if defined(A5XELTE_SMP)
/* GIC CPU-interface bit of each logical CPU (banked ITARGETSR0). */
static uint8_t a5_gic_cpu_masks[SWEET_SMP_MAX_CPUS];

void
a5xelte_send_ipi(unsigned int logical)
{
	if (logical >= SWEET_SMP_MAX_CPUS || a5_gic_cpu_masks[logical] == 0) {
		panic("herolte: GICv2 target CPU%u unavailable", logical);
	}
	__builtin_arm_dsb(DSB_ISHST);
	a5_gic_write32(a5_gicd_base, 0xf00,
	    ((uint32_t)a5_gic_cpu_masks[logical] << 16) | 1U);
}
#endif

void
pe_init_fiq(void)
{
	/* Runs on every CPU. The boot CPU maps the controller before any
	 * secondary starts; SecureDT iterators are not safe on secondaries. */
	if (!a5_gicd_base) {
		DTEntry entry;
		const void *reg;
		unsigned int size;
		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &reg, &size) != kSuccess ||
		    size < 4 * sizeof(uint64_t)) {
			panic("a5xelte: missing GICv2 reg in ADT");
		}
		const uint64_t *r = reg;
		vm_offset_t soc = pe_arm_get_soc_base_phys();
		a5_gicd_base = ml_io_map(soc + r[0], r[1]);
		a5_gicc_base = ml_io_map(soc + r[2], r[3]);
	}
	if (!a5_gicd_base || !a5_gicc_base) {
		panic("a5xelte: cannot map GICv2");
	}
#if defined(A5XELTE_SMP)
	unsigned int logical = ml_get_cpu_number_local();
	uint8_t mask = a5_gic_read32(a5_gicd_base, 0x800) & 0xff;
	if (logical >= SWEET_SMP_MAX_CPUS || mask == 0 || (mask & (mask - 1)) != 0) {
		panic("herolte: invalid GICv2 CPU%u target mask 0x%x", logical, mask);
	}
	a5_gic_cpu_masks[logical] = mask;
#endif
	/* INTID 27 is the virtual timer PPI; SGI 1 is the local self signal. */
	a5_gic_write32(a5_gicd_base, 0x080,
	    a5_gic_read32(a5_gicd_base, 0x080) | (1U << 27) | (1U << 1));
	for (unsigned int i = 0; i < 32; i += 4) {
		a5_gic_write32(a5_gicd_base, 0x400 + i, 0xa0a0a0a0U);
	}
	a5_gic_write32(a5_gicd_base, 0x100, (1U << 27) | (1U << 1));
	a5_gic_write32(a5_gicc_base, 0x004, 0xff); /* GICC_PMR */
	a5_gic_write32(a5_gicc_base, 0x008, 0);    /* GICC_BPR */
	a5_gic_write32(a5_gicc_base, 0x000,
	    a5_gic_read32(a5_gicc_base, 0x000) | 1U);
	a5_gic_write32(a5_gicd_base, 0x000,
	    a5_gic_read32(a5_gicd_base, 0x000) | 1U);
	__builtin_arm_dsb(DSB_SY);
	__builtin_arm_isb(ISB_SY);
}
#else
void
pe_init_fiq()
{
}
#endif /* A5XELTE */
#endif
