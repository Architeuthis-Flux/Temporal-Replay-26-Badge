#!/usr/bin/env python3
"""Fail CI if firmware.bin does not embed the expected semver string."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware_bin", type=Path)
    parser.add_argument("expected_version", help="semver from firmware/VERSION")
    args = parser.parse_args()

    if not args.firmware_bin.is_file():
        print(f"error: {args.firmware_bin} not found", file=sys.stderr)
        return 1

    blob = args.firmware_bin.read_bytes()
    needle = args.expected_version.encode("ascii")
    if needle not in blob:
        print(
            f"error: {args.firmware_bin} does not contain "
            f"{args.expected_version!r} — release asset would report the "
            f"wrong version after OTA",
            file=sys.stderr,
        )
        return 1

    print(f"[verify_firmware_version] OK: found {args.expected_version!r} in "
          f"{args.firmware_bin}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
