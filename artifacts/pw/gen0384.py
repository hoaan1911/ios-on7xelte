#!/usr/bin/env python3
"""Generate 0384-0386 from the post-0383 peek source, one patch per fix."""
import subprocess, sys, os

SRC = "artifacts/cur/SweetFramebuffer.cpp"
PATH = "iokit/Kernel/arm/SweetFramebuffer.cpp"
TMP = "/tmp/gen"
os.makedirs(TMP, exist_ok=True)

src = open(SRC).read()

def rep(text, old, new, what):
    assert text.count(old) == 1, "no unique match: " + what
    return text.replace(old, new)

# ---------------------------------------------------------------- 0384
C_OLD_START = "#if defined(ARM64_BOARD_CONFIG_A5XELTE)\n/*\n * Raise both CPU clusters"
C_END = "}\n#endif\n\nbool\nAppleCLCD::start(IOService *provider)"

a = src.index(C_OLD_START)
b = src.index(C_END, a) + len("}\n#endif\n")
NEW_FN = '''#if defined(ARM64_BOARD_CONFIG_A5XELTE)
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
\tstatic const struct { uint64_t cmu; const char *name; } cpu[] = {
\t\t{ 0x10900000ULL, "CPUCL0" },
\t\t{ 0x10800000ULL, "CPUCL1" },
\t};
\tfor (unsigned i = 0; i < sizeof(cpu) / sizeof(cpu[0]); i++) {
\t\tIOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(
\t\t    cpu[i].cmu, 0x1000, kIODirectionNone);
\t\tIOMemoryMap *map = desc != NULL ? desc->map(kIOMapInhibitCache) : NULL;
\t\tif (map == NULL) {
\t\t\tIOLog("A5XELTE CPU: no %s map\\n", cpu[i].name);
\t\t\tOSSafeReleaseNULL(desc);
\t\t\tcontinue;
\t\t}
\t\tvolatile uint32_t *c = (volatile uint32_t *)map->getVirtualAddress();
\t\tconst uint32_t con0 = c[0x100 / 4];
\t\tconst uint32_t cm = (con0 >> 16) & 0x3ffu, cp = (con0 >> 8) & 0x3fu, cs = con0 & 7u;
\t\tconst uint64_t pll = (cp != 0 && cs < 7) ? 26000ULL * cm / ((uint64_t)cp << cs) : 0;
\t\tconst uint32_t d1 = c[0x400 / 4] & 7u, d2 = c[0x404 / 4] & 7u;
\t\tconst uint64_t arm = pll / ((uint64_t)(d1 + 1) * (uint64_t)(d2 + 1));
\t\tIOLog("A5XELTE CPU: %s PLL_CON0=%08x M=%u P=%u S=%u -> %llu kHz, div %u/%u -> %llu kHz, left as the bootloader set it (asked for %u)\\n",
\t\t    cpu[i].name, con0, cm, cp, cs, pll, d1, d2, arm, khz);
\t\tOSSafeReleaseNULL(map);
\t\tOSSafeReleaseNULL(desc);
\t}
}
#endif
'''
s1 = src[:a] + NEW_FN + src[b:]

s1 = rep(s1, '''\t\t/* j7cpu=<kHz> raises both clusters: every perf line said cpu_mhz=900 because the
\t\t * bootloader leaves them at the 902 MHz operating point and nothing here ever
\t\t * changes that.  0 keeps the bootloader's rate. */
\t\t/* j7cpu is a rate in kHz, and the old default passed 1586 - which is MHz - so
\t\t * every boot logged "1586 kHz is outside the PLL range" and stayed at 902 MHz.
\t\t * Take a small value as MHz, and default to 1794 MHz: cl0_max_support_idx in the
\t\t * vendor DVFS table, its highest usable step (2002 MHz is table entry 0 only).
\t\t * If that PLL will not lock, fall back to 1586 MHz, the top official step. */
\t\tint cpu = 1794;
\t\tPE_parse_boot_argn("j7cpu", &cpu, sizeof(cpu));
\t\tif (cpu > 0 && cpu < 100000) {
\t\t\tcpu *= 1000;
\t\t}
\t\tif (cpu > 0 && !j7_cpu_clock((unsigned)cpu)) {
\t\t\tif (cpu > 1586000) {
\t\t\t\tIOLog("A5XELTE CPU: retrying at 1586000 kHz\\n");
\t\t\t\tj7_cpu_clock(1586000u);
\t\t\t}
\t\t}
''', '''\t\t/* j7cpu=<kHz> is only reported.  The clock is not written: the switch away from
\t\t * the PLL is not at the offsets this tree used and the attempt left the whole
\t\t * machine at 38 MHz (see j7_cpu_clock).  A small value is MHz. */
\t\tint cpu = 1794;
\t\tPE_parse_boot_argn("j7cpu", &cpu, sizeof(cpu));
\t\tif (cpu > 0 && cpu < 100000) {
\t\t\tcpu *= 1000;
\t\t}
\t\tif (cpu > 0) {
\t\t\tj7_cpu_clock((unsigned)cpu);
\t\t}
''', "caller")

# ---------------------------------------------------------------- 0385
HELPER = '''#if defined(ARM64_BOARD_CONFIG_A5XELTE)
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
\td[0x030 / 4] = d[0x030 / 4] & ~(1u << 8);
\td[0x880 / 4] = pa;
\td[0x710 / 4] = 1u;
\t__asm__ volatile ("dsb sy" ::: "memory");
}
#endif

'''
s2 = rep(s1, "bool\nAppleCLCD::start(IOService *provider)", HELPER + "bool\nAppleCLCD::start(IOService *provider)", "helper")

s2 = rep(s2, '''\tint           fFlip;
''', '''\tint           fFlip;
\tint           fFlipped;       /* the first finished frame also went to the loader's buffer */
''', "member")

s2 = rep(s2, '''\t\t\t\t\tfDecon = (volatile uint32_t *)map->getVirtualAddress();
\t\t\t\t\t/* Window 0's own base, and the update request that latches the
\t\t\t\t\t * shadowed window registers at the next frame boundary. */
\t\t\t\t\tfDecon[0x880 / 4] = (uint32_t)fBackPhys[0];
\t\t\t\t\tfDecon[0x710 / 4] = 1u;
\t\t\t\t\t__asm__ volatile ("dsb sy" ::: "memory");
\t\t\t\t\tIOLog("A5XELTE FB: double buffer on, scanout 0x%llx and 0x%llx, ADD0 now %08x\\n",
\t\t\t\t\t    fBackPhys[0], fBackPhys[1], fDecon[0x880 / 4]);
''', '''\t\t\t\t\tfDecon = (volatile uint32_t *)map->getVirtualAddress();
\t\t\t\t\tfFlipped = 0;
\t\t\t\t\tj7_decon_scanout(fDecon, (uint32_t)fBackPhys[0]);
\t\t\t\t\tIOLog("A5XELTE FB: double buffer on, scanout 0x%llx and 0x%llx, ADD0 now %08x SHADOWCON %08x\\n",
\t\t\t\t\t    fBackPhys[0], fBackPhys[1], fDecon[0x880 / 4], fDecon[0x030 / 4]);
''', "flip init")

s2 = rep(s2, '''\tfDecon[0x880 / 4] = (uint32_t)fBackPhys[fBackIndex];
\t__asm__ volatile ("dsb sy" ::: "memory");
\tfDecon[0x710 / 4] = 1u;
\t__asm__ volatile ("dsb sy" ::: "memory");
''', '''\tj7_decon_scanout(fDecon, (uint32_t)fBackPhys[fBackIndex]);
''', "flip body")

s2 = rep(s2, '''\tif (fFlip) {
\t\t/* The panel reads this buffer directly, so every dirty line goes to memory
\t\t * before the base moves: one clean of the whole frame instead of a per-store
\t\t * bus transaction for every fourth of a million pixels. */
\t\tflush_dcache64((addr64_t)(uintptr_t)fTarget, fBackSize, FALSE);
\t\t__asm__ volatile ("dsb sy" ::: "memory");
\t\tfBackIndex ^= 1;
\t\tflip();
\t}
''', '''\tif (fFlip) {
\t\t/* The panel reads this buffer directly, so every dirty line goes to memory
\t\t * before the base moves: one clean of the whole frame instead of a per-store
\t\t * bus transaction for every fourth of a million pixels. */
\t\tflush_dcache64((addr64_t)(uintptr_t)fTarget, fBackSize, FALSE);
\t\t__asm__ volatile ("dsb sy" ::: "memory");
\t\t/* The first finished frame is also put where the loader left the scanout.  If
\t\t * the controller never takes the new base the phone then shows this frame
\t\t * instead of a black panel, and that difference is the whole diagnosis. */
\t\tif (!fFlipped) {
\t\t\tfFlipped = 1;
\t\t\tmemcpy((void *)fPixels, (const void *)fTarget, (size_t)fRowBytes * fHeight);
\t\t\t__asm__ volatile ("dsb sy" ::: "memory");
\t\t}
\t\tfBackIndex ^= 1;
\t\tflip();
\t}
''', "present hedge")

# ---------------------------------------------------------------- 0386
WALL_AT = '''/*
 * The picture is the logical display size and a swap record's layer is whatever the client says,
 * not always the same size: requiring an exact match meant no wallpaper was ever sampled (the
 * log said "took 0 background pixels" on a frame whose dominant colour was black).  Sample the
 * picture at the layer's pixel position instead, nearest neighbour.
 */
static inline uint32_t
sweet_fb_wall_at(int32_t c, int32_t r, int32_t w, int32_t h)
{
\tif (sweet_fb_wall == NULL || sweet_fb_wall_w <= 0 || sweet_fb_wall_h <= 0 || w <= 0 || h <= 0) {
\t\treturn 0;
\t}
\tint64_t sx = (int64_t)c * sweet_fb_wall_w / w;
\tint64_t sy = (int64_t)r * sweet_fb_wall_h / h;
\tif (sx < 0) {
\t\tsx = 0;
\t} else if (sx >= sweet_fb_wall_w) {
\t\tsx = sweet_fb_wall_w - 1;
\t}
\tif (sy < 0) {
\t\tsy = 0;
\t} else if (sy >= sweet_fb_wall_h) {
\t\tsy = sweet_fb_wall_h - 1;
\t}
\treturn sweet_fb_wall[(uint64_t)sy * (uint64_t)sweet_fb_wall_w + (uint64_t)sx];
}

'''
s3 = rep(s2, "static int sweet_fb_bg_wanted;\n", "static int sweet_fb_bg_wanted;\n\n" + WALL_AT, "wall decl")

s3 = rep(s3, '''\t\t\t\t\tif (sweet_fb_wall != NULL && sweet_fb_wall_w == w &&
\t\t\t\t\t    sweet_fb_wall_h == h) {
\t\t\t\t\t\tv = sweet_fb_wall[(uint64_t)r * (uint64_t)w + (uint64_t)c];
\t\t\t\t\t\tsweet_fb_wall_px += (uint32_t)(end - x);
\t\t\t\t\t} else {
''', '''\t\t\t\t\tif (sweet_fb_wall != NULL) {
\t\t\t\t\t\tv = sweet_fb_wall_at(c, r, w, h);
\t\t\t\t\t\tsweet_fb_wall_px += (uint32_t)(end - x);
\t\t\t\t\t} else {
''', "wall branch 1")

s3 = rep(s3, '''\t\t\t\t} else if (v == 0u && sweet_fb_wall != NULL &&
\t\t\t\t    sweet_fb_wall_w == w && sweet_fb_wall_h == h) {
\t\t\t\t\t/* The client left this pixel transparent, so nothing else draws
\t\t\t\t\t * there: the picture goes in, and it counts. */
\t\t\t\t\tv = sweet_fb_wall[(uint64_t)r * (uint64_t)w + (uint64_t)c];
''', '''\t\t\t\t} else if (v == 0u && sweet_fb_wall != NULL) {
\t\t\t\t\t/* The client left this pixel transparent, so nothing else draws
\t\t\t\t\t * there: the picture goes in, and it counts. */
\t\t\t\t\tv = sweet_fb_wall_at(c, r, w, h);
''', "wall branch 2")

open("/tmp/gen/s1.cpp", "w").write(s1)
open("/tmp/gen/s2.cpp", "w").write(s2)
open("/tmp/gen/s3.cpp", "w").write(s3)
print("wrote", len(s1), len(s2), len(s3))
