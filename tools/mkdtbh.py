#!/usr/bin/env python3
"""Pack an FDT blob into Samsung's DTBH dt.img (version 2), the layout S-Boot of the Exynos 7870 reads.
Values (chip 7870, platform 0x50a6, subtype 0x217584da, two revision ranges) are those of the J7 Prime's
own dt.img; the layout was checked against it.   usage: mkdtbh.py -o dt.img file.dtb"""
import argparse, struct
PAGE = 0x800
ap = argparse.ArgumentParser(); ap.add_argument("-o", required=True); ap.add_argument("dtb"); a = ap.parse_args()
dtb = open(a.dtb, "rb").read()
assert dtb[:4] == b"\xd0\x0d\xfe\xed", "not an FDT"
pg = lambda n: -(-n // PAGE) * PAGE
ranges = [(0, 0), (1, 255)]          # hardware revisions an entry serves
hdr_len = pg(12 + 32 * len(ranges))
body = dtb + b"\0" * (pg(len(dtb)) - len(dtb))
out = struct.pack("<4sII", b"DTBH", 2, len(ranges))
for i, (lo, hi) in enumerate(ranges):    # one copy of the blob per entry, as the stock image has
    out += struct.pack("<8I", 7870, 0x50a6, 0x217584da, lo, hi, hdr_len + i * len(body), len(body), 0x20)
out = out.ljust(hdr_len, b"\0") + body * len(ranges)
open(a.o, "wb").write(out); print(a.o, len(out))
