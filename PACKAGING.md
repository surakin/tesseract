# Packaging Tesseract

Tesseract uses [CPack](https://cmake.org/cmake/help/latest/module/CPack.html) to
produce native installers. Configuration lives in
[cmake/Installers.cmake](cmake/Installers.cmake) and is included from the root
`CMakeLists.txt` only on Windows and macOS.

| Platform | Generator   | Output                                      |
| -------- | ----------- | ------------------------------------------- |
| Windows  | `NSIS`      | `Tesseract-<version>-<arch>.exe`            |
| macOS    | `DragNDrop` | `Tesseract-<version>-<arch>.dmg`            |
| Linux    | — (out of scope; use distro-native packaging) |

The Rust SDK is built as a `staticlib`, so installers carry no runtime DLLs
or dylibs — just the executable (Windows) or the `.app` bundle (macOS).

---

## Windows: NSIS `.exe`

### Prerequisites

- Everything from the standard Windows build setup in
  [CLAUDE.md](CLAUDE.md#build-commands).
- [NSIS](https://nsis.sourceforge.io/Download) — `makensis.exe` must be on
  `PATH`. The Chocolatey package works: `choco install nsis`.

### Build

```powershell
cmake --preset windows-release
cmake --build build/windows-release
cmake --build build/windows-release --target package
```

The installer lands at
`build/windows-release/Tesseract-0.1.0-AMD64.exe`.

### What the installer does

- Installs `Tesseract.exe` to `C:\Program Files\Tesseract\bin\` by default.
- Adds a Start Menu shortcut: **Tesseract**.
- Registers an uninstaller under *Settings → Apps* / *Add or Remove Programs*.
- `CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL` is on, so re-running the
  installer cleanly removes the previous version first.

### Code signing (optional)

Pass a `.pfx` and password at configure time; `signtool` runs as part of the
install step that CPack invokes, so the signed binary is what gets packaged:

```powershell
cmake --preset windows-release `
      -DTESSERACT_WIN_SIGN_CERT=C:/keys/tesseract.pfx `
      -DTESSERACT_WIN_SIGN_PASS=...
cmake --build build/windows-release --target package
```

Timestamping uses `http://timestamp.digicert.com` with SHA-256. Verify with:

```powershell
signtool verify /pa "C:\Program Files\Tesseract\bin\Tesseract.exe"
```

---

## macOS: DragNDrop `.dmg`

### Prerequisites

- Everything from the standard macOS build setup in
  [CLAUDE.md](CLAUDE.md#build-commands).
- No extra tooling — `hdiutil` ships with macOS.

### Build

Apple Silicon:

```bash
cmake --preset macos-appkit-arm64-release
cmake --build build/macos-appkit-arm64-release
cmake --build build/macos-appkit-arm64-release --target package
```

Intel:

```bash
cmake --preset macos-appkit-x86_64-release
cmake --build build/macos-appkit-x86_64-release --target package
```

Each preset produces one arch-specific DMG, e.g.
`build/macos-appkit-arm64-release/Tesseract-0.1.0-arm64.dmg`. There is no
universal-binary preset today; ship both DMGs or add a `lipo`-merge step.

### What the installer does

- Mounts a volume named "Tesseract 0.1.0".
- Contains `Tesseract.app` plus a symlink to `/Applications`.
- The user drags the app over to install. No admin prompt; no system-wide
  install paths (use a PKG generator if you need one).

### Code signing (optional)

Provide a Developer ID identity at configure time:

```bash
cmake --preset macos-appkit-arm64-release \
      -DTESSERACT_MAC_CODESIGN_IDENTITY="Developer ID Application: Foo (TEAMID)"
cmake --build build/macos-appkit-arm64-release --target package
```

`codesign --options runtime --timestamp` is invoked against the `.app` before
CPack rolls it into the DMG. Verify with:

```bash
codesign --verify --deep --strict /Applications/Tesseract.app
```

### Notarization (optional)

After producing the signed DMG, submit it to Apple's notary service:

```bash
cmake --preset macos-appkit-arm64-release \
      -DTESSERACT_MAC_CODESIGN_IDENTITY="Developer ID Application: Foo (TEAMID)" \
      -DTESSERACT_MAC_NOTARIZE_PROFILE=AC_NOTARY
cmake --build build/macos-appkit-arm64-release --target package
cmake --build build/macos-appkit-arm64-release --target notarize
```

`AC_NOTARYTOOL` is a keychain profile created once with
`xcrun notarytool store-credentials`. The `notarize` target runs
`xcrun notarytool submit --wait` followed by `xcrun stapler staple` against
the DMG.

---

## CMake cache variables

| Variable                            | Default | Purpose                                              |
| ----------------------------------- | ------- | ---------------------------------------------------- |
| `TESSERACT_WIN_SIGN_CERT`           | empty   | Path to `.pfx` for Windows `signtool`. Empty = skip. |
| `TESSERACT_WIN_SIGN_PASS`           | empty   | Password for the `.pfx`.                             |
| `TESSERACT_MAC_CODESIGN_IDENTITY`   | empty   | `codesign --sign` identity. Empty = skip.            |
| `TESSERACT_MAC_NOTARIZE_PROFILE`    | empty   | `xcrun notarytool` keychain profile. Empty = skip.   |

---

## Open items

- The `CPACK_PACKAGE_CONTACT`, `CPACK_NSIS_HELP_LINK`, and
  `CPACK_NSIS_URL_INFO_ABOUT` values in
  [cmake/Installers.cmake](cmake/Installers.cmake) are placeholders
  (`https://github.com/<TBD>`). Replace before shipping.
- There is no `LICENSE` file in the repo, so the NSIS license page is
  skipped. Drop one at the root and set `CPACK_RESOURCE_FILE_LICENSE` in
  `cmake/Installers.cmake` to enable it.
