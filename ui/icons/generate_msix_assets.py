"""Render ui/icons/tesseract.svg into the flat PNG asset set MSIX packages
require: Square44x44Logo, Square150x150Logo, Wide310x150Logo, StoreLogo, and
SplashScreen.

Square assets are rendered directly at their target size via resvg (same
renderer as generate_ico.py). The two non-square assets (Wide310x150Logo,
SplashScreen) paste a square render centered onto a transparent canvas of the
required aspect ratio — resvg's width/height render only stretches to fill,
which would distort the logo on a non-square target, so this step needs an
actual compositing step rather than a second resvg call.

Invoked at build time by cmake/Msix.cmake. To run manually:

    python -m pip install resvg-py pillow
    python ui/icons/generate_msix_assets.py <out_dir> [input.svg]
"""
from __future__ import annotations

import io
import sys
from pathlib import Path

import resvg_py
from PIL import Image

# (filename, canvas width, canvas height, size of the square logo pasted
# centered into that canvas)
ASSETS = [
    ("Square44x44Logo.png", 44, 44, 44),
    ("Square150x150Logo.png", 150, 150, 150),
    ("StoreLogo.png", 50, 50, 50),
    ("Wide310x150Logo.png", 310, 150, 150),
    ("SplashScreen.png", 620, 300, 300),
]


def render_square(svg_path: Path, size: int) -> Image.Image:
    png_bytes = bytes(resvg_py.svg_to_bytes(
        svg_path=str(svg_path), width=size, height=size))
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: generate_msix_assets.py <out_dir> [input.svg]", file=sys.stderr)
        return 1

    out_dir = Path(sys.argv[1])
    svg = (Path(sys.argv[2]) if len(sys.argv) > 2
           else Path(__file__).resolve().parent / "tesseract.svg")
    out_dir.mkdir(parents=True, exist_ok=True)

    for name, width, height, logo_size in ASSETS:
        canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        logo = render_square(svg, logo_size)
        offset = ((width - logo_size) // 2, (height - logo_size) // 2)
        canvas.paste(logo, offset, logo)
        canvas.save(out_dir / name)
        print(f"wrote {out_dir / name} ({width}x{height})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
