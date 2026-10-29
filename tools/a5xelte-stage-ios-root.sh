#!/bin/bash
# Stage a minimal iPhone 8 Plus iOS 16 root on an already-mounted HFSX volume.
# Run as root: iOS UID/GID values are part of the boot contract.
set -euo pipefail

if [[ $# != 4 || ${EUID} != 0 ]]; then
    echo "usage (as root): $0 OS_MOUNT SYSTEMOS_MOUNT APPOS_MOUNT HFSX_MOUNT" >&2
    exit 2
fi
os=$1 sysos=$2 appos=$3 root=$4
# /Applications also contains launchd-managed system UI services, not just
# optional home-screen apps. Require their source before starting any staging.
ui_apps=(PosterBoard PaperBoard PreBoard InputUI MediaRemoteUI ClockAngel)
for app in "${ui_apps[@]}"; do
    [[ -f "$os/Applications/$app.app/$app" ]] || {
        echo "missing required UI source: $app" >&2; exit 1;
    }
done
for source in "$os/sbin/launchd" "$os/System/Library/CoreServices/SpringBoard.app/SpringBoard" \
    "$sysos/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"; do
    [[ -f "$source" ]] || { echo "missing source: $source" >&2; exit 1; }
done
[[ -d "$appos/System" && -d "$root" ]] || exit 1
[[ $(stat -f %u "$os/sbin/launchd") == 0 ]] || {
    echo "OS image was mounted without owners; remount with -owners on" >&2
    exit 1
}
[[ $(stat -f %u "$sysos/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64") == 0 ]] || {
    echo "SystemOS image was mounted without owners" >&2
    exit 1
}
mount | grep -F " on $root (hfs," >/dev/null || {
    echo "destination must be a mounted HFSX volume" >&2
    exit 1
}
[[ ! -e "$root/sbin/launchd" ]] || {
    echo "destination already has launchd; refusing to overlay a partial root" >&2
    exit 1
}

# Keep the selected display/lock-screen/input service bundles. /Applications
# also contains system services, so a blanket exclusion is not safe. The user
# requested only display-related restoration, not every stock app.
app_filters=('--include=/Applications/')
for app in "${ui_apps[@]}"; do
    app_filters+=("--include=/Applications/$app.app/***")
done
app_filters+=('--exclude=/Applications/***')
# STAGE_ALL_APPS=1 keeps every stock app (Settings, Photos, ...; about
# 215 MiB): the HEROLTE root has room for them.
if [[ ${STAGE_ALL_APPS:-0} == 1 ]]; then
    # Not an empty array: macOS bash 3.2 with set -u treats it as unbound.
    app_filters=('--include=/Applications/***')
fi
# Other omitted asset families are not proven optional for every feature.
omit=(
    '--exclude=/System/Library/LinguisticData/***'
    '--exclude=/System/Library/PreinstalledAssetsV2/***'
    '--exclude=/System/Library/Health/***'
    '--exclude=/System/Library/NanoTimeKit/***'
    '--exclude=/System/Library/Assistant/***'
    '--exclude=/System/Library/OnBoardingBundles/***'
    '--exclude=/System/Library/Audio/***'
)
echo "Copying selected OS files"
/usr/bin/rsync -aH --numeric-ids --extended-attributes "${app_filters[@]}" "${omit[@]}" "$os/" "$root/"
# iOS 16 parks the removable stock apps (Mail, Maps, Calendar, Notes, Camera, Music, Weather, ...) in
# /private/var/staged_system_apps and installs them into data containers on first boot. That install
# fails here (code signing), so those apps would simply be missing. Move them into /Applications, where
# iOS loads system apps in place, same volume so it is a rename.
staged="$root/private/var/staged_system_apps"
if [[ -d $staged ]]; then
    moved=0
    for bundle in "$staged"/*.app; do
        [[ -d $bundle ]] || continue
        name=${bundle##*/}
        if [[ -e "$root/Applications/$name" ]]; then
            echo "already in /Applications: $name"
        else
            mv "$bundle" "$root/Applications/$name"
            moved=$((moved + 1))
        fi
    done
    echo "Moved $moved stock apps from staged_system_apps into /Applications"
else
    echo "no private/var/staged_system_apps in this OS image"
fi
# GPU delivery: /usr/lib/sweetgl.dylib is reserved as a real 64 MiB file (a marker and block number in every 4 KiB block) so a
# library built later can be written over its own blocks from TWRP (tools/j7-push-lib.py, extents from
# tools/hfs-file-extents.py). Loaded into backboardd only when the boot args say so (sweetenv=...); a
# placeholder that dyld cannot load is skipped (the AMFI policy allows failed insertions).
lib="$root/usr/lib/sweetgl.dylib"
if [[ ! -e $lib ]]; then
    # Every 4096-byte block starts with the marker and its own block number, so tools/hfs-file-extents.py
    # can prove, block by block, that the extents it reports are this file's.
    python3 - "$lib" <<'PY'
import sys
with open(sys.argv[1], 'wb') as f:
    for i in range(16384):
        block = bytearray(4096)
        block[0:8] = b'J7GLHOLD'
        block[8:16] = i.to_bytes(8, 'little')
        f.write(block)
PY
    chmod 755 "$lib"
    chown 0:0 "$lib" 2>/dev/null || true
    echo "Reserved $lib (64 MiB placeholder)"
fi
echo "Copying SystemOS and AppOS cryptexes"
mkdir -p "$root/private/preboot/Cryptexes/OS" "$root/private/preboot/Cryptexes/App"
/usr/bin/rsync -aH --numeric-ids --extended-attributes "$sysos/" "$root/private/preboot/Cryptexes/OS/"
/usr/bin/rsync -aH --numeric-ids --extended-attributes "$appos/" "$root/private/preboot/Cryptexes/App/"

# The donor's ignition_level=1 boot looks for the shared cache at this path.
# Hard links use no second copy of the roughly 3 GiB cache.
cache="$root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
mkdir -p "$root/System/Library/Caches/com.apple.dyld"
for file in "$cache"/*; do
    [[ -f "$file" ]] || continue
    ln "$file" "$root/System/Library/Caches/com.apple.dyld/${file##*/}"
done

# dyld may request an individual cryptex dylib before full cryptex mounting.
for cryptex in OS App; do
    source="$root/private/preboot/Cryptexes/$cryptex"
    while IFS= read -r -d '' file; do
        relative=${file#"$source"/}
        [[ $relative == System/Library/Caches/com.apple.dyld/* ]] && continue
        target="$root/$relative"
        [[ -e $target || -L $target ]] && continue
        mkdir -p "${target%/*}"
        if [[ -L $file ]]; then
            ln -s "$(readlink "$file")" "$target"
        else
            ln "$file" "$target"
        fi
    done < <(find "$source" ! -type d -print0)
done

db="$root/private/var/db"
install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for dir in diagnostics uuidtext diagnostics/timesync; do
    install -o 272 -g 272 -m 0644 /dev/null "$db/$dir/.migrated"
done
install -d -o 0 -g 0 -m 0700 "$root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
    > "$root/private/var/keybags/systembag.kb"
chown 0:0 "$root/private/var/keybags/systembag.kb"
chmod 0600 "$root/private/var/keybags/systembag.kb"

[[ $(stat -f %u "$root/sbin/launchd") == 0 ]] || {
    echo "launchd ownership was not preserved" >&2
    exit 1
}
[[ -f "$root/System/Library/CoreServices/SpringBoard.app/SpringBoard" ]] || exit 1
for app in "${ui_apps[@]}"; do
    [[ -f "$root/Applications/$app.app/$app" ]] || {
        echo "required UI executable missing after copy: $app" >&2; exit 1;
    }
done
[[ -f "$root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64" ]] || exit 1
sync
df -h "$root"
echo "A5XELTE staged root complete; validate contents and filesystem before flashing"
