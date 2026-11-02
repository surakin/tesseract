#!/bin/sh
# Called by Tesseract's screenshot mode with the output path (see
# wayland_session.sh): saves the app's window, frame included, at exactly
# 1440x900.
#
# With GRAB_MEASURE set, writes how much the frame adds to the content size
# ("W H") to that file and fails instead, ending the probe run.
set -eu

out="$1"
want_w=1440
want_h=900

# Prints "x y w h" of everything that differs from the empty screen
# wayland_session.sh captured before starting the app (GRAB_BACKGROUND): the
# window and its frame. Prints "none" while the window isn't on screen yet.
bbox() {
    grim -t ppm - | python3 -c '
import sys
def load(data):
    _, dims, _, pixels = data.split(b"\n", 3)
    width, height = map(int, dims.split())
    return width, height, pixels
width, height, pixels = load(sys.stdin.buffer.read())
with open(sys.argv[1], "rb") as f:
    _, _, empty = load(f.read())
stride = width * 3
rows = [y for y in range(height)
        if pixels[y * stride:(y + 1) * stride] != empty[y * stride:(y + 1) * stride]]
if not rows:
    print("none")
    sys.exit()
mid = rows[len(rows) // 2] * stride
row, empty_row = pixels[mid:mid + stride], empty[mid:mid + stride]
cols = [x for x in range(width) if row[x * 3:x * 3 + 3] != empty_row[x * 3:x * 3 + 3]]
left, right = cols[0] * 3, (cols[-1] + 1) * 3
# Below the title bar, a window that has not drawn yet is one flat colour.
body = [pixels[y * stride + left:y * stride + right] for y in range(rows[0] + 60, rows[-1])]
if all(line == body[0] for line in body) and body[0] == body[0][:3] * (len(body[0]) // 3):
    print("none")
    sys.exit()
print(cols[0], rows[0], cols[-1] - cols[0] + 1, rows[-1] - rows[0] + 1)' "$GRAB_BACKGROUND"
}

box=none
for _ in $(seq 20); do
    box=$(bbox)
    [ "$box" != none ] && break
    sleep 1
done
if [ "$box" = none ]; then
    echo "no drawn window on screen" >&2
    exit 1
fi
set -- $box

if [ -n "${GRAB_MEASURE:-}" ]; then
    content=${TESSERACT_SCREENSHOT_SIZE:-${want_w}x${want_h}}
    echo "$(($3 - ${content%x*})) $(($4 - ${content#*x}))" > "$GRAB_MEASURE"
    exit 3
fi

if [ "$3" -ne "$want_w" ] || [ "$4" -ne "$want_h" ]; then
    echo "window frame is ${3}x${4} at $1,$2, expected ${want_w}x${want_h}" >&2
    # Keep the whole screen for the workflow's artifact.
    grim "$(dirname "$out")/debug-screen.png" || true
    exit 1
fi
grim -g "$1,$2 ${3}x${4}" "$out"
