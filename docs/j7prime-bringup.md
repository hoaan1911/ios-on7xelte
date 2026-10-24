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
