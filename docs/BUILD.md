# Building Tesseract (development)

How to set up a dev machine and build Tesseract from source to hack on it. For
producing distributable installers (`.exe`/`.dmg`/`.deb`/`PKGBUILD`), see
[PACKAGING.md](../PACKAGING.md) instead.

## Prerequisites

All targets require a C++20-capable compiler (`CMAKE_CXX_STANDARD` is set to 20
globally) — MSVC needs `/std:c++20` (the default with recent Visual Studio),
and GCC/Clang need a version with full C++20 support (defaulted comparison
operators, etc.).

### Linux / Qt6

```bash
sudo apt install qt6-base-dev qt6-multimedia-dev ninja-build cmake golang perl \
                 libopus-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
                 libavutil-dev
# also requires a Rust toolchain: rustup
```

Notes on the less obvious deps:

- **golang + perl** are build-only deps for `aws-lc-sys`'s CMake builder.
- **qt6-multimedia-dev** powers MSC3245 voice-message playback via `QMediaPlayer`.
- **libopus-dev** is the Opus codec library; it is linked directly by both the Qt6 and GTK4 shells.
- **libgstreamer1.0-dev + libgstreamer-plugins-base1.0-dev** are required by the shared toolkit's
  off-thread video-frame decoder (used for GIF animation strips) in both the Qt6 and GTK4 builds.
- **libavutil-dev** (from the `ffmpeg` package family) is linked by both Linux shells to silence
  FFmpeg/gst-libav diagnostic output at startup via `av_log_set_level`.
- **libsecret-1-dev** is optional but recommended on Linux: without it, session tokens fall back to
  plaintext storage. Install with `sudo apt install libsecret-1-dev`.
- **libwayland-dev + qt6-base-private-dev** are optional for the Qt6 build: together they enable
  xdg-activation-v1 Wayland window activation (correct window raising from notifications/links on
  Wayland compositors). The build degrades gracefully without them — xdg-activation is simply
  omitted. Install with `sudo apt install libwayland-dev qt6-base-private-dev`.
- For **GTK4** builds also install: `libgtk-4-dev`
  (the gstreamer packages above are already included; gst-plugins-base delivers the Opus decoder at runtime).
- The GTK4 system tray is a pure StatusNotifierItem D-Bus implementation, so no
  appindicator/GTK3 package is needed.

### macOS / AppKit

```bash
brew install ninja cmake go opus
xcode-select --install   # Xcode Command Line Tools
# Rust toolchain + native macOS targets (pick the one matching the preset):
#   rustup toolchain install stable
#   rustup target add aarch64-apple-darwin   # Apple Silicon
#   rustup target add x86_64-apple-darwin    # Intel
```

`go` is a build-only dep for `aws-lc-sys`'s CMake builder; `perl` ships with macOS.

## Calls / MatrixRTC (optional)

Native voice and video calls via LiveKit/WebRTC are gated behind a CMake flag that
defaults to OFF:

```bash
cmake --preset linux-debug -DTESSERACT_ENABLE_CALLS=ON
```

### Additional prerequisites when calls are enabled

**Linux** — one extra package:

```bash
sudo apt install libxtst-dev      # Ubuntu/Debian
# sudo dnf install libXtst-devel  # Fedora/RHEL
```

`libxtst-dev` provides XTest/XRecord headers required by livekit-webrtc's Linux
screen-capture backend.

**macOS** — no extra `brew install` needed. All required frameworks
(CoreAudio, CoreMedia, CoreVideo, IOKit, IOSurface, Metal, VideoToolbox) are part of
the Xcode SDK. ScreenCaptureKit (screen sharing) is weak-linked and requires macOS
12.3 or later; it is silently absent and screen sharing is disabled on older versions.
`opus` is already listed in the base prerequisites above.

**Windows** — calls are not yet supported on Windows.

**First-build download** — when `TESSERACT_ENABLE_CALLS=ON`, `cargo build` downloads a
prebuilt `libwebrtc.a` (~150 MB) during the first build. Internet access is required.
Subsequent builds use the cached copy.

## Presets

All presets live in `CMakePresets.json`:

`windows-debug`, `windows-release`, `linux-debug`, `linux-release`,
`macos-appkit-arm64-debug`, `macos-appkit-arm64-release`,
`macos-appkit-x86_64-debug`, `macos-appkit-x86_64-release`, `mingw-debug`,
`mingw-release`.

The `linux-*` presets configure and build both the GTK4 and Qt6 UIs from a
single configure (`TESSERACT_UI=linux`), producing
`ui/linux-qt/tesseract` and `ui/linux-gtk/tesseract` side by side in the same
build tree. This dual-backend mode is dev-only — its `install`/`package`
targets are disabled since both backends would otherwise fight over the same
install destinations; reconfigure with a single-backend `-DTESSERACT_UI=`
override (below) to install or package.

The `mingw-*` presets cross-compile the Win32 UI from a Linux host via
`cmake/toolchains/mingw-x86_64.cmake` and the `x86_64-pc-windows-gnu` Rust
target (`rustup target add x86_64-pc-windows-gnu`); they require a MinGW-w64
toolchain (`sudo apt install g++-mingw-w64-x86-64`).

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32|macos|linux`
(`linux` builds both GTK4 and Qt6; otherwise auto-detected from platform).
Pass this alongside a `linux-*` preset (e.g. `-DTESSERACT_UI=qt6`) to build
and install/package a single Linux backend instead of both.

## Build internals

- **Corrosion** (the CMake↔Cargo bridge) is fetched automatically via
  `FetchContent` — no global install needed.
- **SQLite** is compiled in-tree via matrix-sdk's `bundled-sqlite` feature.
- **TLS** uses rustls — no system OpenSSL required.

### Link strategy

The three archives (`tesseract_sdk_bridge_cxx`, `tesseract_client`,
`tesseract_sdk_ffi-static`) have a circular dependency through the FFI. They are
linked with `WHOLE_ARCHIVE` to guarantee all symbols are present regardless of
link order.

## Tests

- The cxx bridge is excluded from the Rust test build via `#[cfg(not(test))]`, so
  `cargo test -p tesseract-sdk-ffi` needs no C++ toolchain.
- Catch2 is fetched automatically. Each `TEST_CASE` is registered as a separate
  ctest test. The `tesseract_tests` executable links the full stack (Rust SDK +
  bridge + client lib) with the same `WHOLE_ARCHIVE` pattern as the UI targets.

See [CLAUDE.md](../CLAUDE.md) for the day-to-day configure/build/test command loop.
