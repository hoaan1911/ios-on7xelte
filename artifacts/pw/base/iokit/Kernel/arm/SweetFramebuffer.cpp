/*
 * Built-in IOMobileFramebuffer for SWEET: the ABL's continuous-splash
 * framebuffer (1080x2400 a8r8g8b8 in RAM, the panel scans it out by
 * itself) as the main display, driven by CoreAnimation's CPU renderer.
 *
 * IOMobileFramebuffer.framework (iOS 16.7) counts /arm-io children with
 * device_type "display-subsystem", waits for that many services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider. The class
 * here (AppleCLCD, an IOMobileFramebuffer) matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off (backboardd, power
 *      button / auto-lock), else on. Off fades the panel out over a few
 *      vsyncs (halving each pixel) and keeps it black until on again;
 *      the panel itself stays powered (AMOLED black pixels are off).
 *   18 IsMainDisplay,
 *   28 GetDotPitch, 72 Enable/DisableNotifications [func, refcon, type, 0]
 * Everything else is accepted and logged once.
 *
 * SwapEnd copies the enabled layers' surfaces into the panel memory (no
 * scaling, CPU copy). Vsync (notification type 5) is a 60 Hz timer that
 * sends (count, mach_absolute_time, 0) as 32-bit halves to clients that
 * enabled it, through the port they registered (IOConnectSetNotificationPort).
 *
 * The panel memory itself is published as an IOSurface and returned as the
 * default framebuffer surface. The kernel text console keeps drawing over
 * the GUI by default: boot-arg sweetfb_noconsole=1 stops drawing it from
 * the first swap on (it still reaches the persistent log; panics draw).
 */
#include "SweetSurface.h"

extern "C" int sweet_console_muted;     /* osfmk/console/serial_console.c */
/* iokit/Kernel/arm/A5XELTEGPU.cpp: per-window submit/cache/wait counters, cleared by the read. */
extern "C" unsigned j7m_cpu_mhz(void);

/* scaledCopy(): row build (index, background) versus panel stores, nanoseconds */
static uint64_t sweet_fb_build_ns, sweet_fb_store_ns;
/* j7fb_band=0 stores one panel row at a time instead of four. */
static int sweet_fb_band = 1;
/* set by scaledCopy(): pixels the client left fully transparent in the last present */
static uint32_t sweet_fb_alpha0;
static uint32_t sweet_fb_alpha0_logged;
extern "C" void j7m_take_stats(uint64_t *submits, uint64_t *waits, uint64_t *waitUs,
    uint64_t *storeUs, uint64_t *flushUs);
/* iokit/Kernel/arm/SweetSurface.cpp: wrap selectors backboardd asked for, cleared by the read. */
extern "C" unsigned sweet_surface_take_wrap_calls(void);
/* iokit/Kernel/arm/SweetSurface.cpp: the panel mapping's cache mode, and the clean that
 * has to go with a cacheable one. */
extern "C" int sweet_surface_panel_cacheable;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
extern "C" void flush_dcache64(addr64_t addr, unsigned length, boolean_t phys);
#endif
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
extern "C" uint64_t a5xelte_power_pressed_at;   /* bsd/dev/a5xelte_keys.c */
#endif

#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <sys/proc.h>
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <sys/vfs_context.h>
#endif
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#include "j7wall_data.h"    /* tools/j7wall.py output: the picture below, RGB565 */
#endif

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
static void sweet_fb_copy_init(void);
#endif

enum {
	kSweetFBGetLayerDefaultSurface  = 3,
	kSweetFBSwapBegin               = 4,
	kSweetFBSwapEnd                 = 5,
	kSweetFBSwapWait                = 6,
	kSweetFBGetID                   = 7,
	kSweetFBGetDisplaySize          = 8,
	kSweetFBRequestPowerChange      = 12,
	kSweetFBIsMainDisplay           = 18,
	kSweetFBGetDotPitch             = 28,
	kSweetFBNotifications           = 72,
};

#define SWEET_FB_SWAP_REC_SIZE          0x46c
#define SWEET_FB_LAYERS                 4
#define SWEET_FB_NOTIFY_TYPES           8
#define SWEET_FB_NOTIFY_VSYNC           5
#define SWEET_FB_VSYNC_US               16667
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
#define SWEET_FB_DOT_PITCH              A5XELTE_FB_DOT_PITCH
#else
#define SWEET_FB_DOT_PITCH              458     /* ppi, as the iPhone X panel */
#endif
/*
 * The iOS userland only has status bar / layout data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", the red
 * rdar://45025538 bar for 360x800 points). The display is therefore
 * presented as the iPhone X panel, 1125x2436 (375x812 points @3x), and the
 * centered 1080x2400 part of it is shown on the 1080x2400 panel.
 * Boot-arg sweetfb_native=1 presents the real panel size instead.
 */
#define SWEET_FB_LOGICAL_W              1125
#define SWEET_FB_LOGICAL_H              2436
#define SWEET_FB_FORMAT_BGRA            0x42475241

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

/*
 * SwapWait (selector 6) used to answer success at once, without waiting for
 * anything. SwapBegin / SwapEnd / SwapWait is the whole frame contract, and the
 * wait is what paces the render server; answering it instantly leaves the client
 * with no idea when a frame reached the panel. The 2026-11-13 log shows 22
 * frames committed in eleven minutes, so the UI only redraws on a state change
 * and never animates. These counters say which of the two it is.
 */
static uint64_t sweet_fb_swap_count;
static uint64_t sweet_fb_swap_waits;
static uint64_t sweet_fb_swap_wait_us;
static bool     sweet_fb_swap_pending;
static uint64_t sweet_fb_swap_wait_for;
static int      sweet_fb_nowait;

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uint32_t      fLogicalW, fLogicalH;     /* size reported to userland */
	int32_t       fOffX, fOffY;             /* logical origin of the panel */
	bool          fScaled;                  /* sweetfb_scale: logical = 2/3 panel, x1.5 */
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	SweetSurface *fSurface;         /* the panel memory as an IOSurface */
	SweetSurface *fCanvas;          /* what clients draw and swap into */
	uint32_t     *fShadow;          /* canvas as last presented, for row skipping */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[SWEET_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fFlushTicks;    /* vsync ticks between panel copies */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	bool          fPerfLog;       /* A5-only copy timing; never on the donor */
	uint64_t      fPerfFrames;
	uint64_t      fPerfBytes;
	uint64_t      fPerfNS;
	uint64_t      fPerfMaxNS;
	uint64_t      fPerfWindowStart;
#endif
	bool          fPowerOff;      /* RequestPowerChange(0) */
	uint32_t      fFadeStep;      /* fade-out steps done while off */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Double buffering: present() paints the buffer the panel is not scanning. */
	void         *fBackMem[2];    /* cacheable contiguous scanout buffers */
	uint64_t      fBackPhys[2];
	uintptr_t     fBack[2];
	uint32_t      fBackSize;
	int           fBackIndex;     /* the one on screen */
	int           fFlip;
	int           fFlipped;       /* the first finished frame also went to the loader's buffer */
	uintptr_t     fTarget;        /* what present() writes: back buffer, or the panel */
	volatile uint32_t *fDecon;    /* DECON-F window 0 */
#endif

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void waitForSwap(void);
	uint64_t present(const uint8_t *rec);
	uint64_t scaledCopy(uintptr_t src, uint32_t srcRowBytes, int32_t dx, int32_t dy,
	    int32_t w, int32_t h);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	void flip(void);
#endif
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[SWEET_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[SWEET_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
/*
 * Read the CPU cluster clocks, and leave them alone.
 *
 * The Exynos7870 is rated 1586 MHz, this tree has no cpufreq driver, and the bootloader leaves
 * both clusters at the 902 MHz operating point, so every perf window measured cpu_mhz=900.
 * Reading the clock is safe:
 *   CMU_CPUCL0 0x10900000, CMU_CPUCL1 0x10800000
 *     +0x100 PLL_CON0, M 16:25, P 8:13, S 0:2
 *     +0x400/+0x404 the two ARMCLK dividers, bits 0:2, value + 1
 *
 * Writing it is not, and 0380 found out how not.  A PLL cannot be reprogrammed while the cores
 * run from it, so the cores have to be moved off it first - and the switch gate and divider this
 * tree believed were at CMU_MIF +0x858/+0x458 are not where they are.  The boot after that patch
 * logged "CPUCL0 did not leave the PLL, left alone" and then the perf window reported
 * cpu_mhz=38: 2.4 s per frame, half a minute per touch.  That number comes from j7m_cpu_mhz(),
 * a timing loop, so the machine really was running at 38 MHz - far worse than the 902 MHz the
 * bootloader leaves.  Until the vendor pwrcal sequence is confirmed, read and report only.
 */
static void
j7_cpu_clock(unsigned khz)
{
	static const struct { uint64_t cmu; const char *name; } cpu[] = {
		{ 0x10900000ULL, "CPUCL0" },
		{ 0x10800000ULL, "CPUCL1" },
	};
	for (unsigned i = 0; i < sizeof(cpu) / sizeof(cpu[0]); i++) {
		IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
		    cpu[i].cmu, 0x1000, kIODirectionNone);
		IOMemoryMap *map = desc != NULL ? desc->map(kIOMapInhibitCache) : NULL;
		if (map == NULL) {
			IOLog("A5XELTE CPU: no %s map\n", cpu[i].name);
			OSSafeReleaseNULL(desc);
			continue;
		}
		volatile uint32_t *c = (volatile uint32_t *)map->getVirtualAddress();
		const uint32_t con0 = c[0x100 / 4];
		const uint32_t cm = (con0 >> 16) & 0x3ffu, cp = (con0 >> 8) & 0x3fu, cs = con0 & 7u;
		const uint64_t pll = (cp != 0 && cs < 7) ? 26000ULL * cm / ((uint64_t)cp << cs) : 0;
		const uint32_t d1 = c[0x400 / 4] & 7u, d2 = c[0x404 / 4] & 7u;
		const uint64_t arm = pll / ((uint64_t)(d1 + 1) * (uint64_t)(d2 + 1));
		IOLog("A5XELTE CPU: %s PLL_CON0=%08x M=%u P=%u S=%u -> %llu kHz, div %u/%u -> %llu kHz, left as the bootloader set it (asked for %u)\n",
		    cpu[i].name, con0, cm, cp, cs, pll, d1, d2, arm, khz);
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(desc);
	}
}
#endif

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
/*
 * Point window 0 at pa, the way the verified loader does it.
 *
 * SHADOWCON bit 8 protects window 0.  With it set, the base lands in a shadow register and only
 * moves when a trigger consumes it; the previous attempt wrote the base with the bit set and
 * asked for UPDATE, and the request was still pending a second later (the dump read UPDATE=1 and
 * ADD0 held the new address) - the window kept scanning the buffer it had, which the console
 * mute had already cleared, and the panel stayed black.  Clear the bit first: the base then
 * takes effect at the next frame start, between two whole frames, so the change is still atomic.
 * UPDATE goes after it because the loader writes it and it costs nothing when nothing is
 * shadowed.
 */
static void
j7_decon_scanout(volatile uint32_t *d, uint32_t pa)
{
	d[0x030 / 4] = d[0x030 / 4] & ~(1u << 8);
	d[0x880 / 4] = pa;
	d[0x710 / 4] = 1u;
	__asm__ volatile ("dsb sy" ::: "memory");
}
#endif

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* The loader's DECON handoff scans out this physical XRGB8888 buffer.
	 * It does not fill boot_args.Video, so PE_current_console is empty.
	 * Keep the physical address out of the donor path. */
	fPhys = A5XELTE_FB_PA;
	fWidth = A5XELTE_FB_WIDTH;
	fHeight = A5XELTE_FB_HEIGHT;
	fRowBytes = A5XELTE_FB_STRIDE;
#else
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
#endif
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		IOLog("SWEET FB: no boot framebuffer\n");
		return false;
	}
	int native = 0;
	PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));
	fLogicalW = fWidth;
	fLogicalH = fHeight;
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	/* iPhone X userland on the 1440x2560 panel: its 1125x2436 canvas is
	 * shown centered (fOffX/fOffY are negative), unscaled. */
	if (!native) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
	/* Experimental: the CPU renderer (no GPU) draws 960x1706, 2.25x fewer
	 * pixels than the panel, and present() scales it x1.5 (nearest). */
	int scale = 0;
	PE_parse_boot_argn("sweetfb_scale", &scale, sizeof(scale));
	if (scale) {
		fScaled = true;
		fLogicalW = fWidth * 2 / 3;
		fLogicalH = fHeight * 2 / 3;
	}
#elif defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Internal resolution (J7 Prime): the CPU renderer draws a smaller display and
	 * present() scales it to the panel (scaledCopy, nearest). sweetfb_scale=1 alone gives
	 * 2/3 of the panel; sweetfb_lw/sweetfb_lh pick the size, e.g. 750x1334, the iPhone 8
	 * screen (375x667 points at @2x). They must match the ADT canvas (mkadt_j7prime.py
	 * --iphone8). Without sweetfb_scale nothing changes: the display is the panel. */
	int scale = 0;
	PE_parse_boot_argn("sweetfb_scale", &scale, sizeof(scale));
	if (scale) {
		fScaled = true;
		fLogicalW = fWidth * 2 / 3;
		fLogicalH = fHeight * 2 / 3;
		uint32_t lw = 0, lh = 0;
		PE_parse_boot_argn("sweetfb_lw", &lw, sizeof(lw));
		PE_parse_boot_argn("sweetfb_lh", &lh, sizeof(lh));
		/* The canvas may be larger than the panel: the iPhone 8 Plus renders 1242x2208 and
		 * present() shrinks it to the panel.  Two panels' worth is the sanity bound. */
		if (lw >= 320 && lw <= fWidth * 2 && lh >= 480 && lh <= fHeight * 2) {
			fLogicalW = lw;
			fLogicalH = lh;
		}
	}
#else
	if (!native && fWidth <= SWEET_FB_LOGICAL_W && fHeight <= SWEET_FB_LOGICAL_H) {
		fLogicalW = SWEET_FB_LOGICAL_W;
		fLogicalH = SWEET_FB_LOGICAL_H;
	}
#endif
	fOffX = fScaled ? 0 : (int32_t)(fLogicalW - fWidth) / 2;
	fOffY = fScaled ? 0 : (int32_t)(fLogicalH - fHeight) / 2;
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	/* The default surface must have the presented size (a mismatch left
	 * every swap layer empty); scaled, it is the panel's top-left part.  It is a view of the panel
	 * frame buffer, so it stays clamped to the panel even when the presented size is larger. */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* j7fb_cache=1: map the panel cacheable and clean the band once. Off by default - the
	 * default-cache mapping of this physical range is device memory and faulted. */
	int panel_cache = 0;
	PE_parse_boot_argn("j7fb_cache", &panel_cache, sizeof(panel_cache));
	sweet_surface_panel_cacheable = panel_cache != 0;
#endif
	const uint32_t surfaceW = fScaled && fLogicalW < fWidth ? fLogicalW : fWidth;
	const uint32_t surfaceH = fScaled && fLogicalH < fHeight ? fLogicalH : fHeight;
	/* Map all fHeight panel rows even when the surface is only the presented (canvas) part of
	 * the panel: present() scales the canvas into every panel row and the clears below write
	 * fRowBytes * fHeight.  A 750x1334 canvas used to map 4320 * 1334 bytes here and the
	 * start() clear of the panel took a data abort one page past it. */
	fSurface = sweet_surface_create_physical_len(fPhys, surfaceW, surfaceH, fRowBytes,
	    SWEET_FB_FORMAT_BGRA, fHeight);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	/*
	 * Clients must not draw into the panel surface.  They did: selector 3 handed out a view of
	 * the scanout buffer, so half of CoreAnimation's frames were rendered straight into panel
	 * memory at canvas size, landed unscaled in the top-left corner, and present() skipped them
	 * because the layer surface was fSurface.  On the phone that is the flicker between a
	 * correctly scaled frame and a raw 750-wide one.  Hand out a canvas surface of our own
	 * instead - allocated and shared once, never remapped at another cache mode, because two
	 * mappings of the same page with different attributes corrupt the pmap PV lists.
	 */
	if (fScaled && fLogicalW > 0 && fLogicalH > 0) {
		uint32_t stride = (fLogicalW * 4 + 63) & ~63u;
		fCanvas = sweet_surface_create_owned(fLogicalW, fLogicalH, stride,
		    SWEET_FB_FORMAT_BGRA);
		fShadow = (uint32_t *)IOMallocData((vm_size_t)fLogicalW * fLogicalH * 4);
		if (fShadow != NULL) {
			bzero(fShadow, (vm_size_t)fLogicalW * fLogicalH * 4);
		}
		/* Without it every present rebuilds and restores all 1920 panel rows even when the
		 * frame did not change, which is most of a copy that measured 35 ms. */
		IOLog("A5XELTE FB: row shadow %s\n", fShadow != NULL ? "on" : "off");
		if (fCanvas == NULL) {
			IOLog("SWEET FB: no canvas surface, clients keep the panel\n");
		} else {
			IOLog("SWEET FB: canvas surface id %u %ux%u stride %u\n",
			    fCanvas->id, fLogicalW, fLogicalH, stride);
		}
	}
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("SWEET FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Mute the panel console here, not on the first swap: present() only clears the panel once,
	 * and with the 750x1334 canvas boot text reappeared along the bottom because the console kept
	 * drawing after that single clear.  The persistent eMMC log is unaffected, and panics still
	 * draw. */
	int noconsole = 0;
	if (PE_parse_boot_argn("a5fb_noconsole", &noconsole, sizeof(noconsole)) && noconsole != 0) {
		sweet_console_muted = 1;
		memset((void *)fPixels, 0, (size_t)fRowBytes * fHeight);
		IOLog("A5XELTE FB: console muted at start, panel cleared\n");
	}
	{
		/*
		 * Two scanout buffers instead of the loader's one.
		 *
		 * The panel scans 0x67000000 continuously, and every store present() made
		 * landed under that live scanout: that is the tearing, and it is also why the
		 * copy cost 34 ms - a write-combining mapping charges every store on its own
		 * (about 15 cycles a word), which is store latency, not memory bandwidth.
		 *
		 * Allocate two contiguous buffers in normal cacheable memory, paint the one
		 * that is not on screen, clean it once, and point the display controller's
		 * window base at it.  The panel never sees a half-written frame, and the CPU
		 * writes at cache speed.  j7fb_flip=0 keeps the loader's single buffer.
		 */
		int flip = 1;
		PE_parse_boot_argn("j7fb_flip", &flip, sizeof(flip));
		fFlip = flip != 0 && fScaled;
		fTarget = fPixels;
		if (fFlip) {
			vm_size_t size = ((vm_size_t)fRowBytes * fHeight + 0x7fffu) & ~(vm_size_t)0x7fffu;
			bool made = true;
			fBackMem[0] = fBackMem[1] = NULL;
			for (unsigned i = 0; i < 2; i++) {
				uint64_t pa = 0;
				fBackMem[i] = IOMallocContiguous(size, 0x8000, &pa);
				if (fBackMem[i] == NULL) {
					made = false;
					break;
				}
				fBack[i] = (uintptr_t)fBackMem[i];
				fBackPhys[i] = pa;
				bzero(fBackMem[i], size);
			}
			if (!made) {
				fFlip = 0;
				IOLog("A5XELTE FB: no flip, no scanout buffer\n");
				for (unsigned i = 0; i < 2; i++) {
					if (fBackMem[i] != NULL) {
						IOFreeContiguous(fBackMem[i], size);
						fBackMem[i] = NULL;
					}
				}
			} else {
				fBackSize = (uint32_t)size;
				fBackIndex = 0;
				IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
				    0x14830000ULL, 0x1000, kIODirectionNone);
				IOMemoryMap *map = desc != NULL ? desc->map(kIOMapInhibitCache) : NULL;
				if (map == NULL) {
					fFlip = 0;
					IOLog("A5XELTE FB: no flip, no display controller map\n");
					for (unsigned i = 0; i < 2; i++) {
						IOFreeContiguous(fBackMem[i], size);
						fBackMem[i] = NULL;
					}
				} else {
					fDecon = (volatile uint32_t *)map->getVirtualAddress();
					fFlipped = 0;
					j7_decon_scanout(fDecon, (uint32_t)fBackPhys[0]);
					IOLog("A5XELTE FB: double buffer on, scanout 0x%llx and 0x%llx, ADD0 now %08x SHADOWCON %08x\n",
					    fBackPhys[0], fBackPhys[1], fDecon[0x880 / 4], fDecon[0x030 / 4]);
				}
			}
		} else {
			IOLog("A5XELTE FB: double buffer off, the panel keeps 0x%llx\n", fPhys);
		}
	}
#endif
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("SWEET FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 10;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* A5's 1080x1920 panel copies 8.29 MB per full frame. Keep the
	 * donor's 10 Hz default while allowing 30 Hz on this board. */
	hz = 30;
#endif
	PE_parse_boot_argn("sweetfb_hz", &hz, sizeof(hz));
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	PE_parse_boot_argn("a5fb_hz", &hz, sizeof(hz));
	int perf = 1;
	PE_parse_boot_argn("a5fb_perf", &perf, sizeof(perf));
	fPerfLog = perf != 0;
	fPerfFrames = fPerfBytes = fPerfNS = fPerfMaxNS = 0;
	fPerfWindowStart = mach_absolute_time();
#endif
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	if (hz == 0) {
		hz = 30;
	} else if (hz > 60) {
		hz = 60;
	}
	/* Round the tick interval up so 31-59 Hz never silently means 60 Hz. */
	fFlushTicks = (60 + hz - 1) / hz;
#else
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
#endif
	/* The IOKit object is not zero-filled and none of these are constructor-initialized.  A
	 * fConsolePaused that happened to read true skipped the console mute for the whole boot, which
	 * is why the panel kept the boot log at the bottom after a5fb_noconsole=1. */
	fConsolePaused = false;
	fDirty = false;
	fPowerOff = false;
	fFadeStep = 0;
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	sweet_fb_copy_init();
#endif
	fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("SWEET FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, presented as %ux%u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fLogicalW, fLogicalH);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	{
		int band = 1;
		if (PE_parse_boot_argn("j7fb_band", &band, sizeof(band))) {
			sweet_fb_band = band != 0;
		}
		IOLog("A5XELTE FB: row bands %s\n", sweet_fb_band ? "on" : "off");
	}
	{
		/* The display controller the loader left running is what scans the panel memory
		 * this class writes, and every write lands under a live scanout: that is the
		 * tearing. Read its window 0 state before touching it, so a double buffer and a
		 * flip at vsync can be programmed against real values instead of guesses. */
		int decon = 1;
		/* Default on: PE_parse_boot_argn() returns false when the argument is absent, so
		 * "parsed && decon" left the dump off on every boot that did not pass it. */
		PE_parse_boot_argn("j7decon", &decon, sizeof(decon));
		if (decon != 0) {
			IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
			    0x14830000ULL, 0x1000, kIODirectionNone);
			IOMemoryMap *map = desc != NULL ? desc->map(kIOMapInhibitCache) : NULL;
			if (map != NULL) {
				volatile uint32_t *d = (volatile uint32_t *)map->getVirtualAddress();
				IOLog("A5XELTE DECON: VIDCON0=%08x VIDCON1=%08x SHADOWCON=%08x WINCHMAP0=%08x\n",
				    d[0x000 / 4], d[0x004 / 4], d[0x030 / 4], d[0x040 / 4]);
				IOLog("A5XELTE DECON: WINCON0=%08x WHOLE_X=%u WHOLE_Y=%u OFF_X=%u OFF_Y=%u\n",
				    d[0x050 / 4], d[0x130 / 4], d[0x134 / 4], d[0x170 / 4], d[0x174 / 4]);
				IOLog("A5XELTE DECON: OSDA=%08x OSDB=%08x ADD0=%08x UPDATE=%08x TRIGCON=%08x\n",
				    d[0x230 / 4], d[0x234 / 4], d[0x880 / 4], d[0x710 / 4], d[0x6b0 / 4]);
			} else {
				IOLog("A5XELTE DECON: no map\n");
			}
			OSSafeReleaseNULL(map);
			OSSafeReleaseNULL(desc);
		}
	}
	{
		/* j7cpu=<kHz> is only reported.  The clock is not written: the switch away from
		 * the PLL is not at the offsets this tree used and the attempt left the whole
		 * machine at 38 MHz (see j7_cpu_clock).  A small value is MHz. */
		int cpu = 1794;
		PE_parse_boot_argn("j7cpu", &cpu, sizeof(cpu));
		if (cpu > 0 && cpu < 100000) {
			cpu *= 1000;
		}
		if (cpu > 0) {
			j7_cpu_clock((unsigned)cpu);
		}
	}
	{
		/* j7clk=1 reads - and only reads - the clock controller bank the mainline driver
		 * documents for this SoC (CMU_MIF 0x10460000, PLL_CON0 of the memory, media and
		 * bus PLLs at +0x100/+0x120/+0x140). The CPU has been measured at 900 MHz against
		 * a rated 1586 MHz, and no PLL or divider may be written until the M/P/S values
		 * the bootloader left are known. Off by default: an address in an unmapped part
		 * of a CMU bank raises a synchronous abort, as the G3D probe found. */
		int clk = 0;
		if (PE_parse_boot_argn("j7clk", &clk, sizeof(clk)) && clk != 0) {
			IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
			    0x10460000ULL, 0x1000, kIODirectionNone);
			IOMemoryMap *map = desc != NULL ? desc->map(kIOMapInhibitCache) : NULL;
			if (map != NULL) {
				volatile uint32_t *c = (volatile uint32_t *)map->getVirtualAddress();
				static const struct { uint32_t off; const char *name; } plls[] = {
					{ 0x100, "MEM" }, { 0x120, "MEDIA" }, { 0x140, "BUS" },
				};
				IOLog("A5XELTE CLK: CMU_MIF word0=%08x\n", c[0]);
				for (unsigned i = 0; i < sizeof(plls) / sizeof(plls[0]); i++) {
					uint32_t con0 = c[plls[i].off / 4];
					uint32_t m = (con0 >> 16) & 0x3ff;
					uint32_t pd = (con0 >> 8) & 0x3f;
					uint32_t s = con0 & 0x7;
					uint64_t khz = 0;
					if (pd != 0 && s < 7) {
						khz = 26000ULL * m / ((uint64_t)pd << s);
					}
					IOLog("A5XELTE CLK: %s PLL_CON0=%08x M=%u P=%u S=%u en=%u -> %llu kHz\n",
					    plls[i].name, con0, m, pd, s, (con0 >> 31) & 1, khz);
				}
				/* Never walk a CMU bank. 0x10460004 is unimplemented, and a read of it is a
				 * synchronous external abort (esr 0x96000210, far = base + 4) that takes the
				 * machine down before it can log, which is exactly what a full 4 KB dump did.
				 * Read only the offsets the mainline driver defines. */
				static const struct { uint32_t off; const char *name; } mif[] = {
					{ 0x200, "GAT_MUX_MEM_PLL" }, { 0x204, "GAT_MUX_MEDIA_PLL" },
					{ 0x208, "GAT_MUX_BUS_PLL" }, { 0x220, "MUX_BUSD" },
					{ 0x420, "DIV_BUSD" }, { 0x424, "DIV_APB" },
					{ 0x460, "DIV_G3D_SWITCH" },
				};
				for (unsigned i = 0; i < sizeof(mif) / sizeof(mif[0]); i++) {
					IOLog("A5XELTE CLK: MIF %s=%08x\n", mif[i].name, c[mif[i].off / 4]);
				}
			} else {
				IOLog("A5XELTE CLK: no map\n");
			}
			/* CMU_G3D (0x11460000), documented offsets only. The GPU core runs from this
			 * PLL through CLK_CON_DIV_G3D_BUS, so both numbers are needed before its
			 * frequency can be raised: the divider is 0-based, result = pll / (div + 1). */
			IOMemoryDescriptor *gdesc = IOMemoryDescriptor::withPhysicalAddress(
			    0x11460000ULL, 0x1000, kIODirectionNone);
			IOMemoryMap *gmap = gdesc != NULL ? gdesc->map(kIOMapInhibitCache) : NULL;
			if (gmap != NULL) {
				volatile uint32_t *g = (volatile uint32_t *)gmap->getVirtualAddress();
				uint32_t con0 = g[0x100 / 4];
				uint32_t gm = (con0 >> 16) & 0x3ff;
				uint32_t gp = (con0 >> 8) & 0x3f;
				uint32_t gs = con0 & 0x7;
				uint32_t div = g[0x400 / 4] & 0x7u;
				uint64_t pll = (gp != 0 && gs < 7) ? 26000ULL * gm / ((uint64_t)gp << gs) : 0;
				IOLog("A5XELTE CLK: G3D PLL_CON0=%08x M=%u P=%u S=%u en=%u -> %llu kHz\n",
				    con0, gm, gp, gs, (con0 >> 31) & 1, pll);
				IOLog("A5XELTE CLK: G3D MUX=%08x DIV_BUS=%08x DIV_APB=%08x -> %llu kHz\n",
				    g[0x208 / 4], g[0x400 / 4], g[0x404 / 4], pll / (uint64_t)(div + 1));
				/* The core runs at pll / (div + 1) and the divider is already 0, so the
				 * only way up is the PLL itself: 343 MHz, against a T830 rated far higher.
				 * j7gpuclk=<kHz> reprograms PLL_CON0 - M 343 -> 900 at the same P 13, S 1,
				 * which is 1800 MHz in the VCO and 900 out. The top byte of CON0 is kept
				 * because what those bits mean is not documented. */
				int want = 0;
				if (PE_parse_boot_argn("j7gpuclk", &want, sizeof(want)) && want >= 200 &&
				    want <= 1000 && (uint32_t)want != pll) {
					uint32_t gp_p = (gp != 0) ? gp : 13u;
					uint32_t gp_s = gs;
					/* Fout = 26 MHz * M / (P << S), so M = Fout * (P << S) / 26 MHz.
					 * The first version multiplied by P instead of dividing, wrote M = 0,
					 * and took the core from 343 MHz down to 256: the register read back
					 * as a1000d01 with an empty M field. j7gpuclk is in MHz. */
					uint64_t m64 = ((uint64_t)want * 1000000ULL *
					    ((uint64_t)gp_p << gp_s)) / 26000000ULL;
					if (m64 < 10 || m64 > 1023) {
						IOLog("A5XELTE CLK: G3D %d MHz needs M=%llu, out of range\n",
						    want, m64);
					} else {
						uint32_t m = (uint32_t)m64;
						uint32_t con0new = (con0 & 0xff000000u) | ((m & 0x3ffu) << 16) |
						    ((gp_p & 0x3fu) << 8) | (gp_s & 0x7u);
						IOLog("A5XELTE CLK: G3D %d MHz -> M=%u P=%u S=%u CON0 %08x -> %08x "
						    "(VCO %llu kHz)\n", want, m, gp_p, gp_s, con0, con0new,
						    26000ULL * m / gp_p);
						g[0x100 / 4] = con0new;
						IODelay(200);
						uint32_t back = g[0x100 / 4];
						uint32_t bm = (back >> 16) & 0x3ff;
						uint64_t now = (gp_p != 0) ?
						    26000ULL * bm / ((uint64_t)gp_p << gp_s) : 0;
						IOLog("A5XELTE CLK: G3D reads back %08x -> %llu kHz\n",
						    back, now);
					}
				}
			} else {
				IOLog("A5XELTE CLK: no G3D map\n");
			}
			OSSafeReleaseNULL(gmap);
			OSSafeReleaseNULL(gdesc);
			OSSafeReleaseNULL(map);
			OSSafeReleaseNULL(desc);
		}
	}
	{
		int nowait = 0;
		if (PE_parse_boot_argn("j7fb_nowait", &nowait, sizeof(nowait))) {
			sweet_fb_nowait = nowait != 0;
		}
	}
	IOLog("A5XELTE FB: requested %u Hz, effective %u Hz, copy timing %s, swap wait %s\n",
	    hz, 60 / fFlushTicks, fPerfLog ? "on" : "off", sweet_fb_nowait ? "off" : "on");
#endif
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(SWEET_FB_NOTIFY_VSYNC, values, 6);
	}
	if (fb->fPowerOff) {
		/* Fade out: halve every channel each vsync, then black, then idle. */
		uint32_t step = fb->fFadeStep;
		if (step <= 6) {
			fb->fFadeStep = step + 1;
		}
		IOLockUnlock(fb->fLock);
		if (step <= 6) {
			/* The scanout buffer, not the loader's: that one is not on screen once
			 * the display is double buffered. */
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
			uintptr_t fade = fb->fFlip ? fb->fBack[fb->fBackIndex] : fb->fPixels;
#else
			uintptr_t fade = fb->fPixels;
#endif
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fade + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
			flush_dcache64((addr64_t)fade, fb->fBackSize ? fb->fBackSize : fb->fRowBytes * fb->fHeight, FALSE);
			__asm__ volatile ("dsb sy" ::: "memory");
#endif
		}
		sender->setTimeoutUS(SWEET_FB_VSYNC_US);
		return;
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	uint8_t *rec = NULL;
	if (flush) {
		rec = (uint8_t *)IOMallocData(SWEET_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, SWEET_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		uint64_t before = fb->fPerfLog ? mach_absolute_time() : 0;
#endif
		uint64_t bytes = fb->present(rec);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		if (fb->fPerfLog) {
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - before, &ns);
			fb->fPerfFrames++;
			fb->fPerfBytes += bytes;
			fb->fPerfNS += ns;
			if (ns > fb->fPerfMaxNS) {
				fb->fPerfMaxNS = ns;
			}
		}
#else
		(void)bytes;
#endif
		IOFreeData(rec, SWEET_FB_SWAP_REC_SIZE);
	}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Aggregate after 300 callbacks; measure wall time because expensive
	 * copies can delay the timer and reduce the actual frame rate. */
	if (fb->fPerfLog && (count % 300) == 0) {
		uint64_t elapsedNS = 0;
		uint64_t finished = mach_absolute_time();
		absolutetime_to_nanoseconds(finished - fb->fPerfWindowStart, &elapsedNS);
		uint64_t submits = 0, waits = 0, waitUs = 0, storeUs = 0, flushUs = 0;
		j7m_take_stats(&submits, &waits, &waitUs, &storeUs, &flushUs);
		unsigned wraps = sweet_surface_take_wrap_calls();
		if (fb->fPerfFrames != 0 && elapsedNS != 0) {
			uint64_t fpsTenths = fb->fPerfFrames * 10000000000ULL / elapsedNS;
			IOLog("A5XELTE FB: frames=%llu elapsed_ms=%llu fps=%llu.%llu bytes=%llu avg_copy_us=%llu max_copy_us=%llu\n",
			    fb->fPerfFrames, elapsedNS / 1000000, fpsTenths / 10,
			    fpsTenths % 10, fb->fPerfBytes,
			    fb->fPerfNS / fb->fPerfFrames / 1000, fb->fPerfMaxNS / 1000);
			IOLog("A5XELTE FB: submits=%llu wait_us=%llu store_us=%llu flush_us=%llu wrap=%u cpu_mhz=%u\n",
			    submits, waitUs, storeUs, flushUs, wraps, j7m_cpu_mhz());
			IOLog("A5XELTE FB: copy split build_us=%llu store_us=%llu\n",
			    sweet_fb_build_ns / 1000, sweet_fb_store_ns / 1000);
			sweet_fb_build_ns = sweet_fb_store_ns = 0;
			IOLog("A5XELTE FB: swap_ends=%llu swap_waits=%llu avg_swapwait_us=%llu\n",
			    sweet_fb_swap_count, sweet_fb_swap_waits,
			    sweet_fb_swap_waits ? sweet_fb_swap_wait_us / sweet_fb_swap_waits : 0);
			sweet_fb_swap_waits = 0;
			sweet_fb_swap_wait_us = 0;
		}
		fb->fPerfFrames = fb->fPerfBytes = fb->fPerfNS = fb->fPerfMaxNS = 0;
		fb->fPerfWindowStart = finished;
	}
#endif
	sender->setTimeoutUS(SWEET_FB_VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t sweet_fb_swaps_logged;

#if defined(ARM64_BOARD_CONFIG_HEROLTE)
/*
 * A full 1440x2560 frame is 14.7 MB into write-combined memory: 10-20 ms
 * on one core (a5fb_perf). Helper threads copy row bands next to the
 * caller. Boot-arg sweetfb_copythreads=N helpers (default 3, 0 = off).
 */
#define SWEET_FB_COPY_MAX_HELPERS 7
struct sweet_fb_copy_job {
	uintptr_t       dst;
	uintptr_t       src;
	uint64_t        dstStride, srcStride;
	size_t          rowBytes;
	int32_t         rows;
};
static IOLock *sweet_fb_copy_lock;
static struct sweet_fb_copy_job sweet_fb_copy;
static uint32_t sweet_fb_copy_helpers;
static uint64_t sweet_fb_copy_generation;
static uint32_t sweet_fb_copy_pending;

static void
sweet_fb_copy_band(const struct sweet_fb_copy_job *job, uint32_t band, uint32_t bands)
{
	int32_t first = (int32_t)((int64_t)job->rows * band / bands);
	int32_t last = (int32_t)((int64_t)job->rows * (band + 1) / bands);
	for (int32_t y = first; y < last; y++) {
		memcpy((void *)(job->dst + (uint64_t)y * job->dstStride),
		    (const void *)(job->src + (uint64_t)y * job->srcStride), job->rowBytes);
	}
}

static void
sweet_fb_copy_helper(void *arg, wait_result_t wr __unused)
{
	uint32_t band = (uint32_t)(uintptr_t)arg;
	uint64_t seen = 0;
	for (;;) {
		IOLockLock(sweet_fb_copy_lock);
		while (sweet_fb_copy_generation == seen) {
			IOLockSleep(sweet_fb_copy_lock, &sweet_fb_copy_generation, THREAD_UNINT);
		}
		seen = sweet_fb_copy_generation;
		struct sweet_fb_copy_job job = sweet_fb_copy;
		IOLockUnlock(sweet_fb_copy_lock);

		sweet_fb_copy_band(&job, band, sweet_fb_copy_helpers + 1);

		IOLockLock(sweet_fb_copy_lock);
		if (--sweet_fb_copy_pending == 0) {
			IOLockWakeup(sweet_fb_copy_lock, &sweet_fb_copy_pending, false);
		}
		IOLockUnlock(sweet_fb_copy_lock);
	}
}

static void
sweet_fb_copy_init(void)
{
	uint32_t helpers = 3;
	PE_parse_boot_argn("sweetfb_copythreads", &helpers, sizeof(helpers));
	if (helpers > SWEET_FB_COPY_MAX_HELPERS) {
		helpers = SWEET_FB_COPY_MAX_HELPERS;
	}
	sweet_fb_copy_lock = IOLockAlloc();
	if (sweet_fb_copy_lock == NULL) {
		return;
	}
	uint32_t started = 0;
	for (uint32_t i = 0; i < helpers; i++) {
		thread_t thread;
		/* Band 0 is the caller's; helper i copies band i + 1. */
		if (kernel_thread_start(&sweet_fb_copy_helper, (void *)(uintptr_t)(i + 1), &thread) != KERN_SUCCESS) {
			break;
		}
		thread_deallocate(thread);
		started++;
	}
	sweet_fb_copy_helpers = started;
	IOLog("A5XELTE FB: parallel copy with %u helper threads\n", started);
}

/* Copies rows; returns after every band is done. */
static void
sweet_fb_copy_rows(uintptr_t dst, uint64_t dstStride, uintptr_t src, uint64_t srcStride,
    size_t rowBytes, int32_t rows)
{
	struct sweet_fb_copy_job job = { dst, src, dstStride, srcStride, rowBytes, rows };
	uint32_t helpers = sweet_fb_copy_helpers;
	if (helpers == 0 || rows < 64) {
		sweet_fb_copy_band(&job, 0, 1);
		return;
	}
	IOLockLock(sweet_fb_copy_lock);
	sweet_fb_copy = job;
	sweet_fb_copy_pending = helpers;
	sweet_fb_copy_generation++;
	IOLockWakeup(sweet_fb_copy_lock, &sweet_fb_copy_generation, false);
	IOLockUnlock(sweet_fb_copy_lock);

	sweet_fb_copy_band(&job, 0, helpers + 1);

	IOLockLock(sweet_fb_copy_lock);
	while (sweet_fb_copy_pending != 0) {
		IOLockSleep(sweet_fb_copy_lock, &sweet_fb_copy_pending, THREAD_UNINT);
	}
	IOLockUnlock(sweet_fb_copy_lock);
}
#endif

/* Copy each enabled layer's surface into the panel memory (no scaling). */
/*
 * A swap only records the frame: copying 10 MB into the panel on every
 * swap (backboardd presents at 60 Hz) took the whole CPU. The vsync timer
 * shows the latest frame every fFlushTicks ticks (boot-arg sweetfb_hz,
 * default 10 Hz).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	IOLockLock(fLock);
	memcpy(fPending, rec, SWEET_FB_SWAP_REC_SIZE);
	fDirty = true;
	IOLockUnlock(fLock);
	/* Wait for the very next tick, not the next present tick. A present costs about
	 * 34 ms and fFlushTicks is 2, so aiming at a multiple of fFlushTicks made the
	 * caller miss its own target and block for a second full cycle: the 2026-11-14
	 * log shows a 66 ms average wait on a 34 ms copy, which is most of a 70 ms
	 * frame. One tick is the shortest honest answer. */
	sweet_fb_swap_wait_for = fVsyncCount + 1;
	sweet_fb_swap_pending = true;
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
	/* Raw record of the first swaps: offset=value of every non-zero word. */
	if (sweet_fb_swap_count < 8) {
		char line[160];
		int n = 0;
		for (uint32_t off = 0; off + 4 <= SWEET_FB_SWAP_REC_SIZE; off += 4) {
			uint32_t v = get32(rec, off);
			if (v == 0) {
				continue;
			}
			n += snprintf(line + n, sizeof(line) - n, " %x=%x", off, v);
			if (n > (int)sizeof(line) - 24) {
				IOLog("HEROLTE FB: rec %llu%s\n", sweet_fb_swap_count, line);
				n = 0;
			}
		}
		IOLog("HEROLTE FB: rec %llu%s\n", sweet_fb_swap_count, n ? line : " end");
	}
#endif
	if ((++sweet_fb_swap_count % 1000) == 0) {
		IOLog("SWEET FB: %llu swaps\n", sweet_fb_swap_count);
	}
}

/*
 * Wait, once per recorded swap, for the tick that presents it. Bounded because
 * a frame that never arrives must not wedge the calling process, and skipped
 * entirely with j7fb_nowait=1 so the instant-answer behaviour stays reachable.
 */
void
AppleCLCD::waitForSwap(void)
{
	uint64_t startNs = 0, nowNs = 0, endNs = 0, target;
	bool pending;

	IOLockLock(fLock);
	pending = sweet_fb_swap_pending;
	target = sweet_fb_swap_wait_for;
	sweet_fb_swap_pending = false;
	IOLockUnlock(fLock);
	if (!pending || sweet_fb_nowait) {
		return;
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &startNs);
	while (fVsyncCount < target) {
		absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
		if (nowNs - startNs >= 100 * NSEC_PER_MSEC) {
			break;
		}
		IOSleep(1);
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &endNs);
	sweet_fb_swap_wait_us += (endNs - startNs) / 1000;
	sweet_fb_swap_waits++;
}

/*
 * Wallpaper. The swap records this driver is handed carry one opaque desktop layer and no
 * wallpaper layer at all, so the only way to show one is to supply the pixels: a raw BGRA
 * image at the logical display size, read once from the data volume (the root is the iOS
 * user data volume, rootrw=1). j7wall=<path> overrides /var/mobile/j7wall.raw.
 */
static uint32_t *sweet_fb_wall;
static int32_t sweet_fb_wall_w, sweet_fb_wall_h;
static int sweet_fb_wall_tried;
static uint64_t sweet_fb_wall_px;   /* pixels the last present took from the picture */
static int sweet_fb_wall_reports;
/* this frame's own background colour, 0 when it has none: see sweet_fb_dominant() */
static uint32_t sweet_fb_bg;
static int sweet_fb_bg_wanted;

/*
 * The picture is the logical display size and a swap record's layer is whatever the client says,
 * not always the same size: requiring an exact match meant no wallpaper was ever sampled (the
 * log said "took 0 background pixels" on a frame whose dominant colour was black).  Sample the
 * picture at the layer's pixel position instead, nearest neighbour.
 */
static inline uint32_t
sweet_fb_wall_at(int32_t c, int32_t r, int32_t w, int32_t h)
{
	if (sweet_fb_wall == NULL || sweet_fb_wall_w <= 0 || sweet_fb_wall_h <= 0 || w <= 0 || h <= 0) {
		return 0;
	}
	int64_t sx = (int64_t)c * sweet_fb_wall_w / w;
	int64_t sy = (int64_t)r * sweet_fb_wall_h / h;
	if (sx < 0) {
		sx = 0;
	} else if (sx >= sweet_fb_wall_w) {
		sx = sweet_fb_wall_w - 1;
	}
	if (sy < 0) {
		sy = 0;
	} else if (sy >= sweet_fb_wall_h) {
		sy = sweet_fb_wall_h - 1;
	}
	return sweet_fb_wall[(uint64_t)sy * (uint64_t)sweet_fb_wall_w + (uint64_t)sx];
}


#if defined(ARM64_BOARD_CONFIG_A5XELTE)
/*
 * Expand the compiled-in picture to the logical display size. Nothing is visible to a
 * computer yet, so the only thing that can reach the phone is the boot image: the
 * wallpaper travels inside the kernel instead of in a file the user would have to push.
 * The data is half size RGB565; nearest neighbour both ways, once, at first present.
 */
static void
sweet_fb_wall_embedded(uint32_t *dst, int32_t w, int32_t h)
{
	for (int32_t y = 0; y < h; y++) {
		int64_t sy = (int64_t)y * J7WALL_HEIGHT / h;
		const unsigned short *srow = j7wall_data + sy * J7WALL_WIDTH;
		uint32_t *drow = dst + (int64_t)y * w;
		int32_t sx = 0;
		for (int32_t x = 0; x < w; x++) {
			while (sx + 1 < J7WALL_WIDTH && (int64_t)(sx + 1) * w <= (int64_t)x * J7WALL_WIDTH) {
				sx++;
			}
			uint32_t v = srow[sx];
			uint32_t r = (v >> 11) & 0x1fu;
			uint32_t g = (v >> 5) & 0x3fu;
			uint32_t b = v & 0x1fu;
			drow[x] = 0xff000000u | (((r << 3) | (r >> 2)) << 16) |
			    (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
		}
	}
}

static void
sweet_fb_wall_load(int32_t w, int32_t h)
{
	char path[96] = "/var/mobile/j7wall.raw";
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULL;
	int32_t size = w * h * 4;

	sweet_fb_wall_tried = 1;
	(void)PE_parse_boot_argn("j7wall", path, sizeof(path));
	if (size <= 0) {
		return;
	}
	sweet_fb_wall = (uint32_t *)IOMallocData((vm_size_t)size);
	if (sweet_fb_wall == NULL) {
		IOLog("A5XELTE FB: no memory for a wallpaper\n");
		return;
	}
	if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0 || vp == NULL) {
		IOLog("A5XELTE FB: no wallpaper at %s, using the built-in picture\n", path);
		sweet_fb_wall_embedded(sweet_fb_wall, w, h);
		sweet_fb_wall_w = w;
		sweet_fb_wall_h = h;
		return;
	}
	int error = vn_rdwr(UIO_READ, vp, (caddr_t)sweet_fb_wall, size, 0, UIO_SYSSPACE,
	    IO_NODELOCKED | IO_NOCACHE, kauth_cred_get(), NULL, NULL);
	vnode_close(vp, FREAD, ctx);
	if (error != 0) {
		IOLog("A5XELTE FB: wallpaper read %d, using the built-in picture\n", error);
		sweet_fb_wall_embedded(sweet_fb_wall, w, h);
		sweet_fb_wall_w = w;
		sweet_fb_wall_h = h;
		return;
	}
	sweet_fb_wall_w = w;
	sweet_fb_wall_h = h;
	IOLog("A5XELTE FB: wallpaper %dx%d from %s\n", w, h, path);
}
#endif

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
/*
 * The colour the desktop paints behind everything.
 *
 * The fixed near-black test replaced nothing on this phone: the report line said "took 0
 * background pixels" on every frame, because the home screen, the lock screen and the app
 * switcher each paint their own dark background and none of them is (0,0,0).  Sample the layer
 * once per present, take the most common colour bucket, and accept it only when it covers a
 * fifth of the samples and is dark - which a wallpaper area is and an app's white page is not.
 */
static uint32_t
sweet_fb_dominant(uintptr_t src, uint32_t stride, int32_t w, int32_t h)
{
	static uint16_t bins[4096];
	unsigned total = 0, best = 0, seen = 0;
	uint64_t r = 0, g = 0, b = 0;

	bzero(bins, sizeof(bins));
	for (int32_t y = 0; y < h; y += 8) {
		const uint32_t *row = (const uint32_t *)(src + (uint64_t)y * stride);
		for (int32_t x = 0; x < w; x += 8) {
			const uint32_t v = row[x];
			if ((v >> 24) != 0xffu) {
				continue;
			}
			bins[((v >> 12) & 0xf00u) | ((v >> 8) & 0xf0u) | ((v >> 4) & 0x0fu)]++;
			total++;
		}
	}
	for (unsigned i = 1; i < 4096; i++) {
		if (bins[i] > bins[best]) {
			best = i;
		}
	}
	if (total == 0 || (unsigned)bins[best] * 5u < total) {
		return 0;
	}
	for (int32_t y = 0; y < h; y += 8) {
		const uint32_t *row = (const uint32_t *)(src + (uint64_t)y * stride);
		for (int32_t x = 0; x < w; x += 8) {
			const uint32_t v = row[x];
			if ((v >> 24) != 0xffu ||
			    (((v >> 12) & 0xf00u) | ((v >> 8) & 0xf0u) | ((v >> 4) & 0x0fu)) != best) {
				continue;
			}
			r += (v >> 16) & 0xffu;
			g += (v >> 8) & 0xffu;
			b += v & 0xffu;
			seen++;
		}
	}
	if (seen == 0) {
		return 0;
	}
	r /= seen;
	g /= seen;
	b /= seen;
	/* Only a dark background is replaced: a white app page must stay white. */
	if ((r * 299u + g * 587u + b * 114u) / 1000u > 0x40u) {
		return 0;
	}
	return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
#endif

/*
 * sweetfb_scale: the logical rect (dx,dy,w,h) at src goes to panel pixels
 * x1.5, nearest neighbour (two source pixels become three). A panel row
 * whose source row equals the previous one is copied from it. Called only
 * from present(), which the vsync timer serializes.
 */
uint64_t
AppleCLCD::scaledCopy(uintptr_t src, uint32_t srcRowBytes, int32_t dx, int32_t dy,
    int32_t w, int32_t h)
{
	static uint16_t column[2048];
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Once per present, from the layer that covers the display: its own background colour. */
	if (sweet_fb_bg_wanted && sweet_fb_wall != NULL && fLogicalW > 0 && fLogicalH > 0 &&
	    (int64_t)w * h * 2 >= (int64_t)fLogicalW * fLogicalH) {
		sweet_fb_bg = sweet_fb_dominant(src, srcRowBytes, w, h);
		sweet_fb_bg_wanted = 0;
	}
#endif
	/* One cacheable destination row. fPixels is write-combined panel memory, where a
	 * per-pixel store costs far more than filling a normal row and copying it once: the
	 * 750x1334 -> 1080x1920 frame measured 28 ms done pixel by pixel. */
	static uint32_t row[2048] __attribute__((aligned(16)));
	/* Four panel rows staged and stored as one contiguous run. fRowBytes is 4320, which is
	 * 67.5 cache lines: a row's stores start 32 bytes into a line and end 32 bytes into
	 * another, so a write-combining mapping flushes two partial lines per row. Four rows are
	 * 17280 bytes, 270 whole lines, and a band that starts on an even row is 64-byte aligned
	 * at both ends, so nothing is partial. */
	static uint32_t band[4][2048] __attribute__((aligned(64)));
	/* panel / logical ratio, whatever the logical size is (1.5 for 2/3 of the panel, 1.44 for 750x1334) */
	int32_t px0 = dx * (int32_t)fWidth / (int32_t)fLogicalW, px1 = (dx + w) * (int32_t)fWidth / (int32_t)fLogicalW;
	int32_t py0 = dy * (int32_t)fHeight / (int32_t)fLogicalH, py1 = (dy + h) * (int32_t)fHeight / (int32_t)fLogicalH;
	if (px1 > (int32_t)fWidth) {
		px1 = (int32_t)fWidth;
	}
	if (py1 > (int32_t)fHeight) {
		py1 = (int32_t)fHeight;
	}
	int32_t pw = px1 - px0;
	if (pw <= 0 || py1 <= py0 || pw > (int32_t)(sizeof(column) / sizeof(column[0]))) {
		return 0;
	}
	for (int32_t x = 0; x < pw; x++) {
		int32_t c = (px0 + x) * (int32_t)fLogicalW / (int32_t)fWidth - dx;
		column[x] = (uint16_t)(c < 0 ? 0 : (c >= w ? w - 1 : c));
	}
	int32_t previous = -1;
	uint64_t buildStart = 0, storeStart = 0;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* With a double buffer the row skip is wrong - the back buffer holds the frame from
	 * two presents ago, not what is on the panel - and the band staging is pointless,
	 * because the destination is cacheable memory where a row copy is one store per
	 * cache line instead of one store per word. */
	const uintptr_t target = fTarget != 0 ? fTarget : fPixels;
	bool skipUnchanged = !fFlip;
#else
	const uintptr_t target = fPixels;
	const bool skipUnchanged = true;
#endif
	/* Whole rows, and a band boundary that keeps the run aligned: see band[] above. */
	const bool banded = sweet_fb_band && px0 == 0 && pw * 4 == (int32_t)fRowBytes
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	    && !fFlip
#endif
	    ;
	int32_t bandRows = 0, bandFirst = 0;
	for (int32_t py = py0; py < py1; py++) {
		int32_t r = py * (int32_t)fLogicalH / (int32_t)fHeight - dy;
		r = r < 0 ? 0 : (r >= h ? h - 1 : r);
		if (r == previous) {
			/* 1334 logical rows into 1920 panel rows: 586 of them are the row above. The
			 * panel row copied last already holds exactly these pixels, so skip it
			 * instead of storing 4320 bytes a second time. */
			if (bandRows > 0) {
				memcpy((void *)(target + (uint64_t)bandFirst * fRowBytes),
				    band, (size_t)bandRows * fRowBytes);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
				if (sweet_surface_panel_cacheable) {
					flush_dcache64((addr64_t)(uintptr_t)(target + (uint64_t)bandFirst * fRowBytes),
					    (unsigned)(bandRows * (int32_t)fRowBytes), FALSE);
				}
#endif
				bandRows = 0;
			}
			continue;
		}
		uint32_t *out = (uint32_t *)(target + (uint64_t)py * fRowBytes + (uint64_t)px0 * 4);
		const uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
		/* The desktop behind the icons arrives as opaque black, not as transparency: a whole frame
		 * carried only 4320 fully transparent pixels, and every swap record carries a single
		 * non-zero layer id, so there is no wallpaper layer to composite. Replace wide runs of
		 * opaque black instead - a wallpaper area is hundreds of pixels across, a glyph stem is
		 * one or two - so text and icons survive. */
		const uint32_t ramp = (uint32_t)py * 256u / (uint32_t)(fHeight ? fHeight : 1);
		const uint32_t bg = 0xff000000u | ((12u + (ramp * 40u >> 8)) << 16) |
		    ((16u + (ramp * 26u >> 8)) << 8) | (40u + (ramp * 70u >> 8));
		if (r != previous && skipUnchanged && fShadow != NULL &&
		    memcmp(fShadow + (uint64_t)r * fLogicalW, in, (size_t)w * 4) == 0) {
			/* Unchanged since the last present: the panel rows already hold it. */
			if (bandRows > 0) {
				memcpy((void *)(target + (uint64_t)bandFirst * fRowBytes),
				    band, (size_t)bandRows * fRowBytes);
				bandRows = 0;
			}
			previous = r;
			continue;
		}
		if (r != previous) {
			buildStart = mach_absolute_time();
			if (fShadow != NULL) {
				memcpy(fShadow + (uint64_t)r * fLogicalW, in, (size_t)w * 4);
			}
			/* One source pixel becomes one or two output pixels: write the run, not the
			 * output pixel, so the source is read once per pixel instead of gathered. */
			int32_t x = 0;
			while (x < pw) {
				int32_t c = column[x];
				int32_t end = x + 1;
				uint32_t v;
				while (end < pw && column[end] == c) {
					end++;
				}
				v = in[c];
				/* The desktop is not exactly black: the lock screen and the home screen
				 * paint a near-black (0xff0a0a0a and friends), which the exact test used
				 * to miss, so no wallpaper ever appeared. Any opaque colour whose
				 * brightest channel is almost nil is background. */
				const uint32_t cr = (v >> 16) & 0xffu, cg = (v >> 8) & 0xffu, cb = v & 0xffu;
				const uint32_t cmax = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
				/* 0x14 was too tight: the lock screen and the home screen paint
				 * 0xff1c1c1e and friends, which stayed on screen as black. */
				bool isbg = cmax <= 0x28u;
				/* The exact and near-black tests both found nothing on this phone: each
				 * screen paints its own dark colour.  Anything close to the frame's own
				 * dominant colour (sweet_fb_dominant) is background too. */
				if (!isbg && sweet_fb_bg != 0u) {
					const int32_t dr = (int32_t)cr - (int32_t)((sweet_fb_bg >> 16) & 0xffu);
					const int32_t dg = (int32_t)cg - (int32_t)((sweet_fb_bg >> 8) & 0xffu);
					const int32_t db = (int32_t)cb - (int32_t)(sweet_fb_bg & 0xffu);
					isbg = dr <= 12 && dr >= -12 && dg <= 12 && dg >= -12 &&
					    db <= 12 && db >= -12;
				}
				if ((v >> 24) == 0xffu && isbg && (end - x) >= 32) {
					/* A wallpaper area is hundreds of pixels across, a glyph stem is one
					 * or two, so wide dark runs are background. Paint the image there
					 * when one is loaded, the gradient when it is not. */
					if (sweet_fb_wall != NULL) {
						v = sweet_fb_wall_at(c, r, w, h);
						sweet_fb_wall_px += (uint32_t)(end - x);
					} else {
						v = bg;
					}
					sweet_fb_alpha0 += (uint32_t)(end - x);
				} else if (v == 0u && sweet_fb_wall != NULL) {
					/* The client left this pixel transparent, so nothing else draws
					 * there: the picture goes in, and it counts. */
					v = sweet_fb_wall_at(c, r, w, h);
					sweet_fb_wall_px += (uint32_t)(end - x);
				}
				for (; x < end; x++) {
					row[x] = v;
				}
			}
			previous = r;
			{
				uint64_t ns = 0;
				absolutetime_to_nanoseconds(mach_absolute_time() - buildStart, &ns);
				sweet_fb_build_ns += ns;
			}
		}
		/* Two pixels per store. The present copy measured 34 ms for 8.29 MB, which is about
		 * 15 cycles per four-byte store: the panel map costs one bus transaction per store and
		 * never merges them, so this is transaction bound, not bandwidth bound. */
		storeStart = mach_absolute_time();
		if (banded && (bandRows > 0 || (py & 1) == 0)) {
			if (bandRows == 0) {
				bandFirst = py;
			}
			memcpy(band[bandRows], row, (size_t)pw * 4);
			if (++bandRows == 4 || py == py1 - 1) {
				memcpy((void *)(target + (uint64_t)bandFirst * fRowBytes),
				    band, (size_t)bandRows * fRowBytes);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
				if (sweet_surface_panel_cacheable) {
					flush_dcache64((addr64_t)(uintptr_t)(target + (uint64_t)bandFirst * fRowBytes),
					    (unsigned)(bandRows * (int32_t)fRowBytes), FALSE);
				}
#endif
				bandRows = 0;
			}
		} else {
			memcpy(out, row, (size_t)pw * 4);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
			if (sweet_surface_panel_cacheable) {
				flush_dcache64((addr64_t)(uintptr_t)out, (unsigned)(pw * 4), FALSE);
			}
#endif
		}
		{
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - storeStart, &ns);
			sweet_fb_store_ns += ns;
		}
	}
	if (bandRows > 0) {
		memcpy((void *)(target + (uint64_t)bandFirst * fRowBytes),
		    band, (size_t)bandRows * fRowBytes);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		if (sweet_surface_panel_cacheable) {
			flush_dcache64((addr64_t)(uintptr_t)(target + (uint64_t)bandFirst * fRowBytes),
			    (unsigned)(bandRows * (int32_t)fRowBytes), FALSE);
		}
#endif
	}
	if (sweet_fb_alpha0 != 0 && !sweet_fb_alpha0_logged) {
		sweet_fb_alpha0_logged = 1;
		IOLog("A5XELTE FB: %u black pixels replaced with a background\n", sweet_fb_alpha0);
	}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Every row that was stored has already been cleaned, row band by row band. The
	 * scanout needs those lines in memory, not the ones belonging to rows that were
	 * skipped because they did not change: cleaning the whole 8.29 MB panel here took
	 * 33 ms of every present, which was a third of a frame at 10 fps. */
#endif
	return (uint64_t)pw * (uint64_t)(py1 - py0) * 4;
}

uint64_t
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	sweet_fb_bg_wanted = 1;
	/* Paint the buffer the panel is not scanning, then hand that one to the
	 * controller once it is complete. */
	fTarget = fFlip ? fBack[fBackIndex ^ 1] : fPixels;
#endif
	uint64_t copied = 0;


	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
		if (PE_parse_boot_argn("a5fb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
#else
		if (PE_parse_boot_argn("sweetfb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
#endif
			/* Not PE_initialize_console(kPEDisableScreen): its console switch
			 * writes the read-only-late cons_ops_index and panics. */
			sweet_console_muted = 1;
			IOLog("A5XELTE FB: console muted (a5fb_noconsole), panel cleared\n");
			/* Whatever the console drew before this first swap stays on screen wherever the GUI
			 * does not draw (boot log lines at the bottom, junk at the top): start from black.
			 * fPixels is the physical map of the whole panel, scaledCopy() writes all of it too. */
			memset((void *)fPixels, 0, (size_t)fRowBytes * fHeight);
		}
	}

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	if (!sweet_fb_wall_tried && fScaled && fLogicalW > 0 && fLogicalH > 0) {
		sweet_fb_wall_load((int32_t)fLogicalW, (int32_t)fLogicalH);
	}
#endif
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Say what the picture is and how much of the panel it replaced: without this the
	 * only evidence was the screen, and a dark frame could mean either "no picture" or
	 * "the client never drew a dark pixel". */
	if (sweet_fb_wall != NULL && sweet_fb_wall_reports < 60) {
		sweet_fb_wall_reports++;
		IOLog("A5XELTE FB: wall %dx%d px0=%08x mid=%08x bg=%08x took %llu background pixels last frame\n",
		    sweet_fb_wall_w, sweet_fb_wall_h, sweet_fb_wall[0],
		    sweet_fb_wall[(uint64_t)(sweet_fb_wall_h / 2) * (uint64_t)sweet_fb_wall_w +
		    sweet_fb_wall_w / 2], sweet_fb_bg, (unsigned long long)sweet_fb_wall_px);
		sweet_fb_wall_px = 0;
	}
#endif

	static uint32_t sweet_fb_reclog;
	if (sweet_fb_reclog < 8) {
		sweet_fb_reclog++;
		IOLog("SWEET FB: rec %u mask=0x%x ids %u %u %u %u\n", get32(rec, 0x50), mask,
		    get32(rec, 0x54), get32(rec, 0x58), get32(rec, 0x5c), get32(rec, 0x60));
	}
	for (uint32_t layer = 0; layer < SWEET_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x54 + 4 * layer);
		const uint8_t *a = rec + 0x64 + 16 * layer;
		const uint8_t *b = rec + 0xc4 + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || id == 0) {
			continue;
		}
		SweetSurface *s = sweet_surface_copy(id);
		if (sweet_fb_swaps_logged < 16) {
			sweet_fb_swaps_logged++;
			IOLog("SWEET FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x50), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (w <= 0 || h <= 0) {
			w = (int32_t)s->width;
			h = (int32_t)s->height;
		}
		if (sx < 0 || sy < 0) {
			sx = sy = 0;
		}
		if (dx < 0 || dy < 0) {
			dx = dy = 0;
		}
		/* Logical (presented) display coordinates -> panel coordinates. */
		dx -= fOffX;
		dy -= fOffY;
		if (dx < 0) {
			sx -= dx;
			w += dx;
			dx = 0;
		}
		if (dy < 0) {
			sy -= dy;
			h += dy;
			dy = 0;
		}
		if (w > (int32_t)s->width - sx) {
			w = (int32_t)s->width - sx;
		}
		if (h > (int32_t)s->height - sy) {
			h = (int32_t)s->height - sy;
		}
		/* Scaled, dx/dy/w/h stay logical: clip to the logical display. */
		int32_t limitW = (int32_t)(fScaled ? fLogicalW : fWidth);
		int32_t limitH = (int32_t)(fScaled ? fLogicalH : fHeight);
		if (w > limitW - dx) {
			w = limitW - dx;
		}
		if (h > limitH - dy) {
			h = limitH - dy;
		}
		uint64_t last = (uint64_t)s->offset + (uint64_t)(sy + h - 1) * s->bytesPerRow +
		    (uint64_t)(sx + w) * 4;
		if (w <= 0 || h <= 0 || s->elementWidth != 1 || s->elementHeight != 1 ||
		    s->memory == NULL || last > s->memory->getLength()) {
			src = 0;
		}
		if (src != 0 && bpe == 4 && fScaled) {
			copied += scaledCopy(src + s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4,
			    s->bytesPerRow, dx, dy, w, h);
		} else if (src != 0 && bpe == 4) {
			copied += (uint64_t)w * (uint64_t)h * 4;
#if defined(ARM64_BOARD_CONFIG_HEROLTE)
			sweet_fb_copy_rows(fPixels + (uint64_t)dy * fRowBytes + (uint64_t)dx * 4, fRowBytes,
			    src + s->offset + (uint64_t)sy * s->bytesPerRow + (uint64_t)sx * 4, s->bytesPerRow,
			    (size_t)w * 4, h);
#else
			for (int32_t y = 0; y < h; y++) {
				memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
				    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
				    (size_t)w * 4);
			}
#endif
		}
		s->release();
	}
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	if (fFlip) {
		/* The panel reads this buffer directly, so every dirty line goes to memory
		 * before the base moves: one clean of the whole frame instead of a per-store
		 * bus transaction for every fourth of a million pixels. */
		flush_dcache64((addr64_t)(uintptr_t)fTarget, fBackSize, FALSE);
		__asm__ volatile ("dsb sy" ::: "memory");
		/* The first finished frame is also put where the loader left the scanout.  If
		 * the controller never takes the new base the phone then shows this frame
		 * instead of a black panel, and that difference is the whole diagnosis. */
		if (!fFlipped) {
			fFlipped = 1;
			memcpy((void *)fPixels, (const void *)fTarget, (size_t)fRowBytes * fHeight);
			__asm__ volatile ("dsb sy" ::: "memory");
		}
		fBackIndex ^= 1;
		flip();
	}
#endif
	return copied;
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

#if defined(ARM64_BOARD_CONFIG_A5XELTE)
/*
 * Point window 0 at the buffer present() just finished. The controller reads this
 * base when it starts a frame, so a single store cannot tear: the frame in flight
 * keeps its own base and the next one starts on the new buffer. The update request
 * after it costs nothing when the window is unprotected and latches the base when
 * it is shadowed.
 */
void
AppleCLCD::flip(void)
{
	if (!fFlip || fDecon == NULL) {
		return;
	}
	j7_decon_scanout(fDecon, (uint32_t)fBackPhys[fBackIndex]);
}
#endif

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= SWEET_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t sweet_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	switch (selector) {
	case kSweetFBGetLayerDefaultSurface:
		out[0] = sweet_surface_port_in_task(fFB->fCanvas != NULL ?
		    fFB->fCanvas : fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kSweetFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kSweetFBSwapEnd: {
		uint8_t rec[SWEET_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < SWEET_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kSweetFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kSweetFBGetDisplaySize:
		out[0] = fFB->fLogicalW;
		out[1] = fFB->fLogicalH;
		outCount = 2;
		break;
	case kSweetFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kSweetFBGetDotPitch:
		out[0] = SWEET_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kSweetFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < SWEET_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("SWEET FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
#if defined(ARM64_BOARD_CONFIG_A5XELTE)
			int keep_on = 0;
			if (off && PE_parse_boot_argn("a5fb_keep_on", &keep_on, sizeof(keep_on)) && keep_on) {
				/* backboardd also asks for off on its own (right after the
				 * first swap, idle); only a Power press just before is the
				 * user locking the phone (bsd/dev/a5xelte_keys.c). */
				uint64_t pressed = a5xelte_power_pressed_at, since_ns = UINT64_MAX;
				if (pressed) {
					absolutetime_to_nanoseconds(mach_absolute_time() - pressed, &since_ns);
				}
				if (since_ns > 3 * NSEC_PER_SEC) {
					IOLog("A5XELTE FB: display-off request ignored (a5fb_keep_on)\n");
					break;
				}
				IOLog("A5XELTE FB: display-off after Power press\n");
			}
#endif
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			IOLog("SWEET FB: display %s\n", off ? "off" : "on");
		}
		break;
	case kSweetFBSwapWait:
		fFB->waitForSwap();
		out[0] = 1;
		outCount = 1;
		break;
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(sweet_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			sweet_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("SWEET FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* SWEET || A5XELTE */
