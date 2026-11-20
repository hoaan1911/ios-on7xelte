/*
 * Exynos7580 Mali-T720 resource service for Samsung a5xelte.
 *
 * Stage 1 registers a resource-only IOKit service (a5gpu=1). Stage 2 can
 * passively probe the PMU/clock gates and, only if they are already on,
 * read GPU_ID (a5gpu=2). It does not write registers or claim acceleration.
 *
 * The Exynos7580 G3D domain needs an ordered BUCK4/bus/PMU/CMU sequence.
 * Reading GPU_ID while power-gated can abort. Active power-up and job
 * submission require a separately verified BUCK4/bus/PMU/CMU sequence.
 */
#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <vm/memory_types.h>
#include <arm/memory_types.h>

/* The IOKit exported machine header omits these kernel-private mappings. */
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size,
    unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
/* Timebase helpers: linkage specs are only legal at file scope. */
extern "C" uint64_t mach_absolute_time(void);
extern "C" void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *nsecs);
extern "C" void nanoseconds_to_absolutetime(uint64_t nsecs, uint64_t *abstime);

/*
 * Where a frame's time goes, counted instead of guessed.
 *
 * The 2026-11-13 log presents 1-2 frames per 5.3 s while one present costs 13.7 ms, so the copy is
 * ~0.3 % of the time and the rest is in this driver or above it.  The two cache passes are over
 * every mapped BO (j7m_cache() at submit and again after the wait) and both walk the whole set for
 * each chain, so they are the first suspects; these counters settle it.  The framebuffer's perf
 * line prints and clears them once per window.
 */
static uint64_t j7mStatSubmits, j7mStatWaits, j7mStatWaitUs, j7mStatStoreUs, j7mStatFlushUs;

static inline uint64_t
j7m_now_us(void)
{
    uint64_t freq, count;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(count));
    if (freq == 0) {
        return 0;
    }
    return count / (freq / 1000000u);
}

/* Real core clock. The tree has no cpufreq driver, so the CPU runs at whatever
 * rate the bootloader left behind and nothing in the system reports it.
 *
 * The first version timed a loop against cntfrq_el0/cntvct_el0 and printed
 * cpu_mhz=0 for every window, because this firmware leaves cntfrq_el0 at zero.
 * mach_absolute_time() does work here - every other number in the perf line
 * comes from it - so measure against that instead, and count cycles with a
 * dependent add chain, which retires one add per cycle on Cortex-A53.
 *
 * Loop overhead is not guessed away: the same chain is timed with an 8-add and
 * a 16-add body, so with T = n*a + (n/u)*b for u adds per iteration the
 * per-add cost a falls out exactly:
 *     a = T * (2/n16 - 1/n8),  freq = 1/a
 * Diagnostic only: no MMIO, no privileged register writes, no PMU. The result
 * is cached after the first successful measurement.
 */
extern "C" unsigned
j7m_cpu_mhz(void)
{
    static unsigned cached;

    if (cached) {
        return cached;
    }

    uint64_t window, t0, t1, n8 = 0, n16 = 0, ns = 0;
    nanoseconds_to_absolutetime(2000000ULL, &window);

    t0 = mach_absolute_time();
    do {
        uint64_t acc = 0, i = 2048;
        __asm__ __volatile__(
            "1:\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "subs %1, %1, #1\n"
            "b.ne 1b\n"
            : "+r"(acc), "+r"(i) :: "cc");
        n8 += 2048ULL * 8;
        t1 = mach_absolute_time();
    } while (t1 - t0 < window);

    t0 = mach_absolute_time();
    do {
        uint64_t acc = 0, i = 2048;
        __asm__ __volatile__(
            "1:\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "subs %1, %1, #1\n"
            "b.ne 1b\n"
            : "+r"(acc), "+r"(i) :: "cc");
        n16 += 2048ULL * 16;
        t1 = mach_absolute_time();
    } while (t1 - t0 < window);

    absolutetime_to_nanoseconds(t1 - t0, &ns);
    if (ns == 0 || n8 == 0 || n16 == 0) {
        return 0;
    }

    /* per-add nanoseconds = ns * (2/n16 - 1/n8) */
    {
        uint64_t delta = (2ULL * ns * 1000ULL) / n16 - (ns * 1000ULL) / n8;
        if (delta == 0) {
            return 0;
        }
        cached = (unsigned)(1000000ULL / delta);
    }
    return cached;
}

extern "C" void
j7m_take_stats(uint64_t *submits, uint64_t *waits, uint64_t *waitUs, uint64_t *storeUs,
    uint64_t *flushUs)
{
    *submits = j7mStatSubmits;
    *waits = j7mStatWaits;
    *waitUs = j7mStatWaitUs;
    *storeUs = j7mStatStoreUs;
    *flushUs = j7mStatFlushUs;
    j7mStatSubmits = j7mStatWaits = j7mStatWaitUs = j7mStatStoreUs = j7mStatFlushUs = 0;
}

/* Passive probe of the already-powered GPU only. All offsets are from the
 * native Exynos7580 Linux G3D clock/PMU driver. A powered-off GPU register
 * access can abort XNU, so the PMU and both CMU gate banks are checked first.
 */
static uint32_t
a5g3d_read32(vm_offset_t base, vm_offset_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void
a5g3d_write32(vm_offset_t base, vm_offset_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

/* Panfrost's Mali-LPAE format uses four 4-KiB levels even for a low IOVA.
 * Keep the test's descriptor and two output pixels in a single mapped page.
 * No identity mapping or access to the display buffer is provided to Mali.
 */
static const uint64_t kA5G3DTestVA = 0x02000000ULL;
static const uint32_t kA5G3DTestBytes = 0x10000;
static const uint32_t kA5G3DDataOffset = 0x4000;
static const uint32_t kA5G3DOutputOffset = 0x100;

struct A5G3DWriteValueJob {
    uint32_t exceptionStatus;
    uint32_t firstIncompleteTask;
    uint64_t faultPointer;
    uint8_t typeAndSize;      /* 64-bit descriptor: 1 | (WRITE_VALUE=2)<<1 */
    uint8_t flags;
    uint16_t index;
    uint16_t dependency1;
    uint16_t dependency2;
    uint64_t next;
    uint64_t outputVA;
    uint64_t valueType;        /* Midgard WRITE_VALUE zeroes 64 bits for 3 */
} __attribute__((packed));
static_assert(sizeof(A5G3DWriteValueJob) == 48, "Mali job descriptor size");

static bool
a5g3d_try_write_value(IOService *service, vm_offset_t gpu,
    IOBufferMemoryDescriptor *&storage)
{
    /* Linux Panfrost's JS1 is the vertex/tiler queue, including WRITE_VALUE.
     * Never disturb a running job or an active address space.
     */
    const uint32_t shader = a5g3d_read32(gpu, 0x140);
    const uint32_t l2 = a5g3d_read32(gpu, 0x160);
    const uint32_t addressSpaces = a5g3d_read32(gpu, 0x018);
    const uint32_t jobSlots = a5g3d_read32(gpu, 0x01c);
    const uint32_t pending = a5g3d_read32(gpu, 0x1880 + 0x60);
    const uint32_t as0Status = a5g3d_read32(gpu, 0x2400 + 0x28);
    const uint32_t as0Transtab = a5g3d_read32(gpu, 0x2400);
    IOLog("A5XELTE G3D: job gate shader=%08x l2=%08x as=%08x js=%08x pending=%08x as0=%08x ttbr=%08x\n",
        shader, l2, addressSpaces, jobSlots, pending, as0Status, as0Transtab);
    if (!shader || !l2 || !(addressSpaces & 1) || !(jobSlots & 2) ||
        pending || (as0Status & 1) || as0Transtab) {
        service->setProperty("A5G3DJobResult", "not-ready");
        return false;
    }

    storage = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        kA5G3DTestBytes, kA5G3DTestBytes);
    if (!storage || !storage->getBytesNoCopy()) {
        service->setProperty("A5G3DJobResult", "allocation-failed");
        return false;
    }
    IOByteCount segmentLength = 0;
    const uint64_t physical = storage->getPhysicalSegment(0, &segmentLength,
        kIOMemoryMapperNone);
    if (!physical || segmentLength < kA5G3DTestBytes ||
        (physical & 0xfff) != 0) {
        service->setProperty("A5G3DJobResult", "physical-segment-invalid");
        return false;
    }
    uint8_t *bytes = static_cast<uint8_t *>(storage->getBytesNoCopy());
    bzero(bytes, kA5G3DTestBytes);
    uint64_t *l0 = reinterpret_cast<uint64_t *>(bytes);
    uint64_t *l1 = reinterpret_cast<uint64_t *>(bytes + 0x1000);
    uint64_t *l2table = reinterpret_cast<uint64_t *>(bytes + 0x2000);
    uint64_t *l3 = reinterpret_cast<uint64_t *>(bytes + 0x3000);
    l0[0] = (physical + 0x1000) | 3;
    l1[0] = (physical + 0x2000) | 3;
    l2table[(kA5G3DTestVA >> 21) & 0x1ff] = (physical + 0x3000) | 3;
    for (unsigned i = 0; i < 4; ++i) {
        /* Mali-LPAE: read/write, outer-shareable, implementation-defined
         * noncached attribute, level-3 leaf type 1 (Linux io-pgtable-arm).
         */
        l3[i] = (physical + kA5G3DDataOffset + i * 0x1000) | 0x2c1;
    }
    A5G3DWriteValueJob *job = reinterpret_cast<A5G3DWriteValueJob *>(
        bytes + kA5G3DDataOffset);
    job->typeAndSize = 1 | (2 << 1);
    job->index = 1;
    job->outputVA = kA5G3DTestVA + kA5G3DOutputOffset;
    job->valueType = 3;
    uint64_t *output = reinterpret_cast<uint64_t *>(
        bytes + kA5G3DDataOffset + kA5G3DOutputOffset);
    *output = 0xa5a5a5a55a5a5a5aULL;
    if (storage->performOperation(kIOMemoryIncoherentIOStore, 0,
        kA5G3DTestBytes) != kIOReturnSuccess) {
        service->setProperty("A5G3DJobResult", "cache-store-failed");
        return false;
    }
    __sync_synchronize();

    /* Program the otherwise unused AS0 with only the private 16-KiB test
     * region, then submit exactly one WRITE_VALUE job to JS1. Poll with a
     * finite deadline. The allocation stays owned by the service on timeout.
     */
    a5g3d_write32(gpu, 0x2004, 0x00010001);
    a5g3d_write32(gpu, 0x1004, 0x00020002);
    const uint64_t transtab = physical | 0x7;
    a5g3d_write32(gpu, 0x2400, static_cast<uint32_t>(transtab));
    a5g3d_write32(gpu, 0x2404, static_cast<uint32_t>(transtab >> 32));
    a5g3d_write32(gpu, 0x2408, 0x888d88);
    a5g3d_write32(gpu, 0x240c, 0);
    a5g3d_write32(gpu, 0x2418, 1); /* AS_COMMAND_UPDATE */
    for (unsigned i = 0; i < 10000 && (a5g3d_read32(gpu, 0x2428) & 1); ++i)
        IODelay(10);
    if (a5g3d_read32(gpu, 0x2428) & 1) {
        service->setProperty("A5G3DJobResult", "mmu-update-timeout");
        return false;
    }
    a5g3d_write32(gpu, 0x18c0, static_cast<uint32_t>(kA5G3DTestVA));
    a5g3d_write32(gpu, 0x18c4, 0);
    a5g3d_write32(gpu, 0x18d0, shader);
    a5g3d_write32(gpu, 0x18d4, 0);
    a5g3d_write32(gpu, 0x18d8, (8 << 16) | (3 << 8) | (3 << 12));
    __sync_synchronize();
    a5g3d_write32(gpu, 0x18e0, 1); /* JS_COMMAND_NEXT(1) = START */

    uint32_t jobIRQ = 0, mmuIRQ = 0, gpuIRQ = 0;
    for (unsigned i = 0; i < 20000; ++i) {
        jobIRQ = a5g3d_read32(gpu, 0x1000);
        mmuIRQ = a5g3d_read32(gpu, 0x2000);
        gpuIRQ = a5g3d_read32(gpu, 0x20);
        if ((jobIRQ & (1u << 1)) || (jobIRQ & (1u << 17)) ||
            mmuIRQ || (gpuIRQ & 0x81))
            break;
        IODelay(10);
    }
    if (storage->performOperation(kIOMemoryIncoherentIOFlush,
        kA5G3DDataOffset + kA5G3DOutputOffset,
        sizeof(*output)) != kIOReturnSuccess) {
        service->setProperty("A5G3DJobResult", "cache-flush-failed");
        return false;
    }
    IOLog("A5XELTE G3D: write-value job irq=%08x mmu=%08x gpu=%08x js=%08x asfault=%08x output=%016llx\n",
        jobIRQ, mmuIRQ, gpuIRQ, a5g3d_read32(gpu, 0x18a4),
        a5g3d_read32(gpu, 0x241c), *output);
    const bool complete = (jobIRQ & (1u << 1)) && !mmuIRQ &&
        !(jobIRQ & (1u << 17)) && !(gpuIRQ & 0x81) && *output == 0;
    service->setProperty("A5G3DJobResult", complete ? "zero-written" : "failed-or-timeout");
    if (jobIRQ)
        a5g3d_write32(gpu, 0x1004, jobIRQ);
    if (mmuIRQ)
        a5g3d_write32(gpu, 0x2004, mmuIRQ);
    return complete;
}

static void
a5g3d_probe_existing_power(IOService *service, bool runJob,
    IOBufferMemoryDescriptor *&storage)
{
    vm_offset_t pmu = ml_io_map_unmappable(0x10464000, 0x1000, VM_WIMG_IO);
    if (!pmu) {
        service->setProperty("A5G3DProbeResult", "pmu-map-failed");
        return;
    }
    uint32_t power = a5g3d_read32(pmu, 0x64);
    service->setProperty("A5G3DPMUStatus", static_cast<uint64_t>(power), 32);
    if ((power & 0xf) != 0xf) {
        service->setProperty("A5G3DProbeResult", "power-off");
        IOLog("A5XELTE G3D: PMU off (%08x); GPU aperture untouched\n", power);
        ml_io_unmap(pmu, 0x1000);
        return;
    }

    vm_offset_t cmu = ml_io_map_unmappable(0x11460000, 0x1000, VM_WIMG_IO);
    if (!cmu) {
        service->setProperty("A5G3DProbeResult", "cmu-map-failed");
        ml_io_unmap(pmu, 0x1000);
        return;
    }
    uint32_t gate0 = a5g3d_read32(cmu, 0x800);
    uint32_t gate1 = a5g3d_read32(cmu, 0x900);
    service->setProperty("A5G3DGate0", static_cast<uint64_t>(gate0), 32);
    service->setProperty("A5G3DGate1", static_cast<uint64_t>(gate1), 32);
    /* The native Linux clk enable path sets bits 0, 1 and 4 in both banks. */
    if ((gate0 & 0x13) != 0x13 || (gate1 & 0x13) != 0x13 ||
        (a5g3d_read32(pmu, 0x64) & 0xf) != 0xf) {
        service->setProperty("A5G3DProbeResult", "clock-gated");
        IOLog("A5XELTE G3D: clock gate off (%08x/%08x); GPU aperture untouched\n",
            gate0, gate1);
        ml_io_unmap(cmu, 0x1000);
        ml_io_unmap(pmu, 0x1000);
        return;
    }

    const vm_size_t gpuSize = runJob ? 0x3000 : 0x1000;
    vm_offset_t gpu = ml_io_map_unmappable(0x11400000, gpuSize, VM_WIMG_IO);
    if (!gpu) {
        service->setProperty("A5G3DProbeResult", "gpu-map-failed");
        ml_io_unmap(cmu, 0x1000);
        ml_io_unmap(pmu, 0x1000);
        return;
    }
    /* GPU_ID at +0x0, decoded by Panfrost as product bits 31:16. */
    uint32_t id = a5g3d_read32(gpu, 0);
    uint32_t product = id >> 16;
    service->setProperty("A5G3DGPUId", static_cast<uint64_t>(id), 32);
    service->setProperty("A5G3DProduct", static_cast<uint64_t>(product), 32);
    service->setProperty("A5G3DProbeResult", product == 0x720 ? "mali-t720" : "unexpected-id");
    IOLog("A5XELTE G3D: passive GPU_ID=%08x product=%04x (expected 0720); %s\n",
        id, product, runJob ? "offscreen test requested" : "no GPU jobs");
    if (runJob && product == 0x720)
        a5g3d_try_write_value(service, gpu, storage);
    ml_io_unmap(gpu, gpuSize);
    ml_io_unmap(cmu, 0x1000);
    ml_io_unmap(pmu, 0x1000);
}

/*
 * Samsung Galaxy J7 Prime (Exynos7870, Mali-T830), selected by boot-arg
 * j7gpu=1|2|3.  Addresses and SPI numbers are from the mainline exynos7870.dtsi
 * and the phone's own /proc/iomem and /proc/interrupts (GIC 313/314/315 are
 * SPI 281 GPU, 282 JOB, 283 MMU).
 *
 *   j7gpu=1  resource-only service (publishes the numbers, touches nothing)
 *   j7gpu=2  also reads the PMU G3D status; if the domain is on, logs the
 *            non-zero registers of CMU_G3D.  The GPU aperture is never read.
 *   j7gpu=3  as 2, then reads GPU_ID once if the PMU says the domain is on.
 *            Reading an unclocked GPU can stall the bus; if the phone freezes,
 *            hold Power for ten seconds.  Use 2 first.
 *
 * Nothing is written to any register and no job is submitted.  This is a
 * diagnostic stage, not acceleration.
 */
#define J7G3D_PMU_PAGE   0x10484000ULL   /* PMU base 0x10480000 + 0x4000 */
#define J7G3D_CMU_BASE   0x11460000ULL
#define J7G3D_GPU_BASE   0x11400000ULL

static void
j7g3d_publish(IOService *service, int level)
{
    service->setProperty("J7G3DMMIOBase", J7G3D_GPU_BASE, 64);
    service->setProperty("J7G3DMMIOSize", 0x5000ULL, 64);
    service->setProperty("J7G3DCMUBase", J7G3D_CMU_BASE, 64);
    service->setProperty("J7G3DPMUBase", 0x10480000ULL, 64);
    service->setProperty("J7G3DJobSPI", 282ULL, 32);
    service->setProperty("J7G3DMMUSPI", 283ULL, 32);
    service->setProperty("J7G3DGpuSPI", 281ULL, 32);
    service->setProperty("J7G3DDriverStage", level >= 7 ? "user-client-service" : level == 6 ? "job-manager-selftest" : level == 5 ? "write-value-job" : level == 4 ? "core-power-test" : level == 3 ? "gpu-id-probe" :
        (level == 2 ? "pmu-cmu-probe" : "resources-only"));
}

/* ===================================================================================
 * Mali-T830 job manager core (j7gpu=6 selftest now; the user client builds on this).
 *
 * One address space (AS0), four-level Mali-LPAE page tables carved from a physically
 * contiguous pool, buffer objects mapped at kernel-chosen GPU VAs below 2 GiB, jobs
 * submitted by GPU VA of the job chain head, completion polled from JOB_IRQ_RAWSTAT.
 * Register layout and descriptor formats are the ones the A5 test already proved on
 * this GPU family (write-value job, 0x2c1 leaves, memattr 0x888d88).
 * =================================================================================== */
/* 64 was the selftest's number.  A user-space client maps a BO per IOSurface and CoreAnimation
 * holds far more than that: minutes into a boot every CREATE_BO came back ENOMEM, Mesa got a
 * NULL BO and died in submit_batch.  The table is scanned linearly, so this is cheap. */
#define J7M_MAX_BOS     512
#define J7M_VA_LO       0x01000000ULL
#define J7M_VA_HI       0x80000000ULL   /* MMU_FEATURES 0x2821: 33 VA bits; stay well below */
#define J7M_PT_BYTES    (64u * 1024 * 1024)  /* pool ceiling, 16384 tables */
#define J7M_PT_MIN      (4u * 1024 * 1024)   /* never below what the earlier boots used */
#define J7M_PT_TABLES   (J7M_PT_BYTES / 0x1000u)
#define J7M_PAGE        0x1000ULL        /* the GPU's page size, whatever the CPU's is */

#define J7M_JOB_IRQ_RAWSTAT 0x1000
#define J7M_JOB_IRQ_CLEAR   0x1004
#define J7M_MMU_IRQ_RAWSTAT 0x2000
#define J7M_MMU_IRQ_CLEAR   0x2004
#define J7M_AS0             0x2400
#define J7M_JS(n)           (0x1800 + 0x80 * (n))

struct J7MaliBo {
    IOBufferMemoryDescriptor *md;   /* kernel-allocated BO, or NULL */
    IOMemoryDescriptor *umd;        /* wired client memory, or NULL */
    void *owner;                    /* the J7MaliUserClient that mapped umd */
    uint64_t va;
    uint64_t start;                 /* va + the client's offset into the first page */
    uint64_t size;
    bool used;
    uint64_t *pas;                  /* one PA per page, cached at map time: the submit check never
                                     * has to ask the memory descriptor again */
};

/* A short history of BO lifetimes, printed with the next fault.  A translation fault inside a
 * mapped BO means the GPU's walk found nothing where this driver wrote a leaf; whether that range
 * was just mapped, freed or re-used tells a lost mapping apart from a range that was never ours. */
#define J7M_EVENTS      16

struct J7MEvent {
    uint64_t va;
    uint64_t size;
    uint32_t slot;
    char kind;                      /* 'k' kernel BO, 'm' client map, 'f' free */
};

static struct {
    vm_offset_t gpu;
    IOBufferMemoryDescriptor *pool;
    uint8_t *poolVA;
    uint64_t poolPA;
    uint32_t poolTables;
    uint32_t poolUsed;
    uint64_t root;
    uint32_t shader;
    bool mmuLive;
    bool mmuReady;        /* AS0 programmed after serve_start: protects the first client submit */
    uint32_t seq;
    /* Bumped by every leaf/entry this driver writes or clears: the whole-AS verify only has to run
     * again when one of these moved (or every so often, as a safety net). */
    uint32_t mapGen;
    uint32_t verifyGen;
    bool inflight;
    uint32_t inflightSlot;
    struct J7MaliBo bos[J7M_MAX_BOS];
    struct J7MEvent events[J7M_EVENTS];
    uint32_t eventHead;
} gJ7M;

static uint64_t *
j7m_table(uint64_t pa)
{
    return reinterpret_cast<uint64_t *>(gJ7M.poolVA + (pa - gJ7M.poolPA));
}

static void
j7m_note(char kind, unsigned slot, uint64_t va, uint64_t size)
{
    struct J7MEvent *e = &gJ7M.events[gJ7M.eventHead % J7M_EVENTS];
    gJ7M.eventHead++;
    e->kind = kind;
    e->slot = slot;
    e->va = va;
    e->size = size;
}

/* Tables whose whole 2 MiB window went away.  Handed out again before the pool grows, so a session
 * that keeps mapping and freeing surfaces stays inside the pool. */
static uint64_t gJ7MPoolFree[J7M_PT_TABLES];
static uint32_t gJ7MPoolFreeCount;

/* PA of a new zeroed 4 KiB table, or 0. */
static uint64_t
j7m_table_new(void)
{
    uint64_t pa;

    if (gJ7MPoolFreeCount) {
        pa = gJ7MPoolFree[--gJ7MPoolFreeCount];
    } else {
        if (gJ7M.poolUsed >= gJ7M.poolTables) {
            static bool said;
            if (!said) {
                said = true;
                IOLog("J7 G3D: page-table pool exhausted (%u of %u tables): further maps refused\n",
                    gJ7M.poolUsed, gJ7M.poolTables);
            }
            return 0;
        }
        pa = gJ7M.poolPA + (uint64_t)gJ7M.poolUsed * J7M_PAGE;
        gJ7M.poolUsed++;
    }
    bzero(j7m_table(pa), J7M_PAGE);
    return pa;
}

static bool
j7m_pool_init(void)
{
    bzero(&gJ7M.bos, sizeof(gJ7M.bos));
    gJ7M.poolUsed = 0;
    gJ7M.mapGen = 0;
    gJ7M.verifyGen = 0xffffffffu;    /* differs from mapGen: the first submit verifies */
    gJ7MPoolFreeCount = 0;
    /* 2 MiB (512 tables) was sized for the selftest.  A compositor session maps a surface per glyph
     * and the pool never handed a table back, so minutes in every map was refused ("512 of 512 page
     * tables"); each refused map left a job slot faulting and the frame died on a five second wait.
     * Ask for 16 MiB and halve until a physically contiguous block is actually there. */
    for (uint32_t bytes = J7M_PT_BYTES; bytes >= J7M_PT_MIN; bytes >>= 1) {
        gJ7M.pool = IOBufferMemoryDescriptor::withOptions(
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, bytes, J7M_PAGE);
        if (!gJ7M.pool || !gJ7M.pool->getBytesNoCopy()) {
            if (gJ7M.pool) {
                gJ7M.pool->release();
                gJ7M.pool = NULL;
            }
            continue;
        }
        IOByteCount seg = 0;
        gJ7M.poolPA = gJ7M.pool->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!gJ7M.poolPA || seg < bytes || (gJ7M.poolPA & (J7M_PAGE - 1))) {
            gJ7M.pool->release();
            gJ7M.pool = NULL;
            gJ7M.poolPA = 0;
            continue;
        }
        gJ7M.poolVA = static_cast<uint8_t *>(gJ7M.pool->getBytesNoCopy());
        gJ7M.poolTables = bytes / static_cast<uint32_t>(J7M_PAGE);
        bzero(gJ7M.poolVA, bytes);
        break;
    }
    if (!gJ7M.pool || !gJ7M.poolVA || !gJ7M.poolTables) {
        return false;
    }
    gJ7M.root = j7m_table_new();
    return gJ7M.root != 0;
}

/* Push table writes to memory before the GPU's walker can read them. */
static void
j7m_pool_sync(void)
{
    /* Only the tables that exist: a whole-pool store on every map and every submit would cost more
     * than the tables it is protecting now that the pool can be 16 MiB. */
    const vm_size_t bytes = static_cast<vm_size_t>(gJ7M.poolUsed) * static_cast<vm_size_t>(J7M_PAGE);
    if (bytes) {
        gJ7M.pool->performOperation(kIOMemoryIncoherentIOStore, 0, bytes);
    }
    __sync_synchronize();
}

/* Is pa a page inside the page-table pool?  Every walk this driver does, and every walk the
 * repair path does, is bounds-checked against this: a stale entry must never be followed. */
static bool
j7m_in_pool(uint64_t pa)
{
    if (!gJ7M.poolVA || !gJ7M.poolPA || !gJ7M.poolTables) {
        return false;
    }
    const uint64_t end = gJ7M.poolPA + (uint64_t)gJ7M.poolTables * J7M_PAGE;
    return pa >= gJ7M.poolPA && pa <= end - J7M_PAGE && (pa & (J7M_PAGE - 1)) == 0;
}

/* replace=false is the normal map (an existing leaf is an allocator bug); replace=true is the
 * fault repair, which rewrites the leaf and replaces an intermediate entry that does not point
 * into the pool. */
static bool
j7m_map_page_at(uint64_t va, uint64_t pa, bool replace)
{
    uint64_t table = gJ7M.root;
    const unsigned shifts[3] = { 39, 30, 21 };
    for (unsigned level = 0; level < 3; level++) {
        uint64_t *t = j7m_table(table);
        uint64_t &e = t[(va >> shifts[level]) & 0x1ff];
        if (!(e & 3) || !j7m_in_pool(e & 0x0000fffffffff000ULL)) {
            if ((e & 3) && !replace) {
                return false;
            }
            uint64_t next = j7m_table_new();
            if (!next) {
                return false;
            }
            e = next | 3;
        }
        table = e & 0x0000fffffffff000ULL;
    }
    uint64_t &leaf = j7m_table(table)[(va >> 12) & 0x1ff];
    if ((leaf & 3) && !replace) {
        return false;                /* already mapped: allocator bug */
    }
    /* Mali-LPAE leaf: read/write, outer-shareable, attribute index 0, type 1 (as the A5 test). */
    leaf = pa | 0x2c1;
    gJ7M.mapGen++;
    return true;
}

static bool
j7m_map_page(uint64_t va, uint64_t pa)
{
    return j7m_map_page_at(va, pa, false);
}

/* A cleared mapping that a live BO still owns is this driver unmapping memory the GPU is about to
 * read: the fault-repair loop has been papering over exactly that, one 5 s slot timeout at a time.
 * Name the BO and the caller once, so the next log says who does it. */
#define J7M_AUDIT_LOG   16              /* one line per hit, then quiet */

static const struct J7MaliBo *gJ7MUnmapping;   /* BO being torn down: not an audit hit */

static void
j7m_audit_cleared(uint64_t va, uint64_t len, const char *what)
{
    static unsigned shown;

    if (shown >= J7M_AUDIT_LOG) {
        return;
    }
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        const struct J7MaliBo *bo = &gJ7M.bos[i];
        if (!bo->used || bo == gJ7MUnmapping || va + len <= bo->va || va >= bo->va + bo->size) {
            continue;
        }
        shown++;
        IOLog("J7 G3D: audit: %s cleared %llx+%llx but live bo=%u owns %llx+%llx\n", what,
            (unsigned long long)va, (unsigned long long)len, i + 1,
            (unsigned long long)bo->va, (unsigned long long)bo->size);
        return;
    }
}

/* Give an L3 table back when its whole 2 MiB window is empty: clear the entry that points at it and
 * push the page on the free list.  A window still shared with another BO keeps its table. */
static void
j7m_release_l3(uint64_t va, uint64_t l3)
{
    if (!j7m_in_pool(l3) || gJ7MPoolFreeCount >= J7M_PT_TABLES) {
        return;
    }
    const uint64_t *leaves = j7m_table(l3);
    for (unsigned i = 0; i < 512; i++) {
        if (leaves[i] & 3) {
            return;
        }
    }
    uint64_t parent = gJ7M.root;
    const unsigned shifts[3] = { 39, 30, 21 };
    for (unsigned level = 0; level < 2; level++) {
        const uint64_t e = j7m_table(parent)[(va >> shifts[level]) & 0x1ff];
        if (!(e & 3) || !j7m_in_pool(e & 0x0000fffffffff000ULL)) {
            return;
        }
        parent = e & 0x0000fffffffff000ULL;
    }
    uint64_t &slot = j7m_table(parent)[(va >> 21) & 0x1ff];
    if ((slot & 0x0000fffffffff000ULL) != l3) {
        return;
    }
    j7m_audit_cleared(va & ~((1ULL << 21) - 1), 1ULL << 21, "release-l3");
    slot = 0;
    gJ7MPoolFree[gJ7MPoolFreeCount++] = l3;
}

static void
j7m_unmap_page(uint64_t va)
{
    uint64_t table = gJ7M.root;
    const unsigned shifts[3] = { 39, 30, 21 };
    for (unsigned level = 0; level < 3; level++) {
        /* This walk used to trust every entry: a slot holding anything but a pool table (a leaf, or
         * the junk that also makes leaves disappear) turned into j7m_table(garbage) and the CPU took
         * a data abort inside the map path - the panic of 2026-11-10 (pc j7m_unmap_page+0x58,
         * far 0x00000d000b2a1000).  Stop at the first entry that is not a pool table. */
        if (!j7m_in_pool(table)) {
            return;
        }
        const uint64_t e = j7m_table(table)[(va >> shifts[level]) & 0x1ff];
        if (!(e & 3)) {
            return;
        }
        if (!j7m_in_pool(e & 0x0000fffffffff000ULL)) {
            static unsigned shown;
            if (shown++ < J7M_AUDIT_LOG) {
                IOLog("J7 G3D: unmap walk: level %u entry %016llx is not a pool table (va %llx)\n",
                    level, (unsigned long long)e, (unsigned long long)va);
            }
            return;
        }
        table = e & 0x0000fffffffff000ULL;
    }
    j7m_audit_cleared(va & ~(J7M_PAGE - 1), J7M_PAGE, "unmap");
    j7m_table(table)[(va >> 12) & 0x1ff] = 0;
    gJ7M.mapGen++;
    j7m_release_l3(va, table);
}

/* Walk our own tables exactly the way the GPU's walker does and return the page's PA (0 turned
 * into false).  The fault log reads this instead of a hand-computed index chain. */
static bool
j7m_lookup(uint64_t va, uint64_t *pa)
{
    uint64_t table = gJ7M.root;
    const unsigned shifts[3] = { 39, 30, 21 };
    for (unsigned level = 0; level < 3; level++) {
        if (!j7m_in_pool(table)) {
            return false;
        }
        const uint64_t e = j7m_table(table)[(va >> shifts[level]) & 0x1ff];
        if (!(e & 1)) {
            return false;
        }
        table = e & 0x0000fffffffff000ULL;
    }
    if (!j7m_in_pool(table)) {
        return false;
    }
    const uint64_t leaf = j7m_table(table)[(va >> 12) & 0x1ff];
    if (!(leaf & 1)) {
        return false;
    }
    if (pa) {
        *pa = leaf & 0x0000fffffffff000ULL;
    }
    return true;
}

/* Physical page for a page of an already mapped BO (kernel BO or wired client memory). */
static bool
j7m_bo_page(struct J7MaliBo *bo, uint64_t page, uint64_t *pa)
{
    if (page < bo->va || page >= bo->va + bo->size) {
        return false;
    }
    if (bo->pas) {
        const uint64_t p = bo->pas[(page - bo->va) / J7M_PAGE];
        if (!p || (p & (J7M_PAGE - 1))) {
            return false;
        }
        *pa = p;
        return true;
    }
    IOMemoryDescriptor *md = bo->md ? static_cast<IOMemoryDescriptor *>(bo->md) : bo->umd;
    if (!md) {
        return false;
    }
    IOByteCount seg = 0;
    const uint64_t p = md->getPhysicalSegment(page - bo->va, &seg, kIOMemoryMapperNone);
    if (!p || (p & (J7M_PAGE - 1)) || seg < J7M_PAGE) {
        return false;
    }
    *pa = p;
    return true;
}

static bool
j7m_mmu_wait_idle(void)
{
    for (unsigned i = 0; i < 10000; i++) {
        if (!(a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x28) & 1)) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

static bool
j7m_mmu_cmd(uint32_t cmd)
{
    if (!j7m_mmu_wait_idle()) {
        return false;
    }
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x18, cmd);
    return j7m_mmu_wait_idle();
}

/* Program AS0 with the page-table root (also valid after a GPU reset). */
static bool
j7m_mmu_enable(void)
{
    a5g3d_write32(gJ7M.gpu, J7M_MMU_IRQ_CLEAR, 0xffffffffu);
    a5g3d_write32(gJ7M.gpu, J7M_JOB_IRQ_CLEAR, 0xffffffffu);
    const uint64_t transtab = gJ7M.root | 0x7;
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x00, static_cast<uint32_t>(transtab));
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x04, static_cast<uint32_t>(transtab >> 32));
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x08, 0x888d88);
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x0c, 0);
    gJ7M.mmuLive = j7m_mmu_cmd(1);   /* AS_COMMAND_UPDATE */
    return gJ7M.mmuLive;
}

/* Lock + flush the page-table caches for [va, va+len) (Panfrost's sequence). */
static bool
j7m_mmu_flush(uint64_t va, uint64_t len)
{
    if (!gJ7M.mmuLive) {
        return true;            /* AS0 not programmed (or reset): nothing cached to flush */
    }
    uint64_t size = J7M_PAGE;
    while (size < len || (va & ~(size - 1)) != ((va + len - 1) & ~(size - 1))) {
        size <<= 1;
    }
    unsigned width = 0;
    while ((1ULL << (width + 1)) < size) {
        width++;
    }
    /* region_width = log2(size) - 1 */
    const uint64_t lockaddr = (va & ~(size - 1)) | width;
    if (!j7m_mmu_wait_idle()) {
        return false;
    }
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x10, static_cast<uint32_t>(lockaddr));
    a5g3d_write32(gJ7M.gpu, J7M_AS0 + 0x14, static_cast<uint32_t>(lockaddr >> 32));
    /* LOCK, FLUSH_PT, UNLOCK (Panfrost's locked flush).  The unlock used to be missing, which
     * left the AS locked on the flushed range after every map. */
    return j7m_mmu_cmd(2) && j7m_mmu_cmd(4) && j7m_mmu_cmd(3);
}

/*
 * 0327 proved the holes are real and that filling them makes the GPU draw, but it only checked a
 * 4096-page slice per submit: a BO whose leaves were gone near the end of the walk still faulted,
 * the job slot lost that chain, and the frame died on a five second wait timeout - hence a display
 * that updates every few minutes.  Two things change here.
 *
 * Every page of every live BO is now checked before the chain reaches the job manager, and the
 * check compares against a PA list cached at map time instead of calling the memory descriptor
 * again, so a full pass is a table walk (no locking, no descriptor walk) and one flush at the end
 * when something actually moved.
 *
 * The counters stay: a BO that still loses leaves between map and submit prints one line naming
 * it, and a clean pass prints a heartbeat line every 240 submits, so the next log says whether the
 * map path, a free, or something else keeps unmasking live pages.
 */
#define J7M_VERIFY_LOG      16          /* per-BO hole lines before the channel goes quiet */

/* Lock held.  Rewrite every leaf of one BO that does not read back as this BO's own page.
 * Returns the number of leaves written; *firstHole is the VA of the first one. */
static unsigned
j7m_bo_verify(struct J7MaliBo *bo, unsigned *checked, unsigned *unknown, uint64_t *firstHole)
{
    unsigned filled = 0;

    if (!bo->used || bo->size < J7M_PAGE) {
        return 0;
    }
    const uint64_t pages = bo->size / J7M_PAGE;
    for (uint64_t i = 0; i < pages; i++) {
        const uint64_t at = bo->va + i * J7M_PAGE;
        uint64_t pa = 0, have = 0;
        if (!j7m_bo_page(bo, at, &pa)) {
            (*unknown)++;
            continue;
        }
        (*checked)++;
        if (j7m_lookup(at, &have) && have == pa) {
            continue;
        }
        if (j7m_map_page_at(at, pa, true)) {
            if (firstHole && filled == 0) {
                *firstHole = at;
            }
            filled++;
        }
    }
    return filled;
}

/* Lock held.  Whole-AS pass before a submit: every page of every BO, one flush if anything moved. */
static void
j7m_verify_as(void)
{
    static unsigned passes, shown;
    unsigned checked = 0, filled = 0, unknown = 0;
    uint64_t lo = 0, hi = 0;

    if (!gJ7M.poolVA || !gJ7M.root) {
        return;
    }
    passes++;
    /* 0328 walked every page of every BO on every submit - 17k lookups per submit on a path that
     * was already at 4 fps.  The tables only change when this driver writes or clears a mapping, so
     * re-walk when that happened and once every 32nd submit as a safety net. */
    if (gJ7M.mapGen == gJ7M.verifyGen && (passes % 64) != 0) {
        return;
    }
    gJ7M.verifyGen = gJ7M.mapGen;
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        struct J7MaliBo *bo = &gJ7M.bos[i];
        uint64_t first = 0;
        unsigned n;

        if (!bo->used) {
            continue;
        }
        n = j7m_bo_verify(bo, &checked, &unknown, &first);
        if (n == 0) {
            continue;
        }
        filled += n;
        if (shown < J7M_VERIFY_LOG) {
            shown++;
            IOLog("J7 G3D: submit verify: bo=%u gva=%llx pages=%llu filled=%u first=%llx unknown=%u tables=%u/%u\n",
                i + 1, (unsigned long long)bo->va,
                (unsigned long long)(bo->size / J7M_PAGE), n, (unsigned long long)first,
                unknown, gJ7M.poolUsed, gJ7M.poolTables);
        }
        if (lo == 0 || bo->va < lo) {
            lo = bo->va;
        }
        if (bo->va + bo->size > hi) {
            hi = bo->va + bo->size;
        }
    }
    if (filled) {
        j7m_pool_sync();
        (void)j7m_mmu_flush(lo, hi - lo);
    } else if ((passes % 240) == 0) {
        IOLog("J7 G3D: submit verify: checked=%u filled=0 unknown=%u tables=%u/%u\n",
            checked, unknown, gJ7M.poolUsed, gJ7M.poolTables);
    }
}

/* Lock held.  Re-walk a BO that was just mapped.  Silent unless it finds something: a hole here
 * would mean the map path itself lost the entry it just wrote. */
static void
j7m_map_verify(struct J7MaliBo *bo)
{
    static unsigned shown;
    unsigned checked = 0, unknown = 0;
    uint64_t first = 0;
    const unsigned n = j7m_bo_verify(bo, &checked, &unknown, &first);

    if (n == 0) {
        return;
    }
    j7m_pool_sync();
    (void)j7m_mmu_flush(bo->va, bo->size);
    if (shown++ < J7M_VERIFY_LOG) {
        IOLog("J7 G3D: map verify: bo=%u gva=%llx pages=%llu missing=%u first=%llx unknown=%u\n",
            (unsigned)(bo - gJ7M.bos) + 1, (unsigned long long)bo->va,
            (unsigned long long)(bo->size / J7M_PAGE), n, (unsigned long long)first, unknown);
    }
}

/* First-fit GPU VA for a new BO, one guard page after every BO. 0 if none. */
static uint64_t
j7m_va_alloc(uint64_t size)
{
    uint64_t cand = J7M_VA_LO;
    for (;;) {
        bool clash = false;
        for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
            const struct J7MaliBo &b = gJ7M.bos[i];
            if (!b.used) {
                continue;
            }
            const uint64_t b_end = b.va + b.size + J7M_PAGE;
            if (cand < b_end && cand + size + J7M_PAGE > b.va) {
                cand = b_end;          /* overlaps: restart after this BO */
                clash = true;
                break;
            }
        }
        if (clash) {
            continue;
        }
        if (cand + size + J7M_PAGE > J7M_VA_HI) {
            return 0;
        }
        return cand;
    }
}

/* Create a zeroed BO, map every 4 KiB of it into AS0. Returns the BO or NULL. */
static struct J7MaliBo *
j7m_bo_new(uint64_t size)
{
    size = (size + J7M_PAGE - 1) & ~(J7M_PAGE - 1);
    if (size == 0 || size > (64ULL << 20)) {
        return NULL;
    }
    struct J7MaliBo *bo = NULL;
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        if (!gJ7M.bos[i].used) {
            bo = &gJ7M.bos[i];
            break;
        }
    }
    if (!bo) {
        return NULL;
    }
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut, size, PAGE_SIZE);
    if (!md || !md->getBytesNoCopy()) {
        if (md) {
            md->release();
        }
        return NULL;
    }
    bzero(md->getBytesNoCopy(), size);
    md->performOperation(kIOMemoryIncoherentIOStore, 0, size);
    const uint64_t va = j7m_va_alloc(size);
    if (!va) {
        md->release();
        return NULL;
    }
    const vm_size_t paBytes = (vm_size_t)((size / J7M_PAGE) * sizeof(uint64_t));
    uint64_t *pas = static_cast<uint64_t *>(IOMalloc(paBytes));
    if (pas) {
        bzero(pas, paBytes);
    }
    uint64_t done = 0;
    bool ok = true;
    for (; done < size && ok; done += J7M_PAGE) {
        IOByteCount seg = 0;
        const uint64_t pa = md->getPhysicalSegment(done, &seg, kIOMemoryMapperNone);
        ok = pa && !(pa & (J7M_PAGE - 1)) && seg >= J7M_PAGE && j7m_map_page(va + done, pa);
        if (ok && pas) {
            pas[done / J7M_PAGE] = pa;
        }
    }
    if (!ok) {
        for (uint64_t u = 0; u < done; u += J7M_PAGE) {
            j7m_unmap_page(va + u);
        }
        j7m_pool_sync();
        if (pas) {
            IOFree(pas, paBytes);
        }
        md->release();
        return NULL;
    }
    j7m_pool_sync();
    (void)j7m_mmu_flush(va, size);
    bo->md = md;
    bo->va = va;
    bo->size = size;
    bo->used = true;
    bo->pas = pas;
    j7m_note('k', static_cast<unsigned>(bo - gJ7M.bos) + 1, va, size);
    j7m_map_verify(bo);
    return bo;
}

static void
j7m_bo_free(struct J7MaliBo *bo)
{
    j7m_note('f', static_cast<unsigned>(bo - gJ7M.bos) + 1, bo->va, bo->size);
    unsigned kept = 0;
    gJ7MUnmapping = bo;
    for (uint64_t u = 0; u < bo->size; u += J7M_PAGE) {
        /* Only clear a leaf that still maps this BO's own page: a leaf that maps something else
         * belongs to another live BO and clearing it would unmask that BO for the GPU. */
        uint64_t pa = 0, have = 0;
        if (j7m_bo_page(bo, bo->va + u, &pa) && j7m_lookup(bo->va + u, &have) && have != pa) {
            kept++;
            continue;
        }
        j7m_unmap_page(bo->va + u);
    }
    if (kept) {
        static unsigned freed;
        if (freed++ < 8) {
            IOLog("J7 G3D: free bo=%u gva=%llx kept=%u leaves that map another BO\n",
                (unsigned)(bo - gJ7M.bos) + 1, (unsigned long long)bo->va, kept);
        }
    }
    gJ7MUnmapping = NULL;
    j7m_pool_sync();
    (void)j7m_mmu_flush(bo->va, bo->size);
    if (bo->md) {
        bo->md->release();
    }
    if (bo->umd) {
        bo->umd->complete();
        bo->umd->release();
    }
    if (bo->pas) {
        IOFree(bo->pas, (vm_size_t)((bo->size / J7M_PAGE) * sizeof(uint64_t)));
    }
    bzero(bo, sizeof(*bo));
}

/* Start one job chain on slot (0 = fragment, 1 = vertex/tiler, 2 = compute). */
static bool
j7m_submit(uint32_t slot, uint64_t jc_va, uint32_t *seq)
{
    const uint32_t js = J7M_JS(slot);
    if (gJ7M.inflight || slot > 2 || (a5g3d_read32(gJ7M.gpu, js + 0x60) & 1)) {
        return false;
    }
    a5g3d_write32(gJ7M.gpu, J7M_JOB_IRQ_CLEAR, 0xffffffffu);
    /* Everything the CPU wrote for this chain has to be in memory before the job manager reads it (the S7
     * port saw ~4% of chains stop with JOB_READ_FAULT at their first job without this barrier). */
    __asm__ volatile ("dsb sy" ::: "memory");
    a5g3d_write32(gJ7M.gpu, js + 0x40, static_cast<uint32_t>(jc_va));
    a5g3d_write32(gJ7M.gpu, js + 0x44, static_cast<uint32_t>(jc_va >> 32));
    a5g3d_write32(gJ7M.gpu, js + 0x50, gJ7M.shader);
    a5g3d_write32(gJ7M.gpu, js + 0x54, 0);
    /* AS0 | start/end clean+invalidate flushes | thread priority 8 (Panfrost's JS_CONFIG). */
    a5g3d_write32(gJ7M.gpu, js + 0x58, (8u << 16) | (3u << 8) | (3u << 12));
    __sync_synchronize();
    a5g3d_write32(gJ7M.gpu, js + 0x60, 1);   /* JS_COMMAND_NEXT = START */
    gJ7M.inflight = true;
    gJ7M.inflightSlot = slot;
    *seq = ++gJ7M.seq;
    return true;
}

/*
 * The one thing the log did not have: the address a fault was about.  Every chain said "-3" with the
 * same job status, and the address with the BO it lands in (or the fact that it lands in none) is what
 * tells a missing mapping from a wrong pointer.  First four only - a broken frame repeats this forever.
 */
static void
j7m_fault_report(uint32_t jobIRQ, uint32_t mmuIRQ)
{
    static unsigned shown;

    if (shown++ >= 4) {
        return;
    }
    const uint64_t fault = ((uint64_t)a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x24) << 32) |
        a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x20);
    const uint32_t fs = a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x1c);
    const unsigned status = fs & 0xff;
    const unsigned level = status & 3;
    const unsigned access = (fs >> 8) & 3;
    const char *faultKind = (status & 0xfc) == 0xc0 ? "translation" : "other";
    const char *accessKind = access == 1 ? "execute" : access == 2 ? "read" : access == 3 ? "write" : "unknown";
    uint64_t mapped = 0;
    unsigned used = 0, inside = 0;
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        const struct J7MaliBo &b = gJ7M.bos[i];
        if (!b.used) {
            continue;
        }
        used++;
        mapped += b.size;
        if (fault >= b.va && fault < b.va + b.size) {
            inside = i + 1;
        }
    }

    const uint64_t transtab = ((uint64_t)a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x04) << 32) |
        a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x00);
    uint64_t ent[4] = { 0, 0, 0, 0 };
    bool walkOK = gJ7M.poolVA && gJ7M.poolPA && gJ7M.poolTables;
    uint64_t table = transtab & 0x0000fffffffff000ULL;
    const unsigned shifts[3] = { 39, 30, 21 };
    const uint64_t poolEnd = gJ7M.poolPA + (uint64_t)gJ7M.poolTables * J7M_PAGE;
    for (unsigned i = 0; walkOK && i < 4; i++) {
        if (table < gJ7M.poolPA || table > poolEnd - J7M_PAGE || (table & (J7M_PAGE - 1))) {
            walkOK = false;
            break;
        }
        const unsigned shift = i < 3 ? shifts[i] : 12;
        ent[i] = j7m_table(table)[(fault >> shift) & 0x1ff];
        if (i < 3) {
            if (!(ent[i] & 1)) {
                break;
            }
            table = ent[i] & 0x0000fffffffff000ULL;
        }
    }
    IOLog("J7 G3D: fault at %llx in bo %u of %u (%llu KiB mapped, tables %u/%u): "
        "faultstatus=%08x (%s level %u %s source=%x) mmu_irq=%08x job_irq=%08x js_status=%08x\n",
        (unsigned long long)fault, inside, used, (unsigned long long)(mapped / 1024),
        gJ7M.poolUsed, gJ7M.poolTables, fs, faultKind, level, accessKind, fs >> 16,
        mmuIRQ, jobIRQ, a5g3d_read32(gJ7M.gpu, J7M_JS(gJ7M.inflightSlot) + 0x24));
    IOLog("J7 G3D: walk transtab=%llx root=%llx valid=%u ent=%llx l2=%llx l3=%llx leaf=%llx\n",
        (unsigned long long)transtab, (unsigned long long)gJ7M.root, walkOK,
        (unsigned long long)ent[0], (unsigned long long)ent[1],
        (unsigned long long)ent[2], (unsigned long long)ent[3]);
    /* ent[] here is root at 39, L1 at 30, L2 at 21, L3 leaf at 12: the L3 table lives in ent[2].
     * 0325 read it out of ent[1] (one level too high), so its l3_leaf=0 meant nothing. */
    uint64_t l3base = 0, l3entry = 0, ours = 0;
    const unsigned l3leaf = (fault >> 12) & 0x1ff;
    if (walkOK && (ent[2] & 1)) {
        l3base = ent[2] & 0x0000fffffffff000ULL;
        if (j7m_in_pool(l3base)) {
            l3entry = j7m_table(l3base)[l3leaf];
        }
    }
    const bool selfMapped = j7m_lookup(fault, &ours);
    IOLog("J7 G3D: page-table readback l2_entry=%llx l3_table=%llx l3_leaf=%u leaf=%llx "
        "ours=%u pa=%llx\n", (unsigned long long)ent[2], (unsigned long long)l3base, l3leaf,
        (unsigned long long)l3entry, selfMapped, (unsigned long long)ours);
    const uint32_t n = gJ7M.eventHead < J7M_EVENTS ? gJ7M.eventHead : J7M_EVENTS;
    for (uint32_t i = 0; i < n; i++) {
        const struct J7MEvent &e = gJ7M.events[(gJ7M.eventHead - n + i) % J7M_EVENTS];
        IOLog("J7 G3D: recent %c bo=%u va=%llx size=%llx\n", e.kind, e.slot,
            (unsigned long long)e.va, (unsigned long long)e.size);
    }
}

/*
 * A translation fault stalls the job on this MMU; the walker retries the access once the tables
 * cover it (the model Panfrost implements in panfrost_mmu_map_fault_addr).  The readback showed
 * lanes missing under BOs this driver had already mapped, so a fault is treated as a mapping to
 * finish, not as the end of the frame: the chain is resumed instead of failing the submit, which
 * is what turned every lock-screen frame into "panfrost_batch_submit failed: 5" and a CPU copy
 * of the screen surface.
 */
#define J7M_REPAIR_LIMIT    64      /* resumed faults per job before a real reset */
#define J7M_REPAIR_PAGES    96      /* pages per BO re-walked at fault time */
#define J7M_REPAIR_MAX      16384   /* hard ceiling for a full-BO repair (64 MiB) */

/* 1 = tables changed (resume), 0 = faulting VA is not ours (the selftest's deliberate fault),
 * -1 = our BO but nothing to fix: the GPU walked tables that already cover the page. */
static int
j7m_repair(uint64_t fault)
{
    static unsigned shown;
    const uint64_t page = fault & ~(J7M_PAGE - 1);
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        struct J7MaliBo *bo = &gJ7M.bos[i];
        if (!bo->used || page < bo->va || page >= bo->va + bo->size) {
            continue;
        }
        unsigned fixed = 0, unmappable = 0;
        uint64_t firstFix = 0;
        /* The faulting page first, then every other page of that BO.  The 2026-11-11 log shows the
         * same BO faulting at 0x3904000, 0x3963000, 0x3964000... one page repaired per fault, each
         * repair flushing the whole BO: a storm of faults that costs more than the job itself.
         * Walking the BO once costs a table walk and ends the storm. */
        uint64_t boPages = bo->size / J7M_PAGE;
        if (boPages > J7M_REPAIR_MAX) {
            boPages = J7M_REPAIR_MAX;
        }
        for (unsigned n = 0; n < (unsigned)boPages; n++) {
            const uint64_t at = n == 0 ? page : bo->va + (uint64_t)(n - 1) * J7M_PAGE;
            if (at >= bo->va + bo->size) {
                break;
            }
            uint64_t pa = 0;
            if (!j7m_bo_page(bo, at, &pa)) {
                unmappable++;
                continue;
            }
            uint64_t have = 0;
            if (j7m_lookup(at, &have) && have == pa) {
                continue;
            }
            if (j7m_map_page_at(at, pa, true)) {
                if (fixed == 0) {
                    firstFix = at;
                }
                fixed++;
            }
        }
        if (fixed) {
            j7m_pool_sync();
            (void)j7m_mmu_flush(bo->va, bo->size);
        }
        if (shown++ < 8) {
            IOLog("J7 G3D: fault repair: bo=%u va=%llx fixed=%u first=%llx unmappable=%u "
                "tables=%u/%u\n", i + 1, (unsigned long long)page, fixed,
                (unsigned long long)firstFix, unmappable, gJ7M.poolUsed, gJ7M.poolTables);
        }
        return fixed ? 1 : -1;
    }
    return 0;
}

/* Poll for completion. 0 = done OK, -1 = timeout, -2 = job fault, -3 = MMU fault. */
static int
j7m_wait(unsigned timeout_us)
{
    if (!gJ7M.inflight) {
        return 0;
    }
    const uint32_t slot = gJ7M.inflightSlot;
    const uint32_t jsreg = J7M_JS(slot);
    unsigned repairs = 0;
    for (;;) {
        uint32_t jobIRQ = 0, mmuIRQ = 0, waited = 0;
        for (unsigned us = 0; us < timeout_us; us += 10) {
            waited = us + 10;
            jobIRQ = a5g3d_read32(gJ7M.gpu, J7M_JOB_IRQ_RAWSTAT);
            mmuIRQ = a5g3d_read32(gJ7M.gpu, J7M_MMU_IRQ_RAWSTAT);
            if ((jobIRQ & ((1u << slot) | (1u << (slot + 16)))) || mmuIRQ) {
                break;
            }
            IODelay(10);
        }
        int rc = 0;
        if (mmuIRQ) {
            rc = -3;
        } else if (jobIRQ & (1u << (slot + 16))) {
            rc = -2;
        } else if (!(jobIRQ & (1u << slot))) {
            rc = -1;
        }
        if (rc == -3) {
            j7m_fault_report(jobIRQ, mmuIRQ);
            if (repairs < J7M_REPAIR_LIMIT) {
                const uint64_t fault =
                    ((uint64_t)a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x24) << 32) |
                    a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x20);
                if (j7m_repair(fault) > 0) {
                    /* Tables now cover the address: clear the fault and let the stalled job
                     * retry the access instead of failing the submit. */
                    repairs++;
                    a5g3d_write32(gJ7M.gpu, J7M_MMU_IRQ_CLEAR, mmuIRQ);
                    a5g3d_write32(gJ7M.gpu, J7M_JOB_IRQ_CLEAR, jobIRQ);
                    continue;
                }
            }
        }
        if (rc == -1) {
            /* The completion lands just after the last poll often enough to matter: look once more
             * before calling the chain dead (the 2026-11-11 log shows JOB_IRQ_RAWSTAT = 2 arriving
             * while the client was already being told -1). */
            const uint32_t late = a5g3d_read32(gJ7M.gpu, J7M_JOB_IRQ_RAWSTAT);
            if (late & (1u << slot)) {
                jobIRQ |= late;
                rc = 0;
            } else {
                /* A start the job manager never took stays latched in JS_COMMAND_NEXT and every
                 * later submit to that slot is refused; drop it. */
                a5g3d_write32(gJ7M.gpu, jsreg + 0x60, 0);
            }
        }
        /* Clear everything: a stale slot-done bit would fake a completion for the next chain. */
        a5g3d_write32(gJ7M.gpu, J7M_JOB_IRQ_CLEAR, 0xffffffffu);
        a5g3d_write32(gJ7M.gpu, J7M_MMU_IRQ_CLEAR, 0xffffffffu);
        gJ7M.inflight = false;
        static unsigned loggedWait;
        if (loggedWait++ < 8 || rc != 0) {
            if (loggedWait <= 8) {
                IOLog("J7 G3D: wait slot=%u rc=%d after %u us (job_irq=%08x mmu_irq=%08x)\n",
                    slot, rc, waited, jobIRQ, mmuIRQ);
            }
        }
        return rc;
    }
}

/* GPU soft reset: stops everything, clears AS0 and the job slots. */
static bool
j7m_gpu_reset(void)
{
    a5g3d_write32(gJ7M.gpu, 0x28, 0);
    a5g3d_write32(gJ7M.gpu, 0x24, 0xffffffffu);
    a5g3d_write32(gJ7M.gpu, 0x30, 1);
    for (unsigned us = 0; us < 100000; us += 10) {
        if (a5g3d_read32(gJ7M.gpu, 0x20) & (1u << 8)) {
            a5g3d_write32(gJ7M.gpu, 0x24, 0xffffffffu);
            gJ7M.inflight = false;
            gJ7M.mmuLive = false;
            return true;
        }
        IODelay(10);
    }
    return false;
}


/* ---- j7gpu=6: exercise the job manager end to end from the kernel, no user space needed ---- */
static uint64_t
j7m_word(struct J7MaliBo *bo, uint32_t off)
{
    bo->md->performOperation(kIOMemoryIncoherentIOFlush, 0, bo->size);
    return *reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(bo->md->getBytesNoCopy()) + off);
}

static void
j7m_seed(struct J7MaliBo *bo, uint32_t off)
{
    *reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(bo->md->getBytesNoCopy()) + off) =
        0xa5a5a5a55a5a5a5aULL;
    bo->md->performOperation(kIOMemoryIncoherentIOStore, 0, bo->size);
}

/* Two chained WRITE_VALUE(zero) jobs: out+0x0 and out+0x1008 (second 4 KiB page). */
static bool
j7m_run_chain(struct J7MaliBo *jobs, struct J7MaliBo *out, const char *what)
{
    j7m_seed(out, 0x0);
    j7m_seed(out, 0x8);
    j7m_seed(out, 0x1008);
    uint8_t *base = static_cast<uint8_t *>(jobs->md->getBytesNoCopy());
    bzero(base, 0x200);
    A5G3DWriteValueJob *j0 = reinterpret_cast<A5G3DWriteValueJob *>(base);
    A5G3DWriteValueJob *j1 = reinterpret_cast<A5G3DWriteValueJob *>(base + 0x100);
    j0->typeAndSize = 1 | (2 << 1);
    j0->index = 1;
    j0->next = jobs->va + 0x100;
    j0->outputVA = out->va + 0x0;
    j0->valueType = 3;
    j1->typeAndSize = 1 | (2 << 1);
    j1->index = 2;
    j1->next = 0;
    j1->outputVA = out->va + 0x1008;
    j1->valueType = 3;
    jobs->md->performOperation(kIOMemoryIncoherentIOStore, 0, jobs->size);
    __sync_synchronize();
    uint32_t seq = 0;
    if (!j7m_submit(1, jobs->va, &seq)) {
        IOLog("J7 G3D: [%s] submit refused\n", what);
        return false;
    }
    const int rc = j7m_wait(200000);
    const uint64_t w0 = j7m_word(out, 0x0), w1 = j7m_word(out, 0x8), w2 = j7m_word(out, 0x1008);
    IOLog("J7 G3D: [%s] seq=%u wait=%d out0=%016llx out8=%016llx out1008=%016llx js1_status=%08x\n",
        what, seq, rc, w0, w1, w2, a5g3d_read32(gJ7M.gpu, J7M_JS(1) + 0x24));
    return rc == 0 && w0 == 0 && w2 == 0 && w1 == 0xa5a5a5a55a5a5a5aULL;
}

static bool
j7m_selftest(IOService *service, vm_offset_t gpu)
{
    bzero(&gJ7M, sizeof(gJ7M));
    gJ7M.gpu = gpu;
    gJ7M.shader = a5g3d_read32(gpu, 0x140);
    if (!gJ7M.shader || !(a5g3d_read32(gpu, 0x018) & 1) || !(a5g3d_read32(gpu, 0x01c) & 2)) {
        IOLog("J7 G3D: job manager: cores not ready\n");
        service->setProperty("J7G3DJobMgr", "cores-not-ready");
        return false;
    }
    if (!j7m_pool_init()) {
        IOLog("J7 G3D: job manager: page-table pool allocation failed\n");
        service->setProperty("J7G3DJobMgr", "pool-failed");
        return false;
    }
    IOLog("J7 G3D: job manager: page-table pool pa=%016llx %u tables, root=%016llx\n",
        gJ7M.poolPA, gJ7M.poolTables, gJ7M.root);
    j7m_pool_sync();
    bool ok = j7m_mmu_enable();
    struct J7MaliBo *jobs = ok ? j7m_bo_new(0x1000) : NULL;
    struct J7MaliBo *out = jobs ? j7m_bo_new(0x2000) : NULL;
    struct J7MaliBo *pad = out ? j7m_bo_new(0x10000) : NULL;
    ok = ok && jobs && out && pad;
    if (ok) {
        IOLog("J7 G3D: job manager: BO jobs va=%llx out va=%llx pad va=%llx\n", jobs->va, out->va, pad->va);
        ok = j7m_run_chain(jobs, out, "chain 1");
    } else {
        IOLog("J7 G3D: job manager: MMU enable or BO allocation failed\n");
    }
    if (ok) {
        /* Allocator + unmap + remap: free the middle BO, take the same hole again, run again. */
        const uint64_t oldVA = out->va;
        j7m_bo_free(out);
        out = j7m_bo_new(0x2000);
        ok = out != NULL;
        if (ok) {
            IOLog("J7 G3D: job manager: BO out re-created at va=%llx (was %llx)\n", out->va, oldVA);
            /* the job chain still names the old VA; rebuild it for the new one */
            ok = j7m_run_chain(jobs, out, "chain 2");
        }
    }
    if (ok && out) {
        /* Deliberate fault: a chain that writes to an unmapped VA must fail cleanly (MMU IRQ). */
        uint8_t *base = static_cast<uint8_t *>(jobs->md->getBytesNoCopy());
        A5G3DWriteValueJob *j0 = reinterpret_cast<A5G3DWriteValueJob *>(base);
        bzero(base, 0x200);
        j0->typeAndSize = 1 | (2 << 1);
        j0->index = 1;
        j0->outputVA = 0x7ff00000ULL;     /* inside J7M_VA range, never mapped */
        j0->valueType = 3;
        jobs->md->performOperation(kIOMemoryIncoherentIOStore, 0, jobs->size);
        uint32_t seq = 0;
        if (j7m_submit(1, jobs->va, &seq)) {
            const int rc = j7m_wait(200000);
            IOLog("J7 G3D: [fault test] seq=%u wait=%d (expect -3 MMU fault or -2 job fault) mmu=%08x\n",
                seq, rc, a5g3d_read32(gpu, J7M_MMU_IRQ_RAWSTAT));
            ok = (rc == -3 || rc == -2);
            /* recover: reset clears AS0 and the faulted slot; reprogram the MMU */
            ok = j7m_gpu_reset() && ok;
        } else {
            ok = false;
        }
    }
    /* Teardown. A hung GPU may still touch these pages: only free after a good reset. */
    const bool reset = j7m_gpu_reset();
    if (reset) {
        if (pad) { j7m_bo_free(pad); }
        if (out) { j7m_bo_free(out); }
        if (jobs) { j7m_bo_free(jobs); }
        gJ7M.pool->release();
        gJ7M.pool = NULL;
    }
    IOLog("J7 G3D: job manager selftest %s%s\n", ok ? "PASSED" : "FAILED", reset ? "" : " (GPU did not reset; memory kept)");
    service->setProperty("J7G3DJobMgr", ok ? "selftest-passed" : "selftest-failed");
    return ok;
}

/* ===================================================================================
 * j7gpu=7: serve the job manager to user space through an IOUserClient.
 *
 * The driver keeps the GPU powered, AS0 programmed and the page-table pool alive.  A client
 * hands over ordinary memory it owns (selector 0); the kernel wires it and maps it into AS0 at
 * a kernel-chosen GPU VA.  Clients submit a job chain by GPU VA (selector 2) and wait (3).
 * Cache maintenance is done by the kernel around every submit/wait (whole maps for now).
 * AS0 is shared: a client that crafts GPU addresses can reach other clients' mappings; fine
 * for a single compositor, to be split into one AS per client later.
 * =================================================================================== */
static IOLock *gJ7MLock;
static bool gJ7MServing;
static uint32_t gJ7MGpuId, gJ7MShaderPresent, gJ7MTilerPresent, gJ7ML2Present;

static bool
j7m_power_wait(uint32_t on_reg, uint32_t ready_reg, uint32_t mask)
{
    a5g3d_write32(gJ7M.gpu, on_reg, mask);
    for (unsigned us = 0; us < 50000; us += 10) {
        if ((a5g3d_read32(gJ7M.gpu, ready_reg) & mask) == mask) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

/*
 * After every reset, before the cores are powered, as Linux panfrost does (panfrost_gpu_init_quirks); the
 * Galaxy S7 port found the same three needed on the T880, the same GPU family as this T830:
 *   SHADER_CONFIG 0xf04 bit 16 SC_LS_ALLOW_ATTR_TYPES: the load/store unit must accept typed attribute
 *     descriptors, or Mesa's jobs stop with DATA_INVALID_FAULT (0x58)
 *   TILER_CONFIG 0xf08 bit 0 TC_CLOCK_GATE_OVERRIDE: the tiler clock gate (T76X_3953)
 *   JM_CONFIG 0xf00: job throttle limit 0x3f at bit 3
 * Without them the first tiler job of Mesa's first batch (a write_value, a vertex and a tiler job) ended with
 * exception 0x58 and backboardd went down; the reset on that fault cleared them for good.
 */
static void
j7m_config_quirks(void)
{
    a5g3d_write32(gJ7M.gpu, 0xf04, a5g3d_read32(gJ7M.gpu, 0xf04) | (1u << 16));
    a5g3d_write32(gJ7M.gpu, 0xf08, a5g3d_read32(gJ7M.gpu, 0xf08) | 1u);
    a5g3d_write32(gJ7M.gpu, 0xf00, 0x3fu << 3);
    IOLog("J7 G3D: config jm=%08x shader=%08x tiler=%08x l2_mmu=%08x\n",
        a5g3d_read32(gJ7M.gpu, 0xf00), a5g3d_read32(gJ7M.gpu, 0xf04),
        a5g3d_read32(gJ7M.gpu, 0xf08), a5g3d_read32(gJ7M.gpu, 0xf0c));
}

/* L2, then shader, then tiler (the Panfrost order that passed on the phone). */
static bool
j7m_cores_on(void)
{
    j7m_config_quirks();
    a5g3d_write32(gJ7M.gpu, 0x24, 0xffffffffu);
    const bool ok = j7m_power_wait(0x1a0, 0x160, gJ7ML2Present) &&
        j7m_power_wait(0x180, 0x140, gJ7MShaderPresent) &&
        j7m_power_wait(0x190, 0x150, gJ7MTilerPresent);
    a5g3d_write32(gJ7M.gpu, 0x24, 0xffffffffu);
    gJ7M.shader = a5g3d_read32(gJ7M.gpu, 0x140);
    return ok;
}

/* After a fault or timeout: reset, repower, reprogram AS0.  Page tables and maps stay valid. */
static bool
j7m_recover(void)
{
    return j7m_gpu_reset() && j7m_cores_on() && j7m_mmu_enable();
}

/* Take over the GPU for user clients.  The GPU must be idle and unpowered, as the probe found it. */
static bool
j7m_serve_start(IOService *service, vm_offset_t gpu)
{
    bzero(&gJ7M, sizeof(gJ7M));
    gJ7M.gpu = gpu;
    gJ7MGpuId = a5g3d_read32(gpu, 0x000);
    gJ7MShaderPresent = a5g3d_read32(gpu, 0x100);
    gJ7MTilerPresent = a5g3d_read32(gpu, 0x110);
    gJ7ML2Present = a5g3d_read32(gpu, 0x120);
    if (a5g3d_read32(gpu, 0x140) || a5g3d_read32(gpu, 0x150) || a5g3d_read32(gpu, 0x160) ||
        a5g3d_read32(gpu, 0x034) || !(a5g3d_read32(gpu, 0x018) & 1) || !(a5g3d_read32(gpu, 0x01c) & 2) ||
        !gJ7MShaderPresent || !gJ7MTilerPresent || !gJ7ML2Present) {
        IOLog("J7 G3D: serve: GPU not idle/unpowered or unexpected present masks\n");
        service->setProperty("J7G3DServe", "gpu-not-idle");
        return false;
    }
    gJ7MLock = IOLockAlloc();
    if (!gJ7MLock || !j7m_gpu_reset() || !j7m_cores_on() || !j7m_pool_init()) {
        IOLog("J7 G3D: serve: reset, core power or page-table pool failed\n");
        service->setProperty("J7G3DServe", "bring-up-failed");
        return false;
    }
    j7m_pool_sync();
    if (!j7m_mmu_enable()) {
        IOLog("J7 G3D: serve: AS0 enable failed\n");
        service->setProperty("J7G3DServe", "mmu-failed");
        return false;
    }
    gJ7M.mmuReady = true;   /* AS0 programmed and the page-table pool is live: the user client may submit. */
    service->setProperty(kIOUserClientClassKey, "J7MaliUserClient");
    service->setProperty("J7G3DServe", "ready");
    gJ7MServing = true;
    IOLog("J7 G3D: serving user clients (gpu_id=%08x, cores on, AS0 up, page-table pool %u tables)\n",
        gJ7MGpuId, gJ7M.poolTables);
    return true;
}

/* User space only sees the ENOMEM of the ioctl; which of the four steps ran out has to be said here. */
static void
j7m_map_fail(const char *why)
{
    static unsigned shown;

    if (shown++ < 8) {
        unsigned used = 0;
        for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
            if (gJ7M.bos[i].used) {
                used++;
            }
        }
        IOLog("J7 G3D: client map refused: %s (%u of %u BO slots used, %u of %u page tables)\n", why, used,
            (unsigned)J7M_MAX_BOS, gJ7M.poolUsed, gJ7M.poolTables);
    }
}

/* Lock held.  Wire md and map its pages into AS0.  On success the slot owns the caller's
 * reference to md; on failure the caller keeps it. */
static struct J7MaliBo *
j7m_map_user(IOMemoryDescriptor *md, void *owner)
{
    const uint64_t size = md->getLength();
    if (size == 0 || size > (64ULL << 20) || (size & (J7M_PAGE - 1))) {
        return NULL;
    }
    struct J7MaliBo *bo = NULL;
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        if (!gJ7M.bos[i].used) {
            bo = &gJ7M.bos[i];
            break;
        }
    }
    if (!bo) {
        j7m_map_fail("no free BO slot");
        return NULL;
    }
    if (md->prepare(kIODirectionInOut) != kIOReturnSuccess) {
        j7m_map_fail("wiring the client's memory failed");
        return NULL;
    }
    const uint64_t va = j7m_va_alloc(size);
    if (!va) {
        j7m_map_fail("no GPU VA left");
        md->complete();
        return NULL;
    }
    const vm_size_t paBytes = (vm_size_t)((size / J7M_PAGE) * sizeof(uint64_t));
    uint64_t *pas = static_cast<uint64_t *>(IOMalloc(paBytes));
    if (pas) {
        bzero(pas, paBytes);
    }
    uint64_t done = 0;
    bool ok = true;
    for (; done < size && ok; done += J7M_PAGE) {
        IOByteCount seg = 0;
        const uint64_t pa = md->getPhysicalSegment(done, &seg, kIOMemoryMapperNone);
        ok = pa && !(pa & (J7M_PAGE - 1)) && seg >= J7M_PAGE && j7m_map_page(va + done, pa);
        if (ok && pas) {
            pas[done / J7M_PAGE] = pa;
        }
    }
    if (!ok) {
        j7m_map_fail("a physical segment or the page-table pool");
        for (uint64_t u = 0; u < done; u += J7M_PAGE) {
            j7m_unmap_page(va + u);
        }
        j7m_pool_sync();
        if (pas) {
            IOFree(pas, paBytes);
        }
        md->complete();
        return NULL;
    }
    j7m_pool_sync();
    (void)j7m_mmu_flush(va, size);
    bo->md = NULL;
    bo->umd = md;
    bo->owner = owner;
    bo->va = va;
    bo->start = va;
    bo->size = size;
    bo->used = true;
    bo->pas = pas;
    j7m_note('m', static_cast<unsigned>(bo - gJ7M.bos) + 1, va, size);
    j7m_map_verify(bo);
    return bo;
}

/* Lock held. */
static void
j7m_cache(unsigned options)
{
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        struct J7MaliBo &b = gJ7M.bos[i];
        if (!b.used) {
            continue;
        }
        IOMemoryDescriptor *d = b.umd ? b.umd : static_cast<IOMemoryDescriptor *>(b.md);
        d->performOperation(options, 0, b.size);
    }
}

/* Read-only Midgard GPU_CONTROL identity registers a client may read (Panfrost's GET_PARAMs):
 * GPU_ID to JS_PRESENT, the THREAD, TEXTURE and JS feature registers, and SHADER, TILER and L2 PRESENT
 * (low and high words). Named offsets only: the PMU and CMU pages of this SoC fault on holes, so nothing
 * is read by range. */
static bool
j7m_id_reg_ok(uint64_t off)
{
    if (off & 3) {
        return false;
    }
    if (off <= 0x01c || (off >= 0x0a0 && off <= 0x0fc)) {
        return true;
    }
    return off == 0x100 || off == 0x104 || off == 0x110 || off == 0x114 || off == 0x120 || off == 0x124;
}

class J7MaliUserClient : public IOUserClient {
    OSDeclareDefaultStructors(J7MaliUserClient);

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
    void freeMaps(void);

private:
    task_t fTask;
    bool ownsRange(uint64_t va);
};

OSDefineMetaClassAndStructors(J7MaliUserClient, IOUserClient);

bool
J7MaliUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
        return false;
    }
    fTask = owningTask;
    IOLog("J7 G3D: user client opened\n");
    return true;
}

void
J7MaliUserClient::freeMaps(void)
{
    if (!gJ7MLock) {
        return;
    }
    IOLockLock(gJ7MLock);
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        if (gJ7M.bos[i].used && gJ7M.bos[i].owner == this) {
            j7m_bo_free(&gJ7M.bos[i]);
        }
    }
    IOLockUnlock(gJ7MLock);
}

IOReturn
J7MaliUserClient::clientClose(void)
{
    freeMaps();
    terminate();
    return kIOReturnSuccess;
}

/* Lock held: does va lie inside a mapping this client owns? */
bool
J7MaliUserClient::ownsRange(uint64_t va)
{
    for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
        const struct J7MaliBo &b = gJ7M.bos[i];
        if (b.used && b.owner == this && va >= b.va && va < b.va + b.size) {
            return true;
        }
    }
    return false;
}

IOReturn
J7MaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;
    if (!gJ7MServing) {
        return kIOReturnNotReady;
    }
    IOReturn ret = kIOReturnBadArgument;
    IOLockLock(gJ7MLock);
    switch (selector) {
    case 0:     /* map memory: in (addr, len) -> out (gpu va) */
        if (args->scalarInputCount == 2 && args->scalarOutputCount >= 1) {
            const uint64_t addr = args->scalarInput[0], len = args->scalarInput[1];
            const uint64_t base = trunc_page_64(addr), end = round_page_64(addr + len);
            if (len == 0 || end <= base || end - base > (64ULL << 20)) {
                break;
            }
            IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(base, end - base,
                kIODirectionInOut, fTask);
            if (!md) {
                ret = kIOReturnNoMemory;
                break;
            }
            struct J7MaliBo *bo = j7m_map_user(md, this);
            if (!bo) {
                md->release();
                ret = kIOReturnNoResources;
                break;
            }
            bo->start = bo->va + (addr - base);
            {
                static unsigned mapped;
                if (mapped++ < J7M_VERIFY_LOG) {
                    IOLog("J7 G3D: client map gva=%llx %llu bytes bo=%u\n",
                        (unsigned long long)bo->start, (unsigned long long)(end - base),
                        (unsigned)(bo - gJ7M.bos) + 1);
                }
            }
            args->scalarOutput[0] = bo->start;
            args->scalarOutputCount = 1;
            ret = kIOReturnSuccess;
        }
        break;
    case 1:     /* unmap: in (gpu va returned by selector 0) */
        if (args->scalarInputCount == 1) {
            ret = kIOReturnNotFound;
            for (unsigned i = 0; i < J7M_MAX_BOS; i++) {
                if (gJ7M.bos[i].used && gJ7M.bos[i].owner == this && gJ7M.bos[i].start == args->scalarInput[0]) {
                    j7m_bo_free(&gJ7M.bos[i]);
                    ret = kIOReturnSuccess;
                    break;
                }
            }
            {
                static unsigned unmapped;
                if (unmapped++ < J7M_VERIFY_LOG) {
                    IOLog("J7 G3D: client unmap gva=%llx ret=%x\n",
                        (unsigned long long)args->scalarInput[0], ret);
                }
            }
        }
        break;
    case 2:     /* submit: in (slot, job chain gpu va) -> out (seq) */
        if (args->scalarInputCount == 2 && args->scalarOutputCount >= 1) {
            const uint64_t slot = args->scalarInput[0], jc = args->scalarInput[1];
            if (slot > 2 || !ownsRange(jc)) {
                break;
            }
            uint32_t seq = 0;
            if (!gJ7M.mmuReady) {
                IOLog("J7 G3D: client submit refused: AS0/MMU not ready yet (mmuReady=%u, poolTables=%u)\n",
                    gJ7M.mmuReady, gJ7M.poolTables);
                ret = kIOReturnNotReady;
                break;
            }
            /* No page-table walk here any more: the map paths verify on change and the whole-AS
             * audit still runs as a safety net, just not in front of every single submit. */
            {
                const uint64_t t0 = j7m_now_us();
                j7m_cache(kIOMemoryIncoherentIOStore);
                j7mStatStoreUs += j7m_now_us() - t0;
            }
            j7mStatSubmits++;
            if (!j7m_submit(static_cast<uint32_t>(slot), jc, &seq)) {
                ret = kIOReturnBusy;
                break;
            }
            args->scalarOutput[0] = seq;
            args->scalarOutputCount = 1;
            ret = kIOReturnSuccess;
        }
        break;
    case 3:     /* wait: in (timeout us) -> out (status 0/-1/-2/-3, js status) */
        if (args->scalarInputCount == 1 && args->scalarOutputCount >= 2) {
            const uint32_t slot = gJ7M.inflightSlot;
            const bool wasInflight = gJ7M.inflight;
            /* 600 us is a frame's worth of GPU work at any useful rate; a client asking for five
             * seconds only ever waited out a job slot that was already dead. */
            /* 20 ms: a tiled 1242x2208 chain can take longer than 2 ms to raise its interrupt
             * (the 2026-11-11 log shows the IRQ arriving after the client was already told -1),
             * and the wait still returns the moment the interrupt lands. */
            const unsigned timeout = static_cast<unsigned>(args->scalarInput[0] > 20000 ? 20000 : args->scalarInput[0]);
            int rc;
            {
                const uint64_t t0 = j7m_now_us();
                rc = j7m_wait(timeout);
                j7mStatWaitUs += j7m_now_us() - t0;
                j7mStatWaits++;
            }
            const uint32_t jsReg = a5g3d_read32(gJ7M.gpu, J7M_JS(slot) + 0x20);
            const uint32_t js = wasInflight ? a5g3d_read32(gJ7M.gpu, J7M_JS(slot) + 0x24) : 0;
            {
                const uint64_t t0 = j7m_now_us();
                j7m_cache(kIOMemoryIncoherentIOFlush);
                j7mStatFlushUs += j7m_now_us() - t0;
            }
            if (rc != 0) {
                /* Why did a real client chain never complete?  Dump the job slot and the fault block
                 * once per boot (four times) instead of guessing: a chain whose START never latched
                 * (JS_COMMAND_NEXT still set), one that faulted (JS_STATUS exception nibble), and an
                 * MMU fault all look the same from the client. */
                static unsigned dumps;
                if (dumps++ < 4) {
                    IOLog("J7 G3D: dump %u slot=%u rc=%d js_status(+20)=%08x js_status(+24)=%08x "
                        "js_config(+58)=%08x js_cmd(+60)=%08x\n", dumps, slot, rc, jsReg, js,
                        a5g3d_read32(gJ7M.gpu, J7M_JS(slot) + 0x58),
                        a5g3d_read32(gJ7M.gpu, J7M_JS(slot) + 0x60));
                    IOLog("J7 G3D: dump %u job_irq raw=%08x stat=%08x mask=%08x mmu_irq raw=%08x "
                        "as_fault=%08x as_transtab=%08x gpu_fault=%08x\n", dumps,
                        a5g3d_read32(gJ7M.gpu, J7M_JOB_IRQ_RAWSTAT),
                        a5g3d_read32(gJ7M.gpu, J7M_JOB_IRQ_RAWSTAT + 8),
                        a5g3d_read32(gJ7M.gpu, J7M_JOB_IRQ_RAWSTAT + 12),
                        a5g3d_read32(gJ7M.gpu, J7M_MMU_IRQ_RAWSTAT),
                        a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x1c),
                        a5g3d_read32(gJ7M.gpu, J7M_AS0 + 0x00),
                        a5g3d_read32(gJ7M.gpu, 0xf3c));
                }
                /* A soft reset tears down the MMU, the cores and every mapping; doing that for
                 * every failed chain was the bulk of the frame time.  Only reset once a few
                 * have failed in a row, or at once when the MMU itself faulted. */
                static unsigned consecutiveFailures;
                if (++consecutiveFailures >= 4 || rc == -3) {
                    consecutiveFailures = 0;
                    if (!j7m_recover()) {
                        gJ7MServing = false;
                        IOLog("J7 G3D: recovery failed; GPU service stopped\n");
                    }
                }
            }
            args->scalarOutput[0] = static_cast<uint64_t>(static_cast<int64_t>(rc));
            args->scalarOutput[1] = js;
            args->scalarOutputCount = 2;
            ret = kIOReturnSuccess;
        }
        break;
    case 4:     /* info: out (gpu id, shader/tiler/L2 present, JS present, AS present) */
        if (args->scalarOutputCount >= 6) {
            args->scalarOutput[0] = gJ7MGpuId;
            args->scalarOutput[1] = gJ7MShaderPresent;
            args->scalarOutput[2] = gJ7MTilerPresent;
            args->scalarOutput[3] = gJ7ML2Present;
            args->scalarOutput[4] = a5g3d_read32(gJ7M.gpu, 0x01c);
            args->scalarOutput[5] = a5g3d_read32(gJ7M.gpu, 0x018);
            args->scalarOutputCount = 6;
            ret = kIOReturnSuccess;
        }
        break;
    case 5:     /* log: structure input = text, copied into the kernel log (reaches the persistent log) */
        if (args->structureInput && args->structureInputSize > 0 && args->structureInputSize <= 160) {
            char line[161];
            memcpy(line, args->structureInput, args->structureInputSize);
            line[args->structureInputSize] = '\0';
            IOLog("J7 G3D client: %s\n", line);
            ret = kIOReturnSuccess;
        }
        break;
    case 6:     /* identity register: in (byte offset) -> out (value). Read-only GPU_CONTROL ID registers only. */
        if (args->scalarInputCount == 1 && args->scalarOutputCount >= 1 && j7m_id_reg_ok(args->scalarInput[0])) {
            args->scalarOutput[0] = a5g3d_read32(gJ7M.gpu, static_cast<vm_offset_t>(args->scalarInput[0]));
            args->scalarOutputCount = 1;
            ret = kIOReturnSuccess;
        }
        break;
    default:
        break;
    }
    IOLockUnlock(gJ7MLock);
    return ret;
}

/* ---- kernel-side client: drives the real externalMethod() entry points with kernel_task as the
 * "user" task, so the whole path (map user memory, submit, wait, fault, recovery, unmap) is checked
 * from the boot log without any user-space binary on the phone. ---- */
static IOReturn
j7m_call(J7MaliUserClient *c, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out,
    uint32_t nout, const void *sin = NULL, uint32_t sinsz = 0)
{
    IOExternalMethodArguments a;
    bzero(&a, sizeof(a));
    a.selector = sel;
    a.scalarInput = in;
    a.scalarInputCount = nin;
    a.scalarOutput = out;
    a.scalarOutputCount = nout;
    a.structureInput = sin;
    a.structureInputSize = sinsz;
    return c->externalMethod(sel, &a, NULL, NULL, NULL);
}

static bool
j7m_client_selftest(IOService *service)
{
    J7MaliUserClient *c = OSTypeAlloc(J7MaliUserClient);
    if (!c || !c->initWithTask(kernel_task, NULL, 0)) {
        IOLog("J7 G3D: client selftest: cannot create a client object\n");
        service->setProperty("J7G3DClientTest", "no-client");
        if (c) {
            c->release();
        }
        return false;
    }
    const vm_size_t len = 0x10000;
    uint8_t *mem = static_cast<uint8_t *>(IOMallocAligned(len, PAGE_SIZE));
    if (!mem) {
        c->release();
        return false;
    }
    bzero(mem, len);
    uint64_t in[3] = { 0 }, out[6] = { 0 };
    bool ok = true;
    IOReturn r = j7m_call(c, 4, in, 0, out, 6);
    IOLog("J7 G3D: client info ret=%x gpu_id=%08llx shader=%llx tiler=%llx l2=%llx js=%llx as=%llx\n",
        r, out[0], out[1], out[2], out[3], out[4], out[5]);
    ok = ok && r == kIOReturnSuccess && (out[0] >> 16) == 0x830;
    const uint64_t gpuIdFromInfo = out[0];
    in[0] = 0x000;
    r = j7m_call(c, 6, in, 1, out, 1);
    ok = ok && r == kIOReturnSuccess && out[0] == gpuIdFromInfo;
    in[0] = 0x34;       /* GPU_STATUS is not an ID register */
    IOReturn refused = j7m_call(c, 6, in, 1, out, 1);
    in[0] = 0x0c0;
    IOReturn js0 = j7m_call(c, 6, in, 1, out, 1);
    IOLog("J7 G3D: client id registers: gpu_id via selector 6 ret=%x, GPU_STATUS refused ret=%x, JS0_FEATURES=%08llx ret=%x\n",
        r, refused, out[0], js0);
    ok = ok && refused == kIOReturnBadArgument && js0 == kIOReturnSuccess;
    const char hello[] = "hello from the kernel-side client";
    r = j7m_call(c, 5, NULL, 0, NULL, 0, hello, sizeof(hello) - 1);
    ok = ok && r == kIOReturnSuccess;

    uint64_t gva = 0;
    in[0] = reinterpret_cast<uint64_t>(mem);
    in[1] = len;
    r = j7m_call(c, 0, in, 2, out, 1);
    gva = out[0];
    IOLog("J7 G3D: client map ret=%x gva=%llx (%lu bytes)\n", r, gva, static_cast<unsigned long>(len));
    ok = ok && r == kIOReturnSuccess && gva != 0;

    /* Job chain at +0, two outputs: +0x2008 (first 16 KiB page) and +0x9010 (a different one). */
    for (unsigned round = 0; ok && round < 3; round++) {
        const bool faulty = (round == 1);
        bzero(mem, 0x200);
        A5G3DWriteValueJob *j0 = reinterpret_cast<A5G3DWriteValueJob *>(mem);
        A5G3DWriteValueJob *j1 = reinterpret_cast<A5G3DWriteValueJob *>(mem + 0x100);
        *reinterpret_cast<uint64_t *>(mem + 0x2008) = 0xa5a5a5a55a5a5a5aULL;
        *reinterpret_cast<uint64_t *>(mem + 0x9010) = 0xa5a5a5a55a5a5a5aULL;
        j0->typeAndSize = 1 | (2 << 1);
        j0->index = 1;
        j0->next = faulty ? 0 : gva + 0x100;
        j0->outputVA = faulty ? 0x7ff00000ULL : gva + 0x2008;     /* round 1: unmapped VA */
        j0->valueType = 3;
        j1->typeAndSize = 1 | (2 << 1);
        j1->index = 2;
        j1->outputVA = gva + 0x9010;
        j1->valueType = 3;
        in[0] = 1;
        in[1] = gva;
        r = j7m_call(c, 2, in, 2, out, 1);
        const uint64_t seq = out[0];
        in[0] = 200000;
        IOReturn w = j7m_call(c, 3, in, 1, out, 2);
        const int rc = static_cast<int>(static_cast<int64_t>(out[0]));
        const uint64_t a = *reinterpret_cast<volatile uint64_t *>(mem + 0x2008);
        const uint64_t b = *reinterpret_cast<volatile uint64_t *>(mem + 0x9010);
        IOLog("J7 G3D: client round %u (%s) submit=%x seq=%llu wait=%x status=%d js=%08llx out2008=%016llx out9010=%016llx\n",
            round, faulty ? "fault" : "good", r, seq, w, rc, out[1], a, b);
        if (faulty) {
            ok = r == kIOReturnSuccess && w == kIOReturnSuccess && (rc == -3 || rc == -2);
        } else {
            ok = r == kIOReturnSuccess && w == kIOReturnSuccess && rc == 0 && a == 0 && b == 0;
        }
    }
    /* A submit for a VA this client never mapped must be refused. */
    in[0] = 1;
    in[1] = 0x7ff00000ULL;
    r = j7m_call(c, 2, in, 2, out, 1);
    IOLog("J7 G3D: client submit of an unowned VA -> %x (expect bad argument)\n", r);
    ok = ok && r == kIOReturnBadArgument;
    in[0] = gva;
    r = j7m_call(c, 1, in, 1, out, 0);
    IOLog("J7 G3D: client unmap ret=%x\n", r);
    ok = ok && r == kIOReturnSuccess;
    c->freeMaps();
    c->release();
    IOFreeAligned(mem, len);
    IOLog("J7 G3D: client selftest %s\n", ok ? "PASSED" : "FAILED");
    service->setProperty("J7G3DClientTest", ok ? "passed" : "failed");
    return ok;
}

/*
 * j7gpu=4: reset the GPU, power L2 -> shader -> tiler (the Panfrost order), check READY, power them
 * off again.  Only GPU_CONTROL registers below 0x230 are touched; no address space, no job slot,
 * no memory is programmed and every GPU interrupt stays masked.
 */
static bool
j7g3d_wait_ready(vm_offset_t gpu, uint32_t ready_reg, uint32_t mask, bool want_on,
    unsigned timeout_us, uint32_t *last)
{
    uint32_t ready = 0;
    for (unsigned us = 0; us < timeout_us; us += 10) {
        ready = a5g3d_read32(gpu, ready_reg);
        if (want_on ? ((ready & mask) == mask) : ((ready & mask) == 0)) {
            *last = ready;
            return true;
        }
        IODelay(10);
    }
    *last = ready;
    return false;
}

static void
j7g3d_power_test(IOService *service, vm_offset_t gpu, int level,
    IOBufferMemoryDescriptor *&storage)
{
    const uint32_t shader = a5g3d_read32(gpu, 0x100);
    const uint32_t tiler = a5g3d_read32(gpu, 0x110);
    const uint32_t l2 = a5g3d_read32(gpu, 0x120);
    if (a5g3d_read32(gpu, 0x140) || a5g3d_read32(gpu, 0x150) || a5g3d_read32(gpu, 0x160) ||
        a5g3d_read32(gpu, 0x034)) {
        service->setProperty("J7G3DPowerResult", "not-idle-before-reset");
        IOLog("J7 G3D: GPU not idle/off before reset; power test skipped\n");
        return;
    }
    a5g3d_write32(gpu, 0x28, 0);            /* GPU_IRQ_MASK: nothing enabled */
    a5g3d_write32(gpu, 0x24, 0xffffffffu);  /* GPU_IRQ_CLEAR */
    a5g3d_write32(gpu, 0x30, 1);            /* GPU_COMMAND_SOFT_RESET */
    uint32_t raw = 0;
    unsigned us = 0;
    for (; us < 100000; us += 10) {
        raw = a5g3d_read32(gpu, 0x20);      /* GPU_IRQ_RAWSTAT */
        if (raw & (1u << 8)) {              /* RESET_COMPLETED */
            break;
        }
        IODelay(10);
    }
    IOLog("J7 G3D: soft reset rawstat=%08x after %u us (bit8 = reset completed)\n", raw, us);
    if (!(raw & (1u << 8))) {
        service->setProperty("J7G3DPowerResult", "reset-timeout");
        return;
    }
    a5g3d_write32(gpu, 0x24, 0xffffffffu);

    const struct { const char *name; uint32_t on, ready, mask; unsigned timeout_us; } up[] = {
        { "L2", 0x1a0, 0x160, l2, 20000 },
        { "shader", 0x180, 0x140, shader, 20000 },
        { "tiler", 0x190, 0x150, tiler, 20000 },
    };
    bool ok = true;
    for (const auto &step : up) {
        uint32_t ready = 0;
        a5g3d_write32(gpu, step.on, step.mask);
        bool on = j7g3d_wait_ready(gpu, step.ready, step.mask, true, step.timeout_us, &ready);
        IOLog("J7 G3D: power on %s mask=%08x ready=%08x %s\n", step.name, step.mask, ready,
            on ? "OK" : "TIMEOUT");
        if (!on) {
            ok = false;
            break;
        }
    }
    IOLog("J7 G3D: after power on: GPU_STATUS=%08x IRQ_RAWSTAT=%08x\n",
        a5g3d_read32(gpu, 0x34), a5g3d_read32(gpu, 0x20));
    a5g3d_write32(gpu, 0x24, 0xffffffffu);

    /*
     * j7gpu=5: the A5's proven private-page test, now on the T830: one WRITE_VALUE job on JS1 through
     * a private 4-level page table in AS0 (16 KiB, no access to RAM or the display).  The job must
     * zero a 64-bit word that starts as 0xa5a5a5a55a5a5a5a.  Always resets the GPU afterwards if it
     * did not complete cleanly.
     */
    bool jobAttempted = false, jobOK = false;
    if (ok && level == 5) {
        jobAttempted = true;
        IOLog("J7 G3D: running one WRITE_VALUE job on JS1 (AS0, private 16 KiB page table)\n");
        jobOK = a5g3d_try_write_value(service, gpu, storage);
        IOLog("J7 G3D: write-value job %s\n", jobOK ? "COMPLETED, output zeroed" : "FAILED or timed out");
    } else if (ok && level >= 6) {
        jobAttempted = true;
        jobOK = j7m_selftest(service, gpu);
    }

    /* Always try to leave the cores off again, shader first, L2 last. */
    const struct { const char *name; uint32_t off, ready, mask; } down[] = {
        { "shader", 0x1c0, 0x140, shader },
        { "tiler", 0x1d0, 0x150, tiler },
        { "L2", 0x1e0, 0x160, l2 },
    };
    bool off_ok = true;
    for (const auto &step : down) {
        uint32_t ready = 0;
        a5g3d_write32(gpu, step.off, step.mask);
        bool off = j7g3d_wait_ready(gpu, step.ready, step.mask, false, 20000, &ready);
        IOLog("J7 G3D: power off %s ready=%08x %s\n", step.name, ready, off ? "OK" : "TIMEOUT");
        off_ok = off_ok && off;
    }
    a5g3d_write32(gpu, 0x24, 0xffffffffu);
    if (jobAttempted && !jobOK) {
        /* Clear AS0 and any stuck job slot before leaving the GPU alone. */
        a5g3d_write32(gpu, 0x30, 1);
        uint32_t rr = 0;
        for (unsigned t = 0; t < 100000; t += 10) {
            rr = a5g3d_read32(gpu, 0x20);
            if (rr & (1u << 8)) {
                break;
            }
            IODelay(10);
        }
        IOLog("J7 G3D: cleanup soft reset rawstat=%08x\n", rr);
        a5g3d_write32(gpu, 0x24, 0xffffffffu);
    }
    service->setProperty("J7G3DPowerResult", ok ? (off_ok ? "power-cycle-ok" : "powered-on-off-timeout") :
        "power-on-timeout");
    IOLog("J7 G3D: core power test %s\n", ok ? (off_ok ? "PASSED" : "on OK, off timed out") : "FAILED");
}

static void
j7g3d_probe(IOService *service, int level, IOBufferMemoryDescriptor *&storage)
{
    vm_offset_t pmu = ml_io_map_unmappable(J7G3D_PMU_PAGE, 0x1000, VM_WIMG_IO);
    if (!pmu) {
        service->setProperty("J7G3DProbeResult", "pmu-map-failed");
        return;
    }
    /*
     * Do NOT sweep this page: the PMU has holes that raise a synchronous external abort
     * (the first J7 trial panicked reading PMU+404c, fault_code 0x10, far = page + 0x4c).
     * Read only the two registers this probe needs.
     */
    uint32_t config = a5g3d_read32(pmu, 0x60);
    uint32_t status = a5g3d_read32(pmu, 0x64);
    service->setProperty("J7G3DPMUConfig", static_cast<uint64_t>(config), 32);
    service->setProperty("J7G3DPMUStatus", static_cast<uint64_t>(status), 32);
    IOLog("J7 G3D: PMU G3D config=%08x status=%08x\n", config, status);
    if ((status & 0xf) != 0xf) {
        service->setProperty("J7G3DProbeResult", "power-off-or-unknown-status");
        IOLog("J7 G3D: PMU status low nibble is not 0xf; CMU and GPU untouched\n");
        ml_io_unmap(pmu, 0x1000);
        return;
    }

    vm_offset_t cmu = ml_io_map_unmappable(J7G3D_CMU_BASE, 0x1000, VM_WIMG_IO);
    if (!cmu) {
        service->setProperty("J7G3DProbeResult", "cmu-map-failed");
        ml_io_unmap(pmu, 0x1000);
        return;
    }
    /*
     * The CMU page has holes too: the second J7 trial read CMU+000 fine (0x79e) and then
     * panicked on CMU+004 (fault_code 0x10).  Read only that one known register.
     */
    uint32_t cmu0 = a5g3d_read32(cmu, 0);
    service->setProperty("J7G3DCMUWord0", static_cast<uint64_t>(cmu0), 32);
    IOLog("J7 G3D: CMU+000=%08x (page not swept)\n", cmu0);
    if (level < 3) {
        service->setProperty("J7G3DProbeResult", "pmu-on-cmu-word0");
        IOLog("J7 G3D: PMU says G3D is on; GPU aperture untouched (use j7gpu=3 to read GPU_ID)\n");
        ml_io_unmap(cmu, 0x1000);
        ml_io_unmap(pmu, 0x1000);
        return;
    }

    if ((a5g3d_read32(pmu, 0x64) & 0xf) != 0xf) {
        service->setProperty("J7G3DProbeResult", "power-dropped");
        ml_io_unmap(cmu, 0x1000);
        ml_io_unmap(pmu, 0x1000);
        return;
    }
    /* Job and MMU registers live at +0x1000..+0x2fff: map up to +0x2fff only for j7gpu=5. */
    const vm_size_t gpuSize = level >= 5 ? 0x3000 : 0x1000;
    vm_offset_t gpu = ml_io_map_unmappable(J7G3D_GPU_BASE, gpuSize, VM_WIMG_IO);
    if (!gpu) {
        service->setProperty("J7G3DProbeResult", "gpu-map-failed");
        ml_io_unmap(cmu, 0x1000);
        ml_io_unmap(pmu, 0x1000);
        return;
    }
    bool serving = false;
    IOLog("J7 G3D: reading GPU_ID now\n");
    uint32_t id = a5g3d_read32(gpu, 0);
    uint32_t product = id >> 16;
    service->setProperty("J7G3DGPUId", static_cast<uint64_t>(id), 32);
    service->setProperty("J7G3DProduct", static_cast<uint64_t>(product), 32);
    service->setProperty("J7G3DProbeResult", product == 0x830 ? "mali-t830" : "unexpected-id");
    IOLog("J7 G3D: passive GPU_ID=%08x product=%04x (expected 0830); no GPU jobs\n", id, product);
    if (product == 0x830) {
        /* Standard Midgard read-only identity/feature registers (all implemented, no holes). */
        static const struct { uint32_t off; const char *name; } regs[] = {
            { 0x004, "L2_FEATURES" }, { 0x00c, "TILER_FEATURES" }, { 0x010, "MEM_FEATURES" },
            { 0x014, "MMU_FEATURES" }, { 0x018, "AS_PRESENT" }, { 0x01c, "JS_PRESENT" },
            { 0x034, "GPU_STATUS" }, { 0x0a0, "THREAD_MAX_THREADS" }, { 0x0ac, "THREAD_FEATURES" },
            { 0x0b0, "TEXTURE_FEATURES_0" }, { 0x0c0, "JS0_FEATURES" },
            { 0x100, "SHADER_PRESENT_LO" }, { 0x110, "TILER_PRESENT_LO" }, { 0x120, "L2_PRESENT_LO" },
            { 0x140, "SHADER_READY_LO" }, { 0x160, "L2_READY_LO" },
        };
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            uint32_t v = a5g3d_read32(gpu, regs[i].off);
            IOLog("J7 G3D: %s(+%03x)=%08x\n", regs[i].name, regs[i].off, v);
        }
        if (level >= 7) {
            /* Keep the GPU mapped and powered: user clients use it from now on. */
            serving = j7m_serve_start(service, gpu);
            if (serving) {
                (void)j7m_client_selftest(service);
            }
        } else if (level >= 4) {
            j7g3d_power_test(service, gpu, level, storage);
        }
    }
    if (!serving) {
        ml_io_unmap(gpu, gpuSize);
    }
    ml_io_unmap(cmu, 0x1000);
    ml_io_unmap(pmu, 0x1000);
}

class A5XELTEGPU : public IOService {
    OSDeclareDefaultStructors(A5XELTEGPU);

    /* Keep GPU-visible pages alive until reboot, including on a job timeout. */
    IOBufferMemoryDescriptor *fTestMemory = nullptr;

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(A5XELTEGPU, IOService);

bool
A5XELTEGPU::start(IOService *provider)
{
    int enabled = 0;
    int j7 = 0;
    if (PE_parse_boot_argn("j7gpu", &j7, sizeof(j7)) && j7 >= 1 && j7 <= 7) {
        if (!IOService::start(provider)) {
            return false;
        }
        j7g3d_publish(this, j7);
        registerService();
        IOLog("J7 G3D: resource service registered (level %d); GPU power unchanged; CPU display active\n", j7);
        if (j7 >= 2) {
            j7g3d_probe(this, j7, fTestMemory);
        }
        return true;
    }
    if (!PE_parse_boot_argn("a5gpu", &enabled, sizeof(enabled)) ||
        (enabled != 1 && enabled != 2 && enabled != 3)) {
        return false; /* a5gpu=0, the safe default */
    }
    if (!IOService::start(provider)) {
        return false;
    }

    /* These are physical addresses from the working Exynos7580 Linux DT.
     * The matching ADT node uses arm-io-relative addresses. This service
     * only publishes numbers; it does not map these ranges.
     */
    setProperty("A5G3DMMIOBase", 0x11400000ULL, 64);
    setProperty("A5G3DMMIOSize", 0x5000ULL, 64);
    setProperty("A5G3DCMUBase", 0x11460000ULL, 64);
    setProperty("A5G3DCMUSize", 0x1000ULL, 64);
    setProperty("A5G3DPMUBase", 0x10460000ULL, 64);
    setProperty("A5G3DPMUSize", 0x5000ULL, 64);
    setProperty("A5G3DJobSPI", 282ULL, 32);
    setProperty("A5G3DMMUSPI", 283ULL, 32);
    setProperty("A5G3DGpuSPI", 281ULL, 32);
    setProperty("A5G3DDriverStage", enabled == 3 ? "offscreen-write-value" :
        (enabled == 2 ? "passive-id-probe" : "resources-only"));
    registerService();
    IOLog("A5XELTE G3D: resource service registered; GPU power unchanged; CPU display active\n");
    if (enabled >= 2) {
        a5g3d_probe_existing_power(this, enabled == 3, fTestMemory);
    }
    return true;
}
#endif /* ARM64_BOARD_CONFIG_A5XELTE */
