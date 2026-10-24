#!/usr/bin/env python3
"""Build the herolte (SM-G930F, Exynos 8890) ADT for the HEROLTE XNU config.

Derived from mkadt_a5xelte.py. Addresses come from exynos8890.dtsi and the
phone's own BOOT DTB (see docs/herolte-bringup.md); none is device-verified
under XNU yet. Storage is the Exynos 8890 UFS (/herolte-ufs); no touch or
GPU nodes: the A5 drivers for those target Exynos 7580 hardware.
"""
import argparse
import os
from pathlib import Path

import adt
import mkadt

DRAM_BASE = 0x90000000      # isolated XNU window, free in the herolte DTB
DRAM_SIZE = 0x20000000      # 512 MiB, ends below TIMA at 0xb1000000
# --bank2: the second DRAM bank (DTB memory@880000000), 2 GiB with no
# reserved-memory node in it; needs the bank-aware shim/herolte loader.
BANK2_BASE, BANK2_SIZE = 0x880000000, 0x80000000
SOC_BASE = 0x10000000       # arm-io ranges base; child regs are relative
FB_BASE, FB_WIDTH, FB_HEIGHT = 0xe2a00000, 1440, 2560


def set_prop(node, name, value):
    for i, (key, flags, _) in enumerate(node.props):
        if key == name:
            node.props[i] = (key, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for candidate in node.children:
        if candidate.prop('name') == mkadt.cstr(name):
            return candidate
    raise ValueError(f'missing donor ADT node {name}')


def build(canvas=(FB_WIDTH, FB_HEIGHT), dram=(DRAM_BASE, DRAM_SIZE)):
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    # The userland is the iPhone X IPSW (iPhone10,3 / D22AP), like the donor:
    # compatible[0] is hw.model/hw.target, model is hw.machine. The donor's
    # /buttons stays (home-button-type 2: gesture navigation, no Home key).
    # 'samsung,a5xelte' keeps the shared SweetPlatformExpert IONameMatch.
    set_prop(root, 'compatible', mkadt.cstr('D22AP') + mkadt.cstr('iPhone10,3') +
             mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,herolte') +
             mkadt.cstr('samsung,exynos8890') + mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    set_prop(chosen, 'dram-base', mkadt.u64(dram[0]))
    set_prop(chosen, 'dram-size', mkadt.u64(dram[1]))
    set_prop(chosen, 'firmware-version', mkadt.cstr('herolte-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('herolte-entry'))
    # MobileGestalt derives the UDID from these; without them it waits for a
    # Bluetooth/Wi-Fi driver forever, and LaunchServices never finishes seeding
    # the app database (SpringBoard stays on the boot logo). Synthetic,
    # locally administered addresses, not the phone's.
    for name, last in (('wifi0', 1), ('bluetooth0', 2), ('ethernet0', 3)):
        set_prop(chosen, f'mac-address-{name}', bytes((0x02, 0x48, 0x45, 0x52, 0x4f, last)))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    # S-Boot starts on the Cortex-A53 "Apollo" cluster: MPIDR 0x100..0x103
    # (logical 0..3); the Mongoose cluster, MPIDR 0..3, is logical 4..7.
    # One XNU cluster (homogeneous SMP); separate L2 ids. cpus=4 keeps the
    # A53s only.
    set_prop(cpu0, 'compatible', mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8'))
    set_prop(cpu0, 'reg', mkadt.u32(0x100))
    set_prop(cpu0, 'timebase-frequency', mkadt.u32(26000000))
    for logical in range(1, 8):
        mongoose = logical >= 4
        core = logical - 4 if mongoose else logical
        cpus.children.append(mkadt.node(f'cpu{logical}', [
            ('device_type', mkadt.cstr('cpu')),
            # SweetCPU matches IONameMatch 'arm,cortex-a53' (KernelConfigTables).
            ('compatible', (mkadt.cstr('samsung,mongoose') if mongoose else b'') +
                           mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8')),
            ('reg', mkadt.u32(core if mongoose else 0x100 | core)),
            ('cpu-id', mkadt.u32(logical)),
            ('cluster-id', mkadt.u32(0)),
            ('cluster-type', mkadt.u32(0)),
            ('die-cluster-id', mkadt.u32(0)),
            ('cluster-core-id', mkadt.u32(logical)),
            ('l2-cache-id', mkadt.u32(1 if mongoose else 0)),
            ('l3-cache-id', mkadt.u32(0)),
            ('state', mkadt.cstr('stopped')),
            ('timebase-frequency', mkadt.u32(26000000)),
            ('clock-frequency', mkadt.u32(2600000000 if mongoose else 1586000000)),
            ('bus-frequency', mkadt.u32(100000000)),
            ('memory-frequency', mkadt.u32(1794000000)),
            ('peripheral-frequency', mkadt.u32(26000000)),
            ('fixed-frequency', mkadt.u32(26000000)),
        ]))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('herolte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos8890-io') +
             mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, SOC_BASE, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    # GIC-400: distributor 0x11001000, CPU interface 0x11002000.
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        ('virtual-timer-intid', mkadt.u32(27)),
        ('reg', mkadt.u64(0x11001000 - SOC_BASE, 0x1000, 0x11002000 - SOC_BASE, 0x1000)),
    ]))
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        # AppleCLCD (SweetFramebuffer.cpp) matches 'samsung,a5xelte-display'
        # in KernelConfigTables.cpp; without it SpringBoard sees no main display.
        ('compatible', mkadt.cstr('samsung,a5xelte-display') +
         mkadt.cstr('samsung,herolte-display')),
        # MobileGestalt's main-screen size: backboardd sizes its surfaces
        # from it, not from the framebuffer's GetDisplaySize. With
        # sweetfb_scale=1 it must be the scaled 960x1706 (--canvas).
        ('canvas-width', mkadt.u32(canvas[0])),
        ('canvas-height', mkadt.u32(canvas[1])),
    ]))
    root.children = [n for n in root.children if n.prop('name') not in
                     (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    # Exynos 8890 UFSHCI (exynos8890.dtsi ufs@0x155A0000): standard HCI at
    # +0, vendor HCI +0x100, UNIPRO +0x1000, UFS protector +0x2000. Absolute
    # address, read by bsd/dev/herolte_ufs.c (ufsprobe=1, rd=ufsroot0).
    root.children.append(mkadt.node('herolte-ufs', [
        ('reg', mkadt.u64(0x155A0000, 0x3000)),
    ]))
    # pe_serial.c looks this node up by name for the kprintf console.
    root.children.append(mkadt.node('a5xelte-fbcon', [
        ('reg', mkadt.u64(FB_BASE, FB_WIDTH * FB_HEIGHT * 4)),
        ('width', mkadt.u32(FB_WIDTH)),
        ('height', mkadt.u32(FB_HEIGHT)),
        ('stride', mkadt.u32(FB_WIDTH * 4)),
    ]))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--canvas', default=f'{FB_WIDTH}x{FB_HEIGHT}',
                        help='presented display WxH (960x1706 with sweetfb_scale=1)')
    parser.add_argument('--bank2', action='store_true',
                        help='XNU RAM = the second bank instead of the 512 MiB window')
    parser.add_argument('--bank2-mb', type=int, default=BANK2_SIZE >> 20,
                        help='MiB of the second bank to use; 1536 boots (with boot-arg '
                             'vm_compression_limit=98304), 2048 panics in write_pte_fast')
    args = parser.parse_args()
    canvas = tuple(int(v) for v in args.canvas.lower().split('x'))
    dram = (BANK2_BASE, min(args.bank2_mb << 20, BANK2_SIZE)) if args.bank2 else (DRAM_BASE, DRAM_SIZE)
    blob = adt.serialize(build(canvas, dram))
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
