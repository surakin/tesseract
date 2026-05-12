"""Render ui/icons/tesseract.svg to a multi-resolution Windows .ico.

Rendering is done by resvg (Rust, via resvg-py). The ICO container itself is
packed with stdlib `struct` — Windows Vista+ accepts PNG-encoded frames at
every size, so each rendered PNG can be embedded verbatim without a BMP step
(and without pulling in Pillow).

Invoked at build time by ui/windows/CMakeLists.txt. To run manually:

    python -m pip install resvg-py
    python ui/icons/generate_ico.py [output.ico]
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

import resvg_py

# Sizes Windows commonly asks for: tray (16), titlebar (16/20/24), taskbar
# (24/32/40), start menu / file manager (48/64), large icons / jump lists
# (96/128), and Explorer "extra large" (256).
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def render_png(svg_path: Path, size: int) -> bytes:
    return bytes(resvg_py.svg_to_bytes(
        svg_path=str(svg_path),
        width=size,
        height=size,
    ))


def pack_ico(frames: list[tuple[int, bytes]]) -> bytes:
    """Pack (size, png_bytes) frames into a Windows ICO byte stream."""
    n = len(frames)
    header = struct.pack("<HHH", 0, 1, n)  # reserved=0, type=1 (icon), count

    entries = bytearray()
    images = bytearray()
    offset = 6 + 16 * n  # frames start after the directory
    for size, png in frames:
        # In ICONDIRENTRY, width/height = 0 means 256 (or larger).
        w = h = 0 if size >= 256 else size
        entries += struct.pack(
            "<BBBBHHII",
            w, h,
            0,  # colorCount (0 for >8bpp)
            0,  # reserved
            1,  # planes
            32, # bitCount
            len(png),
            offset,
        )
        images += png
        offset += len(png)

    return bytes(header) + bytes(entries) + bytes(images)


def main() -> int:
    here = Path(__file__).resolve().parent
    svg = here / "tesseract.svg"

    if len(sys.argv) > 1:
        out = Path(sys.argv[1])
    else:
        out = here.parent / "windows" / "Tesseract.ico"
    out.parent.mkdir(parents=True, exist_ok=True)

    frames = [(s, render_png(svg, s)) for s in SIZES]
    out.write_bytes(pack_ico(frames))
    print(f"wrote {out} ({out.stat().st_size} bytes, sizes={SIZES})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
