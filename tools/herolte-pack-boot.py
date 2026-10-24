#!/usr/bin/env python3
"""Inspect or repack a Samsung herolte (SM-G930F) BOOT image.

Host-side only: never opens a block device and never flashes the phone.
The stock BOOT may contain private data; keep it and every output off Git.

  info  STOCK                      print header, kernel Image header and DTBH
  pack  STOCK IMAGE OUT [--append P] [--ramdisk R] [--partition-size N]
        replace the kernel with IMAGE (probe or XNU loader), keep the stock
        DTBH, optionally append the XNU payload to IMAGE (preferred: S-Boot
        leaves only 16 MiB between initrd 0x89000000 and FDT 0x8a000000) or
        replace the ramdisk, copy the stock arm64 Image header fields.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

ARM64_MAGIC = b"ARMd"


def parse(boot):
    if boot[:8] != b"ANDROID!":
        raise SystemExit("not an Android BOOT image")
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page, dt_size, unused) = struct.unpack_from("<10I", boot, 8)
    if page not in (2048, 4096):
        raise SystemExit(f"unexpected page size {page}")
    align = lambda size: (size + page - 1) // page * page
    kernel_off = page
    ramdisk_off = kernel_off + align(kernel_size)
    second_off = ramdisk_off + align(ramdisk_size)
    dt_off = second_off + align(second_size)
    if dt_off + dt_size > len(boot):
        raise SystemExit("image is truncated")
    return {
        "kernel_addr": kernel_addr, "ramdisk_addr": ramdisk_addr,
        "second_addr": second_addr, "tags_addr": tags_addr, "page": page,
        "unused": unused, "align": align,
        "name": boot[48:64].split(b"\0")[0].decode(errors="replace"),
        "cmdline": boot[64:576].split(b"\0")[0].decode(errors="replace"),
        "kernel": boot[kernel_off:kernel_off + kernel_size],
        "ramdisk": boot[ramdisk_off:ramdisk_off + ramdisk_size],
        "second": boot[second_off:second_off + second_size],
        "dt": boot[dt_off:dt_off + dt_size],
        "tail": boot[dt_off + align(dt_size):],
    }


def image_header(kernel):
    if len(kernel) < 64 or kernel[56:60] != ARM64_MAGIC:
        return None
    text_offset, image_size, flags = struct.unpack_from("<3Q", kernel, 8)
    return text_offset, image_size, flags


def dtbh_entries(dt):
    if dt[:4] != b"DTBH":
        return None, []
    version, count = struct.unpack_from("<2I", dt, 4)
    if not 1 <= count <= 64 or 12 + count * 32 > len(dt):
        return version, []
    # chip, platform, subtype, hw_rev, hw_rev_end, offset, size, space
    return version, [struct.unpack_from("<8I", dt, 12 + i * 32) for i in range(count)]


def cmd_info(args):
    boot = args.stock.read_bytes()
    b = parse(boot)
    print(f"bytes={len(boot)} sha256={hashlib.sha256(boot).hexdigest()}")
    print(f"page={b['page']} name={b['name']!r}")
    print(f"kernel={len(b['kernel'])} at {b['kernel_addr']:#x}")
    print(f"ramdisk={len(b['ramdisk'])} at {b['ramdisk_addr']:#x}")
    print(f"second={len(b['second'])} at {b['second_addr']:#x} tags={b['tags_addr']:#x}")
    print(f"dt={len(b['dt'])} unused={b['unused']:#x} tail={b['tail'][:32]!r}")
    print(f"cmdline={b['cmdline']!r}")
    hdr = image_header(b["kernel"])
    if hdr:
        print("arm64 Image: text_offset={:#x} image_size={:#x} flags={:#x}".format(*hdr))
    else:
        print(f"kernel is not a raw arm64 Image (starts {b['kernel'][:4].hex()})")
    version, entries = dtbh_entries(b["dt"])
    if version is None:
        print(f"dt does not start with DTBH (starts {b['dt'][:4]!r})")
    else:
        print(f"DTBH version={version} entries={len(entries)}")
        for e in entries:
            print("  chip={} platform={:#x} subtype={:#x} hw_rev={}..{} off={:#x} size={:#x}".format(*e[:7]))


def cmd_pack(args):
    if args.output.exists():
        raise SystemExit("output exists; choose a new private path")
    stock = args.stock.read_bytes()
    b = parse(stock)
    image = bytearray(args.image.read_bytes())
    if image[56:60] != ARM64_MAGIC:
        raise SystemExit("IMAGE lacks the arm64 Image header")
    if args.append:
        # xnu_boot looks for the payload at its 4 KiB-aligned _image_end.
        payload = args.append.read_bytes()
        if payload[:8] != b"SWEETXNU":
            raise SystemExit("--append payload is not a SWEETXNU container")
        image.extend(b"\0" * (-len(image) % 4096))
        image.extend(payload)
    version, entries = dtbh_entries(b["dt"])
    if version is None or not any(e[0] == 8890 for e in entries):
        raise SystemExit("stock DT has no Exynos 8890 DTBH entry; refusing")
    stock_hdr = image_header(b["kernel"])
    if stock_hdr:
        text_offset, _, flags = stock_hdr
        # image_size covers the loaded file plus 64 KiB of zeroed headroom.
        image_size = (len(image) + 0x10000 + 0xfff) & ~0xfff
        struct.pack_into("<3Q", image, 8, text_offset, image_size, flags)
    else:
        print("warning: stock kernel header unreadable; keeping IMAGE header", file=sys.stderr)
    ramdisk = args.ramdisk.read_bytes() if args.ramdisk else b["ramdisk"]
    if not ramdisk:
        raise SystemExit("empty ramdisk")
    page, align = b["page"], b["align"]
    header = bytearray(stock[:page])
    struct.pack_into("<I", header, 8, len(image))
    struct.pack_into("<I", header, 16, len(ramdisk))
    digest = hashlib.sha1()
    for part in (bytes(image), ramdisk, b["second"], b["dt"]):
        digest.update(part)
        digest.update(struct.pack("<I", len(part)))
    header[576:596] = digest.digest()
    header[596:608] = b"\0" * 12
    out = bytearray(header)
    for part in (bytes(image), ramdisk, b["second"], b["dt"]):
        if part:
            out.extend(part)
            out.extend(b"\0" * (align(len(part)) - len(part)))
    # Unlocked S-Boot still looks for this marker; the stock signature is dropped.
    out.extend(b"SEANDROIDENFORCE")
    if args.partition_size and len(out) > args.partition_size:
        raise SystemExit(f"candidate {len(out)} exceeds BOOT partition {args.partition_size}")
    args.output.write_bytes(out)
    print("candidate only; no device write")
    print(f"stock_sha256={hashlib.sha256(stock).hexdigest()}")
    print(f"candidate_sha256={hashlib.sha256(out).hexdigest()}")
    print(f"bytes={len(out)} kernel_addr={b['kernel_addr']:#x} ramdisk_addr={b['ramdisk_addr']:#x}")
    if not args.partition_size:
        print("warning: pass --partition-size (blockdev --getsize64 BOOT) to check the fit")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    info = sub.add_parser("info")
    info.add_argument("stock", type=Path)
    pack = sub.add_parser("pack")
    pack.add_argument("stock", type=Path)
    pack.add_argument("image", type=Path)
    pack.add_argument("output", type=Path)
    pack.add_argument("--ramdisk", type=Path)
    pack.add_argument("--append", type=Path,
                      help="SWEETXNU payload appended to IMAGE (kernel slot)")
    pack.add_argument("--partition-size", type=int)
    args = ap.parse_args()
    {"info": cmd_info, "pack": cmd_pack}[args.cmd](args)


if __name__ == "__main__":
    main()
