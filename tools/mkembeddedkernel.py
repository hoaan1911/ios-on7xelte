#!/usr/bin/env python3
"""Embed a SWEETXNU payload in the Samsung J7 Prime ARM64 Image.

The loader expects a 16-byte J7XNPAY1 wrapper at kernel offset 0x2000,
followed by the mkpayload.py output. The ARM64 Image image_size field is
updated so it describes the complete embedded image plus 64 KiB headroom.
"""
import argparse
import struct
from pathlib import Path

OFFSET = 0x2000
MAGIC = b"J7XNPAY1"
HEADROOM = 0x10000

ap = argparse.ArgumentParser()
ap.add_argument("loader", help="shim/j7prime/outxnu/Image")
ap.add_argument("payload", help="tools/mkpayload.py output")
ap.add_argument("output", help="combined kernel Image output")
a = ap.parse_args()
loader = Path(a.loader).read_bytes()
payload = Path(a.payload).read_bytes()
if len(loader) > OFFSET:
    raise SystemExit(f"loader is {len(loader)} bytes; it must fit before offset 0x{OFFSET:x}")
if loader[56:60] != b"ARMd":
    raise SystemExit("loader lacks ARM64 Image magic ARMd at offset 56")
if struct.unpack_from("<Q", loader, 8)[0] != 0:
    raise SystemExit("loader text_offset must be zero")
if struct.unpack_from("<Q", loader, 24)[0] != 0xA:
    raise SystemExit("loader flags must be 0xa")
if payload[:8] != b"SWEETXNU":
    raise SystemExit("payload does not start with SWEETXNU magic")

combined = bytearray(loader)
combined.extend(b"\0" * (OFFSET - len(combined)))
combined.extend(MAGIC)
combined.extend(struct.pack("<Q", len(payload)))
combined.extend(payload)
# ARM64 Image header: image_size is the uint64 at byte offset 16.
image_size = len(combined) + HEADROOM
struct.pack_into("<Q", combined, 16, image_size)
Path(a.output).write_bytes(combined)
print(f"loader: {len(loader)} bytes")
print(f"payload: {len(payload)} bytes")
print(f"embedded payload offset: 0x{OFFSET + 16:x}")
print(f"combined kernel: {len(combined)} bytes; Image image_size=0x{image_size:x}")
