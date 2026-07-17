#!/usr/bin/env python3
"""Merge a PlatformIO ESP32 build into one binary flashable at offset 0x0."""

import os
import subprocess
import sys
from pathlib import Path


BOOTLOADER_OFFSET = {
    "esp32": "0x1000",
    "esp32s2": "0x1000",
    "esp32s3": "0x0",
    "esp32c3": "0x0",
    "esp32c6": "0x0",
    "esp32h2": "0x0",
}


def find_boot_app0(build_dir: Path) -> str:
    """Find boot_app0.bin in the build or installed PlatformIO packages."""
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

    raise FileNotFoundError(
        "boot_app0.bin not found in the build directory or PlatformIO packages"
    )


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit("usage: merge_firmware.py <build_dir> <output.bin> [chip]")

    build_dir = Path(sys.argv[1])
    output = Path(sys.argv[2]).resolve()
    chip = sys.argv[3] if len(sys.argv) > 3 else "esp32s3"
    boot_offset = BOOTLOADER_OFFSET.get(chip, "0x0")

    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    firmware = build_dir / "firmware.bin"
    for artifact in (bootloader, partitions, firmware):
        if not artifact.exists():
            raise FileNotFoundError(f"missing build artifact: {artifact}")

    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        chip,
        "merge_bin",
        "-o",
        str(output),
        boot_offset,
        str(bootloader),
        "0x8000",
        str(partitions),
        "0xe000",
        find_boot_app0(build_dir),
        "0x10000",
        str(firmware),
    ]
    print(f"chip={chip} bootloader@{boot_offset}")
    print("Running:", " ".join(command))
    subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
