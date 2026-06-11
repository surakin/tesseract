# Building Tesseract (development)

How to set up a dev machine and build Tesseract from source to hack on it. For
producing distributable installers (`.exe`/`.dmg`/`.deb`/`PKGBUILD`), see
[PACKAGING.md](../PACKAGING.md) instead.

## Prerequisites

### Linux / Qt6

```bash
sudo apt install qt6-base-dev qt6-multimedia-dev ninja-build cmake sqlite3 libsqlite3-dev golang perl
# also requires a Rust toolchain: rustup
```

Notes on the less obvious deps:

- **golang + perl** are build-only deps for `aws-lc-sys`'s CMake builder.
- **qt6-multimedia-dev** powers MSC3245 voice-message playback via `QMediaPlayer`.
- For **GTK4** builds also install: `libgtk-4-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`
  (gst-plugins-base provides the opus decoder voice messages use).
- The GTK4 system tray is a pure StatusNotifierItem D-Bus implementation, so no
  appindicator/GTK3 package is needed.

### macOS / AppKit

```bash
brew install ninja cmake go
xcode-select --install   # Xcode Command Line Tools
# Rust toolchain + native macOS targets (pick the one matching the preset):
#   rustup toolchain install stable
#   rustup target add aarch64-apple-darwin   # Apple Silicon
#   rustup target add x86_64-apple-darwin    # Intel
```

`go` is a build-only dep for `aws-lc-sys`'s CMake builder; `perl` ships with macOS.

## Presets

All presets live in `CMakePresets.json`:

`windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-gtk-release`,
`linux-qt6-debug`, `linux-qt6-release`, `macos-appkit-arm64-debug`,
`macos-appkit-arm64-release`, `macos-appkit-x86_64-debug`,
`macos-appkit-x86_64-release`, `mingw-debug`, `mingw-release`.

The `mingw-*` presets cross-compile the Win32 UI from a Linux host via
`cmake/toolchains/mingw-x86_64.cmake` and the `x86_64-pc-windows-gnu` Rust
target (`rustup target add x86_64-pc-windows-gnu`); they require a MinGW-w64
toolchain (`sudo apt install g++-mingw-w64-x86-64`).

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32|macos` (otherwise
auto-detected from platform).

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
