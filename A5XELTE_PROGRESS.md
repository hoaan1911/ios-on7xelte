# A5XELTE port progress

This file tracks work on the Samsung SM-A510F (a5xelte, Exynos7580). `NOTES.md` is the original SWEET donor's history. A local build, an observation from the phone, and a working iOS feature are different milestones.

## 2026-10-19 — first XNU entry on the phone

- **Device observation:** The Cortex-A53 reported `ID_AA64MMFR0_EL1=0x1122` from EL1. Its 16 KiB translation granule is unsupported. A separate 4 KiB MMU probe completed on the phone and left `A5XNU001` in the recovery log.
- **Local build:** Xcode 27.1 built a 4 KiB arm64 XNU kernel with the initial entry patch. That first binary still used `MACHINE_CONFIG=SWEET`; it is an entry experiment, not the final a5xelte board target. Kernel SHA-256: `f093a6bc0d8f9c060f6dc4429810f768e00ec3f093ca624f1cd2694bd9a9b444`.
- **Device observation:** The 19,990,544-byte Samsung BOOT candidate (SHA-256 `8dff262ad0934b71396900eefef2ce34b681f0574e5e37ba065fc7482cff5921`) was written to BOOT p10 in TWRP and read back with the same hash. After reboot the user saw the Samsung logo. The subsequent TWRP `/proc/last_kmsg` contained loader marker `A5XNU002` and XNU-owned entry marker `XNUELTE1` at the expected `+0x100` offset. This proves that execution entered XNU's `start_first_cpu`; it does not prove MMU setup, a kernel console, iOS userland, or a usable screen.
- **Partition scope:** The test wrote BOOT p10 only. Recovery, EFS and USERDATA were not written. The phone returned to TWRP. The working Linux BOOT checkpoint remains available separately; it was not restored at the user's request.

## Current A5XELTE build work

- Patches `0085`–`0088` introduce `MACHINE_CONFIG=A5XELTE`, a dedicated board header and ADT identity, GICv2 interrupt handling, virtual timer INTID 27, and guards around SWEET-only framebuffer and sampler code. The port temporarily shares donor `SWEET` compile-time facilities. This compatibility definition must not be taken as hardware equivalence.
- The first A5XELTE link exposed donor-only symbols from the framebuffer and sampler; patch `0088` excludes those paths. The next build **linked** `kernel.development.a5xelte.unstripped` (19 MB, SHA-256 `609163d2bded8bd3a4364ad067dd1a068fb9e4ecf833318bcf0b87fc696013c2`). The full install target then failed in `dsymutil` because the local shell could not open its `/dev/fd/62` process substitution (`Operation not permitted`). This is a packaging/debug-symbol failure after linking, not a successful end-to-end build.
- **Second BOOT attempt:** The linked A5XELTE Mach-O was packaged with a 3,604-byte a5xelte ADT, containing the GICv2 node, and the same Samsung loader. Candidate size: 19,820,560 bytes; SHA-256: `1943b7747f060e74c757f42ff5b8fa646c957773fb904ddcb6daab3d9f094d59`. TWRP wrote only BOOT p10; readback over the entire candidate matched that hash. The user saw the Samsung logo and returned to TWRP. The recovered 2 MiB `/proc/last_kmsg` (SHA-256 `7c19efd088589addfab0d0ecbc00276cece8aedf32e44d4aefdb0f748ef255da`) contains `A5XNU002` at offset `0x7f41` and XNU-owned `XNUELTE1` at `0x8041`, again exactly `+0x100`. Loader state is 5, with entry PA `0x8141a488`, ten segments and boot args PA `0x823cd000`. **The A5XELTE-named kernel reached its XNU entry on the phone.** No later MMU, interrupt or console checkpoint was observed.
- **Early screen test:** A temporary loader wrote text to the `s3cfb.bootloaderfb=0x67000000` RAM address and a temporary XNU entry routine wrote a green rectangle. The BOOT candidate was 19,822,608 bytes, SHA-256 `3333be3a95d87f44faed4079b52ee6e67d641c389cfc1f7e78f553a4b75c55a4`; BOOT p10 readback matched. The user saw **only the Samsung logo**. TWRP's recovery log (SHA-256 `3be3f1cae0b2204c01a8f57642da748eb647f22f7e444d23c2493ac554a4059d`) still contains `A5XNU002` and `XNUELTE1` at offsets `0x19b1e1` and `0x19b2e1`. The direct RAM writes did not produce visible output. This temporary code was removed from the working source; see `docs/a5xelte-screen-console.md` for the Linux-driver comparison.
- **DECON screen milestone:** The loader now follows the Exynos7580 Linux driver's bounded DECON/DSIM handoff and scans out a new buffer at `0x67000000`. Patch `0089` adds a green XNU entry block and a blue block after 4 KiB MMU enable. The 19,824,656-byte BOOT candidate (SHA-256 `82c6556a0216f841dc103e815072bcc0f40f53c0ccce8274b2fff40477afa981`) was written only to BOOT p10 and read back exactly. **The user saw all four loader labels and both green and blue XNU blocks.** This proves the physical display path and XNU instructions after MMU enable. It does not establish `arm_init` or general console output. TWRP `/proc/last_kmsg` (SHA-256 `4e32198cbb98b8fae915fb6927f77136b69d730b61ed9c5ed9993b482786a40c`) contains `A5XNU002` at `0x137ec4`, `XNUELTE1` at `0x137fc4`, display state `5`, power status `0x3000f`, post-handoff DECON status `0x8010010f` and FB_START `0x67000000`.
- **First text-console candidate:** A 4 KiB A5XELTE XNU with early `arm_init` stage text and an ADT-backed `a5xelte-fbcon` serial console linked (Mach-O SHA-256 `f02bbff37c0c7aefb7eb8199060be5168827de4ed4797b150c313e9556e35c19`). Its 19,824,656-byte BOOT candidate (SHA-256 `33371f5628cf82d14d31d997b58424e8e886d12252ecbb312bdef8e58d8d37a0`) was written to BOOT p10 and exact bytes read back. **The user still saw only the four loader lines and two XNU blocks.** No `arm_init` stage or `kprintf` text was visible.
- **Assembly stage diagnostic:** An updated 19,824,656-byte BOOT image (SHA-256 `351f915f10a18217b5617a9cafe3dcf92f19617349a6b6b1b32a88b1c9f5d8c9`) added loader labels and XNU-owned status blocks after enabling FP at EL1, after CPU tunables, and immediately before `ret` to `arm_init`. BOOT p10 readback matched. **The user reported five total blocks**, so all three new assembly checkpoints executed after the original green entry and blue post-MMU blocks. No `A5 XNU STAGE` C text was reported. This narrows the remaining problem to the branch/C entry or early framebuffer text path; it does not establish execution inside `arm_init`.
- **First XNU-owned screen text:** Patch `0092` adds an assembly renderer using XNU's `iso_font`, with no C runtime or stack, at XNU entry and after MMU enable. The linked Mach-O SHA-256 is `54e29e2a1d91e7e790e32792e6556a8dbf1af75b4682db3dac39b035bd2842f4`; the BOOT candidate is 19,824,656 bytes, SHA-256 `3d3594a3d92f333878c2238a47786440e7d8711876b72a2dfed099851489a5c6`. TWRP wrote only BOOT p10, and exact candidate bytes read back with the same hash. **The user saw both XNU-rendered messages, `XNU ENTRY` and `XNU MMU ON`, on the phone.** This is XNU-owned text before C startup; it is not `kprintf` or an iOS console. No `A5 XNU STAGE` C text has been reported.
- **39-bit kernel VA and C startup:** Patch `0093` replaces the macOS `ARM_LARGE_MEMORY` kernel link address (`0xfffffe0007004000`, outside the A5's configured 39-bit TTBR1 range) with `0xfffffff007004000` for A5XELTE only. The new linked Mach-O SHA-256 is `0bad1b43983dc8aa14976cc81593f00d51ffd48778f60a9bdd9d978b1b0ff51b`. The 19,709,968-byte BOOT candidate SHA-256 is `a8eafe99fa19dc5f7848df02194af251f3bbfa42af07241abedcdf3a2df2fdcf`; TWRP wrote BOOT p10 only and the exact image read back with the same hash. **The user's screen photo shows XNU C startup stages 1–4: `arm_init`, `PE_init_platform`, `ml_parse_cpu_topology`, and `cpu_bootstrap` completed.** XNU then printed `Undefined kernel instruction: pc=0xfffffff0072a92c4 instr=b8280028 @sleh.c:951`. This is a kernel panic, not a completed XNU boot or iOS startup.
- **Instruction diagnosis and A53 rebuild:** Disassembly of that exact linked Mach-O maps `0xfffffff0072a92c4` to `ldadd w8, w8, [x1]` in `lck_grp_reference`, generated from `os_atomic_inc(cnt, relaxed)`. Cortex-A53 implements ARMv8.0 and cannot execute this ARMv8.1 LSE instruction. A local compiler reproduction showed Apple's arm64 default CPU (`apple-m1`) emits `ldadd` even with `-march=armv8-a+crypto`, while `-mcpu=cortex-a53 -march=armv8-a+crypto` emits an ARMv8.0 `ldxr`/`stxr` loop. Patch `0094` selects Cortex-A53 code generation. Patch `0095` disables the board's unsupported PAN register path. Patch `0096` replaces two `casa` instructions in the commpage trylocks with one-attempt LL/SC sequences, preserving their non-blocking behavior. REA's binary inspection could not start because its provider process did not have Ghidra/Java configured; this diagnosis uses the local Mach-O disassembler and source.
- **External archive check:** The first A53 XNU link still contained 22 LSE instructions in firehose functions. They came from the separately built `libfirehose_kernel` archive, not the board XNU compiler flags. The archive was rebuilt for Cortex-A53 from the staged libdispatch source (repeatable with `tools/a5xelte-build-firehose.sh`) and XNU was relinked. The new Mach-O SHA-256 is `3be8b30a251ecda0a61ad3e7ffc3e13765b232e2221833a786ad4c8d36b5162b`; a full disassembly scan found no `cas`, `ldadd`, `ldclr`, `ldeor`, `ldset`, or `swp` LSE mnemonics. `lck_grp_reference` now has `ldxr`/`stxr`. The final `dsymutil` step still fails on local `/dev/fd` permissions after the kernel link; the tested input is the linked unstripped Mach-O.
- **A53 BOOT trial and native panic text:** A 20,127,760-byte BOOT image was packaged from that Mach-O and the same display loader, SHA-256 `1de5bf676720a7a94884611816d5792ded8d891ae1b1e124c1cdd9007f2e3b1d`. TWRP reported `omni_a5xelte`; the image was written only to BOOT p10 and read back with the exact same hash. **The user's screen photo shows `kprintf initialized`, `pmap_startup`, `vm_page_bootstrap` (119857 free pages), VM bootstrap, and a native XNU panic:** `kmem_range_init: vm_map_locate_space failing for claim zones @vm_kern.c:3645`. It also reports `pe_arm_map_interrupt_controller: failed to find the interrupt-controller`; that ADT issue is a separate pending item. This is progress through VM bootstrap, not a complete XNU boot.
- **Zone VA correction:** `zalloc.c` selects a fixed 128 GiB zone VA claim for `XNU_PLATFORM_MacOSX`. The A5XELTE non-large 4 KiB kernel uses a compact address layout with roughly 48 GiB of heap VA, so the 128 GiB claim cannot fit. Patch `0097` selects the existing 24 GiB zone VA size for this board while leaving other Mac targets unchanged. The corrected Mach-O linked (SHA-256 `c05856182eec7a33bf22cc6c5a52f599a10b378c5ee87bf8f031520426066805`); the final `dsymutil` operation still hit the local `/dev/fd` error. The 20,127,760-byte BOOT image SHA-256 is `a0535c2125c41524a1087e57f7aba14b8260051fc80cf83434a06a5276cc79d1`; TWRP wrote only BOOT p10, full-size readback matched, and the phone rebooted. The user screen photo verified it progressed beyond this `zones` failure.
- **Zone trial screen result:** The user's next screen photo shows XNU progressed beyond zone allocation into IOKit platform initialization, then panicked with `Kernel data abort` at PC `0xfffffff00793cd94`, FAR `0x28`, LR `0xfffffff0079abe38`. The exact Mach-O maps this to `SHA1Init` called from `IOPlatformExpertDevice::generatePlatformUUID()`. The load at PC dereferences `g_crypto_funcs->ccsha1_di`; the register dump shows that pointer was zero. The donor's portable SHA-1 path is guarded by `ARM64_BOARD_CONFIG_SWEET`, while A5XELTE has its own board flag and no corecrypto kext. This is a confirmed null dereference, not an ARM instruction trap.
- **Portable crypto and interrupt setup candidate:** Patch `0098` enables the existing portable SHA-1, SHA-2, AES and MD5 sources for A5XELTE without defining the SWEET board globally. Disassembly of the newly linked SHA-1 function calls `br_sha1_init` instead of dereferencing `g_crypto_funcs`. The previous screen had also reported `pe_arm_map_interrupt_controller: failed to find the interrupt-controller`; XNU searches the ADT property value `master`, while the A5 node had an empty boolean. The ADT now supplies `master`. Patch `0099` allows Exynos7580's architected virtual timer (GIC PPI 27) without inventing an MMIO timer register range. The new Mach-O SHA-256 is `a1a489b2f5132720b28de566da7a254830ea2f6c5f4f18ffabf39aade2e87a3b`; the final `dsymutil` step still fails on local `/dev/fd` access after linking. The 20,152,336-byte BOOT image SHA-256 is `33d89b37112264182d8be439b6547ea746bee8af0e8ef5f193ed6389d4e2442e`; TWRP wrote only BOOT p10, exact readback matched, and the phone rebooted. **The user's screen photo shows it reached zone map creation, IOKit memory ownership, OSLog/Firehose, scheduler and IPC pthread initialization, then panicked on a recount assertion:** `(snap->rsn_time_mach & RCT_PR_IDLING) == 0` at `recount.c:964`. This is a later runtime stop, not proof that interrupt delivery or iOS userland works.
- **Counter epoch device result:** `recount_snapshot()` reads `ml_get_speculative_timebase()`, which adds `cpu_base_timebase` to `CNTVCT_EL0`. `recount_processor_run()` reserves bit 63 of that result as `RCT_PR_IDLING`; the earlier assertion therefore means the sampled Mach time carried that bit. The 4 KiB probe measured `CNTFRQ_EL0=0`, but did not capture `CNTVCT_EL0`, so the exact hardware counter epoch remains unmeasured. Patch `0100` starts the A5XELTE boot CPU's Mach time at one tick by setting a software offset after `cpu_data_init()`; it does not mask the recount assertion or write timer registers. Xcode linked the Mach-O (SHA-256 `440475624fa9c0dde00afb6e7fbc53e47e9030dd962b1746728d04eb65757bfe`); `dsymutil` again failed afterward on `/dev/fd/62`. The 20,152,336-byte BOOT candidate has SHA-256 `11463c70b363690c2f5bbbc2f14cc02443d96ed714d0f6b95383d826120e885d`. TWRP identified `a5xelte`; VM, device file, and BOOT p10 readback hashes all matched. Only BOOT p10 was written. **The user's new screen photos show `recount` passed, SGI1 self-IPI reported OK, one CPU initialized, HFS registered, and `mbinit` completed.** The next panic is `pthread kernel extension not loaded (function table is NULL)` at `pthread_shims.c:318`.
- **A5XELTE pthread device result:** The donor's built-in libpthread sources existed, but `sweet_pthread_init.c`, `sweet_pthread_support.c`, `sweet_pthread_synch.c`, and the related `ksyn_waitq_element` layout were guarded only by `ARM64_BOARD_CONFIG_SWEET`. On the prior A5 build, `sweet_pthread_init.o` had no symbols and no registration startup entry; this explains the null function table. Patch `0102` enables the same paired Apple libpthread-514.100.2 provider for `ARM64_BOARD_CONFIG_A5XELTE`, and its staging script now accepts A5XELTE. Patch `0101` gives the active boot CPU its Cortex-A53 name, restricts the A5 topology to the tested CPU0, and labels CPU logs A5XELTE; the donor class/source remains shared pending a separate structural migration. The linked Mach-O SHA-256 is `7a3166988a8ae5d757e5d1f07bead92ced43f31c05da1d01e19ebfde483fae04`. Its symbol table contains the `EARLY_BOOT` registration entry, `pthread_internal_functions`, and `sweet_pthread_start`; final `dsymutil` again failed on `/dev/fd/62`. The 20,197,392-byte BOOT candidate SHA-256 is `d69b3fe71dbc9918b7efbbb90162cff6d07ebd2f49e1d0b687668499b596ebca`. TWRP identified `a5xelte`; VM file, TWRP file, and BOOT p10 exact-size readback all matched that hash. Only BOOT p10 was written. **The user's screen photos show `A5XELTE pthread: libpthread-514.100.2 registered`, CPU0 SGI1 success, HFS registration, network kernel threads running, and passing Skywalk self-tests.** It then waits for an `IOMedia` service with `Content=Apple_HFS` because no RAM disk was included and the A5 eMMC XNU path is absent. `Failed to write boot-picker-bringup-reason to NVRAM` appears later; this is a separate, nonfatal diagnostic.
- **HFS RAM disk device result:** Alpine VM `hfsprogs` formatted an 8 MiB temporary HFS+ image (SHA-256 `c34463255f2311874df78ec0b48e2bedb22de1d162ec39b3989cf2f39daea471`, HFS+ signature `48 2b` at byte 1024). `mkpayload.py --ramdisk` placed it at physical `0x82428000` and added `/chosen/memory-map/RAMDisk`; the boot line selects `rd=md0`. The same linked XNU is used. The 28,586,000-byte BOOT candidate SHA-256 is `bb49e25bd61a15199d6e370d2cea7642438bc46cf08e9b22e9767276b59bbc53`, below BOOT p10's 32 MiB size. TWRP identified `a5xelte`; VM file, device file and exact-size BOOT p10 readback all matched that hash. Only BOOT p10 was written. **The user's screen photos show `Added memory device md0/rmd0`, `BSD root: md0`, then a panic at `IOService.cpp:5200`: `IOBlockStorageDriverClass` assertion failed.** The RAM disk reached the BSD root selection path; HFS mount and iOS userland have not been demonstrated. The image is empty and contains no userland.
- **A5XELTE RAM disk mount result:** The earlier panic was in `IOService::setRootMedia()`. The donor code already tolerates absent `IOBlockStorageDriverClass` and `IOMedia` on its SWEET RAM disk path; A5XELTE selected the other branch and asserted. Patch `0103` extends those two guards to A5XELTE, without adding an eMMC driver or changing partitions. The rebuilt Mach-O SHA-256 is `2f6fdfa557d7f670982c53ed7ea5a628992a5d4cda23acf3cce252c134c4ea1d`; the link completed, followed by the known local `dsymutil` `/dev/fd/62` failure. The 28,586,000-byte BOOT candidate SHA-256 is `0b8c2d7d6ff705f70a04afed1690eb4a65b4f5205b4d51bed33524795c1db8dd`. TWRP identified `a5xelte`; Mac file, VM file, TWRP file and exact-size BOOT p10 readback all matched. Only BOOT p10 was written. **The user's screen photo shows `hfs: mounted A5XNU on device ... (st: 0)`, followed by an attempt to execute `/sbin/launchd`.** The empty 8 MiB volume has no such file, so process 1 panics with `errno 2` at `kern_exec.c:6929`. This proves HFS+ root mount and the transition to userland launch, not a running iOS process.
- Exynos eMMC, an XNU display service, touch, buttons, SMP and iOS root remain unimplemented or unverified in XNU.

## 2026-10-20 — local iPhone 8 Plus IPSW inventory

- The user-provided `iPhone_5.5_P3_16.7.16_20H392_Restore.ipsw` was **moved**, not copied, from Downloads into ignored `research/ipsw/`. No IPSW content is tracked by Git. The ZIP manifest is readable: iOS 16.7.16 (20H392), supported products `iPhone10,5` and `iPhone10,2`, and install identities `d211ap` and `d21ap` (erase and upgrade variants).
- Both device classes name `087-86694-044.dmg` as OS (4,723,392,522 bytes), `087-86588-047.dmg` as `Cryptex1,SystemOS` (2,743,074,816 bytes), and `087-86715-047.dmg` as `Cryptex1,AppOS` (10,485,760 bytes). The DeviceTree members differ: `Firmware/all_flash/DeviceTree.d211ap.im4p` or `DeviceTree.d21ap.im4p`. These are archive member sizes, **not** the installed HFS root size or available eMMC space. The inspector now resolves the `Cryptex1,` manifest keys instead of reporting both cryptexes absent.
- No Apple image was extracted, root image built, device identity changed, or partition written during this inventory. The active A5XELTE ADT still retains donor userland identity fields. A read-only Exynos eMMC XNU path and verified root size remain prerequisites for an on-device system disk.

## 2026-10-20 — read-only eMMC GPT probe candidate

- Patch `0104` adds an A5XELTE-only, boot-arg-gated (`emmcprobe=1`) Exynos7580 DesignWare eMMC probe. Its register window comes from the A5XELTE ADT (`0x13540000`, `0x2000`) and the working Linux device tree. It checks the enabled clock and host FIFO width, preserves bootloader bus timing, disables DMA for polled PIO, sends only CMD17 for LBA1, and tests for `EFI PART`. The FIFO offset and data width follow the downstream `dw_mmc` source; the timeout field uses the verified native Linux encoding. It has no write command, block device registration or partition mutation path. `rd=md0` remains the boot root.
- **Local build:** The A5XELTE Mach-O linked with `a5xelte_emmc_probe` and `emmc_read_gpt_header` symbols; SHA-256 `ea89bf404fb5656906ea72fc16286558c322597fe236b0ef9c890ec0f36d04ff`. Final `dsymutil` again failed at `/dev/fd/62` after the kernel link. The 28,586,000-byte Samsung BOOT candidate SHA-256 is `406c3ee31fce571f922fad5503c13859ccce9fac4e80b88a8f158ccfaba8aa45`.
- **First flash/readback and phone result:** TWRP identified `a5xelte` and BOOT p10 as a 65,536-sector (32 MiB) partition. The Mac image, temporary transfer copies and exact-size BOOT p10 readback had the same SHA-256. Only BOOT p10 was written. The user's screen photo shows `A5XELTE eMMC: host ctrl=10 status=8906 clk=1 hcon=8e42541`, then `CMD17 timeout rint=424 words=64/64`. Software performed 64 FIFO reads, but `0x424` includes host-timeout bit 10 and lacks data-over bit 3; the later counter capture shows only 256 actual bytes transferred. The sector is **not yet accepted as valid GPT**. XNU continued to mount `md0` and reached the expected missing `/sbin/launchd` panic. The next revision acknowledged RXDR after FIFO drain, treated host timeout as an error, used the Linux-verified timeout value, and printed the first word and transfer counts. The user requested direct Mac ADB for subsequent transfers instead of the VM USB route used in the first attempt.
- **Second local build, flash and phone result:** The RXDR/timeout diagnostic revision linked as Mach-O SHA-256 `dbe480eec211a1392523922c56d0836d021e97024bf437468612092418970eb2`; final `dsymutil` failed at `/dev/fd/62` after link. The 28,586,000-byte BOOT candidate SHA-256 is `753d9d2b254fef0196b5c44decbb4ae09a045828baf665046d0dc64188107acd`. Direct Mac ADB identified `a5xelte` TWRP, pushed this exact image, wrote only BOOT p10, and read back the exact image length with matching SHA-256. The user's next screen photo shows `CMD17 error rint=424 status=808d09 words=64/64 first=2049464520494645 tcbc=256 tbbc=0` (line wraps on screen). The first word is the low 32-bit `EFI ` fragment duplicated, and the controller transferred only 256 of 512 bytes. The working Linux port sets `CONFIG_MMC_DW_FORCE_32BIT_SFR_RW=y`: each 64-bit FIFO unit must be read as two 32-bit SFR accesses at `DATA` and `DATA+4`. The XNU probe is being corrected accordingly. GPT validity is still unverified.
- **Third local build, flash and phone result:** The paired 32-bit FIFO access variant linked as Mach-O SHA-256 `79d2906efb9c58f95beac838791542ae5f70b0e8c3869f3da969222423328dfd` and packaged as a 28,586,000-byte BOOT image SHA-256 `1a15ada941a86dbf6cd0ab65d1ba0706e36f1d0ce61953e3f38916f62ae510a4`. Final `dsymutil` again failed on `/dev/fd/62` after link. Mac ADB transferred the image to `a5xelte` TWRP, wrote only BOOT p10 and obtained an exact-size readback with the same SHA-256. The user's screen photo shows `A5XELTE eMMC: read-only GPT LBA1 OK, entries=128, ver=270a, fifo=64-bit`; the paired 32-bit FIFO reads now yield a valid GPT signature on the physical phone. XNU still selected the HFS `md0` RAM disk and reached the expected missing `/sbin/launchd` panic. A GPT read is not yet a block device or disk root mount.

## 2026-10-20 — SYSTEM HFS root transition

- The user explicitly authorized erasing all of Linux SYSTEM p20 for a dedicated HFS+ root and declined another backup. The earlier Linux-port notes record a full 3,145,728,000-byte SYSTEM backup on the private VM, but its current availability could not be rechecked over SSH. Recovery p11, EFS p3 and USERDATA p23 remain outside this change.
- **Read-only device inventory:** Mac ADB in TWRP identified `a5xelte`; `SYSTEM` resolves to `/dev/block/mmcblk0p20` and measures exactly 3,145,728,000 bytes. A private read of GPT sectors 0–33 shows Samsung's 512-byte GPT header (rather than the usual 92 bytes). Its CRC and the 128-entry table CRC both match. The `SYSTEM` entry is LBA 507904–6651903, exactly 6,144,000 sectors, the same size as p20.
- **Local XNU build:** Patch `0104` now extends the proven CMD17 reader into a read-only BSD disk named `emmcroot0`; it accepts only the GPT named `SYSTEM` after both CRCs pass, range-checks every request, and has no eMMC write command. `rd=emmcroot0` selects it as BSD root. The 20,139,792-byte XNU Mach-O linked; as before, the subsequent local `dsymutil` step failed on `/dev/fd/62`. The 20,199,440-byte Samsung BOOT candidate has SHA-256 `484283b0ad9e815bb06725bc46c8fd187ced1be2a6033016ed2e47d82e7afa8e`. Device boot and HFS mount from p20 remain unverified until the phone test.
- **HFS image, write and readback:** Mac `hdiutil` produced a raw 3,145,728,000-byte journaled HFS+ volume `A5XNU` with 4 KiB allocation blocks and both primary and alternate HFS headers. Its SHA-256 is `5001611ce3f9b9ee4526c763d8877738547e8e59618487158106fdf2b5918ab5`. ADB's binary stream was first tested against TWRP `/tmp` with matching hashes. The full image was then written to **SYSTEM p20 only**; `sha256sum /dev/block/mmcblk0p20` returned the exact image SHA-256, and the device reported the expected 3,145,728,000-byte size. It is an empty filesystem: there is no `/sbin/launchd` or iOS userland yet. The raw file stays in private `/tmp`, outside Git.
- **First disk-root BOOT and phone result:** Mac ADB identified `a5xelte` TWRP, checked the BOOT by-name path as `/dev/block/mmcblk0p10`, checked the 20,199,440-byte candidate file and SHA-256, then wrote only BOOT p10. Exact-size readback SHA-256 matched `484283b0ad9e815bb06725bc46c8fd187ced1be2a6033016ed2e47d82e7afa8e`. The user's screen photo shows `GPT SYSTEM first=507904 sectors=6144000 read-only`, `/dev/emmcroot0 ready`, `BSD root: emmcroot0, major 1, minor 0`, and `hfs: mounted A5XNU on device b(1, 0)`. **The physical phone has now mounted HFS+ from the persistent SYSTEM p20.** The next panic is `rootvp not authenticated after mounting @bsd_init.c:947`: an unsealed HFS root has no Image4/AMFI authentication chain on this lab phone.
- **Unsealed-root fix and second phone result:** Patch `0105` follows the donor's existing unsigned-root path but reports A5XELTE separately. It does not alter eMMC reads or add writes. The new 20,199,440-byte BOOT image SHA-256 is `5823cf2ff0c08a4d13f53f0e806a59c9e1345425d15986edd6b58642e9b69ddb`. Mac ADB TWRP transfer and exact-size BOOT p10 readback matched. The user's next screen photo shows `BSD root: emmcroot0`, `hfs: mounted A5XNU on device b(1, 0)`, `A5XELTE: root volume is not authenticated (error 25); continuing unsealed`, `devfs_kernel_mount: kernel_mount failed: 2`, then attempts to load `/usr/appleinternal/sbin/launchd.development` and `/sbin/launchd`. The empty HFS volume has neither file, so process 1 panics with `errno 2` at `kern_exec.c:6929`. The persistent SYSTEM HFS+ root and transition to userland exec are **device verified**; no iOS userland is installed or running.

## Next verified milestone

Stage the iPhone 8 Plus userland in an exact-size HFSX USERDATA image, select it with `emmcroot=USERDATA`, and observe the first on-device launchd attempt. The persistent SYSTEM p20 HFS+ root mount, unsealed-root continuation and attempted process 1 exec already have screen evidence. Persistent NVRAM, writable XNU eMMC, device identity, the display service and running SpringBoard remain later milestones. See `docs/a5xelte-bringup.md`, `docs/a5xelte-screen-console.md` and `docs/a5xelte-migration-map.md`.

## 2026-10-20 — USERDATA root preparation

- The user authorized complete erasure of USERDATA p23 if the iOS root would not fit in SYSTEM and asked to prioritize SpringBoard over stock applications. Existing GPT reports USERDATA LBA 7,184,384–30,769,151, or exactly 12,075,401,216 bytes (11.25 GiB).
- The local iPhone 8 Plus OS, SystemOS and AppOS images were extracted to private `/tmp` and mounted read-only with actual UID/GID ownership. Their apparent file totals are about 7.49 GiB, 3.13 GiB and 8 MiB respectively. The OS plus cryptexes leave too little space in USERDATA without selection. The new `tools/a5xelte-stage-ios-root.sh` retains launchd, SpringBoard, frameworks and dyld cache while omitting stock apps and large optional assets. Staging and phone execution still require verification.
- A private, case-sensitive HFSX sparse image of the exact USERDATA byte length was staged. It uses 8.7 GiB and leaves 2.6 GiB free. Mac `diskutil verifyVolume` reported a journaled, case-sensitive HFS+ volume with a clean catalog and exit code 0. `/sbin/launchd` and SpringBoard retain UID/GID 0:0, while the dyld shared cache retains 0:80. The donor's generic launchd, Setup and SpringBoard first-boot tweaks were applied; no mobileactivationd data ark was found to seed.
- Patch `0104` now accepts `emmcroot=USERDATA` while retaining `SYSTEM` as the default and keeping the eMMC device strictly read-only. The XNU Mach-O linked with this selector (SHA-256 `5d6346c4c153b9247ff0067cca80b22ada54ba7ce98694d15b139e76ffdac943`); final `dsymutil` again failed on `/dev/fd/62` after link. The 20,199,440-byte BOOT candidate contains `rd=emmcroot0 emmcroot=USERDATA` plus the donor's early userland boot arguments.
- **First USERDATA and BOOT write:** Mac ADB identified `a5xelte` in TWRP, `/dev/block/mmcblk0p23` measured exactly 12,075,401,216 bytes, and BOOT p10 measured 33,554,432 bytes. A read-only virtual device for the staged HFSX image also measured exactly 12,075,401,216 bytes. The full source stream and TWRP `dd` each reported exactly 12,075,401,216 bytes written to p23. The user explicitly asked to skip the full-device hash comparison, so this is a transfer-length check, not a content readback verification. The 20,199,440-byte BOOT candidate was written to p10 and its first eight bytes read back as `ANDROID!`; no full BOOT hash readback was done.
- **USERDATA phone result:** The user's screen photo shows `hfs: mounted A5IOSROOT on device b(1, 0)` and XNU continuing to the first `/sbin/launchd` exec. It then panics on a kernel data abort at `pc=0xfffffff0077ef060`, `far=0x1a0`. The local linked Mach-O symbolicates this to `accelerate_entitlement_queries + 32`, called from `ubc_cs_blob_add`. The AMFI interface pointer is NULL because donor registration is guarded for SWEET only. The populated persistent HFS root is device-verified; launchd has not yet run.
- **A5XELTE AMFI bridge trial:** Patch `0106` enables the donor's built-in AMFI and XML-entitlements interface on A5XELTE and gives its runtime messages an A5XELTE label. The user does not require a production code-integrity chain for this lab bring-up. The revised Mach-O linked, SHA-256 `1af902f3524f288b6452e52356c862bfdd641b9ea4fd057e70b412cb7424abff`; the usual `dsymutil` `/dev/fd/62` failure occurred after link. The 20,219,920-byte BOOT candidate has SHA-256 `f833bffb4baafc1e64e9b309f32511887bd38f0a2cfbcd25a90a3a0a9498bca9`. Mac ADB identified TWRP `a5xelte`, wrote this candidate only to BOOT p10 and read back `ANDROID!` from p10 before reboot. USERDATA p23 was not rewritten for this kernel-only change.
- **launchd executes:** The user's next phone photo shows libignition running inside PID 1, opening `/System/Library/Caches/com.apple.dyld`, and dyld attempting the shared cache. The kernel rejects its CodeDirectory page shift 14 (16 KiB), so dyld reports `code signature registration for shared cache failed`, cannot load `libSystem.B.dylib`, and PID 1 exits. `ignition_level=1` also reports that some cryptex paths require level 2 or 3, but the immediate fatal error is the shared cache registration. This is device proof that launchd began executing, not that it started its jobs or SpringBoard.
- **4 KiB unsigned shared-cache trial:** Patch `0107` lets A5XELTE record a 16 KiB CodeDirectory without trying to hash 16 KiB from a 4 KiB VM page, and disables code-signing enforcement in the shared region for this lab port. The BOOT command line includes `cs_enforcement_disable=1`. The linked Mach-O SHA-256 is `57e7bf2e8d5ebb47a778356a95be6ad499915d6dc037661c0f72d085abc6ab09`; the subsequent `dsymutil` step failed on `/dev/fd/62`. The 20,217,872-byte BOOT candidate SHA-256 is `a7fa97ece6c84ee97925a4dabdf60c1462025f2ca9f20e137d9ddaa0e2985a9c`. Mac ADB wrote only BOOT p10, read its `ANDROID!` header and rebooted. USERDATA was not retransferred.
- **PID 1 boot tasks:** The next phone photo shows launchd's `Bootstrapped Version 7.0.0`, on-demand mode, `fsck`, and `mount-phase-1`. The `mount` task reports `GetMasterBlock: Error 16 opening /dev/emmcroot0`, `mount_hfs: Read-only file system`, then exits status 1 and causes the launchd `mount[3] exited` panic. The 16 KiB dyld cache registration blocker was passed. SpringBoard has not started.
- **USERDATA write trial:** Patch `0108` adds polled CMD24 single-sector writes, using the working Linux driver's paired 32-bit FIFO accesses and a 64-entry FIFO depth from the a5xelte downstream device tree. A boot argument `emmcwrite=1` enables writes only when GPT selected `USERDATA`; SYSTEM remains read-only. The strategy path still checks every request against the selected partition range, supports read-modify-write for partial sectors, and serializes access. The Mach-O linked with SHA-256 `ce378a376e43c889cc7ec703309d7bed32ca4c762492f7e01b4a3b449049293f`; the later `dsymutil` step failed on `/dev/fd/62`. The 20,217,872-byte BOOT candidate SHA-256 is `d913f9483adc57d68906532a7987a16e0cf8badab3cc5e53759edcba31529f81`. Mac ADB wrote only BOOT p10, read `ANDROID!` and rebooted. On-device writable mount and USERDATA writes remain unverified pending the phone screen result.
- **Later boot progress reported from the phone:** The user first reported that the writable candidate passed `system boot started`, then supplied a screen photo showing launchd allowing spawns of `amsaccountsd`, `xpcproxy`, `axassetsd`, `passd`, `bluetoothuserd`, `cloudpaird`, `accessoryupdaterd`, `familynotificationd`, `peopled`, `navd`, `askpermissiond`, `contextstored`, `tccd`, `atc`, `OTACrashCopier`, and `biomed`. The screen also warns that `logd_helper[92]` used more than 50% CPU over 180 seconds (83% recently). There is no panic in this photo. This proves launchd advanced beyond the earlier `mount-phase-1` panic and is attempting more userland jobs; it does not prove each spawned service remained alive, a completed HFS write, SpringBoard process start, or rendered graphics. Leave this slow first boot running until a later log, panic, or visible UI gives a clearer endpoint.
- **Additional live screen evidence:** The next photo shows repeated `xpcproxy` spawn messages, `ACCHWComponentAuthService`, `mobileassetd`, `com.apple.datamigrator`, `lsd`, `MobileGestaltHelper`, and `PowerUIAgent`. It also shows `A5XELTE Sandbox: containermanager op 16` and `op 15`. The `SWEET AKS: powerd ...` lines are donor compatibility labels, not evidence of SWEET hardware. These messages show more launchd activity; repeated `xpcproxy` requests may indicate restarts, and the photo does not show SpringBoard or an iOS frame.
- **Continued runtime:** Another screen photo shows `bluetoothd`, `contactsd`, `assistantd`, `sharingd`, `remindd`, and repeated `xpcproxy` spawn requests. `SWEET AKS` reports successful selector responses for these processes. No SpringBoard name, GUI frame, or panic is visible in this capture; the current phone boot remains a userland-service milestone.
- **Display bridge candidate, local only:** The Linux Exynos7580 DECON handoff and Samsung loader already scan out physical `0x67000000` as 1080×1920 XRGB8888, which the early XNU text proves on-device. The A5 build previously excluded the donor `AppleCLCD`, omitted `/arm-io/disp0`, and left `boot_args.Video` empty. Patch `0109` enables the existing software IOSurface/IOMobileFramebuffer service for A5XELTE, supplies the exact DECON scanout geometry directly, keeps the A5 logical canvas at 1080×1920, and adds a matching `disp0` ADT node. It does not perform cold panel init or add a GPU driver. The active phone is still running the previous BOOT; the candidate has not been flashed or shown on-device.
- **Repeated service attempts:** A further phone photo shows many `bluetoothd` and `xpcproxy` "Allowing spawn" messages, plus `A5XELTE Sandbox: symptomsd privilege 1002 denied`. The message confirms spawn authorization, not process survival. Sustained repetition is consistent with a service restart loop but its exact exit reason is not yet captured. No SpringBoard record is visible.
- **Display candidate build/package:** The first build exposed an A5-only missing `sweet_console_muted` symbol; patch `0109` now includes the console mute hook and preserves panic output. The second local XNU link succeeded with `AppleCLCD` and `IOMobileFramebuffer` symbols, Mach-O SHA-256 `48e127fc38422cd3c573c781b36e49d92bfe951f9139b9ac5d0c06ef2307b18b`; the usual `dsymutil` `/dev/fd/62` step failed after link. The ADT parser confirms `disp0` compatible `samsung,a5xelte-display`. The 20,252,688-byte Samsung BOOT candidate SHA-256 is `ad90032bdf22f06507b4d9a4f20048b937e17b2be66f20a327e99e9ad06e93ab`, with `ANDROID!` header, `emmcroot=USERDATA emmcwrite=1`, and `a5fb_noconsole=1` so the text console mutes after the first software-presented frame. It is a local build and package check only; device graphics are unverified.
- **Display BOOT trial started:** Mac ADB identified TWRP `a5xelte`, confirmed BOOT p10 at `/dev/block/mmcblk0p10` and 33,554,432 bytes, transferred the 20,252,688-byte candidate, wrote exactly that count to p10, read back its `ANDROID!` first eight bytes, and rebooted. USERDATA was not retransferred. Physical display-service registration, frame presentation and SpringBoard remain unverified until the new phone screen result.
- **Later service log:** The user reported `A5XELTE Sandbox: tccd op 5` and continuing console output from that trial. This proves the tccd sandbox hook was reached; the line alone gives neither its result nor evidence of SpringBoard or an iOS frame. The boot remains the display-bridge trial until a later device observation settles it.
- **Persistent boot-log candidate build:** Patch `0110` ports the donor's RAM console ring and periodic flush to A5XELTE's eMMC. `_cnputs` appends without locking or device I/O, including after framebuffer console mute. The logger is opt-in with `a5log=1`, requires the writable USERDATA root, and writes only the first 8,193 sectors (4 MiB + 512 bytes) of the separate GPT-verified SYSTEM p20. Both the root and log sector writer check LBA bounds. This overwrites the former unused `A5XNU` HFS test volume's beginning, which the user authorized erasing; USERDATA remains the iOS root. Recovery p11 and EFS p3 are outside the write ranges. The Mach-O linked with SHA-256 `f4f0ea80a991abfae163f6854e32f7241504687bffca14c48e374e9d401daf63`; the known local `dsymutil` `/dev/fd/62` failure followed the link. The 20,252,688-byte BOOT candidate SHA-256 is `d4f59ca6c5826b4654b6aa9696472a867fb6fb8a9d73418fe4c7799d67904e2a`. Device logging and recovery readback are not yet verified. `tools/a5xelte-log-pull.sh` and `tools/a5xelte-log.py` read and decode the private log after returning to TWRP.
- **Boot-log BOOT trial started:** Mac ADB identified `omni_a5xelte` TWRP and confirmed `BOOT -> /dev/block/mmcblk0p10` and `SYSTEM -> /dev/block/mmcblk0p20` through the platform by-name links. The BOOT and SYSTEM sizes were 33,554,432 and 3,145,728,000 bytes. The 20,252,688-byte candidate was pushed to TWRP, written only to BOOT p10, and its `ANDROID!` header was read back before reboot. This is a transfer/header check, not a full hash readback; the user requested skipping full partition hashes. No device proof of a successful SYSTEM log flush yet.
- **Frame service screen evidence:** On the boot-log trial, the user's phone photo shows `SWEET FB: AppleCLCD 1080x1920 stride 4320 at 0x67000000` below the A5XELTE CPU and platform lines. This confirms AppleCLCD registration with A5XELTE's physical scanout address and geometry on the phone. `SWEET FB` is still a donor label. The photo does not show a presented iOS frame, SpringBoard, or a successful persistent log flush. The user is leaving the device running for five minutes before returning to TWRP for the first log readback.
- **Persistent log recovered from the phone:** After that run, TWRP read the reserved SYSTEM log area. The `A5XLOG1` decoder recovered flush #329 and 234,303 console bytes. The record contains `Early boot complete. Continuing system boot.`, ten SpringBoard spawn authorizations, nineteen backboardd authorizations, 308 bluetoothd authorizations, and eleven fairplayd.H2 authorizations. It contains AppleCLCD registration but no framebuffer swap/present record. Spawn authorization alone does not show whether those processes survived. Two CMD17 errors also occurred; their relation to the missing GUI is unproven. Raw device logs remain private and are not committed.
- **Crash diagnostics and two-core candidate:** Patch `0111` enables the donor's bounded exit/crash diagnostics on A5XELTE and shortens repetitive spawn messages. Patch `0112` prepares CPU0 plus same-cluster CPU1 using the actual Exynos7580 MPIDR IDs 0 and 1, PSCI CPU_ON, banked GICv2 per-CPU initialization and target-mask IPIs. It uses the physical counter for kernel time and blocks direct user-space use of the inconsistent virtual counter, following the verified Linux SMP port. The ADT now describes CPU1; the trial boot argument selects `cpus=2`. This is source preparation, not yet device proof that CPU1 started or that SpringBoard drew a frame.
- **First SMP phone result:** The `0111`/`0112` Mach-O linked (SHA-256 `1ed976ab4739f2086fd3a5b877dccbccf1beacd011f47d3b475e4bfe9c26bb49`); final `dsymutil` failed after link. Mac ADB wrote the 20,252,688-byte BOOT candidate to p10 and read back its `ANDROID!` header. The user's screen photo proves PSCI `CPU_ON rc=0` for MPIDR 1 and shows CPU1 executing `arm_cpu_init()` and `initCPU()` at EL1. It then reports `IRQs=0`, CPU1 not alive after five seconds, and `startup timeout (reset/MMU/GIC/SGI) @SweetCPU.cpp:183`. This is a secondary CPU startup failure, not verified SMP scheduling.
- **SGI-service candidate:** Patch `0113` briefly unmasks CPU1 interrupts inside `initCPU()` to service its queued self-SGI before returning to XNU's startup continuation, then restores the previous mask. It prints banked GIC group, enable, pending, CPU-interface control, priority-mask, and highest-pending state around that step. The revised Mach-O linked (SHA-256 `1992418de65f990750853554c38aeebdf1e34fbb526f9c6d72c9d946584e8e7b`); final `dsymutil` again failed after link. The 20,254,736-byte BOOT candidate SHA-256 is `4d41d9feeaa57d8f9ac2f65869fcfdf5d1e468a7cef8249ce3ecbfecf65bd195`. Mac ADB identified `a5xelte` TWRP, wrote only BOOT p10, read back its `ANDROID!` header, and rebooted. Device result pending.
- **SGI-service phone result and INTID fix:** The user's next photo shows CPU1's own SGI pending (`pending=00000002`, `hppir=00000401`), then consumed (`pending=0`, `hppir=000003ff`) after the brief unmask. `cpu=1 ... NOT alive after 5s` still panics. The GICv2 IAR/HPPIR lower ten bits are the INTID; bits 10–12 encode the sending CPU for SGIs. The shared IRQ handler instead compared the entire `0x401` against `<16`, classifying SGI1 from CPU1 as an ordinary interrupt and never clearing XNU's `SIGPdisabled` flag. Patch `0114` masks the GICv2 IAR with `0x3ff`, preserving the unmasked value for EOI. This is consistent with Arm Trusted Firmware's GICv2 `INT_ID_MASK`. The corrected Mach-O linked (SHA-256 `833ac90d2de1d44e0657ea020c4bf9400df10df70670301c882e52bccb7933bd`); final `dsymutil` still failed after link. The 20,254,736-byte BOOT candidate SHA-256 is `5aa2a060845d719ceddb89a40f0a4438e59151ca5890a52279116273b4d270a8`.
- **Two-core phone result:** Mac ADB identified `a5xelte` TWRP, confirmed BOOT p10 is 32 MiB, wrote the `0114` BOOT image only to p10, checked its `ANDROID!` header, and rebooted. The user reports seeing `A5XELTE CPU: 2/2 CPUs initialized, SGI1 verified` near the start of boot and says the previous CPU1 startup panic no longer occurs. This is user-verified two-core initialization and SGI delivery. Sustained multi-core operation under iOS userland load and an actual SpringBoard frame are still unverified.
- **Two-core runtime log and backboardd crash:** TWRP read flush #62 of the reserved SYSTEM log area: 59,842 console bytes. It confirms `2/2 CPUs initialized, SGI1 verified`, AppleCLCD registration, `exec SpringBoard`, and two `backboardd` launches followed by `NSInvalidArgumentException`: `-[NSURL initFileURLWithPath:]: nil string parameter`. The backboardd crashes are recorded for PIDs 57 and 158. `installcoordinationd` also reports `Failed to fetch system container URL`. `exec SpringBoard` proves an exec event, not successful process survival or a rendered frame. The raw log stays private in `/tmp`.
- **Eight-core source preparation:** The Exynos7580 Linux device tree describes two clusters of four Cortex-A53 cores, with MPIDRs `0`–`3` and `0x100`–`0x103`. Patch `0115` expands the A5XELTE board to eight CPUs and two topology clusters; the ADT generator supplies those seven secondary nodes. `cpus=N` continues to select a prefix. Build and phone verification are separate milestones below.
- **SMP scope narrowed after device tests:** The eight-core trial reached CPU1 but panicked on CPU0 with an unexpectedly owned ticket lock while registering the second physical cluster. Selecting `cpus=4` on the same eight-core-capable source instead produced a user-visible `4/4 CPUs initialized, SGI1 verified`. TWRP recovered flush #181 of the reserved SYSTEM console area (77,413 bytes). That four-core log shows `exec SpringBoard`, repeated `backboardd` SIGABRT crashes with `NSInvalidArgumentException: -[NSURL initFileURLWithPath:]: nil string parameter`, and `installcoordinationd: Failed to fetch system container URL`. It does not show a rendered iOS frame. Raw logs and Apple binaries remain private outside git.
- **Backboardd crash localization:** Symbolication of the stock iPhone 8 Plus iOS 16.7.16 `backboardd` and dyld shared cache maps the failing path to `-[BSSystemContainerForCurrentProcessPathProvider libraryPath]` / `cachesPath`. `backboardd` obtains that provider from `BSPathProviderFactory`; BaseBoard asks `container_system_path_for_identifier` for the system shared directory and passes the resulting string to `NSURL fileURLWithPath:`. The observed `nil` parameter establishes a missing system-container path as the immediate failure. Whether the underlying cause is missing entitlements, containermanagerd failure, or unsupported filesystem/persona behavior remains unproven. The stock `backboardd` carries `com.apple.security.system-container=true`; the local HFS image has the expected `/private/var/containers` parent directories.
- **Container entitlement diagnostic trial:** Patch `0116` adds bounded boot-console traces for the parsed XML entitlement count and `com.apple.security.system-container` value of `com.apple.backboardd` and `com.apple.containermanagerd`. The A5XELTE Mach-O linked with these strings; the known `dsymutil` `/dev/fd/62` error followed the link. A 20,258,832-byte BOOT candidate selects `cpus=4`, USERDATA root and `a5log=1`. Mac ADB identified `a5xelte` in TWRP, confirmed BOOT p10 at `/dev/block/mmcblk0p10` with size 33,554,432 bytes, wrote exactly 20,258,832 bytes to p10, read back its `ANDROID!` header and rebooted. This is a header check, not a full partition hash comparison. Runtime trace and crash outcome await the next TWRP log pull. SYSTEM and USERDATA images were not retransferred.
- **Entitlement trace and other crashes:** TWRP recovered flush #31 (51,708 bytes) from that trial. Four cores again initialized with SGI1. The kernel parsed 80 XML entitlements for `com.apple.backboardd` and found `com.apple.security.system-container=1`; therefore the immediate missing system-container path is not explained by absent backboardd XML entitlement. `com.apple.containermanagerd` had 17 parsed entitlements and `system-container=0`, matching its stock binary, which instead has `com.apple.private.security.container-manager`. The log records one `backboardd` SIGABRT with the same `nil` URL exception and one `lsd` SIGABRT. Symbolication places the `lsd` trace in CoreServices' `-[_LSDefaults systemGroupContainerURL]` lazy initialization; its exact abort reason was not printed. Neither crash is an illegal ARM instruction trap. `launchd` executed `xpcproxy` 144 times and `bluetoothd` 11 times in the recovered window; repeated exec lines alone cannot prove those processes crashed. `usermanagerd` repeatedly failed atomic writes of `usersession.kb` and `persona.kb` with ENOTSUP (45), while `securityd` received an unsupported AKS selector result and the sandbox denied one `logd` and one `rapportd` privilege. No kernel panic appears in the recovered window. Raw logs remain private.
- **A5XELTE HFS atomic-write candidate:** The donor's existing HFS `RENAME_SWAP` fallback (`0052`, `VNOP_EXCHANGE` for two regular files on the same volume) was compiled only with `ARM64_BOARD_CONFIG_SWEET`. Patch `0117` enables that same fallback for A5XELTE. This matches the observed `usermanagerd` ENOTSUP but is a hypothesis for improving the system-container lookup; the next phone log must confirm whether the atomic-write errors and service crashes change. The A5XELTE Mach-O linked, followed by the known `dsymutil` `/dev/fd/62` error. The 20,258,832-byte four-core candidate was written only to BOOT p10 by Mac ADB after checking the 32 MiB partition; the `ANDROID!` header was read back and the phone rebooted. USERDATA and SYSTEM were not retransferred. Runtime result pending.
- **0117 physical screen observation:** The user photographed a white Apple logo near the centre of the Samsung display with console text still visible along the bottom (`SK[0]: __nexus_register` and `rapportd(83): nxprov_create:`). This proves pixels beyond the prior text-only console are being drawn in this boot. It does not yet prove SpringBoard rendered a frame, completed its startup, or reached an interactive home screen. The persistent log from this boot has not been recovered yet; in particular the hypothesized reduction in `usermanagerd` ENOTSUP and container crashes remains unverified.
- **SWEET-only guard audit after 0117 (source inspection, not a device result):** `bsd/kern/kern_mib.c` selects the macOS arm64 compatibility branch for A5XELTE and returns hardcoded `iPad8,6` as `hw.machine` to iOS processes, while the current A5 ADT inherits `iPhone10,3`/`D22AP` from the SWEET donor. Both identities also differ from the installed iPhone 8 Plus IPSW (`iPhone10,2`/`iPhone10,5`): resolve the identity consistently before changing this guard. Syscall 435 `pid_hibernate` is `nosys` on A5XELTE because `syscalls.master` and `bsd/vm/vm_unix.c` include SWEET but not A5XELTE; the donor observed that SpringBoard calls this when an app leaves the foreground, so it is a later navigation blocker. `bsd/kern/kern_core.c` defaults full process core dumps on for A5, unlike SWEET; large daemon crashes may therefore consume time and root space. The SWEET `/dev/null` console tee in `bsd/dev/mem.c` and bounded IOKit open/match diagnostics in `IOUserClient.cpp` are also absent on A5, limiting crash analysis. SWEET-only HFS dirty non-journaled root overrides and Qualcomm UFS, USB, touch, keys, SPMI, RPMh and battery drivers are hardware-specific and should not be enabled wholesale on Exynos. None of these source guards establishes the cause of the current Apple-logo state; inspect this boot's log before making another trial image.
- **0117 recovered log:** TWRP recovered flush #71 (72,322 console bytes) into a private local file. It records `4/4 CPUs initialized`, one `exec SpringBoard`, one `exec backboardd`, `SWEET FB: display on`, five BGRA swaps, then `SWEET FB: display off`. No kernel panic, `NSInvalidArgumentException`, or `usermanagerd` ENOTSUP line occurs in this saved window. The observed Apple logo fading is consistent with the explicit display-off request; this does not prove SpringBoard rendered an interactive frame. `PosterBoard` is missing from the staged minimal root. The log is not committed.
- **0118 Home/HID and touch diagnostic BOOT:** Patch `0118` enables the donor HID buttons and touch provider on A5XELTE, sets the logical touch area to 1080×1920, adds a mechanical Home button to the generated ADT, polls the Linux-documented Exynos7580 GPA2-5 Home GPIO with debounce and routes its edges to the HID provider. Its touch component is currently a **read-only register probe**, not a functioning ZT7538 touch driver: it records interrupt/pin-mux/I²C clock state before PMIC power and I²C implementation. The patch also enables A5's `pid_hibernate` syscall, defaults core dumps off, and adds `a5fb_keep_on=1` to ignore display-off requests for this test. The A5 Mach-O linked; final `dsymutil` failed after link on local `/dev/fd/62` permissions. The 20,258,832-byte BOOT candidate SHA-256 is `ed6b8af5610cc4d390cddf7665fd720aaa65badea219d88486f07f0e1edfff8f`. Mac ADB identified `a5xelte` in TWRP, confirmed BOOT p10 at 33,554,432 bytes, saved the current 32 MiB BOOT contents locally, wrote only the candidate's 20,258,832 bytes to p10, read back its `ANDROID!` header, and rebooted. SYSTEM and USERDATA were not written. Home event, touch probe, display behaviour and iOS runtime results still require phone observation and a later TWRP log pull.
- **0118 phone observation before log recovery:** The user reports that a clock and then the iOS home screen became visible. The interface is extremely slow. Volume buttons responded, and the user subsequently confirmed that the physical Home button also works. The initial lack of Home response was therefore transient or an observation made before the interface responded; its cause is unknown. Touch still did not respond: no touchscreen contacts can be expected from this image because it has no ZT7538 event driver. The runtime bottleneck and touch controller state require the persistent log after a return to TWRP. The visible home screen and working physical keys are user verified; touch and smooth interaction are not.
- **0118 recovered log:** TWRP recovered 127,448 decoded console bytes into a private local file, excluded from Git. It records `4/4 CPUs initialized, SGI1 verified`, one `exec SpringBoard`, one `exec backboardd`, the HID buttons and touch providers opening, and Home, Power and volume GPIO edges reaching HID clients. The touch probe observed GPC3 `con=0` and `pud=0x55`; the I2C2 peripheral clock gate was on, while the PMIC HSI2C3 gate was on but its GPM0 pins were not in the Linux touch power profile. I2C2 itself read `con=0xa stat=0` before initialization. These observations explain why merely registering a touch HID provider does not produce contacts. The repeated `xpcproxy` and `bluetoothd` exec lines do not alone establish crash or restart causes.
- **0119 ZT7538 identity trial:** The working native Linux port identifies a ZT7538 at I2C2 address `0x20` and reads chip code `0xe538` from register `0xcc00`; it powers AVDD through S2MPU03 LDO33. Patch `0119` adds an opt-in, bounded S3C2440 I2C2 transfer that temporarily selects Linux's GPC3 function and pull settings, sends the register pointer, reads two chip-code bytes, logs the result and restores GPC3. It does not configure the PMIC, run touchscreen initialization, alter NVM or report contacts. The A5 Mach-O linked; final `dsymutil` again failed on local `/dev/fd/62` permissions after link. The 20,258,832-byte BOOT candidate has `a5touchbus=1` and SHA-256 `a738982a9b8e80d8de78ddd7058ac844c5c842417b3a2c4415e9c568d44014f8f`. Mac ADB identified TWRP `a5xelte`, saved all 32 MiB of the previous BOOT p10 locally, wrote only p10, read back its `ANDROID!` header and rebooted. Device I2C result and touch events are not yet verified. SYSTEM and USERDATA were not written.
- **0119 recovered log:** TWRP recovered flush #106 (108,419 decoded console bytes) into a private local file. The early I2C2 probe reached the ZT7538 address but logged `address NACK stat=0xf1`; no chip-code bytes were received. The same boot reached `4/4 CPUs initialized`, `exec SpringBoard`, `exec backboardd` and display-on logs. This is consistent with Linux's separate LDO33 touch supply being off, though a bus setup fault remains possible. It does not establish a failed touchscreen IC.
- **0120 early touch text trial:** Patch `0120` adds an opt-in Exynos7580 HSI2C3 PMIC transfer, checks the S2MPU03 ID and the existing LDO33 voltage selector, enables only LDO33 at 3.3 V, then follows the native ZT7538 power-on sequence. It requires chip code `0xe538`, firmware checksum `0x55aa` and readable geometry before enabling mode-0 contact reports. A bounded early kernel thread polls the interrupt GPIO, acknowledges packets and logs up to 256 coordinate lines. It does not yet feed coordinates to iOS HID and does not program touchscreen NVM. Each failed stage logs and aborts touch setup without blocking boot. The A5 Mach-O linked without warnings in the new touch source; final local `dsymutil` failed after link on `/dev/fd/62` permissions. The 20,260,880-byte BOOT candidate has `a5touchlog=1` and SHA-256 `d800007cbca1499aa6652e47bac2db6d9346d75a55845a7b5a1e90d396c6dfdd`. Mac ADB identified `a5xelte` TWRP, saved all 32 MiB of the previous BOOT p10 locally, wrote only p10, read back `ANDROID!` and rebooted. The PMIC, ZT7538 and contact logger runtime outcomes remain unverified until the next screen observation or TWRP log pull. SYSTEM and USERDATA were not written.
- **0120 phone result:** TWRP recovered flush #21 (36,330 decoded bytes) from the reserved SYSTEM diagnostic area. Four CPUs initialized, SpringBoard executed, and physical key events reached HID clients. The PMIC probe stopped at `HSI2C error status=0x20005 irq=0 fifo=0x1000001` and never read the S2MPU03 identity. The status includes `NO_DEV_ACK`; therefore no LDO33 write, ZT7538 initialization or contact polling happened. This does not establish a defective touchscreen. In recovery Linux, `/sys/bus/i2c/devices/3-a066/name` identifies `s2mpu03-pmic`; the `a` in `3-a066` records a ten-bit client registration. The native Linux port previously read ID `0x02` through its standard seven-bit driver, so the address mode on XNU requires a bounded physical test rather than an assumption. Raw boot logs remain outside Git.
- **0121 PMIC address diagnostic trial:** Patch `0121` first tries the existing seven-bit PMIC identity read and, if that fails, resets HSI2C3 and retries in ten-bit controller mode. It logs mode, direction, byte count and controller status. LDO33 can only be enabled after PMIC ID `0x02`, the expected existing voltage selector, and write readback; other partitions and touch NVM are untouched. The A5 Mach-O linked; the known final `dsymutil` `/dev/fd/62` error occurred after link. The 20,260,880-byte BOOT candidate SHA-256 is `9ce9054cace3d907ec6fb1bc7bf63db52662447e32c29d2702dd85dbcb1beb80`. Mac ADB verified `a5xelte` in TWRP and the 32 MiB BOOT p10, saved the entire previous BOOT locally, wrote only p10, read back its `ANDROID!` header and rebooted. The user has not yet reported a phone result or returned to TWRP for the diagnostic log. SYSTEM and USERDATA were not written.
- **0121 recovered PMIC result:** TWRP recovered flush #5 (30,799 decoded bytes). Four CPUs initialized and SpringBoard executed. Both the seven-bit and ten-bit attempts failed in the **read phase** with `status=0x20005` (`NO_DEV_ACK`); the write phase produced no error record. No PMIC identity or LDO33 readback was obtained, and no touch contacts were logged. The ten-bit registration observed in recovery is therefore not by itself a fix. Physical Home/volume events still appear in the log.
- **0122 Linux HSI2C transfer alignment trial:** The working native Linux controller initializes `HSI2C_CTL` with `FUNC_MODE_I2C | MASTER`, while the previous XNU probe used `MASTER` alone. Patch `0122` adds the I2C function bit, uses Linux's 400 kHz timing values for this 100 MHz source, seven-bit address `0x66`, per-message FIFO triggers, and the Linux register programming order. XNU still polls boundedly because it has no HSI2C IRQ handler. PMIC identity and LDO33 voltage checks remain required before any regulator write; no touch NVM operation was added. The A5 Mach-O linked; final `dsymutil` failed after link on `/dev/fd/62`. The BOOT candidate is 20,260,880 bytes, SHA-256 `e08cca903ae37ed7329e6db27eb0d1352cc72eda2e8413a6b792f241f67de49e`. Mac ADB identified `a5xelte` TWRP and 32 MiB BOOT p10, saved the complete previous BOOT locally, wrote only p10, read back its `ANDROID!` header and rebooted. Runtime PMIC, ZT7538 and contact results are not yet established. SYSTEM and USERDATA were not written.
- **0122 recovered result:** TWRP recovered diagnostic flush #76 (79,468 decoded bytes). Four CPUs initialized and SpringBoard executed. The old HSI2C code reported the PMIC register-address write as complete at `status=0x20001`, then stopped the read phase on `status=0x20002`, `irq=0`. On Exynos7, the low nibble of `TRANS_STATUS` is the master state machine; the old XNU code mistook state `1` for completion and state `2` for an Exynos5 abort. No S2MPU03 identity, LDO33 enable, ZT7538 initialization or contact event was observed in this boot.
- **0123 integrated touch candidate and device observation:** Patch `0123` uses the Exynos7 `INT_STATUS` completion/error flags for HSI2C3 and polls the transfer FIFO, with bounded retries and stop/idle handling. It preserves the S2MPU03 ID `0x02`, LDO33 3.3 V selector and readback gates. It also carries the Linux ZT7538 send retry, mode-0 status/slot rules, heartbeat handling, contact release and bounded power-cycle recovery into XNU, and forwards live contacts to `sweet_hid_touch`. No touch NVM programming was introduced. The A5 Mach-O linked; local `dsymutil` still failed after link on `/dev/fd/62`. The 20,260,880-byte BOOT candidate SHA-256 is `b738b00c4fe0c30db58816ea482956ef40abc2df18a98d1c6500f269142e5085`. Mac ADB confirmed `a5xelte` TWRP and 32 MiB BOOT p10, saved the complete prior BOOT privately, wrote only p10, and rebooted. At the user's request, no post-write readback/hash comparison was performed. The user reports both touch log lines and an iOS UI response when touching an on-screen item. Contact reporting and basic iOS touch input are therefore device verified; sustained operation, multitouch and wake behavior remain untested. SYSTEM and USERDATA were not written.
- **0123 performance baseline recovered in TWRP:** A read-only pull of the reserved SYSTEM console area yielded flush #160 and 172,849 bytes. This boot reports `4/4 CPUs initialized, SGI1 verified`, one `exec SpringBoard`, one `exec backboardd`, AppleCLCD at 1080×1920 and working touch/HID events; no `panic(` occurs in the recovered window. It also records 368 `exec xpcproxy`, 45 `exec bluetoothd` and 44 `exec fairplayd.H2` lines. These counts suggest additional service churn but do not establish their exit reasons or CPU cost. The inherited framebuffer code limits A5 to 10 Hz unless overridden and copies presented surfaces on the CPU; this is source evidence, not a measured frame rate. The private raw log remains outside Git. See `docs/a5xelte-gpu-port.md` for the distinct Mali bring-up requirements.
- **0124 display-pacing trial started:** A5XELTE now defaults to a 30 Hz panel-copy cap, retains the donor's 10 Hz default, and logs aggregate actual frames per second, copied bytes and average/maximum CPU copy time every 300 vsync callbacks. This changes CPU presentation only; it does not render with Mali. The Mach-O linked; the usual local `dsymutil` `/dev/fd/62` failure happened after link. A 20,260,880-byte BOOT candidate with `a5fb_hz=30 a5fb_perf=1 a5touchlog=0` was packaged (SHA-256 `aad4a4409d2b2931e872eafae3a5b657e2a398effb57165c008c16791b510ec3`). Mac ADB verified `a5xelte`, BOOT p10 at 33,554,432 bytes and its by-name link, then saved the full current BOOT privately, wrote only p10 and rebooted. Per the user's request, no post-write hash comparison was performed. Actual iOS frame rate, responsiveness and GPU status await physical observation and a later TWRP log pull.
- **0125 GPU resource foundation, not device tested:** A5XELTE ADT now describes the Exynos7580 G3D, CMU and PMU ranges and JOB/MMU/GPU SPIs. The opt-in `A5XELTEGPU` service (`a5gpu=1`) publishes these resources without powering, mapping or submitting work to Mali. The A5 Mach-O linked with its `start()` symbol; the local `dsymutil` `/dev/fd/62` restriction again caused the final build command to exit 2 after link. No 0125 BOOT image has been flashed, no GPU ID has been read and no acceleration is claimed. The 0124 phone trial should establish the display baseline before an active G3D test.
- **0126 passive GPU status trial started:** An explicit `a5gpu=2` mode reads G3D PMU status, checks both native CMU gate banks only if the power domain is already on, and reads Mali `GPU_ID` only if those gates are enabled. It changes no regulator, PMU, clock or GPU register and submits no GPU job. The A5 Mach-O linked with the probe code; the known local `dsymutil` `/dev/fd/62` restriction caused exit 2 after link. A fresh ADT differed from the 0124 one only by the new `/arm-io/gpu` node. A 20,283,408-byte BOOT candidate was packaged (SHA-256 `b41607e5f5ce5d75987b6ac5ebd435afabd7871f95fbabde8905a589554f2c57`). Mac ADB confirmed `a5xelte` TWRP and 32 MiB BOOT p10, saved the full previous BOOT privately, wrote only p10 and rebooted. The user-requested post-write hash comparison was omitted. Device probe result is pending. Missing coordinated BUCK4 and TOP/MIF/INT clock providers still prevent safe active GPU power-on; even a successful ID probe would not establish GPU acceleration.
- **0124 phone performance result:** A read-only TWRP pull of the reserved SYSTEM console area recovered flush #102 (107,493 bytes). `A5XELTE FB` confirms the requested and effective cap were both 30 Hz. Twelve consecutive ~5.3-second metric windows total 23 present calls in 63.857 seconds, about 0.36/s; three windows copied zero bytes. Maximum recorded copy time was 11,589 µs and typical full-frame copies were about 5–6 ms. The cap and memcpy duration therefore do not explain the extremely low present rate in this observed interval. The log shows one SpringBoard and one backboardd exec, 323 xpcproxy execs, 22 bluetoothd execs, 34 fairplayd.H2 execs, 38 `SWEET AKS: SpringBoard sel 8` failures with code `0xe00002c7`, and no `panic(`. Those counts do not establish process CPU usage or exact restart causes. The user reports the GUI is still equally slow.
- **0126 device results and ADT correction:** The first 0126 BOOT printed repeated `IODeviceTreeSupport.cpp:579 unexpected`, because the new `gpu` ADT node advertised interrupts without a parent `#interrupt-cells` description; the early display visibly flickered. The existing SYSTEM ring did not update beyond the 0124 flush. Removing the unused interrupt cells from the ADT left IRQ numbers only as resource-service properties. In the second 0126 BOOT trial, the phone screen showed the service registering and a passive `GPU_ID=07201000`, product `0720` read: the G3D domain and checked CMU gates were already active at that point. Immediately afterward XNU panicked in `vm_map_delete` while `ml_io_unmap` tried to remove a permanent map created by `ml_io_map`. This proves a readable Mali-T720 register, not job execution or iOS acceleration. The second BOOT candidate SHA-256 was `cc583907b6a4a40b279a45e8a61bf11f334612859fd986308aba7ac5831a0f8c`; only BOOT p10 was written, with the working 0124 BOOT backup retained privately.
- **0127 mapping-lifecycle fix trial started:** XNU `osfmk/arm/io_map.c` allocates `ml_io_map` mappings as permanent; `ml_io_unmap` frees through `kernel_map` and therefore panics on them. Patch `0127` uses `ml_io_map_unmappable(..., VM_WIMG_IO)` for the PMU, CMU and GPU register pages before the existing unmaps. This preserves the passive read-only probe and keeps the CPU display fallback. The A5 Mach-O linked; the known local `dsymutil` `/dev/fd/62` restriction again caused exit 2 after link. The 20,283,408-byte BOOT candidate SHA-256 is `7439dd3967b0f90f09d0cdfb53d678317a364c162b3b99582154dac105983ae4`. Mac ADB reconfirmed `a5xelte` TWRP and BOOT p10 size, wrote only p10 and rebooted. Per the user's preference, no post-write hash comparison was performed. Phone runtime outcome is pending.
- **0127 phone result:** The SYSTEM ring pulled in TWRP after the 0127 trial recorded the `A5XELTE G3D` resource service, passive `GPU_ID=07201000` / product `0720`, `exec SpringBoard`, and `exec backboardd`. It recorded no repeat of the `vm_map_delete` panic. The framebuffer reported 30 Hz requested/effective but only 2 presents over 5330 ms in one window (0.3/s); a full 8,294,400-byte copy took about 5.3 ms. This is CPU display evidence, not Mali rendering or an iOS GPU API.
- **0128 offscreen GPU job trial started:** `a5gpu=3` gates on live shader/L2 resources, idle JS1 and unused AS0, maps only a private 16 KiB Mali VA range, and submits one Midgard `WRITE_VALUE` job to zero a sentinel word. Finite polling records completion, fault state and output. The candidate A5XELTE Mach-O linked, while the final `dsymutil` step hit the known `/dev/fd/62` restriction. The private 20,283,408-byte BOOT candidate has SHA-256 `2f2e648930d759794dee9ad7c51c5e6ec064d66b96fa5d3e521a0bdfa941834c`. Mac ADB confirmed a5xelte TWRP and BOOT p10 (32 MiB), saved the previous BOOT privately, wrote only p10, and rebooted. No post-write hash comparison was run at the user's request. Runtime result is pending; this cannot accelerate SpringBoard until a graphics userland contract is implemented.
- **0128 phone result:** Read-only TWRP pull recovered SYSTEM flush #56 (60,286 bytes). GPU ID was `07201000`, AS0 and JS1 were idle and present, but shader/L2 ready masks were zero, so the 0128 job was correctly skipped. SpringBoard and backboardd executed; no panic was recorded. TWRP's own regulator sysfs reported `vdd_g3d 787500 disabled`, which describes recovery, not XNU boot. No Mali execution or iOS acceleration is established.
- **0129 BUCK4 and core power trial started:** Patch `0129` reuses the early touch HSI2C3 PMIC path before touch takes ownership, verifies S2MPU03 ID and BUCK4 control, writes only BUCK4's selector to the Linux-verified 900 mV OPP if needed, and checks readback. It requires inherited TOP mux and a core divider of at least three before powering L2, shader and tiler in Panfrost's order with finite polls. The private `WRITE_VALUE` job remains gated by these checks. The XNU Mach-O linked; final `dsymutil` again failed at `/dev/fd/62` after link. The 20,283,408-byte BOOT candidate SHA-256 is `17291b355097dd3a8525828e8f506ad488f457fda5deaa42f5742f983a0d3d7c`. Mac ADB verified TWRP `a5xelte` and BOOT p10 (32 MiB), wrote only p10 and rebooted; the user-requested post-write hash comparison was omitted. GPU core, job and iOS runtime results await the next device observation and TWRP log pull. This is not yet a graphics renderer.
- **0129 phone result:** TWRP pulled SYSTEM flush #39 (47,531 bytes). The G3D ID and present masks were correct (`shader=3`, `tiler=1`, `l2=1`), but `mux=0` and divider register `0x30` were outside the conservative TOP/3 gate. No BUCK4 write, Mali core power-on or job submission happened. XNU showed 4/4 CPUs, SpringBoard and backboardd, with no recorded panic. This isolated the inherited clock selection as the next prerequisite.
- **0130 safe-clock trial started:** Patch `0130` first verifies BUCK4 at 900 mV, then, only with GPU cores off, mirrors native Exynos7580 CCF: set both dividers to eight, switch the core mux to TOP with bounded status polling, set core /3 and peripheral /4, verify register readback, power L2/shader/tiler, and attempt the same private `WRITE_VALUE` job. It also requires the PMIC check even if firmware left cores already ready. The A5XELTE Mach-O linked; final `dsymutil` failed at `/dev/fd/62` after link. The 20,283,408-byte BOOT image SHA-256 is `ca4e0a223725c6e91863192357ab84766a2e1a79f80ac0cd72169aa7b34a17dd`. Mac ADB verified `a5xelte` TWRP and BOOT p10, wrote only p10 and rebooted; no post-write hash comparison was run. Core/job outcome awaits physical boot and a later TWRP log pull. iOS graphics remain on the CPU path.
- **0130 phone result: first Mali job verified:** A read-only TWRP pull of SYSTEM flush #10 recovered 30,302 bytes. S2MPU03 BUCK4 moved from selector `0x3e` (787500 uV) to readback 900000 uV with enable control unchanged. The clock settled at TOP mux `1`, divider `0x32`, status `0x2/0`; L2 mask `1`, shader mask `3`, and tiler mask `1` all became ready. The one private Midgard `WRITE_VALUE` job completed with job IRQ `0x2`, MMU IRQ `0`, AS fault `0`, and the 64-bit sentinel changed to zero. Four CPUs initialized, SpringBoard and backboardd executed, and no `panic(` was recorded. This proves GPU execution in an isolated buffer, not fragment rendering, framebuffer compositing, or iOS UI acceleration.
- **0131 offscreen fragment-clear trial started:** `a5gpu=4` submits a Mesa 26.2.4 Panfrost v4-style Mali-T720 fragment job on JS0 to clear a private 16×16 RGBA8 buffer and checks all 256 pixels. The existing `a5gpu=3` write-value mode remains available. The A5XELTE Mach-O linked; the final `dsymutil` `/dev/fd/62` restriction still exits after link. The 20,283,408-byte BOOT image SHA-256 is `743ce9330abdf0bcd3630de81033e900a43f88475084deee434ef67ed27b5a04`. Mac ADB confirmed `a5xelte`, BOOT p10 and its 32 MiB size, saved the full working 0130 BOOT privately, wrote exactly 20,283,408 bytes only to p10, and rebooted. No post-write hash comparison was run at the user's request. Fragment completion, pixel output and iOS boot remain unverified until the next TWRP log pull; the test does not accelerate SpringBoard.
- **0131 phone result:** The read-only TWRP pull recovered SYSTEM flush #10 (30,121 bytes). BUCK4, TOP clock, L2/shader/tiler power and Mali ID checks passed. The fragment job reported JS0 IRQ `0x10000`, status `0x58` (`DATA_INVALID_FAULT`), MMU IRQ `0`, AS fault `0`, and `0/256` red pixels; first and last pixels retained the sentinel `0x5a5a5a5a`. SpringBoard and backboardd executed. The GPU did not render this image.
- **0132 page-aligned polygon-list trial started:** Mesa allocates the polygon list in a separate page-aligned BO. Patch `0132` moves the private Mali polygon list from VA `+0x300` to `+0x1000` and the pixel target to `+0x2000`, reserving a full page for each. It adds job-header exception/fault-pointer and GPU fault-register logging. The Mach-O linked; the known `dsymutil` `/dev/fd/62` error followed link. The 20,283,408-byte BOOT candidate SHA-256 is `36319730a577276a07b00617b13292e4a8d80b191d4f1d82ba4c60436638f23b`. Mac ADB confirmed a5xelte TWRP and BOOT p10, wrote exactly that count only to p10, and rebooted. No post-write hash comparison was run. Physical fragment and boot results await the next TWRP log.
- **0132 phone result: first Mali-rendered pixels verified:** TWRP recovered SYSTEM flush #13 (30,922 bytes). JS0 fragment completion IRQ was `0x1`; job header exception was `0x1` (DONE), MMU IRQ, AS fault and GPU fault were zero, and all 256 private target pixels changed from the sentinel to opaque red `0xff0000ff`. SpringBoard and backboardd executed. This proves isolated fragment rendering on the phone, not display scanout or iOS UI acceleration.
- **0133 direct-scanout trial started:** `a5gpu=5` maps 16 pages of the loader's physical DECON scanout buffer into Mali AS0 and submits a one-shot 16×16 opaque-green clear at `(528,944)` with 4320-byte row stride. `a5gpu=4` still tests only private memory. The A5XELTE Mach-O linked; the known `dsymutil` `/dev/fd/62` error followed link. The 20,283,408-byte BOOT candidate SHA-256 is `2b9cff9ee4d52e289e5eb54c732a2eb3fa341c338212da3f3de4090fe830f8e3`. Mac ADB confirmed a5xelte TWRP and BOOT p10, saved the full working 0132 BOOT privately, wrote exactly that count only to p10, and rebooted. No post-write hash comparison was run.
- **0133 phone result: Mali pixels reached the panel:** The user saw the green square. A read-only TWRP pull recovered SYSTEM flush #1 (12,109 bytes): fragment job-header exception `1` (DONE), JS0 IRQ `1`, MMU IRQ `0`, AS fault `0`, and GPU fault `0`. This establishes one GPU-produced visible patch, not CoreAnimation or SpringBoard acceleration. The captured early log does not show later userland boot.

## 2026-11-08 — J7 Prime: first iOS 16 boot with the GPU service up (logo, no lock screen)
- Boot args `j7gpu=7 sweetfb_scale=1 sweetfb_lw=750 sweetfb_lh=1334`. The backboardd crash patch `0320` targets is gone: no
  `IOSurface+0x4ffc` / `_platform_memmove` fault anywhere in the boot (it was 11 per boot before), `sel 6` now reads
  `SWEET SURF: backboardd wrap sel 6 refused: scalars 0 [0x0 0x0] struct in 32, out 2664`, and `0321` copies the client's
  pixels: `SWEET SURF: pixels of 120x120 taken from client address 0x110938000 -> 0` for surfaces 4-11.
- The panel shows the Apple logo and never the lock screen. The log has two independent faults, neither yet fixed:
  1. **Every** GPU submit fails, from frame 1: `submit jc=… slot=1 failed status=-3 js=8` then
     `panfrost_batch_submit failed: 5`. `-3` is the driver's "MMU fault" and `js=8` is the same JS_STATUS the kernel's own
     intentional-fault round prints (`client round 1 (fault) … status=-3 js=00000008`), while its good rounds pass
     (`client selftest PASSED`). So the job manager and AS0 work for kernel memory and no user-space chain has ever run;
     the logo on screen is the one iBoot drew, not a GPU frame.
  2. ~6 minutes in, `MESA: error: DRM_IOCTL_PANFROST_CREATE_BO failed (err=12)` starts and backboardd dies:
     `crash: far=48 pc=sweetgl.dylib+6b71b4 submit_batch+f4`, `f0 0 panfrost_batch_submit+5cc`,
     `f02 panfrost_ptr_unmap+234`, `f03 u_default_texture_subdata+9c`, `f08 surface_to_texture+c8c`,
     `f09 my_texImageIOSurface+35c`. Mesa dereferences the NULL BO that our failed ioctl returned. The kernel table was
     `J7M_MAX_BOS 64` and CoreAnimation maps a BO per IOSurface (textures up to 76 by then), so the slots ran out.
- Patch `0322`: `J7M_MAX_BOS` 64 -> 512, plus the two facts the log did not have - `J7 G3D: fault at <address> in bo N of M
  (… KiB mapped, tables used/total): faultstatus=… mmu_irq=… job_irq=… js_status=…` for the first four faults (AS0+0x1c/
  0x20/0x24, the address matched against the live BO table), and `J7 G3D: client map refused: <why>` naming which of the
  four mapping steps ran out instead of a bare ENOMEM in user space.
- New boot-log diagnosis: the `CREATE_BO failed (err=12)` lines and the backboardd NULL-BO crash are gone; there is no `crash: sig=` line. The selftest still passes its good rounds (`client round 0/2 (good) status=0 js=00000000`) and rejects its deliberate fault (`status=-3 js=00000008`); unowned-VA submit is rejected and client unmap succeeds. The four observed faults decode as translation faults, not permission faults: low status byte `0xc2`/`0xc3` is translation fault at level 2/3; access bits identify READ/WRITE and bits 31:16 identify source `0x7c10`/`0x7c20`. One fault VA `0x38e2440` is exactly the job-chain head, within BO 11 (`0x38e2000`, 64 KiB); another, `0x3d45000`, is BO 18's first page. The faults recur at the first head while every GPU submit remains `status=-3 js=8` from frame 1; frames 1–3 each report GL work but zero draws in place. There is one `serving user clients` / page-table-pool initialization. The log still shows the Apple logo and no lock screen. This locates the issue in GPU translation/page-table visibility or root selection, but does not establish which one.
- Patch `0323` adds fault-status words, AS0 TRANSTAB readback vs. `gJ7M.root`, a pool-bounded walk printing `ent/l2/l3/leaf`, and a 16-entry map/free event ring (`recent` lines) to distinguish stale/missing mappings from a wrong root or VA. It is generated from the reconstructed source; its `git apply --check` validation passes against the 0322 reconstruction. No source-level cause is claimed yet.
- **Device observation (later 0322 boot):** With 21 BOs and 50,640 KiB live, the same three client faults recur — `0x38e2440` and `0x38e2dc0` (`7c1002c2`, translation level 2, read) inside the 64 KiB job-chain BO, and `0x3d45000` (`7c2003c3`, level 3, write) at a BO start — while the deliberate client fault reports `7c2003c1` (level 1). Every submit still returns `status=-3 js=8`, and the recovered console has no `J7 G3D: walk` or `recent` line, so that kernel was built with `0322` only: `xnu/patches/0323-j7-g3d-fault-walk.patch` on `main` is the exported format-patch, not the kernel diff.
- Not established: a GPU-rendered frame, the lock screen, or why the GPU walker faults. The next boot log should include `J7 G3D: walk transtab=...` and `J7 G3D: recent ...` lines; those will distinguish a wrong root/table from a missing leaf or BO lifetime issue.
- Patch `0324` (`xnu/patches/0324-j7prime-g3d-mmu-ready-guard.patch`) is the same kernel diff, generated from the reconstructed tree and byte-verified end to end: `17ab0a1` -> apply `0323` -> apply `0324` -> result matches the committed 0323+A+B+C target. It adds `bool mmuReady;` to `gJ7M`, sets `gJ7M.mmuReady = true;` in `j7m_serve_start` after `j7m_mmu_enable()` succeeds, and refuses selector-2 submit with `kIOReturnNotReady` when `!gJ7M.mmuReady` before `j7m_cache`+`j7m_submit`. It is one file, `iokit/Kernel/arm/A5XELTEGPU.cpp`, 33 lines. It is a hypothesis (first submit faults because AS0/MMU is not yet ready), to be confirmed or refuted by the next boot's `walk`/`recent` lines; if the fault is a wrong root or stale leaf, revert this guard and fix the real cause.

# 2026-11-09 — J7 G3D: verify every BO page before submit (0328-gpu)

- 0327 boots to lock screen/home screen with GPU acceleration but is painfully slow:
  checking only a 4096-page slice per submit left whole BOs unmapped, the job slot faulted
  and every frame waited out the client's five second timeout (`frames=1 elapsed_ms=5250`,
  `fps=0.1`). 0328 therefore verifies every page of every live BO before each submit, against
  PAs cached at map time — a full pass is a table walk (no descriptor walks) and one flush
  per submit instead of per-BO.  A BO that still loses leaves prints one line
  (`submit verify: bo=.. gva=.. pages=.. filled=.. first=.. unknown=.. tables=..`), and a
  clean pass prints a heartbeat every 240 submits, so the next log says whether holes appear
  between map and submit.

- Core change: each BO now keeps `uint64_t *pas`, one PA per page, filled at map time.
  `j7m_bo_page()` answers from `pas` via a pure table walk. `j7m_verify_as()` walks every
  page of every live BO, issues a single `j7m_pool_sync()` + `j7m_mmu_flush()` for the
  changed range. `j7m_bo_new` allocates `pas` with `IOMalloc`/`bzero`; `j7m_bo_free` frees it
  before `bzero(bo, sizeof(*bo))`.

- Expected effect: the 5-second wait timeout per frame disappears, frame rate should improve
  from ~0.1 fps to a much higher rate. Remaining slowness may be Mesa/CPU side.

- Note: the console mute patch (`a5fb_noconsole=1` stopping log text on panel) is deferred
  to future work — it targets `osfmk/console/serial_console.c` which is not in this repo.
  The panel text issue remains as known known in 0327.
# 2026-11-10 — J7 G3D: 0328 device result, panel console mute (0329), pool recycle (0330)

- **Device observation:** 0328 (the aggressive every-page verification that the build actually
  applied) boots on the J7 Prime without a panic and the lock-screen clock is GPU-rendered (white,
  not the CPU path's red). It is still very slow and the screen glitches.
- **Boot log evidence:** `frames=1 elapsed_ms=5321` per present; `client map refused ... 512 of 512
  page tables`; `fault at 4fcac00 ... translation level 2 execute`; `submit ... failed status=-3
  js=8`; `CRASH backboardd ... submit_batch`. The 5 s stalls are page-table pool exhaustion
  (2 MiB, 512 tables, nothing ever returned), not the cost of verification.
- **0329** mutes `_cnputc` for every console op, not only `VC_CONS_OPS`: on this board
  `cons_ops_index` stays on `SERIAL_CONS_OPS` and `serial_putc_options` falls back to
  `fbcon_putc()`, which is what painted the panel, so 0328's gate never fired.
- **0330** asks for a 16 MiB pool (4096 tables, halving until a contiguous block exists), recycles an
  L3 table when its whole 2 MiB window is empty, and syncs only the tables that exist.
- `0328-j7-g3d-verify-every-page.patch` was never applied (build log: `Skipping patch`); it is
  removed so `0328-j7-g3d-verify-every-page-and-mute-console.patch` is the single 0328 again.
# 2026-11-17 — J7: 0373 panicked, 0374 refuses the job at spawn time

- **Device result:** the image built from 0370–0373 (kernel run `43977272777`, boot run
  `43980361019`, sha256 `2058edb1…38b71e`) **panicked at boot**:
  `kern_exec.c:7713 Assertion failed: signature_failure_reason != OS_REASON_NULL`
  — "handshake structure not initialized". Cause: 0373 returned `ENOENT` from the exec-time
  refusal inside `process_signature()`, and that function's common exit tail asserts that any
  failure leaving it carries an `os_reason`. The refusal cannot be quiet there: the same
  reason is what produces the crash report, so dropping it trips the assert.
- **0374** (`xnu/patches/0374-j7-refuse-spawn-before-it-is-built.patch`, commit `8f82f3b` on
  `main`) refuses the listed names at the top of `posix_spawn()`, matched against the path
  launchd passes (basename, exact, from `j7nospawn=`), returning `ENOENT` before `__spawn_data`,
  before the task and before any reason exists. The old exec-time branch is restored to
  `EPERM` + `EXEC_EXIT_REASON_SECURITY_POLICY` as a backstop.
- **Build:** kernel run `44017738607` success, no `037x` skipped, `refused spawn` string present
  (kernel 17,689,896 bytes). Boot run `44018785107` success.
- Image `artifacts/j7-boot-44018785107/j7-xnu-boot.img`, 17,162,256 bytes,
  sha256 `ab23b11459b76a45c29bd4985722952e431abb512f22db8da7b6ca9e17af147f`. Not yet flashed
  when this entry was written: the panic is fixed by inspection and by the restored assert
  contract, not yet by device evidence.
# 2026-11-17 — J7: the 5.3 s frame was the job trace, and why no wallpaper

- **Device result (0374 image, sha256 `ab23b114…af147f`):** no panic, boot completes.
  `A5XELTE CLK: G3D 900 MHz -> M=900 P=13 S=1 CON0 a1570d01 -> a3840d01` and
  `G3D reads back … 900000 kHz` — the GPU PLL fix works. `j7nospawn: refused spawn
  xpcproxy (ENOENT)` (the caller is xpcproxy; the refused *path* is bluetoothd/fairplayd.H2)
  and the corpse spam is gone.
- **Still 5.3 s per frame** (`frames=1 elapsed_ms=5332`), with `submits` 20–58 per present and
  `avg_copy_us` 33 ms: the copy is not the cost. The console carries `J7 G3D client: t: …`
  lines — `tools/sweetgl-mali/sweet_trace.c` sets `PAN_MESA_DEBUG=trace`, so panfrost's
  pandecode decodes **every job chain, for the life of the process**, and each line is pushed
  through the kernel log. `sweet_trace_start()` honours `SWEET_TRACE=0`, and `sweetenv=`
  accepts several `VAR=VAL` per name, so the boot cmdline now carries
  `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib,SWEET_TRACE=0`.
  No kernel change; expect the boot log to lose the trace and `SWEET ENV: backboardd += SWEET_TRACE=0`
  to appear.
- **Wallpaper:** the picture loads (`no wallpaper at /var/mobile/j7wall.raw, using the built-in
  picture` — 25 % of the 375x667 RGB565 image is black letterbox, mean RGB 105/96/74, so the
  data is sound). `scaledCopy()` substitutes background pixels with it, but the test was an
  exact `v == 0xff000000u`: iOS paints a near-black, so nothing was ever replaced. 0375 accepts
  any opaque colour with `max(r,g,b) <= 0x14` and a run of 32+ pixels, and logs
  `A5XELTE FB: wall 750x1334 px0=… mid=… took N background pixels last frame` six times.
- Image `artifacts/j7-boot-44033362935/j7-xnu-boot.img`, 17,164,304 bytes,
  sha256 `b321d008992783669e247da315d53b6d05b140472520380a867927fdf20b1506`. Not yet flashed.
# 2026-11-17 — J7 speed: the 33 ms was a whole-panel dcache clean

- Trace off worked: fps went from 0.1–0.3 to a sustained 5–10 (`frames=80 elapsed_ms=8033
  fps=9.9`), so the pandecode decode was most of the old 5.3 s frame.
- `avg_copy_us` stayed pinned at 33 ms while `build_us` was 1–2 ms per frame and
  `store_us` 0.07 ms, i.e. the time was in neither the row build nor the stores. It was the
  single `flush_dcache64` over the whole scanout at the end of `scaledCopy()`: 8.29 MB per
  present regardless of content. 0376 flushes each 4-row band (17 KB, 64-byte aligned) as it
  is stored and drops the whole-panel clean; a skipped row is already clean in memory.
- Image `artifacts/j7-boot-44075613366/j7-xnu-boot.img`, 17,164,304 bytes,
  sha256 `97d2b17ed3a255b46d17f50e07c8eb48e6a1481ee2fefb8681b9dd76a04ccaeb`. Not yet flashed.
# 2026-11-18 — J7: the CPU at 1.6 GHz, a background found by its own colour

- **Device result (0376 image, sha256 `97d2b17e…4ccaeb`):** still 10 fps and still no wallpaper.
  Every perf line reads `cpu_mhz=900`, and `A5XELTE FB: wall … took 0 background pixels last
  frame` — the near-black test replaced nothing, so the fixed colour is the wrong idea.
- **CPU clock (0377).** The vendor pwrcal source for this SoC (S5E7870) documents the cluster
  blocks: CMU_CPUCL0 0x10900000, CMU_CPUCL1 0x10800000, PLL_CON0 +0x100 (M 16:25, P 8:13,
  S 0:2, enable 31, locked 29), ARMCLK mux +0x208 bit 12 with its status at +0x608, dividers
  +0x400/+0x404, and in CMU_MIF the switch gate +0x858/+0x85c and its divider +0x458/+0x45c.
  The DVFS table puts 1586 MHz at the same 900 mV as the 902 MHz the bootloader leaves, so no
  PMIC write is needed. ARMCLK moves to the 800 MHz switch input, the dividers go to /1, the
  PLL is written (M=244 P=4 S=0) and polled for lock, then ARMCLK comes back. `j7cpu=<kHz>`
  overrides, 0 disables; every wait is bounded and a PLL that never locks leaves the cores on
  the switch clock instead of hanging. First device evidence will be `A5XELTE CPU: CPUCL0 …`
  and `cpu_mhz=` in the next perf window.
- **Wallpaper (0378).** The report line named the bug: no pixel was ever classified as
  background, because each screen paints its own dark colour. The layer that covers the display
  is sampled once per present (every 8th pixel), its most common colour bucket is taken when it
  covers a fifth of the samples and is dark, and runs of 32+ pixels near it (or near-black) are
  replaced with the picture. A white app page produces no background colour and is left alone.
- **Gauge (0379).** Nothing answered at 0x3b on any of the six S3C controllers, and the
  charger/FG part is an S2MU005 — the same part family as the PMIC at 0x66 — so the probe now
  asks 0x3b, 0x66 and 0x1b and keeps the pair that ACKs. RSOC is read in whichever of the two
  vendor layouts the value fits (hundredths of a percent, or 1/16384 of full) instead of
  assuming the first, which is why the percentage never matched.
- Image `artifacts/j7-boot-44238348399/j7-xnu-boot.img`, 17,164,304 bytes,
  sha256 `7aac66ad8a9a44f41f1b1264396d704f632048d03ce072ae016364bd14778bf2`. Not yet flashed.

## 2026-11-18 — the clock never ran, and the panel is now double buffered

The 2026-11-18 boot log of the 0377/0378/0379 image says, one line after the DECON dump:

    A5XELTE CPU: 1586 kHz is outside the PLL range

0377 handed the MHz figure to a function that takes kHz, the multiplier came out as 0, and the
whole clock change returned early. `j7m_cpu_mhz()` still measured 900 in every perf window, which
is the bootloader's 902 MHz operating point. 0380 takes a small `j7cpu` value as MHz, defaults to
1794 MHz (`cl0_max_support_idx`, the highest step the vendor table allows), falls back to 1586 MHz
if that PLL will not lock, and reads the rate back out of the programmed PLL and dividers.

The same log has `avg_copy_us=35000` on 8.29 MB frames: 15 cycles per 4-byte store, which is store
latency on the write-combining panel map, not bandwidth. Every one of those stores also landed
under the live scanout — that is the tearing. 0381 allocates two contiguous scanout buffers in
ordinary cacheable memory, paints the one the panel is not scanning, cleans it once, and points
DECON window 0 (0x14830000 + 0x880, base 0x67000000, WHOLE 1080x1920 from the dump) at the
finished one. `j7fb_flip=0` keeps the loader's single buffer. The row-shadow skip is disabled
while flipping because the back buffer is two frames old.

0382: `wall ... took 0 background pixels` on every frame. The near-black test allowed 0x14; the
home screen paints 0xff1c1c1e. Up to 0x28 now, the transparent path counts its pixels too, and
the log carries the dominant colour.

0383: every gauge probe logged `stat=0xf0 con=0xe3` — exactly the bytes written to IICSTAT/IICCON,
read back. Those addresses (0x13860000 and up) are the Exynos high-speed I2C, not the S3C2440
controller the touch driver answers on, and the S2MPU05/S2MU005 pair sits on it at 0x66/0x3b. The
patch adds the high-speed sequence and scans 0x13860000-0x138c0000 as well.

Expected in the next log: `A5XELTE CPU: CPUCL0 ... -> 1794000 kHz` (or 1586000), `cpu_mhz=1794`
in the perf windows, `A5XELTE FB: double buffer on, scanout 0x... and 0x...`, a copy time far
below 35 ms, and `A5XELTE FG: gauge on 0x... (hsi2c) at 0x3b`.

## 2026-11-18 — the 38 MHz boot: the clock write and the unlatched flip

The device booted the 0380–0383 image and reported a black panel and 900 MHz. The log says
something worse than that, and it explains both complaints:

    A5XELTE FB: submits=2 ... cpu_mhz=38
    A5XELTE FB: frames=1 elapsed_ms=8175 fps=0.1 avg_copy_us=2369996
    A5XELTE FB: copy split build_us=2212965 store_us=63823

`cpu_mhz` is measured by `j7m_cpu_mhz()` (a timed add loop), not read from a table, so the machine
really was running at 38 MHz: a present took 2.4 s (10 s in the first window) and a touch waited
half a minute. 0380 never reprogrammed the PLL — the log has `CPUCL0 did not leave the PLL, left
along` and the PLL_CON0 readback is still `a15b0501` (M=347 P=5 S=1, 902 MHz) — but it had already
written the switch gate and divider at CMU_MIF +0x858/+0x458, which are not where this tree
believed. 0384 removes every write: `j7_cpu_clock()` reads PLL_CON0 and the ARMCLK dividers,
logs the rate, and leaves the tree as the bootloader set it. 902 MHz is slow; 38 MHz is unusable.

The black panel is the other half:

    A5XELTE FB: double buffer on, scanout 0x87c58000 and 0x885a8000, ADD0 now 87c58000
    A5XELTE DECON: OSDA=00000000 OSDB=0086e77f ADD0=87c58000 UPDATE=00000001 TRIGCON=00000040

ADD0 holds the new address and UPDATE still reads 1 a second later, so the update request was
never consumed: window 0 is protected by SHADOWCON bit 8 (0x701 in the same dump) and the base
stayed in its shadow register. The controller kept scanning 0x67000000, which the `a5fb_noconsole`
mute had already cleared — a black panel that no wake can change. The verified loader
(`shim/a5xelte/display_console.c`) clears SHADOWCON bit 8, writes FB_START, then requests the
update; 0385 does exactly that in `j7_decon_scanout()`, used by the first scanout and every flip,
and copies the first finished frame to the loader's buffer once so a controller that still refuses
the base shows a frame instead of black.

0386: `wall … took 0 background pixels` was never about the colour test. Both wallpaper paths
required the swap layer to be exactly the picture's size (750x1334); a layer of another size
matched neither path, and the frame's dominant colour being black proves the background was there.
The picture is now sampled at the layer's own pixel position.

Expected in the next log: `cpu_mhz=900` again, `avg_copy_us` back near 35 ms, `double buffer on,
scanout … SHADOWCON 00000601` (bit 8 clear), and a non-zero `took N background pixels`.

## 2026-11-19 — 9 fps, no tearing, and a wallpaper that still would not paint

The 0384–0386 boot worked: the panel is up, tearing is gone, and the clock is where the
bootloader left it.

    A5XELTE FB: frames=147 elapsed_ms=15962 fps=9.3 avg_copy_us=71090 max_copy_us=71792
    A5XELTE FB: copy split build_us=9866386 store_us=268052      (147 frames)
    A5XELTE FB: wall 750x1334 px0=ff000000 mid=ff4a4939 bg=ff000000 took 0 background pixels

`cpu_mhz=891` (measured, not a table), so the machine is back at the bootloader's 902 MHz and the
copy is back to 71 ms. Of that, `build_us` is 65 ms per frame — 45 ns per source pixel, which is
one uncached read of the client's surface each — and `store_us` is 1.8 ms. 0387 stages each source
row in cacheable memory once and builds every output pixel from that copy.

The wallpaper: `bg=ff000000` (the frame's dominant colour is black) and still `took 0`. The paint
required a run of 32 consecutive output pixels of the same colour, but at the panel's 1.44x scale
one source pixel covers one or two output pixels, so no run is ever longer than two and that test
could never match. The only pixels the picture ever replaced were the 4320 fully transparent ones
(`took 4320`), which is the transparency path, not the background path. 0387 paints every dark
opaque pixel with the picture, sampled through a column table built once per present.

The gauge still answered on nothing:

    A5XELTE FG: nothing on 0x13830000, stat=0xf0 con=0xe3            (S3C layout, unclocked)
    A5XELTE FG: nothing on 0x13860000 (hsi2c), istat=0 ctl=0x9      (same for 0x1387-0x1389)
    A5XELTE FG: nothing on 0x138a0000 (hsi2c), istat=0x7fffffff     (nothing mapped there)

`istat=0` on eight high-speed buses and `0x7fffffff` on the blocks that are not there is a block
that is clocked and reached, and 0383 was reading and writing the wrong words of it: the Exynos
HSI2C has `I2C_INT_ENABLE` at 0x20 and `I2C_INT_STATUS` at 0x24 (the map in
drivers/i2c/busses/i2c-exynos5.c), not 0x10/0x14, and the FIFOs at 0x34/0x38, not 0x1c/0x20.
0388 drives the block in its auto mode — `I2C_CTL` for master and direction, `I2C_ADDR` for the
target, `I2C_AUTO_CONF` for length/read/stop/run, `I2C_INT_STATUS` for the result — and leaves the
timing registers alone because the bootloader already read this gauge.

Expected in the next log: `build_us` near 10 ms per 147 frames and `fps` 20–30, a `wall … took N
background pixels` with N in the hundreds of thousands, and either
`A5XELTE FG: gauge on 0x... (hsi2c) at 0x3b, RSOC=...` or a `hsi2c read of 2 byte(s) failed` line
carrying the real `stat`, `ts` and `conf`.

## 2026-11-19 — the wallpaper paints, and where the 70 ms actually goes

Image `44518516504` (patches 0384–0388) on the phone. The wallpaper is on screen — the user's
photo of the phone shows the built-in picture behind the Apple logo — and the log says so:

    A5XELTE FB: wall 750x1334 px0=ff000000 mid=ff4a4939 bg=ff000000 took 1422150 background pixels last frame
    A5XELTE FB: copy split build_us=70045 store_us=1925
    A5XELTE FB: frames=1 elapsed_ms=5385 fps=0.1 bytes=8294400 avg_copy_us=78487 max_copy_us=78487

So `sweet_fb_dominant()` and the per-dark-pixel paint both work. Three things follow from the
numbers, and the user reports all three: the wallpaper is a little too early, the frame is still
70 ms of build, and the gauge still answers on nothing.

**The build is not the paint.** `build_us=70045` for a 78 ms present, with 1.4 M pixels painted in
it, is 47 µs per source row (1334 rows). Painting a row is 750 table lookups; reading it is
3000 bytes out of the client's surface, which is cache-inhibited in this kernel, so the 0387
`memcpy` into cacheable memory is itself a 32-bit load per bus transaction — the same cost as the
gather it replaced, which is why build_us barely moved from 65 ms. 0389 loads the row two pixels at
a time, four loads in flight, and does the picture decision once per *source* pixel on the staged
copy instead of once per output pixel. The copy split line gains `src_us` so the read and the
paint are separated in the next log.

**Too early.** The picture is painted on the boot logo screen, so the Apple sits on the wallpaper
instead of on black. There is no userspace hook here, but the kernel sees the right moment itself:
`kern_exec.c` already prints `exec SpringBoard`, and SpringBoard's spawn is the last step before
the lock screen. 0390 sets `sweet_fb_wall_ready` there, and `scaledCopy()` paints nothing until it
is set.

**The gauge never got its 100 µs.** 0388's completion poll gave up after 100 µs. A two-byte read at
100 kHz is about 300 µs, and `drivers/i2c/busses/i2c-exynos5.c` waits 100 ms, so every transfer was
abandoned while it was still running, which is exactly `istat=0` on all eight high-speed buses.
0391 waits 5 ms, pushes the command bytes into the transmit FIFO *before* the run bit (an empty
FIFO at start reads as an underrun), clears `I2C_AUTO_CONF` after a failed message, and keeps the
scan's addresses in 7-bit form — `I2C_ADDR` takes bits [19:10] (`HSI2C_SLV_ADDR_MAS`) and `IICDS`
takes `addr << 1`, and 0388 wrote the same byte-form value into both, which is a different device
on the S3C controller and only landed in the right HSI2C field by accident for 0x76.

Expected in the next log: `src_us` and `build_us` together well under the 70 ms, `fps` 20–30 on a
frame that changes, `wall … took N` only *after* `exec SpringBoard`, and either
`A5XELTE FG: gauge on 0x... (hsi2c) at 0x3b, RSOC=...` or
`nothing on 0x... (hsi2c), istat=… ts=… ctl=…` with a `ts` value that says whether the bus ACKed.

## 2026-11-20 — the row read is fixed, the picture is gone, and why every bus said 0x2000c

Device log for image `44577466397` (17,180,688 bytes, sha256
`3f67dcab730229296d116085c1c6841dad4a887b78ae333e27c853a4ed6e5877`). The user reports it as
faster and more responsive, still not finished, and compares it to a 3GS on iOS 9.

**0389 worked.** `copy split build_us=64635 src_us=2968 store_us=1898` for 8 frames is 0.37 ms of
row read per frame instead of the 47 µs per row (≈65 ms) of 0387. The uncached gather is gone; the
`src_us` share of `build_us` is now under 1 %.

**The present is still 55–72 ms.** `avg_copy_us` in the interaction windows is 55–73 ms
(`fps=9.3` in the 16 s window with 600 presents) and in the idle windows `fps` is 0.1–2 because
almost nothing presents. `build_us` over those windows is 60 % of wall time (≈16 ms per frame), so
the remaining 40 ms is not in `scaledCopy`'s two measured sections. That is the next thing to
measure, not to guess at.

**The picture had holes because it is not the desktop's wallpaper.** It is one half-size RGB565
photograph painted where the client painted background, so every dark part of the photograph stayed
greyed and the tile showed black blocks. 0392 removes it from the kernel entirely: no
`j7wall_data.h`, no embedded expansion. A raw file at `/var/mobile/j7wall.raw` (or `j7wall=<path>`)
is still honoured, and that path now opens the file before allocating and frees the buffer when the
read fails.

**"Add New Wallpaper" is empty because the gallery is not in the kernel.** The gallery tiles are
PosterBoard's poster extensions and the app-provided poster plug-ins, both under `/Applications`,
and this root was staged with `/Applications/***` excluded and then narrowed to six bundles
(`docs/a5xelte-root-application-omission.md`). Evidence in this boot: PosterBoard runs
(`exec PosterBoard`, and its `sel 17` calls answer), while `PaperBoard` — the wallpaper gallery
service, `paperboard.wallpaper-service` — never appears in the log at all, not as an exec and not in
a launchd message. That is a staged-root gap, not something a patch can add: the bundles are Apple
payload and stay out of Git.

**Every high-speed bus logged the same `ts=0x2000c`, and that is a leftover.** Bit 17 of
`I2C_TRANS_STATUS` is the stop phase. 0388/0391 never waited for a stop to finish and never reset
the block, so the first failed message left the controller stopping and every bus probed after it
reported the same inherited word — which is why three unrelated blocks produced one identical value.
0393 drops the hand-rolled sequence for the one that is device verified on this IP in the A5XELTE
PMIC path (`bsd/dev/a5xelte_touch.c`): reset through `I2C_CTL` bit 31, Linux's 400 kHz timing
registers, the byte count in the FIFO trigger the controller expects, both FIFOs serviced while the
transfer runs, `INT_STATUS` polled and cleared bit by bit, success only after `TRANS_STATUS` bit 17
clears, and a reset after a failed message. The scan asks 0x36 (MAX77854, as the S7 uses next to
its 0x66 PMIC) instead of 0x0d, which is not a fuel gauge on any Samsung board.

Expected in the next log: a `wall` report line only when a picture file exists (there is none, so
the desktop keeps its graded background and no photograph at all), and either
`A5XELTE FG: gauge on 0x... (hsi2c) at 0x3b, RSOC=...` or `hsi2c read of 2 byte(s) failed
irq=… ts=… fifo=…` with an `irq` from the transfer itself instead of an inherited `0x2000c`.

## 2026-11-20 — restoring the wallpaper gallery without a reflash

**Why the gallery is empty is answered; getting the bundles in is the new step.** The tiles
behind Settings > Wallpaper > Add New Wallpaper come from `PaperBoard`
(`paperboard.wallpaper-service`) and `PosterBoard`'s poster extensions, and `PaperBoard` is a
`/Applications` bundle this root was staged without. That is Apple payload, not a kernel
behaviour, so no patch can add it and it cannot live in Git.

**`tools/j7-root-extras.py` does it in place.** TWRP's kernel has no hfsplus driver and iOS has
no shell, so the HFSX volume is edited on the PC, not on the phone:

- `stage` takes the bundles out of the IPSW (`--ipsw`, mounted with apfs-fuse; `--os` for an
  already-mounted OS image; `--tar` for the release asset) and writes
  `device/j7-root-extras.tar` — gzip-free tar, numeric owners from the image, `device/` is
  git-ignored so nothing Apple reaches the repository. Default set is the same six bundles
  `tools/a5xelte-stage-ios-root.sh` stages, plus any `*Poster*` bundle under
  `System/Library/ExtensionKit/Extensions`; `--all-apps` takes all of `/Applications`.
- `push` (phone in TWRP, no Data mounted) pulls USERDATA once to
  `device/USERDATA-before.img`, copies it, `fsck.hfsplus -fy`, loop-mounts it read-write,
  prints what `/Applications` already has, extracts only the missing bundles with
  `tar --numeric-owner`, checks each executable is root:wheel and executable, unmounts, repairs,
  then writes only the 4 KiB runs that differ with `dd conv=fsync` — each run read back and
  compared by SHA-256 — and records them in `device/USERDATA-changed.txt`.
- `restore` writes those same runs back from the pre-change image, with the same comparison.

Neither path touches BOOT: the kernel, ADT and boot arguments are unchanged, and because
USERDATA keeps its existing contents there is no data migration and no hour-long first boot.
The changed delta is normally a few hundred KiB of HFS metadata plus ~1.5 MiB of bundle data;
`--max-changed-mib 512` refuses anything that looks like a wrong image instead of a bundle
install. Resource forks are not carried by tar (the six audited bundles have none).

`.github/workflows/j7-root-extras.yml` packs the same tar straight out of the IPSW on a macOS
runner and attaches it to a private release, so the laptop does not need a 6 GB IPSW download.

Not device-verified: no bundle has been pushed to the phone yet, so "the gallery lists its
posters" is the expected result of this step, not an observation. The evidence to collect is
the next boot log: `PaperBoard` must appear as an `exec` where it has never appeared before.

## 2026-11-20 — the CI packer's mount path, and where widgets come from

**The first `j7-root-extras` run (4468486141) failed on a path that does not exist on macOS.**
`hdiutil attach` mounts the OS image's system volume *at the mount point itself*, so
`Applications/` is `/Volumes/SydneyUpdate20H392.D21D211OS/Applications`. Only `apfs-fuse`, the
laptop's route, presents that same tree under `root/`, which is why `tools/j7-root-extras.py`
probes both. The workflow now does the same, prints the tree it chose, and fails with a listing of
the mount instead of a bare "no /Applications" if neither has it. That check failing was not a
missing-bundle finding.

**Widgets are the same kind of gap.** A widget is a `widgetkit-extension` appex embedded in its
app's `PlugIns` folder (or under `System/Library/ExtensionKit/Extensions`) that `chronod` hosts and
renders once the app is installed. A root staged down to six `/Applications` bundles has no widget
extensions for Calendar, Clock, Weather and so on, and what is left on the screen is the empty
black/grey host tile. `j7-root-extras` therefore packs all of `/Applications` by default now
(~215 MiB, the same in-place push, no reflash): the gallery bundles and the widget extensions
travel together. `all_apps=0` still selects the six display bundles alone.

Not device-verified: no bundle has been pushed yet. The distinguishing evidence in the next boot
log is `PaperBoard` as an `exec`, and `chronod` starting a `widgetkit-extension` process when a
widget is added. If a widget is still grey with its extension present, the remaining problem is in
the render path, not in the bundle set.

## 2026-11-20 — the extras tool on Windows

`tools/j7-root-extras.py` crashed on Windows before doing anything: `os.geteuid()` does not exist
there, so the module-level `SUDO` line raised `AttributeError`. The crash was the first thing the
user hit, not a tar problem.

Only `stage --tar` can work on Windows (it copies and owner-checks the tar; no mounting). `push`
and `restore` loop-mount the HFS+ USERDATA image read-write, and `stage --os/--ipsw` mounts an APFS
image, so all three are Linux-only. WSL2 is not a workaround: Microsoft's kernel is built without
`CONFIG_HFSPLUS_FS`, and adb over USB needs `usbipd-win` on top. The tool now computes `SUDO`
without `geteuid` and these three steps exit with one sentence saying so and pointing at the Linux
machine, instead of a traceback. Verified by loading the module with `os.name = 'nt'` and calling
each mode: `stage --tar` runs, the other three exit cleanly. Nothing was pushed to the phone.

### macOS as the push host

macOS can write HFS+ natively and macOS 26 still supports it, so the tool grew a second mount path:
`hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -readwrite` gives the image a
`/dev/diskN` node, `fsck_hfs -fy` runs on that node (advisory; the phone never unmounts cleanly) and
`mount -t hfs` mounts it read-write, `hdiutil detach` releases it. Registers as a check that the
volume is actually writable before anything is touched — a dirty HFS+ volume can come up read-only,
and macOS 26.4 betas had a `fsck_hfs`/auto-mount bug with exactly that symptom.

Two space changes make 30 GB of free space enough: the pull skips all-zero 4 KiB blocks, so the
14-16 GiB partition lands as roughly the volume's used size, and the working copy is an APFS clone
(`cp -c`) instead of a second full file. Ownership is re-applied with `chown -R 0:0`
after extraction because bsdtar does not promise numeric owners, and the executable check uses
`os.stat` instead of GNU `stat -c`, which macOS lacks.

Verified only on Linux: a fake adb stream (1 MiB data, 2 MiB zeros, 1 MiB data) produced a 4 MiB
sparse image with 2 MiB holes and a change run at the expected block, and the Darwin code path with
`run()` stubbed produced hdiutil attach, fsck_hfs on /dev/disk9, mount -t hfs, umount and hdiutil
detach in that order. No mounting, pulling or writing was done on a Mac or on the phone.

The hackintosh reached this step first, and hit a host problem rather than a device one: adb 37.0.1
on Darwin 25.6 stops the daemon right after `usb_osx.cpp:323 reported max packet size for …`, so
`adb devices` prints `ADB server didn't ACK` / `failed to start daemon` and nothing can be pushed.
That is the legacy USB backend 37.x brought back for macOS; `ADB_LIBUSB=1` selects libusb instead,
and platform-tools 35.0.1 is libusb by default. The tool now prints that hint when `adb devices`
returns an error, and `docs/j7-root-extras-push.md` has an "ADB server didn't ACK on macOS" section.
Nothing was run against the phone or an HFS+ image on the Mac yet.

### macOS reached the phone, and the J7's USERDATA does not end on a block boundary

adb 37.0.1 on that Mac keeps the daemon down in its legacy USB backend until `ADB_LIBUSB=1` is on the
command that starts the server; with libusb the J7 appears as `52103bedfa51642f  recovery` and every
later step works. The first real device fact from it: `blockdev --getsize64` reports USERDATA as
**23,531,077,120 bytes**, i.e. 4096 x 5,744,891 plus 3,584 — a 32 GB handset, and a partition that is
not a whole number of 4 KiB blocks, which the tool refused outright.

That refusal was wrong: the alternate HFS+ volume header sits in the last 1024 bytes of the volume, so
the short last block has to travel too. The pull, the 4 KiB change comparison and the write now all
carry a short final block at its real length (`ceil` blocks, `min(n*BLOCK, size - off*BLOCK)`, read-back
truncated to the written length), and the by-name lookup walks every `/dev/block/*/by-name/{USERDATA,
userdata}` layout before falling back to `mmcblk0p25`. A signature check reads the first 8 KiB for
`H+`/`HX` before the pull, so a wrong `--dev` costs 8 KiB instead of 22 GiB.

Verified with a fake adb and a 3 MiB + 3,584-byte "partition": the pull keeps the exact size and the
tail bytes, stays sparse over 1 MiB of zeros, the comparison finds the edited middle block and the short
last block as separate runs, and the write reproduces the edited image byte for byte on a fake device —
while a corrupted write is still caught by the read-back. Not verified: no real HFS+ volume was mounted
and nothing has been written to the phone yet.

### "already on the phone" was a bundle-name check, and widgets need files inside those bundles

The first successful dry run against the real J7 reported **174 bundles already in /Applications** and
then `nothing to install` — because `install()` tested only whether `Applications/X.app` existed. A
phone that was pushed once with an app-only tar has every bundle *by name* and still no
`PlugIns/*.appex`, which is exactly the state that draws empty grey widget tiles, so that run would
have written nothing at all.

`install()` now builds the tar's member list per install unit and compares it against the image file by
file: a present-but-incomplete unit is reported as `incomplete, N file(s) missing` with a sample, and
only those members are extracted (`tar -T`, names reversed so a directory follows its own children —
GNU tar recurses into a named directory and then calls the later child names "not found in archive").
A tar carrying zero `.appex` now says so out loud, because such a tar cannot supply a widget extension
however many app bundles it has.

Verified with a synthetic phone tree plus tar: an app present without its `PlugIns/W.appex` is reported
incomplete, the appex and its executable land with mode 755 while the existing executable is untouched,
a second run reports every unit complete, `--replace` still re-extracts whole units, and the owner check
still refuses an executable that is not root-owned. Not verified: the real tar's contents, which is the
next thing to look at.

### Every unit in the tar is already complete on the phone

`tar -tf device/j7-root-extras.tar | grep -c '\.appex'` gives 3782 lines, i.e. 130 `.appex` bundles
inside 140 install units — the tar does carry the extensions. The member-by-member dry run against the
real image then reported **all 140 units complete**: the phone already holds every file this tar has,
extensions included. So the grey tiles are not a missing-bundle problem on this phone, and no push
will change them; the next question is the render path or resources outside `/Applications`.

Added `inspect` so the pulled image can be read without a pull or a write: it attaches
`device/USERDATA-before.img` read-only (no fsck, `-o rdonly`, `hdiutil -readonly`), walks one path and
prints sizes, optionally filtered by `--glob`. Verified against a fake mount: read-only flag reaches
`attach_image`, listing and globbing are correct, and a missing image exits instead of mounting.
Not verified: a real HFS+ mount of the image (no macOS here).

### macOS will not mount a dirty HFS+ journal read-only, so `inspect` repairs a clone

The first `inspect` runs on the hackintosh failed before listing anything:

```
command failed: sudo mount -t hfs -o rdonly /dev/disk6 /var/folders/.../j7look-et221l1c
mount_hfs: Invalid argument
mount: /private/var/.../j7look-et221l1c failed with 1
```

`hdiutil -readonly` attached the image fine; the mount is what fails. That is the journal, not the
image: a USERDATA pulled from TWRP was never unmounted, so the HFS+ journal is dirty, and macOS
refuses to mount a dirty journal read-only at all (`hfs: early journal init: volume on disk6 is
read-only and journal is dirty. Can not mount volume.` / `hfs_mountfs: ... failed, erroring out` /
`hfs_mount: hfs_mountfs returned error=22`), which the mount_hfs wrapper prints as EINVAL. The
read-write path never hit it because it runs `fsck_hfs -fy` first.

`attach_image` now returns `(handle, clone)`: it tries the read-only mount, and when that is refused
it copies the image to `<image>.look` (an APFS clone: no extra space), runs `fsck_hfs -fy` on the
copy, attaches that and mounts it, and `inspect` deletes the copy when it exits. The pulled image is
never repaired, because it is the baseline the next `push` diffs against the device. Verified with
stubbed `run`/`copy_image` on Darwin: the failing read-only mount, the detach, the clone, the fsck,
the second attach and the final mount all happen in that order, and the clone is removed; the
read-write path and the Linux path are unchanged. Not verified: a real dirty HFS+ volume on macOS (no
Mac here) — that run is the user's next step.

## 2026-11-21 — the purple is our own gradient, and ExtensionKit is not on the root at all

Two answers came out of the same evening: one from the boot log, one from the phone image.

**The log says where the 50 ms goes.** In the interaction windows of the 2026-11-20 image the
present is still 44–62 ms and the row build is nearly all of it:

    A5XELTE FB: frames=2 elapsed_ms=5412 fps=0.3 bytes=16588800 avg_copy_us=61933 max_copy_us=65934
    A5XELTE FB: copy split build_us=100134 src_us=6279 store_us=3662      (2 frames)
    A5XELTE FB: frames=9 elapsed_ms=11156 fps=0.8 avg_copy_us=44558
    A5XELTE FB: copy split build_us=341784 src_us=22100 store_us=12844      (9 frames)

So of a 58 ms present, 50 ms is `build_us`, 3 ms is the source read (0389's two-pixel row copy,
working) and 2 ms is the panel store. The build is still two passes over every pixel: 0389 moved the
picture decision to one per *source* pixel but left the run-length expansion below it, which scans
the output row for equal neighbours before writing it. 1334 rows × (750 decisions + 1080 run scans)
at `cpu_mhz=908` is the 50 ms.

**The purple is this driver.** With no picture file, every wide run of near-black pixels was
replaced by `bg`, a vertical gradient built from the row index:

    const uint32_t bg = 0xff000000u | ((12u + (ramp * 40u >> 8)) << 16) |
        ((16u + (ramp * 26u >> 8)) << 8) | (40u + (ramp * 70u >> 8));

12/16/40 at the top of the panel and 52/42/110 at the bottom: blue with a red lift, i.e. purple,
painted exactly where the desktop draws its dark background. The picture that gradient existed for
was removed in 0392, so on this image it replaced the desktop's own near-black for nothing.

**Patch 0394 removes both.** The gradient, its ramp and the whole per-source-pixel pass that looked
for dark runs are gone: with no picture file the client's row is handed to the panel unchanged, so
the desktop's black arrives as black. The expansion is a flat `row[x] = in[column[x]]` over the
output row instead of a run scan, and the shadow row is only copied when the unchanged-row skip can
actually read it (`fFlip` is on, so it never did — 4 MB of cached stores per frame were dead).
Expected in the next log: `copy split build_us` well under the current 50 ms per frame and `fps` up
from the user's 7–9 in an interaction window.

**The gallery and the widgets have one missing tree.** `tools/j7-root-extras.py inspect` on
`device/USERDATA-before.img` now works on the hackintosh (the dirty-journal clone repair from
`aba1e89`), and its two answers were:

    System/Library/ExtensionKit/Extensions is not on this image
    d 1564 Applications/PaperBoard.app/PlugIns/LegacyPoster.appex

The root was staged without `System/Library/ExtensionKit`, so the phone has no ExtensionKit
framework and no extension directory at all. That is one cause for both remaining symptoms: a
`widgetkit-extension` is started by ExtensionKit, and the wallpaper gallery's poster extensions are
ExtensionKit extensions, so `chronod` has nothing to host (grey widget tiles) and `PaperBoard` has
nothing to list (empty gallery) even though its app bundle is on the volume.

`stage` therefore packs `System/Library/ExtensionKit/Extensions` and
`System/Library/Frameworks/ExtensionKit.framework` out of the OS image with the app bundles, and
`install_unit()` no longer treats a bare directory as an install unit (a `System/Library` unit would
have been reported incomplete on every run). `j7-root-extras.yml` packs the same two trees.
Not verified: nothing has been pushed since, so "PaperBoard execs and the gallery lists posters" is
still the expected result of the push, not an observation. `docs/j7-root-extras-push.md` has the
three commands.

## 2026-11-21 — the 38 MHz boot explained: OSCCLK, not the switch

The 0380 boot that came up at 38 MHz is now fully explained by Samsung's own clock source, and the
CPU can be raised properly. The vendor kernel (`drivers/soc/samsung/pwrcal/S5E7870`) has:

    CLK_MUX(CPUCL0_MUX_CLK_CPUCL0, cpucl0_mux_clk_cpucl0_p, CLK_CON_MUX_CLK_CPUCL0, 12, 1, ...)
    cpucl0_mux_clk_cpucl0_p[] = { CLK(CPUCL0_MUX_CPUCL0_PLL), CLK(CPUCL0_MUX_CLKCMU_CPUCL0_SWITCH_USER) };
    cpucl0_mux_clkcmu_cpucl0_switch_user_p[] = { CLK(OSCCLK), CLK(MIF_GATE_CLKCMU_CPUCL0_SWITCH) };

so +0x208 bit 12 does move ARMCLK to the switch — but only if the switch-source mux at **+0x204
bit 12** has first selected the MIF switch (bit 27 = 0 takes it out of hardware control). 0380 never
wrote +0x204, so the "switch" input was OSCCLK at 26 MHz; that is the 38 MHz.  Two further facts
from `pwrcal-cmu.c` and `pwrcal-dfs.c`:

- `pwrcal_mux_set_src()` waits for `status == (1 << src)`, i.e. **2** when selecting the switch and
  **1** when selecting the PLL. 0380 waited for 1 both times, so it timed out after the move had
  already happened and returned with the PLL never programmed.
- the PLL write is more than M/P/S: `_clk_pll141xx_set_pms()` writes the lock time at +0x000 as
  `P * 150`, clears bit 26 and sets bit 5 of PLL_CON0, and only polls the lock bit 29 when the
  PLL is enabled.

The CPU DFS table in `exynos7870.dtsi` (a6lte, same SoC) carries 1794 MHz at **900 mV**, the same
voltage as the 902 MHz bootloader step, and `cl0_max_support_idx = 2` points at that row — so the
top step needs no PMIC write, and 1794 is the ceiling rather than 1586. `asv` caps a non-fused part
at 1586, so 1586 stays the fallback if 1794 does not lock. The table's divider members are not in
the DFS table (it has switches and a PLL only), which is why ARMCLK is the PLL output and the
+0x400/+0x404 dividers are reported, not written.

`xnu/patches/0395` implements exactly that sequence: gate and source mux, switch divider to its
800 MHz step, ARMCLK onto the switch and status polled for 2, lock time + PLL_CON0 and polled for
29, ARMCLK back and status polled for 1, source mux/divider/gate restored. A cluster whose switch
never reports is untouched, and a PLL that never locks gets its old CON0 back and the cores
returned to it, so the worst case is the 902 MHz the bootloader left. Expected in the next log:
`A5XELTE CPU: CPUCL0 ... asked for 1794000` followed by `... -> 1794000 kHz`, and `cpu_mhz=1794` in
the perf windows; the operating point is not verified until a boot log reports it.

Image `artifacts/j7-boot-44879118860/j7-xnu-boot.img`, 16,674,832 bytes,
sha256 `a29ced6edf2eec2dcb6e342d755a9ecf081f3b310e66f10d815cf142cd1b5f8f` (xnu run 44878722876,
0395 built and the image carries its log strings). Not yet flashed.

## 2026-11-21 — the 1794 MHz image stopped before the logo: the switch is not a clock

The 0395 image (`a29ced6e…5b8f`) showed no Apple logo and produced no new log: the log the user
pulled was the previous boot's. That is what a stopped CPU clock looks like from the outside. The
raise runs in `AppleCLCD::start`, before the logo is handed to the panel and before the kernel
flushes its ring to storage, so a core that stops inside `j7_cpu_clock()` writes nothing anywhere
and leaves the old log in place.

**Why 0395 stopped.** It switched the cores to the MIF switch *before* touching the PLL and wrote
CMU_MIF `+0x858`/`+0x458`. The status it polled at `+0x608` bits 12:13 is the mux's selection,
`1 << src`, not a clock-present signal: it reads 2 as soon as the mux has selected that input,
whether or not the switch is running. Unlike 0380's failure (cores on OSCCLK at 26 MHz, machine
alive, `cpu_mhz=38`) this one produced nothing at all, which fits the cores being parked on a dead
input rather than on the always-running oscillator.

**The transition that is known to run here is OSCCLK.** 0380 never wrote `+0x204`, so the switch
input's 0 source was OSCCLK, and the boot survived at 26 MHz: it only failed because it waited for
status `1` where the switch reads `2` and returned before programming the PLL. Patch **0396** keeps
exactly that detour and drops all CMU_MIF access: ARMCLK to the switch input (`+0x208` bit 12),
poll `+0x608` for 2, write lock time `P*150` and `PLL_CON0` (M 276 / P 4 / S 0 = 1794 MHz), poll
lock 29, move back and poll 1. The polls use `IODelay`, not `IOSleep`, because at 26 MHz a
scheduling sleep is an eternity; a mux that does not move is put back and a PLL that never locks
gets its old CON0 back with the cores returned to it.

**GPU: the 900 MHz step was never on in these boots.** The GPU PLL only moved when `j7gpuclk=900`
was in the boot args, and the last cmdline did not carry it, so G3D stayed at the bootloader's
343 MHz. The herolte branch reaches its speed with Mesa's Panfrost on the Mali-T880 (its own
`gpu/eagl` + `hakios-gl`, and AIR→NIR for Metal); this port already runs the same Panfrost stack
through `sweetgl.dylib` on the T830, so the GPU clock is the whole of the available GPU-side gain
without writing a Metal driver. Patch **0397** defaults `j7gpuclk` to 900 MHz, the rate the 0374
boot read back, so every boot gets it.

Expected in the next log: `A5XELTE CPU: CPUCL0 … asked for 1794000`, `… -> 1794000 kHz`,
`cpu_mhz=1794`, and `A5XELTE CLK: G3D 900 MHz -> … reads back … 900000 kHz`. If 1794 does not
lock, the same image logs `PLL never locked` and stays at 902; `j7cpu=1586` is the rated step.

Image `artifacts/j7-boot-44902100379/j7-xnu-boot.img`, 16,674,832 bytes, sha256
`439b0baeca049cc42e3c52b2983e6e1757445f73b6980e656fcbc2bbdf7d115c`, sha1
`52e397b27ecdb397fee726b8a6a544e275361c79`, md5 `9aa3b0a9702c405e60e805a70e8f1131`
(xnu run 44901664589 with 0396+0397, boot run 44902100379). The image carries
`did not take the oscillator`, `PLL never locked at` and `G3D reads back`, and no longer carries
0395's `did not move to the switch`; cmdline is the same as the previous boot, so the only change
on the phone is the kernel. Not flashed or booted here.

## 2026-11-21 — the OSCCLK image was silent too, and the 900 MHz GPU step was dead code

The 0396 image (`439b0baec…d115c`) showed the same thing as 0395: no Apple logo, no new log. Two
different ARMCLK transitions, the same silence. That is the answer to "which part of the move is
wrong": it is not the switch versus the oscillator, because both of them stop the machine in
`AppleCLCD::start`, before the panel is handed a frame and before the persistent ring is ever
flushed to storage. 0380's `cpu_mhz=38` boot remains the only measured evidence of an ARMCLK move
on this phone, and it is equally consistent with "the mux never moved and something else slowed
down" as with the cores reaching 26 MHz. Until the vendor ARMCLK mux register is confirmed at
`+0x208` bit 12 - and `+0x608` as its status - every boot that asks for the top step buys a black
screen. Patch **0398** makes `j7cpu` default to 0, so the clock is left at the bootloader's 902 MHz
operating point, which is the state every log so far was recorded in. `j7cpu=1794` is still there
for an explicit test.

**The GPU never reached 900 MHz either, for a plain code reason.** 0397 changed the `j7gpuclk`
default, but the whole CMU_G3D block - the readback, the M/P/S write and the `G3D reads back` line -
was nested inside the `j7clk` gate, and `j7clk` is the CMU_MIF dump, off by default and absent from
the phone's cmdline. So the "900 MHz by default" image still ran the T830 at the bootloader's
343 MHz: the default was dead code. Patch **0399** moves the CMU_G3D block out of the gate so it
runs on every boot, keeps CMU_MIF behind `j7clk`, and leaves the 0374-verified sequence alone
(900 MHz, `j7gpuclk=0` keeps the loader rate, readback logged). The GPU is a separate clock domain,
so this cannot take the CPU down with it the way the ARMCLK move does.

**Checked while looking for a second cause:** the ADT grew from 9,864 to 12,464 bytes at the first
black-screen image, because that is when the private-repository `/product` fetch started producing
Apple's node (the older tree has none). The new tree parses, has no duplicate top-level names, and
re-serialises byte-identically, so it is not malformed; it is `product` with 61 properties and no
children. If this image is silent as well, that node is the next thing to remove - the workflow's
private-only fetch is the switch for it.

Image `artifacts/j7-boot-44906332073/j7-xnu-boot.img`, 16,674,832 bytes, sha256
`e9a44e43e4d73a8e2c1b77616784bee37e4f3da5f18efc9408ef34b2fd3651de`, sha1
`089d50ff5af87408315bcffcd34fa3d54ba568e0`, md5 `ef989823df8192ec6fbe6790dc8ffb78`
(xnu run 44905960778 with 0396+0397+0398+0399, boot run 44906332073, `iphone8=false`). It carries
`G3D reads back` and `did not take the oscillator` (dead code now), and the cmdline is unchanged
from the last boot. Not flashed or booted here.

Expected in the next log: `SWEET FB: AppleCLCD 1080x1920`, `A5XELTE CLK: G3D 900 MHz -> M=900 P=13
S=1` then `G3D reads back … 900000 kHz`, `cpu_mhz=902` (unchanged, by design), and a boot that
reaches userland. `j7gpuclk=0` in the same image reverts the GPU to 343 MHz without a rebuild.

## 2026-11-21 — the same silence again, so the /product node comes out

The 0398+0399 image was silent too: no Apple logo and no new log line. That is three images in a
row (0395, 0396+0397, 0398+0399) that fail in the same place, and the kernel side now has no
clock code that runs by default - `j7cpu` is 0 and the GPU block only touches CMU_G3D, a separate
domain. The one thing all three black-screen images share and none of the working boots had is the
ADT: they carried the 12,464-byte tree with Apple's `/product` node from the private-repo IPSW
fetch, while every boot that reached `sweet-progress` used the 9,864-byte entry-only tree. The node
parses and round-trips, so this is not a proof that it is the cause, but it is the last single
variable that changed between "draws" and "silent".

So the fetch is now opt-in: a `product_node` workflow input, default **off**. With it off the
"Fetch the iPhone device tree" step is skipped entirely, `tools/mkadt_j7prime.py` warns and builds
the entry-only ADT, and nothing from the IPSW enters the image. Turn it back on with
`-f product_node=true` once the phone boots again; the repository's visibility itself is unchanged
(still private, so the old behaviour is one input away).

New image: `artifacts/j7-boot-44911576305/j7-xnu-boot.img`, 16,672,784 bytes, sha256
`05efad31793b63d114aef2028025efd6d9ce1027626525e70b619e0034e9eb50`, sha1
`f3309437710d16707740f9d18e9d8108017676d9`, md5 `2a855201da3f1487c4a09c242b8d0684` (xnu run
44905960778 = 0398+0399, boot run 44911576305, `board=d21ap`, `iphone8=false`,
`product_node=false`; ADT 9,864 bytes, top level chosen/defaults/cpus/arm-io/buttons/
a5xelte-fbcon/a5xelte-emmc, no `product`). The cmdline is byte-identical to the last boot, so this
is the ADT alone. `G3D reads back` is still in the image. Not flashed or booted here.

Expected in the next log if the node was the problem: `SWEET FB: AppleCLCD 1080x1920`, then
`A5XELTE CLK: G3D 900 MHz -> M=900 P=13 S=1` and `G3D reads back … 900000 kHz`, `cpu_mhz=902`,
and userland. If it is silent again the ADT is cleared and the next step is the panel handoff in
`AppleCLCD::start` itself - the one function every failing boot dies inside.

## 2026-11-21 — the /product node is not it: two images, two candidate causes

The entry-only ADT (9,864 bytes, no `product`) booted exactly as silently as the 12,464-byte one,
so the Apple node is ruled out. With the ADT and the cmdline back to the values every working boot
used, the only remaining difference between the last boot that drew and these is the kernel:
patches 0394-0399. Two kernels were built to split that range without another guess:

**A - does the GPU write stop it?** Patch 0399 moved the CMU_G3D block out of the `j7clk` gate, so
the 900 MHz PLL write now runs on every boot inside `AppleCLCD::start`, and it is the only clock
*write* left in that path (the CPU block is skipped at `j7cpu=0`). 0374 read 900 MHz back once, but
this tree never raises BUCK4 with it, so it is a higher clock at the bootloader's GPU voltage. That
is testable with no rebuild: the same kernel with `j7gpuclk=0` in the cmdline skips the write and
only logs the registers.

- `artifacts/j7-boot-44915548052/j7-xnu-boot.img`, 16,672,784 bytes, sha256
  `617fa1c16b851c1bb8298b0106a64de1b4706a01d94b1c833b15ce7223792d11`, sha1
  `5d89758410ea7587f6089b05f43b726cf57738c8`, md5 `a0aca4c5d048fdf7e918de4a43c7f391`
  (xnu run 44905960778 = 0394-0399, boot run 44915548052, `product_node=false`, cmdline as the
  last boot plus `j7gpuclk=0`).

**B - is it any of those patches at all?** The last kernel that drew anything was the 0393 series,
so tag `j7-0393` points at that commit (`ad84bc6`) and it was rebuilt through today's workflow:
`did not take the oscillator` and `did not move to the switch` are absent, `G3D reads back` is
still there from 0374's gated block. Packed with the same ADT and cmdline as the last boot.

- `artifacts/j7-boot-44916011653/j7-xnu-boot.img`, 16,672,784 bytes, sha256
  `86138fe4e13d10831d3e1133e2d2ba3d2e5e284d86e56f4dcb7e6ddfa2151604`, sha1
  `a5626d53e047d4fb7b6116e747dec66860e1dab6`, md5 `af58c4bfb11d159c77ba0b710af207b2`
  (xnu run 44915658764 on tag `j7-0393`, boot run 44916011653).

Decision tree, one flash at a time: **A boots** -> the 900 MHz GPU write is the killer, and 0399
goes back behind a boot argument with the voltage raise as the next question. **A silent, B boots**
-> the GPU is cleared and the difference left in that range is 0394, the row build that runs in
`scaledCopy` on the first present. **Both silent** -> the regression is not in these patches, and
the next thing to compare is the kernel build itself against the run that last drew. Neither image
is flashed or booted here.

## 2026-11-21 — both bisect images silent: the cmdline lost its second half

Both A and B came up black with no log, and image B was the 0393 kernel rebuilt, which cleared the
patches. The cause is in the images, not in the phone, not in macOS and not in the kernel: every
image from `44879118860` onward was packed with a **188-character** boot argument string instead of
the 522-character one every booting image used. The payload's `cmdline[608]` is where this is filed:

    44678509937 (booting, 2026-11-20)  530 chars
    44879118860 / 44902100379 / 44906332073 / 44911576305 / 44915548052 / 44916011653  188-199 chars

The truncated string stops after `sweetfb_lh=1334` and drops exactly the switches that make a boot
visible:

    rd=emmcroot0 emmcroot=USERDATA emmcwrite=1 a5log=1 rootrw=1 a5fb_noconsole=1
    cs_enforcement_disable=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1
    wdt=-1 -wdt_no_pmu wdt_test=no_user_checkin j7nospawn=bluetoothd,fairplayd.H2
    j7clk=1 j7gpuclk=900 j7fb_flip=1 j7cpu=1794

Without `a5log=1` nothing writes the persistent log, so "no log" was literal and not a hang in the
logger; without `rd=emmcroot0` there is no root device and no `ignition`; with the watchdog left on
(`wdt=-1 -wdt_no_pmu` gone) the boot is reset instead of dying where it was; `a5fb_noconsole=1` and
`j7fb_flip=1` are gone too, so the panel is never handed over. A boot with those missing can produce
exactly what was reported: black, silent, no new log, every time, with any kernel.

**Not macOS.** The two kernel builds that straddle the change ran on the same image and toolchain:
`44574976269` (booting) and `44677377375` (first silent) both print `ProductVersion: 15.8.2`,
`BuildVersion: 24H232`, `Xcode 16.0`, `MacOSX15.0.sdk`, and neither log shows a runner update. The
`__TEXT` segment did shrink by 507,904 bytes between the two, but that is 0392 removing the embedded
wallpaper picture and is harmless: the smaller `__TEXT` is the one that booted on 2026-11-20.

The fix is a repack, no kernel change:

- `artifacts/j7-boot-44928291250/j7-xnu-boot.img`, 16,672,784 bytes, sha256
  `b052b9fdd3282cd377b3b90faac8cc5ee2b645ce1e3472f0f65b63ecf9165ccf`, sha1
  `611c8e07dfbdfddf8131b7f564dc511c05b83b81`, md5 `55fe0dd50167c15702f29f3d69ceabee`
  (xnu run 44905960778 = 0394-0399, boot run 44928291250, entry-only 9,864-byte ADT,
  `product_node=false`). Its ten kernel segments hash identically to image A, so this is A's kernel
  with the working cmdline.

The two unproven clock writes stay off while the boot is being re-established: `j7cpu=0` keeps
0395's OSCCLK sequence behind its `if (cpu > 0 && cpu < 100000)` gate and `j7gpuclk=0` keeps 0399's
CMU_G3D block to the readback. `j7clk=1` is kept because the G3D block is outside that gate now and
the log line is the evidence for the next step.

Exact dispatch, copied so it is not retyped shortened:

    gh workflow run j7-boot.yml --ref main -f kernel_run_id=44905960778 -f board=d21ap \
      -f ios_version=16.7.16 -f iphone8=false -f product_node=false -f cmdline='debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress j7hw=1 j7gpu=7 sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib,SWEET_TRACE=0 rd=emmcroot0 emmcroot=USERDATA emmcwrite=1 a5log=1 rootrw=1 a5fb_noconsole=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1 -wdt_no_pmu wdt_test=no_user_checkin j7nospawn=bluetoothd,fairplayd.H2 j7clk=1 j7gpuclk=0 j7fb_flip=1 j7cpu=0 sweetfb_scale=1 sweetfb_lw=750 sweetfb_lh=1334'

Before flashing any image, read its payload at `0x48` and check that `a5log=1`, `rd=emmcroot0` and
`wdt=-1` are in it. Nothing was flashed or booted here.
