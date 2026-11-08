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
