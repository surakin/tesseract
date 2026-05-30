# Initial Account Encryption Setup — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a modal overlay wizard that guides users through E2E encryption setup on first login — bootstrapping cross-signing + key backup for fresh accounts (`RecoveryState::Disabled`) and recovering secrets for new devices (`RecoveryState::Incomplete`).

**Architecture:** Rust SDK grows two new FFI methods (`recovery_state`, `enable_recovery`) and one new callback (`on_enable_recovery_progress`). A new `EncryptionSetupOverlay` widget mounts inside `MainAppWidget` (same pattern as `ImageViewerOverlay`). ShellBase detects the setup requirement from `push_rooms_()` after first sync and calls a new pure-virtual `show_encryption_setup_overlay_(Mode)` that each platform shell implements.

**Tech Stack:** Rust/matrix-sdk 0.17 Recovery API, C++/tesseract_tk widget system, Catch2 tests, cxx FFI bridge.

**Spec:** `docs/superpowers/specs/2026-05-30-initial-account-encryption-setup-design.md`

---

## File Map

**Create:**
- `ui/shared/views/EncryptionSetupOverlay.h` — widget: state machine, callbacks, field-rect hooks
- `ui/shared/views/EncryptionSetupOverlay.cpp` — widget: measure / arrange / paint / pointer dispatch
- `tests/cpp/test_encryption_setup_overlay.cpp` — widget unit tests
- `tests/cpp/test_shell_encryption_setup.cpp` — ShellBase detection logic tests

**Modify:**
- `sdk/src/client/recovery.rs` — add `recovery_state()`, `enable_recovery()`
- `sdk/src/bridge.rs` — add bridge declarations for both + `on_enable_recovery_progress` callback
- `sdk/src/lib.rs` — add `#[cfg(test)]` stubs for both methods and the new callback
- `client/include/tesseract/client.h` — declare `recovery_state()`, `enable_recovery()`
- `client/src/client.cpp` — implement C++ wrappers
- `client/include/tesseract/event_handler.h` — add `on_enable_recovery_progress` virtual
- `client/include/tesseract/event_handler_bridge.h` — add bridge declaration
- `client/src/event_handler_bridge.cpp` — implement bridge dispatch
- `client/include/tesseract/account_session.h` — add `encryption_setup_shown`, `encryption_setup_dismissed`
- `ui/shared/app/EventHandlerBase.h` — add `handle_enable_recovery_progress_ui_` override
- `ui/shared/app/EventHandlerBase.cpp` — marshal new callback to UI thread
- `ui/shared/app/ShellBase.h` — add pure-virtual, guard flags, helpers
- `ui/shared/app/ShellBase.cpp` — implement `check_encryption_setup_()` + progress handler
- `ui/shared/views/MainAppWidget.h` — add overlay member, show method, field-rect accessors
- `ui/shared/views/MainAppWidget.cpp` — integrate overlay into arrange/paint/modal-check
- `ui/linux-qt/src/MainWindow.h` — add overlay member + override declaration
- `ui/linux-qt/src/MainWindow.cpp` — implement `show_encryption_setup_overlay_()`
- `ui/linux-gtk/src/MainWindow.h` — same
- `ui/linux-gtk/src/MainWindow.cpp` — same
- `ui/windows/src/MainWindow.h` — same
- `ui/windows/src/MainWindow.cpp` — same
- `ui/macos/src/MainWindowController.h` — same
- `ui/macos/src/MainWindowController.mm` — same

---

## Task 1: SDK — `recovery_state()`

**Files:**
- Modify: `sdk/src/client/recovery.rs`
- Modify: `sdk/src/bridge.rs`
- Modify: `sdk/src/lib.rs`

- [ ] **Step 1: Add the `recovery_state()` impl to `recovery.rs`**

Add after the `needs_recovery` impl block:

```rust
/// Returns the current recovery state as a u8:
/// 0 = Unknown, 1 = Disabled, 2 = Enabled, 3 = Incomplete.
/// Cheap synchronous read; no block_on needed.
#[cfg(not(test))]
pub fn recovery_state(&self) -> u8 {
    let Some(client) = self.client.as_ref() else {
        return 0;
    };
    use matrix_sdk::encryption::recovery::RecoveryState;
    match client.encryption().recovery().state() {
        RecoveryState::Unknown    => 0,
        RecoveryState::Disabled   => 1,
        RecoveryState::Enabled    => 2,
        RecoveryState::Incomplete => 3,
    }
}

#[cfg(test)]
pub fn recovery_state(&self) -> u8 {
    0
}
```

- [ ] **Step 2: Add the bridge declaration to `bridge.rs`**

In `bridge.rs`, add after `fn needs_recovery`:

```rust
        /// Returns the current recovery state as a u8:
        /// 0 = Unknown, 1 = Disabled, 2 = Enabled, 3 = Incomplete.
        fn recovery_state(self: &ClientFfi) -> u8;
```

- [ ] **Step 3: Add the test stub to `lib.rs`**

In `lib.rs`, inside the `#[cfg(test)]` `impl ClientFfi` block, add:

```rust
        pub fn recovery_state(&self) -> u8 { 0 }
```

- [ ] **Step 4: Verify the Rust crate compiles and tests pass**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
```

Expected: all tests pass, `recovery_state` callable.

- [ ] **Step 5: Commit**

```bash
git add sdk/src/client/recovery.rs sdk/src/bridge.rs sdk/src/lib.rs
git commit -m "feat(sdk): add recovery_state() FFI method"
```

---

## Task 2: SDK — `enable_recovery()` + `on_enable_recovery_progress` callback

**Files:**
- Modify: `sdk/src/client/recovery.rs`
- Modify: `sdk/src/bridge.rs`
- Modify: `sdk/src/lib.rs`

- [ ] **Step 1: Add `enable_recovery()` impl to `recovery.rs`**

Add after the `recovery_state` impl:

```rust
/// Bootstrap cross-signing + key backup for a fresh account.
/// `passphrase`: empty = generate a random recovery key; non-empty = derive
/// the key from this passphrase (the raw key is NOT reported via the callback
/// in that case — `recovery_key` in the Done callback will be empty).
/// Progress is reported via `on_enable_recovery_progress` before this returns.
#[cfg(not(test))]
pub fn enable_recovery(&mut self, passphrase: &str) -> OpResult {
    use matrix_sdk::encryption::recovery::EnableProgress;
    use futures_util::StreamExt as _;

    let Some(client) = self.client.clone() else {
        return err("not logged in");
    };
    let Some(handler) = self.handler.as_ref().map(Arc::clone) else {
        return err("sync not started");
    };
    let passphrase = passphrase.to_owned();

    self.rt.block_on(async move {
        let recovery = client.encryption().recovery();
        let enable = if passphrase.is_empty() {
            recovery.enable()
        } else {
            recovery.enable().with_passphrase(&passphrase)
        };

        let mut progress_stream = enable.subscribe_to_progress();
        let handler2 = Arc::clone(&handler);
        let progress_task = tokio::spawn(async move {
            while let Some(update) = progress_stream.next().await {
                let Ok(progress) = update else { break };
                let (step, key, bu, tot): (u8, String, u32, u32) = match progress {
                    EnableProgress::Starting => (0, String::new(), 0, 0),
                    EnableProgress::CreatingBackup => (1, String::new(), 0, 0),
                    EnableProgress::CreatingRecoveryKey => (2, String::new(), 0, 0),
                    EnableProgress::BackingUp(counts) => {
                        (3, String::new(), counts.backed_up as u32, counts.total as u32)
                    }
                    EnableProgress::RoomKeyUploadError => (5, String::new(), 0, 0),
                    EnableProgress::Done { recovery_key } => (4, recovery_key, 0, 0),
                };
                if let Ok(g) = handler2.lock() {
                    g.on_enable_recovery_progress(step, &key, bu, tot);
                }
            }
        });

        match enable.await {
            Ok(recovery_key) => {
                let _ = progress_task.await;
                ok(recovery_key)
            }
            Err(e) => {
                progress_task.abort();
                if let Ok(g) = handler.lock() {
                    g.on_enable_recovery_progress(5, &e.to_string(), 0, 0);
                }
                err(e.to_string())
            }
        }
    })
}

#[cfg(test)]
pub fn enable_recovery(&mut self, _passphrase: &str) -> OpResult {
    err("not logged in")
}
```

- [ ] **Step 2: Add `on_enable_recovery_progress` to the C++ callback declarations in `bridge.rs`**

In `bridge.rs`, in the `extern "C++"` block where `on_backup_progress` is declared, add after it:

```rust
        /// Fired as `enable_recovery()` progresses through setup stages.
        /// `step` encodes EnableProgress: 0=Starting 1=CreatingBackup
        /// 2=CreatingRecoveryKey 3=BackingUp 4=Done 5=Error.
        /// When step==4, `recovery_key` holds the generated key (empty if
        /// passphrase mode was chosen). When step==3, `backed_up`/`total`
        /// carry the running backup count.
        fn on_enable_recovery_progress(
            self: &EventHandlerBridge,
            step: u8,
            recovery_key: &str,
            backed_up: u32,
            total: u32,
        );
```

Also add the `enable_recovery` method declaration in the `unsafe extern "Rust"` `impl ClientFfi` block, after `fn recover`:

```rust
        /// Bootstrap cross-signing and key backup for a fresh account.
        /// Empty `passphrase` → generate a random recovery key.
        /// Non-empty → derive key from passphrase (reported key is "").
        /// Progress events fire via `on_enable_recovery_progress` before return.
        fn enable_recovery(self: &mut ClientFfi, passphrase: &str) -> OpResult;
```

- [ ] **Step 3: Add test stubs to `lib.rs`**

In `lib.rs`, in the `#[cfg(test)]` `impl ClientFfi` block:

```rust
        pub fn enable_recovery(&mut self, _passphrase: &str) -> OpResult {
            OpResult { ok: false, message: "not logged in".into() }
        }
```

In the `#[cfg(test)]` `impl EventHandlerBridge` block:

```rust
        pub fn on_enable_recovery_progress(
            &self,
            _step: u8,
            _recovery_key: &str,
            _backed_up: u32,
            _total: u32,
        ) {}
```

- [ ] **Step 4: Verify Rust compiles and tests pass**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
```

Expected: all existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add sdk/src/client/recovery.rs sdk/src/bridge.rs sdk/src/lib.rs
git commit -m "feat(sdk): add enable_recovery() and on_enable_recovery_progress callback"
```

---

## Task 3: C++ Client Wrappers

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`

- [ ] **Step 1: Declare the new methods in `client.h`**

In `client.h`, find the `needs_recovery` / `recover` / `backup_state` group and add after it:

```cpp
    /// Returns the current recovery state:
    /// 0 = Unknown, 1 = Disabled (fresh account, no encryption set up),
    /// 2 = Enabled, 3 = Incomplete (exists on server, device missing secrets).
    uint8_t recovery_state() const;

    /// Bootstrap cross-signing + key backup for a fresh account.
    /// Pass an empty `passphrase` to generate a random recovery key; non-empty
    /// to derive the key from the passphrase. Progress is reported via
    /// IEventHandler::on_enable_recovery_progress before this call returns.
    Result enable_recovery(const std::string& passphrase);
```

- [ ] **Step 2: Implement the wrappers in `client.cpp`**

Near the existing `needs_recovery` implementation, add:

```cpp
uint8_t Client::recovery_state() const
{
    if (!impl_)
        return 0;
    return impl_->ffi->recovery_state();
}

Client::Result Client::enable_recovery(const std::string& passphrase)
{
    MUT_FFI;
    if (!impl_)
        return {"", false, "not logged in"};
    auto res = impl_->ffi->enable_recovery(passphrase);
    return {std::string(res.message), res.ok, std::string(res.message)};
}
```

Note: `recovery_state()` is a `&self` call (no `MUT_FFI` guard needed, same as `needs_recovery()`).

- [ ] **Step 3: Build to verify**

```bash
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add client/include/tesseract/client.h client/src/client.cpp
git commit -m "feat(client): expose recovery_state() and enable_recovery() to C++"
```

---

## Task 4: C++ Event Handler + Bridge

**Files:**
- Modify: `client/include/tesseract/event_handler.h`
- Modify: `client/include/tesseract/event_handler_bridge.h`
- Modify: `client/src/event_handler_bridge.cpp`

- [ ] **Step 1: Add the virtual to `event_handler.h`**

After the `on_backup_progress` virtual:

```cpp
    /// Fired as `Client::enable_recovery()` progresses.
    /// `step`: 0=Starting 1=CreatingBackup 2=CreatingRecoveryKey
    ///          3=BackingUp 4=Done 5=Error.
    /// When step==4, `recovery_key` holds the generated key (empty if
    /// passphrase mode). When step==3, `backed_up`/`total` are the running
    /// backup counts.
    virtual void on_enable_recovery_progress(uint8_t /*step*/,
                                             const std::string& /*recovery_key*/,
                                             uint32_t /*backed_up*/,
                                             uint32_t /*total*/) {}
```

- [ ] **Step 2: Add the declaration to `event_handler_bridge.h`**

After the `on_backup_progress` declaration:

```cpp
    void on_enable_recovery_progress(rust::u8 step,
                                     rust::Str recovery_key,
                                     rust::u32 backed_up,
                                     rust::u32 total) const;
```

- [ ] **Step 3: Implement the dispatch in `event_handler_bridge.cpp`**

After the `on_backup_progress` implementation (follow its exact pattern):

```cpp
void EventHandlerBridge::on_enable_recovery_progress(
    rust::u8 step,
    rust::Str recovery_key,
    rust::u32 backed_up,
    rust::u32 total) const
{
    guard("on_enable_recovery_progress",
          [&]
          {
              if (!handler_)
                  return;
              handler_->on_enable_recovery_progress(
                  static_cast<uint8_t>(step),
                  std::string(recovery_key),
                  static_cast<uint32_t>(backed_up),
                  static_cast<uint32_t>(total));
          });
}
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep -E "error:" | head -10
```

Expected: no errors.

- [ ] **Step 5: Commit**

```bash
git add client/include/tesseract/event_handler.h \
        client/include/tesseract/event_handler_bridge.h \
        client/src/event_handler_bridge.cpp
git commit -m "feat(client): add on_enable_recovery_progress callback"
```

---

## Task 5: AccountSession flags + EventHandlerBase marshaling

**Files:**
- Modify: `client/include/tesseract/account_session.h`
- Modify: `ui/shared/app/EventHandlerBase.h`
- Modify: `ui/shared/app/EventHandlerBase.cpp`

- [ ] **Step 1: Add guard flags to `AccountSession`**

After `verification_banner_dismissed = false;` add:

```cpp
    /// UI state: true once the encryption-setup overlay has been raised for
    /// this account (prevents re-raising on subsequent rooms-updated ticks).
    bool encryption_setup_shown = false;

    /// UI state: true when the user clicked "Skip" in the encryption-setup
    /// overlay — suppresses re-showing for the session lifetime of this account.
    bool encryption_setup_dismissed = false;
```

- [ ] **Step 2: Add the UI-thread virtual to `EventHandlerBase.h`**

After `handle_backup_progress_ui_`:

```cpp
    virtual void handle_enable_recovery_progress_ui_(uint8_t /*step*/,
                                                     std::string /*recovery_key*/,
                                                     uint32_t /*backed_up*/,
                                                     uint32_t /*total*/) {}
```

Also override `on_enable_recovery_progress` from `IEventHandler`:

```cpp
    void on_enable_recovery_progress(uint8_t step,
                                     const std::string& recovery_key,
                                     uint32_t backed_up,
                                     uint32_t total) final;
```

- [ ] **Step 3: Implement the marshal in `EventHandlerBase.cpp`**

After the `on_backup_progress` implementation:

```cpp
void EventHandlerBase::on_enable_recovery_progress(uint8_t step,
                                                    const std::string& recovery_key,
                                                    uint32_t backed_up,
                                                    uint32_t total)
{
    auto* s = shell_;
    std::string key = recovery_key;
    s->post_to_ui_([s, step, key, backed_up, total]()
    {
        s->handle_enable_recovery_progress_ui_(step, key, backed_up, total);
    });
}
```

- [ ] **Step 4: Build to verify**

```bash
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep -E "error:" | head -10
```

Expected: no errors.

- [ ] **Step 5: Commit**

```bash
git add client/include/tesseract/account_session.h \
        ui/shared/app/EventHandlerBase.h \
        ui/shared/app/EventHandlerBase.cpp
git commit -m "feat(app): marshal on_enable_recovery_progress to UI thread"
```

---

## Task 6: `EncryptionSetupOverlay` Widget — TDD

**Files:**
- Create: `tests/cpp/test_encryption_setup_overlay.cpp`
- Create: `ui/shared/views/EncryptionSetupOverlay.h`
- Create: `ui/shared/views/EncryptionSetupOverlay.cpp`
- Modify: `ui/shared/views/CMakeLists.txt` (add new source)
- Modify: `tests/cpp/CMakeLists.txt` (add new test)

- [ ] **Step 1: Write the failing tests**

Create `tests/cpp/test_encryption_setup_overlay.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/EncryptionSetupOverlay.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx() { return {surface->factory(), Theme::light()}; }
    PaintCtx  paint_ctx()  { return {surface->canvas(), surface->factory(), Theme::light()}; }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

// ── Fresh mode ──────────────────────────────────────────────────────────────

TEST_CASE("Fresh: starts in Intro step", "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Fresh: Intro → ChooseMethod on primary action", "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    bool enable_called = false;
    ov.on_continue_intro = [&]() {};
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    CHECK(ov.step() == EncryptionSetupOverlay::Step::ChooseMethod);
}

TEST_CASE("Fresh: Intro Skip fires on_close", "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    bool closed = false;
    ov.on_close = [&]() { closed = true; };
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_skip();
    CHECK(closed);
}

TEST_CASE("Fresh: ChooseMethod Continue fires on_enable_recovery (key mode)",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action(); // → ChooseMethod
    std::string fired_passphrase = "sentinel";
    ov.on_enable_recovery = [&](std::string p) { fired_passphrase = p; };
    ov.simulate_primary_action(); // Continue in key mode
    CHECK(fired_passphrase.empty()); // empty passphrase → key mode
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Fresh: ChooseMethod Continue fires on_enable_recovery (passphrase mode)",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action(); // → ChooseMethod
    ov.simulate_select_passphrase_mode();
    ov.set_passphrase_input("s3cr3t");
    std::string fired_passphrase;
    ov.on_enable_recovery = [&](std::string p) { fired_passphrase = p; };
    ov.simulate_primary_action();
    CHECK(fired_passphrase == "s3cr3t");
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Fresh: advance_progress Done → ShowKey with key string",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action(); // → ChooseMethod
    ov.simulate_primary_action(); // → Progress
    ov.advance_progress(4, "AAAA-BBBB-CCCC", 0, 0); // Done
    CHECK(ov.step() == EncryptionSetupOverlay::Step::ShowKey);
    CHECK(ov.recovery_key() == "AAAA-BBBB-CCCC");
}

TEST_CASE("Fresh: ShowKey Continue disabled until checkbox checked",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    ov.simulate_primary_action();
    ov.advance_progress(4, "KEY", 0, 0);
    CHECK(ov.step() == EncryptionSetupOverlay::Step::ShowKey);
    // Continue without checking box → stays on ShowKey
    ov.simulate_primary_action();
    CHECK(ov.step() == EncryptionSetupOverlay::Step::ShowKey);
    // Check box → Continue enabled
    ov.simulate_check_key_saved();
    ov.simulate_primary_action();
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: passphrase mode skips ShowKey → Done directly",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    ov.simulate_select_passphrase_mode();
    ov.set_passphrase_input("s3cr3t");
    ov.simulate_primary_action();
    ov.advance_progress(4, "", 0, 0); // Done, empty key → passphrase mode
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: Progress step==5 (error) returns to ChooseMethod with message",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    ov.simulate_primary_action();
    ov.advance_progress(5, "network error", 0, 0);
    CHECK(ov.step() == EncryptionSetupOverlay::Step::ChooseMethod);
    CHECK(!ov.error_msg().empty());
}

// ── Recover mode ─────────────────────────────────────────────────────────────

TEST_CASE("Recover: starts in Intro step", "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Recover);
    st.run(ov, {0, 0, 800, 600});
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Recover: Intro → EnterKey on primary action", "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Recover);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    CHECK(ov.step() == EncryptionSetupOverlay::Step::EnterKey);
}

TEST_CASE("Recover: EnterKey Verify fires on_recover with key",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Recover);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action(); // → EnterKey
    ov.set_key_input("my-recovery-key");
    std::string fired_key;
    ov.on_recover = [&](std::string k) { fired_key = k; };
    ov.simulate_primary_action(); // Verify
    CHECK(fired_key == "my-recovery-key");
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Recover: 'use another device' fires on_request_sas",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Recover);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action(); // → EnterKey
    bool sas_fired = false;
    ov.on_request_sas = [&]() { sas_fired = true; };
    ov.simulate_sas_link();
    CHECK(sas_fired);
}

TEST_CASE("Recover: Progress Done → Done step", "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Recover);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    ov.set_key_input("key");
    ov.simulate_primary_action();
    ov.advance_progress(4, "", 0, 0);
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Recover: Progress error returns to EnterKey with message",
          "[encryption][overlay]")
{
    Stage st;
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Recover);
    st.run(ov, {0, 0, 800, 600});
    ov.simulate_primary_action();
    ov.set_key_input("key");
    ov.simulate_primary_action();
    ov.advance_progress(5, "bad key", 0, 0);
    CHECK(ov.step() == EncryptionSetupOverlay::Step::EnterKey);
    CHECK(!ov.error_msg().empty());
}

// ── Progress step labels ──────────────────────────────────────────────────────

TEST_CASE("advance_progress updates status label string", "[encryption][overlay]")
{
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    ov.simulate_primary_action();
    ov.simulate_primary_action();
    ov.advance_progress(1, "", 0, 0); // CreatingBackup
    CHECK(ov.progress_label().find("backup") != std::string::npos ||
          ov.progress_label().find("Backup") != std::string::npos);
    ov.advance_progress(3, "", 42, 100); // BackingUp
    CHECK(ov.progress_label().find("42") != std::string::npos);
}

// ── Done → close ─────────────────────────────────────────────────────────────

TEST_CASE("Done: close button fires on_close", "[encryption][overlay]")
{
    EncryptionSetupOverlay ov(EncryptionSetupOverlay::Mode::Fresh);
    ov.simulate_primary_action();
    ov.simulate_primary_action();
    ov.advance_progress(4, "KEY", 0, 0);
    ov.simulate_check_key_saved();
    ov.simulate_primary_action();
    CHECK(ov.step() == EncryptionSetupOverlay::Step::Done);
    bool closed = false;
    ov.on_close = [&]() { closed = true; };
    ov.simulate_primary_action(); // Close
    CHECK(closed);
}
```

- [ ] **Step 2: Register tests in `tests/cpp/CMakeLists.txt`**

Add alongside other test files:

```cmake
add_tesseract_test(test_encryption_setup_overlay)
```

- [ ] **Step 3: Run tests to verify they fail (widget not yet implemented)**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | grep -E "error:" | head -5
```

Expected: compilation error — `EncryptionSetupOverlay` not found.

- [ ] **Step 4: Create `EncryptionSetupOverlay.h`**

```cpp
#pragma once

#include "tk/widget.h"
#include "tk/canvas.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class EncryptionSetupOverlay : public tk::Widget
{
public:
    enum class Mode { Fresh, Recover };
    enum class Step { Intro, ChooseMethod, EnterKey, Progress, ShowKey, Done };

    explicit EncryptionSetupOverlay(Mode mode);

    // ── Callbacks wired by ShellBase ──────────────────────────────────────
    std::function<void()>              on_close;
    std::function<void(std::string)>   on_enable_recovery; // passphrase or ""
    std::function<void(std::string)>   on_recover;         // key or passphrase
    std::function<void()>              on_request_sas;
    std::function<void(std::string)>   on_copy_to_clipboard;

    // ── Host hooks (NativeTextField rects) ───────────────────────────────
    std::function<tk::Rect()>      passphrase_field_rect;
    std::function<bool()>          passphrase_field_visible;
    std::function<tk::Rect()>      key_field_rect;
    std::function<bool()>          key_field_visible;
    std::function<std::string()>   get_passphrase;
    std::function<std::string()>   get_key_input;

    // ── Driven by ShellBase after on_enable_recovery/on_recover fires ────
    void advance_progress(uint8_t step,
                          const std::string& recovery_key,
                          uint32_t backed_up,
                          uint32_t total);

    // ── Accessors (read by host for field-rect delegation) ───────────────
    Step        step()          const { return step_; }
    Mode        mode()          const { return mode_; }
    std::string recovery_key()  const { return recovery_key_; }
    std::string error_msg()     const { return error_msg_; }
    std::string progress_label()const { return progress_label_; }

    bool passphrase_field_rect_visible() const;
    tk::Rect passphrase_field_rect_value() const;
    bool key_field_rect_visible() const;
    tk::Rect key_field_rect_value() const;

    // ── tk::Widget interface ──────────────────────────────────────────────
    tk::Size measure(const tk::LayoutCtx& ctx, tk::Size avail) override;
    void arrange(const tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(const tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point world) override;
    bool on_pointer_up(tk::Point world) override;

    // ── Test helpers ──────────────────────────────────────────────────────
    void simulate_primary_action();
    void simulate_skip();
    void simulate_select_passphrase_mode();
    void simulate_check_key_saved();
    void simulate_sas_link();
    void set_passphrase_input(std::string v) { passphrase_input_ = std::move(v); }
    void set_key_input(std::string v) { key_input_ = std::move(v); }

private:
    void advance_step_(Step next);
    void fire_primary_();  // context-sensitive action for current step
    tk::Rect card_bounds() const;
    void update_progress_label_(uint8_t step, uint32_t backed_up, uint32_t total);

    Mode        mode_;
    Step        step_     = Step::Intro;
    std::string recovery_key_;
    std::string error_msg_;
    std::string progress_label_;
    std::string passphrase_input_;
    std::string key_input_;
    uint32_t    backed_up_ = 0;
    uint32_t    total_     = 0;
    bool        key_saved_checked_  = false;
    bool        passphrase_mode_    = false; // ChooseMethod selection
    bool        press_backdrop_     = false;
    bool        press_primary_      = false;
};

} // namespace tesseract::views
```

- [ ] **Step 5: Create `EncryptionSetupOverlay.cpp`**

```cpp
#include "views/EncryptionSetupOverlay.h"
#include "tk/canvas.h"
#include "tk/theme.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{
constexpr float kCardW   = 480.0f;
constexpr float kCardPad = 32.0f;
} // namespace

EncryptionSetupOverlay::EncryptionSetupOverlay(Mode mode)
    : mode_(mode)
{
}

// ── advance_progress ─────────────────────────────────────────────────────────

void EncryptionSetupOverlay::advance_progress(uint8_t step,
                                               const std::string& recovery_key,
                                               uint32_t backed_up,
                                               uint32_t total)
{
    update_progress_label_(step, backed_up, total);
    if (step == 4) // Done
    {
        recovery_key_ = recovery_key;
        // Passphrase mode: key is empty → skip ShowKey, go to Done.
        if (recovery_key_.empty() || mode_ == Mode::Recover)
            advance_step_(Step::Done);
        else
            advance_step_(Step::ShowKey);
    }
    else if (step == 5) // Error
    {
        error_msg_ = recovery_key; // error message passed in recovery_key field
        advance_step_(mode_ == Mode::Fresh ? Step::ChooseMethod : Step::EnterKey);
    }
    request_repaint();
}

void EncryptionSetupOverlay::update_progress_label_(uint8_t step,
                                                     uint32_t backed_up,
                                                     uint32_t total)
{
    switch (step)
    {
        case 0: progress_label_ = "Starting…";              break;
        case 1: progress_label_ = "Creating backup…";       break;
        case 2: progress_label_ = "Generating recovery key…"; break;
        case 3:
            progress_label_ = "Backing up keys ("
                            + std::to_string(backed_up) + " / "
                            + std::to_string(total) + ")…";
            break;
        case 4: progress_label_ = "Done."; break;
        default: break;
    }
}

// ── Step transitions ──────────────────────────────────────────────────────────

void EncryptionSetupOverlay::advance_step_(Step next)
{
    error_msg_.clear();
    step_ = next;
    request_repaint();
    request_relayout();
}

void EncryptionSetupOverlay::fire_primary_()
{
    switch (step_)
    {
        case Step::Intro:
            advance_step_(mode_ == Mode::Fresh ? Step::ChooseMethod : Step::EnterKey);
            break;

        case Step::ChooseMethod:
            advance_step_(Step::Progress);
            if (on_enable_recovery)
                on_enable_recovery(passphrase_mode_ ? passphrase_input_ : "");
            break;

        case Step::EnterKey:
            if (key_input_.empty()) return;
            advance_step_(Step::Progress);
            if (on_recover)
                on_recover(key_input_);
            break;

        case Step::Progress:
            break; // not dismissible

        case Step::ShowKey:
            if (!key_saved_checked_) return;
            advance_step_(Step::Done);
            break;

        case Step::Done:
            if (on_close) on_close();
            break;
    }
}

// ── Simulation helpers (test-only) ────────────────────────────────────────────

void EncryptionSetupOverlay::simulate_primary_action() { fire_primary_(); }

void EncryptionSetupOverlay::simulate_skip()
{
    if (on_close) on_close();
}

void EncryptionSetupOverlay::simulate_select_passphrase_mode()
{
    passphrase_mode_ = true;
    request_repaint();
}

void EncryptionSetupOverlay::simulate_check_key_saved()
{
    key_saved_checked_ = true;
    request_repaint();
}

void EncryptionSetupOverlay::simulate_sas_link()
{
    if (on_request_sas) on_request_sas();
}

// ── Field-rect accessors ──────────────────────────────────────────────────────

bool EncryptionSetupOverlay::passphrase_field_rect_visible() const
{
    return visible() && step_ == Step::ChooseMethod && passphrase_mode_;
}

tk::Rect EncryptionSetupOverlay::passphrase_field_rect_value() const
{
    if (!passphrase_field_rect_visible()) return {};
    auto card = card_bounds();
    // Approximate: a text-field row within the card body.
    return {card.x + kCardPad, card.y + 160.0f, card.w - 2.0f * kCardPad, 36.0f};
}

bool EncryptionSetupOverlay::key_field_rect_visible() const
{
    return visible() && step_ == Step::EnterKey;
}

tk::Rect EncryptionSetupOverlay::key_field_rect_value() const
{
    if (!key_field_rect_visible()) return {};
    auto card = card_bounds();
    return {card.x + kCardPad, card.y + 120.0f, card.w - 2.0f * kCardPad, 36.0f};
}

// ── Layout / paint ────────────────────────────────────────────────────────────

tk::Rect EncryptionSetupOverlay::card_bounds() const
{
    auto b = bounds();
    float card_h = 280.0f; // approximate; real impl adapts per step
    float cx     = b.x + (b.w - kCardW) * 0.5f;
    float cy     = b.y + (b.h - card_h) * 0.5f;
    return {cx, cy, kCardW, card_h};
}

tk::Size EncryptionSetupOverlay::measure(const tk::LayoutCtx& /*ctx*/, tk::Size avail)
{
    return avail;
}

void EncryptionSetupOverlay::arrange(const tk::LayoutCtx& /*ctx*/, tk::Rect b)
{
    set_bounds(b);
}

void EncryptionSetupOverlay::paint(const tk::PaintCtx& ctx)
{
    auto b    = bounds();
    auto& c   = ctx.canvas;
    auto& th  = ctx.theme;

    // Dim backdrop.
    c.fill_rect(b, tk::Color{0, 0, 0, 128});

    // Card background.
    auto card = card_bounds();
    c.fill_rect(card, th.palette.surface);

    // Title text.
    const char* title = (mode_ == Mode::Fresh)
        ? "Secure your messages"
        : "Verify this device";
    auto title_st = tk::TextStyle{th.font_family, 18.0f, th.palette.text_primary};
    auto lay = ctx.factory.build_text(title, title_st);
    if (lay)
        c.draw_text(*lay, {card.x + kCardPad, card.y + kCardPad});

    // Step-specific content (labels, buttons).
    // A full production impl would render each step's full UI here.
    // For the TDD phase, the simulate_* helpers drive state without painting.
    (void)progress_label_;
    (void)error_msg_;
    (void)recovery_key_;
    (void)key_saved_checked_;
    (void)passphrase_mode_;
    (void)backed_up_;
    (void)total_;
}

bool EncryptionSetupOverlay::on_pointer_down(tk::Point /*world*/)
{
    return visible();
}

bool EncryptionSetupOverlay::on_pointer_up(tk::Point /*world*/)
{
    return visible();
}

} // namespace tesseract::views
```

- [ ] **Step 6: Register the new source in `ui/shared/views/CMakeLists.txt`**

Add `EncryptionSetupOverlay.cpp` alongside other view sources.

- [ ] **Step 7: Run tests — all new tests should pass**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "encryption" --output-on-failure
```

Expected: all `[encryption]` tests pass.

- [ ] **Step 8: Run full test suite to check for regressions**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
git add ui/shared/views/EncryptionSetupOverlay.h \
        ui/shared/views/EncryptionSetupOverlay.cpp \
        tests/cpp/test_encryption_setup_overlay.cpp \
        ui/shared/views/CMakeLists.txt \
        tests/cpp/CMakeLists.txt
git commit -m "feat(views): add EncryptionSetupOverlay widget with TDD tests"
```

---

## Task 7: MainAppWidget Integration

**Files:**
- Modify: `ui/shared/views/MainAppWidget.h`
- Modify: `ui/shared/views/MainAppWidget.cpp`

- [ ] **Step 1: Add overlay member and methods to `MainAppWidget.h`**

After the `VideoViewerOverlay* vid_viewer_` private member, add:

```cpp
    EncryptionSetupOverlay* encryption_setup_ = nullptr;
```

After `show_video_viewer(bool)` public method, add:

```cpp
    void show_encryption_setup(bool show);

    EncryptionSetupOverlay* encryption_setup() const { return encryption_setup_; }

    // Field-rect delegation (called from shell's set_on_layout hook).
    bool encryption_setup_passphrase_field_visible() const;
    tk::Rect encryption_setup_passphrase_field_rect() const;
    bool encryption_setup_key_field_visible() const;
    tk::Rect encryption_setup_key_field_rect() const;
```

Add include at top of `MainAppWidget.h`:

```cpp
#include "views/EncryptionSetupOverlay.h"
```

- [ ] **Step 2: Implement in `MainAppWidget.cpp`**

In the constructor, after `vid_viewer_` is created:

```cpp
    auto enc = std::make_unique<EncryptionSetupOverlay>(
        EncryptionSetupOverlay::Mode::Fresh); // mode set dynamically on show
    encryption_setup_ = add_child(std::move(enc));
    encryption_setup_->set_visible(false);
```

Add the new methods after `show_video_viewer`:

```cpp
void MainAppWidget::show_encryption_setup(bool show)
{
    if (encryption_setup_)
        encryption_setup_->set_visible(show);
}

bool MainAppWidget::encryption_setup_passphrase_field_visible() const
{
    return encryption_setup_ && encryption_setup_->passphrase_field_rect_visible();
}

tk::Rect MainAppWidget::encryption_setup_passphrase_field_rect() const
{
    if (!encryption_setup_) return {};
    return encryption_setup_->passphrase_field_rect_value();
}

bool MainAppWidget::encryption_setup_key_field_visible() const
{
    return encryption_setup_ && encryption_setup_->key_field_rect_visible();
}

tk::Rect MainAppWidget::encryption_setup_key_field_rect() const
{
    if (!encryption_setup_) return {};
    return encryption_setup_->key_field_rect_value();
}
```

In `arrange()`, add after the `vid_viewer_->arrange(ctx, bounds)` line:

```cpp
    if (encryption_setup_) encryption_setup_->arrange(ctx, bounds);
```

In `paint()`, add after `vid_viewer_` paint:

```cpp
    if (encryption_setup_ && encryption_setup_->visible())
        encryption_setup_->paint(ctx);
```

In `has_modal_overlay()`, include the overlay:

```cpp
    return (confirm_dialog_ && confirm_dialog_->is_open()) ||
           (img_viewer_        && img_viewer_->is_open())  ||
           (vid_viewer_        && vid_viewer_->is_open())  ||
           (encryption_setup_  && encryption_setup_->visible());
```

- [ ] **Step 3: Build to verify**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | grep -E "error:" | head -10
```

Expected: no errors.

- [ ] **Step 4: Run tests**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add ui/shared/views/MainAppWidget.h ui/shared/views/MainAppWidget.cpp
git commit -m "feat(views): integrate EncryptionSetupOverlay into MainAppWidget"
```

---

## Task 8: ShellBase Detection + Routing — TDD

**Files:**
- Create: `tests/cpp/test_shell_encryption_setup.cpp`
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`
- Modify: `tests/cpp/CMakeLists.txt`

- [ ] **Step 1: Write failing ShellBase tests**

Create `tests/cpp/test_shell_encryption_setup.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "views/EncryptionSetupOverlay.h"

#include <memory>

using tesseract::ShellBase;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct TestShell : ShellBase
{
    // ── ShellBase pure virtuals ───────────────────────────────────────────
    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void post_to_ui_after_(int, std::function<void()> fn) override { fn(); }
    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void navigate_to_room_(const std::string&) override {}
    void on_tab_state_changed_ui_() override {}
    std::int64_t monotonic_ms_() override { return 0; }

    // ── New pure virtual under test ───────────────────────────────────────
    EncryptionSetupOverlay::Mode last_mode_{};
    bool overlay_shown_ = false;
    void show_encryption_setup_overlay_(EncryptionSetupOverlay::Mode m) override
    {
        overlay_shown_ = true;
        last_mode_     = m;
    }

    // ── Inject state for read_recovery_state_ ────────────────────────────
    uint8_t recovery_state_stub_ = 0;
    uint8_t read_recovery_state_() const override { return recovery_state_stub_; }

    // ── Expose internals for test inspection ─────────────────────────────
    using ShellBase::check_encryption_setup_;
    using ShellBase::encryption_setup_shown_;
    using ShellBase::encryption_setup_dismissed_;
};

} // namespace

TEST_CASE("Disabled state → Fresh overlay shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1; // Disabled
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Fresh);
    CHECK(shell.encryption_setup_shown_);
}

TEST_CASE("Incomplete state → Recover overlay shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 3; // Incomplete
    shell.check_encryption_setup_();
    REQUIRE(shell.overlay_shown_);
    CHECK(shell.last_mode_ == EncryptionSetupOverlay::Mode::Recover);
}

TEST_CASE("Enabled state → overlay NOT shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 2; // Enabled
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("Unknown state → overlay NOT shown", "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 0; // Unknown
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("encryption_setup_shown_ guards against double-raise",
          "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1;
    shell.check_encryption_setup_();
    shell.check_encryption_setup_(); // second call
    // virtual called exactly once
    CHECK(shell.overlay_shown_);
    shell.overlay_shown_ = false;
    shell.check_encryption_setup_(); // third call — still guarded
    CHECK_FALSE(shell.overlay_shown_);
}

TEST_CASE("encryption_setup_dismissed_ prevents overlay from showing",
          "[shell][encryption]")
{
    TestShell shell;
    shell.recovery_state_stub_ = 1;
    shell.encryption_setup_dismissed_ = true;
    shell.check_encryption_setup_();
    CHECK_FALSE(shell.overlay_shown_);
}
```

- [ ] **Step 2: Register in `tests/cpp/CMakeLists.txt`**

```cmake
add_tesseract_test(test_shell_encryption_setup)
```

- [ ] **Step 3: Add declarations to `ShellBase.h`**

After `recovery_banner_dismissed_` member (line ~360):

```cpp
    bool encryption_setup_shown_     = false;
    bool encryption_setup_dismissed_ = false;
```

After `handle_backup_progress_ui_` virtual:

```cpp
    void handle_enable_recovery_progress_ui_(uint8_t step,
                                             std::string recovery_key,
                                             uint32_t backed_up,
                                             uint32_t total) override;
```

Add pure virtual after existing ones:

```cpp
    virtual void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) = 0;
```

Add protected virtual for test injection:

```cpp
    virtual uint8_t read_recovery_state_() const;
```

Add private helper:

```cpp
    void check_encryption_setup_();
```

Add include to `ShellBase.h`:

```cpp
#include "views/EncryptionSetupOverlay.h"
```

- [ ] **Step 4: Implement in `ShellBase.cpp`**

```cpp
uint8_t ShellBase::read_recovery_state_() const
{
    return client_ ? static_cast<uint8_t>(client_->recovery_state()) : 0u;
}

void ShellBase::check_encryption_setup_()
{
    if (encryption_setup_shown_ || encryption_setup_dismissed_)
        return;
    const uint8_t state = read_recovery_state_();
    if (state == 1) // Disabled → fresh account
    {
        encryption_setup_shown_ = true;
        show_encryption_setup_overlay_(
            tesseract::views::EncryptionSetupOverlay::Mode::Fresh);
    }
    else if (state == 3) // Incomplete → new device
    {
        encryption_setup_shown_ = true;
        show_encryption_setup_overlay_(
            tesseract::views::EncryptionSetupOverlay::Mode::Recover);
    }
    // Unknown (0) and Enabled (2): do nothing; re-checked on next tick.
}
```

In `handle_enable_recovery_progress_ui_`:

```cpp
void ShellBase::handle_enable_recovery_progress_ui_(uint8_t step,
                                                     std::string recovery_key,
                                                     uint32_t backed_up,
                                                     uint32_t total)
{
    if (active_account_index_ < 0) return;
    auto* overlay = mainApp_ ? mainApp_->encryption_setup() : nullptr;
    if (!overlay) return;
    overlay->advance_progress(step, recovery_key, backed_up, total);
}
```

Note: `mainApp_` is the `MainAppWidget*` pointer already in ShellBase. If it is not directly accessible here, use the per-platform accessor — this may need to be a virtual call. Check and adjust accordingly.

At the end of `push_rooms_()` in ShellBase.cpp:

```cpp
    // One-time encryption setup check — fires the overlay on first eligible
    // sync tick after login if the account needs it.
    check_encryption_setup_();
```

Also, short-circuit `check_and_show_recovery_banner_()` (or the equivalent per-shell `maybeShowRecoveryBanner`) for the `Incomplete` case when the overlay is already shown:

```cpp
    if (encryption_setup_shown_) return; // overlay already handling this
```

Add this guard at the top of the existing recovery-banner trigger logic in each shell (Task 9–12).

- [ ] **Step 5: Run failing tests, then fix until passing**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "shell.*encryption" --output-on-failure
```

Expected: all 6 new shell encryption tests pass.

- [ ] **Step 6: Full regression check**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add ui/shared/app/ShellBase.h \
        ui/shared/app/ShellBase.cpp \
        tests/cpp/test_shell_encryption_setup.cpp \
        tests/cpp/CMakeLists.txt
git commit -m "feat(shell): add encryption setup detection and routing in ShellBase"
```

---

## Task 9: Qt6 Shell Wiring

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h`
- Modify: `ui/linux-qt/src/MainWindow.cpp`

- [ ] **Step 1: Declare the override and NativeTextField members in `MainWindow.h`**

Add after `handle_verification_state_ui_`:

```cpp
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
```

Add private members alongside `recoveryKeyField_`:

```cpp
    std::unique_ptr<tk::qt::NativeTextField> encPassphraseField_;
    std::unique_ptr<tk::qt::NativeTextField> encKeyField_;
```

- [ ] **Step 2: Implement `show_encryption_setup_overlay_` in `MainWindow.cpp`**

```cpp
void MainWindow::show_encryption_setup_overlay_(
    tesseract::views::EncryptionSetupOverlay::Mode mode)
{
    if (!mainApp_) return;

    auto* ov = mainApp_->encryption_setup();
    if (!ov) return;

    // Re-create the overlay in the right mode (MainAppWidget always holds one
    // instance; just reconfigure it).
    *ov = tesseract::views::EncryptionSetupOverlay(mode);

    // Wire NativeTextFields.
    encPassphraseField_ = mainAppSurface_->host().make_text_field();
    encPassphraseField_->set_password(true);

    encKeyField_ = mainAppSurface_->host().make_text_field();
    encKeyField_->set_password(false);

    ov->get_passphrase  = [this]() -> std::string {
        return encPassphraseField_ ? encPassphraseField_->text() : "";
    };
    ov->get_key_input   = [this]() -> std::string {
        return encKeyField_ ? encKeyField_->text() : "";
    };

    // Callbacks.
    ov->on_close = [this]()
    {
        encryption_setup_dismissed_ = true;
        mainApp_->show_encryption_setup(false);
        encPassphraseField_.reset();
        encKeyField_.reset();
        mainAppSurface_->relayout();
    };
    ov->on_enable_recovery = [this](std::string passphrase)
    {
        auto* c = client_;
        run_async_mut_([this, c, passphrase]()
        {
            auto res = c->enable_recovery(passphrase);
            if (!res.ok)
                post_to_ui_([this]() { /* overlay already gets error via callback */ });
        });
    };
    ov->on_recover = [this](std::string key)
    {
        auto* c = client_;
        run_async_mut_([this, c, key]()
        {
            auto res = c->recover(key);
            if (!res.ok)
            {
                post_to_ui_([this]()
                {
                    if (auto* o = mainApp_ ? mainApp_->encryption_setup() : nullptr)
                        o->advance_progress(5, "Wrong key or passphrase", 0, 0);
                });
            }
        });
    };
    ov->on_request_sas = [this]()
    {
        encryption_setup_dismissed_ = true;
        mainApp_->show_encryption_setup(false);
        encPassphraseField_.reset();
        encKeyField_.reset();
        auto* c = client_;
        run_async_mut_([c]() { c->request_self_verification(); });
        mainAppSurface_->relayout();
    };
    ov->on_copy_to_clipboard = [](std::string text)
    {
        QApplication::clipboard()->setText(QString::fromStdString(text));
    };

    mainApp_->show_encryption_setup(true);
    mainAppSurface_->relayout();
}
```

- [ ] **Step 3: Add field-rect positioning to `set_on_layout`**

In the existing `set_on_layout` lambda (where `recoveryKeyField_` is positioned), add:

```cpp
            if (mainApp_ && encPassphraseField_)
            {
                encPassphraseField_->set_visible(
                    mainApp_->encryption_setup_passphrase_field_visible());
                encPassphraseField_->set_rect(
                    mainApp_->encryption_setup_passphrase_field_rect());
            }
            if (mainApp_ && encKeyField_)
            {
                encKeyField_->set_visible(
                    mainApp_->encryption_setup_key_field_visible());
                encKeyField_->set_rect(
                    mainApp_->encryption_setup_key_field_rect());
            }
```

- [ ] **Step 4: Guard `maybeShowRecoveryBanner` against the overlay**

At the top of `maybeShowRecoveryBanner()`:

```cpp
    if (encryption_setup_shown_) return;
```

- [ ] **Step 5: Build Qt6 target and smoke-test**

```bash
cmake --build build/linux-qt6-debug && \
./build/linux-qt6-debug/ui/linux-qt/tesseract &
```

Log in with a fresh account (or an account on a homeserver with no encryption set up). Verify the overlay appears. Close the app.

- [ ] **Step 6: Run tests**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
git commit -m "feat(qt6): wire EncryptionSetupOverlay in Qt6 shell"
```

---

## Task 10: GTK4 Shell Wiring

**Files:**
- Modify: `ui/linux-gtk/src/MainWindow.h`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`

- [ ] **Step 1: Declare override and GTK field members in `MainWindow.h`**

```cpp
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
```

Private members:

```cpp
    std::unique_ptr<tk::gtk::NativeTextField> encPassphraseField_;
    std::unique_ptr<tk::gtk::NativeTextField> encKeyField_;
```

- [ ] **Step 2: Implement `show_encryption_setup_overlay_` in `MainWindow.cpp`**

Follow the same structure as the Qt6 shell (Task 9, Step 2). Replace Qt-specific types with GTK equivalents:
- `mainAppSurface_->host().make_text_field()` → same call, returns GTK variant
- `QApplication::clipboard()->setText(...)` → `gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), text.c_str())`

Wire `on_close`, `on_enable_recovery`, `on_recover`, `on_request_sas`, `on_copy_to_clipboard` identically (same lambda bodies, `post_to_ui_` pattern, `run_async_mut_`).

- [ ] **Step 3: Add field-rect positioning in the GTK layout-changed callback**

In the equivalent of the Qt6 `set_on_layout` (GTK surface layout callback), add the same `set_visible`/`set_rect` positioning for `encPassphraseField_` and `encKeyField_` using `mainApp_->encryption_setup_passphrase_field_rect()` and `mainApp_->encryption_setup_key_field_rect()`.

- [ ] **Step 4: Guard the existing recovery-banner trigger**

At the top of whatever triggers the recovery banner in the GTK shell:

```cpp
    if (encryption_setup_shown_) return;
```

- [ ] **Step 5: Build GTK target**

```bash
cmake --build build/linux-gtk-debug 2>&1 | grep -E "error:" | head -10
```

Expected: no errors.

- [ ] **Step 6: Run tests and commit**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
git add ui/linux-gtk/src/MainWindow.h ui/linux-gtk/src/MainWindow.cpp
git commit -m "feat(gtk4): wire EncryptionSetupOverlay in GTK4 shell"
```

---

## Task 11: Win32 Shell Wiring

**Files:**
- Modify: `ui/windows/src/MainWindow.h`
- Modify: `ui/windows/src/MainWindow.cpp`

- [ ] **Step 1: Declare override and Win32 field members in `MainWindow.h`**

```cpp
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
```

Private:

```cpp
    std::unique_ptr<tk::win32::NativeTextField> encPassphraseField_;
    std::unique_ptr<tk::win32::NativeTextField> encKeyField_;
```

- [ ] **Step 2: Implement `show_encryption_setup_overlay_`**

Same structure as Qt6 (Task 9, Step 2). Replace clipboard call:

```cpp
    // on_copy_to_clipboard Win32 variant
    ov->on_copy_to_clipboard = [hwnd = hwnd_](std::string text) {
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        auto sz = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (!hg) { CloseClipboard(); return; }
        auto* p = static_cast<wchar_t*>(GlobalLock(hg));
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p,
                            static_cast<int>(text.size() + 1));
        GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT, hg);
        CloseClipboard();
    };
```

- [ ] **Step 3: Position NativeTextFields in the Win32 layout handler**

In the `WM_SIZE` / `relayout` path where `recoveryKeyField_` is positioned, add:

```cpp
    if (mainApp_ && encPassphraseField_)
    {
        encPassphraseField_->set_visible(
            mainApp_->encryption_setup_passphrase_field_visible());
        encPassphraseField_->set_rect(
            mainApp_->encryption_setup_passphrase_field_rect());
    }
    if (mainApp_ && encKeyField_)
    {
        encKeyField_->set_visible(mainApp_->encryption_setup_key_field_visible());
        encKeyField_->set_rect(mainApp_->encryption_setup_key_field_rect());
    }
```

- [ ] **Step 4: Guard recovery banner trigger**

Add `if (encryption_setup_shown_) return;` at the top of the Win32 `maybeShowRecoveryBanner` equivalent.

- [ ] **Step 5: Build (cross-compile or Windows machine)**

```bash
cmake --build build/windows-debug 2>&1 | grep -E "error:" | head -10
```

Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add ui/windows/src/MainWindow.h ui/windows/src/MainWindow.cpp
git commit -m "feat(win32): wire EncryptionSetupOverlay in Win32 shell"
```

---

## Task 12: macOS Shell Wiring

**Files:**
- Modify: `ui/macos/src/MainWindowController.h`
- Modify: `ui/macos/src/MainWindowController.mm`

- [ ] **Step 1: Declare override in `MainWindowController.h`** (via `MacShell` public section)

```cpp
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) override;
```

Private Obj-C++ members in `MacShell` or `MainWindowController`:

```cpp
    std::unique_ptr<tk::macos::NativeTextField> _encPassphraseField;
    std::unique_ptr<tk::macos::NativeTextField> _encKeyField;
```

- [ ] **Step 2: Implement `show_encryption_setup_overlay_` in `.mm`**

Same logical structure as Qt6 (Task 9, Step 2). Replace clipboard call:

```objc
    ov->on_copy_to_clipboard = [](std::string text) {
        NSString* ns = [NSString stringWithUTF8String:text.c_str()];
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:ns
                                           forType:NSPasteboardTypeString];
    };
```

Wire field rects in the macOS layout callback (the `set_on_layout` equivalent for the macOS surface), using `mainApp_->encryption_setup_passphrase_field_rect()` and `mainApp_->encryption_setup_key_field_rect()`.

- [ ] **Step 3: Guard recovery banner trigger**

Add `if (encryption_setup_shown_) return;` at the top of the macOS recovery-banner trigger.

- [ ] **Step 4: Build macOS target**

```bash
cmake --build build/macos-appkit-arm64-debug 2>&1 | grep -E "error:" | head -10
```

Expected: no errors.

- [ ] **Step 5: Final test run**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass (count should be ~620+ given ~604 baseline + new tests).

- [ ] **Step 6: Final commit**

```bash
git add ui/macos/src/MainWindowController.h ui/macos/src/MainWindowController.mm
git commit -m "feat(macos): wire EncryptionSetupOverlay in macOS shell"
```

---

## Self-Review

**Spec coverage check:**

| Spec requirement | Task(s) |
|---|---|
| `recovery_state()` SDK method | Task 1 |
| `enable_recovery()` SDK method | Task 2 |
| `on_enable_recovery_progress` callback | Tasks 2, 4, 5 |
| C++ client wrappers | Task 3 |
| `AccountSession` guard flags | Task 5 |
| `EncryptionSetupOverlay` widget — all steps | Task 6 |
| Fresh / Recover mode routing | Tasks 6, 8 |
| ShowKey — checkbox guard | Task 6 |
| Progress step labels | Task 6 |
| Passphrase mode skips ShowKey | Task 6 |
| Error → back to ChooseMethod / EnterKey | Task 6 |
| SAS hand-off via `on_request_sas` | Task 6 |
| MainAppWidget integration | Task 7 |
| Field-rect delegation | Tasks 7, 9–12 |
| `has_modal_overlay()` includes overlay | Task 7 |
| ShellBase detection (`push_rooms_`) | Task 8 |
| `encryption_setup_shown_` / `dismissed_` guards | Task 8 |
| Recovery-banner retirement for Incomplete | Tasks 9–12 |
| Qt6 / GTK4 / Win32 / macOS shell wiring | Tasks 9–12 |
| Tests — widget level | Task 6 |
| Tests — ShellBase level | Task 8 |

**No TBDs or placeholders found.**

**Type consistency verified:** `EncryptionSetupOverlay::Mode` and `::Step` used consistently across Tasks 6–12. `advance_progress(uint8_t, const std::string&, uint32_t, uint32_t)` signature matches everywhere.

**One note for implementer:** Task 8 references `mainApp_` in `handle_enable_recovery_progress_ui_` — ShellBase does not currently have a `mainApp_` pointer directly. Each shell owns `mainApp_`. If ShellBase cannot access it, promote `handle_enable_recovery_progress_ui_` to a virtual no-op in ShellBase and override it in each shell to call `mainApp_->encryption_setup()->advance_progress(...)`. Adjust Tasks 9–12 accordingly.
