# Galaxy J7 Prime (SM-G610F, Exynos 7870) on the A5XELTE XNU

The J7 Prime reuses the A5XELTE kernel configuration (4 KiB pages, Cortex-A53, GICv2): only the device
tree (tools/mkadt_j7prime.py) and the S-Boot loader (shim/j7prime/) are new.

Measured on the phone with a probe (hand-off facts):
- S-Boot runs the BOOT "kernel" at 0x40080000, EL1, MMU off, x0 = device tree at 0x48000000
- the ramdisk (our payload) lands at 0x43000000
- CPU Cortex-A53 r0p4, ID_AA64MMFR0_EL1 = 0x1122: no 16 KiB pages, 4 KiB fine; CNTFRQ_EL0 reads 0 (26 MHz)
- GICv2 at 0x104e1000 / 0x104e2000; eMMC at 0x13540000; scanout 0x67000000, 1080x1920, stride 4320,
  and CPU writes to it reach the panel directly (no DECON handoff, unlike the A5)

## Build order
1. Actions > xnu > Run workflow: branch herolte, machine_config = A5XELTE. Wait for it to finish.
2. Take the number from that run's URL (.../actions/runs/NUMBER).
3. Actions > j7-boot > Run workflow: kernel_run_id = NUMBER. Download the artifact j7-xnu-boot.
4. Flash j7-xnu-boot.img to BOOT (TWRP Install Image, or dd to /dev/block/mmcblk0p10).

First boot expectation: the Samsung logo, then "J7 PRIME XNU LOADER / PAYLOAD READY / STARTING XNU", then
the kernel's own early text blocks. The loader log page (marker J7XNU001) is at 0x47041000, inside the
sec_avc_log reserved area (readable in TWRP, unverified).
The first milestone uses the in-memory root (rd=md0); no partition is touched.

## Wiping Android later (only when the root partition step comes)
Safe to erase and reuse: SYSTEM (p21), VENDOR (p22), CACHE (p23), USERDATA (p25).
Do NOT touch: BOTA0/BOTA1 (S-Boot itself), PARAM, EFS (p3), CPEFS (p4), m9kefs1-3 (p5-7), PERSISTENT (p17),
PERSDATA (p18), STEADY (p19), RADIO, CARRIER, RECOVERY (TWRP). EFS and the modem partitions hold the IMEI
and radio calibration: back them up first, from TWRP:
  for p in 3 4 5 6 7 9 17 18 19; do dd if=/dev/block/mmcblk0p$p of=/sdcard/mmcblk0p$p.img; done
and copy them to the PC with adb pull. The stock firmware (Odin) can always bring Android back.

## Cores and eMMC speed (patches 0304, 0305)
The A5XELTE kernel used to be capped at ONE CPU (SWEET_SMP_MAX_CPUS 1, SweetCPU panic unless num_cpus == 1), and
the boot args had cpus=1. 0304 shares the S7's working PSCI CPU_ON + GICv2 SGI bring-up (0208/0218/0219) with
this board; the ADT (mkadt_j7prime.py) describes one XNU cluster of 8 (MPIDR 0..3, 0x100..0x103).
Test order, one flash each: `cpus=4` (boot cluster; known to start on this SoC family), then `cpus=8`.
Expected log lines: `A5XELTE CPU: cpu=N mpidr=0x... came alive; SGI1 OK`, `8/8 CPUs initialized`, and the
timer test / vct_lag lines (lag should be < 1 ms and timers must wake on all 8). If cpu4..7 time out, PSCI for
the second cluster (CPU_ON target 0x100..) is the suspect: report the `CPU_ON rc=` lines.
0305 adds multi-block eMMC reads, off unless the boot args contain `emmcfast=1`; try it after cores work.
Do not expect miracles from either: the UI is still software-rendered on the CPU (no GPU), so cores help most.

## GPU bring-up stages (boot-arg j7gpu=N, Mali-T830 MP1)
1 publish resources only; 2 also read PMU G3D status + CMU+000; 3 also read GPU_ID and the Midgard identity
registers (confirmed on hardware: GPU_ID 08300010, 3 job slots, 8 address spaces, shader/tiler/L2 mask 1);
4 (0308, passed on hardware with cpus=8) also soft-reset the GPU and power-cycle L2/shader/tiler, no memory or jobs;
5 (0309, passed on hardware) also runs one WRITE_VALUE job on JS1 through a private page table (the A5's test);
6 (0310) runs the real job manager's selftest: page-table pool, BOs at kernel-chosen GPU VAs, chained jobs across BOs,
free/re-create at the same VA (TLB flush), a deliberate MMU fault + reset. Look for `job manager selftest PASSED` (passed on hardware).
7 (0311) keeps the GPU powered and serves it to user space through the J7MaliUserClient IOUserClient (selectors: 0 map,
1 unmap, 2 submit, 3 wait, 4 info, 5 log) and first drives it from a kernel-side client (kernel_task) at boot:
look for `client selftest PASSED`. Drop 0311 if it does not compile; 0304-0310 stand alone.
The PMU and CMU pages have holes that raise an external abort (fault_code 0x10): never sweep them, read named
registers only (0306, 0307). Kernel log: SYSTEM is mmcblk0p21 on the J7 Prime (not p20 as on the A5).

## iPhone 8 Plus root and identity (boards d21ap / d22ap)
The iOS root is built from one iPhone's IPSW and the ADT must claim the same board: SpringBoard, MobileGestalt and the
artwork look up `<board>.plist` and the `/product` node of exactly that device.
- `ios-root` now defaults to iPhone10,2 (iPhone 8 Plus, board d21ap; 1080x1920 with a Home button, the closest match to
  the J7 panel). `ipsw_device`/`board` can still select iPhone X (iPhone10,3 / d22ap). `tools/a5xelte-stage-ios-root.sh`
  also moves the stock apps iOS parks in `/private/var/staged_system_apps` (Mail, Maps, Calendar, ...) into
  `/Applications`, because iOS's own first-boot install of them fails here.
- `j7-boot` takes `board` (default d21ap) and, in a private repository, downloads that IPSW's device tree and grafts its
  real `/product` node into the ADT (`tools/mkadt_j7prime.py --board d21ap`). Without `/product`, Settings said "iPad" and
  General > About had a blank model name.
- Always flash a root and a boot image of the SAME board together. A D21AP boot image on an iPhone X root (or the
  reverse) misses the board's plists.

## GPU in user space: delivery path (0312, j7gpu-dylib, j7-push-lib)
No SSH and an HFS+ root mean a library cannot just be copied to the phone, so (as in the Pixel repo) the root image
reserves `/usr/lib/sweetgl.dylib` as a real 64 MiB file (marker `J7GLHOLD`, then zeros) and its on-disk extents are
published with the ios-root release as `sweetgl-extents.txt`.
1. Build the library: Actions > `j7gpu-dylib` (macOS runner) > artifact `sweetgl-dylib` (a 64 MiB padded, ad-hoc signed
   `sweetgl.dylib`). Today it is `tools/j7gpu/j7gputest.c`; Mesa's Panfrost driver replaces it later under the same name.
2. In TWRP, from the folder with adb: `python j7-push-lib.py sweetgl.dylib sweetgl-extents.txt` (only that file's blocks are
   written, then read back and compared). No root reflash is needed for later library versions.
3. Boot with `j7gpu=7 sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib` (kernel patch 0312 injects the
   variable and answers dyld's AMFI policy question so it honours it). A placeholder that cannot be loaded is skipped.
4. The library runs the checks of the kernel-side client selftest from inside backboardd and writes them to the kernel
   log: look for `J7 G3D client: dylib: ... j7gputest PASSED`.

## Mesa Panfrost on iOS (tools/sweetgl-mali), first steps
- `sweet_drm_mali.c` + `include/xf86drm.h`: libdrm and the Panfrost DRM ioctls in user space over the kernel's
  J7MaliUserClient (CREATE_BO maps user memory into the GPU, SUBMIT = selectors 2+3 run to completion, GET_PARAM
  from the identity registers through selector 6 / patch 0314). `sh tools/sweetgl-mali/test/run.sh` runs it against
  a fake kernel client with sanitizers (passes).
- `patch-mesa.py` (two asserted edits: BO mmap -> sweet_drm_bo_map, BO munmap -> no-op) and the `mesa-build` workflow
  (macOS runner, Mesa 24.3.4, `-Dgallium-drivers=panfrost`, static libs, `ninja -k 0` so one run lists every error).
  Not yet built: send the uploaded `mesa-build-logs` artifact (or the `error:` lines) and fix from there.
- Patches 0312 (AMFI answers dyld, policy renamed "AMFI") and 0313 (sweetenv) are independent: if the xnu build
  skips one ("Skipping patch" in its log), the other still applies. 0314 adds selector 6.
- mesa-build, second round: the first run built 213 of 214 steps; the one failure was `drmSyncobjTransfer` missing
  from the libdrm stand-in (Valhall's CSF code), now added together with the other sync functions. The core libraries
  (state tracker, gallium auxiliary, NIR, GLSL) were never requested because nothing consumes them without an
  EGL/DRI target, so the workflow now asks ninja for every library by name and then links all objects into a
  throwaway dylib with `-undefined error`: `link.log` lists every symbol still missing.
- 0313 (sweetenv) was rewritten against the real kern_exec.c from the xnu-peek artifact: exec_add_user_string takes an
  int segment here and the first attempt's context did not match.

## backboardd's NULL in IOSurfaceClientGetBulkAttachments (patch 0320, 2026-11-08)
- The GPU job no longer fails (log: `heap BO of 131072 KiB is 32768 KiB`, no `submit ... failed`), and CoreAnimation's
  first shaders now compile on the GPU path (`hakios-gl: the first shaders`, `SWEET SURF: backboardd create id 2
  750x1334 fmt 0x42475241 ...`). backboardd then dies, 11 times a boot, at `IOSurfaceClientGetBulkAttachments+0x2c`:
  `sig 11 far 0x30`, pc in `_platform_memmove+0xb0`, lr `IOSurface+0x4ffc` (unslid `0x1d1b01e70`, the same as before the
  heap fix). From the crash reporter's registers: `memmove(dst = stack, src = 0x30, len = 0x70)` with
  `x0 = 0x1704a3100` (= sp + 0x20).
- 0x30/0x70 is `state + 0x30`, the 0x70 bytes of bulk attachments in the shared state page (NOTES: lock result +0x08 is
  that page). So `IOSurfaceClient + 0x8` is NULL: a client built out of a lock result that had no base address and no
  state page, not a real surface of ours.
- The last surface call before the crash is `SWEET SURF: backboardd sel 6` = `WrapClientImage`, which the kernel's
  `IOSurfaceRootUserClient` did not implement: the default case answered success with zeroed outputs, and the framework
  builds an `IOSurfaceClient` out of whatever a wrap/lookup/create returns. Patch **0320** refuses 6 and 7
  (`WrapClientImage`, `WrapClientMemory`) with `kIOReturnUnsupported`, so no such client is built, and logs
  `SWEET SURF: <proc> wrap sel N refused: scalars ..., struct in ..., out ...` once per selector so the next boot says
  what the framework actually asks for if the wrap turns out to be needed. Not phone-tested: needs an `xnu` build.
- The signature above comes from `tools/sweetgl-mali/eagl/sweet_crash.c` (built into sweetgl.dylib by `mesa-build`,
  installed in backboardd by `sweet_backboardd.c`): `J7 G3D client: crash: ...` plus a 14-frame `dladdr` walk, which is
  what turned the shared-cache addresses into `IOSurface+0x4ffc` / `QuartzCore+...`.

## Compared with the Galaxy S7 that has GPU acceleration (wardevsa/ios-a5xelte, branch `herolte`)
- The backboardd crash above is their **`0244-iosurface-wrap-refuse-drop-trunc.patch`**, found independently and for the
  same reason ("the default case's zeroed success left a client with NULL pointers, and IOSurfaceGetBulkAttachments
  faulted on NULL+0x30"). Their selectors 6/7 comments match ours (`6` = wrap client image, struct in 0x20; `7` = wrap
  client memory, scalars [address, length]) and they note what we assumed: refusing makes the library return NULL and
  the caller, CoreAnimation's image upload, takes its copying path. 0320 therefore agrees with a tree whose GPU path
  runs, which is the strongest check available while our build is unverified.
- `0255-sweetsurface-client-address-and-diag.patch` -> **0321**: a surface created with `IOSurfaceAddress` (the client's
  own memory) must start with those pixels; ignored, it stayed zeroed and CoreAnimation's 180x180 app icons never
  appeared. Its per-key property dump is left out here as noise.
- `0244` also fixes their `herolte_drop.c` (vnode_open leaves O_TRUNC to its caller, so a shorter file kept its old
  tail). This tree has no `herolte_drop.c`; if a short `/usr/lib/sweetgl.dylib` push ever leaves a stale tail, that is
  the shape of the bug.
- Not ported, and why: `0252`/`0254` round every surface's row pitch up to 64 bytes so Panfrost can import it - that is
  for their surface-BO import, while our bridge copies through OSMesa (`gl_sweet.c` returns "not done"), and 0254 had to
  make it `sweetrowalign=1`-only because the keyboard then came out skewed; `0248-herogpu-surface-bo` is that import
  itself, a feature our bridge does not have; `0317` is WebKit-only logging; `0276` replaces the scaled default surface
  with its own memory (ours is still 0222's `fScaled ? fLogicalW : fWidth` over panel memory), a display-scaling
  difference to revisit once drawing works, not a crash.

## First boot with the GPU up: logo and no lock screen (2026-11-08)
- `0320` holds: the boot has no IOSurface fault (11 per boot before) and `sel 6` is refused once per upload. `0321` is
  doing its job too - `SWEET SURF: pixels of 120x120 taken from client address 0x… -> 0` for every client-owned surface.
- Two faults of their own, in order of importance:
  1. every submit fails `status=-3 js=8` (MMU fault; the kernel's own intentional-fault round prints the same js=8, its
     good rounds pass), so nothing on the panel was drawn by the GPU - the logo is iBoot's;
  2. after ~6 minutes CREATE_BO returns ENOMEM and backboardd dies in Mesa's `submit_batch` on the NULL BO. The kernel
     BO table was 64 entries and CoreAnimation maps one per IOSurface.
- `0322` raises the table to 512 and adds the initial fault address/BO report and map-refusal reason.
- **Next boot-log result:** the BO-table exhaustion and backboardd NULL-BO crash are gone; the selftest's valid submission passes, while all actual submits still fail `-3 js=8`. The reported VAs include `0x38e2440`, the job-chain head in BO 11, and `0x3d45000`, BO 18's first page. Correctly decoded Mali status bytes `0xc2`/`0xc3` indicate level-2/level-3 translation faults (READ/WRITE), with source IDs `0x7c10`/`0x7c20`. A single pool initialization is logged. No GPU frame or lock screen has been observed; root/table visibility remains unresolved.
- `0323` adds TRANSTAB readback, a bounds-checked page-table walk for the fault VA, decoded fault fields, and a 16-entry successful BO map/free ring. It applies cleanly to the reconstructed source including 0322. On the next boot, pull `J7 G3D: walk transtab=...` and `J7 G3D: recent ...` lines to determine whether the root, table chain, leaf, or BO lifetime is wrong.
- The build copies `xnu/patches/*.patch` into the xnu source tree, so each file must be the kernel diff itself. `0323-j7-g3d-fault-walk.patch` must stay a single patch of `iokit/Kernel/arm/A5XELTEGPU.cpp`; an exported `git format-patch` (repo-root paths plus a `create mode` entry) cannot apply there and silently leaves the walk out of the kernel.
- Patch `0324` (`xnu/patches/0324-j7prime-g3d-mmu-ready-guard.patch`) is the same kernel diff, generated from the reconstructed tree and byte-verified end to end: `17ab0a1` -> apply `0323` -> apply `0324` -> result matches the committed 0323+A+B+C target. It adds `bool mmuReady;` to `gJ7M`, sets `gJ7M.mmuReady = true;` in `j7m_serve_start` after `j7m_mmu_enable()` succeeds, and refuses selector-2 submit with `kIOReturnNotReady` when `!gJ7M.mmuReady` before `j7m_cache`+`j7m_submit`. It is one file, `iokit/Kernel/arm/A5XELTEGPU.cpp`, 33 lines. It is a hypothesis (first submit faults because AS0/MMU is not yet ready), to be confirmed or refuted by the next boot's `walk`/`recent` lines; if the fault is a wrong root or stale leaf, revert this guard and fix the real cause.

## 2026-11-09 — 0328-gpu: verify every BO page before submit (slowness fix)

0327 boots to the lock screen with GPU rendering. The remaining defect is the frame rate:
0327 checked only a 4096-page slice per submit, so a BO outside the slice still faulted
(`fault at 300b040 ... l3=0`), the job slot faulted (`js=8`), the wait ran into the
client's 5 s timeout (`status=-1`, `frames=1 elapsed_ms=5250`) and Mesa dropped the batch.
0328 therefore checks the whole address space of every live BO before each submit, against
PAs cached when the BO was mapped. This is a pure table walk (no descriptor walks) with one
flush per submit instead of per-BO. Expected effect: the 5-second wait timeout per frame
disappears, frame rate improves from ~0.1 fps to a much higher rate.

The console mute (stopping log text on the panel) is deferred to future work — it targets
`osfmk/console/serial_console.c` which is not in this repo. In 0327, `a5fb_noconsole=1`
only mutes `_cnputs()` (the ring drain) but the video console paints through
`_cnputc()` → `cons_ops[VC_CONS_OPS].putc`, so log text still appears on the panel.
## 2026-11-10 — device result of 0328, and the real bottleneck (0329/0330)

**Device observation (boot log + photos).** 0328 boots, does not panic, and the lock-screen clock is
white instead of red: the compositor is on the GPU. It is still unusably slow (the clock jumped from
7:11 to 11:xx while waiting, and the screen glitches). The boot log says why:

- `frames=1 elapsed_ms=5321 fps=0.1` — every present waits out ~5.3 s, i.e. the job slot never
  completes and the client gives up on its timeout.
- `client map refused: a physical segment or the page-table pool (135 of 512 BO slots used, 512 of 512 page tables)`
  — the 2 MiB page-table pool is exhausted.
- then `fault at 4fcac00 in bo 47 ... faultstatus=000001c2 (translation level 2 execute)` and
  `submit jc=... slot=1 failed status=-3 js=8`: the refused map left a window with no L3 table.
- `CRASH backboardd ... submit_batch`: sweetgl dereferences the NULL BO, backboardd restarts, and
  that restart is the glitching screen.

So the 5-second stalls are pool exhaustion, not verification cost.

**0329 — panel console.** 0328 gated `_cnputc` with `idx == VC_CONS_OPS`, but this board has no Apple
video console: `cons_ops_index` stays on `SERIAL_CONS_OPS` and `serial_putc_options` falls back to
`fbcon_putc()`, which is what paints the panel. The gate never fired, which is why the log text is
still at the bottom of the panel. 0329 mutes every console op once the GUI has presented; `_cnputs()`
has already appended the line to the persistent eMMC log by then and panics still draw.

**0330 — page-table pool.** Ask for 16 MiB (4096 tables), halving until a physically contiguous block
exists, recycle an L3 table as soon as its whole 2 MiB window is empty (free list reused by
`j7m_table_new`), and store only the tables that exist on each sync instead of the whole pool.

**Patch bookkeeping.** `0328-j7-g3d-verify-every-page.patch` never applied — it sorted after
`0328-j7-g3d-verify-every-page-and-mute-console.patch` and the build log reported `Skipping patch
... 0328-j7-g3d-verify-every-page.patch`, so the aggressive every-page verification that boots is the
one in the `-and-mute-console` file. The duplicate file is removed so the tree has one 0328 again.

## 2026-11-10 (second boot of 0328+0329+0330) — pool was not the cause

**Device observation:** console text still at the bottom, only marginally faster, and it panics after
a while. The boot log has no panic: `debug_log_init: Error!! gPanicBase is still not initialized`
means panic-time logging is not reaching the persistent log, so the log ends mid-run.

**What the log proves**

- `J7 G3D: serving user clients (... page-table pool 4096 tables)` and maps of 4096 continued to be
  accepted (`tables 4/4096`), so 0330 did what it said. It did **not** fix the stalls: submits still
  fault (`fault at 3010400 in bo 5 ... translation level 2 execute`, `walk l2=83e71003 l3=0 leaf=0`)
  and `fault repair: bo=44 va=39c1000 fixed=95` rewrites the same BO's leaves repeatedly, with the
  table count climbing 705 -> 1863 -> 2546 -> 2914 of 4096. A **whole 2 MiB window entry** is being
  cleared under a BO that is still live, and repair only patches the pages the next job touches.
- `frames=1 elapsed_ms=5328` is a 300-callback window, i.e. mostly idle (no swap, no frame); the busy
  windows show `frames=22 elapsed_ms=5549` (about 4 fps) with `avg_copy_us=12000`. So the 5 s number
  is a frame-rate metric artefact, not a per-frame wait; the real cost is the fault/repair churn.
- The console mute never ran: `AppleCLCD::present()` mutes inside a `fConsolePaused` block, and that
  member is never initialized (the IOKit allocation is not zero-filled), so a first swap that read
  it as true skipped the mute - and the panel clear with it - for the whole boot.

**0331** initializes `fConsolePaused`, `fDirty`, `fPowerOff` and `fFadeStep` in `start()` and logs the
mute, so the next log answers "did the mute run" directly.

**0332** stops paying for the 17k-page whole-AS verify on every submit (it re-walks only when the
driver's map generation moved, plus every 32nd submit), audits both clear paths and prints
`audit: <caller> cleared <va>+<len> but live bo=<n> owns <va>+<len>` when a live BO is hit (that line
is the answer to the leaf/entry vanishing), and raises the pool ceiling to 64 MiB with a one-shot
"pool exhausted" line so exhaustion is never a silent timeout again.

## 2026-11-10 panic on the 0331+0332 build — symbolicated

The device panicked after a while, with the panel still drawing the "slide to power off" UI. That
panic **is** captured on the panel but not in the persistent log (`debug_log_init` says
`gPanicBase is still not initialized`, so panic-time kprintf has nowhere to go).

```
Kernel data abort at pc 0xfffffff007a4ab88, lr 0xfffffff007a48c88
esr 0x96000004   (EC 0x25 data abort, DFSC 0b000100 = translation fault, level 0)
far 0x00000d000b2a1000
```

Resolved against `kernel.development.a5xelte` from xnu run 42804501140 (`__TEXT` vmaddr
`0xfffffff007004000`): **`j7m_unmap_page+0x58`, called from `j7m_map_user+0xc68`**.

`j7m_unmap_page` walked the page tables trusting every entry (unlike `j7m_lookup`, which checks
`j7m_in_pool()` at each level). One slot holding anything that is not a pool table — a leaf, or the
junk that also makes leaves disappear — became `j7m_table(garbage)`, i.e. a wild pointer, and the
CPU took the abort inside the **map** path. That also means the map path itself is where the
corruption surfaces, which fits the table count climbing while leaves vanish under live BOs.

**0333** guards all three levels (and logs the offending entry once instead of dereferencing it),
removes the five-second stall (a slot that is already stopped/faulted never raises the completion
interrupt, so the wait used to burn the whole client timeout — now 2 ms of grace and report it
dead), and raises the per-job repair ceiling 8 -> 64.

**0334 + `tools/mkadt_j7prime.py --iphone8plus`** let the internal resolution exceed the panel, so
the iPhone 8 Plus canvas (1242x2208 @3x, display-scale 3) is rendered and shrunk to 1080x1920. The
default surface is a view of the panel frame buffer and stays clamped to the panel.
Boot args for it: `sweetfb_scale=1 sweetfb_lw=1242 sweetfb_lh=2208`.
Note the cost: 2.7x the pixels of 750x1334, so this is not the setting for speed.

## 2026-11-11 — no panic, console muted, and where the frame time actually went

The 0333/0334 boot came up, the panel console stays muted, and the "slide to power off" UI is
drawn by SpringBoard's own compositor. The log is unambiguous about the remaining slowness:

```
J7 G3D: config jm=00000000 shader=00010000 tiler=00000001 l2_mmu=00000000
J7 G3D: slot 0 stopped without an interrupt (js=00000008), not waiting
J7 G3D client: submit jc=3dae4c0 slot=0 failed status=-1 js=8
A5XELTE FB: frames=1 elapsed_ms=5309 fps=0.1 bytes=8294400 avg_copy_us=15048
```

Every client chain ends in `status=-1`: the job slot never raises its interrupt. Two things made
that cost seconds per frame rather than microseconds:

1. `j7m_wait` returned -1 with `gJ7M.inflight` still set, so the *next* submit was refused with
   `kIOReturnBusy` until something cleared it.
2. user-client case 3 then ran `j7m_recover()` - a full soft reset, cores off, MMU torn down and
   rebuilt, every mapping re-created - for every failed chain.

0335 removes both: a dead slot clears both IRQ words and `inflight` itself, the client timeout is
capped at 600 us, the whole-address-space verify walk is gone from the submit path, and a reset
only happens after four consecutive failures (or immediately after a real MMU fault).

The `js=8` in the logs was also misread for several builds: +0x24 is JOB_HEAD_HI, not JS_STATUS.
JS_STATUS is +0x20 and its bit 0 is ACTIVE; the early-out now reads that.

**Battery.** SweetBattery was compiled for SWEET and HEROLTE only, so powerd fell back to the 1 %
icon. This board's gauge is the S2MU004 block at 0x3B on `i2c@13830000` (Samsung
`s2mu004-fuelgauge`, sibling of the S2MPU03 that `a5xelte_touch.c` already drives): 0336 adds
`bsd/dev/a5xelte_battery.c` (RSOC 0x08, RVBAT 0x04, RCUR_CC 0x06, RBATCAP 0x0e, polled
S3C2440-layout transfers) and enables SweetBattery for A5XELTE with the 3300 mAh design capacity.
`j7fg=0` disables it.

**Storage** already works: `A5XELTE eMMC: GPT USERDATA ... writable`, BSD root `emmcroot0`, HFS
mounted read-write, and the userland above it writes (SpringBoard, the log ring). What is still
absent is the IOMedia/disk0 layer, which is what DiskArbitration and Settings' storage summary
enumerate - that is a separate piece of work, not a write path.

## 2026-11-11 (third boot) — the battery never probed, and every client chain still dies

**Battery: the class was never matched.** This boot has no `SWEET BATTERY:` line and no
`A5XELTE FG:` line at all. `iokit/KernelConfigTables.cpp` guards the personality with

```
#if !defined(ARM64_BOARD_CONFIG_A5XELTE) || defined(ARM64_BOARD_CONFIG_HEROLTE)
```

so on this board `SweetBattery` was compiled and linked but no personality ever matched it -
`start()` never ran, so 0336's fuel-gauge probe was never called. 0337 drops that exclusion
(AppleCLCD keeps its own A5XELTE guard - this board presents through SweetFB). The next log must
show `SWEET BATTERY:` and either `A5XELTE FG: S2MU004 answers, RSOC=...` or
`A5XELTE FG: nothing at 0x3b ...`.

**GPU: no client chain reaches the GPU at all.** The 0335 build behaves exactly like the one
before it: `submit jc=... slot=0 failed status=-1 js=8` for every Mesa chain, while the kernel's
own client selftest on the same slot reports `round 0 (good) status=0 js=00000000` and its
deliberate fault reports `status=-3 js=00000008`. The whole 494 KB log contains **one**
`fault at` line - the selftest's own - so no Mesa chain even reaches the MMU fault path. Also
notable: 0335 capped the client timeout at 600 us, which silently disabled the dead-slot check
(it only ran from 2 ms), so this boot timed out blind.

0338 makes the driver answer the question instead of guessing: the first four failed submits print
JS_STATUS at +0x20 and +0x24, JS_CONFIG_NEXT, JS_COMMAND_NEXT, JOB_IRQ_RAWSTAT plus the next two
words, MMU_IRQ_RAWSTAT, AS0 fault status and TRANSTAB, and GPU_FAULT_STATUS. The dead-slot check now
starts at 150 us so it runs inside the cap, and a latched JS_COMMAND_NEXT is cleared after a failure
so a START the job manager never took cannot wedge the slot.

**Storage.** `disk0` cannot exist yet: this XNU tree has no IOStorageFamily at all - there is no
`IOMedia.cpp`, no `IOBlockStorageDriver.cpp`, nothing under `iokit/Families/` but IONVRAM and
IOSystemManagement. `IOService::setRootMedia` only survives because patches 0021/0103 tolerate the
missing `IOBlockStorageDriver` class. The eMMC block device (`emmcroot0`) is served directly from
`bsd/dev/a5xelte_emmc.c`, which is why HFS mounts and the userland writes, and why nothing appears
in DiskArbitration, `df`, or Files' on-device storage: those enumerate IOMedia, which does not exist
in this kernel. Adding it means porting IOStorageFamily (IOMedia + IOBlockStorageDriver +
IOMediaBSDClient), not another eMMC patch.

## 2026-11-11 (fourth boot) — the dump answers the GPU question, and the gauge ACKs but stalls

**The battery now probes** (`0337` worked): `SWEET BATTERY:` appeared and called the gauge -
`A5XELTE FG: write of 1 byte(s) failed stat=0xf0 con=0xe1`, then `nothing at 0x3b`. `IICSTAT`
0xf0 is the address phase with START still asserted and ACK clear at bit 0, and `IICCON` 0xe1 has
the clock prescaler (bits 1:0) at zero; the touch bus on this phone, which is device verified,
writes 0xe3 and also sets the SDA filter at 0x10. **0340** uses those values.

**The GPU dump (`0338`) settles where a Mesa chain dies.**

```
J7 G3D: dump 2 slot=0 rc=-1 js_status(+20)=00000000 js_status(+24)=00000008 js_config(+58)=00003300 js_cmd(+60)=00000000
J7 G3D: dump 2 job_irq raw=00000000 stat=00000000 mask=00000000 mmu_irq raw=00000000 as_fault=00000000 as_transtab=8336d007 gpu_fault=00000000
```

- `js_cmd(+60)` reads 0: the START was **taken** by the job manager, not ignored.
- `js_config(+58)` is 0x3300, exactly the start/end clean+invalidate the submit wrote.
- `+0x24` is `00000008`, the same word the kernel's deliberate-fault round reports.
- `JOB_IRQ_RAWSTAT`, `MMU_IRQ_RAWSTAT`, AS0 fault status and `GPU_FAULT_STATUS` are all zero.

So the chain reaches the slot, the slot ends up faulted at **job level**, and no interrupt has
arrived at the moment the 600 us cap expires. 0335's cap was simply too short to ever see the
fault: earlier 5 s builds reported `status=-3` for the same chains. **0339** waits 2 ms, and once a
batch has failed it returns from the next 64 waits without polling - one probe per dead frame
instead of one per submit, which is the "everything waits for a GPU that never says yes" stutter.

**Storage is unchanged and remains a separate port**: `disk0` needs IOStorageFamily (IOMedia,
IOBlockStorageDriver, IOMediaBSDClient), which this tree does not contain at all.

## 2026-11-11 (fifth boot, six hours up) — the GPU does complete; my probe was lying

The log finally contains the completion the driver had been ignoring:

```
J7 G3D: dump 2 slot=1 rc=-1 js_status(+20)=00000000 js_status(+24)=00000008 js_config(+58)=00003300 js_cmd(+60)=00000000
J7 G3D: dump 2 job_irq raw=00000002 stat=00000000 mask=00000000 mmu_irq raw=00000000 as_fault=00000000 as_transtab=833cd007 gpu_fault=00000000
```

`JOB_IRQ_RAWSTAT = 0x2` is bit 1: the **slot 1 tiler chain completed**. `js_cmd(+60) = 0` means the
start was taken, `as_fault = 0`, `gpu_fault = 0`, and `js_status(+24) = 8` is the same word the
selftest's finished chains report. Nothing faulted.

So two of my own changes were the entire wall:

1. the "slot looks stopped" early-out fired at 150 us, i.e. while the slot is legitimately idle
   *between* the start and the completion interrupt, and reported a live chain as dead;
2. the counter it armed made the next 64 waits return -1 **without reading RAWSTAT at all**.

**0341** deletes both, waits up to 20 ms but returns the moment the interrupt lands, re-reads
RAWSTAT once before declaring failure, clears the whole interrupt word so a stale bit cannot fake
the next chain, and logs the first eight waits with their measured microseconds.

**0342** paints the scanout blue in `AppleCLCD::start()`, so an empty compositor frame is visibly
different from a dead panel (`sweetfb_bg=0` disables it). The first real frame replaces it.
