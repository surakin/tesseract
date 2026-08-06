#!/usr/bin/env python3
"""Scoped scan for live MSC (Matrix Spec Change) references in Tesseract's
own source tree. Run as: python3 scan_mscs.py

Exists so the msc-support skill never needs a freehand grep/find over the
whole repo (which would also pick up vendored/third-party code and stale
historical docs). The scan targets below are a hardcoded allow-list of
Tesseract's own source locations; CHANGES.md, FEATURES.md, ROADMAP.md,
STATUS.md, i18n/*.po, and build*/ directories are deliberately never
touched — those are historical/generated, not current-implementation signal.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

# Directories/files to walk, relative to REPO_ROOT. Directories are walked
# recursively; files are read directly.
SCAN_TARGETS = [
    "sdk/src",
    "sdk/Cargo.toml",
    "client/src",
    "client/include",
    "ui/shared",
    "ui/linux-qt/src",
    "ui/linux-gtk/src",
    "ui/windows/src",
    "ui/macos/src",
    "docs/BUILD.md",
    "packaging/arch/PKGBUILD.in",
]

# Path segments to never descend into, even under a scanned directory
# (vendored/third-party code isn't Tesseract's own spec-support signal).
EXCLUDE_DIR_SEGMENTS = {
    "build", "_deps", "third_party", ".git", "node_modules", "target",
}

MSC_RE = re.compile(r"(?i)\bmsc0*(\d{3,5})\b")


def iter_files(target: Path):
    if target.is_file():
        yield target
        return
    for path in target.rglob("*"):
        if not path.is_file():
            continue
        if EXCLUDE_DIR_SEGMENTS & set(path.relative_to(target).parts[:-1]):
            continue
        yield path


def main() -> int:
    hits = {}  # msc number (int) -> list[(relpath, lineno, snippet)]

    for rel in SCAN_TARGETS:
        target = REPO_ROOT / rel
        if not target.exists():
            print(f"warning: scan target missing, skipping: {rel}", file=sys.stderr)
            continue
        for path in iter_files(target):
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            relpath = path.relative_to(REPO_ROOT)
            for lineno, line in enumerate(text.splitlines(), start=1):
                for m in MSC_RE.finditer(line):
                    num = int(m.group(1))
                    snippet = line.strip()
                    if len(snippet) > 160:
                        snippet = snippet[:157] + "..."
                    hits.setdefault(num, []).append((str(relpath), lineno, snippet))

    if not hits:
        print("No MSC references found in scanned source locations.")
        return 0

    for num in sorted(hits):
        matches = hits[num]
        print(f"MSC{num} ({len(matches)} hit{'s' if len(matches) != 1 else ''}):")
        for relpath, lineno, snippet in matches:
            print(f"  {relpath}:{lineno}: {snippet}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
