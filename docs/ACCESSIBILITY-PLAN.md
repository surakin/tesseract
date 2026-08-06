# Accessibility implementation plan

> **Status note (2026-08-06):** this plan was approved in plan mode on
> 2026-08-01 and is preserved here as the design record. Phases 0–2's
> foundation work landed largely as written (`Role`/`AccessState` on
> `tk::Widget`, `ui/shared/tk/access_tree.{h,cpp}`, Linux-first rollout).
> The **AccessKit integration layer described below was abandoned**: the
> Phase 0 spike hit "an unfixable two-application problem on both
> toolkits" (per the `2b052962` commit message) and the project pivoted
> to real native bridges instead — a cached `QAccessibleInterface` tree
> for Qt6 (`ui/linux-qt/tk/qt_accessible.cpp`) and per-node `GtkAccessible`
> widgets for GTK4 (`ui/linux-gtk/tk/gtk_accessible.cpp`), landed in
> `2b052962` and `193edf02`. No `access/` crate exists. Treat every
> AccessKit-specific detail below as historical context, not current
> architecture; the `Role`/`AccessState`/tree-walker model and the
> phase/rollout structure are still the accurate plan of record.

## Context

Tesseract currently has zero accessibility (screen-reader/AT) support on any platform — flagged as a known Tier-5 gap in `ROADMAP.md`. An initial hand-rolled-bridge estimate put this at 5–7 months solo. Research into [AccessKit](https://accesskit.dev) — a Rust-core, cross-platform accessibility-tree library purpose-built for custom-painted UIs (used by egui, Slint, Xilem/Bevy) — cut that to ~3–4 months, because its Windows adapter has a `SubclassingAdapter` mode built for apps that own a raw `HWND` outside `winit` (matches Tesseract's Win32 shell exactly), and its Unix adapter implements AT-SPI directly over D-Bus independent of GTK/Qt — meaning Tesseract's Qt6 and GTK4 Linux shells can share **one** integration instead of two. The user asked for a full implementation plan built on this approach.

This is a multi-month initiative. The plan below is detailed for the parts that start immediately (spike → foundation → first platform), and lighter-touch for later phases that depend on decisions made along the way.

## Integration path: new sibling Rust crate `access/`

Add a new crate (`tesseract-access-ffi`, directory `access/`), sibling to `sdk/`, wired into the CMake build via Corrosion exactly like `sdk/` is today (`corrosion_import_crate` walks the root `Cargo.toml`'s `[workspace] members`, currently `["sdk"]`; add `"access"`). Use `corrosion_add_cxxbridge(tesseract_access_bridge_cxx CRATE tesseract_access_ffi FILES bridge.rs)`, mirroring the existing `tesseract_sdk_bridge_cxx` target.

**Why a separate crate, not folded into `sdk/`, and not raw C bindings:**
- `sdk/` is explicitly reserved for async I/O (`CLAUDE.md`: "All async I/O lives here... a `tokio` runtime runs in background threads"). Accessibility is synchronous UI-thread work triggered by relayout/paint/focus-change — different lifecycle, and coupling it to `matrix-sdk`'s heavy build (encryption/webrtc feature flags) would force irrelevant recompiles both directions.
- `cxx` (already proven via `sdk/src/lib.rs`'s `#[cxx::bridge]`) gives the same ergonomics AccessKit's raw C bindings would require hand-rolling (`Vec`/`String`/opaque boxed types across the FFI boundary) — reuse the pattern instead of a second FFI style.
- The new crate stays a thin adapter shim: **UI semantics (which widget maps to which AT role) stay in C++**, in `ui/shared/tk/`, where `children()`/`bounds()`/theme are available. The Rust crate's only job is: take an already-built node list from C++ and push it through the platform adapter (`accesskit_windows`/`accesskit_macos`/`accesskit_unix`).

## Phase 0 — Spike (2–3 days, do this before anything else)

Goal: prove the integration path end-to-end on Linux, since the shared Unix/AT-SPI adapter validates both the Qt6 and GTK4 shells from one integration.

1. `access/Cargo.toml` with `accesskit`, `accesskit_unix`, `cxx`; add `"access"` to root `Cargo.toml` workspace members.
2. Minimal `access/src/lib.rs` `cxx::bridge` — hardcode a single node (root + one labeled button), no generic widget-walking yet.
3. Wire `corrosion_add_cxxbridge` in root `CMakeLists.txt`, link only into `ui/linux-qt` for the spike.
4. In `ui/linux-qt/tk/host_qt.h`/`.cpp`, construct the adapter alongside `Surface`'s existing `host_` member; use AccessKit's `ActivationHandler` so the tree only builds when AT-SPI is actually listening (avoid always-on cost).
5. **Done criterion (user-verified — cannot be agent-verified per this project's no-run-the-app rule)**: the user runs Orca against a built `linux-qt` binary; the hardcoded button is announced with correct role/label, and activating it via AT-SPI fires the existing click callback.
6. Fallback if AT-SPI/D-Bus hits unexpected friction: spike the Windows `SubclassingAdapter` instead before committing to Phase 1's generic API design.

## Phase 1 — Foundation: role/name/state model on `tk::Widget`

Add to `ui/shared/tk/widget.h`, following the existing pattern of virtuals-with-safe-defaults already used by `focusable()`/`focus_on_click()`:

```cpp
enum class Role { None, Button, CheckBox, TextInput, StaticText, List, ListItem,
                   Tab, TabList, Dialog, MenuItem, Image, Link, Group /* grows incrementally */ };
struct AccessState { bool checked=false, expanded=false, selected=false, busy=false; };

virtual Role access_role() const { return Role::None; }   // None = excluded from AT tree; opt-in per widget
virtual std::string access_name() const { return {}; }     // Button/Label can default from their existing label_
virtual AccessState access_state() const;                  // default reads enabled_/has_focus_, already on Widget
```

`Role::None` as default means this ships without every one of the ~120 widget/view classes needing an immediate override — mapping is incremental (Phase 4).

For the long tail (live regions, custom actions, set-size/position-in-set for list rows), use an optional interface discovered via `dynamic_cast`, following the existing `ScrollableRegion` precedent (`widget.h:765`, already used by `scroll_widget_into_view()` walking up `parent()` via `dynamic_cast<ScrollableRegion*>`) — avoids bloating every `Widget` subclass with rarely-used virtuals.

**Tree walker**: new `ui/shared/tk/access_tree.{h,cpp}` with `build_access_tree(Widget* root)`, mirroring the existing focus-order walk that backs `next_focusable()` — same `visible_in_tree()` filtering, same traversal order. This is deliberate: AT reading order should match Tab order by construction (divergence between visual/Tab/AT order is a classic accessibility bug class), and there's already one canonical tree-walk to reuse rather than inventing a second one that could drift.

**Virtualized lists** (`MessageListView`/`RoomListView`): extend the existing `on_visible_range_changed` callback (`ui/shared/views/MessageListView.h:656`) to also trigger a partial AT update — only realized rows become AT nodes. Populate AccessKit's set-size/position-in-set node properties from the adapter's existing row-count/index so a screen reader can announce "item 12 of 340" without realizing off-screen rows (same shape as `aria-setsize`/`aria-posinset` on virtualized web lists).

## Phase 2 — First full platform bridge: Linux (Qt6 + GTK4 together)

Continue directly from the spike — two shells sharing one adapter is the strongest test that Phase 1's walker is genuinely shared, not accidentally single-platform-shaped.

- `ui/linux-qt/tk/host_qt.h/.cpp`, `ui/linux-gtk/tk/host_gtk.h/.cpp`: `Surface` gets a member owning the adapter handle (the new crate's `cxx` opaque type), constructed alongside `host_`.
- **Update hook**: reuse `Surface::set_on_layout(std::function<void()> cb)` — already present on all four `Surface` classes, documented as firing "at the tail of every relayout" and currently used to keep native text overlays aligned (`host_qt.h:71`). Register a callback there that rebuilds/diffs the AT tree.
- **Update frequency**: do not push a full tree diff on every relayout — scroll/typing can relayout many times/sec. Mirror the existing `request_repaint()` vs `request_relayout()` split on `Host` (`host.h:357-431`, `request_relayout()` documented as heavier than `request_repaint()`'s "redraw only, reusing whatever geometry"): (1) structural diffs (nodes added/removed, role changes) only when the node-id set actually changes; (2) narrow value/state-only pushes (focus moved, a checkbox toggled, live-region text changed) fired directly from the mutation site, not gated on relayout.
- **Focus events**: hook exactly where `Host` flips `has_focus_` — `widget.h:640` documents this as the sole authority point ("Only Host may flip has_focus_"). This is also where the project's known focus-system gotchas live (`focusable()`/`focus_on_click()` split, `claim_native_focus_container_()`, Qt's Tab-interception override at `host_qt.h:77-84`) — land AT focus wiring in the same review pass as those, not bolted on separately.

## Phase 3 — Rollout to remaining platforms

- **Windows**: AccessKit's `SubclassingAdapter` against the raw `HWND` `ui/windows/tk/host_win32.h`'s `Surface` already owns; same `set_on_layout` hook pattern.
- **macOS**: AccessKit's macOS adapter attaches to the single `NSView` subclass `ui/macos/tk/host_macos.h`'s `Surface` owns; same hook pattern (the shell being composition-based, `MacShell : ShellBase`, is a shell-level detail, not a Surface-level one).
- Payoff of doing Linux first: the `Role`/`AccessState` model and tree walker are already shared and debugged against two different `Host`/`Surface` implementations — Phase 3 is "only" two more thin adapter-attachment points, not a redesign.

## Phase 4 — Widget-by-widget mapping (prioritized, incremental — runs over weeks)

1. **Core navigation chrome** first — `RoomListView`, `TabBar`/`TabView`/`SideTabView` — nothing else is reachable non-visually without this.
2. **Core chat loop** — `MessageListView` (hardest case, below), `ComposeBar`, `RoomView` chrome.
3. **Secondary/frequent** — `ThreadView`/`ThreadListView`, `UserInfo`, `SettingsView` — mostly `Label`/`Button`/`CheckButton`/`ComboBox` composites that get role+name "for free" once those base classes override `access_role()`/`access_name()`.
4. **Grid/media pickers** — `EmojiPicker`/`StickerPicker` (`TabbedGridPicker` base): genuinely hard, needs `Role::Grid`/`GridCell` with row/col properties and roving-tabindex-style arrow-key navigation — doesn't fall out of the simple model.
5. **Overlays/dialogs** — `ImageViewerOverlay`, `VideoViewerOverlay`, `ShortcodePopup`, `JoinRoomView`, `AccountPicker`, `ListPopupBase`-derived popups — lower traffic, tackle last; reuse existing popup-dismiss/focus-return logic for modal semantics rather than reinventing it.

**Two product decisions to flag for explicit sign-off when reached** (not pure engineering calls):
- **`MessageListView` per-message AT structure**: one flattened node per message bubble (sender+timestamp+body as a single `access_name`) vs. a subtree (sender as header, body as static text, reactions as a group of toggle buttons, read receipts as a live region). Leaning toward flattened bubble + subtree only for the interactive/dynamic parts, to avoid AT-tree explosion — but this determines the actual reading experience and should be a product call, not just an engineering default.
- **`ComposeBar`**: check early whether its text buffer is still routed through the native `tk::NativeTextArea` overlay (per `CLAUDE.md`, text input stays native "so IME and selection behave correctly per-OS"). If so, the OS-native control (real Win32 EDIT/NSTextView/QLineEdit/GtkEntry) may already have working AT support with zero AccessKit involvement — shrinking `ComposeBar`'s burden to only the hand-painted chrome around it (attachments, mention autocomplete, formatting toolbar). Cheap to verify, changes scope significantly.
- Every new accessible-name string must go through `tk::tr()`/`trn()`/`trf()` with `.po` entries added to `i18n/es.po` and `i18n/pseudo.po`, per existing i18n policy.

## Phase 5 — Broader accessibility (parallel track, no AccessKit dependency)

Can run in a separate session/track with no ordering dependency on the AT-tree work:
- Keyboard-only operability audit — verify every interactive path is reachable via `next_focusable()`/Tab and has a visible focus ring (`paint_own_focus_ring()` already exists).
- High-contrast theming — extend the existing `Theme::light()`/`dark()` factory with a high-contrast variant.
- Reduced-motion — audit `Animator`/`AnimImageCache`/animated-image decode paths for an app-settings toggle.
- Text scaling is already shipped (v0.8.5) — spot-check only.

## Testing Strategy

**Agent/CI-verifiable:**
- Build success across CMake presets after each phase (never run the GTK4 target or launch built binaries, per project workflow rules).
- Catch2/ctest unit tests for pure-logic pieces: `access_tree.cpp`'s walk/diff logic against widget-tree fixtures, `Role`/`AccessState` defaulting, virtualized-list set-size/position-in-set math.
- `cargo test -p tesseract-access-ffi` for pure-Rust logic in the new crate, mirroring `cargo test -p tesseract-sdk-ffi`.

**Strictly user-verified** (real screen readers aren't automatable, and per established workflow the user does hands-on cross-platform testing personally):
- Actually driving NVDA/Narrator, VoiceOver, or Orca against a built binary. This is the Phase 0 done-criterion and every subsequent phase's real acceptance test. Explicit user checkpoints at the end of Phase 0, Phase 2, and each Phase 3 platform — do not claim a phase "works" without this.

## Critical Files (foundation + first platform bridge)

- `ui/shared/tk/widget.h` — `Role`/`AccessState` virtuals on `Widget`
- `ui/shared/tk/widget.cpp` — new `access_tree` walk alongside existing focus-order walk
- `ui/shared/tk/host.h` — focus-authority hook point, `request_repaint`/`request_relayout`-style tiering for AT push frequency
- `CMakeLists.txt` (root) — new `access/` crate wiring (`corrosion_import_crate`/`corrosion_add_cxxbridge`)
- `Cargo.toml` (root) — add `"access"` workspace member
- `access/Cargo.toml`, `access/src/lib.rs` — new crate
- `ui/linux-qt/tk/host_qt.h`/`.cpp`, `ui/linux-gtk/tk/host_gtk.h`/`.cpp` — first platform bridge attachment via existing `set_on_layout`
- `ui/shared/views/MessageListView.h` — `on_visible_range_changed` extension for virtualized AT rows

## Verification

- Phase 0 and Phase 2: build succeeds via `cmake --preset linux-debug && cmake --build build/linux-debug`; `cargo test -p tesseract-access-ffi`; then a user checkpoint with Orca against the built binary before calling either phase done.
- Phase 1: `ctest --test-dir build/linux-debug --output-on-failure` covering the new tree-walk/diff unit tests.
- Phase 3: same build+test loop per platform, each ending in a user checkpoint with the platform's native screen reader (Narrator/NVDA on Windows, VoiceOver on macOS).
- Phase 4/5: incremental — verify each newly-mapped widget class against the relevant screen reader as it's completed, rather than batching verification to the end.
