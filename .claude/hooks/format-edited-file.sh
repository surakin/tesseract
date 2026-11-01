#!/bin/bash
# Claude Code PostToolUse hook (Edit/Write/MultiEdit): formats the file that
# was just edited, so code never drifts from STYLE.md.
#   C/C++/ObjC: clang-format with the repo's .clang-format, only when the
#               installed major matches tests/clang-format-version (another
#               major formats differently). clang-format itself skips files
#               listed in .clang-format-ignore.
#   Rust:       rustfmt, for files under sdk/.
# Reads the hook JSON on stdin; does nothing for other files.
# Portable: Linux, macOS (bash 3.2, no jq by default) and Git Bash on Windows.
input=$(cat)
if command -v jq >/dev/null; then
    f=$(printf '%s' "$input" | jq -r '.tool_response.filePath // .tool_input.file_path // empty')
else
    py=$(command -v python3 || command -v python) || exit 0
    f=$(printf '%s' "$input" | "$py" -c 'import json, sys
d = json.load(sys.stdin)
print((d.get("tool_response") or {}).get("filePath") or (d.get("tool_input") or {}).get("file_path") or "")')
fi
[ -n "$f" ] || exit 0
# Windows: C:\src\x -> /c/src/x, to compare with Git Bash paths
if command -v cygpath >/dev/null; then
    f=$(cygpath -u "$f")
fi
[ -f "$f" ] || exit 0
root=$(cd "$(dirname "$0")/../.." && pwd)
case "$f" in
    "$root"/*) rel=${f#"$root"/} ;;
    *) exit 0 ;;
esac
case "$rel" in
    third_party/* | build/* | build-*/*) exit 0 ;;
esac
case "$rel" in
    *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.mm | *.m)
        command -v clang-format >/dev/null || exit 0
        want=$(cut -d. -f1 "$root/tests/clang-format-version")
        have=$(clang-format --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p')
        [ "$have" = "$want" ] || exit 0
        cd "$root" && clang-format -i --style=file "$rel"
        ;;
    sdk/*.rs)
        command -v rustfmt >/dev/null || exit 0
        edition=$(sed -n 's/^edition *= *"\([0-9]*\)".*/\1/p' "$root/sdk/Cargo.toml" | head -1)
        rustfmt --edition "${edition:-2021}" "$f"
        ;;
esac
exit 0
