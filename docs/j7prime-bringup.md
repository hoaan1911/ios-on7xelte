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
