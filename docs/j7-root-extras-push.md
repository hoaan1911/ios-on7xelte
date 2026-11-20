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
extensions inside the app bundles need — plus any `*Poster*` extension, refuses to run in a
public repository, attaches `j7-root-extras.tar` to a private release, and prints the asset tag.
Download that tar into `device/j7-root-extras.tar`.

```bash
python3 tools/j7-root-extras.py stage --tar device/j7-root-extras.tar   # re-checks owners, 1 s
```

Run the workflow with `all_apps = 0` to pack only `PosterBoard`, `PaperBoard`, `PreBoard`,
`InputUI`, `MediaRemoteUI` and `ClockAngel` instead (about 1.5 MiB, gallery only), or pass
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

1. pulls USERDATA once to `device/USERDATA-before.img` (~10–20 min) and keeps it as the undo
   image. Free zero blocks are seeked over while pulling, so the file is sparse and takes the
   volume's *used* size rather than the partition size. The working copy is a clone where the host
   can do it (`cp -c` on APFS, `cp --reflink=auto` on btrfs/XFS) and a plain copy otherwise — only
   the plain-copy case needs about 2x the partition free; `--image FILE` reuses a pull you already
   made this boot;
2. clones/copies that image, repairs it (the phone never unmounts cleanly: `fsck.hfsplus` on Linux,
   `fsck_hfs` on macOS) and mounts it read-write (`mount -t hfsplus -o loop,force`, or `hdiutil
   attach` plus `mount -t hfs` on macOS). If the volume still comes up read-only, the tool stops
   before changing anything;
3. reports how many bundles `/Applications` already has and which ones are missing, then extracts
   only the missing ones from the tar with their numeric owners (root:wheel) and checks that each
   bundle's executable exists, is root-owned and is executable. That count is also the answer to
   "how much of the root is really missing": the phone keeps its first-boot state, so the tool
   only prints, it never replaces an existing bundle unless you pass `--replace`;
4. unmounts/detaches it and compares the copy with the pulled image at 4 KiB granularity;
5. writes only those runs (usually a few hundred KiB: catalog B-tree, allocation bitmap, volume
   header, plus the ~1.5 MiB of bundle data) with `dd … conv=fsync`, **reads every run back and
   compares its SHA-256**, then `sync`s;
6. saves the written ranges to `device/USERDATA-changed.txt`.

Reboot out of TWRP into the normal boot (same `j7-xnu-boot.img`, same arguments). Open
**Settings > Wallpaper > Add New Wallpaper**: the poster tiles should be listed now, and a widget
added to the Home Screen or Today View should draw its content instead of the empty grey tile.
The first boot after this is a normal boot — no data migration, no hour-long first boot.

What the log should then show: `PaperBoard` as an `exec` where it has never appeared, and, when a
widget is added, `chronod` starting a `widgetkit-extension` process. If a widget is still grey
after the push, that log line is the difference between "the extension is not installed" and
"the extension is installed and does not render" — they need different fixes, and the second one
is not a bundle problem.

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
