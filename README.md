# Tesseract

A stupid vibe-coded multiplatform matrix client, based on the matrix rust sdk

## Minimum OS requirements

| Platform | Minimum OS | Architecture |
| -------- | ---------- | ------------ |
| Windows (Win32) | Windows 10 version 1607 (Anniversary Update) | x86-64 |
| macOS (AppKit) | macOS 11 Big Sur | Apple Silicon (arm64) or Intel (x86-64) |
| Linux — Qt6 | Debian 12 (Bookworm) / Ubuntu 24.04 LTS or newer | x86-64 |
| Linux — GTK4 | Debian 12 (Bookworm) / Ubuntu 22.04 or newer | x86-64 |

### Notes

- **Windows** — the build targets `_WIN32_WINNT=0x0A00`. `GetDpiForWindow`
  and per-monitor-v2 DPI awareness require Windows 10 1607; Windows 11 is
  also supported. The rendering stack is Direct2D + DirectWrite + WIC and
  audio uses Media Foundation, all present from Windows 10.
- **macOS** — deployment target and `LSMinimumSystemVersion` are both 11.0.
  Builds are per-architecture (arm64 or x86-64). Voice-message (MSC3245
  opus) playback additionally requires **macOS 14+**; the rest of the app
  runs on macOS 11+.
- **Linux (Qt6)** — runtime depends on Qt ≥ 6.4, which is the version
  shipped by Debian 12 and Ubuntu 24.04 LTS. Older distros work if a
  Qt ≥ 6.4 is provided.
- **Linux (GTK4)** — needs GTK 4 plus GStreamer base/good plugins (for
  voice-message playback); the system tray is a built-in StatusNotifierItem
  D-Bus implementation and needs no AppIndicator package.
  GTK 4 is available from Debian 12 / Ubuntu 22.04 onward.
- **Building from source** requires Rust ≥ 1.75 on every platform (install
  via `rustup` on distros that ship an older toolchain). See
  [CLAUDE.md](CLAUDE.md) and [PACKAGING.md](PACKAGING.md) for the full
  build and packaging instructions.
