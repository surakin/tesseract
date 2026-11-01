# Developing Tesseract

How to change the code without fighting the structure. Read [ARCHITECTURE.md](ARCHITECTURE.md) first for the big picture, then the deep dives [TOOLKIT.md](TOOLKIT.md), [SHELLS.md](SHELLS.md) and [BRIDGE.md](BRIDGE.md) for the area you are touching, and [BUILD.md](BUILD.md) to get a working build.

## AI-assisted development

AI coding tools are welcome here. Most of Tesseract was written with Claude, so a ban would be hypocritical. The condition is that a human has **read, understood and tested** every line they submit, and can explain it in review. You are responsible for the change, whoever or whatever typed it. Unreviewed generated code, large drive-by rewrites and changes you couldn't verify will be sent back.

## The day-to-day loop

```bash
cmake --preset linux-debug               # macOS: macos-appkit-arm64-debug
cmake --build build/linux-debug
ctest --test-dir build/linux-debug --output-on-failure
cargo test -p tesseract-sdk-ffi          # Rust only, no C++ toolchain needed
```

Build and test on your own platform only. You don't need the other three to submit a change, but say which ones you could not build.

## Ground rules

1. **Put code in `ui/shared/`.** Platform shells (`ui/windows`, `ui/macos`, `ui/linux-qt`, `ui/linux-gtk`) hold only what is truly platform-specific: windows, menus, OS calls, thin wiring. If you write the same logic in two shells, it belongs in `ui/shared/`.
2. **Respect the layers.** `tk` ← `platform` ← `views` ← `app`: a layer may include lower ones, never higher ones. The `layering_check` test enforces it. New OS services go in `ui/shared/platform/`, not `tk/` or a shell.
3. **The Matrix protocol is the SDK's job.** Don't reimplement flows, and don't patch the SDK. If matrix-sdk lacks something, wait for it. The exception is event-content MSCs, which may be handled as raw JSON in `sdk/src/msc/<mscNNNN>.rs`.
4. **Keep UI parity.** The app should look and behave the same on every platform. Shared views make this nearly automatic, so avoid per-platform visual tweaks.
5. **Follow [STYLE.md](../STYLE.md).** In short, for C++: Allman braces, always brace control-flow bodies, `snake_case` functions, `PascalCase` types, trailing `_` on members. Objective-C methods are `camelCase`. Rust uses standard idiomatic style: match the surrounding code, and don't run `cargo fmt` over the whole `sdk/` crate, which would reformat unrelated code.
6. **Real widgets, not painted ones.** Use `tk::Button` / `tk::CheckButton` children instead of painting controls by hand. A clickable region should use the toolkit's hover feature.

## Recipes

### Add a user-visible string

Every string a user can see goes through the translation functions from `ui/shared/tk/i18n.h`:

```cpp
tk::tr("Microphone")                              // plain
tk::trf(tk::tr("{0} is typing…"), {name})         // with values, never concatenate
tk::trn(...)                                      // plurals
tk::N_("Dark")                                    // mark a literal in a table; pass it through tk::tr() where shown
```

Then:

1. Run `cmake --build build/linux-debug --target i18n-extract` to refresh `i18n/tesseract.pot` (needs `xgettext`).
2. Add a `msgid`/`msgstr` entry for the string to **every** `.po` file in `i18n/`. Run `i18n-pseudo` for `pseudo.po`.
3. Run ctest. `i18n_catalogs_complete` lists anything still missing and flags bare `tr()` or `_()` calls in the shells. Those always return English.

Strings outside `ui/` are not scanned unless their file is listed in `cmake/I18n.cmake`. Use `tk::format_date` and `tk::format_size` for dates and sizes.

### Add a setting

1. Add the field to `Settings` in `client/include/tesseract/settings.h`, and its load/save in `client/src/settings.cpp`.
2. Add a `SettingKey` entry in `ui/shared/views/settings/SettingBinder.h`.
3. In the right section under `ui/shared/views/settings/`, create the control and bind it:
   `binder.bind(control, &Settings::field, SettingKey::X);`
4. If changing it needs more than saving, add a `case` in `ShellBase_settings.cpp`. A per-window effect (widgets, client, policy) goes in `ShellBase::apply_setting_to_window_`, which runs once per window. A process-global effect (disk, proxy, font config, …) goes in `ShellBase::handle_setting_changed_`, which runs once.
5. Add its label strings (see above).

### Add a keyboard shortcut

1. In `ui/shared/views/shortcut_registry.h`, add a `ShortcutId`. In `shortcut_registry.cpp`, add its entry in `build()` with a `tk::N_("…")` description. That string needs `.po` entries (see above).
2. Handle it where the action lives, with `matches(ShortcutId::X, event)` (for example `MainAppWidget::handle_primary_shortcut_`).
3. The shells bind global accelerators from the registry (the Qt shell loops over it). On macOS, menu items bind specific IDs in `ui/macos/src/AppDelegate.mm`, so add a menu item there if it needs one.

The shortcuts overlay (Ctrl+/, or ⌘/ on macOS; F1 as well off macOS) lists every entry. Never hand-write a key binding in a shell.

### Add a Rust ↔ C++ call

1. Write the logic in `sdk/src/client/<subsystem>.rs`, with `#[cfg(test)]` unit tests beside it.
2. Declare it in the `extern "Rust"` block in `sdk/src/bridge.rs`. The bridge is synchronous. Async work runs on the tokio runtime inside the SDK.
3. Wrap it on `tesseract::Client` (declaration in `client/include/tesseract/client.h`, body in `client/src/client.cpp`).
4. For results that arrive later, add a callback in four places:
   - a method on `EventHandlerBridge` (`client/include/tesseract/event_handler_bridge.h` and `client/src/event_handler_bridge.cpp`), declared in the `extern "C++"` block of `bridge.rs`;
   - the matching method on `IEventHandler`;
   - the override in `EventHandlerBase`, which moves the call to the UI thread and calls a `handle_*_ui_()` function;
   - that `handle_*_ui_()` function on `ShellBase`, in `ui/shared/app/`.

### Add a new Matrix feature (MSC)

Check matrix-sdk first. If it already supports the feature, call it from `sdk/src/client/`. If it only needs event-content handling, add a module under `sdk/src/msc/`. After merging, update [MSCS.md](MSCS.md) (the `msc-support` skill regenerates it).

### Add a ShellBase member

`ShellBase` is split by subsystem into `ShellBase_<subsystem>.cpp`. Put new members in the file for their subsystem. When pulling a subsystem out into a collaborator, keep the fields shells read (`client_`, `account_manager_`, `main_room_pane_`, `rooms_`) on `ShellBase`, add no new pure virtuals (use `std::function` hooks), and keep `tests/cpp/shell_test_double.h` compiling unchanged.

### Add a test

- **C++:** add a `tests/cpp/test_<thing>.cpp` with Catch2 `TEST_CASE`s, and list it in `tests/CMakeLists.txt`. Each `TEST_CASE` becomes its own ctest test.
- **Rust:** a `#[cfg(test)] mod tests` in the file. The cxx bridge is excluded from the test build, so no C++ toolchain is needed.
- Tests never touch your real config or data. A test listener redirects the app directories for each test.

## Before you submit

- Build and run ctest and `cargo test -p tesseract-sdk-ffi` on your platform.
- Add one line to the **Unreleased** section of [CHANGES.md](../CHANGES.md): `type(scope): what changed in plain words. <what you built and tested>`. One sentence, about 150 characters. No class or file names, no root-cause story.
- Write commits as `type(scope): summary` (`fix(messages): …`, `feat(calls): …`). Put the explanation in the commit body.
- Keep the change focused. Style-only changes go in their own commit.
