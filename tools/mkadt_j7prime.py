#!/usr/bin/env python3
"""Build an entry-only ADT for the Samsung Galaxy J7 Prime (SM-G610F, Exynos 7870).

Derived from mkadt_a5xelte.py (GPL-2.0). The J7 Prime shares the A5's 4+4 Cortex-A53
layout, GICv2, 26 MHz timer, eMMC window and 1080x1920 scanout at 0x67000000; the GIC
moves to 0x104e1000/0x104e2000 (on7xelte device tree, confirmed with the S-Boot probe).
A Mali-T830 G3D resource node is included (inert unless j7gpu=1|2|3). It is not yet an iOS root/device tree.
"""
import argparse
import os
import sys
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


# Userland identity.  The iOS root comes from the IPSW of this device, so the ADT must
# claim the same board: SpringBoard, MobileGestalt and the artwork all look up
# <board>.plist / the /product node of that exact device.  iPhone 8 Plus (1080x1920, Home
# button) is the closest match to the J7 Prime panel; iPhone X is kept for old roots.
IDENTITIES = {
    'd21ap': {'model': 'iPhone10,2', 'type': 'D21', 'board': 'D21AP', 'home_button': 0},
    'd22ap': {'model': 'iPhone10,3', 'type': 'D22', 'board': 'D22AP', 'home_button': 2},
}
RESEARCH = Path(__file__).resolve().parent.parent / 'research' / 'ipsw-x'


def apply_identity(root, ident, product_adt):
    """Claim the iPhone's identity and graft the real device tree's /product node."""
    set_prop(root, 'compatible', mkadt.cstr('samsung,a5xelte') + mkadt.cstr('samsung,on7xelte') +
             mkadt.cstr('samsung,exynos7870') + mkadt.cstr(ident['board']) +
             mkadt.cstr(ident['model']) + mkadt.cstr('AppleARM'))
    set_prop(root, 'model', mkadt.cstr(ident['model']))
    set_prop(root, 'target-type', mkadt.cstr(ident['type']))
    set_prop(root, 'target-sub-type', mkadt.cstr(ident['board']))
    # mkadt.build() may already have added the iPhone X /product when research/ipsw-x/d22ap.adt
    # exists; the one that goes in must belong to this identity.
    root.children = [c for c in root.children if c.prop('name') != mkadt.cstr('product')]
    if product_adt and Path(product_adt).exists():
        props = mkadt.d22ap_product(str(product_adt))
        root.children.append(mkadt.node('product', [('AAPL,phandle', mkadt.u32(0x300))] + props))
        print(f'/product node from {product_adt} ({len(props)} properties)', file=sys.stderr)
    else:
        print(f'warning: {product_adt} missing: no /product node, so Settings/About show a blank '
              f'model name and iPad wording', file=sys.stderr)


def build(board='d21ap', product_adt=None):
    root = mkadt.build(seed=os.urandom(64), qemu=True)
    # 'samsung,a5xelte' stays first (apply_identity): the A5XELTE kernel's platform expert matches on it.
    ident = IDENTITIES[board]
    chosen = child(root, 'chosen')
    buttons = child(root, 'buttons')
    # Samsung's GPA2-5 mechanical key is the iOS Home (Menu) button.
    # The J7 Prime has a mechanical Home key: always a Home-button device (0), whatever the
    # donor board was; iPhone 8 Plus is one too.
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
    # Two A53 clusters of four: logical 0..3 = MPIDR 0..3 (the boot cluster S-Boot starts on),
    # logical 4..7 = MPIDR 0x100..0x103. XNU sees ONE homogeneous cluster (cluster-id and
    # die-cluster-id 0, as in mkadt_herolte.py): describing the second cluster as a second XNU
    # cluster panicked the A5 eight-core trial (ticket lock owned while registering it).
    # The cpus=N boot argument selects a prefix: cpus=1, cpus=4 (boot cluster only), cpus=8.
    for logical in range(1, 8):
        cluster, core = divmod(logical, 4)
        mpidr = (cluster << 8) | core
        cpus.children.append(mkadt.node(f'cpu{logical}', [
            ('device_type', mkadt.cstr('cpu')),
            ('compatible', mkadt.cstr('arm,cortex-a53') + mkadt.cstr('ARM,v8')),
            ('reg', mkadt.u32(mpidr)),
            ('cpu-id', mkadt.u32(logical)),
            ('cluster-id', mkadt.u32(0)),
            ('cluster-type', mkadt.u32(0)),
            ('die-cluster-id', mkadt.u32(0)),
            ('cluster-core-id', mkadt.u32(logical)),
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
    apply_identity(root, ident, product_adt)
    return root


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--board', choices=sorted(IDENTITIES), default='d21ap',
                        help='iPhone identity of the iOS root (d21ap = iPhone 8 Plus, d22ap = iPhone X)')
    parser.add_argument('--product-adt', type=Path, default=None,
                        help='real <board>.adt from the IPSW (default: research/ipsw-x/<board>.adt)')
    args = parser.parse_args()
    blob = adt.serialize(build(args.board, args.product_adt or RESEARCH / f'{args.board}.adt'))
    args.output.write_bytes(blob)
    print(f'{args.output}: {len(blob)} bytes')
