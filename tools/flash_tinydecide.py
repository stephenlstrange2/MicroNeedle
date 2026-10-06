#!/usr/bin/env python3
"""Flash the packed TinyDecide image into MicroNeedle's model partition."""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / ".models" / "tinydecide-esp32.bin"
OFFSET = "0x310000"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="serial port, for example /dev/ttyACM0")
    parser.add_argument("--baud", default="921600")
    args = parser.parse_args()
    if not IMAGE.exists():
        print("Model image is missing; run: python3 tools/setup_tinydecide.py", file=sys.stderr)
        return 1

    if importlib.util.find_spec("esptool") is not None:
        command = [sys.executable, "-m", "esptool"]
    else:
        bundled = Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
        if not bundled.exists():
            print("esptool is unavailable; build once with PlatformIO or install esptool", file=sys.stderr)
            return 1
        command = [sys.executable, str(bundled)]
    command += ["--chip", "esp32s3"]
    if args.port:
        command += ["--port", args.port]
    command += ["--baud", args.baud, "write_flash", OFFSET, str(IMAGE)]
    print("+", " ".join(command))
    try:
        return subprocess.run(command, check=False).returncode
    except OSError as error:
        print(f"Unable to run esptool: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
