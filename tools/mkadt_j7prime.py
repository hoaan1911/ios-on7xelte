#!/usr/bin/env python3
"""Build an entry-only ADT for the Samsung Galaxy J7 Prime (SM-G610F, Exynos 7870).

Derived from mkadt_a5xelte.py (GPL-2.0). The J7 Prime shares the A5's 4+4 Cortex-A53
layout, GICv2, 26 MHz timer, eMMC window and 1080x1920 scanout at 0x67000000; the GIC
moves to 0x104e1000/0x104e2000 (on7xelte device tree, confirmed with the S-Boot probe).
A Mali-T830 G3D resource node is included (inert unless j7gpu=1|2|3). It is not yet an iOS root/device tree.
"""
import argparse
import os
from pathlib import Path

import adt
import mkadt


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


def build():
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    # 'samsung,a5xelte' stays first: the A5XELTE kernel's platform expert matches on it.
    set_prop(root, 'compatible', mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,on7xelte') +
             mkadt.cstr('samsung,exynos7870') + mkadt.cstr('AppleARM'))
    chosen = child(root, 'chosen')
    buttons = child(root, 'buttons')
    # Samsung's GPA2-5 mechanical key is the iOS Home (Menu) button.
    set_prop(buttons, 'home-button-type', mkadt.u32(0))
    set_prop(buttons, 'button-names', b''.join(mkadt.cstr(name) for name in
                                       ('volup', 'voldown', 'hold', 'home')))
    set_prop(chosen, 'dram-base', mkadt.u64(0x80000000))
    set_prop(chosen, 'dram-size', mkadt.u64(0x20000000))
    set_prop(chosen, 'firmware-version', mkadt.cstr('a5xelte-entry'))
    set_prop(chosen, 'system-firmware-version', mkadt.cstr('a5xelte-entry'))
    cpus = child(root, 'cpus')
    cpu0 = child(cpus, 'cpu0')
    set_prop(cpu0, 'compatible', mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8'))
    set_prop(cpu0, 'reg', mkadt.u32(0))
    set_prop(cpu0, 'timebase-frequency', mkadt.u32(26000000))
    # The working Exynos7580 Linux DT has two A53 clusters of four cores:
    # MPIDR 0..3 in cluster 0 and 0x100..0x103 in cluster 1. The cpus=N
    # boot argument still selects a prefix, so cpus=1 and cpus=2 remain valid.
    for logical in range(1, 8):
        cluster, core = divmod(logical, 4)
        mpidr = (cluster << 8) | core
        cpus.children.append(mkadt.node(f'cpu{logical}', [
            ('device_type', mkadt.cstr('cpu')),
            ('compatible', mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8')),
            ('reg', mkadt.u32(mpidr)),
            ('cpu-id', mkadt.u32(logical)),
            ('cluster-id', mkadt.u32(cluster)),
            ('cluster-type', mkadt.u32(0)),
            ('die-cluster-id', mkadt.u32(cluster)),
            ('cluster-core-id', mkadt.u32(core)),
            ('l2-cache-id', mkadt.u32(cluster)),
            ('l3-cache-id', mkadt.u32(0)),
            ('state', mkadt.cstr('stopped')),
            ('timebase-frequency', mkadt.u32(26000000)),
            ('clock-frequency', mkadt.u32(1600000000)),
            ('bus-frequency', mkadt.u32(100000000)),
            ('memory-frequency', mkadt.u32(1866000000)),
            ('peripheral-frequency', mkadt.u32(26000000)),
            ('fixed-frequency', mkadt.u32(26000000)),
        ]))
    arm_io = child(root, 'arm-io')
    set_prop(arm_io, 'device_type', mkadt.cstr('a5xelte-io'))
    set_prop(arm_io, 'compatible', mkadt.cstr('samsung,exynos7870-io') +
             mkadt.cstr('arm-io'))
    set_prop(arm_io, 'ranges', mkadt.u64(0, 0x10000000, 0xf0000000))
    arm_io.children = [n for n in arm_io.children if n.prop('name') == mkadt.cstr('wdt')]
    arm_io.children.append(mkadt.node('gic', [
        ('compatible', mkadt.cstr('arm,cortex-a15-gic')),
        ('interrupt-controller', mkadt.cstr('master')),
        ('virtual-timer-intid', mkadt.u32(27)),
        ('reg', mkadt.u64(0x04e1000, 0x1000, 0x04e2000, 0x2000)),   # 0x104e1000 / 0x104e2000
    ]))
    # The Linux DECON handoff and Samsung loader already scan out this buffer.
    # Publish its iOS display service without importing the SWEET panel size.
    arm_io.children.append(mkadt.node('disp0', [
        ('device_type', mkadt.cstr('display-subsystem')),
        ('compatible', mkadt.cstr('samsung,a5xelte-display')),
        ('canvas-width', mkadt.u32(1080)),
        ('canvas-height', mkadt.u32(1920)),
    ]))
    # Mali-T830 resource nub (Exynos7870 G3D). The 7580 compatible string is kept
    # second so the existing A5XELTEGPU matching entry still starts the service;
    # it stays off unless j7gpu=1|2|3 is on the boot command line. Addresses are
    # relative to the arm-io range base 0x10000000. No interrupts property, as on
    # the A5 (no GIC-backed handler exists yet).
    arm_io.children.append(mkadt.node('gpu', [
        ('device_type', mkadt.cstr('gpu')),
        ('compatible', mkadt.cstr('samsung,exynos7870-mali') +
         mkadt.cstr('samsung,exynos7580-mali')),
        ('reg', mkadt.u64(0x01400000, 0x5000)),
        ('cmu-reg', mkadt.u64(0x01460000, 0x1000)),
        ('pmu-reg', mkadt.u64(0x00480000, 0x10000)),
    ]))
    root.children = [n for n in root.children if n.prop('name') not in
                     (mkadt.cstr('sweet-fbcon'), mkadt.cstr('sweet-ufs'))]
    root.children.append(mkadt.node('a5xelte-fbcon', [
        ('reg', mkadt.u64(0x67000000, 1080 * 1920 * 4)),
        ('width', mkadt.u32(1080)),
        ('height', mkadt.u32(1920)),
        ('stride', mkadt.u32(4320)),
    ]))
    root.children.append(mkadt.node('a5xelte-emmc', [
        ('reg', mkadt.u64(0x13540000, 0x2000)),
    ]))
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    blob = adt.serialize(build())
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
