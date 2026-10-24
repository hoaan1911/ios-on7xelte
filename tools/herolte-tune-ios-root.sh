#!/bin/bash
# HEROLTE: adjust a staged iOS 16 root (tools/a5xelte-stage-ios-root.sh) for a
# phone without modem, GPU or SEP, following the SWEET donor's image v12/v13
# (NOTES.md). Run as root on macOS with the HFSX root mounted read-write.
#   - com.apple.migration.plist LastSystemVersion = this build: backboardd
#     holds SpringBoard's system-shell check-in until DataMigration finishes,
#     and the migration plugin wrapper aborts on every boot.
#   - CommCenter jobs removed from launchd.plist (no modem: crash loop).
#   - SpringBoard watchdogs off; Setup (purplebuddy) exempt from the scene
#     watchdog (it blocks in CoreTelephony without CommCenter).
#   - Setup marked complete (donor image v14), no cloud configuration UI.
#   - /private/var/containers skeleton (on iOS it lives on the data volume).
set -euo pipefail

if [[ $# != 2 || ${EUID} != 0 ]]; then
    echo "usage (as root): $0 HFSX_MOUNT BUILD" >&2
    exit 2
fi
root=$1 build=$2
[[ -f "$root/sbin/launchd" ]] || { echo "not a staged root: $root" >&2; exit 1; }

prefs="$root/private/var/mobile/Library/Preferences"
install -d -o 501 -g 501 -m 0755 "$root/private/var/mobile" \
    "$root/private/var/mobile/Library" "$prefs"

plist() {  # plist FILE KEY TYPE VALUE ...  (creates or updates entries)
    local file=$1; shift
    [[ -f $file ]] || /usr/bin/plutil -create xml1 "$file"
    while (($#)); do
        /usr/bin/plutil -replace "$1" "-$2" "$3" "$file"
        shift 3
    done
    /usr/bin/plutil -convert xml1 "$file"
    chown 501:501 "$file"
    chmod 0600 "$file"
}

plist "$prefs/com.apple.migration.plist" LastSystemVersion string "$build"
plist "$prefs/com.apple.springboard.plist" \
    BKNoWatchdogs bool YES \
    FBLaunchWatchdogExceptions json '{"com.apple.purplebuddy":100}' \
    MCForceShowCloudConfigurationUI bool NO
# Setup (purplebuddy) recognises the swipe but never finishes: it waits on
# CoreTelephony and activation (donor image v14). Mark it complete;
# activation itself comes from the ADT /chosen production-status 0 and
# security-mode 1 properties (mobileactivationd's native branch).
plist "$prefs/com.apple.purplebuddy.plist" \
    SetupDone bool YES \
    SetupFinishedAllSteps bool YES \
    SetupVersion integer 11 \
    daemonizeNeedsToRun bool NO
plist "$prefs/com.apple.managedconfiguration.notbackedup.plist" \
    LockdownActivationIndicatesCloudConfigurationAvailable bool NO

launchd="$root/System/Library/xpc/launchd.plist"
/usr/bin/python3 - "$launchd" <<'EOF'
import plistlib, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    data = plistlib.load(f)
fmt = plistlib.FMT_BINARY
removed = []
for section in ('LaunchDaemons', 'Agents', 'LaunchAgents'):
    jobs = data.get(section, {})
    for key in list(jobs):
        label = jobs[key].get('Label', '') if isinstance(jobs[key], dict) else ''
        if 'CommCenter' in key or 'CommCenter' in label:
            removed.append(label or key)
            del jobs[key]
if not removed:
    sys.exit('no CommCenter job found in launchd.plist')
with open(path, 'wb') as f:
    plistlib.dump(data, f, fmt=fmt)
print('launchd.plist: removed', ', '.join(removed))
EOF

containers="$root/private/var/containers"
install -d -o 0 -g 0 -m 0755 "$containers" "$containers/Bundle" "$containers/Data" \
    "$containers/Data/System" "$containers/Shared" "$containers/Shared/SystemGroup" \
    "$containers/Temp"

sync
echo "HEROLTE root tuning complete for build $build"
