#!/usr/bin/env python3
"""Change the XNU boot-args inside a finished J7 boot.img without rebuilding.
Finds the real SWEETXNU payload by its structure (the loader's own code also contains the text SWEETXNU,
so a plain search is not enough: version must be 1 and nsegs 1..32), in the kernel part (payload
embedded in the loader image) or in the ramdisk, and rewrites only its cmdline field (0x48, 608 bytes).
usage: patch-cmdline.py in.img out.img 'boot args'"""
import hashlib, re, struct, sys
src, dst, cmd = sys.argv[1], sys.argv[2], sys.argv[3].encode()
assert len(cmd) < 608, 'boot-args too long'
d = bytearray(open(src, 'rb').read())
ks, ka, rs, ra, ss, sa, ta, ps, dts = struct.unpack('<9I', d[8:44])
pg = lambda n: -(-n // ps) * ps
k0, r0 = ps, ps + pg(ks); d0 = r0 + pg(rs) + pg(ss)
at = None
for name, a, n in (('kernel', k0, ks), ('ramdisk', r0, rs)):
    blob = bytes(d[a:a + n])
    for m in re.finditer(b'SWEETXNU', blob):
        i = m.start()
        if i + 0x2a8 > len(blob):
            continue
        ver, nsegs = struct.unpack('<II', blob[i + 8:i + 16])
        old = blob[i + 0x48:i + 0x48 + 608]
        text = old.split(b'\0')[0]
        if ver == 1 and 1 <= nsegs <= 32 and all(32 <= c < 127 for c in text):
            at = a + i + 0x48
            print('payload in %s at +%#x, %d segments\nold: %s' % (name, i, nsegs, text.decode()))
            break
    if at is not None:
        break
if at is None:
    sys.exit('no valid SWEETXNU payload found; nothing written')
d[at:at + 608] = cmd.ljust(608, b'\0')
h = hashlib.sha1()
for a, n in ((k0, ks), (r0, rs), (d0, dts)):
    h.update(bytes(d[a:a + n])); h.update(struct.pack('<I', n))
d[576:608] = h.digest().ljust(32, b'\0')
open(dst, 'wb').write(d); print('new:', cmd.decode()); print('wrote', dst, len(d))
