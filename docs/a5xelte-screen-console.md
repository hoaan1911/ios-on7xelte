# A5XELTE early screen console

The visible Samsung logo is not proof that CPU writes to the bootloader's framebuffer address reach the panel. On the SM-A510F boot command line, `s3cfb.bootloaderfb=0x67000000`. TWRP reports a 1080x1920, 32-bit display with 4320-byte stride, but that describes TWRP's framebuffer after its display driver has started.

On 2026-10-19 a temporary Samsung loader wrote white text into physical RAM at `0x67000000`, cleaned those cache lines, and a temporary XNU entry routine wrote a green rectangle at the corresponding screen offset. The user saw only the Samsung logo. TWRP `/proc/last_kmsg` (SHA-256 `3be3f1cae0b2204c01a8f57642da748eb647f22f7e444d23c2493ac554a4059d`) contains loader marker `A5XNU002` at `0x19b1e1` and XNU-owned `XNUELTE1` at `0x19b2e1`. The CPU ran both stages; the direct framebuffer writes were not visibly presented. That temporary code was removed from the ongoing loader and patch series.

## What the working Linux port does

The verified native driver is `mainline/drivers/exynos7580-drm-handoff.c` in the Linux port. It uses DECON at `0x14830000`, DSIM at `0x14800000`, display SysMMU at `0x14860000`, the display power-domain status at `0x10464084`, and six display clocks from the native clock driver. Its primary scanout register is DECON `FB_START` at offset `0x880`. The address written there is the DMA address of a newly allocated XRGB8888 buffer; it is not a fixed `0x67000000` mapping. The Linux record contains user-verified physical kernel text and color bars after this handoff.

The driver's `pipe_enable()` waits for the current command-mode frame, stops and resets DECON, disables only the display SysMMU after DMA stops, sets window format/stride/size/trigger registers, prepares DSIM while retaining firmware panel power, restores the panel's DCS column and page ranges, sets the scanout DMA address, then starts DECON and the software trigger. It checks the power domain and DSIM state before touching the display. The Samsung logo can remain in panel GRAM while there is no active scanout; this is an inference from the failed RAM-write test and the driver sequence.

## Device-verified DECON handoff

The A5XELTE loader now checks the powered display domain and DSIM dimensions, enables the six gates used by the Linux driver, stops and resets DECON with bounded waits, and disables the display SysMMU only after stopping DMA. It programs a new 1080×1920 XRGB8888 scanout at physical `0x67000000`, restores DCS column/page/address mode, then starts DECON. These steps are in `shim/a5xelte/display_console.c` and are independent of iOS userland.

On 2026-10-19 the user saw `A5XELTE XNU`, `DECON DISPLAY READY`, `XNU ENTRY:`, `MMU ON:`, a green XNU entry block and a blue XNU post-MMU block on the physical SM-A510F. TWRP's recovered reserved log recorded display state `5`, DECON `0x8010010f`, FB_START `0x67000000`, and the XNU-owned entry marker. This verifies scanout and XNU execution after enabling the 4 KiB MMU. The user subsequently verified `XNU ENTRY` and `XNU MMU ON` drawn by XNU assembly with its own `iso_font`. The ADT framebuffer serial console has not yet shown dynamic text on the phone.

## Next implementation boundary

The loader already captures DECON, DSIM, SysMMU and power status in reserved RAM and performs the bounded handoff. Three further XNU assembly blocks appeared after EL1 FP enable, CPU tunables and immediately before return to `arm_init`. The immediate task is locating the stop between that return and the first C stage. Once XNU reaches C initialization, confirm `a5xelte-fbcon` actually maps the scanout buffer and displays `kprintf` output. Keep screen confirmation, recovered log contents, build success and iOS userland claims separate.
