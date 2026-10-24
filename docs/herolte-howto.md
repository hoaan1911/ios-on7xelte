# Trying iOS 16 on the Galaxy S7 (SM-G930F)

Experimental. This **erases USERDATA** (all Android data) and replaces the
BOOT partition. You need a flat **SM-G930F (Exynos 8890)**, an unlocked
bootloader (OEM unlock), **TWRP** and `adb` on a PC. Other S7 variants
(G930FD, G935F edge, Snapdragon models) are untested and have different
hardware.

Apple's iOS userland is **not** in this repository and must not be
uploaded publicly. You build your own root image from Apple's public IPSW in
a **private** repository of your own (step 2).

## 1. Kernel (public, GitHub Actions)

1. Fork this repository and enable Actions.
2. Push to the `herolte` branch, or run the **xnu** workflow by hand
   (`MACHINE_CONFIG` = `HEROLTE`, the default).
3. Download the `xnu-kernel` artifact; it contains
   `kernel.development.herolte`.

## 2. iOS root image (private repository)

1. Create a **private** repository and push this branch to it as
   `ios-root-build`. The `ios-root` workflow refuses to run in a public one.
2. It downloads the iPhone X (iPhone10,3) iOS 16.7.16 IPSW members, stages a
   20 GiB HFSX root (`tools/a5xelte-stage-ios-root.sh`, then
   `tools/herolte-tune-ios-root.sh`) and publishes a private release with
   `root.img.gz.part00..02`, `d22ap.adt` and `SHA256SUMS`.
3. Download those files. Put `d22ap.adt` in `research/ipsw-x/` (gitignored).

## 3. Loader, device tree and BOOT image

```sh
# Loader (any aarch64 GCC; e.g. Arm GNU Toolchain aarch64-none-elf)
cd shim/herolte && CROSS=aarch64-none-elf- sh build.sh && cd ../..

# Apple device tree with the S7 hardware and the iPhone X /product node
python3 tools/mkadt_herolte.py herolte.adt

# Kernel + ADT + boot arguments
python3 tools/mkpayload.py kernel.development.herolte herolte.adt payload.bin \
  --cmdline "debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam -noprogress \
rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1 cs_enforcement_disable=1 \
unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 \
wdt=-1 -wdt_no_pmu wdt_test=no_user_checkin \
a5fb_keep_on=1 a5fb_noconsole=1 a5fb_perf=1 sweetfb_native=1"
```

In TWRP, save your current BOOT first (keep it: it is your way back to
Android):

```sh
adb shell dd if=/dev/block/sda5 of=/tmp/boot-stock.img
adb pull /tmp/boot-stock.img
```

Build the XNU BOOT image from it (the Samsung header and DTBs are kept):

```sh
python3 tools/herolte-pack-boot.py pack boot-stock.img \
  shim/herolte/out/xnu_boot.Image xnu-boot.img \
  --append payload.bin --partition-size 41943040
```

## 4. Flash (TWRP)

Unmount `/data` in TWRP (Mount menu) first. Check the partitions on your
phone (`ls -l /dev/block/platform/155a0000.ufs/by-name/`): on the tested
phone BOOT is `sda5` and USERDATA is `sda18`.

```sh
# iOS root into USERDATA (64 MiB chunks, each verified by SHA-256;
# resumable with --state; --diff rewrites only changed chunks)
python3 tools/herolte-flash-root.py adb root.img.gz.part00 root.img.gz.part01 \
  root.img.gz.part02 --state flash.state

# BOOT
adb push xnu-boot.img /tmp/boot.img
adb shell "dd if=/tmp/boot.img of=/dev/block/sda5 bs=1048576 conv=fsync"
adb reboot
```

The first boot takes several minutes. The console text stops when iOS
draws its first frame; that is expected.

## 5. Logs

The kernel keeps its console in the last 4 MiB of USERDATA. Back in TWRP
(Power + Volume Down, then Power + Volume Up + Home):

```sh
python3 tools/herolte-log-pull.py adb boot.txt
```

Please attach this log (it contains no personal data, but check it) and a
photo of the screen when you report a result.

## Back to Android

Write your saved `boot-stock.img` back to BOOT and format Data in TWRP.
