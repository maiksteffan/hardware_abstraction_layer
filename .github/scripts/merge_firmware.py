#!/usr/bin/env python3
"""Merge a PlatformIO ESP32 build into a single, 0x0-flashable binary.

Usage: merge_firmware.py <build_dir> <output.bin> [chip]

The Raspberry Pi flashes the resulting image with `write_flash 0x0`, so this
merges the bootloader, partition table, boot_app0 and application into one file
based at 0x0. The bootloader flash offset depends on the chip (0x0 for the
S3/C-series, 0x1000 for the classic ESP32 / S2), so it is derived from `chip`
(default: esp32s3, the firmware's default board).

PlatformIO's Arduino builds do NOT emit flasher_args.json (that is an ESP-IDF
artifact), and boot_app0.bin lives in the framework package rather than the
build directory — both handled here.
"""

import os
import subprocess
import sys
from pathlib import Path

# Bootloader flash offset per chip family.
BOOTLOADER_OFFSET = {
    "esp32": "0x1000",
    "esp32s2": "0x1000",
    "esp32s3": "0x0",
    "esp32c3": "0x0",
    "esp32c6": "0x0",
    "esp32h2": "0x0",
}


def find_boot_app0(build_dir: Path) -> str:
    """boot_app0.bin ships in the framework package, not the build dir."""
    local = build_dir / "boot_app0.bin"
    if local.exists():
        return str(local)
    search_roots = [
        Path(os.path.expanduser("~")) / ".platformio" / "packages",
        Path("/home/runner/.platformio/packages"),
    ]
    for root in search_roots:
        if root.exists():
            hits = list(root.rglob("boot_app0.bin"))
            if hits:
                return str(hits[0])
    raise FileNotFoundError("boot_app0.bin not found in build dir or PlatformIO packages")


def main() -> int:
    build_dir = Path(sys.argv[1])
    output = Path(sys.argv[2]).resolve()
    chip = sys.argv[3] if len(sys.argv) > 3 else "esp32s3"
    boot_offset = BOOTLOADER_OFFSET.get(chip, "0x0")

    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    firmware = build_dir / "firmware.bin"
    for f in (bootloader, partitions, firmware):
        if not f.exists():
            raise FileNotFoundError(f"missing build artifact: {f}")
    boot_app0 = find_boot_app0(build_dir)

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", chip,
        "merge_bin",
        "-o", str(output),
        boot_offset, str(bootloader),
        "0x8000", str(partitions),
        "0xe000", boot_app0,
        "0x10000", str(firmware),
    ]
    print(f"chip={chip} bootloader@{boot_offset}")
    print("Running:", " ".join(cmd))
    subprocess.run(cmd, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
