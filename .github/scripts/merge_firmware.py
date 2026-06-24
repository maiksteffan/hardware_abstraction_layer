#!/usr/bin/env python3
"""Merge a PlatformIO build into a single flashable binary.

Reads `flasher_args.json` from a PlatformIO build directory and runs
`esptool merge_bin` with the exact chip, flash flags and offset->file map that
PlatformIO produced. This keeps the merge correct regardless of the board
(ESP32 WROOM vs ESP32-S3) without hardcoding offsets.

Usage: merge_firmware.py <build_dir> <output.bin>
"""

import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    build_dir = Path(sys.argv[1])
    output = Path(sys.argv[2]).resolve()

    args = json.loads((build_dir / "flasher_args.json").read_text())
    chip = args.get("extra_esptool_args", {}).get("chip", "esp32")
    write_args = args.get("write_flash_args", [])
    flash_files = args["flash_files"]  # {offset: filename}

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", chip,
        "merge_bin",
        "-o", str(output),
        *write_args,
    ]
    for offset, filename in flash_files.items():
        cmd += [offset, filename]

    print("Running:", " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=build_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
