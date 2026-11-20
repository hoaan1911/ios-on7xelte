#!/usr/bin/env python3
"""Put the bundles the iOS root is missing (the wallpaper gallery) into the phone, no reflash.

Why two steps: TWRP's kernel has no hfsplus driver and iOS itself has no shell, so the HFSX
root cannot be edited from the phone. It is edited here, on the PC, and only the blocks that
changed travel over adb -- the phone keeps its first-boot state, so nothing takes an hour.

  stage   keep the bundles out of the IPSW, outside Git, as device/j7-root-extras.tar
          (choose one source):
            --ipsw FILE   the phone's .ipsw (its OS image is mounted with apfs-fuse)
            --os DIR      an already-mounted OS image, e.g.
                          /run/media/$USER/087-86698-044 (the tree that holds Applications/)
            --tar FILE    j7-root-extras.tar from the j7-root-extras workflow
          Default set: PosterBoard, PaperBoard, PreBoard, InputUI, MediaRemoteUI, ClockAngel
          (the same list tools/a5xelte-stage-ios-root.sh stages) plus any *Poster* extension
          bundle under System/Library/ExtensionKit/Extensions. --all-apps takes all of
          /Applications (about 215 MiB; check free space on the phone first).

  push    with the phone in TWRP and adb working:
            pulls USERDATA once to device/USERDATA-before.img (kept for undo),
            installs the tar into a local copy of that image,
            writes back only the changed 4 KiB blocks, read-back compared,
            saves the written ranges to device/USERDATA-changed.txt.

  restore undo a push: writes those same ranges back from device/USERDATA-before.img.

Examples (Linux, root via sudo; run `sudo -v` first):
  python3 tools/j7-root-extras.py stage --ipsw iPhone10,2_16.7.16_20H393_Restore.ipsw
  python3 tools/j7-root-extras.py push
  python3 tools/j7-root-extras.py restore

Apple payload stays in device/j7-root-extras.tar and the pulled image; both are git-ignored.
The tool never touches BOOT: the kernel and the boot arguments are not part of this.
"""
import argparse
import hashlib
import os
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

BLOCK = 4096
MIB = 1 << 20
# Same list as tools/a5xelte-stage-ios-root.sh: the display/lock-screen/wallpaper services.
UI_APPS = ('PosterBoard', 'PaperBoard', 'PreBoard', 'InputUI', 'MediaRemoteUI', 'ClockAngel')
EXT_DIR = 'System/Library/ExtensionKit/Extensions'
SUDO = [] if os.geteuid() == 0 or os.name != 'posix' else ['sudo']


def run(cmd, check=True, capture=True, **kw):
    p = subprocess.run(cmd, capture_output=capture, text=True, **kw)
    if check and p.returncode:
        sys.exit(f'command failed: {" ".join(cmd)}\n{(p.stderr or p.stdout or "").strip()[-2000:]}')
    return p


def precheck_sudo():
    if SUDO and run([*SUDO, '-n', 'true'], check=False).returncode:
        sys.exit('this step mounts and repairs an HFS+ image as root: run "sudo -v" first')


def human(n):
    return f'{n / MIB:.1f} MiB'


class Phone:
    """adb in TWRP: shell text out, exec-out bytes out."""

    def __init__(self, tool):
        self.tool = tool
        devices = run([tool, 'devices']).stdout
        if not any(line.split('\t')[-1].strip() in ('device', 'recovery')
                   for line in devices.splitlines()[1:] if line.strip()):
            sys.exit(f'no adb device ({tool} devices). Boot TWRP and plug the phone in.')
        if self.sh('mount').find(' /data ') >= 0:
            sys.exit('/data is mounted: unmount Data in TWRP first (USERDATA must not be in use)')

    def sh(self, cmd, check=True):
        p = run([self.tool, 'shell', cmd], check=check)
        return p.stdout

    def push(self, local, remote):
        run([self.tool, 'push', local, remote])

    def bytes_out(self, cmd):
        p = subprocess.run([self.tool, 'exec-out', cmd], capture_output=True)
        if p.returncode:
            sys.exit(f'adb exec-out failed: {cmd}\n{p.stderr.decode(errors="replace")[-800:]}')
        return p.stdout

    def userdata(self, explicit=None):
        """Resolve the raw USERDATA block device (by-name first, then the J7's p25)."""
        if explicit:
            return explicit
        for path in ('/dev/block/by-name/USERDATA', '/dev/block/platform/soc/by-name/USERDATA',
                     '/dev/block/mmcblk0p25'):
            got = self.sh(f'readlink -f {path} 2>/dev/null', check=False).strip()
            if got.startswith('/dev/block/'):
                return got
        sys.exit('cannot find the USERDATA block device in TWRP; pass --dev')


# ---------------------------------------------------------------- stage

def os_root(mount):
    """The tree holding /Applications: apfs-fuse puts it under root/ on iOS 16 images."""
    return os.path.join(mount, 'root') if os.path.isdir(os.path.join(mount, 'root', 'Applications')) else mount


def collect(root, all_apps):
    targets, missing = [], []
    apps = os.path.join(root, 'Applications')
    names = sorted(os.listdir(apps)) if all_apps and os.path.isdir(apps) else \
        [f'{name}.app' for name in UI_APPS]
    for name in names:
        if os.path.isdir(os.path.join(apps, name)):
            targets.append(f'Applications/{name}')
        elif not all_apps:
            missing.append(name)
    ext = os.path.join(root, EXT_DIR)
    if os.path.isdir(ext):
        for name in sorted(os.listdir(ext)):
            if 'Poster' in name and os.path.isdir(os.path.join(ext, name)):
                targets.append(f'{EXT_DIR}/{name}')
    return targets, missing


def make_tar(root, targets, out):
    """Tar with the IPSW's numeric owners, then hand the file back to the user."""
    stage = tempfile.mkdtemp(prefix='j7extras-')
    try:
        run([*SUDO, 'rsync', '-aH', '--numeric-ids', '--relative', *targets, stage], cwd=root)
        tops = sorted(os.listdir(stage))
        run([*SUDO, 'tar', '--numeric-owner', '-cpf', out, '-C', stage, *tops])
    finally:
        run([*SUDO, 'rm', '-rf', stage], check=False)
    run([*SUDO, 'chown', f'{os.getuid()}:{os.getgid()}', out], check=False)
    return out


def stage_from_os(root, out, all_apps):
    targets, missing = collect(root, all_apps)
    if missing:
        print(f'not in this OS image: {", ".join(missing)}')
    if not targets:
        sys.exit(f'no bundles found under {root}/Applications')
    for target in targets:
        exe = os.path.join(root, target)
        print(f'  {target}')
        if not os.path.isdir(exe):
            sys.exit(f'{target} disappeared before packing')
    make_tar(root, targets, out)
    with tarfile.open(out) as t:
        members = t.getmembers()
    print(f'{out}: {len(members)} entries, {human(os.path.getsize(out))}, '
          f'{len(targets)} bundle(s)')
    print('Apple payload: keep this file local (device/ is git-ignored).')


def stage_from_ipsw(ipsw, board, out, all_apps):
    if not shutil.which('apfs-fuse'):
        sys.exit('apfs-fuse is missing: sudo pacman -S apfs-fuse (Arch), '
                 'sudo apt install apfs-fuse (Debian/Ubuntu)')
    precheck_sudo()
    work = tempfile.mkdtemp(prefix='j7ipsw-')
    mount = os.path.join(work, 'os')
    os.mkdir(mount)
    try:
        with zipfile.ZipFile(ipsw) as archive:
            manifest = plistlib.loads(archive.read('BuildManifest.plist'))
            idents = [i for i in manifest.get('BuildIdentities', [])
                      if i['Info'].get('DeviceClass', '').casefold() == board.casefold()]
            if not idents:
                sys.exit(f'{ipsw} has no build identity for board {board}')
            ident = next((i for i in idents if 'Erase' in i['Info'].get('Variant', '')), idents[0])
            member = ident['Manifest']['OS']['Info']['Path']
            print(f'member {member} from {os.path.basename(ipsw)}')
            dmg = os.path.join(work, 'os.dmg')  # apfs-fuse only parses UDIF by .dmg name
            with archive.open(member) as src, open(dmg, 'wb') as dst:
                shutil.copyfileobj(src, dst, 8 * MIB)
        print(f'mounted here: {mount} (the OS image is large, this can take a minute)')
        run([*SUDO, shutil.which('apfs-fuse'), dmg, mount])
        stage_from_os(os_root(mount), out, all_apps)
    finally:
        run([*SUDO, 'umount', mount], check=False)
        run([*SUDO, 'rm', '-rf', work], check=False)


def stage_from_tar(tar, out):
    """Re-pack: drop paths that are already what we want, and check owners survive."""
    with tarfile.open(tar) as t:
        members = t.getmembers()
    print(f'{tar}: {len(members)} entries, {human(os.path.getsize(tar))}')
    if os.path.abspath(tar) != os.path.abspath(out):
        shutil.copyfile(tar, out)
    owned = sum(1 for m in members if m.uid == 0)
    print(f'{owned} of {len(members)} entries are root-owned: ', end='')
    print('ok' if owned == len(members) else 'WARNING - re-run stage so owners come from the IPSW')


# ---------------------------------------------------------------- phone image

BUNDLE_SUFFIXES = ('.app', '.appex', '.bundle')


def install_unit(name):
    """The directory a tar member belongs to: Applications/X.app, .../Extensions/X.appex."""
    parts = [p for p in name.strip('/').split('/') if p and p != '.']
    if not parts:
        return None
    for index, part in enumerate(parts):
        if part.endswith(BUNDLE_SUFFIXES):
            return '/'.join(parts[:index + 1])
    return '/'.join(parts[:2]) if len(parts) > 1 else parts[0]


def tar_targets(tar):
    """Install units inside the tar (Applications/X.app, .../Extensions/X.appex)."""
    tops = set()
    with tarfile.open(tar) as t:
        for name in t.getnames():
            unit = install_unit(name)
            if unit:
                tops.add(unit)
    return sorted(tops)


def install(mnt, tar, targets, replace):
    apps = os.path.join(mnt, 'Applications')
    present = set(os.listdir(apps)) if os.path.isdir(apps) else set()
    print(f'{len(present)} bundle(s) already in /Applications on the phone')
    todo = []
    for target in targets:
        exists = os.path.lexists(os.path.join(mnt, target))
        state = 'already there' if exists else 'missing'
        print(f'  {target}: {state}')
        if not exists or replace:
            todo.append(target)
    if not todo:
        print('nothing to install: every bundle in the tar is already on the phone')
        return False
    for target in todo:
        run([*SUDO, 'tar', '--numeric-owner', '-xpf', tar, '-C', mnt, target])
    # The kernel's exec and early launchd care about the owners of the executables.
    for target in todo:
        if not target.endswith('.app'):
            continue
        exe = os.path.join(mnt, target, os.path.basename(target)[:-4])
        if not os.path.isfile(exe):
            sys.exit(f'{target} has no executable named {os.path.basename(exe)} after extraction')
        info = run([*SUDO, 'stat', '-c', '%u:%g %a', exe]).stdout.split()
        if info[0] != '0:0':
            sys.exit(f'{exe} is {info[0]} after extraction: the tar did not keep numeric owners')
        if not int(info[1], 8) & 0o111:
            sys.exit(f'{exe} is not executable (mode {info[1]})')
    return True


def pull(phone, dev, image):
    size = int(phone.sh(f'blockdev --getsize64 {dev}').strip())
    print(f'USERDATA {dev}: {size >> 20} MiB -> {image} (this is the slow part, ~10-20 min)')
    with open(image, 'wb') as out:
        p = subprocess.Popen([phone.tool, 'exec-out', f'dd if={dev} bs=1048576 2>/dev/null'],
                             stdout=out, stderr=subprocess.DEVNULL)
        if p.wait():
            sys.exit(f'pulling {dev} failed')
    got = os.path.getsize(image)
    if got != size:
        sys.exit(f'{image} is {got} bytes, {dev} is {size}: the pull was interrupted')
    return size


def changed_runs(before, after, gap_blocks):
    """4 KiB block runs that differ, merging gaps so one push covers nearby edits."""
    size = os.path.getsize(after)
    if os.path.getsize(before) != size or size % BLOCK:
        sys.exit('the two images are not the same size')
    total = size // BLOCK
    step = (8 << 20) // BLOCK
    runs = []
    with open(before, 'rb') as fb, open(after, 'rb') as fa:
        base = 0
        while base < total:
            n = min(step, total - base)
            bb, ba = fb.read(n * BLOCK), fa.read(n * BLOCK)
            if len(bb) != n * BLOCK or len(ba) != n * BLOCK:
                sys.exit('an image ended early while comparing')
            if bb == ba:  # the common case: 8 MiB compared at once
                base += n
                continue

            def same(k):
                return bb[k * BLOCK:(k + 1) * BLOCK] == ba[k * BLOCK:(k + 1) * BLOCK]

            i = 0
            while i < n:
                if same(i):
                    i += 1
                    continue
                j = i + 1
                while j < n:
                    if not same(j):
                        j += 1
                        continue
                    k = j
                    while k < n and k - j < gap_blocks and same(k):
                        k += 1
                    if k < n and k - j < gap_blocks and not same(k):
                        j = k + 1
                        continue
                    break
                runs.append((base + i, j - i))
                i = j
            base += n
    return size, runs


def read_range(path, start, count):
    with open(path, 'rb') as f:
        f.seek(start * BLOCK)
        return f.read(count * BLOCK)


def write_runs(phone, dev, image, runs, tmpdir, limit):
    """Push and dd each run, then read it back and compare its SHA-256."""
    total = sum(c for _, c in runs)
    print(f'writing {len(runs)} run(s), {human(total * BLOCK)} over adb')
    written = 0
    for index, (start, count) in enumerate(runs, 1):
        off, left = start, count
        while left:
            n = min(left, limit)
            data = read_range(image, off, n)
            if len(data) != n * BLOCK:
                sys.exit(f'run {start}+{count} is past the end of {image}')
            local = os.path.join(tmpdir, 'j7blk')
            with open(local, 'wb') as f:
                f.write(data)
            phone.push(local, '/tmp/j7blk')
            got = phone.sh('wc -c < /tmp/j7blk').strip()
            if got != str(len(data)):
                sys.exit(f'the block file arrived as {got} of {len(data)} bytes (recovery /tmp full?)')
            phone.sh(f'dd if=/tmp/j7blk of={dev} bs={BLOCK} seek={off} count={n} conv=fsync || exit 1')
            back = phone.bytes_out(f'dd if={dev} bs={BLOCK} skip={off} count={n} 2>/dev/null')
            if hashlib.sha256(back).digest() != hashlib.sha256(data).digest():
                sys.exit(f'READ-BACK MISMATCH at block {off}+{n}: nothing further was written. '
                         f'The pre-change image is still at {image}')
            written += n
            off += n
            left -= n
        print(f'  run {index}/{len(runs)} at block {start}: {count} block(s) verified')
    phone.sh('rm -f /tmp/j7blk', check=False)
    phone.sh('sync')
    print(f'{human(written * BLOCK)} written and read back identical')


def do_push(args):
    if not os.path.isfile(args.tar):
        sys.exit(f'{args.tar} does not exist: run "stage" first (or download the '
                 f'j7-root-extras artifact)')
    precheck_sudo()
    if not shutil.which('fsck.hfsplus'):
        sys.exit('fsck.hfsplus is missing: sudo pacman -S hfsprogs / sudo apt install hfsprogs')
    phone = Phone(args.adb)
    dev = phone.userdata(args.dev)
    image = args.image
    if not image:
        image = args.before
        free = shutil.disk_usage(os.path.dirname(os.path.abspath(image)) or '.').free
        size = int(phone.sh(f'blockdev --getsize64 {dev}').strip())
        if not args.force and free < 2 * size:
            sys.exit(f'{image} needs about {2 * size >> 20} MiB free on this PC '
                     f'({free >> 20} MiB free): free some, or pass --image with a fresh pull')
        pull(phone, dev, image)
    else:
        print(f'using {image} as the pre-change image (it must be a fresh pull of {dev})')

    work = args.work
    print(f'working copy {work}')
    shutil.copyfile(image, work)
    run([*SUDO, 'chown', f'{os.getuid()}:{os.getgid()}', work], check=False)
    run([*SUDO, 'fsck.hfsplus', '-fy', work], check=False)  # the phone never unmounts cleanly

    mnt = tempfile.mkdtemp(prefix='j7root-')
    mounted = False
    try:
        run([*SUDO, 'mount', '-t', 'hfsplus', '-o', 'loop,force', work, mnt])
        mounted = True
        targets = tar_targets(args.tar)
        changed_tree = install(mnt, args.tar, targets, args.replace)
        if args.dry_run:
            print('--dry-run: nothing written to the phone')
            return 0
        if not changed_tree:
            print('the phone root already has every bundle: nothing written')
            return 0
        run([*SUDO, 'sync'])
        run([*SUDO, 'umount', mnt])
        mounted = False
    finally:
        if mounted:
            run([*SUDO, 'umount', mnt], check=False)
        run([*SUDO, 'rmdir', mnt], check=False)
    run([*SUDO, 'fsck.hfsplus', '-fy', work], check=False)

    size, runs = changed_runs(image, work, args.merge_gap_mib * MIB // BLOCK)
    total = sum(c for _, c in runs)
    print(f'{len(runs)} changed run(s), {human(total * BLOCK)} of {size >> 20} MiB')
    if not runs:
        sys.exit('the installed image is identical to the phone: nothing to write')
    if total * BLOCK > args.max_changed_mib * MIB and not args.force:
        sys.exit(f'{human(total * BLOCK)} is more than --max-changed-mib '
                 f'{args.max_changed_mib}: that is not a bundle install, pass --force if sure')
    with open(args.runs, 'w', newline='\n') as f:
        for start, count in runs:
            f.write(f'{start} {count}\n')
    print(f'written ranges saved to {args.runs} (restore mode uses them)')

    with tempfile.TemporaryDirectory() as tmpdir:
        write_runs(phone, dev, work, runs, tmpdir, args.chunk_mib * MIB // BLOCK)
    if not args.keep_work:
        os.unlink(work)
    print(f'done. Reboot out of TWRP: the wallpaper gallery should list its posters now.\n'
          f'Before: {image}  undo with: python3 {sys.argv[0]} restore')
    return 0


def do_restore(args):
    if not os.path.isfile(args.runs):
        sys.exit(f'{args.runs} does not exist: nothing was pushed from here')
    if not os.path.isfile(args.before):
        sys.exit(f'{args.before} does not exist: it is the pre-change image, keep it for undo')
    runs = [tuple(int(x) for x in line.split()) for line in open(args.runs) if line.strip()]
    phone = Phone(args.adb)
    dev = phone.userdata(args.dev)
    print(f'undoing {len(runs)} run(s) from {args.before}')
    with tempfile.TemporaryDirectory() as tmpdir:
        write_runs(phone, dev, args.before, runs, tmpdir, args.chunk_mib * MIB // BLOCK)
    print('the phone root is back to the pre-change image (same check the push did)')
    return 0


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='mode', required=True)

    st = sub.add_parser('stage', help='keep the bundles from the IPSW, outside Git')
    src = st.add_mutually_exclusive_group(required=True)
    src.add_argument('--ipsw', help='the phone IPSW; its OS image is mounted with apfs-fuse')
    src.add_argument('--os', help='an already-mounted OS image (the tree holding Applications/)')
    src.add_argument('--tar', help='an existing j7-root-extras.tar, re-checked')
    st.add_argument('--board', default='d21ap', help='BuildManifest board (default d21ap)')
    st.add_argument('--all-apps', action='store_true', help='every /Applications bundle (~215 MiB)')
    st.add_argument('--out', default=os.path.join('device', 'j7-root-extras.tar'))

    pu = sub.add_parser('push', help='install the tar into the phone root in place')
    pu.add_argument('--tar', default=os.path.join('device', 'j7-root-extras.tar'))
    pu.add_argument('--adb', default='adb')
    pu.add_argument('--dev', help='USERDATA block device in TWRP (default: resolve by name)')
    pu.add_argument('--image', help='reuse a fresh pull instead of pulling again')
    pu.add_argument('--before', default=os.path.join('device', 'USERDATA-before.img'))
    pu.add_argument('--work', default=os.path.join('device', 'USERDATA-work.img'))
    pu.add_argument('--runs', default=os.path.join('device', 'USERDATA-changed.txt'))
    pu.add_argument('--replace', action='store_true', help='overwrite bundles that are present')
    pu.add_argument('--dry-run', action='store_true', help='mount, install locally, write nothing')
    pu.add_argument('--merge-gap-mib', type=int, default=1,
                    help='merge changed runs closer than this (default 1)')
    pu.add_argument('--chunk-mib', type=int, default=32, help='bytes per adb push (default 32)')
    pu.add_argument('--max-changed-mib', type=int, default=512)
    pu.add_argument('--keep-work', action='store_true', help='keep the edited copy of the image')
    pu.add_argument('--force', action='store_true', help='ignore the size and space guards')

    rs = sub.add_parser('restore', help='write the pre-change ranges back')
    rs.add_argument('--adb', default='adb')
    rs.add_argument('--dev')
    rs.add_argument('--before', default=os.path.join('device', 'USERDATA-before.img'))
    rs.add_argument('--runs', default=os.path.join('device', 'USERDATA-changed.txt'))
    rs.add_argument('--chunk-mib', type=int, default=32)

    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(
        getattr(a, 'out', None) or getattr(a, 'before', None) or 'device/x')), exist_ok=True)

    if a.mode == 'stage':
        if a.tar:
            stage_from_tar(a.tar, a.out)
        elif a.os:
            if not os.path.isdir(a.os):
                sys.exit(f'{a.os} is not a directory')
            precheck_sudo()
            stage_from_os(os_root(a.os), a.out, a.all_apps)
        else:
            stage_from_ipsw(a.ipsw, a.board, a.out, a.all_apps)
        return 0
    if a.mode == 'push':
        return do_push(a)
    return do_restore(a)


if __name__ == '__main__':
    sys.exit(main())
