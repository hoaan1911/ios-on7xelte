#!/usr/bin/env python3
"""List local IPSW identities and root-image members without extracting them."""

import argparse
import plistlib
import sys
import zipfile
from pathlib import Path


ROOT_KEYS = {
    "OS": ("OS",),
    "SystemOS": ("SystemOS", "Cryptex1,SystemOS"),
    "AppOS": ("AppOS", "Cryptex1,AppOS"),
    "DeviceTree": ("DeviceTree",),
}


def member_path(entry):
    if not isinstance(entry, dict):
        return None
    info = entry.get("Info")
    return info.get("Path") if isinstance(info, dict) else None


def inspect(path, device_class=None):
    with zipfile.ZipFile(path) as archive:
        names = set(archive.namelist())
        if "BuildManifest.plist" not in names:
            raise ValueError("BuildManifest.plist is missing")
        manifest = plistlib.loads(archive.read("BuildManifest.plist"))
        print(f"IPSW: {path.name}")
        print(f"iOS: {manifest.get('ProductVersion', '?')} ({manifest.get('ProductBuildVersion', '?')})")
        print("Supported products:", ", ".join(manifest.get("SupportedProductTypes", [])) or "unknown")

        matches = 0
        for index, identity in enumerate(manifest.get("BuildIdentities", [])):
            info = identity.get("Info", {})
            board = info.get("DeviceClass", "?")
            if device_class and board.casefold() != device_class.casefold():
                continue
            matches += 1
            print(f"\nIdentity {index}: {board} — {info.get('Variant', '?')}")
            items = identity.get("Manifest", {})
            for label, keys in ROOT_KEYS.items():
                key = next((key for key in keys if member_path(items.get(key))), None)
                member = member_path(items[key]) if key else None
                if not member:
                    print(f"  {label}: absent")
                    continue
                name = label if key == label else f"{label} [{key}]"
                if member in names:
                    size = archive.getinfo(member).file_size
                    print(f"  {name}: {member} ({size} bytes)")
                else:
                    print(f"  {name}: {member} (not found in archive)")
        if not matches:
            raise ValueError(f"no build identity for {device_class or 'this IPSW'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipsw", type=Path, help="local IPSW file; read only")
    parser.add_argument("--device-class", help="filter using the BuildManifest DeviceClass")
    args = parser.parse_args()
    try:
        inspect(args.ipsw, args.device_class)
    except (OSError, ValueError, zipfile.BadZipFile, plistlib.InvalidFileException) as exc:
        parser.exit(1, f"IPSW inspection failed: {exc}\n")


if __name__ == "__main__":
    main()
