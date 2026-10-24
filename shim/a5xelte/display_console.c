/* SPDX-License-Identifier: GPL-2.0-only
 * Early SM-A510F display console. Register layout and sequence follow the
 * working mainline/drivers/exynos7580-drm-handoff.c and
 * mainline/drivers/clk-exynos7580-display.c. Firmware must already own panel
 * power and DSI PLL/PHY. No storage, panel reset or power-domain transition.
 */
#include <stdint.h>
#include "display_console.h"

#define BIT(n) (1U << (n))
#define DECON 0x14830000UL
#define DSIM 0x14800000UL
#define SYSMMU 0x14860000UL
#define DISP_CMU 0x14870000UL
#define MIF_CMU 0x10430000UL
#define DISP_POWER 0x10464080UL
#define FB_BASE 0x67000000UL
#define FB_WIDTH 1080U
#define FB_HEIGHT 1920U
#define FB_STRIDE 4320U
#define MAX_POLL 2000000U

#define VIDCON0 0x0000U
#define VIDOUTCON0 0x0004U
#define VCLKCON0 0x0010U
#define SHADOWCON 0x0030U
#define WINCHMAP0 0x0040U
#define WINCON0 0x0050U
#define WHOLE_X 0x0130U
#define WHOLE_Y 0x0134U
#define OFFSET_X 0x0170U
#define OFFSET_Y 0x0174U
#define OSD_A 0x0230U
#define OSD_B 0x0234U
#define OSD_C 0x0238U
#define OSD_D 0x023cU
#define WIN_MAP0 0x0340U
#define BLENDCON 0x03d8U
#define INTCON1 0x0504U
#define VIDCON1 0x0600U
#define VTIMING0 0x0610U
#define VTIMING1 0x0614U
#define HTIMING0 0x0618U
#define HTIMING1 0x061cU
#define SIZE 0x0620U
#define TRIGCON 0x06b0U
#define UPDATE 0x0710U
#define FB_START 0x0880U

/* Recovery reads the reserved Samsung log page after the boot attempt. */
enum {
    DISP_STATE = 64, DISP_POWER_BEFORE, DISP_DSIM_STATUS_BEFORE,
    DISP_DSIM_CONFIG_BEFORE, DISP_DSIM_SIZE_BEFORE, DISP_DECON_BEFORE,
    DISP_FB_START_BEFORE, DISP_SYSMMU_BEFORE, DISP_CLOCK_GATE_BEFORE,
    DISP_DSIM_STATUS_AFTER, DISP_DECON_AFTER, DISP_FB_START_AFTER,
    DISP_SYSMMU_AFTER,
};

static uint32_t rd(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

static void wr(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
    __asm__ volatile("dsb sy" : : : "memory");
}

static void setbits(uintptr_t address, uint32_t mask)
{
    wr(address, rd(address) | mask);
    (void)rd(address);
}

static void mask(uintptr_t address, uint32_t bits, uint32_t value)
{
    wr(address, (rd(address) & ~bits) | value);
}

static int poll(uintptr_t address, uint32_t bits, uint32_t want)
{
    for (unsigned i = 0; i < MAX_POLL; ++i)
        if ((rd(address) & bits) == want) return 1;
    return 0;
}

static void clean_fb(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, CTR_EL0" : "=r" (ctr));
    uintptr_t line = 4UL << ((ctr >> 16) & 15U);
    for (uintptr_t p = FB_BASE; p < FB_BASE + FB_STRIDE * FB_HEIGHT; p += line)
        __asm__ volatile("dc cvac, %0" : : "r" (p) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

/* A compact 5x7 uppercase font for the few early boot messages. */
struct glyph { char ch; uint8_t rows[7]; };
static const struct glyph font[] = {
    {' ',{0,0,0,0,0,0,0}}, {'-',{0,0,0,31,0,0,0}},
    {':',{0,4,4,0,4,4,0}}, {'?',{14,17,1,2,4,0,4}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,30,1,1,17,14}},
    {'6',{6,8,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,2,12}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
};

static const uint8_t *glyph_for(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    for (unsigned i = 0; i < sizeof(font) / sizeof(font[0]); ++i)
        if (font[i].ch == ch) return font[i].rows;
    return font[3].rows;
}

static void draw_text(unsigned row, const char *s)
{
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)FB_BASE;
    const unsigned scale = 3, top = 24 + row * 32, left = 24;
    if (row >= 12) return;
    for (unsigned col = 0; s[col] && col < 48; ++col) {
        const uint8_t *g = glyph_for(s[col]);
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 5; ++x) {
                uint32_t pixel = (g[y] & (1U << (4 - x))) ? 0xffffffffU : 0xff000000U;
                for (unsigned sy = 0; sy < scale; ++sy)
                    for (unsigned sx = 0; sx < scale; ++sx)
                        fb[(top + y * scale + sy) * FB_WIDTH +
                           left + col * 6 * scale + x * scale + sx] = pixel;
            }
    }
}

static void enable_clocks(void)
{
    /* Exact six-gate union from the working display CCF driver. */
    setbits(MIF_CMU + 0x810, BIT(0));
    setbits(MIF_CMU + 0xa14, BIT(0) | BIT(1));
    setbits(DISP_CMU + 0x800, BIT(0));
    setbits(DISP_CMU + 0x804, BIT(0) | BIT(9));
    setbits(DISP_CMU + 0x900, BIT(0) | BIT(4) | BIT(11));
    setbits(DISP_CMU + 0xa00, BIT(0) | BIT(1) | BIT(5) | BIT(9) | BIT(10));
}

static int dsim_handoff(void)
{
    setbits(DSIM + 0x0c, BIT(16));
    if (!poll(DSIM + 0x0c, BIT(16), 0)) return 0;
    setbits(DSIM + 0x1c, 0x7ffU << 21);
    setbits(DSIM + 0x14, 0xffffffU);
    mask(DSIM + 0x78, 0xffffU, 0xffffU);
    setbits(DSIM + 0x20, BIT(30));
    setbits(DSIM + 0x18, BIT(31) | 0x1fU);
    mask(DSIM + 0x1c, BIT(7) | BIT(6), 0);
    setbits(DSIM + 0x20, BIT(31));
    setbits(DSIM + 0x0c, BIT(16));
    if (!poll(DSIM + 0x0c, BIT(16), 0)) return 0;
    setbits(DSIM + 0x10, BIT(31));
    return 1;
}

static int dcs_long(const uint8_t *bytes, unsigned length)
{
    uint32_t old = rd(DSIM + 0x1c);
    mask(DSIM + 0x1c, BIT(7), 0);
    wr(DSIM + 0x34, BIT(28) | BIT(29) | BIT(15));
    for (unsigned i = 0; i < length; i += 4) {
        uint32_t word = 0;
        if (!poll(DSIM + 0x4c, BIT(21), 0)) goto fail;
        for (unsigned j = 0; j < 4 && i + j < length; ++j)
            word |= (uint32_t)bytes[i + j] << (j * 8);
        wr(DSIM + 0x40, word);
    }
    if (!poll(DSIM + 0x4c, BIT(23), 0)) goto fail;
    wr(DSIM + 0x3c, 0x39U | (length << 8));
    if (!poll(DSIM + 0x34, BIT(28) | BIT(29), BIT(28) | BIT(29))) goto fail;
    wr(DSIM + 0x34, BIT(28) | BIT(29) | BIT(15));
    wr(DSIM + 0x1c, old);
    return 1;
fail:
    wr(DSIM + 0x1c, old);
    return 0;
}

static int dcs_address_mode(uint8_t *mode)
{
    uint32_t old = rd(DSIM + 0x1c), response;
    mask(DSIM + 0x1c, BIT(7), 0);
    wr(DSIM + 0x34, BIT(28) | BIT(18) | BIT(15));
    if (!poll(DSIM + 0x4c, BIT(23), 0)) goto fail;
    wr(DSIM + 0x3c, 0x37U | (1U << 8)); /* maximum return packet: one byte */
    if (!poll(DSIM + 0x34, BIT(28), BIT(28))) goto fail;
    wr(DSIM + 0x34, BIT(28));
    if (!poll(DSIM + 0x4c, BIT(23), 0)) goto fail;
    wr(DSIM + 0x3c, 0x06U | (0x0bU << 8)); /* DCS GET_ADDRESS_MODE */
    if (!poll(DSIM + 0x34, BIT(28) | BIT(18), BIT(28) | BIT(18))) goto fail;
    if (rd(DSIM + 0x34) & BIT(15)) goto fail;
    response = rd(DSIM + 0x44);
    if ((response & 0xff) != 0x21) goto fail;
    *mode = (uint8_t)(response >> 8);
    (void)rd(DSIM + 0x44); /* 7580 trailing RX word */
    wr(DSIM + 0x34, BIT(28) | BIT(18) | BIT(15));
    wr(DSIM + 0x1c, old);
    return 1;
fail:
    wr(DSIM + 0x1c, old);
    return 0;
}

static int dcs_short_param(uint8_t command, uint8_t parameter)
{
    uint32_t old = rd(DSIM + 0x1c);
    mask(DSIM + 0x1c, BIT(7), 0);
    wr(DSIM + 0x34, BIT(28) | BIT(15));
    if (!poll(DSIM + 0x4c, BIT(23), 0)) goto fail;
    wr(DSIM + 0x3c, 0x15U | ((uint32_t)command << 8) |
        ((uint32_t)parameter << 16));
    if (!poll(DSIM + 0x34, BIT(28), BIT(28))) goto fail;
    wr(DSIM + 0x34, BIT(28) | BIT(15));
    wr(DSIM + 0x1c, old);
    return 1;
fail:
    wr(DSIM + 0x1c, old);
    return 0;
}

int a5_display_console_start(volatile uint64_t *log)
{
    log[DISP_STATE] = 1;
    log[DISP_POWER_BEFORE] = rd(DISP_POWER + 4);
    if ((log[DISP_POWER_BEFORE] & 0xf) != 0xf) return -1;
    log[DISP_DSIM_STATUS_BEFORE] = rd(DSIM + 4);
    log[DISP_DSIM_CONFIG_BEFORE] = rd(DSIM + 0x18);
    log[DISP_DSIM_SIZE_BEFORE] = rd(DSIM + 0x20);
    log[DISP_DECON_BEFORE] = rd(DECON + VIDCON0);
    log[DISP_FB_START_BEFORE] = rd(DECON + FB_START);
    log[DISP_SYSMMU_BEFORE] = rd(SYSMMU);
    log[DISP_CLOCK_GATE_BEFORE] = rd(DISP_CMU + 0xa00);
    if (!(log[DISP_DSIM_STATUS_BEFORE] & BIT(31)) ||
        (log[DISP_DSIM_CONFIG_BEFORE] & BIT(25)) ||
        (log[DISP_DSIM_SIZE_BEFORE] & 0xfff) != FB_WIDTH ||
        ((log[DISP_DSIM_SIZE_BEFORE] >> 16) & 0xfff) != FB_HEIGHT)
        return -2;

    log[DISP_STATE] = 2;
    enable_clocks();
    mask(DECON + TRIGCON, BIT(4), 0);
    if (!poll(DECON + VIDCON1, 0x1fffU << 17, 0)) return -3;
    wr(DECON + VIDCON0, 0);
    if (!poll(DECON + VIDCON0, BIT(2), 0)) return -4;
    wr(DECON + VIDCON0, BIT(28));
    if (!poll(DECON + VIDCON0, BIT(28), 0)) return -5;
    /* Only after DMA stopped: discard stale firmware IOVA translation. */
    wr(SYSMMU, 0);
    (void)rd(SYSMMU);
    for (unsigned i = 0; i < 3; ++i) {
        wr(DECON + WINCON0 + i * 4, 0x1e000);
        wr(DECON + WIN_MAP0 + i * 4, 0);
    }
    wr(DECON + BLENDCON, BIT(0));
    wr(DECON + VIDOUTCON0, BIT(24) | BIT(23));
    wr(DECON + VCLKCON0, BIT(8) | BIT(0));
    wr(DECON + VIDCON1, 3U << 9);
    wr(DECON + VTIMING0, 11);
    wr(DECON + VTIMING1, 0);
    wr(DECON + HTIMING0, 0);
    wr(DECON + HTIMING1, 0);
    wr(DECON + SIZE, (1919U << 16) | 1079U);
    wr(DECON + TRIGCON, BIT(13) | BIT(3));
    wr(DECON + INTCON1, 7);
    log[DISP_STATE] = 3;
    if (!dsim_handoff()) return -6;
    static const uint8_t column[] = {0x2a, 0, 0, 0x04, 0x37};
    static const uint8_t page[] = {0x2b, 0, 0, 0x07, 0x7f};
    if (!dcs_long(column, sizeof(column))) return -7;
    if (!dcs_long(page, sizeof(page))) return -8;
    uint8_t address_mode;
    if (!dcs_address_mode(&address_mode)) return -9;
    log[78] = address_mode;
    if (!dcs_short_param(0x36, address_mode & (uint8_t)~BIT(3))) return -10;

    log[DISP_STATE] = 4;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)FB_BASE;
    for (unsigned i = 0; i < FB_WIDTH * FB_HEIGHT; ++i)
        fb[i] = 0xff000000U;
    draw_text(0, "A5XELTE XNU");
    draw_text(1, "DECON DISPLAY READY");
    draw_text(2, "XNU ENTRY:");
    draw_text(3, "MMU ON:");
    draw_text(4, "EL1 FP ENABLE:");
    draw_text(5, "CPU TUNABLES:");
    draw_text(6, "ARM INIT JUMP:");
    clean_fb();
    mask(DECON + SHADOWCON, BIT(8), BIT(8));
    wr(DECON + FB_START, FB_BASE);
    wr(DECON + WHOLE_X, FB_WIDTH);
    wr(DECON + WHOLE_Y, FB_HEIGHT);
    wr(DECON + OFFSET_X, 0);
    wr(DECON + OFFSET_Y, 0);
    wr(DECON + OSD_A, 0);
    wr(DECON + OSD_B, (1079U << 13) | 1919U);
    wr(DECON + OSD_C, 0);
    wr(DECON + OSD_D, 0xffffffU);
    wr(DECON + WIN_MAP0, 0);
    mask(DECON + WINCHMAP0, 7, 1);
    wr(DECON + WINCON0, (15U << 13) | (4U << 2) | BIT(1) | BIT(0));
    mask(DECON + SHADOWCON, BIT(8), 0);
    wr(DECON + UPDATE, BIT(0));
    wr(DECON + VIDCON0, BIT(1) | BIT(0));
    wr(DECON + UPDATE, BIT(0));
    mask(DECON + TRIGCON, BIT(4), BIT(4));
    log[DISP_DSIM_STATUS_AFTER] = rd(DSIM + 4);
    log[DISP_DECON_AFTER] = rd(DECON + VIDCON0);
    log[DISP_FB_START_AFTER] = rd(DECON + FB_START);
    log[DISP_SYSMMU_AFTER] = rd(SYSMMU);
    log[DISP_STATE] = 5;
    return 1;
}
