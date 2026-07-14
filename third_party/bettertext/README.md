# BetterText

BetterText is a native Win32 C++20 text input control intended as a modern alternative to RichEdit for small Windows applications.

This first implementation provides:

- A reusable `bettertext` static library target.
- A custom `HWND` control registered as `BETTERTEXT_CLASS_NAME`.
- Public APIs for text, JSON, HTML, selection, undo/redo, read-only mode, theme, and default/per-range text styles.
- A paragraph/run document model with inline URI-backed image attachment runs.
- DirectWrite/Direct2D rendering for text, caret, and selection.
- Keyboard and mouse editing, Unicode clipboard text, scrolling, and basic IME committed text handling.
- Host extension interfaces for URI image resolution, clipboard image mapping, and font collection customization.
- A Win32 demo app and CTest test executable.

WebView2, Chromium, Qt, and browser-based editors are intentionally not used.

This was created to fix the text input fields in [Tesseract](https://surakin.github.io/tesseract), because at some point I got tired of fighting against RichText.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

BetterText currently targets Windows 10+ and requires Visual Studio Build Tools or another MSVC-compatible Windows C++ toolchain.

## License

BetterText is licensed under the [MIT License](LICENSE).

The bundled Noto Color Emoji font is licensed separately under the SIL Open
Font License 1.1. See [`third_party/fonts/NotoEmoji-LICENSE`](third_party/fonts/NotoEmoji-LICENSE).

## Current Limits

This is the foundation slice, not a complete replacement for every RichEdit behavior yet. Full TSF `ITextStoreACP`, UI Automation TextPattern, DirectWrite inline object rendering for actual image layout, rich styling commands, and production-grade HTML import are the next major pieces.
