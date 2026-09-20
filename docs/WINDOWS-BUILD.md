# Building Tesseract on Windows

This is a Windows-focused build guide for working on Tesseract from source.
It supplements `docs/BUILD.md` with the practical details that matter on a
real Windows dev machine.

## What you need installed

### Required

- **Visual Studio with the MSVC C++ toolchain**
  - A recent Visual Studio with C++ support is required.
  - Make sure the **Desktop development with C++** workload is installed.
  - You also need the **Windows SDK**.
- **Rust (MSVC toolchain)**
  - Install Rust via `rustup`.
  - Make sure the active toolchain is:

    ```powershell
    rustup toolchain install stable-x86_64-pc-windows-msvc
    rustup default stable-x86_64-pc-windows-msvc
    ```
- **CMake**
- **Ninja**
  - The repo's Windows presets use Ninja.
- **Python 3**

### Required for the Windows app target

The Windows UI build generates `.ico` files from SVGs at build time.
Install these Python packages into the same Python that CMake finds:

```powershell
python -m pip install resvg-py pillow
```

Without that, the `tesseract_win32` build fails while generating icons with an
error like:

```text
ModuleNotFoundError: No module named 'resvg_py'
```

## Recommended shell

Use one of these:

- **Developer PowerShell for Visual Studio**
- **Developer Command Prompt for Visual Studio**

This matters because the build needs the full MSVC environment, not just `cl`
on `PATH`.

If you run from a plain shell, you may see errors like:

```text
CMake was unable to find a build program corresponding to "Ninja"
CMAKE_CXX_COMPILER not set
```

## Verify your environment

From a Visual Studio developer shell, these should all work:

```powershell
cmake --version
ninja --version
cargo --version
rustc --version
cl
python --version
```

## Important Windows-specific path warning

The LiveKit/WebRTC dependency extracts a very deep header tree during the Rust
build. On Windows, a long checkout path can make MSVC report nested headers as
missing even when the files actually exist.

Examples of symptoms:

```text
fatal error C1083: Cannot open include file:
'modules/desktop_capture/delegated_source_list_controller.h'
```

or later:

```text
fatal error C1083: Cannot open include file:
'absl/strings/internal/str_format/constexpr_parser.h'
```

Those files may genuinely be present on disk; the path is just too deep.

### What the repo does to help

The Windows presets intentionally use short build directories:

- `windows-debug` -> `b/wd`
- `windows-release` -> `b/wr`

instead of `build/windows-*`.

### If your checkout path is still too deep

Use one of these workarounds:

#### Option A: clone/move to a shorter path

Examples:

```text
C:\src\tesseract
```

or

```text
C:\t\tesseract
```

#### Option B: use a temporary drive mapping

```powershell
subst T: C:\Users\yourname\path\to\tesseract
T:
cmake --preset windows-debug
cmake --build b\wd --target tesseract_tests
ctest --test-dir b\wd --output-on-failure
subst T: /d
```

This is often the easiest fix for deep-path WebRTC build failures.

## Configure

From the repo root, in a Visual Studio developer shell:

```powershell
cmake --preset windows-debug
```

This configures the build into:

```text
b\wd
```

For release:

```powershell
cmake --preset windows-release
```

which configures into:

```text
b\wr
```

## Build the app

Build the Windows executable target:

```powershell
cmake --build b\wd --target tesseract_win32
```

The executable is produced at:

```text
b\wd\ui\windows\Tesseract.exe
```

Run it with:

```powershell
.\b\wd\ui\windows\Tesseract.exe
```

## Build the C++ test binary

```powershell
cmake --build b\wd --target tesseract_tests
```

That produces the Catch2 test executable under `b\wd\tests\`.

## Run C++ tests

Run the full C++/ctest suite:

```powershell
ctest --test-dir b\wd --output-on-failure
```

Run only tests matching a regex:

```powershell
ctest --test-dir b\wd --output-on-failure -R "<your test regex>"
```

List registered tests without running them:

```powershell
ctest --test-dir b\wd -N
```

## Run Rust-only tests

These do not require the C++ test executable:

```powershell
cargo test -p tesseract-sdk-ffi
```

## Clean rebuilds

If you want to wipe the Windows preset build tree:

```powershell
Remove-Item -Recurse -Force .\b\wd
```

Then reconfigure:

```powershell
cmake --preset windows-debug
```

## Troubleshooting

### 1. `cmake` / `ninja` / `cargo` not found

Use a Visual Studio developer shell and verify:

```powershell
cmake --version
ninja --version
cargo --version
```

If needed, install or add:

- CMake
- Ninja
- Rust (`cargo`)

But remember: on Windows, **PATH alone is often not enough**. The MSVC include,
library, and SDK environment also need to be set correctly, which the Visual
Studio developer shell does for you.

### 2. WebRTC or Abseil headers reported missing, but they exist on disk

This is usually a **deep path** problem, not a missing file.

Try:

- moving the repo to a shorter path, or
- building via `subst T:`

See the path warning section above.

### 3. `ModuleNotFoundError: No module named 'resvg_py'`

Install the missing Python packages:

```powershell
python -m pip install resvg-py pillow
```

### 4. `No space left on device`

Large Rust/C++ dependency builds can consume a lot of disk space. Free space and
remove stale build artifacts:

```powershell
Remove-Item -Recurse -Force .\b\wd
```

You may also need to clean Rust build output if no process is locking it:

```powershell
Remove-Item -Recurse -Force .\target
```

If `target\` cannot be deleted, something still has files open in it.
Close running build tools, editor-integrated background tasks, or test
processes and try again.

### 5. `LNK1104: cannot open file 'tests\tesseract_tests.exe'`

If linking the test executable fails after a previous run, remove stale test
link outputs and rebuild:

```powershell
Remove-Item .\b\wd\tests\tesseract_tests.lib -ErrorAction SilentlyContinue
Remove-Item .\b\wd\tests\tesseract_tests.exp -ErrorAction SilentlyContinue
Remove-Item .\b\wd\tests\tesseract_tests.pdb -ErrorAction SilentlyContinue
cmake --build b\wd --target tesseract_tests
```

### 6. x86/x64 mismatch warnings or link failures

Use an **x64 Visual Studio developer shell**.

If you see warnings like:

```text
library machine type 'x64' conflicts with target machine type 'x86'
```

then the shell was set up for the wrong architecture.

### 7. `ctest -N` mentions a missing `bettertext_tests.exe`

You may see a registered test entry for `third_party/bettertext/bettertext_tests.exe`
when listing all tests. Treat that separately from the normal Tesseract app and
`tesseract_tests` targets.

## Known-good command sequence

If you want one practical end-to-end sequence:

```powershell
python -m pip install resvg-py pillow
cmake --preset windows-debug
cmake --build b\wd --target tesseract_tests
ctest --test-dir b\wd --output-on-failure
cmake --build b\wd --target tesseract_win32
.\b\wd\ui\windows\Tesseract.exe
```

If you only want to run a subset of the C++ tests, add a regex filter:

```powershell
ctest --test-dir b\wd --output-on-failure -R "<your test regex>"
```

## Outputs to expect

- App:
  - `b\wd\ui\windows\Tesseract.exe`
- Test executable:
  - `b\wd\tests\tesseract_tests.exe`
- Debug preset build tree:
  - `b\wd`
- Release preset build tree:
  - `b\wr`
