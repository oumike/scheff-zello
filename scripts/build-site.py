#!/usr/bin/env python3
"""Add the latest release's firmware to the Pages site.

For each board in site/devices.json with a release image in <site>/firmware,
writes an esp-web-tools manifest, <site>/firmware/<env>.json, and
release.json, which the page reads.

The release image is the full merged factory image, which blanks every data
partition up to the app (NVS sits between the partition table and the app), so
flashing it whole would wipe the saved Wi-Fi and Zello settings. The manifest
instead flashes it in parts that skip every data partition; the flasher's
"Erase device" option still wipes everything when chosen.

Usage: build-site.py <gh release view json> <site dir>
"""

import json
import struct
import sys
from pathlib import Path

ENTRY = struct.Struct("<2sBBII16sI")
DATA = 1


def partition_table(image):
    for offset in range(0x1000, 0x10000, 0x1000):
        if image[offset:offset + 2] == b"\xaa\x50":
            entries = []
            for at in range(offset, offset + 0xC00, ENTRY.size):
                magic, kind, _, start, size, _, _ = ENTRY.unpack_from(image, at)
                if magic != b"\xaa\x50":
                    break
                entries.append((kind, start, size))
            return entries
    raise ValueError("no partition table in the image")


def web_parts(image):
    """[(offset, bytes)] covering the image except its data partitions."""
    skip = sorted((start, start + size) for kind, start, size in partition_table(image)
                  if kind == DATA and start < len(image))
    parts, at = [], 0
    for start, end in skip + [(len(image), len(image))]:
        if start > at:
            parts.append((at, image[at:start]))
        at = max(at, end)
    return parts


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    release = json.loads(Path(sys.argv[1]).read_text() or "{}")
    site = Path(sys.argv[2])
    firmware = site / "firmware"
    firmware.mkdir(parents=True, exist_ok=True)
    version = release.get("tagName", "").lstrip("v")
    envs = []
    for device in json.loads((site / "devices.json").read_text()):
        env = device["env"]
        image = firmware / f"scheff-zello-{env}.bin"
        if not image.exists():
            continue
        parts = []
        for offset, data in web_parts(image.read_bytes()):
            name = f"{env}-{offset:#x}.bin"
            (firmware / name).write_bytes(data)
            parts.append({"path": name, "offset": offset})
        image.unlink()
        manifest = {
            "name": f"Scheff for Zello ({device['name']})",
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": device["chip"], "parts": parts}],
        }
        (firmware / f"{env}.json").write_text(json.dumps(manifest, indent=2))
        envs.append(env)
        print(env, ", ".join(f"{p['offset']:#x}" for p in parts))
    (firmware / "release.json").write_text(json.dumps(
        {"version": version, "url": release.get("url", ""), "envs": envs}))
    print(f"version {version or 'none'}: {', '.join(envs) or 'no images'}")


if __name__ == "__main__":
    main()
