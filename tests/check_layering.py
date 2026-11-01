#!/usr/bin/env python3
"""Layering check for ui/shared: a layer must not include a higher layer.

Usage: check_layering.py ROOT   (ROOT is the ui/shared directory)

Exits 0 when clean; otherwise exits 1 and prints one line per violation.
"""
import re
import sys
from pathlib import Path

# Source dir (relative to ROOT) -> include prefixes it must not use.
RULES = {
    "tk": ["views/", "app/", "platform/"],
    "platform": ["views/", "app/"],
    "views": ["app/"],
}

# Platform-service headers/sources that must live in platform/, not tk/.
FORBIDDEN_IN_TK = ["video_capture", "screen_capture", "location_provider",
                   "video_decode", "single_instance", "gst_"]

EXTENSIONS = {".h", ".hpp", ".cpp", ".mm"}
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s+"([^"]+)"')


def forbidden(path, prefixes):
    # Strip any number of leading "../" so "../app/x.h" is caught too.
    while path.startswith("../"):
        path = path[3:]
    return any(path.startswith(p) for p in prefixes)


def main(argv):
    if len(argv) != 2:
        print("usage: check_layering.py ROOT", file=sys.stderr)
        return 2
    root = Path(argv[1])
    violations = []
    for layer, prefixes in RULES.items():
        base = root / layer
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*")):
            if f.suffix not in EXTENSIONS or not f.is_file():
                continue
            text = f.read_text(encoding="utf-8", errors="replace")
            for n, line in enumerate(text.splitlines(), 1):
                m = INCLUDE_RE.match(line)
                if m and forbidden(m.group(1), prefixes):
                    target = next(p for p in prefixes
                                  if m.group(1).lstrip("./").startswith(p))
                    violations.append(
                        f"{f}:{n}: {layer} must not include "
                        f"{target.rstrip('/')}: {line.strip()}")
    tk_dir = root / "tk"
    if tk_dir.is_dir():
        for f in sorted(tk_dir.rglob("*")):
            if f.is_file() and f.name.startswith(tuple(FORBIDDEN_IN_TK)):
                violations.append(
                    f"{f}: FORBIDDEN_IN_TK: platform service belongs in platform/")
    for v in violations:
        print(v)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
