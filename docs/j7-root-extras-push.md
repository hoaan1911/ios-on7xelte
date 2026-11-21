# Putting the wallpaper-gallery bundles into the phone root without reflashing

The gallery behind **Add New Wallpaper** lists posters that `PosterBoard` asks its
`PaperBoard` service for, and the service is one of the `/Applications` bundles this root was
staged without (`docs/a5xelte-root-application-omission.md`). Those are Apple files, so they
cannot live in Git and they are not a kernel patch: they have to be written into the HFSX root
on USERDATA.

**Widgets come from the same place.** A widget is a `widgetkit-extension` appex embedded in its
app's `PlugIns` folder (or in `System/Library/ExtensionKit/Extensions`), and `chronod` renders it
after the app is installed. On this root a widget whose app bundle is absent has no extension to
render, and what is left on screen is the empty black/grey host tile. Packing `/Applications`
whole therefore brings the widget extensions in with the apps.

**The host itself was missing.** `inspect --path System/Library/ExtensionKit/Extensions` on the
pulled image answers "not on this image": this root was staged without ExtensionKit at all. That is
one cause for both symptoms, because ExtensionKit is what starts a `widgetkit-extension` process and
what the gallery's poster extensions are loaded through. `stage` now packs
`System/Library/ExtensionKit/Extensions` and `System/Library/Frameworks/ExtensionKit.framework` from
the OS image along with the app bundles.

Two things make that awkward:

- TWRP's kernel has **no hfsplus driver**, so the volume cannot be edited from the phone;
- iOS has no shell, so nothing on the phone can copy them in either.

So the volume is edited on the PC and only the **changed 4 KiB blocks** go over adb. The phone's
existing first-boot state on USERDATA is kept, which is the whole point: flashing a new root
would mean another hour or two of first boot, and a full iOS image push.

BOOT is not touched at all. The kernel, the ADT and the boot arguments stay exactly as they are.

## 1. Get the bundles (one time)

**A. From the bundles release (no download of a 6 GB IPSW).** Run the `j7-root-extras` workflow
(Actions > j7-root-extras > Run workflow). It packs every `/Applications` bundle out of the
iPhone 8 Plus IPSW — about 215 MiB, which is what the wallpaper gallery *and* the widget
extensions inside the app bundles need — plus `System/Library/ExtensionKit/Extensions` and
`ExtensionKit.framework`, refuses to run in a public repository, attaches `j7-root-extras.tar` to a
private release, and prints the asset tag.
Download that tar into `device/j7-root-extras.tar`.

```bash
python3 tools/j7-root-extras.py stage --tar device/j7-root-extras.tar   # re-checks owners, 1 s
```

Run the workflow with `all_apps = 0` to pack only `PosterBoard`, `PaperBoard`, `PreBoard`,
`InputUI`, `MediaRemoteUI` and `ClockAngel` instead (about 1.5 MiB, gallery only, ExtensionKit
still included), or pass
`--all-apps` to your own `stage` below.

**B. From your own IPSW on the laptop** (needs `apfs-fuse`; the OS image is mounted read-only):

```bash
python3 tools/j7-root-extras.py stage --ipsw iPhone10,2_16.7.16_20H393_Restore.ipsw
# or, if you already have the OS image mounted:
python3 tools/j7-root-extras.py stage --os /run/media/$USER/087-86698-044
```

`stage` writes `device/j7-root-extras.tar` and lists every bundle it packed. If you want the
whole stock app set instead of the six (`Settings`, `Photos`, ... about 215 MiB), add
`--all-apps` — check free space with the `push` step first, it prints it.

`device/` is in `.gitignore`: the tar and every pulled image stay on your machine.

## 2. Push it into the phone root

**This step runs on Linux or macOS, not Windows.** It mounts the phone's HFS+ USERDATA image
read-write and Windows cannot do that at all (no HFS+ write support, and the error you get is
`AttributeError: module 'os' has no attribute 'geteuid'` from older copies of this tool). On
Windows, step 1 (`stage --tar`) works — it only copies and checks the tar — and `stage --ipsw`
never did (it needs `apfs-fuse`). Copy this whole folder plus `device/j7-root-extras.tar` to the
Linux or macOS machine; nothing else is needed and the repo does not have to be at the same path.
`sudo -v` exists on Linux/macOS only — on Windows 11, `sudo` is a different program and its `-v`
error is expected.

Phone: boot **TWRP**, plug the USB cable, do **not** mount Data (the script refuses if `/data`
is mounted).

On Linux or macOS, first run `sudo -v` (the step loop-mounts the HFS+ image; sudo must not ask for a
password in the middle of it), then:

```bash
python3 tools/j7-root-extras.py push --dry-run     # optional: mounts, installs locally, writes nothing
python3 tools/j7-root-extras.py push
```

What it does, in order:

1. resolves USERDATA (every `by-name` layout TWRP uses, then the J7's `mmcblk0p25`), checks that
   the first 8 KiB really hold an `H+`/`HX` volume signature, then pulls it once to
   `device/USERDATA-before.img` (a few tens of minutes on a 24 GiB partition) and keeps it as the undo
   image. Free zero blocks are seeked over while pulling, so the file is sparse and takes the
   volume's *used* size rather than the partition size. The working copy is a clone where the host
   can do it (`cp -c` on APFS, `cp --reflink=auto` on btrfs/XFS) and a plain copy otherwise — only
   the plain-copy case needs about 2x the partition free; `--image FILE` reuses a pull you already
   made this boot;
2. clones/copies that image, repairs it (the phone never unmounts cleanly: `fsck.hfsplus` on Linux,
   `fsck_hfs` on macOS) and mounts it read-write (`mount -t hfsplus -o loop,force`, or `hdiutil
   attach` plus `mount -t hfs` on macOS). If the volume still comes up read-only, the tool stops
   before changing anything;
3. reports how many bundles `/Applications` already has and then compares every install unit in the
   tar member by member against the image, so a bundle that exists but is missing files inside it
   (the usual case: an app that was pushed without its `PlugIns/*.appex`) is reported as *incomplete*
   and only its missing members are extracted, with numeric owners (root:wheel). Each extracted
   bundle's executable is checked to exist, be root-owned and be executable. Nothing already on the
   phone is overwritten unless you pass `--replace`, and the tar's `.appex` count is printed: a tar
   with no `.appex` at all cannot supply a widget extension, whatever the phone has;
4. unmounts/detaches it and compares the copy with the pulled image at 4 KiB granularity;
5. writes only those runs (usually a few hundred KiB: catalog B-tree, allocation bitmap, volume
   header, plus the ~1.5 MiB of bundle data) with `dd … conv=fsync`, **reads every run back and
   compares its SHA-256**, then `sync`s;
6. saves the written ranges to `device/USERDATA-changed.txt`.

A partition whose size is not a whole number of 4 KiB blocks is handled. The J7's is
23,531,077,120 bytes — 4096 × 5,744,891 **plus 3,584** — and the short last block is pulled,
compared and written back at its real length, because the alternate HFS+ volume header lives in the
last 1024 bytes of the volume and losing it would make the next mount see a damaged volume.

Two guards sit in front of that pull: the volume signature check above (a wrong `--dev` stops before
22 GiB are copied), and the free-space check, which compares the partition size against the disk. The
space check is deliberately pessimistic — it cannot know how much of the volume is free space until
the pull has happened — so on a 24 GiB partition with little room left, `--force` after checking
`df -h` is the intended way past it, not a bigger disk.

Reboot out of TWRP into the normal boot (same `j7-xnu-boot.img`, same arguments). Open
**Settings > Wallpaper > Add New Wallpaper**: the poster tiles should be listed now, and a widget
added to the Home Screen or Today View should draw its content instead of the empty grey tile.
The first boot after this is a normal boot — no data migration, no hour-long first boot.

What the log should then show: `PaperBoard` as an `exec` where it has never appeared, and, when a
widget is added, `chronod` starting a `widgetkit-extension` process. If a widget is still grey
after the push, that log line is the difference between "the extension is not installed" and
"the extension is installed and does not render" — they need different fixes, and the second one
is not a bundle problem.

### Look inside the phone without writing anything

```bash
python3 tools/j7-root-extras.py inspect --path System/Library/ExtensionKit/Extensions --glob '*.appex'
python3 tools/j7-root-extras.py inspect --path Applications/PaperBoard.app/PlugIns --glob '*.appex'
```

`inspect` mounts `device/USERDATA-before.img` **read-only** (no writes to it) and lists what is
really in the root, so "is that extension on the phone?" is answered without a pull or a push.

### The answer on this phone, and what to push next

The two commands above returned `System/Library/ExtensionKit/Extensions is not on this image` and a
single entry, `Applications/PaperBoard.app/PlugIns/LegacyPoster.appex`. So the poster extensions and
the ExtensionKit tree this root should have carried are absent — the same tree that hosts widget
extensions. Check the framework too, then pack and push:

```bash
python3 tools/j7-root-extras.py inspect --path System/Library/Frameworks/ExtensionKit.framework
python3 tools/j7-root-extras.py stage --os /run/media/$USER/<the mounted OS image>   # prints its targets
python3 tools/j7-root-extras.py push --dry-run
python3 tools/j7-root-extras.py push          # phone in TWRP, nothing mounted
```

`stage` prints `System/Library/ExtensionKit/Extensions` and
`System/Library/Frameworks/ExtensionKit.framework` when the OS image has them; if it prints neither,
the mounted image is not the tree that holds the system volume and the poster extensions have to
come from the IPSW's own OS image instead (`stage --ipsw`, Linux + `apfs-fuse` only).

On macOS the first mount can be refused with

```
mount_hfs: Invalid argument
```

That is not a broken image: the volume comes off a phone partition that was never unmounted, so its
journal is dirty, and hfs(5) cannot mount a dirty journal **read-only** (`early journal init: volume
... is read-only and journal is dirty` → `hfs_mountfs returned error=22` = EINVAL). The tool then
makes a throw-away copy next to the image (`device/USERDATA-before.img.look`, an APFS clone so it
costs no space), runs `fsck_hfs -fy` on it and lists that. The pulled image is never repaired — it is
the baseline the next `push` diffs against — and the copy is deleted when `inspect` exits.

Do not write into that mount by hand, and do not copy files into it from Finder:

- the mounted volume is the *local image*, not the phone. `push` writes back only the 4 KiB blocks
  that differ between its image and the device, and it re-clones the working copy on every run, so
  anything dragged in there is thrown away or, at best, only reaches the phone if a later `push`
  happens to diff that image against the device — silently and without its own verification;
- Finder copies break the bundle: files must be owned `root:wheel` (the kernel refuses to `exec` a
  bundle owned by your user) and keep their exact modes and symlinks. A drag-and-drop copy makes them
  `you:staff`, adds `.DS_Store`, and carries resource forks the iOS root never had.

### On macOS (hackintosh or a real Mac)

HFS+ is native to macOS (still fully supported in macOS 26), so this is the shortest path: the tool
attaches the pulled image with `hdiutil`, runs `fsck_hfs -fy` on it and mounts it with `mount -t
hfs`. Needs `python3` (Xcode command line tools) and `adb` (`brew install --cask
android-platform-tools`); there is no `apfs-fuse` on macOS, so `stage --ipsw` stays Linux-only.

Space: a pull of the whole partition plus a second full copy would need about 2x USERDATA, but on
macOS the working copy is an **APFS clone** (`cp -c`), which costs almost nothing, and the pull
skips zero blocks (the image is sparse), so only the used part is really on disk. In practice one
partition's worth of *used* data is the ceiling — comfortably inside 30 GB unless USERDATA is
nearly full, and the tool prints the partition size and refuses to start if the free space is
too small. Older macOS releases use a full second copy, which is the 2x case.

The image is attached by device and mounted outside `/Volumes`, so Spotlight does not index it and
does not write anything into the phone's volume.

Two macOS-26 notes: 26.4 betas had a `fsck_hfs`/auto-mount bug that makes HFS+ volumes show up
read-only (Apple lists it as a bug with `diskutil mount /dev/diskN` as the workaround; 26.1 and
26.3 are unaffected). The tool detects that case — it stops with "mounted read-only" before
changing anything instead of half-installing. Use 26.3 if you have the choice.

```bash
sudo -v
python3 tools/j7-root-extras.py push --dry-run
python3 tools/j7-root-extras.py push
```

### `ADB server didn't ACK` on macOS

adb 37.x switched macOS back to its **legacy USB backend** (`usb_osx.cpp`); `ADB_LIBUSB=1` opts
back into libusb. On some USB controllers — hackintoshes especially — the legacy path crashes the
daemon on the first device it enumerates, which looks like this:

```
* daemon not running; starting now at tcp:5037
ADB server didn't ACK
… usb_osx.cpp:323 reported max packet size for 52103bedfa51642f is 512
* failed to start daemon
```

So, in order:

1. Watch the daemon itself (the client log above stops before the reason):
   `ADB_TRACE=usb:v adb -L tcp:5037 nodaemon server`, Ctrl-C after the error line. If the shell
   reports `Segmentation fault: 11` instead of an error line, it is a host-side crash, and
   `ls -t ~/Library/Logs/DiagnosticReports | grep -i adb | head` names the faulting library.
2. Use libusb instead of the legacy backend, for the server and for this tool:
   `adb kill-server; export ADB_LIBUSB=1 LIBUSB=1; ADB_LIBUSB=1 adb start-server; adb devices`
   `adb kill-server` often answers `cannot connect to daemon at tcp:5037: Connection refused`
   when the server already died — that is the normal answer, there was nothing to kill; carry on.
   The exports only last for that terminal, so the same shell has to run the tool. Put them in
   `~/.zshrc` (`export ADB_LIBUSB=1`) and run `adb kill-server` once, if you would rather not repeat
   the prefix on every call; that covers plain `adb devices` as well as this tool.
   (`LIBUSB=1` is the old name, harmless to set as well.) If the server is already running from a
   previous backend you must `adb kill-server` first — the variable only affects a fresh server.
3. Confirm which backend the server actually picked: `adb server-status` prints `usb_backend:`
   (`LIBUSB` when the variable took effect). If it still says the legacy backend, the variable did
   not reach the server — put it on the command itself, as in step 2, rather than `export`ing it.
4. If libusb does not see the device either, use platform-tools 35.0.1, which is libusb by
   default: `curl -LO https://dl.google.com/android/repository/platform-tools_r35.0.1-darwin.zip`,
   unzip, and call that `adb` explicitly with `--adb /path/to/adb` (or put it first in `PATH`).
5. Check nothing else holds the port: `lsof -nP -iTCP:5037` and kill whatever it lists.

The tool prints this same hint when `adb devices` returns an error, instead of just the raw adb
output.

## 3. Undo

The pre-change image and the changed-run list are both still there:

```bash
python3 tools/j7-root-extras.py restore
```

It writes those same ranges back from `device/USERDATA-before.img`, with the same read-back
comparison. Keep the image until the gallery works.

## Limits worth knowing

- `--max-changed-mib 512` is a guard: a bundle install changes a few MiB, so a much bigger delta
  means the wrong image or an interrupted pull, and nothing is written without `--force`.
- Resource forks (`._` files) are not carried by tar. The six display bundles were audited as
  having none (`docs/a5xelte-root-application-omission.md`); with `--all-apps` some stock apps
  may, and they would run without theirs.
- Only USERDATA content changes. If the gallery is still empty after this, the next line of
  evidence is the boot log: `PaperBoard` should now appear as an `exec`.
- Very large additions are slow for the same reason the rest of this port is: the diff is computed
  and written over adb, so 215 MiB of new bundles is minutes, not seconds. The phone's HFSX volume
  had about 2.1 GiB free when this was written.
