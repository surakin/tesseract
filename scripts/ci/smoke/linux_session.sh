#!/bin/sh
# Runs "$@" on a virtual display with an unlocked login keyring. Meant to run
# inside dbus-run-session. libsecret needs an unlocked default collection;
# without one session_store.cpp falls back to plaintext and the restore phase
# would not exercise the real secret store. The keyring lives under
# XDG_DATA_HOME, which run.py isolates per shell and keeps across phases.
set -e
printf 'smoke' | gnome-keyring-daemon --unlock --components=secrets >/dev/null
# Start the accessibility bus. GTK activates it on demand, but Qt only connects
# when org.a11y.Bus is already registered (QT_LINUX_ACCESSIBILITY_ALWAYS_ON is
# only read then), so without this the Qt run has no accessibility bridge.
dbus-send --session --print-reply --dest=org.a11y.Bus /org/a11y/bus org.a11y.Bus.GetAddress >/dev/null
exec xvfb-run -a -s '-screen 0 1440x1000x24' "$@"
