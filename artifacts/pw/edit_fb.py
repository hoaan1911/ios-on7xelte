P = '/workspace/chats/c57811c4ae693748619586f4/artifacts/pw/fb.new.cpp'
src = open(P).read()
L = src.splitlines(True)

def find(sub, start=0):
    for i in range(start, len(L)):
        if sub in L[i]:
            return i
    raise SystemExit('not found: ' + sub)

CPU = '''#if defined(ARM64_BOARD_CONFIG_A5XELTE)
/*
 * Raise both CPU clusters to the SoC's rated clock.
 *
 * Every perf window reports cpu_mhz=900 while the Exynos7870 is rated 1586 MHz, and this tree
 * has no cpufreq driver, so nothing ever asks for more: the bootloader leaves the cluster PLLs
 * at the 902 MHz operating point.  Samsung's own clock code for this SoC (pwrcal/S5E7870, from
 * the vendor kernel) puts the registers at
 *   CMU_CPUCL0 0x10900000, CMU_CPUCL1 0x10800000
 *     +0x100 PLL_CON0, M 16:25, P 8:13, S 0:2, enable bit 31, locked bit 29
 *     +0x208 the ARMCLK mux, bit 12: 0 = cluster PLL, 1 = the switch input
 *     +0x608 that mux's status, bits 12:13
 *     +0x400/+0x404 the two ARMCLK dividers, bits 0:2, value + 1
 *   CMU_MIF 0x10460000
 *     +0x858/+0x85c the switch gate for cluster 0/1, bit 0
 *     +0x458/+0x45c the switch divider, bits 0:1 (BUS PLL / 2, about 800 MHz)
 * and the DVFS table lists 1586 MHz at the same 900 mV as 902 MHz, so no PMIC write is needed.
 *
 * A PLL cannot be reprogrammed while the cores run from it, so ARMCLK is moved to the switch
 * input first, the dividers are set to /1, the PLL is programmed and polled for lock, and only
 * then is ARMCLK switched back.  Every wait is bounded, and a PLL that never locks leaves the
 * cores on the switch clock with a log line instead of hanging the phone.
 */
static void
j7_cpu_clock(unsigned khz)
{
	static const struct { uint64_t cmu; const char *name; uint32_t gate, swdiv; } cpu[] = {
		{ 0x10900000ULL, "CPUCL0", 0x858u, 0x458u },
		{ 0x10800000ULL, "CPUCL1", 0x85cu, 0x45cu },
	};
	/* 26 MHz * M / (P << S): 244/4/0 is 1586 MHz, the vendor table's top official step. */
	const uint32_t m = (uint32_t)((uint64_t)khz * 4u / 26000u);
	const uint32_t p = 4, s = 0;

	if (m < 64 || m > 0x3ffu) {
		IOLog("A5XELTE CPU: %u kHz is outside the PLL range\\n", khz);
		return;
	}
	IOMemoryDescriptor *mifDesc = IOMemoryDescriptor::withPhysicalAddress(
	    0x10460000ULL, 0x1000, kIODirectionNone);
	IOMemoryMap *mifMap = mifDesc != NULL ? mifDesc->map(kIOMapInhibitCache) : NULL;
	if (mifMap == NULL) {
		IOLog("A5XELTE CPU: no CMU_MIF map\\n");
		OSSafeReleaseNULL(mifDesc);
		return;
	}
	volatile uint32_t *mif = (volatile uint32_t *)mifMap->getVirtualAddress();
	for (unsigned i = 0; i < sizeof(cpu) / sizeof(cpu[0]); i++) {
		IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
		    cpu[i].cmu, 0x1000, kIODirectionNone);
		IOMemoryMap *map = desc != NULL ? desc->map(kIOMapInhibitCache) : NULL;
		if (map == NULL) {
			IOLog("A5XELTE CPU: no %s map\\n", cpu[i].name);
			OSSafeReleaseNULL(desc);
			continue;
		}
		volatile uint32_t *c = (volatile uint32_t *)map->getVirtualAddress();
		const uint32_t con0 = c[0x100 / 4];
		const uint32_t cm = (con0 >> 16) & 0x3ffu, cp = (con0 >> 8) & 0x3fu, cs = con0 & 7u;
		const uint64_t pll = (cp != 0 && cs < 7) ? 26000ULL * cm / ((uint64_t)cp << cs) : 0;
		const uint32_t d1 = c[0x400 / 4] & 7u, d2 = c[0x404 / 4] & 7u;
		const uint64_t arm = pll / ((uint64_t)(d1 + 1) * (uint64_t)(d2 + 1));
		unsigned n;
		bool locked = false;

		IOLog("A5XELTE CPU: %s PLL_CON0=%08x M=%u P=%u S=%u -> %llu kHz, div %u/%u -> %llu kHz\\n",
		    cpu[i].name, con0, cm, cp, cs, pll, d1, d2, arm);
		if (arm >= khz) {
			IOLog("A5XELTE CPU: %s is already at %llu kHz\\n", cpu[i].name, arm);
			OSSafeReleaseNULL(map);
			OSSafeReleaseNULL(desc);
			continue;
		}
		/* Move ARMCLK to the switch input: its gate on, its divider /1 (BUS PLL / 2). */
		mif[cpu[i].gate / 4] = mif[cpu[i].gate / 4] | 1u;
		mif[cpu[i].swdiv / 4] = mif[cpu[i].swdiv / 4] & ~3u;
		__asm__ volatile ("dsb sy" ::: "memory");
		IOSleep(1);
		c[0x208 / 4] = c[0x208 / 4] | (1u << 12);
		__asm__ volatile ("dsb sy" ::: "memory");
		for (n = 0; n < 100; n++) {
			if (((c[0x608 / 4] >> 12) & 3u) == 1u) {
				break;
			}
			IOSleep(1);
		}
		if (n == 100) {
			IOLog("A5XELTE CPU: %s did not leave the PLL, left alone\\n", cpu[i].name);
			OSSafeReleaseNULL(map);
			OSSafeReleaseNULL(desc);
			continue;
		}
		/* Dividers to /1 while the cores are not on this path. */
		c[0x400 / 4] = c[0x400 / 4] & ~7u;
		c[0x404 / 4] = c[0x404 / 4] & ~7u;
		IOSleep(1);
		c[0x100 / 4] = (1u << 31) | (m << 16) | (p << 8) | s;
		__asm__ volatile ("dsb sy" ::: "memory");
		for (n = 0; n < 100; n++) {
			if (c[0x100 / 4] & (1u << 29)) {
				locked = true;
				break;
			}
			IOSleep(1);
		}
		if (!locked) {
			IOLog("A5XELTE CPU: %s PLL never locked (%08x), staying on the switch clock\\n",
			    cpu[i].name, c[0x100 / 4]);
			OSSafeReleaseNULL(map);
			OSSafeReleaseNULL(desc);
			continue;
		}
		IOSleep(1);
		c[0x208 / 4] = c[0x208 / 4] & ~(1u << 12);
		__asm__ volatile ("dsb sy" ::: "memory");
		for (n = 0; n < 100; n++) {
			if (((c[0x608 / 4] >> 12) & 3u) == 0u) {
				break;
			}
			IOSleep(1);
		}
		IOLog("A5XELTE CPU: %s on PLL_CON0=%08x, mux %u\\n",
		    cpu[i].name, c[0x100 / 4], (c[0x608 / 4] >> 12) & 3u);
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(desc);
	}
	OSSafeReleaseNULL(mifMap);
	OSSafeReleaseNULL(mifDesc);
}
#endif

'''

CALL = '''	{
		/* j7cpu=<kHz> raises both clusters: every perf line said cpu_mhz=900 because the
		 * bootloader leaves them at the 902 MHz operating point and nothing here ever
		 * changes that.  0 keeps the bootloader's rate. */
		int cpu = 1586;
		PE_parse_boot_argn("j7cpu", &cpu, sizeof(cpu));
		if (cpu > 0) {
			j7_cpu_clock((unsigned)cpu);
		}
	}
'''

STATICS = '''/* this frame's own background colour, 0 when it has none: see sweet_fb_dominant() */
static uint32_t sweet_fb_bg;
static int sweet_fb_bg_wanted;
'''

DOM = '''#if defined(ARM64_BOARD_CONFIG_A5XELTE)
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

'''

SAMPLE = '''#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	/* Once per present, from the layer that covers the display: its own background colour. */
	if (sweet_fb_bg_wanted && sweet_fb_wall != NULL && fLogicalW > 0 && fLogicalH > 0 &&
	    (int64_t)w * h * 2 >= (int64_t)fLogicalW * fLogicalH) {
		sweet_fb_bg = sweet_fb_dominant(src, srcRowBytes, w, h);
		sweet_fb_bg_wanted = 0;
	}
#endif
'''

PRESENT = '''#if defined(ARM64_BOARD_CONFIG_A5XELTE)
	sweet_fb_bg_wanted = 1;
#endif
'''

TEST = '''				const uint32_t cr = (v >> 16) & 0xffu, cg = (v >> 8) & 0xffu, cb = v & 0xffu;
				const uint32_t cmax = cr > cg ? (cr > cb ? cr : cb) : (cg > cb ? cg : cb);
				bool isbg = cmax <= 0x14u;
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
'''

# 1. the clock function, before AppleCLCD::start
i = find('AppleCLCD::start(IOService *provider)')
assert L[i - 1].rstrip('\n') == 'bool'
L = L[:i - 1] + CPU.splitlines(True) + L[i - 1:]

# 2. the call, as a sibling block before the j7clk block
i = find('/* j7clk=1 reads - and only reads')
assert L[i - 1].rstrip('\n') == '\t{'
L = L[:i - 1] + CALL.splitlines(True) + L[i - 1:]

# 3. statics
i = find('static int sweet_fb_wall_reports;')
L = L[:i + 1] + STATICS.splitlines(True) + L[i + 1:]

# 4. the dominant-colour helper, before the scaledCopy comment
i = find(' * sweetfb_scale: the logical rect')
assert L[i - 1].rstrip('\n') == '/*'
L = L[:i - 1] + DOM.splitlines(True) + L[i - 1:]

# 5. sample once per present, at the top of scaledCopy
i = find('\tstatic uint16_t column[2048];')
L = L[:i + 1] + SAMPLE.splitlines(True) + L[i + 1:]

# 6. ask for a sample at the top of present()
i = find('uint32_t mask = get32(rec, 0x104);')
L = L[:i + 1] + PRESENT.splitlines(True) + L[i + 1:]

# 7. use it in the run test
i = find('const uint32_t cr = (v >> 16) & 0xffu')
assert 'cmax' in L[i + 1] and 'cmax <= 0x14u' in L[i + 2]
L = L[:i] + TEST.splitlines(True) + L[i + 3:]

open(P, 'w').write(''.join(L))
print('ok', len(L))
