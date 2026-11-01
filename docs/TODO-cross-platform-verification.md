# Cross-platform verification checklist

Features/fixes from the last ~3 weeks (since 2026-08-08) that landed
without full Qt6/GTK4/macOS verification — either because they were
written from a session with no toolchain for that platform, or because the
CHANGES.md entry never states a verification status at all. Worth working
through now that all machines (Linux, macOS, Windows) are reachable again.

Check items off as you confirm them live; leave a one-line note (what you
saw, what broke) next to anything that fails so it's easy to file as a
proper bug.

## Priority 1 — silently unverified, no stated status either way

- [x] **Progressive video streaming — macOS** (`f12e7fc3`, 2026-08-26):
      `AVAssetResourceLoaderDelegate` over a growable in-memory buffer.
      Confirmed broken 2026-08-29 (download visible, video never played) —
      root-caused and fixed in a separate session (`306e9d1f`):
      `TkVideoStreamLoader` answered AVFoundation's content-information
      request with `contentLength = 0` (no total is known that early) and
      then its `!contentType` guard blocked ever correcting it, plus a
      content-info-only request was never finished at all — AVFoundation
      concluded the resource was empty and waited forever. Fixed by
      leaving that request pending until the real total is known.
      User-verified on macOS. **Committed** (CHANGES.md/STATUS.md entries
      added retroactively — the fix commit itself didn't include them).
- [x] **Empty-placeholder text-area inset padding — macOS `NSTextView`**
      (`e61dd669`, 2026-08-28): compose bar / Create Room invite field
      height when empty vs. focused. Confirmed broken 2026-08-29 (the
      ~1px caret offset, plus three further compounding glitches found
      once someone actually dug in) — root-caused and fixed in a separate
      session (`3fed0071`): a draft's restored text rendered in the wrong
      font weight (`set_text()` never applied `typingAttributes`), the
      placeholder was a different widget class (`NSTextField`) than the
      real text (`NSTextView`) so their glyphs never aligned, the
      placeholder's height double-counted `textContainerInset`, and the
      blinking caret didn't appear until the first keystroke since its
      wiring only fired on an edit, not on the programmatic focus the
      compose bar's default-focus policy uses. User-verified live on
      macOS. **Committed.** GTK4's half of this commit turned out to be a
      much deeper problem — see the dedicated GTK4 section at the end of
      this file, still open there.
- [x] **Mention/highlight color bleed — macOS CoreText** (`32e68bb7`,
      2026-08-08): message text staying tinted after a mention/code span.
      **User-confirmed fixed 2026-08-29.**

## Priority 2 — macOS-only, explicitly flagged unverified

- [x] **Main window title tracks active room** (`a88516fb`/`80b61bd8`,
      2026-08-26/29) — user-confirmed live on all three: Qt6, GTK4, and
      now macOS.
- [x] **Tray: show window again when relaunched while hidden**
      (`56b667eb`, 2026-08-21) — `NSDistributedNotificationCenter` path.
      **User-confirmed working on macOS.**
- [x] **Room-list search field clear (×) button** (`3c6f260f`,
      2026-08-19) — macOS never wired `on_search_clear` before this.
      **User-confirmed working on macOS.** A second, distinct macOS
      search bug turned up alongside this one and was fixed separately
      (`d4a1cdf2`, not originally on this list): macOS's search wiring
      called `set_search_text` but dropped the shared `refresh_room_list_()`
      call every other shell made, so search neither repainted immediately
      nor widened its scope past root-level rooms. Fixed by hoisting the
      whole debounce/refresh wiring into `ShellBase::wire_main_app_widget_()`
      so all four shells share one implementation instead of four
      hand-duplicated ones. User-verified on macOS. **Committed**
      (CHANGES.md/STATUS.md entries added retroactively).

## Priority 3 — Qt6 + macOS, written with no toolchain at all

GTK4 re-checks for these same commits are folded into the dedicated GTK4
pass at the end of this file instead of listed here — see that section for
why.

- [x] **`tk::Widget` automatic relayout + relayout-pass suppression guard**
      (`bb2e1897`, 2026-08-27) — **User-confirmed working on Qt6 and
      macOS**, including the narrow/wide sidebar-resize freeze scenario
      the original commit found on Windows. Incidentally turned up and
      fixed two unrelated, pre-existing macOS bugs while testing
      (`2f71a620`: compose-bar text never wrapped; `ec1bf6f6`:
      ellipsis-trimmed text rendered left-aligned instead of centered) —
      both already carry their own CHANGES.md entries, no docs gap.
- [x] **Ctrl+Tab / Ctrl+Shift+Tab MRU room switcher** (`052ea233`,
      2026-08-27) — per-platform Ctrl-release detection (Qt6 event filter,
      GTK4 `key-released` signal, macOS `NSEventMaskFlagsChanged` monitor).
      **User-confirmed fully verified** — Qt6, GTK4, and macOS all working,
      alongside Windows' original build+run verification.
- [x] **Dismiss popups on window resize** (`e3d09019`, 2026-08-26) —
      mention/slash/emoji/account-picker/combo/date-picker. **User-confirmed
      working on Qt6 and macOS.** (GTK4 still needs its own check, folded
      into the dedicated GTK4 section below.)
- [x] **Full room-history export feature family** (`6c1fdb67` +
      `afa0fe22` + `32a7b5a3` + HTML restyle, 2026-08-16 → 2026-08-27) —
      the whole export dialog, in its entirety. **User-confirmed working
      on Qt6 and macOS**, alongside Windows' original build+test
      verification. (GTK4 still needs its own check, folded into the
      dedicated GTK4 section below.)

## Priority 4 — macOS live confirmation open (Qt6/GTK4 build+test verified)

- [ ] **DPI/HiDPI live scale-change handling** (`56a6ec11`, 2026-08-24) —
      Linux passed the full test suite; still needs a real HiDPI monitor
      (any platform) to confirm the live scale-change path, not just the
      cached-value path tests exercise.
- [ ] **Cold-start "No Internet Connection" dialog** (`8fa3b3ee`,
      2026-08-09) — needs an actual offline machine to trigger the
      `is_network_available()` probe path on Win32/macOS.
- [x] **"Clear all caches" rework** (2026-08-30) — now a local
      logout+login: `SyncService::expire_sessions()` → wipe
      state/event-cache/media stores → close pop-outs/tabs → re-restore in
      place. Root cause of "room list never comes back" was the
      sliding-sync `pos` surviving in the crypto store (not the state
      store); `expire_sessions()` resets it. Second bug: `restart_sdk_`
      loaded the raw SecretStore wrapper instead of the unwrapped session
      JSON — fixed to use `load_account_with_key`. **User-verified on
      Linux 2026-08-30**: the wiped `@surak:gnomos.org` account recovered
      its full room list on the next clear, no errors.
      Still pending elsewhere: (1) GTK4 — same live check;
      (2) Windows — confirm the SDK store dir actually shrinks after a
      clear (`client.pause()` releasing the `-wal`/`-shm` handles is why
      the deletes now work where the old plain-drop failed);
      (3) macOS — same as Linux.

## Priority 5 — landed 2026-08-31, no on-platform build yet

- [ ] **Native text controls inset 2px inside their border box**
      (`96f7aa44`, 2026-08-31) — `tk::TextField` / `tk::TextArea` now
      default `overlay_inset_` to 2px instead of relying on the Windows
      shell's lone `set_overlay_inset` call. Fixes the Qt6 symptom where
      the opaque native capture painted over the field's border and
      squared its rounded corners (global search, emoji/sticker pickers,
      ~20 views). Linux build + full test suite verified; needs a live
      look on Qt6 (the platform the bug was reported on), plus GTK4 and
      macOS to confirm nothing regressed at the new inset. `ComposeBar`
      opts back out — verify its text/placeholder still sit right.
- [ ] **Login surface no longer covers the custom title bar — Windows**
      (`faeb2e62`, 2026-08-31) — `LoginView::layout()` was resetting its
      HWND to `(0,0)` every call, undoing `MainWindow::on_size()`'s
      `(0, TITLEBAR_H)` offset. Written from a Linux session, unbuilt.
- [ ] **BetterText D2D device recovers on `DEVICE_HUNG` — Windows**
      (`bbf1461e`, 2026-08-31) — the native text control's own D3D device
      only reset on `DEVICE_REMOVED` / `DEVICE_RESET`; `DEVICE_HUNG` (the
      common hybrid-GPU TDR case) left it silently dead. Now matches
      `canvas_d2d.cpp`'s `is_device_lost_hr()`. Written from a Linux
      session, unbuilt — hard to trigger without a hybrid-graphics laptop
      forcing a TDR.

---

## GTK4 — needs its own dedicated pass, not folded into spot-checks

2026-08-29: two GTK4-specific investigations this session turned up
problems well beyond what a quick verification pass expects, one of them
a total regression in ordinary (non-streaming) video playback that had
apparently gone unnoticed since `da488022` landed. That, plus how long the
second investigation ran without fully resolving, is reason enough to
treat GTK4 as its own focused effort rather than one item among many.

- [x] **Progressive video streaming, and by extension all video playback**
      (`da488022`, 2026-08-26; fixed `761be8d6`/`09dd6acf`): `finish_pipeline_and_start_()`
      created `audioconvert`/`videoconvert` with a `nullptr` name, so
      GStreamer auto-suffixed them (`videoconvert0`), and `on_pad_added_()`'s
      exact-name `gst_bin_get_by_name()` lookup (plus its separately-broken
      longname-prefix fallback) never found them — every decoded pad went
      permanently unlinked, breaking *all* GTK4 video playback (buffered
      `play()` included), not just streaming. Confirmed via `GST_DEBUG`
      capture (`qtdemux0`: "streaming stopped, reason not-linked") and a
      standalone `gst-launch-1.0` naming test. Fixed by naming the elements
      explicitly and correcting the fallback to compare short factory
      names. User-verified live. **Committed.**
- [ ] **Dismiss popups on window resize** (`e3d09019`, 2026-08-26) —
      mention/slash/emoji/account-picker/combo/date-picker. Verified
      working on Qt6 and macOS; GTK4 still needs its own check, held here
      rather than in Priority 3 above given this session's track record
      with GTK4-specific bugs.
- [ ] **Full room-history export feature family** (`6c1fdb67` +
      `afa0fe22` + `32a7b5a3` + HTML restyle, 2026-08-16 → 2026-08-27) —
      the whole export dialog. Verified working on Qt6 and macOS; GTK4
      still needs its own check, held here for the same reason as above.
- [ ] **`GtkNativeTextArea` (compose bar / Create Room invite field) —
      empty-placeholder height + stale-content rendering**
      (`e61dd669`, 2026-08-28): **unresolved after 5 fix attempts this
      session; 2 of those attempts regressed a fix that had already been
      confirmed working.** Two distinct symptoms:
      1. Type one character, delete it → the field's reserved height
         balloons and stays there. Root-caused: `on_changed_cb()`/
         `set_text()` toggle `placeholder_label_`'s visibility, then
         synchronously call `gtk_widget_measure()` on it in the same
         call — GTK hasn't allocated the just-shown widget yet at that
         point, and `gtk_widget_measure()` can return wildly wrong values
         in that state (observed live: 858px for a one-line placeholder).
         Deferring the height re-check to the existing capture idle
         callback (`on_capture_idle_cb`) **fixed this specific symptom**.
         Committed (`86385e08`).
      2. The last character typed before deleting stays visibly rendered,
         overlapping the placeholder text once it reappears.
         `placeholder_label_` is a real, immediately-rendered native GTK
         widget, while the actual typed text is a canvas-drawn snapshot
         (`cached_image_`) that only refreshes on a deferred idle tick —
         the two layers can disagree for a stretch. Attempting to also
         defer the placeholder's visibility toggle into the same (or a
         chained second) idle callback, to keep both layers in sync,
         regressed fix #1 twice in a row instead of fixing this — reverted
         both times. **This symptom is still open.**
      - Current state: fix #1 (height growth) is committed (`86385e08`);
        the visibility/capture-ordering experiments for fix #2 were
        reverted back out before that commit and aren't present in the
        tree.
      - Given the video bug above was *also* a fundamental architecture
        interaction (not a typo), this smells like the GTK4 backend's
        native-widget-captured-to-canvas approach (`GtkNativeTextField`/
        `GtkNativeTextArea`'s opacity-0 + `gtk_widget_snapshot_child()`
        spike, shared by every native text control) has more of these
        timing assumptions baked in than anyone's audited. Worth a
        dedicated review of that whole mechanism rather than more
        one-symptom-at-a-time patches.

---

*Generated 2026-08-29 from a commit/CHANGES.md/STATUS.md audit of the prior
three weeks. Delete or trim entries as they're confirmed; this file isn't
meant to be permanent.*
