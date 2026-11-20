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
extern "C" void j7m_take_stats(uint64_t *submits, uint64_t *waits, uint64_t *waitUs,
    uint64_t *storeUs, uint64_t *flushUs);
/* iokit/Kernel/arm/SweetSurface.cpp: wrap selectors backboardd asked for, cleared by the read. */
extern "C" unsigned sweet_surface_take_wrap_calls(void);
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

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void waitForSwap(void);
	uint64_t present(const uint8_t *rec);
	uint64_t scaledCopy(uintptr_t src, uint32_t srcRowBytes, int32_t dx, int32_t dy,
	    int32_t w, int32_t h);
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
			for (uint32_t y = 0; y < fb->fHeight; y++) {
				volatile uint32_t *row = (volatile uint32_t *)(fb->fPixels + (uint64_t)y * fb->fRowBytes);
				for (uint32_t x = 0; x < fb->fWidth; x++) {
					row[x] = step < 6 ? (row[x] >> 1) & 0x7f7f7f7f : 0;
				}
			}
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
	/* One cacheable destination row. fPixels is write-combined panel memory, where a
	 * per-pixel store costs far more than filling a normal row and copying it once: the
	 * 750x1334 -> 1080x1920 frame measured 28 ms done pixel by pixel. */
	static uint32_t row[2048];
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
	for (int32_t py = py0; py < py1; py++) {
		int32_t r = py * (int32_t)fLogicalH / (int32_t)fHeight - dy;
		r = r < 0 ? 0 : (r >= h ? h - 1 : r);
		uint32_t *out = (uint32_t *)(fPixels + (uint64_t)py * fRowBytes + (uint64_t)px0 * 4);
		const uint32_t *in = (const uint32_t *)(src + (uint64_t)r * srcRowBytes);
		if (r != previous && fShadow != NULL &&
		    memcmp(fShadow + (uint64_t)r * fLogicalW, in, (size_t)w * 4) == 0) {
			/* Unchanged since the last present: the panel rows already hold it. */
			previous = r;
			continue;
		}
		if (r != previous) {
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
				for (; x < end; x++) {
					row[x] = v;
				}
			}
			previous = r;
		}
		memcpy(out, row, (size_t)pw * 4);
	}
	return (uint64_t)pw * (uint64_t)(py1 - py0) * 4;
}

uint64_t
AppleCLCD::present(const uint8_t *rec)
{
	uint32_t mask = get32(rec, 0x104);
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
