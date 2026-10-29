#!/usr/bin/env python3
"""Keep std::string-carrying cxx types out of the Rust bridge.

Usage: check_bridge_strings.py BRIDGE_RS

webrtc-sys builds its C++ on Linux against Chromium's libc++, whose
std::string is laid out differently from libstdc++'s, and its cxx runtime
exports the same C symbols as ours (cxxbridge1$cxx_string$data, ...). A
CxxString or CxxVector<CxxString> that crosses the bridge can then be read
with the wrong layout and crash. String lists cross as Vec<String> (see
to_rust_strings() in client/src/client.cpp) and single strings as String.

Exits 0 when clean; otherwise exits 1 and prints one line per violation.
"""
import re
import sys
from pathlib import Path

FORBIDDEN = re.compile(r"\bCxxString\b|\bCxxVector\b")


def main(argv):
    if len(argv) != 2:
        print("usage: check_bridge_strings.py BRIDGE_RS", file=sys.stderr)
        return 2
    path = Path(argv[1])
    violations = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        code = line.split("//", 1)[0]
        if FORBIDDEN.search(code):
            violations.append(f"{path}:{number}: {line.strip()}")
    for v in violations:
        print(v)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
