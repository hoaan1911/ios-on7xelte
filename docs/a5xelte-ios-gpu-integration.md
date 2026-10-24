# A5XELTE iOS graphics integration boundary

Status: source and staged iPhone 8 Plus iOS 16.7.16 image inspection on
2026-10-20. This note does not claim iOS rendering on Mali. The separate
`a5xelte-gpu-port.md` records the physical phone's Mali job results.

## Current frame path

1. CoreAnimation/backboardd supplies completed IOSurface IDs in the
   IOMobileFramebuffer swap record. `xnu/iokit/Kernel/arm/SweetFramebuffer.cpp`
   handles `SwapBegin` selector 4 and `SwapEnd` selector 5 (0x46c-byte record),
   and stores the latest record in `AppleCLCD::swap()`.
2. `AppleCLCD::vsyncFired()` notifies clients on a 60 Hz timer and invokes
   `AppleCLCD::present()` when a pending frame is eligible. `present()` looks up
   each surface by ID and copies BGRA rectangles row by row from its kernel
   mapping into the 1080x1920 DECON scanout buffer at `0x67000000`.
3. `xnu/iokit/Kernel/arm/SweetSurface.cpp` provides `IOCoreSurfaceRoot` and
   `IOSurfaceRootUserClient`. It implements create (selector 0), lookup (4),
   limits (13), lookup from Mach port (34), and create port (35). Lock/unlock
   are traps 2/3. Other external methods, including value, accelerator, and
   shared-event operations, currently return success with zero output without
   doing the requested work. The generic fallback is not a GPU submission
   interface.

The 0124 phone log measured 23 `present()` calls in 63.857 seconds, or
0.36 frames per second. Its full-frame CPU copies typically took 5–6 ms and
at most 11.6 ms. Thus raising the current 30 Hz cap or replacing only the
final copy cannot by itself explain or solve the observed very low frame rate.
The time taken to create frames upstream of `SwapEnd` still needs measurement.

## What the installed iOS userland expects

The staged, untracked IPSW is
`research/ipsw/iPhone_5.5_P3_16.7.16_20H392_Restore.ipsw`; its root was
inspected read-only. The root's split
`System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*` contains
Metal, QuartzCore, IOGPU, IOSurface, IOSurfaceAccelerator, and
IOMobileFramebuffer. The installed
`System/Library/Extensions/AGXMetalA11.bundle/Info.plist` names executable
`AGXMetalA11` and principal class `AGXPrincipalDevice`.

Symbol inspection of that cache establishes these interfaces, but not that
the running port currently reaches all of them:

| Userland component | Concrete symbols or strings | Needed kernel side |
| --- | --- | --- |
| `QuartzCore.framework/QuartzCore` | `IOAcceleratorES`, `IOGPUDeviceCreate`, `IOGPUDeviceSetDisplayParams`; also `SW renderer` diagnostic strings | A discoverable accelerator and GPU device path for GPU composition; software rendering exists today |
| `Metal.framework/Metal` | `MTLCreateSystemDefaultDevice`, `_MTLIOAccel*` command-buffer classes | A compatible Metal device provider |
| `PrivateFrameworks/IOGPU.framework/IOGPU` | `IOGPUDeviceCreate`, `IOGPUCommandQueueCreate`, `IOGPUCommandQueueSubmitCommandBuffers`, `IOGPUResourceCreateIOSurface`, `IOGPUResourceGetGPUVirtualAddress`; `IOGPUMetalTexture initWithDevice:descriptor:iosurface:...` | Device, queue, resource/import, shared memory, command submission and completion ABI |
| `PrivateFrameworks/IOSurfaceAccelerator.framework/IOSurfaceAccelerator` | `IOSurfaceAcceleratorCreate`, `IOSurfaceAcceleratorBlitSurface`, `IOSurfaceAcceleratorTransferSurface` | A separate accelerated blit service, if this route is exercised by the target processes |
| `PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer` | `kern_SwapBegin`, `kern_SwapEnd`, `kern_SwapWait`, `kern_GetLayerDefaultSurface` | Already emulated sufficiently for CPU-backed presentation |

These userland components target Apple's A11 AGX graphics stack. The Linux
Panfrost/DRM API used by the Exynos7580's Mali-T720 cannot be substituted for
the IOGPU/Metal contract simply by adding a kernel service name. The port's
current XNU source has no `IOGPU` or `IOAccelerator` implementation, and its
private Mali job fixture does not import an IOSurface or execute iOS Metal
commands.

## Smallest useful stages

1. **Observe the upstream bottleneck.** With bounded counters, record per
   process attempts to open `IOAcceleratorES`/`IOGPU`, IOSurface creation and
   lock/unlock rates, and `SwapEnd` rate/latency. Keep high-volume logging
   disabled by default. This determines whether backboardd is rendering in
   software, waiting for a service, or submitting frames that are not being
   displayed.
2. **Prove a real IOSurface import on Mali.** Extend the existing bounded GPU
   queue fixture to read a known BGRA IOSurface test pattern and write a
   private GPU output buffer. The GPU page tables must map pinned pages, cache
   ownership must be explicit, and JOB completion must be observed before
   pixel readback. A fixture that only writes a solid color is insufficient.
3. **Optional final-frame acceleration.** Route one `AppleCLCD::present()`
   rectangle through a Mali blit with a completion fence and CPU-copy fallback.
   Compare pixels and latency on the phone. This is an IOSurface-to-DECON
   present path; it saves final CPU copy work but does not make SpringBoard
   icons GPU-rendered.
4. **Full composition.** Provide a Mali-compatible iOS userland rendering
   backend plus the IOGPU/IOAccelerator kernel ABI it needs: device discovery,
   IOSurface-backed resources, command queues, shader/pipeline compilation or
   translation, submission, faults and fences. Validate an actual
   CoreAnimation/Metal command producing a frame and a sustained SpringBoard
   improvement. This is substantially larger than the present fixture and
   should not be inferred from GPU_JOB_DONE or a faster panel copy.

No IPSW, dyld cache, extracted binary, device dump, or proprietary code belongs
in git. Preserve the current CPU-backed display path as a working fallback.
