/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy J7 Prime early screen. S-Boot leaves a continuously scanned framebuffer at 0x67000000
 * (1080x1920, a8r8g8b8, 4320-byte rows) that CPU writes reach directly (verified with probe/),
 * so the A5's DECON handoff is not needed. Runs with the MMU off: aligned 32-bit stores only.
 */
#include <stdint.h>
#include "display_console.h"
#include "font.h"

#define FB_BASE 0x67000000UL
#define FB_W 1080U
#define FB_H 1920U
#define SCALE 4U
#define CELL_W ((5U + 1U) * SCALE)
#define CELL_H ((7U + 2U) * SCALE)
#define WHITE  0xffffffffu
#define YELLOW 0xffffff00u
#define BLACK  0xff000000u

static void glyph(char c, unsigned x, unsigned y, uint32_t fg)
{
    volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
    const unsigned char *rows = font_rows[0];
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 32);
    for (int i = 0; i < FONT_COUNT; ++i)
        if (font_chars[i] == c) {
            rows = font_rows[i];
            break;
        }
    for (unsigned r = 0; r < 7; ++r)
        for (unsigned b = 0; b < 5; ++b) {
            uint32_t col = ((rows[r] >> (4 - b)) & 1) ? fg : BLACK;
            for (unsigned dy = 0; dy < SCALE; ++dy)
                for (unsigned dx = 0; dx < SCALE; ++dx)
                    fb[(y + r * SCALE + dy) * FB_W + x + b * SCALE + dx] = col;
        }
}

static void line(unsigned row, const char *s, uint32_t fg)
{
    for (unsigned col = 0; s[col]; ++col)
        glyph(s[col], col * CELL_W, row * CELL_H, fg);
}

int a5_display_console_start(volatile uint64_t *log)
{
    volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
    for (unsigned i = 0; i < FB_W * FB_H; ++i)
        fb[i] = BLACK;
    line(0, "J7 PRIME XNU LOADER", YELLOW);
    line(2, "PAYLOAD READY", WHITE);
    line(3, "STARTING XNU", WHITE);
    log[77] = 0;
    return 0;
}
