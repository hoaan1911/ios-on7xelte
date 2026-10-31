#!/bin/sh
# Host test of the Panfrost DRM emulation against a fake J7MaliUserClient (no iPhone needed):
#   sh tools/sweetgl-mali/test/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -fsanitize=address,undefined -g \
  -I "$here/include" -I "$here/.." -I "$here/../include" -o "$here/test.bin" "$here/test.c" "$here/../sweet_drm_mali.c"
"$here/test.bin"
rm -f "$here/test.bin"
