# A5XELTE Mali queue and presentation boundary

The 0133 phone test established one completed Mali fragment job writing into
the active DECON buffer. Its job is submitted synchronously during GPU service
startup. It does not accept frames from iOS. `AppleCLCD::present()` still reads
retained `SweetSurface` objects and copies their rectangles with the CPU.

## Kernel queue contract

- Keep a single owner for Mali AS0 and the selected job slot. Serialize jobs
  before changing page tables or slot registers; never replace a descriptor
  while hardware may still read it.
- Give each accepted job a monotonically increasing fence value. Completion
  means the corresponding job IRQ bit and job-header DONE status were observed
  with no MMU, GPU or job error. An attempted submission, timer expiry or an
  `IORegistry` string is not a completed fence.
- Bound waits. On a timeout or fault, stop accepting jobs, keep GPU-visible
  descriptors and page tables pinned, and leave the CPU framebuffer path
  available. Do not free a buffer that Mali might still address.
- Flush CPU-written descriptors and source pages before submission. Before
  the CPU reads GPU-written memory, invalidate the relevant range. The DECON
  scanout result also needs GPU completion before it is presented.
- The current ADT GPU nub has no usable interrupt-parent mapping. Initial
  completion therefore uses bounded polling. A later GIC route can replace
  polling without changing fence meaning.
- Submit only driver-constructed jobs. Exposing raw Mali descriptors or
  arbitrary physical addresses to iOS clients is outside this stage.

## Connection to iOS frames

`AppleCLCD::present()` already receives the latest swap record and retains
each referenced `SweetSurface` through the CPU copy. A GPU compositor can use
the same surface IDs and clipped rectangles, but must first map every physical
segment of each source surface into Mali's page tables. It must retain each
surface through its completion fence, handle per-surface cache maintenance,
and preserve layer order and alpha semantics. The current solid-color fragment
fixture cannot sample or blend an IOSurface; a compiled Midgard shader and
texture/sampler descriptors are still required.

Moving the final panel copy to Mali will save CPU copy time. It will not make
CoreAnimation render icons on Mali: the kernel sees finished IOSurface pixels,
not the framework's draw commands. Full iOS GPU rendering additionally needs
an iOS-facing graphics backend compatible with the installed CoreAnimation and
Metal interfaces. The staged iPhone 8 Plus AGX libraries target Apple's GPU,
not Mali-T720.

## Proof gates

1. Two driver-owned jobs complete in submission order with distinct fence
   values, zero Mali/MMU faults and no later use of freed storage.
2. A GPU job reads a known IOSurface test pattern and writes the expected
   pixels to a private destination. Verify output by readback before scanout.
3. The same GPU operation presents a real swap rectangle with correct colors
   and layer order on the phone; compare CPU time and frame cadence against
   the existing `present()` measurements.
4. Only after the iOS backend submits real UI work should SpringBoard be
   described as GPU accelerated.
