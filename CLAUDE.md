# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Tesseract is a cross-platform desktop Matrix/chat client. The core networking is Rust (using `matrix-sdk`), exposed to C++ via a `cxx` FFI bridge. Platform-specific UIs are written in C++ targeting Win32 (Windows), Qt6 Widgets, or GTK4 (Linux).

## Build Commands

**Prerequisites (Linux/Qt6):**
```bash
sudo apt install qt6-base-dev ninja-build cmake libssl-dev sqlite3 libsqlite3-dev
# also requires a Rust toolchain: rustup
```

**Configure and build:**
```bash
cmake --preset linux-qt6-debug          # or windows-debug, linux-gtk-debug, linux-qt6-release
cmake --build build/linux-qt6-debug
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

**Available presets** (in `CMakePresets.json`): `windows-debug`, `windows-release`, `linux-gtk-debug`, `linux-qt6-debug`, `linux-qt6-release`.

**Override UI selection:** `-DTESSERACT_UI=gtk|qt6|win32` (otherwise auto-detected from platform).

Corrosion (CMake↔Cargo bridge) is fetched automatically via `FetchContent` — no global install needed. OpenSSL and SQLite are found via `find_package()`.

## Architecture

```
sdk/         ← Rust crate: matrix-sdk wrapper + cxx FFI bridge
client/      ← C++ static library: high-level C++ API over the Rust FFI
ui/
  windows/   ← Win32 executable
  linux-qt/  ← Qt6 Widgets executable
  linux-gtk/ ← GTK4 executable
```

### Layer responsibilities

**`sdk/` (Rust)** — All async I/O lives here. A `tokio` runtime runs in background threads. The `cxx` bridge (`sdk/src/lib.rs`) exposes a synchronous C API. `client.rs` wraps `matrix-sdk` for login, sync, and messaging; `oauth.rs` implements the RFC 8252 loopback redirect OAuth flow.

**`client/` (C++)** — `tesseract::Client` (Pimpl) wraps the Rust FFI. `tesseract::IEventHandler` is the interface UIs implement to receive async callbacks (room updates, sync events, session saves). `tesseract::SessionStore` handles platform-specific persistence of the session JSON (`%APPDATA%/Tesseract/` on Windows, `~/.config/tesseract/` on Linux).

**`ui/*/` (C++)** — Each target implements `IEventHandler`. Because Rust callbacks arrive on worker threads, Qt UI must use `QueuedConnection` signals; GTK UI must marshal via glib.

### Key API surface (`client/include/tesseract/`)

- `Client::begin_oauth()` / `Client::await_oauth()` — two-phase OAuth: first call returns the auth URL (open in browser), second blocks until the loopback redirect completes.
- `Client::start_sync()` / `Client::stop_sync()` — background sync loop.
- `Client::send_message()`, `Client::list_rooms()`, `Client::room_messages()` — core messaging.
- `IEventHandler::on_session_saved(json)` — called on token refresh; persist the JSON to resume sessions without re-login.

### Link strategy

The three archives (`tesseract_sdk_bridge_cxx`, `tesseract_client`, `tesseract_sdk_ffi-static`) have a circular dependency through the FFI. They are linked with `WHOLE_ARCHIVE` to guarantee all symbols are present regardless of link order.

## No Tests

There is no automated test suite. Testing is manual: build, run the executable, and exercise the OAuth login flow against a real Matrix homeserver.

## Key Files

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Root build: UI detection, Corrosion setup, WHOLE_ARCHIVE linking |
| `CMakePresets.json` | Per-platform build presets |
| `sdk/Cargo.toml` | Rust dependencies (`matrix-sdk`, `cxx`, `tokio`, `tiny_http`) |
| `sdk/src/lib.rs` | `cxx::bridge` — the FFI boundary between Rust and C++ |
| `sdk/src/client.rs` | Matrix SDK wrapper (sync, rooms, messaging) |
| `sdk/src/oauth.rs` | RFC 8252 loopback OAuth implementation |
| `client/include/tesseract/*.hpp` | C++ public API headers |
| `ui/linux-qt/src/MainWindow.h` | Qt UI with EventBridge pattern for thread marshaling |
