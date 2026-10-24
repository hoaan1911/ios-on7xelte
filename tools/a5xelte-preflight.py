#!/usr/bin/env python3
"""Read-only a5xelte XNU preflight. Run on the existing Linux system.

Prints board facts and unknowns; it neither opens block devices nor writes files.
Keep the output private until reviewed for device-specific identifiers.
"""
import argparse
import json
from pathlib import Path


def prop(path):
    try:
        return path.read_bytes()
    except (FileNotFoundError, PermissionError, IsADirectoryError):
        return None


def strings(data):
    return [x.decode("ascii", "replace") for x in data.split(b"\0") if x] if data else []


def cells(data, count):
    if data is None or len(data) != 4 * count:
        return None
    return [int.from_bytes(data[i:i + 4], "big") for i in range(0, len(data), 4)]


def reg(data, address_cells, size_cells):
    width = 4 * (address_cells + size_cells)
    if not data or not width or len(data) % width:
        return None
    out = []
    for offset in range(0, len(data), width):
        words = cells(data[offset:offset + width], address_cells + size_cells)
        address = 0
        size = 0
        for word in words[:address_cells]:
            address = (address << 32) | word
        for word in words[address_cells:]:
            size = (size << 32) | word
        out.append({"base": hex(address), "size": hex(size)})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dt-root", type=Path, default=Path("/proc/device-tree"))
    ap.add_argument("--cpu-reg", type=Path, default=Path(
        "/sys/devices/system/cpu/cpu0/regs/identification/id_aa64mmfr0_el1"),
        help="Only use a trusted raw EL1 register value; Linux may hide this field from EL0")
    args = ap.parse_args()
    dt = args.dt_root
    ac_raw = cells(prop(dt / "#address-cells"), 1)
    sc_raw = cells(prop(dt / "#size-cells"), 1)
    ac, sc = (ac_raw or [2])[0], (sc_raw or [2])[0]
    if not (1 <= ac <= 2 and 1 <= sc <= 2):
        raise SystemExit("unsupported root address/size cells")
    report = {
        "dt_root": str(dt),
        "compatible": strings(prop(dt / "compatible")),
        "memory": [],
        "gic": [],
        "timer": [],
        "cpu_count": 0,
        "id_aa64mmfr0_el1": None,
        "tgran16": None,
        "supports_16k_granule": None,
    }
    if not dt.is_dir():
        raise SystemExit(f"missing device tree: {dt}")
    for node in dt.rglob("*"):
        if not node.is_dir():
            continue
        compatible = strings(prop(node / "compatible"))
        if node.name.startswith("memory@") or strings(prop(node / "device_type")) == ["memory"]:
            report["memory"].append({"node": node.name, "reg": reg(prop(node / "reg"), ac, sc)})
        if prop(node / "interrupt-controller") is not None and any("gic" in item for item in compatible):
            report["gic"].append({"node": str(node.relative_to(dt)), "compatible": compatible,
                                  "reg": reg(prop(node / "reg"), ac, sc) if node.parent == dt else None})
        if "arm,armv8-timer" in compatible or "arm,armv7-timer" in compatible:
            freq = cells(prop(node / "clock-frequency"), 1)
            report["timer"].append({"node": str(node.relative_to(dt)), "clock_hz": freq[0] if freq else None,
                                    "interrupts_be32": (prop(node / "interrupts") or b"").hex()})
    cpus = dt / "cpus"
    if cpus.is_dir():
        report["cpu_count"] = sum(1 for node in cpus.iterdir()
                                  if node.is_dir() and node.name.startswith("cpu@"))
    # A userspace MRS can be Linux-emulated with TGran16 masked to zero.
    # Treat this file as meaningful only if its producer captured raw EL1 data.
    raw = prop(args.cpu_reg)
    if raw:
        try:
            mmfr0 = int(raw.strip(), 0)
        except ValueError:
            pass
        else:
            report["id_aa64mmfr0_el1"] = hex(mmfr0)
            report["tgran16"] = (mmfr0 >> 20) & 0xf
            report["supports_16k_granule"] = report["tgran16"] == 1
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
