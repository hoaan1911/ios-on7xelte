#!/usr/bin/env python3
"""Repack a Samsung BOOT candidate containing the RAM-only a5xelte probe.

Host-side only. Never opens a block device and never flashes the phone.
The reference BOOT may contain private data; keep it and the output off Git.
"""
import argparse
import hashlib
from pathlib import Path
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("reference_boot", type=Path)
    ap.add_argument("probe_image", type=Path)
    ap.add_argument("output_boot", type=Path)
    ap.add_argument("--ramdisk", type=Path,
                    help="replace the reference ramdisk with an XNU payload")
    args = ap.parse_args()
    if args.output_boot.exists():
        raise SystemExit("output exists; choose a new private path")
    ref = args.reference_boot.read_bytes()
    probe = args.probe_image.read_bytes()
    if ref[:8] != b"ANDROID!":
        raise SystemExit("reference is not an Android BOOT image")
    fields = struct.unpack_from("<10I", ref, 8)
    kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags_addr, page, dt_size, unused = fields
    if page != 2048 or second_size or unused or not ramdisk_size or not dt_size:
        raise SystemExit("unexpected Samsung BOOT layout")
    if probe[56:60] != b"ARMd" or struct.unpack_from("<Q", probe, 8)[0] != 0x205000:
        raise SystemExit("probe lacks the observed legacy Image header")
    if struct.unpack_from("<Q", probe, 16)[0] != 0:
        raise SystemExit("probe Image has an unexpected runtime size")
    align = lambda size: (size + page - 1) // page * page
    ramdisk_off = page + align(kernel_size)
    dt_off = ramdisk_off + align(ramdisk_size)
    if dt_off + dt_size > len(ref):
        raise SystemExit("reference image is truncated")
    ramdisk = (args.ramdisk.read_bytes() if args.ramdisk else
               ref[ramdisk_off:ramdisk_off + ramdisk_size])
    if not ramdisk:
        raise SystemExit("empty ramdisk")
    dt = ref[dt_off:dt_off + dt_size]
    if dt[:4] != b"DTBH" or struct.unpack_from("<I", dt, 4)[0] != 2:
        raise SystemExit("reference DTBH version is not the observed format")
    count = struct.unpack_from("<I", dt, 8)[0]
    if not 1 <= count <= 32 or 12 + count * 32 > len(dt):
        raise SystemExit("invalid DTBH table")
    entries = [struct.unpack_from("<8I", dt, 12 + i * 32) for i in range(count)]
    if not any(e[0] == 7580 and e[3] <= 4 <= e[4] for e in entries):
        raise SystemExit("DTBH has no Exynos7580 hardware revision 4 entry")
    header = bytearray(ref[:page])
    struct.pack_into("<I", header, 8, len(probe))
    struct.pack_into("<I", header, 16, len(ramdisk))
    digest = hashlib.sha1()
    for part in (probe, ramdisk, b"", dt):
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    header[576:596] = digest.digest()
    boot = bytearray(header)
    for part in (probe, ramdisk, dt):
        boot.extend(part)
        boot.extend(b"\0" * (align(len(part)) - len(part)))
    boot.extend(b"SEANDROIDENFORCE")
    if len(boot) >= 32 * 1024 * 1024:
        raise SystemExit("candidate exceeds the 32 MiB BOOT partition")
    args.output_boot.write_bytes(boot)
    print("candidate only; no device write")
    print(f"reference_sha256={hashlib.sha256(ref).hexdigest()}")
    print(f"candidate_sha256={hashlib.sha256(boot).hexdigest()}")
    print(f"bytes={len(boot)} kernel_addr={kernel_addr:#x} ramdisk_addr={ramdisk_addr:#x} tags_addr={tags_addr:#x}")


if __name__ == "__main__":
    main()
