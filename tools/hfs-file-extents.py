#!/usr/bin/env python3
"""Print where a file's data lives on the HFS+ volume that is mounted, macOS only.

  hfs-file-extents.py FILE DEVICE

One line per extent, "START COUNT" in 4096-byte units from the start of the volume (the volume
is a raw image, layout NONE, so these are also offsets into the USERDATA partition it is flashed
to), in file order. Uses fcntl F_LOG2PHYS_EXT, then reads EVERY extent back from the raw DEVICE
and compares it with the file's own bytes; prints nothing and fails if anything differs, so a
wrong map is never published. The file is expected to carry a different 4096-byte pattern in
every block (the stage script writes the block number into each), which makes the comparison
unambiguous.
"""
import fcntl
import os
import struct
import sys

F_LOG2PHYS_EXT = 65
BLOCK = 4096
# <sys/fcntl.h> declares struct log2phys under #pragma pack(4): the two off_t fields are NOT
# 8-byte aligned, so the struct is 20 bytes: u32 flags, i64 contiguous bytes, i64 device offset.
L2P = struct.Struct('=Iqq')


def extents(path, fcntl_call=fcntl.fcntl):
    size = os.path.getsize(path)
    fd = os.open(path, os.O_RDONLY)
    out, off = [], 0
    try:
        while off < size:
            res = fcntl_call(fd, F_LOG2PHYS_EXT, L2P.pack(0, size - off, off))
            _, contig, dev = L2P.unpack(res[:L2P.size])
            if contig <= 0 or dev < 0 or dev % BLOCK:
                raise SystemExit(f'odd extent at file offset {off}: device offset {dev}, {contig} contiguous bytes')
            n = min(contig, size - off)
            out.append((dev // BLOCK, (n + BLOCK - 1) // BLOCK))
            off += n
    finally:
        os.close(fd)
    return size, out


def verify(path, device, ext):
    """Every extent, read from the raw device, must equal the same range of the file."""
    with open(path, 'rb') as f:
        data = f.read()
    dev = os.open(device, os.O_RDONLY)
    try:
        off = 0
        for start, count in ext:
            want = data[off:off + count * BLOCK]
            got = b''
            while len(got) < len(want):
                chunk = os.pread(dev, min(1 << 20, len(want) - len(got)), start * BLOCK + len(got))
                if not chunk:
                    raise SystemExit(f'short read from {device} at block {start + len(got) // BLOCK}')
                got += chunk
            if got != want:
                bad = next(i for i in range(0, len(want), BLOCK) if got[i:i + BLOCK] != want[i:i + BLOCK])
                raise SystemExit(f'extent at block {start}: file block {(off + bad) // BLOCK} differs on the device')
            off += count * BLOCK
    finally:
        os.close(dev)


def main():
    path, device = sys.argv[1], sys.argv[2]
    # Data written a moment ago may still sit in the buffer cache, but the raw device does not see it.
    fd = os.open(path, os.O_RDONLY)
    try:
        fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
    except (AttributeError, OSError):
        pass
    finally:
        os.close(fd)
    os.sync()
    size, ext = extents(path)
    if sum(c for _, c in ext) * BLOCK != size:
        raise SystemExit(f'extents cover {sum(c for _, c in ext) * BLOCK} bytes, file is {size}')
    verify(path, device, ext)
    for start, count in ext:
        print(start, count)


if __name__ == '__main__':
    main()
