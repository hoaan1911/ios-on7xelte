/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef A5XELTE_DISPLAY_CONSOLE_H
#define A5XELTE_DISPLAY_CONSOLE_H

#include <stdint.h>

/* Bounded Exynos7580 display handoff. Diagnostics occupy log[64..95]. */
int a5_display_console_start(volatile uint64_t *log);

#endif
