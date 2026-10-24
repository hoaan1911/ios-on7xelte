#!/usr/bin/env python3
"""Pull and decode the herolte persistent console log (patch 0206) via TWRP.

  herolte-log-pull.py ADB OUT.txt

The log is the last 4 MiB + 4 KiB of USERDATA: block 0 is the header
("HEROLOG1", version, ring size, total bytes, flush sequence), then the ring.
The raw area is kept next to OUT as OUT.bin. Both may hold device
identifiers and userland messages; keep them out of git.
"""
import struct
import subprocess
import sys
from pathlib import Path

DEV = '/dev/block/platform/155a0000.ufs/by-name/USERDATA'
BLOCK = 4096
RING = 4 << 20


def adb(tool, *args):
    p = subprocess.run([tool, *args], capture_output=True, text=True)
    if p.returncode:
        raise SystemExit(f'adb {" ".join(args)} failed: {p.stdout} {p.stderr}')
    return p.stdout


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    tool, out = sys.argv[1], Path(sys.argv[2])
    size = int(adb(tool, 'shell', f'blockdev --getsize64 {DEV}').strip())
    first = size // BLOCK - (1 + RING // BLOCK)
    adb(tool, 'shell', f'dd if={DEV} of=/tmp/herolog.bin bs={BLOCK} skip={first} '
        f'count={1 + RING // BLOCK} 2>/dev/null')
    raw = out.with_suffix('.bin')
    adb(tool, 'pull', '/tmp/herolog.bin', str(raw))
    adb(tool, 'shell', 'rm -f /tmp/herolog.bin')
    data = raw.read_bytes()
    magic, version, ring_size, total, seq = struct.unpack_from('<8sIIQQ', data, 0)
    if magic != b'HEROLOG1' or version != 1 or ring_size != RING:
        raise SystemExit(f'no herolog header (magic {magic!r}); the log never flushed')
    ring = data[BLOCK:BLOCK + ring_size]
    if total <= ring_size:
        text = ring[:total]
    else:
        cut = total % ring_size
        text = ring[cut:] + ring[:cut]
    out.write_text(text.decode('utf-8', errors='replace'), encoding='utf-8')
    print(f'flush #{seq}: {total} bytes written in total, {len(text)} kept -> {out}')


if __name__ == '__main__':
    main()
