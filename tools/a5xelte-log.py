#!/usr/bin/env python3
"""Decode patch 0110's SYSTEM console ring from a TWRP block read."""
import argparse
import pathlib
import struct

RING_SIZE = 4 * 1024 * 1024
AREA_SIZE = 512 + RING_SIZE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("area", type=pathlib.Path, help="first 8193 sectors of SYSTEM")
    parser.add_argument("output", type=pathlib.Path, help="decoded private text file")
    args = parser.parse_args()
    data = args.area.read_bytes()
    if len(data) != AREA_SIZE:
        parser.error(f"expected {AREA_SIZE} bytes, got {len(data)}")
    magic, version, size, total, seq = struct.unpack_from("<8sIIQQ", data)
    if magic != b"A5XLOG1\0" or version != 1 or size != RING_SIZE or not seq:
        parser.error("no valid A5XLOG1 header; log may not have flushed")
    ring = data[512:]
    if total <= size:
        raw = ring[:total]
    else:
        start = total % size
        raw = ring[start:] + ring[:start]
    args.output.write_text(raw.decode("utf-8", "replace").replace("\r", ""))
    print(f"flush #{seq}; {total} bytes written; {len(raw)} bytes recovered"
          f"{' (oldest bytes overwritten)' if total > size else ''}")


if __name__ == "__main__":
    main()
