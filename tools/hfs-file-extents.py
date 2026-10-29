#!/usr/bin/env python3
"""Print where a file's data lives on the HFS+ volume that is mounted, macOS only.

  hfs-file-extents.py FILE DEVICE

One line per extent, "START COUNT" in 4096-byte units from the start of the volume (the volume
is a raw image, layout NONE, so these are also offsets into the USERDATA partition it is flashed
to), in file order. Uses fcntl F_LOG2PHYS_EXT, then reads the first 8 bytes of the file and the
same bytes from the raw DEVICE at the first extent and refuses to print anything if they differ.
"""
import fcntl
import os
import struct
import subprocess
import sys

F_LOG2PHYS_EXT = 65
BLOCK = 4096


def extents(path):
    size = os.path.getsize(path)
    fd = os.open(path, os.O_RDONLY)
    out, off = [], 0
    try:
        while off < size:
            want = size - off
            # struct log2phys { unsigned l2p_flags; off_t l2p_contigbytes; off_t l2p_devoffset; }
            buf = struct.pack('=I4xqq', 0, want, off)
            res = fcntl.fcntl(fd, F_LOG2PHYS_EXT, buf)
            _, contig, dev = struct.unpack('=I4xqq', res)
            if contig <= 0 or dev % BLOCK or contig % BLOCK and off + contig < size:
                raise SystemExit(f'odd extent at {off}: dev={dev} contig={contig}')
            n = min(contig, size - off)
            out.append((dev // BLOCK, (n + BLOCK - 1) // BLOCK))
            off += n
    finally:
        os.close(fd)
    return size, out


def main():
    path, device = sys.argv[1], sys.argv[2]
    size, ext = extents(path)
    if sum(c for _, c in ext) * BLOCK != size:
        raise SystemExit(f'extents cover {sum(c for _, c in ext) * BLOCK} bytes, file is {size}')
    want = open(path, 'rb').read(8)
    got = subprocess.run(['dd', f'if={device}', f'bs={BLOCK}', f'skip={ext[0][0]}', 'count=1'],
                         capture_output=True, check=True).stdout[:8]
    if got != want:
        raise SystemExit(f'first extent of {path} does not match the device: {got!r} vs {want!r}')
    for start, count in ext:
        print(start, count)


if __name__ == '__main__':
    main()
