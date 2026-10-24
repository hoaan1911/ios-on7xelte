/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdint.h>

/* S-Boot scanout buffer (uniLoader board-herolte.c). */
#define HERO_FB_BASE 0xe2a00000UL
#define HERO_FB_WIDTH 1440U
#define HERO_FB_HEIGHT 2560U

/* Let CPU writes reach the panel (DECON_F HW trigger), returns old value. */
uint32_t hero_fb_enable(void);
void hero_fb_text(unsigned row, const char *s);
void hero_fb_hex(unsigned row, const char *label, uint64_t value);
