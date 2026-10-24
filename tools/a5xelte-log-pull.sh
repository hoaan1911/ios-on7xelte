#!/bin/sh
# TWRP only: read patch 0110's log area from SYSTEM p20 into private files.
set -eu

out=${1:?usage: tools/a5xelte-log-pull.sh /private/path/boot-log.txt}
case "$out" in
  /*) ;;
  *) echo "Output path must be absolute and private" >&2; exit 1 ;;
esac
dev=$(adb shell 'readlink -f /dev/block/mmcblk0p20' | tr -d '\r')
[ "$dev" = /dev/block/mmcblk0p20 ] || {
  echo "SYSTEM p20 unavailable: $dev" >&2; exit 1;
}
adb exec-out 'dd if=/dev/block/mmcblk0p20 bs=512 count=8193 2>/dev/null' > "$out.bin"
python3 "$(dirname "$0")/a5xelte-log.py" "$out.bin" "$out"
