#!/usr/bin/env python3
"""Auto-fix leaked HTML entities in a release-notes draft and flag leftover
conventional-commit tags. Run as: python3 verify.py <draft.md>

Exists because models reliably mistype a literal "&" in freehand shell
commands (auto-escaping it to "&amp;"), which silently defeats a hand-written
grep check. This script never requires the caller to type "&" itself.
"""
import html
import re
import sys

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify.py <draft.md>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    with open(path, encoding="utf-8") as fh:
        text = fh.read()

    fixed = html.unescape(text)
    if fixed != text:
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(fixed)
        print(f"Fixed leaked HTML entities in {path} (rewrote the file).")
    else:
        print(f"No leaked HTML entities found in {path}.")

    tag_re = re.compile(
        r"\b(feat|fix|perf|refactor|build|test|docs|chore)\(?[a-zA-Z0-9_-]*\)?:"
    )
    hits = [
        (i, line)
        for i, line in enumerate(fixed.splitlines(), start=1)
        if tag_re.search(line)
    ]
    if hits:
        print(f"WARNING: {len(hits)} leftover conventional-commit tag(s) found "
              "— remove these manually, the draft is not done:")
        for lineno, line in hits:
            print(f"  {path}:{lineno}: {line}")
        return 1

    print("No leftover conventional-commit tags found.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
