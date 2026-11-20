#!/usr/bin/env python3
"""Turn the base/new pairs into xnu/patches/NNNN-*.patch files with the project header."""
import subprocess, sys

CO = "Co-Authored-By: dipa-studio <183933817+dipa-studio@users.noreply.github.com>"

JOBS = [
    ("0387-j7-build-the-panel-row-once-and-paint-every-dark-pixel",
     "iokit/Kernel/arm/SweetFramebuffer.cpp",
     "j7: build the panel row once, and paint every dark pixel",
     """The frame now holds 9 fps with the tearing gone and cpu_mhz=891, and the cost is all in the
row build: 65 ms of a 106 ms frame, which is 45 ns for a source pixel that is one uncached read
of the client's surface.  memcpy each source row into cacheable memory once and build from there,
and do the same for the wallpaper sample, whose column is now a table built once per present.

The wallpaper itself still never appeared because the paint required a run of 32 output pixels of
the same colour, and at 1.44x one source pixel covers one or two, so that test could not match on
this panel.  Paint every dark opaque pixel with the picture instead."""),
    ("0388-j7-read-the-gauge-through-the-hsi2c-auto-mode",
     "bsd/dev/a5xelte_battery.c",
     "j7: read the gauge through the HSI2C block's auto mode",
     """Every high-speed bus logged istat=0 and answered nothing, and the reason is in the register
map: the Exynos HSI2C has I2C_INT_ENABLE at 0x20 and I2C_INT_STATUS at 0x24, not 0x10 and 0x14,
and its FIFOs at 0x34/0x38, not 0x1c/0x20.  The scan was reading and writing the wrong words, so
no transfer ever started.  Use the block's auto mode as drivers/i2c/busses/i2c-exynos5.c does -
master and direction in I2C_CTL, target in I2C_ADDR, length/read/stop/run in I2C_AUTO_CONF,
completion in I2C_INT_STATUS - and leave the timing registers the bootloader programmed alone."""),
]

for name, path, subject, body in JOBS:
    base = "artifacts/pw/base/" + path
    new = "artifacts/pw/new/" + path
    d = subprocess.run(["diff", "-u", "--label", "a/" + path, "--label", "b/" + path, base, new],
                       capture_output=True, text=True)
    if d.returncode != 1:
        sys.exit("diff failed for %s: %d %s" % (path, d.returncode, d.stderr))
    out = "From: j7\nSubject: %s\n\n%s\n\n%s\n\n%s" % (subject, body, CO, d.stdout)
    open("xnu/patches/%s.patch" % name, "w").write(out)
    print("wrote xnu/patches/%s.patch (%d lines)" % (name, out.count("\n")))
