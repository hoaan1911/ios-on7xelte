/*
 * HEROLTE (Galaxy S7 SM-G930F, Exynos 8890) Mali-T880 MP12, stage 1:
 * read-only probe, as A5XELTE's 0126 did for its Mali-T720. From the stock
 * BOOT DTB: mali@14AC0000 (0x5000, SPIs JOB 0xe1, MMU 0xe2, GPU 0xe0),
 * CMU_G3D 0x14AA0000, PMU G3D domain CONFIGURATION/STATUS 0x105C4060/4064
 * (exynos-snapshot reads CMU_G3D only when 0x105C4064 and 0x105C6104 say
 * powered), vdd_g3d = S2MPS16 BUCK6, regulator-always-on.
 *
 * The probe writes nothing. CMU_G3D and the GPU are read only when the
 * domain status low nibble is 0xf: a powered-off block can hang the bus.
 * Boot-arg herogpu=0 skips everything, 1 (default) probes, 2 also dumps the
 * PMIC, 3-5 also run GPU jobs on one core (see the stage comment below).
 */
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void herolte_gpu_probe(void);

#define HG_PMU_PA       0x105c0000u
#define HG_PMU_G3D_CONF 0x4060u
#define HG_PMU_G3D_STAT 0x4064u
#define HG_PMU_G3D_AUX  0x6104u         /* second exynos-snapshot condition */
#define HG_CMU_PA       0x14aa0000u
#define HG_GPU_PA       0x14ac0000u

/* Mali Midgard register offsets (Linux mali_kbase / Panfrost). */
#define GPU_ID                  0x000
#define L2_FEATURES             0x004
#define TILER_FEATURES          0x00c
#define MEM_FEATURES            0x010
#define MMU_FEATURES            0x014
#define AS_PRESENT              0x018
#define JS_PRESENT              0x01c
#define GPU_IRQ_RAWSTAT         0x020
#define GPU_STATUS              0x034
#define SHADER_PRESENT_LO       0x100
#define TILER_PRESENT_LO        0x110
#define L2_PRESENT_LO           0x120
#define SHADER_READY_LO         0x140
#define TILER_READY_LO          0x150
#define L2_READY_LO             0x160

static int hg_sleep;

static uint32_t
hg_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
hg_msleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&hg_sleep, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool
hg_powered(vm_offset_t pmu)
{
	return (hg_rd(pmu, HG_PMU_G3D_STAT) & 0xf) == 0xf;
}

static void
hg_probe(vm_offset_t pmu, const char *when)
{
	uint32_t conf = hg_rd(pmu, HG_PMU_G3D_CONF);
	uint32_t stat = hg_rd(pmu, HG_PMU_G3D_STAT);
	uint32_t aux = hg_rd(pmu, HG_PMU_G3D_AUX);
	printf("HEROLTE GPU: %s: PMU G3D conf=%#x status=%#x aux=%#x\n", when, conf, stat, aux);
	if ((stat & 0xf) != 0xf) {
		printf("HEROLTE GPU: %s: G3D domain off; CMU and Mali not read\n", when);
		return;
	}
	static vm_offset_t cmu, gpu;
	if (!cmu) {
		cmu = ml_io_map(HG_CMU_PA, 0x2000);
	}
	if (!gpu) {
		gpu = ml_io_map(HG_GPU_PA, 0x5000);
	}
	if (!cmu || !gpu) {
		printf("HEROLTE GPU: MMIO mapping failed\n");
		return;
	}
	/* The exynos-snapshot CMU_G3D list: muxes 0x2xx, dividers 0x4xx,
	 * 0x6xx, gates 0x8xx/0x9xx/0xa00. */
	static const uint16_t regs[] = {
		0x200, 0x204, 0x20c, 0x210, 0x214, 0x400, 0x404, 0x408, 0x40c,
		0x600, 0x604, 0x60c, 0x610, 0x614, 0x800, 0x804, 0x900, 0xa00,
	};
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i += 6) {
		printf("HEROLTE GPU: CMU_G3D");
		for (unsigned j = i; j < i + 6 && j < sizeof(regs) / sizeof(regs[0]); j++) {
			printf(" %03x=%#x", regs[j], hg_rd(cmu, regs[j]));
		}
		printf("\n");
	}
	if (!hg_powered(pmu)) {
		printf("HEROLTE GPU: %s: domain went off; Mali not read\n", when);
		return;
	}
	uint32_t id = hg_rd(gpu, GPU_ID);
	printf("HEROLTE GPU: %s: GPU_ID=%#x product %#x (0x880 = Mali-T880) rev r%up%u\n",
	    when, id, id >> 16, (id >> 12) & 0xf, (id >> 4) & 0xff);
	printf("HEROLTE GPU: L2_FEATURES=%#x TILER=%#x MEM=%#x MMU=%#x AS=%#x JS=%#x\n",
	    hg_rd(gpu, L2_FEATURES), hg_rd(gpu, TILER_FEATURES), hg_rd(gpu, MEM_FEATURES),
	    hg_rd(gpu, MMU_FEATURES), hg_rd(gpu, AS_PRESENT), hg_rd(gpu, JS_PRESENT));
	printf("HEROLTE GPU: present shader=%#x tiler=%#x l2=%#x; ready shader=%#x tiler=%#x l2=%#x\n",
	    hg_rd(gpu, SHADER_PRESENT_LO), hg_rd(gpu, TILER_PRESENT_LO), hg_rd(gpu, L2_PRESENT_LO),
	    hg_rd(gpu, SHADER_READY_LO), hg_rd(gpu, TILER_READY_LO), hg_rd(gpu, L2_READY_LO));
	printf("HEROLTE GPU: GPU_STATUS=%#x IRQ_RAWSTAT=%#x; no GPU jobs\n",
	    hg_rd(gpu, GPU_STATUS), hg_rd(gpu, GPU_IRQ_RAWSTAT));
}

/*
 * Stage 2 (herogpu>=2): S2MPS16 registers, read through the APM like the
 * touch supplies, to find vdd_g3d (BUCK6). Nothing is written.
 */
extern bool herolte_apm_pmic_read(uint8_t reg, uint8_t *value);

static void
hg_pmic_dump(void)
{
	for (unsigned base = 0; base < 0x80; base += 16) {
		uint8_t v[16];
		for (unsigned i = 0; i < 16; i++) {
			if (!herolte_apm_pmic_read((uint8_t)(base + i), &v[i])) {
				printf("HEROLTE GPU: PMIC read %#x failed (APM not started?)\n", base + i);
				return;
			}
		}
		printf("HEROLTE GPU: S2MPS16 %02x: %02x %02x %02x %02x %02x %02x %02x %02x "
		    "%02x %02x %02x %02x %02x %02x %02x %02x\n", base, v[0], v[1], v[2], v[3],
		    v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
	}
}

/*
 * Stages 3-5: GPU jobs in a private 64 KiB block of the first DRAM bank,
 * outside XNU's RAM, mapped uncached on the CPU side (no cache
 * maintenance): Mali-LPAE page tables at +0x0000..+0x3fff, job and data
 * pages at +0x4000.. mapped at GPU VA 0x02000000. Only L2, shader core 0
 * and the tiler are powered (voltage and clock as S-Boot left them), all
 * polls are bounded, and the cores are powered off at the end.
 *
 *   herogpu=3  WRITE_VALUE on JS1 (A5XELTEGPU.cpp; done on the phone, 0226)
 *   herogpu=4  + fragment clear of a private 16x16 RGBA8 target on JS0
 *   herogpu=5  + fragment clear of a 128x128 green square in the middle of
 *              the panel scanout buffer (0xe2a00000, stride 5760)
 *
 * The fragment job is the A5's (0131-0133): Mesa Panfrost Midgard v4
 * single-target framebuffer descriptor and the empty polygon list with
 * hierarchical tiling disabled, in its own page. Mesa uses the multi-target
 * descriptor from v5 (T760..T880) on; whether the T880 still takes the
 * single-target one is what herogpu=4 finds out.
 */
#define HG_TEST_PA      0x8ffe0000u     /* below the SMP trampoline 0x8fffe000 */
#define HG_TEST_BYTES   0x10000u
#define HG_TEST_VA      0x02000000ull
#define HG_DATA_OFF     0x4000u         /* GPU VA HG_TEST_VA: 12 data pages */
#define HG_OUT_OFF      0x100u
#define HG_FBD_OFF      0x100u
#define HG_POLY_OFF     0x1000u
#define HG_PIX_OFF      0x2000u
#define HG_RED          0xff0000ffu
#define HG_GREEN        0xff00ff00u

/* Panel scanout (mkadt_herolte.py FB_BASE) and the square to clear. */
#define HG_FB_PA        0xe2a00000ull
#define HG_FB_STRIDE    5760u
#define HG_SQ_X         656u            /* (1440 - 128) / 2, 16-pixel aligned */
#define HG_SQ_Y         1216u           /* (2560 - 128) / 2 */
#define HG_SQ_SIZE      128u
#define HG_SQ_L3        64u             /* l3 index of the first panel page */

#define SHADER_PWRON_LO 0x180
#define TILER_PWRON_LO  0x190
#define L2_PWRON_LO     0x1a0
#define SHADER_PWROFF_LO 0x1c0
#define TILER_PWROFF_LO 0x1d0
#define L2_PWROFF_LO    0x1e0
#define GPU_FAULTSTATUS 0x03c
#define GPU_FAULTADDR_LO 0x040
#define JOB_IRQ_RAWSTAT 0x1000
#define JOB_IRQ_CLEAR   0x1004
#define JS_BASE(n)      (0x1800u + (n) * 0x80u)
#define MMU_IRQ_RAWSTAT 0x2000
#define MMU_IRQ_CLEAR   0x2004
#define AS0             0x2400

static vm_offset_t hg_gpu, hg_mem;

static void
hg_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static bool
hg_power(uint32_t on, uint32_t ready, uint32_t mask, const char *name)
{
	hg_wr(hg_gpu, on, mask);
	uint32_t r = 0;
	for (unsigned us = 0; us < 20000; us += 10) {
		r = hg_rd(hg_gpu, ready);
		if ((r & mask) == mask) {
			break;
		}
		delay(10);
	}
	printf("HEROLTE GPU: power %s mask=%#x ready=%#x\n", name, mask, r);
	return (r & mask) == mask;
}

static bool
hg_cores_on(void)
{
	return hg_power(L2_PWRON_LO, L2_READY_LO, hg_rd(hg_gpu, L2_PRESENT_LO), "L2") &&
	       hg_power(SHADER_PWRON_LO, SHADER_READY_LO, 1, "shader core 0") &&
	       hg_power(TILER_PWRON_LO, TILER_READY_LO, hg_rd(hg_gpu, TILER_PRESENT_LO), "tiler");
}

static void
hg_cores_off(void)
{
	hg_wr(hg_gpu, TILER_PWROFF_LO, hg_rd(hg_gpu, TILER_READY_LO));
	hg_wr(hg_gpu, SHADER_PWROFF_LO, hg_rd(hg_gpu, SHADER_READY_LO));
	hg_wr(hg_gpu, L2_PWROFF_LO, hg_rd(hg_gpu, L2_READY_LO));
	printf("HEROLTE GPU: cores powered off\n");
}

static void
hg_zero(vm_offset_t va, size_t bytes)
{
	volatile uint64_t *q = (volatile uint64_t *)va;
	for (size_t i = 0; i < bytes / 8; i++) {
		q[i] = 0;
	}
}

/* Fresh page tables: 12 data pages, plus the square's panel rows if asked. */
static void
hg_tables(bool panel)
{
	hg_zero(hg_mem, HG_TEST_BYTES);
	volatile uint64_t *l0 = (volatile uint64_t *)hg_mem;
	volatile uint64_t *l1 = (volatile uint64_t *)(hg_mem + 0x1000);
	volatile uint64_t *l2t = (volatile uint64_t *)(hg_mem + 0x2000);
	volatile uint64_t *l3 = (volatile uint64_t *)(hg_mem + 0x3000);
	l0[0] = (HG_TEST_PA + 0x1000) | 3;
	l1[0] = (HG_TEST_PA + 0x2000) | 3;
	l2t[(HG_TEST_VA >> 21) & 0x1ff] = (HG_TEST_PA + 0x3000) | 3;
	for (unsigned i = 0; i < (HG_TEST_BYTES - HG_DATA_OFF) / 0x1000; i++) {
		/* Mali-LPAE leaf: read/write, outer shareable, noncached. */
		l3[i] = (HG_TEST_PA + HG_DATA_OFF + i * 0x1000) | 0x2c1;
	}
	if (panel) {
		uint64_t first = (HG_FB_PA + (uint64_t)HG_SQ_Y * HG_FB_STRIDE + HG_SQ_X * 4) & ~0xfffull;
		uint64_t last = HG_FB_PA + (uint64_t)(HG_SQ_Y + HG_SQ_SIZE - 1) * HG_FB_STRIDE +
		    (HG_SQ_X + HG_SQ_SIZE) * 4;
		for (uint64_t pa = first, i = 0; pa < last; pa += 0x1000, i++) {
			l3[HG_SQ_L3 + i] = pa | 0x2c1;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Program AS0, start the job at HG_TEST_VA on slot js, wait (bounded). */
static bool
hg_run(unsigned js, const char *what)
{
	uint32_t slot = JS_BASE(js);
	if (hg_rd(hg_gpu, slot + 0x60) || hg_rd(hg_gpu, slot + 0x24) & 0xff) {
		printf("HEROLTE GPU: %s: slot %u busy\n", what, js);
	}
	hg_wr(hg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	hg_wr(hg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	uint64_t transtab = HG_TEST_PA | 0x7;
	hg_wr(hg_gpu, AS0 + 0x00, (uint32_t)transtab);
	hg_wr(hg_gpu, AS0 + 0x04, (uint32_t)(transtab >> 32));
	hg_wr(hg_gpu, AS0 + 0x08, 0x888d88);
	hg_wr(hg_gpu, AS0 + 0x0c, 0);
	hg_wr(hg_gpu, AS0 + 0x18, 1);           /* AS_COMMAND_UPDATE */
	for (unsigned i = 0; i < 10000 && (hg_rd(hg_gpu, AS0 + 0x28) & 1); i++) {
		delay(10);
	}
	if (hg_rd(hg_gpu, AS0 + 0x28) & 1) {
		printf("HEROLTE GPU: %s: MMU update timeout\n", what);
		return false;
	}
	hg_wr(hg_gpu, slot + 0x40, (uint32_t)HG_TEST_VA);       /* HEAD_NEXT */
	hg_wr(hg_gpu, slot + 0x44, 0);
	hg_wr(hg_gpu, slot + 0x50, 1);                          /* AFFINITY: core 0 */
	hg_wr(hg_gpu, slot + 0x54, 0);
	hg_wr(hg_gpu, slot + 0x58, (8 << 16) | (3 << 8) | (3 << 12));
	hg_wr(hg_gpu, slot + 0x60, 1);                          /* START */

	uint32_t jirq = 0, mirq = 0, girq = 0;
	for (unsigned i = 0; i < 20000; i++) {
		jirq = hg_rd(hg_gpu, JOB_IRQ_RAWSTAT);
		mirq = hg_rd(hg_gpu, MMU_IRQ_RAWSTAT);
		girq = hg_rd(hg_gpu, GPU_IRQ_RAWSTAT);
		if ((jirq & ((1u << js) | (1u << (js + 16)))) || mirq || (girq & 0x81)) {
			break;
		}
		delay(10);
	}
	volatile uint32_t *hdr = (volatile uint32_t *)(hg_mem + HG_DATA_OFF);
	bool ok = (jirq & (1u << js)) && !(jirq & (1u << (js + 16))) && !mirq && !(girq & 0x81);
	printf("HEROLTE GPU: %s: irq=%#x mmu=%#x gpu=%#x js_status=%#x as_fault=%#x "
	    "exception=%#x fault=%#x:%#x gpu_fault=%#x@%#x\n", what, jirq, mirq, girq,
	    hg_rd(hg_gpu, slot + 0x24), hg_rd(hg_gpu, AS0 + 0x1c), hdr[0], hdr[3], hdr[2],
	    hg_rd(hg_gpu, GPU_FAULTSTATUS), hg_rd(hg_gpu, GPU_FAULTADDR_LO));
	hg_wr(hg_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	hg_wr(hg_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	return ok;
}

static bool
hg_job_write_value(void)
{
	hg_tables(false);
	volatile uint8_t *job = (volatile uint8_t *)(hg_mem + HG_DATA_OFF);
	job[16] = 1 | (2 << 1);                         /* 64-bit, WRITE_VALUE */
	*(volatile uint16_t *)(job + 18) = 1;           /* index */
	*(volatile uint64_t *)(job + 32) = HG_TEST_VA + HG_OUT_OFF;
	*(volatile uint64_t *)(job + 40) = 3;           /* zero 64 bits */
	volatile uint64_t *output = (volatile uint64_t *)(hg_mem + HG_DATA_OFF + HG_OUT_OFF);
	*output = 0xa5a5a5a55a5a5a5aull;
	__asm__ volatile ("dsb sy" ::: "memory");
	bool ok = hg_run(1, "WRITE_VALUE") && *output == 0;
	printf("HEROLTE GPU: WRITE_VALUE output=%#llx -> %s\n", *output,
	    ok ? "THE MALI-T880 RAN A JOB" : "failed or timed out");
	return ok;
}

/*
 * Fragment clear of a size x size RGBA8 target. 0227 sent the A5's
 * single-target descriptor: the T880 answered DATA_INVALID_FAULT (0x58) with
 * the fault pointer on it. Mesa's Midgard v5 layout (genxml/v5.xml), used
 * from the T760 on, is the multi-target one: Framebuffer = Local Storage
 * (+0, 32 B), Parameters (+32, 24 B), Tiler Context (+56, 40 B), Tiler
 * Weights (+96, 32 B), then one 64-byte Render Target (+128). The fragment
 * payload carries the bounds in 16x16 tiles and the framebuffer pointer
 * tagged with Type = 1 (MFBD), render target count - 1 = 0. RT values are
 * the A5 descriptor's (0xb84da201): RGBA8, swizzle 0x688, linear, dither,
 * clean pixel write.
 */
static bool
hg_job_fragment(uint64_t pixels_va, uint32_t size, uint32_t stride, uint32_t color,
    bool panel)
{
	hg_tables(panel);
	uint64_t fb_va = HG_TEST_VA + HG_FBD_OFF;
	uint64_t poly_va = HG_TEST_VA + HG_POLY_OFF;
	uint32_t tiles = (size + 15) / 16;
	volatile uint32_t *hdr = (volatile uint32_t *)(hg_mem + HG_DATA_OFF);
	hdr[4] = 0x00010013;            /* 64-bit | FRAGMENT(9) << 1 | index 1 << 16 */
	hdr[8] = 0;                     /* bound min tile x, y */
	hdr[9] = ((tiles - 1) << 16) | (tiles - 1);     /* bound max tile x, y */
	*(volatile uint64_t *)(hdr + 10) = fb_va | 1;   /* MFBD, 1 render target */

	volatile uint32_t *fb = (volatile uint32_t *)(hg_mem + HG_DATA_OFF + HG_FBD_OFF);
	fb[1] = 31;                     /* Local Storage: no workgroup memory */
	/* Parameters at +32 (word 8). */
	fb[8] = ((size - 1) << 16) | (size - 1);        /* width - 1, height - 1 */
	fb[9] = 0;                                      /* bound min x, y (pixels) */
	fb[10] = ((size - 1) << 16) | (size - 1);       /* bound max x, y */
	fb[11] = (2u << 6) |            /* tie-break MINUS_180_IN_0_OUT, as the A5 */
	    (8u << 9) |                 /* effective tile size log2(16 * 16) */
	    (1u << 24);                 /* color buffer allocation 1 KiB (shr 10) */
	fb[12] = 0;
	fb[13] = 0;                     /* Z clear 0.0 */
	/* Tiler Context at +56 (word 14): empty list, no hierarchy (A5 values). */
	fb[14] = 516;                   /* polygon list size: 512-byte header + 4 */
	fb[15] = 4095;
	*(volatile uint64_t *)(fb + 16) = poly_va;              /* polygon list */
	*(volatile uint64_t *)(fb + 18) = poly_va + 512;        /* body */
	*(volatile uint64_t *)(fb + 20) = poly_va;              /* heap start */
	*(volatile uint64_t *)(fb + 22) = poly_va;              /* heap end */
	/* Render Target at +128 (word 32). */
	volatile uint32_t *rt = fb + 32;
	rt[0] = 1u << 26;               /* internal format R8G8B8A8 */
	rt[1] = 1u |                    /* write enable */
	    (19u << 3) |                /* writeback format R8G8B8A8 */
	    (2u << 10) |                /* block format linear */
	    (1u << 15) |                /* dithering, as the A5 */
	    (0x688u << 16) |            /* identity swizzle */
	    (1u << 31);                 /* clean pixel write */
	*(volatile uint64_t *)(rt + 8) = pixels_va;     /* RGB base */
	rt[10] = stride;                                /* row stride */
	rt[11] = 0;                                     /* surface stride */
	for (unsigned i = 0; i < 4; i++) {
		rt[12 + i] = color;     /* clear color */
	}
	volatile uint32_t *poly = (volatile uint32_t *)(hg_mem + HG_DATA_OFF + HG_POLY_OFF);
	poly[128] = 0xa0000000;         /* empty-list body */
	__asm__ volatile ("dsb sy" ::: "memory");
	return hg_run(0, panel ? "FRAGMENT panel square" : "FRAGMENT private 16x16");
}

static void
hg_jobs(vm_offset_t pmu, int mode)
{
	if (!hg_powered(pmu)) {
		printf("HEROLTE GPU: jobs: domain off\n");
		return;
	}
	if (!hg_gpu) {
		hg_gpu = ml_io_map(HG_GPU_PA, 0x5000);
	}
	if (!hg_mem) {
		hg_mem = ml_io_map(HG_TEST_PA, HG_TEST_BYTES);
	}
	if (!hg_gpu || !hg_mem) {
		printf("HEROLTE GPU: jobs: mapping failed\n");
		return;
	}
	uint32_t as0 = hg_rd(hg_gpu, AS0 + 0x28);
	if (!(hg_rd(hg_gpu, SHADER_PRESENT_LO) & 1) || (as0 & 1)) {
		printf("HEROLTE GPU: jobs: not idle (as0=%#x)\n", as0);
		return;
	}
	if (!hg_cores_on()) {
		printf("HEROLTE GPU: jobs: cores did not come up; nothing submitted\n");
		hg_cores_off();
		return;
	}
	if (!hg_job_write_value() || mode < 4) {
		hg_cores_off();
		return;
	}

	/* Private 16x16 red clear: count the pixels. */
	volatile uint32_t *pix = (volatile uint32_t *)(hg_mem + HG_DATA_OFF + HG_PIX_OFF);
	bool ok = hg_job_fragment(HG_TEST_VA + HG_PIX_OFF, 16, 64, HG_RED, false);
	unsigned red = 0;
	for (unsigned i = 0; i < 256; i++) {
		red += pix[i] == HG_RED;
	}
	printf("HEROLTE GPU: FRAGMENT private: first=%#x last=%#x red=%u/256 -> %s\n",
	    pix[0], pix[255], red, ok && red == 256 ? "THE MALI-T880 RENDERED PIXELS" : "failed");
	if (!ok || red != 256 || mode < 5) {
		hg_cores_off();
		return;
	}

	/* 128x128 green square in the middle of the panel. */
	uint64_t px = HG_FB_PA + (uint64_t)HG_SQ_Y * HG_FB_STRIDE + HG_SQ_X * 4;
	uint64_t va = HG_TEST_VA + (uint64_t)HG_SQ_L3 * 0x1000 + (px & 0xfff);
	ok = hg_job_fragment(va, HG_SQ_SIZE, HG_FB_STRIDE, HG_GREEN, true);
	printf("HEROLTE GPU: FRAGMENT panel square at %u,%u pa=%#llx -> %s\n", HG_SQ_X, HG_SQ_Y,
	    px, ok ? "done: look for a green square in the middle of the screen" : "failed");
	hg_cores_off();
}

static void
hg_thread(void *arg, __unused wait_result_t wr)
{
	int mode = (int)(uintptr_t)arg;
	vm_offset_t pmu = ml_io_map(HG_PMU_PA, 0x8000);
	if (!pmu) {
		printf("HEROLTE GPU: PMU mapping failed\n");
		return;
	}
	/* Once early, once after the UI is up (in case something changed it). */
	hg_msleep(5000);
	hg_probe(pmu, "5s");
	hg_msleep(55000);
	hg_probe(pmu, "60s");
	if (mode >= 2) {
		hg_pmic_dump();
	}
	if (mode >= 3) {
		hg_jobs(pmu, mode);
	}
}

void
herolte_gpu_probe(void)
{
	int enabled = 1;
	thread_t thread;

	PE_parse_boot_argn("herogpu", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("HEROLTE GPU: probe disabled\n");
		return;
	}
	if (kernel_thread_start(hg_thread, (void *)(uintptr_t)enabled, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
#endif /* ARM64_BOARD_CONFIG_HEROLTE */
