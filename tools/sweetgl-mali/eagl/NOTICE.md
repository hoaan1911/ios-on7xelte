# tools/sweetgl-mali/eagl third-party notice

`eagl.c`, `eagl_slots.txt`, `hakios.gl.h`, `ca_hooks.c` and `mkeagl.py` are HakiOS's user-space OpenGL ES
bridge for CoreAnimation's render server, unchanged (copied from the Galaxy S7 port, ios-a5xelte-herolte,
`gpu/hakios-gl/`, which has them from https://github.com/oosexgang/hackios-xnu-kernel, `userland/`).
MIT License, Copyright (c) 2025-2026 the HakiOS authors; full text in `LICENSE.hakios`.
`include/IOKit/IOKitLib.h` is the IOKit declarations eagl.c uses (the iOS SDK has no IOKit headers).

`gl_sweet.c` and `sweet_backboardd.c` are adapted from the S7 port's `gl_hero.c` and `hero_backboardd.c`
(MIT) for this port's libOSMesa (Panfrost on the Mali-T830).
