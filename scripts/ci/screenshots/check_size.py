"""Fail unless every PNG in a directory is 1440x900 (2880x1800 on a 2x Mac).

Usage: check_size.py DIR
"""

import pathlib
import struct
import sys

ALLOWED = {(1440, 900), (2880, 1800)}


def main() -> int:
    pngs = sorted(pathlib.Path(sys.argv[1]).glob("*.png"))
    if not pngs:
        print("no screenshots found", file=sys.stderr)
        return 1
    bad = 0
    for png in pngs:
        width, height = struct.unpack(">II", png.read_bytes()[16:24])
        ok = (width, height) in ALLOWED
        print(f"{png.name}: {width}x{height}{'' if ok else '  <- wrong size'}")
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
