#!/usr/bin/env python3
"""Write sweetgl.dylib over its reserved blocks in USERDATA, from TWRP through adb (Windows ok).

  python tools\\j7-push-lib.py sweetgl.dylib sweetgl-extents.txt [--adb adb] [--dev DEVICE]

sweetgl.dylib is the 64 MiB file from the j7gpu-dylib workflow; sweetgl-extents.txt comes with
the ios-root release that built the flashed root (one "START COUNT" line per extent, 4096-byte
units from the start of USERDATA). Only the blocks of that one file are written: the first extent
must still hold the root's placeholder marker (J7GLHOLD) or a previous library (Mach-O magic),
else nothing is written. The file travels as a file (Windows adb truncates binary stdin to
`adb shell`), is dd'd into place, and is read back after dropping caches and compared by SHA-256.
Refuses to run while /data is mounted. Boot afterwards with
  sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

BLOCK = 4096
SIZE = 64 << 20


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('lib')
    ap.add_argument('extents')
    ap.add_argument('--adb', default='adb')
    ap.add_argument('--dev', default='/dev/block/by-name/USERDATA')
    args = ap.parse_args()

    def adb(*a, check=True):
        r = subprocess.run([args.adb, *a], capture_output=True, text=True)
        if check and r.returncode != 0:
            sys.exit(f'adb {" ".join(a)} failed: {r.stderr.strip() or r.stdout.strip()}')
        return r.stdout

    ext = [tuple(int(x) for x in line.split()) for line in open(args.extents) if line.strip()]
    if not ext or any(len(e) != 2 for e in ext):
        sys.exit('extents file must hold "START COUNT" lines')
    data = open(args.lib, 'rb').read()
    if len(data) != SIZE or sum(c for _, c in ext) * BLOCK != SIZE:
        sys.exit(f'{args.lib} must be exactly 64 MiB and the extents must cover 64 MiB '
                 f'(library {len(data)} bytes, extents {sum(c for _, c in ext) * BLOCK})')
    sha = hashlib.sha256(data).hexdigest()

    if 'device' not in adb('devices'):
        sys.exit('no adb device (boot TWRP and plug the phone in)')
    if ' /data ' in adb('shell', 'mount', check=False):
        sys.exit('/data is mounted: unmount it in TWRP first (USERDATA must not be in use)')
    dev = adb('shell', f'readlink -f {args.dev}').strip()
    if not dev.startswith('/dev/block/'):
        sys.exit(f'cannot resolve {args.dev}: {dev!r}')
    print(f'USERDATA = {dev}, {len(ext)} extent(s), sha256 {sha[:16]}')

    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, 'probe')
        adb('shell', f'dd if={dev} bs={BLOCK} skip={ext[0][0]} count=1 of=/tmp/j7probe 2>/dev/null')
        adb('pull', '/tmp/j7probe', probe)
        magic = open(probe, 'rb').read(8)
        if not (magic == b'J7GLHOLD' or magic[:4] == b'\xcf\xfa\xed\xfe'):
            sys.exit(f'first extent does not hold the placeholder or a library ({magic!r}): '
                     'wrong extents file for the flashed root. Nothing was written.')
        print(f'first extent holds {"the placeholder" if magic == b"J7GLHOLD" else "a library"}: ok')

        # Plain dd only: the recovery's dd has conv= disabled (and a partition is never truncated anyway).
        # Each extent is read back into its own file, so no dd ever needs to seek inside a regular file.
        write, read, off = [], [], 0
        for i, (start, count) in enumerate(ext):
            write.append(f'dd if=/tmp/sweetgl.dylib of={dev} bs={BLOCK} skip={off} seek={start} '
                         f'count={count} || exit 1')
            read.append(f'dd if={dev} of=/tmp/j7back{i} bs={BLOCK} skip={start} count={count} || exit 1')
            off += count
        for name, lines in (('j7write.sh', write), ('j7read.sh', ['rm -f /tmp/j7back*'] + read)):
            path = os.path.join(tmp, name)
            with open(path, 'w', newline='\n') as f:
                f.write('\n'.join(lines) + '\n')
            adb('push', path, f'/tmp/{name}')
        print('pushing the library (64 MiB)...')
        adb('push', args.lib, '/tmp/sweetgl.dylib')
        pushed = adb('shell', 'wc -c < /tmp/sweetgl.dylib', check=False).strip().split()
        if not pushed or not pushed[0].isdigit() or int(pushed[0]) != SIZE:
            sys.exit(f'the library did not arrive in full in the recovery /tmp ({pushed}); free space there: '
                     + adb('shell', 'df /tmp', check=False).strip().replace('\n', ' | '))

        def on_phone(script):
            """Run a pushed script; show everything it printed, fail on its exit status."""
            r = subprocess.run([args.adb, 'shell', f'sh /tmp/{script}; echo RC=$?'], capture_output=True, text=True)
            lines = (r.stdout + r.stderr).strip().splitlines()
            rc = next((l for l in reversed(lines) if l.startswith('RC=')), 'RC=?')
            if rc != 'RC=0':
                sys.exit(f'{script} failed ({rc}). Output from the phone:\n' + '\n'.join(lines[-12:]))

        on_phone('j7write.sh')
        adb('shell', 'sync', check=False)
        # Best effort only: some recoveries refuse this write. The read-back below is what decides.
        if subprocess.run([args.adb, 'shell', 'echo 3 > /proc/sys/vm/drop_caches'],
                          capture_output=True, text=True).returncode != 0:
            print('note: could not drop caches (recovery refused); verifying anyway')
        on_phone('j7read.sh')
        h = hashlib.sha256()
        for i in range(len(ext)):
            part = os.path.join(tmp, f'back{i}')
            adb('pull', f'/tmp/j7back{i}', part)
            h.update(open(part, 'rb').read())
        got = h.hexdigest()
        adb('shell', 'rm -f /tmp/sweetgl.dylib /tmp/j7back* /tmp/j7probe /tmp/j7write.sh /tmp/j7read.sh', check=False)
        if got != sha:
            sys.exit(f'READ-BACK MISMATCH: wrote {sha[:16]}, device returns {got[:16]}')
        print(f'VERIFY OK: {sha[:16]} is on the phone')


if __name__ == '__main__':
    main()
