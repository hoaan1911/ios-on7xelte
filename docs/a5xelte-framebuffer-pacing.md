# A5XELTE framebuffer pacing and measurements

The current `AppleCLCD` bridge takes CoreAnimation's CPU-rendered IOSurfaces and copies their enabled rectangles to the DECON scanout buffer. It does not submit work to Mali. The 1080 × 1920 XRGB8888 panel needs 8,294,400 bytes for a full-frame copy. The 60 Hz timer also sends vsync notifications, independently of whether the kernel copies a frame.

Patch `0124` changes only the A5XELTE copy cadence from the donor's 10 Hz default to 30 Hz. `a5fb_hz=N` overrides the A5 value; `sweetfb_hz=N` remains accepted, and the A5-specific argument takes precedence. A5 rounds the tick interval up, so the actual copy cap never exceeds the requested rate. The initialization log reports requested and effective rates. SWEET retains its existing 10 Hz default and tick calculation.

On A5, `a5fb_perf=1` (the default for this trial) emits one aggregate line after 300 timer callbacks. `a5fb_perf=0` disables timing. The line includes frames copied, elapsed wall time, actual frame rate, bytes copied, and average/maximum kernel copy time. The elapsed time matters because an expensive copy delays the next timer callback. An example format is:

```text
A5XELTE FB: frames=120 elapsed_ms=6030 fps=19.9 bytes=995328000 avg_copy_us=7800 max_copy_us=14000
```

The values above only illustrate the format; they are not device measurements. Copy timing excludes CoreAnimation's userspace rendering, daemon crashes, storage waits and the timer's notification work. A higher copy cap can therefore improve responsiveness without fixing missing icons or other CPU-side bottlenecks. Compare the next recovered device log and visible behavior before retaining 30 Hz as the default. Mali acceleration needs a separate driver and a userspace rendering path.
