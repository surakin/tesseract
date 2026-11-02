#!/bin/sh
# Runs a Tesseract screenshot build under a headless labwc (Wayland, software
# rendering, no GPU or X server needed) for update-screenshots.yml.
#
# Usage: wayland_session.sh COMMAND [ARGS...]
#
# labwc draws the window frame (server-side decorations, for Qt6 and GTK4
# alike); grab.sh finds the window by comparing the screen with a capture of
# the empty desktop.
# The app runs twice: a probe run whose first capture measures the frame and
# stops, then the real run with the content shrunk by the frame
# (TESSERACT_SCREENSHOT_SIZE) so frame plus content is exactly 1440x900.
set -eu

here=$(cd "$(dirname "$0")" && pwd)

runtime=$(mktemp -d)
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime"

mkdir -p "$runtime/labwc"
cat > "$runtime/labwc/rc.xml" <<'EOF'
<?xml version="1.0"?>
<labwc_config>
  <core><decoration>server</decoration></core>
  <!-- Greybird (greybird-gtk-theme) is Xfce's default look. Square corners:
       rounded ones would let the background into the crop. -->
  <theme><name>Greybird</name><cornerRadius>0</cornerRadius></theme>
  <placement><policy>center</policy></placement>
</labwc_config>
EOF

# An invisible pointer: labwc draws one on the headless output, and it would
# otherwise land on the window. Every cursor name is a 1x1 transparent Xcursor.
mkdir -p "$runtime/icons/blank/cursors"
python3 - "$runtime/icons/blank/cursors" <<'EOF'
import os, struct, sys
image = struct.pack("<9I", 36, 0xFFFD0002, 24, 1, 1, 1, 0, 0, 0) + struct.pack("<I", 0)
data = b"Xcur" + struct.pack("<3I", 16, 0x10000, 1) + struct.pack("<3I", 0xFFFD0002, 24, 28) + image
names = ["default", "left_ptr", "pointer", "hand2", "text", "xterm", "wait", "watch",
         "progress", "grab", "grabbing", "move", "crosshair", "not-allowed"]
for name in names:
    with open(os.path.join(sys.argv[1], name), "wb") as f:
        f.write(data)
EOF
export XCURSOR_PATH="$runtime/icons" XCURSOR_THEME=blank XCURSOR_SIZE=24

pids=""
trap 'kill $pids 2>/dev/null || true; rm -rf "$runtime"' EXIT

WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    labwc -C "$runtime/labwc" > "$runtime/labwc.log" 2>&1 &
pids="$!"

display=""
for _ in $(seq 100); do
    for sock in "$runtime"/wayland-*; do
        case "$sock" in *.lock) continue ;; esac
        [ -S "$sock" ] && display=$(basename "$sock")
    done
    [ -n "$display" ] && break
    sleep 0.1
done
if [ -z "$display" ]; then
    echo "labwc did not start" >&2
    cat "$runtime/labwc.log" >&2
    exit 1
fi
export WAYLAND_DISPLAY="$display"

wlr-randr --output HEADLESS-1 --custom-mode 1600x1000
wlr-randr
swaybg -c '#ff00ff' > "$runtime/swaybg.log" 2>&1 &
pids="$pids $!"
sleep 1
# The empty screen, for grab.sh to tell the window apart from the background
# (swaybg's fill isn't a flat colour, and the pointer is drawn too).
grim -t ppm "$runtime/empty.ppm"
export GRAB_BACKGROUND="$runtime/empty.ppm"

export QT_QPA_PLATFORM=wayland GDK_BACKEND=wayland
# No GPU: start GTK in software rendering instead of failing over from GL.
export GSK_RENDERER=cairo
export TESSERACT_SCREENSHOT_CAPTURE="$here/grab.sh"
unset DISPLAY

dump_logs() {
    echo "--- labwc.log ---" >&2
    cat "$runtime/labwc.log" >&2
    echo "--- swaybg.log ---" >&2
    cat "$runtime/swaybg.log" >&2
}

# Probe run: grab.sh measures the frame on the first capture and fails it,
# which ends the run.
GRAB_MEASURE="$runtime/frame" "$@" > "$runtime/probe.log" 2>&1 || true
if [ ! -s "$runtime/frame" ]; then
    echo "probe run did not measure the window frame" >&2
    cat "$runtime/probe.log" >&2
    dump_logs
    exit 1
fi
read -r frame_w frame_h < "$runtime/frame"
export TESSERACT_SCREENSHOT_SIZE="$((1440 - frame_w))x$((900 - frame_h))"
echo "window frame adds ${frame_w}x${frame_h}; content size $TESSERACT_SCREENSHOT_SIZE"

status=0
"$@" || status=$?
[ "$status" -eq 0 ] || dump_logs
exit "$status"
