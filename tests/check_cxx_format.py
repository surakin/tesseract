#!/usr/bin/env python3
"""C++ formatting check: every tracked C/C++/ObjC++ file must match .clang-format.

Usage: check_cxx_format.py ROOT [--clang-format PATH] [--jobs N] [--list]

  ROOT           repository root (holds .clang-format and tests/clang-format-version)
  --clang-format clang-format binary (default: $CLANG_FORMAT, else `clang-format`)
  --jobs N       parallel clang-format processes (default: CPU count)
  --list         print every offending file instead of the first 50

Files come from `git ls-files` (a directory walk when git is unavailable),
minus third_party/, build/ and anything matched by ROOT/.clang-format-ignore
(simple gitignore-like globs, one per line, '#' comments).

The required clang-format version is pinned in tests/clang-format-version;
output differs between major versions, so only the major must match.

Exits 0 when every file is formatted, 1 when some need formatting, and 2 for
usage errors or a missing / wrong-version clang-format. With
--skip-wrong-version, a clang-format of another major version exits 77 instead,
which ctest reports as skipped (the CI style job runs without it).
"""
import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm", ".m"}
EXCLUDED_DIRS = ("third_party/", "build/")
LIST_LIMIT = 50


def load_ignore_patterns(root):
    path = root / ".clang-format-ignore"
    if not path.is_file():
        return []
    patterns = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            patterns.append(line)
    return patterns


def ignored(rel, patterns):
    """gitignore-like: 'dir/' or 'name' matches at any depth, '/x' only at ROOT."""
    for pat in patterns:
        anchored = pat.startswith("/")
        pat = pat.strip("/")
        candidates = [rel] if anchored else [rel, *("/".join(rel.split("/")[i:])
                                                    for i in range(1, rel.count("/") + 1))]
        parts = rel.split("/")
        for cand in candidates:
            if fnmatch.fnmatch(cand, pat) or fnmatch.fnmatch(cand, pat + "/*"):
                return True
        if not anchored and "/" not in pat and any(fnmatch.fnmatch(x, pat) for x in parts):
            return True
    return False


def list_files(root):
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                             check=True, capture_output=True).stdout
        names = [n for n in out.decode("utf-8", "surrogateescape").split("\0") if n]
    except (OSError, subprocess.CalledProcessError):
        names = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != ".git" and not d.startswith("build")]
            for f in filenames:
                names.append(os.path.relpath(os.path.join(dirpath, f), root)
                             .replace(os.sep, "/"))
    patterns = load_ignore_patterns(root)
    result = []
    for rel in sorted(names):
        if Path(rel).suffix not in EXTENSIONS:
            continue
        if rel.startswith(EXCLUDED_DIRS) or "/third_party/" in rel:
            continue
        if ignored(rel, patterns):
            continue
        if not (root / rel).is_file():  # deleted in the working tree
            continue
        result.append(rel)
    return result


SKIP_EXIT = 77  # ctest SKIP_RETURN_CODE


def check_version(binary, root):
    """Returns "ok", "wrong" (runs, but not the pinned major) or "error"."""
    required = (root / "tests" / "clang-format-version").read_text().strip()
    try:
        out = subprocess.run([binary, "--version"], check=True,
                             capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"error: cannot run clang-format ({binary}): {e}\n"
              f"Install the pinned version: pip install clang-format=={required}",
              file=sys.stderr)
        return "error"
    m = re.search(r"version\s+(\d+)\.(\d+)(?:\.(\d+))?", out)
    if not m:
        print(f"error: cannot parse `{binary} --version` output: {out.strip()!r}",
              file=sys.stderr)
        return "error"
    have = m.group(0).split()[-1]
    if m.group(1) != required.split(".")[0]:
        print(f"error: clang-format {have} found ({binary}), but major version "
              f"{required.split('.')[0]} is required (pinned {required} in "
              f"tests/clang-format-version); formatting differs between major "
              f"versions.\nInstall it with: pip install clang-format=={required}",
              file=sys.stderr)
        return "wrong"
    return "ok"


def needs_format(binary, root, rel):
    r = subprocess.run([binary, "--dry-run", "--Werror",
                        f"--style=file:{root / '.clang-format'}", rel],
                       cwd=root, capture_output=True)
    return r.returncode != 0


def main(argv):
    ap = argparse.ArgumentParser(description="Check C++ formatting with clang-format.")
    ap.add_argument("root")
    ap.add_argument("--clang-format", dest="clang_format")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--skip-wrong-version", action="store_true",
                    help=f"exit {SKIP_EXIT} (ctest: skipped) instead of failing "
                         "when clang-format isn't the pinned major version")
    args = ap.parse_args(argv[1:])

    root = Path(args.root).resolve()
    if not (root / ".clang-format").is_file():
        print(f"error: {root}/.clang-format not found", file=sys.stderr)
        return 2
    if not (root / "tests" / "clang-format-version").is_file():
        print(f"error: {root}/tests/clang-format-version not found", file=sys.stderr)
        return 2
    binary = args.clang_format or os.environ.get("CLANG_FORMAT") or "clang-format"
    if os.sep not in binary and shutil.which(binary) is None:
        print(f"error: {binary} not found on PATH", file=sys.stderr)
        return 2
    version = check_version(binary, root)
    if version == "wrong" and args.skip_wrong_version:
        print("skipping: wrong clang-format version", file=sys.stderr)
        return SKIP_EXIT
    if version != "ok":
        return 2

    files = list_files(root)
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        flags = list(pool.map(lambda f: needs_format(binary, root, f), files))
    bad = [f for f, b in zip(files, flags) if b]

    if not bad:
        print(f"C++ formatting OK ({len(files)} files)")
        return 0
    print(f"{len(bad)} of {len(files)} files need clang-format")
    shown = bad if args.list else bad[:LIST_LIMIT]
    for f in shown:
        print(f"  {f}")
    if len(shown) < len(bad):
        print(f"  ... and {len(bad) - len(shown)} more (use --list to see all)")
    print("Fix: `git clang-format` (changed lines only) or "
          "`clang-format -i <files>`")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
