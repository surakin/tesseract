# Command-line options

```text
tesseract [OPTIONS] [MATRIX-URI]
```

The same options work on every platform. The executable is `tesseract` on Linux, `tesseract.exe` on Windows, and `Tesseract.app/Contents/MacOS/Tesseract` on macOS.

| Option | Meaning |
| ------ | ------- |
| `-h`, `--help` | Print usage and exit |
| `-V`, `--version` | Print the version and exit |
| `-p`, `--profile=NAME` | Run an isolated profile (see below) |
| `--hidden` (alias `--minimized`) | Start in the tray without showing the window |
| `--log-level=LEVEL` | SDK log level: `error`, `warn`, `info`, `debug`, `trace`, or a full `tracing` filter such as `matrix_sdk=debug,tesseract=trace` |
| `-v`, `--verbose` | Same as `--log-level=debug` |
| `--open-room=ROOM_ID` | Open this room, switching to the account that has joined it |
| `--open-quick-switcher` | Open the quick switcher |
| `--open-message-search` | Open message search |
| `--open-settings` | Open Settings |
| `--logoutall` | Sign out of every account in the profile and exit (no window) |
| `MATRIX-URI` | A `matrix:` URI or `https://matrix.to/#/…` link to open |

Values can be given as `--opt=value` or `--opt value`, and short options as `-p work` or `-pwork`. Flags can be combined, e.g. `-vp work`. Anything after `--` is treated as a positional argument.

An unknown option or a bad value prints a warning on stderr (`tesseract: warning: …`) and startup continues. Arguments that belong to the toolkit, such as Qt's `-platform wayland` or AppKit's `-NS…` user-default overrides, are passed through without a warning.

`--autostart` is also accepted but not listed in `--help`. The OS login-item entry writes it; it behaves like `--hidden` and marks the launch as automatic.

## When an instance is already running

A second launch forwards its URI and `--open-*` action to the running instance of the same profile, raises that window, and exits. A `--hidden` or `--autostart` launch that has nothing to forward exits quietly without raising anything.

## Profiles

`--profile=NAME` (1–32 characters from `A–Z a–z 0–9 _ -`) runs a completely separate instance:

- Config, data and cache folders get a `-NAME` suffix, e.g. `~/.local/share/tesseract-work` or `%APPDATA%\Tesseract-work`.
- Saved credentials are stored under a profile-scoped key, so the same account signed in to two profiles keeps two independent sessions.
- The single-instance lock is per profile, so profiles run side by side. The same applies to the Win32 window class and mutex, the GTK application id, and the macOS activation notification.
- The Windows Jump List and the autostart entries (Windows Run key, Linux XDG autostart file) relaunch into the same profile.

Platform limits:

- **macOS:** Finder and `open` (LaunchServices) never start a second copy of the app. Run a second profile from a terminal instead (`Tesseract.app/Contents/MacOS/Tesseract --profile=work`). The login item applies to the whole app, not to a single profile.
- **Windows (Microsoft Store / MSIX builds):** the package manifest fixes the app's taskbar identity (AUMID), so all profiles share one taskbar group and Jump List. Unpackaged builds get one per profile.

## `--logoutall`

For each account in the profile, `--logoutall` signs out on the homeserver, which revokes the device, and then deletes the local session, stored credentials and encrypted store. It prints one line per account on stderr.

Exit codes:

| Code | Meaning |
| ---- | ------- |
| 0 | Every account was signed out, or there were none |
| 1 | Tesseract is running for this profile, or the account list is unreadable. Nothing was changed |
| 3 | At least one server sign-out failed. That account was still removed from this device, but its device may still appear in the account's session list elsewhere |

## Windows console output

`tesseract.exe` is a GUI-subsystem program. `--help`, `--version` and warnings are written to a redirect when one is present (`tesseract.exe --help > help.txt`). Otherwise they go to the console that launched it. cmd.exe and PowerShell don't wait for GUI programs to finish, so the text can appear after the prompt has already been redrawn. Redirect it, or use `start /wait tesseract.exe --help`, to get clean output.
