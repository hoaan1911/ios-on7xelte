# A5XELTE GPU port: measured boundary and implementation order

Status: the phone reads Mali-T720 `GPU_ID=07201000`, patch `0130` completed
one isolated GPU memory write, patch `0132` GPU-rendered all 256 pixels of a
private 16×16 target, and patch `0133` GPU-rendered a green square directly
into the display scanout buffer, visible on the phone. An iOS GPU API bridge
and GPU-rendered SpringBoard frames have not been observed.

## Stage-1 code candidate

Patch `xnu/patches/0125-a5xelte-g3d-resource-service.patch` adds the
`A5XELTEGPU` IOKit service, and `tools/mkadt_a5xelte.py` supplies its
`/arm-io/gpu` nub. The service is disabled unless `a5gpu=1` is explicitly in
the BOOT command line. Even when enabled, it only publishes the exact physical
resource addresses and GIC SPI numbers listed below. The ADT nub omits an
`interrupts` property until a controller parent and real handler exist. It has no MMIO map,
power/clock/regulator writes, GPU interrupt handler or user client. Therefore
it cannot alter framebuffer performance yet. It is a compile and matching
foundation for the next hardware stage, with the existing CPU display path
untouched. `a5gpu=0` (or absent) makes the service decline to start.

The ADT node and IOKit service are separate: seeing `gpu` in the device tree
does not prove the service matched, and seeing `A5XELTE G3D: resource service
registered` proves only registration.

## Stage-2 passive probe candidate

Patch `0126-a5xelte-g3d-passive-id-probe.patch` adds a separate, explicit
`a5gpu=2` diagnostic mode. It first reads only PMU G3D status at
`0x10464064`. Unless its low nibble is `0xf`, it leaves the G3D clock and GPU
apertures untouched. If powered, it reads the native G3D CMU clock gates at
`0x11460800` and `0x11460900`; unless bits 0, 1 and 4 are set in both, it
does not read the GPU. It checks PMU status again immediately before mapping
GPU_ID at `0x11400000`, then decodes product bits 31:16 (`0x720` is expected
for Mali-T720). Each outcome is recorded as `A5G3DProbeResult` in IORegistry.
No register writes or GPU jobs are performed. `a5gpu=1` remains resource-only,
and absent/`a5gpu=0` remains disabled. The CPU display path stays active.

This probe can identify an *already powered and clocked* GPU; it cannot power
one on. The native Linux driver requires a live TOP bus parent rate, MIF/INT
interconnect votes, S2MPU03 BUCK4 voltage/enable sequencing, PMU domain
power-on and CMU divider/mux/gate programming. The XNU port has no coordinated
provider for BUCK4 or these bus clocks. Its touch driver owns the PMIC I²C
path, so an independent GPU-side PMIC write would also need bus serialization.
Without verified voltage and bus floors, copying Linux's PMU/CMU writes risks
an undervoltage fault or an external abort. Keep active power-on and job
submission out of `0126`. On the phone, the corrected ADT let the resource
service register and the passive probe printed `GPU_ID=07201000`, product
`0720`. This demonstrates a readable, already-powered Mali-T720 register at
that instant; no GPU job has run. The probe then panicked in `vm_map_delete`
when `ml_io_unmap` tried to remove the permanent mapping created by
`ml_io_map`. Patch `0127` switches those three short-lived maps to XNU's
`ml_io_map_unmappable`, matching the later `ml_io_unmap` calls. The phone's
0127 SYSTEM log contains the expected GPU ID, SpringBoard and backboardd
execs, and framebuffer timing samples without a repeated `vm_map_delete`
panic. It still says `no GPU jobs`.

## Stage-3 offscreen job candidate

Patch `0128-a5xelte-g3d-offscreen-write-value.patch` adds opt-in `a5gpu=3`.
Only after the existing PMU/CMU/ID checks, it checks shader/L2 readiness,
JS1 and unused AS0, allocates a private physically contiguous buffer, installs
four-level Mali page tables, and submits one `WRITE_VALUE` job into a private
8-byte output. It uses finite polls for MMU update and job completion and
reports the fault registers and output. It does not address the display buffer,
wire GPU interrupts, expose an iOS user client, or replace the CPU compositor.
The code linked into an A5XELTE Mach-O and was packaged into a 20,283,408-byte
BOOT candidate. The known local `dsymutil` `/dev/fd/62` restriction makes the
full install target exit after link. Mac ADB confirmed the a5xelte TWRP and
BOOT p10, saved the previous BOOT privately, wrote only p10, and rebooted.
Per the user's preference, no post-write hash comparison was run. The 0128
SYSTEM log shows `GPU_ID=07201000`, available AS0/JS1, but `shader=0` and
`l2=0`; the readiness gate correctly skipped submission. SpringBoard and
backboardd still executed. TWRP separately reported the GPU regulator at
787500 uV and disabled; this recovery observation is not a measurement of
its state during XNU boot.

Patch `0129-a5xelte-g3d-buck4-core-power.patch` reuses the established XNU
HSI2C3 PMIC transfer path before touch takes ownership. It checks S2MPU03
identity, reads BUCK4 selector and external-enable control, changes only the
BUCK4 voltage selector to the Linux-verified 900 mV first OPP, and requires
readback. It checks the inherited TOP clock mux and divider are no faster than
133 MHz before powering L2, shader and tiler in Panfrost's order with bounded
polls. A failure leaves the private GPU job unsubmitted. Success would prove
only an isolated `WRITE_VALUE` job; iOS graphics would still use the CPU path.

The 0129 phone log recorded the expected shader/tiler/L2 present masks but
`mux=0`, divider `0x30`. The conservative 0129 clock gate therefore stopped
before touching BUCK4 or submitting work. Patch `0130-a5xelte-g3d-safe-top-clock.patch`
mirrors the native Exynos7580 clock transition after BUCK4 is verified:
temporarily slow both dividers to eight, select TOP with a bounded mux-status
poll, select core /3 and peripheral /4, and check the final registers before
powering cores. The transition is refused while any GPU core is already ready.
The 0130 device log confirms BUCK4 readback at 900 mV, TOP mux `1` and
divider `0x32`, ready L2/shader/tiler masks `1/3/1`, and one completed private
Mali `WRITE_VALUE` job: job IRQ `0x2`, MMU IRQ `0`, fault status `0`, and a
64-bit sentinel changed to zero. The log also records SpringBoard and
backboardd starting without a panic. This establishes an XNU Mali execution
baseline. Fragment rendering, display buffer access, synchronization with
CoreAnimation, and iOS graphics acceleration are still separate work.

Patch `0131-a5xelte-g3d-private-fragment-clear.patch` adds a second opt-in
test, `a5gpu=4`, based on Mesa 26.2.4 Panfrost's Midgard v4 descriptor layout.
It submits one fragment job on JS0 against four private Mali-mapped pages: a
16×16 linear RGBA8 target, a single-target framebuffer descriptor, and the
T720 empty polygon list. The kernel fills the target with a sentinel first,
then checks that all 256 pixels become opaque red. It records job, MMU and GPU
fault registers and the first/last pixels. No display buffer is mapped and no
iOS rendering API is wired to this path. The 20,283,408-byte BOOT image was
linked, packaged, written only to BOOT p10 after saving the working 0130 BOOT,
and booted. The phone's SYSTEM log recorded JS0 IRQ `0x10000`, status `0x58`
(`DATA_INVALID_FAULT` in Panfrost), no MMU fault, and `0/256` red pixels.
SpringBoard and backboardd still executed. This is a failed fragment job, not
proof of rendered pixels.

Patch `0132-a5xelte-g3d-page-aligned-polygon-list.patch` gives the polygon
list its own 4 KiB-aligned page and moves the output to a separate page, as
Mesa's independent polygon BO allocation does. It also logs the job header's
exception and fault pointer plus GPU fault address/status. The 0132 BOOT was
linked and written only to p10. The phone's SYSTEM log recorded JS0 IRQ `1`,
no MMU or GPU fault, job header exception `1` (DONE), and `256/256` expected
red pixels. This confirms offscreen fragment rendering on the physical Mali.
The page-alignment hypothesis matches the successful result, though the log
does not isolate every possible field the hardware checked.

Patch `0133-a5xelte-g3d-direct-scanout-clear.patch` adds opt-in `a5gpu=5`.
It maps only 16 pages covering a 16×16 patch at `(528,944)` of the loader's
physical DECON scanout buffer `0x67000000` into Mali AS0 and submits the same
fragment clear with 4320-byte row stride and an opaque green value. The
private `a5gpu=4` test stays available. The working 0132 BOOT was saved
privately before only p10 was updated and the phone rebooted. The user saw the
green square on the physical panel. The TWRP SYSTEM log recorded JS0 IRQ `1`,
job-header exception `1` (DONE), and zero MMU, AS and GPU faults. This proves
that a Mali fragment job can write pixels into the active DECON scanout buffer.
The one-shot test does not provide CoreAnimation acceleration or ongoing frame
presentation.

Patch `0134-a5xelte-g3d-serialized-fence-selftest.patch` adds opt-in
`a5gpu=6`. It submits two driver-owned, private 16×16 fragment clears with
different colors through one job slot. The second submission follows only a
completed first fence. Completion requires a job IRQ, an idle slot, a DONE
job header, matching output pixels and no MMU/GPU fault. A timeout or
ambiguous result halts further submissions and keeps GPU-visible pages pinned.
The kernel linked successfully; the build wrapper reported only its known
post-link `dsymutil` file-descriptor permission failure. The 0134 image was
written to BOOT p10 without creating a new BOOT backup at the user's request,
and the device was rebooted. The TWRP SYSTEM log recovered 30,933 bytes from
this boot. It recorded `fence=1` and `fence=2`, each with JS0 IRQ `1`, job
header DONE `1`, `256/256` matching pixels, an idle slot and no MMU/GPU
fault. XNU then reported `4/4 CPUs initialized`; the captured log has no
panic. This proves ordered execution of these two private jobs on the phone.
It remains a boot-time diagnostic, not an iOS frame queue.

The queue ownership, fence semantics and route from IOSurface swaps to Mali
are specified in [a5xelte-gpu-queue.md](a5xelte-gpu-queue.md).
The installed iOS graphics interfaces and the current software frame path
are mapped in [a5xelte-ios-gpu-integration.md](a5xelte-ios-gpu-integration.md).

## What currently limits the screen

`xnu/iokit/Kernel/arm/SweetFramebuffer.cpp` is the inherited AppleCLCD bridge.
It describes the CoreAnimation CPU renderer, holds the latest swap record,
copies IOSurface rectangles with the CPU to the DECON scanout buffer at
`0x67000000`, and emits a 60 Hz timer notification. Its default
`sweetfb_hz=10` permitted a panel copy only every sixth timer tick. Patch `0124`
sets an A5XELTE default of 30 Hz and records actual cadence and copy cost.
Raising the
limit can improve visible update cadence, but it does not run icon composition
or any other rendering work on the Mali GPU. Measure copy duration and CPU
cost before choosing a higher default; a 1080 x 1920 XRGB8888 full frame is
8,294,400 bytes.

The 0124 phone log measured 23 present calls in 63.857 seconds (0.36/s)
despite the effective 30 Hz cap. Full-frame CPU copies commonly took about
5–6 ms and at most 11.6 ms in that interval. These data show the cap alone
and the final memcpy are not the observed source of the very low frame rate;
they do not yet identify the time spent rendering or restarting services.

`xnu/iokit/Kernel/arm/SweetSurface.cpp` maps the physical display surface
write-combined and currently accepts accelerator/shared-event methods without
implementing GPU execution or fences. `AppleCLCD` and `IOSurface` service
registration, a visible Apple logo, and an interactive SpringBoard prove a
display/input path, not a GPU path. A `GPU_ID` print would prove only a register
read.

## Hardware reference that already works under Linux

The native Linux device tree at `../../../mainline/exynos7580-a5xelte-gpu.dts`
describes this handset's Mali-T720 MC2 register aperture at `0x11400000`
(size `0x5000`), G3D clock controller at `0x11460000`, and shared PMU at
`0x10460000`. GIC SPIs are JOB 282, MMU 283, GPU 281. G3D uses S2MPU03
BUCK4, an internal Mali MMU, calibrated ASV/OPP voltages, MIF/INT bandwidth,
and TMU2 cooling. The source and device evidence are in
`../../../docs/gpu-native-migration.md` and `../../../docs/gpu-dvfs-native.md`:
Linux Panfrost/Mesa ran actual T720 jobs and Xorg/Qt rendering. Those results
do not transfer to XNU merely by compiling a kernel driver.

The Linux clock/power source
`../../../mainline/drivers/clk-exynos7580-g3d.c` checks PMU status `0x4064`,
configures the G3D domain and clocks in a bounded order, checks bus clocks
before high OPPs, and retains the domain on. This PMU is shared with other
systems. A guessed write, unbounded wait, or read from a power-gated G3D
aperture could stop boot. The native record in
`../../../docs/gpu-dvfs-native.md` also documents an external abort caused by
one invalid CMU offset; avoid a broad register dump on the phone.

## Port stages and proof required at each gate

1. **Baseline and safe identification.** Preserve the current BOOT candidate
   and record CPU usage, AppleCLCD swap/present counts, copied bytes, presentation
   duration and display cadence during the same SpringBoard workload. Keep
   GPU power and GPU MMIO untouched. Confirm the board-local `a5gpu=0`
   fallback in patch `0125` before any active driver test. A read-only PMU status probe may
   run only after the mapping and register offset are checked against the
   Exynos7580 native source; reading Mali `GPU_ID` additionally requires a
   positively confirmed powered/clocked domain. This is diagnostics, not
   acceleration.
2. **G3D resource owner.** Add an A5XELTE-only IOKit platform service with
   exact PMU/CMU/GPU ranges and IRQs. Port the Exynos7580-specific BUCK4,
   power-domain, TOP-parent, divider/gate, ASV/OPP, bandwidth and thermal
   ordering. All transitions need bounded waits, rollback and a boot-argument
   kill switch. First hold a conservative verified OPP; do not copy values
   from another Exynos model. Proof: device readback of the power state,
   clock rate, voltage and the `GPU_ID` product field `0x720` (register bits
   31:16, as decoded by native Panfrost), with no job submission yet.
3. **GPU execution and memory.** Adapt the Midgard T720 register/job/MMU
   behavior from the native Panfrost implementation to XNU-owned wired pages,
   cache maintenance, mapped Mali virtual addresses, JOB/MMU/GPU interrupts,
   completion fences, fault recovery and timeouts. A simple isolated clear or
   triangle into an offscreen buffer must complete and its pixels must match
   a known reference on the physical phone. The GPU's own MMU is used;
   no external SysMMU should be presumed. Treat a kernel driver that only
   reports an ID as an incomplete stage.
4. **iOS rendering integration.** Trace the exact graphics user clients and
   shared-library calls made by this installed iOS build. The private staged
   iPhone 8 Plus root contains `com.apple.Metal` and
   `com.apple.AGXMetalA11`; these are Apple/A11-side components, while this
   phone has Mali-T720. This is an inventory observation, not a claim about
   every possible fallback in the closed framework. The open XNU tree has no
   Mali-compatible Metal/IOAccelerator provider. Linux DRM/GBM/EGL and Mesa's
   Panfrost userland cannot be inserted into iOS CoreAnimation by a kernel
   device match. Implement and validate the iOS-facing surface, command and
   fence contract, or an explicitly selected compositor backend that can
   submit T720 jobs. Preserve a CPU fallback while this is incomplete.
5. **Scanout without a CPU full-frame copy.** Export linear XRGB8888/BGRA
   buffers that both the GPU and the Exynos7580 DECON path can address;
   synchronize GPU completion before scanout and retire old buffers only
   after presentation. Compare the exact color layout, row stride and cache
   behavior with the working Linux DMA-BUF/DECON result. A new logo, a
   faster timer, or a successful `present()` log is not GPU proof.

## Acceptance before saying "GPU accelerated iOS"

- On the phone, known GPU-produced pixels pass readback, and JOB completion
  plus nonzero GPU activity counters identify Mali execution without faults.
- A SpringBoard icon/animation workload submits jobs through the iOS-facing
  path; GPU-produced frames reach DECON with correct colors and fences.
- In a controlled before/after run, record frame intervals, input-to-visible
  latency, CPU time in `present()`, GPU frequency/temperature and dropped
  frames. The unchanged software path remains bootable if the GPU path fails.

The immediate display-pacing work can be built and tested independently of
these GPU stages. It should be reported as a CPU/display improvement, never
as completed Mali acceleration.

External primary references: [Mesa Panfrost driver architecture and Mali
T720 support](https://docs.mesa3d.org/drivers/panfrost.html), [Apple Metal GPU
programming interface](https://developer.apple.com/documentation/Metal).
