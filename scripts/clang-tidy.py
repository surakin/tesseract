#!/usr/bin/env python3
"""Run clang-tidy over the project's own C++ translation units.

Usage: clang-tidy.py [--preset linux-tidy] [--checks GLOBS] [--fix]
                     [--jobs N] [--no-build] [--list] [PATH ...]

  --preset    configure preset whose build tree holds compile_commands.json
              (default: linux-tidy; windows-xwin-tidy for the Win32 shell).
              It must not use unity builds or PCH, which the *-tidy presets
              guarantee; otherwise the database lists Unity/ stubs, not sources.
  --checks    extra -checks= globs appended to .clang-tidy's list
  --fix       apply fix-its (run on a clean tree, review the diff)
  --jobs N    parallel clang-tidy processes (default: CPU count)
  --build-dir analyse this configured tree instead of build/<preset>
  --skip-unsuitable  exit 77 when clang-tidy is missing or the tree is unity/PCH
  --no-build  skip configuring and generating bridge/autogen headers
  PATH ...    only analyse TUs under these repo-relative paths (default: all of
              client/, ui/, tests/cpp/)

Output goes to build/<preset>/clang-tidy/: raw.txt (full output) and
summary.txt (findings de-duplicated, since headers are reported once per
includer, then counted per check and per top-level directory).

Exits 0 when nothing is reported, 1 when there are findings, 2 for usage or
tooling errors.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCOPE = ("client/", "ui/", "tests/cpp/")
SKIP = ("/build/", "/third_party/", "/_deps/", "/.qt/", "xdg-activation-v1-protocol", "/nanosvg")
# Targets that generate headers/sources the analysed TUs include.
GENERATED = (
    "tesseract_sdk_bridge_cxx",
    "tesseract_icons",
    "tesseract_xdg_protocol",
    # Provides the .moc files the Qt shell includes. Builds the libraries it
    # depends on first (about two minutes, once).
    "tesseract_qt_autogen",
    "tesseract_tests_autogen",
)
FINDING = re.compile(r"^(?P<file>/[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
                     r"(?P<sev>warning|error): (?P<msg>.*) \[(?P<check>[^\]]+)\]$")


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--preset", default="linux-tidy")
    ap.add_argument("--checks", default="")
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--list", action="store_true", help="print the TUs and exit")
    ap.add_argument("--build-dir", help="use this configured tree instead of build/<preset>")
    ap.add_argument("--skip-unsuitable", action="store_true",
                    help="exit 77 (ctest skip) when clang-tidy is missing or the tree is unity/PCH")
    a = ap.parse_args()

    tidy = os.environ.get("CLANG_TIDY") or shutil.which("clang-tidy")
    runner = shutil.which("run-clang-tidy")
    if not tidy or not runner:
        print("clang-tidy / run-clang-tidy not found", file=sys.stderr)
        return 77 if a.skip_unsuitable else 2

    build_dir = os.path.abspath(a.build_dir) if a.build_dir \
        else os.path.join(ROOT, "build", a.preset)
    if not a.no_build and not a.list:
        if run(["cmake", "--preset", a.preset]).returncode:
            return 2
        known = run(["ninja", "-C", build_dir, "-t", "targets", "all"],
                    capture_output=True, text=True).stdout
        targets = [t for t in GENERATED if re.search(rf"^{t}:", known, re.M)]
        if run(["cmake", "--build", build_dir, "--target", *targets]).returncode:
            return 2

    db_path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(db_path):
        print(f"{db_path} missing; configure {a.preset} first", file=sys.stderr)
        return 2
    if a.skip_unsuitable and re.search(r"/Unity/|cmake_pch", open(db_path).read()):
        print("skipped: this tree uses unity builds or PCH (configure a *-tidy preset)")
        return 77
    prefixes = tuple(p.rstrip("/") + "/" if os.path.isdir(os.path.join(ROOT, p)) else p
                     for p in a.paths) or SCOPE
    files = []
    for e in json.load(open(db_path)):
        f = os.path.normpath(os.path.join(e["directory"], e["file"]))
        rel = os.path.relpath(f, ROOT)
        if rel.startswith(prefixes) and not any(s in "/" + rel for s in SKIP) \
                and f.endswith((".cpp", ".cc", ".c", ".mm")):
            files.append(f)
    files = sorted(set(files))
    if not files:
        print("no translation units matched", file=sys.stderr)
        return 2
    if a.list:
        print("\n".join(os.path.relpath(f, ROOT) for f in files))
        return 0

    out_dir = os.path.join(build_dir, "clang-tidy")
    os.makedirs(out_dir, exist_ok=True)
    cmd = [runner, "-clang-tidy-binary", tidy, "-p", build_dir, "-j", str(a.jobs), "-quiet"]
    if a.checks:
        cmd.append(f"-checks={a.checks}")
    if a.fix:
        cmd.append("-fix")
    print(f"clang-tidy: {len(files)} translation units, {a.jobs} jobs", flush=True)
    proc = run(cmd + files, capture_output=True, text=True)
    raw = proc.stdout + proc.stderr
    open(os.path.join(out_dir, "raw.txt"), "w").write(raw)

    seen, by_check, by_dir = set(), collections.Counter(), collections.Counter()
    lines = []
    tool_errors = [l for l in raw.splitlines() if re.match(r"(error|Error)[: ]", l)]
    for l in raw.splitlines():
        m = FINDING.match(l)
        if not m:
            continue
        rel = os.path.relpath(m["file"], ROOT)
        if rel.startswith("..") or any(s in "/" + rel for s in SKIP):
            continue
        key = (rel, m["line"], m["col"], m["check"], m["msg"])
        if key in seen:
            continue
        seen.add(key)
        by_check[m["check"]] += 1
        by_dir["/".join(rel.split("/")[:2])] += 1
        lines.append(f"{rel}:{m['line']}:{m['col']}: {m['check']}: {m['msg']}")
    with open(os.path.join(out_dir, "summary.txt"), "w") as f:
        f.write(f"{len(lines)} findings\n\nper check:\n")
        f.writelines(f"{n:6d}  {c}\n" for c, n in by_check.most_common())
        f.write("\nper directory:\n")
        f.writelines(f"{n:6d}  {d}\n" for d, n in by_dir.most_common())
        f.write("\nfindings:\n" + "\n".join(sorted(lines)) + "\n")
    print(f"{len(lines)} findings; see {os.path.relpath(out_dir, ROOT)}/summary.txt")
    for c, n in by_check.most_common(15):
        print(f"{n:6d}  {c}")
    if not lines and (proc.returncode != 0 or tool_errors):
        print("clang-tidy failed without producing findings:", file=sys.stderr)
        print("\n".join(tool_errors[:10]) or raw[-2000:], file=sys.stderr)
        return 2
    return 1 if lines else 0


if __name__ == "__main__":
    sys.exit(main())
