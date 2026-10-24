#!/bin/sh
# Build the Samsung EL1 loader. No flash or payload packaging here.
set -eu
cd "$(dirname "$0")"
mkdir -p outxnu
: "${CC:=gcc}"
: "${LD:=ld}"
: "${OBJCOPY:=objcopy}"
CFLAGS="-c -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -nostdlib -fno-asynchronous-unwind-tables -mgeneral-regs-only -march=armv8-a -Wall -Wextra -Werror"
"${CC}" $CFLAGS xnu_boot.S -o outxnu/xnu_boot.o
"${CC}" $CFLAGS xnu_boot.c -o outxnu/xnu_boot_c.o
"${CC}" $CFLAGS display_console.c -o outxnu/display_console.o
"${CC}" $CFLAGS ../fdt.c -o outxnu/fdt.o
"${LD}" -static -T xnu_boot.ld --build-id=none outxnu/xnu_boot.o outxnu/xnu_boot_c.o outxnu/display_console.o outxnu/fdt.o -o outxnu/xnu_boot.elf
"${OBJCOPY}" -O binary outxnu/xnu_boot.elf outxnu/Image
python3 - <<'PY'
from pathlib import Path
import struct
p = Path('outxnu/Image').read_bytes()
assert p[56:60] == b'ARMd'
assert struct.unpack_from('<Q', p, 8)[0] == 0x205000
assert struct.unpack_from('<Q', p, 16)[0] == 0
print(f'outxnu/Image: {len(p)} bytes; Samsung legacy Image header OK')
PY
