#!/usr/bin/env python3
"""Copy a board's merged factory image to dist/ under its release name.

PlatformIO writes firmware.factory.bin: bootloader, partition table and app
merged into one image that flashes at 0x0, which is what the web flasher and
`esptool.py write_flash 0x0` expect. This checks that the bootloader and the
partition table sit where the board's sdkconfig says, so a layout change can't
silently publish an image that won't boot.

Usage: package-firmware.py <env> <pio build dir for env> <out dir>
"""

import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sdkconfig_hex(env, key):
    text = (ROOT / f"sdkconfig.{env}").read_text()
    match = re.search(rf"^CONFIG_{key}=(0x[0-9a-fA-F]+)$", text, re.M)
    if not match:
        sys.exit(f"CONFIG_{key} not found in sdkconfig.{env}")
    return int(match.group(1), 16)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    env, build_dir, out_dir = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    image = build_dir / "firmware.factory.bin"
    data = image.read_bytes()

    bootloader = sdkconfig_hex(env, "BOOTLOADER_OFFSET_IN_FLASH")
    partitions = sdkconfig_hex(env, "PARTITION_TABLE_OFFSET")
    if data[bootloader:bootloader + 1] != b"\xe9":
        sys.exit(f"{image}: no bootloader image at {bootloader:#x}")
    if data[partitions:partitions + 2] != b"\xaa\x50":
        sys.exit(f"{image}: no partition table at {partitions:#x}")

    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"scheff-zello-{env}.bin"
    shutil.copyfile(image, target)
    print(f"{target} ({len(data) // 1024} KB, bootloader {bootloader:#x})")


if __name__ == "__main__":
    main()
