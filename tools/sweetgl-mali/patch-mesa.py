#!/usr/bin/env python3
"""Source edits Mesa 24.3's Panfrost needs to run over sweet_drm_mali.c on iOS.

  patch-mesa.py MESA_DIR

Every edit asserts what it expects to find and fails loudly if Mesa moved. Panfrost mmap()s a BO
by the offset MMAP_BO returned (pan_kmod.h) and munmap()s it in pan_bo.c; here the BO memory is
ordinary user memory the kernel maps into the GPU, so the first is routed to sweet_drm_bo_map and
the second must not unmap memory the GPU still maps.
"""
import re
import sys
from pathlib import Path

mesa = Path(sys.argv[1])


def read(rel):
    p = mesa / rel
    if not p.is_file():
        sys.exit(f'{rel}: not found in {mesa}')
    return p, p.read_text()


# 1) pan_kmod.h: os_mmap of a BO -> sweet_drm_bo_map (placed before pan_kmod_bo_mmap's return-type line)
p, s = read('src/panfrost/lib/kmod/pan_kmod.h')
if 'sweet_drm_bo_map' not in s:
    m = re.search(r'(?m)^pan_kmod_bo_mmap\(', s)
    if not m:
        sys.exit('pan_kmod.h: pan_kmod_bo_mmap( not found')
    prev = s.rfind('\n', 0, m.start() - 1) + 1          # start of the return-type line above it
    if not re.search(r'static|inline|void', s[prev:m.start()]):
        sys.exit('pan_kmod.h: unexpected text above pan_kmod_bo_mmap: ' + repr(s[prev:m.start()]))
    if 'os_mmap(' not in s[m.start():m.start() + 1500]:
        sys.exit('pan_kmod.h: os_mmap( not used by pan_kmod_bo_mmap')
    hook = ('#ifdef __APPLE__\n'
            'void *sweet_drm_bo_map(int fd, uint64_t offset);\n'
            '#undef os_mmap\n'
            '#define os_mmap(addr, length, prot, flags, fd, offset) sweet_drm_bo_map(fd, offset)\n'
            '#endif\n\n')
    p.write_text(s[:prev] + hook + s[prev:])
    print('patched pan_kmod.h')

# 2) pan_bo.c (gallium): os_munmap of a BO must not unmap the memory behind a GPU mapping
p, s = read('src/gallium/drivers/panfrost/pan_bo.c')
if 'sweet_drm_bo_unmap' not in s:
    if 'os_munmap(' not in s:
        sys.exit('pan_bo.c: os_munmap( not found')
    inc = list(re.finditer(r'(?m)^#include .*\n', s[:6000]))
    if not inc:
        sys.exit('pan_bo.c: no #include lines near the top')
    at = inc[-1].end()
    hook = ('\n#ifdef __APPLE__\n'
            'int sweet_drm_bo_unmap(void *addr, size_t len);\n'
            '#undef os_munmap\n'
            '#define os_munmap(addr, length) sweet_drm_bo_unmap(addr, length)\n'
            '#endif\n')
    p.write_text(s[:at] + hook + s[at:])
    print('patched pan_bo.c')
