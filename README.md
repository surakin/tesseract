# Tesseract

> The native desktop Matrix client you always wanted.

![Brand new account (Qt6)](./screenshots/brand_new_account_qt6.png)

A full featured multiplatform Matrix client built on the [Matrix Rust SDK](https://github.com/matrix-org/matrix-rust-sdk)

---

## Why Tesseract?

- **Truly native client on four platforms: Qt6, GTK4, Windows and macOS.**
- **Multi account support**
- **All the details and functionality of the popular chat apps.**

---

## Screenshots

| Linux (GTK4 / Qt6) | Windows | macOS |
|---|---|---|
| ![Linux](screenshots/brand_new_account_qt6.png) | ![Windows](screenshots/brand_new_account_windows.png) | ![macOS](screenshots/brand_new_account_macos.png) |


## Features

### Messaging

- Send, receive, edit, reply, react, and redact
- Markdown formatting (send and receive), with syntax-highlighted code blocks
- Custom emoji and reactions (image packs, MSC2545)
- Threads
- Mentions with autocomplete and rich pills
- Read receipts (public and private), typing indicators, fully-read markers
- Day separators and new-message markers

### Media

- Images, video, voice messages, audio, and files — send and receive
- Animated stickers, including bridged stickers
- Media captions
- Zoomable / pannable image viewer
- Inline audio player with voice-message waveforms
- URL previews

### Rooms & navigation

- Room list with Favorites, DMs, Rooms, and Spaces (tag-aware)
- Space and subspace navigation
- Open multiple rooms in tabs
- Automatic grouping of inactive rooms (configurable)
- Jump-to-date with a calendar picker (if supported by server)
- Unread indicators and last-message previews (including media)
- Direct messages

### Security & privacy

- End-to-end encryption with emoji (SAS) device verification
- Key backup recovery and room-key export / import
- OS-native secure credential storage on every platform
- Privacy controls, including presence send/poll toggles
- Device & session management

### Notifications

- Native desktop notifications with inline media
- Unified Push on Linux
- Per-room notification settings (mute / mentions / all)
- Respects your server-side push rules

### Platform integration

- System tray with unread/mention indicator and minimize-to-tray
- Session restore (last open room)
- Light / dark / system themes

## Minimum OS requirements

| Platform | Minimum OS | Architecture |
| -------- | ---------- | ------------ |
| Windows (Win32) | Windows 10 version 1607 (Anniversary Update) | x86-64 |
| macOS (AppKit) | macOS 11 Big Sur | Apple Silicon (arm64) or Intel (x86-64) |
| Linux — Qt6 | Debian 12 (Bookworm) / Ubuntu 24.04 LTS or newer | x86-64 |
| Linux — GTK4 | Debian 12 (Bookworm) / Ubuntu 22.04 or newer | x86-64 |

---

## A note on how this was built

Tesseract was developed with heavy use of AI assistance (Claude Code). I want to be upfront about that, since it's reasonable to want to know.

What that means in practice: I directed the architecture and design decisions, chose to build on the audited [matrix-rust-sdk](https://github.com/matrix-org/matrix-rust-sdk) for everything security-critical (encryption, key management, verification), and reviewed the code that went in. AI assistance is what made it feasible for one person to maintain four native frontends; it didn't replace understanding what the code does.

If that's a dealbreaker for you, that's a fair call to make — the source is open for inspection, and I'd genuinely welcome review.

---

## Project status

This started as a Claude Code experiment, because eventually I gave up and decided to learn what was all this fuzz about. I thought what was the app I always wanted to do my way and never had the time and/or energy? Yes! A Matrix client, of course.

<!-- One honest paragraph. Solo project? Say so. How actively maintained?
     What's the support expectation? What's NOT promised? Setting this
     explicitly attracts the right users and filters out the wrong ones. -->

---

## Contributing

<!-- Decide your stance before publishing:
     - PRs welcome on any frontend? Or core-only with community-owned shells?
     - The shared-core/thin-shell design means a contributor could own one
       frontend without touching the core — say if that's how you'd want it.
     - Link a CONTRIBUTING.md if you write one. -->

---

## Acknowledgements

- Built on [matrix-rust-sdk](https://github.com/matrix-org/matrix-rust-sdk)
<!-- Notable crates, the Matrix spec/community, anything you leaned on heavily. -->

---

## License

Tesseract is licensed under the [GNU Public License v3](./LICENSE)

---

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
