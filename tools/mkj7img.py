#!/usr/bin/env python3
"""
Wrap a firmware binary (U-Boot) into a boot.img that the SM-G610F's S-Boot accepts.
S-Boot ignores the header offsets. No Samsung file is needed: the header values are those of the stock image.

usage: mkj7img.py --kernel u-boot-nodtb.bin --dt dt.img -o j7-boot.img [--ramdisk f] [--cmdline s] [--name s]
"""
import argparse, hashlib, struct
PAGE = 2048
pg = lambda n: -(-n // PAGE) * PAGE
ap = argparse.ArgumentParser()
ap.add_argument("--kernel", required=True); ap.add_argument("--dt", required=True)
ap.add_argument("--ramdisk"); ap.add_argument("--cmdline", default="console=null"); ap.add_argument("--name", default="ios-j7uefi")
ap.add_argument("-o", "--output", required=True)
a = ap.parse_args()
kernel = open(a.kernel, "rb").read()
ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else b"\0" * 0x77
dt = open(a.dt, "rb").read()
assert dt[:4] == b"DTBH", "dt must be a DTBH image (tools/mkdtbh.py)"
hdr = bytearray(PAGE)
hdr[:8] = b"ANDROID!"
struct.pack_into("<9I", hdr, 8, len(kernel), 0x10008000, len(ramdisk), 0x11000000, 0, 0x10f00000, 0x10000100, PAGE, len(dt))
struct.pack_into("<I", hdr, 44, 0x10040144)
hdr[48:64] = a.name.encode()[:15].ljust(16, b"\0")
hdr[64:576] = a.cmdline.encode()[:511].ljust(512, b"\0")
h = hashlib.sha1()
for b in (kernel, ramdisk, dt):
    h.update(b); h.update(struct.pack("<I", len(b)))
hdr[576:608] = h.digest().ljust(32, b"\0")
pad = lambda b: b + b"\0" * (pg(len(b)) - len(b))
out = bytes(hdr) + pad(kernel) + pad(ramdisk) + pad(dt) + b"SEANDROIDENFORCE"
open(a.output, "wb").write(out)
print("%s: %d bytes (kernel %#x, ramdisk %#x, dt %#x)" % (a.output, len(out), len(kernel), len(ramdisk), len(dt)))
