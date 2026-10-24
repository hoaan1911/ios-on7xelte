#!/usr/bin/env python3
"""Write the gzip'd iOS root image to USERDATA through TWRP's adb.

  herolte-flash-root.py ADB root.img.gz.part00 [part01 ...] [--state FILE] [--diff]

Windows adb truncates binary stdin to `adb shell`, and TWRP's adbd ignores
`exec-in`, so the image travels as files: the decompressed stream is cut into
64 MiB chunks; runs of all-zero chunks are written on the phone from
/dev/zero (nothing crosses USB, old USERDATA data is cleared); each other chunk is gzip'd, pushed to /tmp, written at its offset
with dd, and read back for a SHA-256 comparison. --state records finished
chunks so an interrupted run resumes. Refuses to run while /data is mounted.
"""
import argparse
import gzip
import hashlib
import os
import subprocess
import sys
import tempfile
import time

DEV = '/dev/block/platform/155a0000.ufs/by-name/USERDATA'
CHUNK = 64 << 20
MIB = 1 << 20


class Chain:
    """File-like concatenation of the split .gz parts."""

    def __init__(self, paths):
        self.files = [open(p, 'rb') for p in paths]

    def read(self, n=-1):
        out = b''
        while self.files and (n < 0 or len(out) < n):
            data = self.files[0].read(-1 if n < 0 else n - len(out))
            if not data:
                self.files.pop(0).close()
                continue
            out += data
        return out


def adb(args, tool, check=True):
    # The Windows adb server occasionally dies mid-run; restart and retry.
    # Every step is idempotent (push, dd at a fixed offset, readback).
    for attempt in range(6):
        p = subprocess.run([tool, *args], capture_output=True, text=True)
        transient = 'daemon' in p.stderr or 'device offline' in p.stderr or \
            'no devices' in p.stderr or 'cannot connect' in p.stderr
        if not (p.returncode and transient):
            break
        print(f'adb transient error, retry {attempt + 1}: {p.stderr.strip()[:120]}', flush=True)
        time.sleep(5)
        subprocess.run([tool, 'start-server'], capture_output=True)
        subprocess.run([tool, 'wait-for-recovery'], capture_output=True, timeout=120)
    if check and p.returncode:
        raise SystemExit(f'adb {" ".join(args)} failed: {p.stdout} {p.stderr}')
    return p.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('adb')
    ap.add_argument('parts', nargs='+')
    ap.add_argument('--state', default='herolte-flash-root.state')
    ap.add_argument('--diff', action='store_true',
                    help='read each chunk back first and skip it if the phone already has it')
    a = ap.parse_args()

    if adb(['shell', "mount | grep -c ' /data ' || true"], a.adb).strip() != '0':
        raise SystemExit('/data is mounted; unmount it in TWRP first')
    size = int(adb(['shell', f'blockdev --getsize64 {DEV}'], a.adb).strip())
    done = set()
    if os.path.exists(a.state):
        done = {int(x) for x in open(a.state).read().split()}
    state = open(a.state, 'a')
    stream = gzip.GzipFile(fileobj=Chain(a.parts))
    zero = bytes(CHUNK)
    index = written = skipped = same = 0
    total = hashlib.sha256()
    start = time.time()
    zero_run = None

    def clear(first, end):
        # Zero chunks [first, end) on the phone; skipped if all done earlier.
        if all(i in done for i in range(first, end)):
            return
        n = (end - first) * CHUNK // MIB
        adb(['shell', f'dd if=/dev/zero of={DEV} bs=1048576 seek={first * CHUNK // MIB} '
             f'count={n} conv=fsync 2>/dev/null'], a.adb)
        for i in range(first, end):
            state.write(f'{i}\n')
        state.flush()

    with tempfile.TemporaryDirectory() as tmp:
        local = os.path.join(tmp, 'chunk.gz')
        while True:
            data = stream.read(CHUNK)
            if not data:
                break
            total.update(data)
            offset = index * CHUNK
            if offset + len(data) > size:
                raise SystemExit(f'image larger than USERDATA ({size} bytes)')
            if data == zero[:len(data)] and len(data) == CHUNK:
                if zero_run is None:
                    zero_run = index
                skipped += 1
                index += 1
                continue
            if zero_run is not None:
                clear(zero_run, index)
                zero_run = None
            if index not in done and a.diff:
                seek, count = offset // MIB, (len(data) + MIB - 1) // MIB
                back = adb(['shell', f'dd if={DEV} bs=1048576 skip={seek} count={count} '
                            f'2>/dev/null | head -c {len(data)} | sha256sum'], a.adb).split()[0]
                if back == hashlib.sha256(data).hexdigest():
                    state.write(f'{index}\n')
                    state.flush()
                    done.add(index)
                    same += 1
            if index not in done:
                with gzip.open(local, 'wb', compresslevel=1) as f:
                    f.write(data)
                adb(['push', local, '/tmp/chunk.gz'], a.adb)
                seek, count = offset // MIB, (len(data) + MIB - 1) // MIB
                out = adb(['shell', f'gzip -dc /tmp/chunk.gz | dd of={DEV} bs=1048576 '
                           f'seek={seek} conv=fsync 2>&1; rm -f /tmp/chunk.gz'], a.adb)
                back = adb(['shell', f'dd if={DEV} bs=1048576 skip={seek} count={count} '
                            f'2>/dev/null | head -c {len(data)} | sha256sum'], a.adb).split()[0]
                want = hashlib.sha256(data).hexdigest()
                if back != want:
                    raise SystemExit(f'chunk {index} readback mismatch: {back} != {want}\n{out}')
                state.write(f'{index}\n')
                state.flush()
                written += 1
            index += 1
            if index % 8 == 0:
                rate = index * CHUNK / MIB / max(time.time() - start, 1)
                print(f'{index * CHUNK >> 20} MiB scanned, {written} chunks written, '
                      f'{same} already on the phone, {skipped} zero chunks, {rate:.0f} MiB/s',
                      flush=True)
    if zero_run is not None:
        clear(zero_run, index)
    adb(['shell', 'sync'], a.adb)
    print(f'done: {index * CHUNK >> 20} MiB image, {written} chunks written now, '
          f'{len(done)} from a previous run, {skipped} zero chunks cleared on the phone')
    print(f'image sha256 {total.hexdigest()}')


if __name__ == '__main__':
    main()
