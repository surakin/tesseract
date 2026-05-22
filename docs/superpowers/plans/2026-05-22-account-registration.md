# Account Registration (OIDC prompt=create) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a capability-gated "New here? Create an account" link to the login screen that reuses the existing OAuth browser-loopback flow with OIDC `prompt=create`, so users can sign up on the homeserver's provider page.

**Architecture:** `oauth::begin` gains a `register` flag that adds `.prompt(vec![Prompt::Create])`; a new `homeserver_supports_registration` probe checks the OAuth server metadata's `prompt_values_supported`; the shared `LoginView` shows the link only when the resolved homeserver supports registration and drives the same OAuth routine with `register=true`. No per-shell wiring (the LoginView drives the flow itself).

**Tech Stack:** Rust (matrix-sdk 0.17 OAuth + ruma), `cxx` FFI, C++17 `client/` + shared `tk` LoginView, Catch2/ctest. Primary build/test: `linux-qt6-debug`; `linux-gtk-debug` also builds locally; Windows/macOS need no changes (they consume the unchanged LoginView hook API) and are verified by inspection + CI.

**Design spec:** `docs/superpowers/specs/2026-05-22-account-registration-design.md`

---

## Background the engineer needs

- The app is OAuth/OIDC-only. `sdk/src/oauth.rs::begin(homeserver, sqlite_path)` builds a loopback `/callback` listener and an auth URL via `client.oauth().login(redirect_url, None, Some(registration_data), None).build()`, then appends a `device_display_name` query param. `await_callback` does `finish_login` + device rename. `cancel` aborts.
- `sdk/src/client.rs::oauth_begin(&mut self, homeserver: &str) -> OAuthBegin` (line ~810) is a SINGLE definition (NOT `#[cfg]`-gated) — it calls `self.rt.block_on(oauth::begin(&hs, &path))`. `OAuthBegin { ok, message, auth_url, redirect_uri }`.
- `sdk/src/bridge.rs` declares `oauth_begin(self: &mut ClientFfi, homeserver: &str) -> OAuthBegin` plus `oauth_await_callback`, `oauth_cancel`, `discover_homeserver`. `ClientFfi` methods are real Rust compiled in both test and non-test builds (only the `cxx` bridge module + `EventHandlerBridge` are stubbed for tests; `ClientFfi` is not).
- `matrix-sdk` 0.17: `OAuthAuthCodeUrlBuilder::prompt(Vec<Prompt>)` (the builder returned by `client.oauth().login(...)`), and `client.oauth().server_metadata().await -> Result<AuthorizationServerMetadata, _>` whose `.prompt_values_supported: Vec<Prompt>` lists supported prompts. `Prompt` and `AuthorizationServerMetadata` live at `matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::{Prompt, AuthorizationServerMetadata}`; `Prompt::Create` is the registration signal.
- `client/include/tesseract/client.h` — `OAuthFlow { bool ok; std::string message, auth_url, redirect_uri; operator bool; }`; `OAuthFlow begin_oauth(const std::string& homeserver)` wraps `impl_->ffi->oauth_begin(homeserver)`. `DiscoveryResult discover_homeserver(...)` exists.
- `ui/shared/views/LoginView.{h,cpp}` drives the WHOLE flow. Card children created in the ctor (`LoginView.cpp` ~line 38-87): `title_lbl_, caption_lbl_, hs_input_label_, hs_field_lbl_, discovery_lbl_, sign_in_btn_ (tk::Button Primary, on_click → sign_in_()), cancel_btn_ (tk::Button Subtle, on_click → cancel_()), status_lbl_`. The homeserver field is a native overlay (`init_with_field`); the buttons/labels are shared-canvas `tk` widgets.
  - `sign_in_()` (~line 243): resolves `hs` (uses `resolved_base_url_` when `discovery_state_ == Resolved`), sets Waiting state, then on a `worker_` thread calls `c->begin_oauth(hs)` and posts `begin_completed_(ok, payload)`. `begin_completed_` opens the browser + spawns `await_oauth`. This whole sequence is identical for login and registration except the `begin_oauth` argument.
  - `set_state(State s)` (~line 94): `sign_in_btn_->set_visible(s == State::Form); cancel_btn_->set_visible(mode_ == Mode::AddAccount);`.
  - `set_discovery_state(DiscoveryState s, std::string detail)` (~line 138): sets `resolved_base_url_` on Resolved, clears on Idle, and updates `discovery_lbl_`. Discovery itself (`hs_changed_`, ~line 299) is debounced via `run_async_` with a `discovery_gen_` atomic generation counter + `post_to_ui_`.
  - Hooks injected by shells: `set_client`, `set_post_to_ui`, `set_run_async`, `set_relayout`, `set_open_browser`, `set_on_begin_oauth`, `set_on_success`, `set_on_cancel_done`. None change.

### Build & test commands

```bash
cargo test -p tesseract-sdk-ffi
cmake --preset linux-qt6-debug && cmake --build build/linux-qt6-debug
ctest --test-dir build/linux-qt6-debug --output-on-failure
cmake --preset linux-gtk-debug && cmake --build build/linux-gtk-debug   # also builds locally
```

OAuth is network/integration code: registration URL correctness and the live metadata probe are verified by manual smoke, not unit tests (matches the existing OAuth code, which has no behavioral unit tests beyond `extract_query`).

---

## Task 1: Rust OAuth flow — `register` flag + `supports_registration`

**Files:**
- Modify: `sdk/src/oauth.rs`

- [ ] **Step 1: Add the `register` parameter to `begin`.** In `sdk/src/oauth.rs`, change the signature:

```rust
pub async fn begin(
    homeserver: &str,
    sqlite_path: &std::path::Path,
    register: bool,
) -> anyhow::Result<BeginResult> {
```

- [ ] **Step 2: Add the `prompt=create` builder step.** In `begin`, where the auth URL is built (currently `client.oauth().login(redirect_url, None, Some(registration_data), None).build().await`), insert a `.prompt(...)` call when registering. Replace that builder chain with:

```rust
    let mut login_builder = client.oauth().login(
        redirect_url,
        None, /* device_id */
        Some(registration_data),
        None, /* additional_scopes */
    );
    if register {
        use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::Prompt;
        login_builder = login_builder.prompt(vec![Prompt::Create]);
    }
    let mut auth_data = login_builder
        .build()
        .await
        .context("oauth login() — does the homeserver support OAuth?")?;
```

(The subsequent `device_display_name` append and the `Ok(BeginResult { ... })` return are unchanged.)

- [ ] **Step 3: Add the `supports_registration` helper.** Add to `sdk/src/oauth.rs`:

```rust
/// Best-effort check whether `homeserver` allows account registration via the
/// OIDC `prompt=create` flow. Builds a throwaway client and inspects the OAuth
/// authorization-server metadata. Returns `false` on any error (non-OAuth
/// server, network failure, missing metadata).
pub async fn supports_registration(homeserver: &str) -> bool {
    use matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::Prompt;
    let client = match Client::builder()
        .server_name_or_homeserver_url(homeserver)
        .user_agent(build_user_agent())
        .build()
        .await
    {
        Ok(c) => c,
        Err(_) => return false,
    };
    match client.oauth().server_metadata().await {
        Ok(meta) => meta.prompt_values_supported.contains(&Prompt::Create),
        Err(_) => false,
    }
}
```

Note: `Client::builder()` here intentionally omits `.sqlite_store(...)` — this is a throwaway capability probe with no session to persist. If the builder requires a store to `build()`, use whatever in-memory/default store the SDK provides; the build error will make this obvious. Do NOT add encryption settings or `handle_refresh_tokens` — they are irrelevant to a metadata fetch.

- [ ] **Step 4: Build (compile check).**

Run: `cargo build -p tesseract-sdk-ffi 2>&1 | tail -15`
Expected: clean compile. (No callers yet besides the next task; this just verifies the signatures + imports resolve.)

- [ ] **Step 5: Run the existing Rust tests.**

Run: `cargo test -p tesseract-sdk-ffi 2>&1 | tail -3`
Expected: all pass (the `oauth.rs` `extract_query`/HTML tests are unaffected; no new tests — registration is network/integration code).

- [ ] **Step 6: Commit.**

```bash
git add sdk/src/oauth.rs
git commit -m "feat(oauth): begin(register) adds prompt=create; supports_registration probe"
```

---

## Task 2: Rust FFI bridge + `client.rs`

**Files:**
- Modify: `sdk/src/bridge.rs`
- Modify: `sdk/src/client.rs`

- [ ] **Step 1: Update the bridge declarations.** In `sdk/src/bridge.rs`, in the `extern "Rust"` block, change `oauth_begin` and add the probe:

```rust
        /// Begin the OAuth login (or, when `register` is true, registration via
        /// OIDC prompt=create) flow. Returns the auth URL to open in a browser.
        fn oauth_begin(self: &mut ClientFfi, homeserver: &str, register: bool) -> OAuthBegin;

        /// Best-effort: does `homeserver` advertise OIDC registration support
        /// (`prompt_values_supported` contains `create`)? Blocks — worker thread.
        fn homeserver_supports_registration(
            self: &mut ClientFfi,
            homeserver: &str,
        ) -> bool;
```

- [ ] **Step 2: Thread `register` into `ClientFfi::oauth_begin`.** In `sdk/src/client.rs`, change the signature `pub fn oauth_begin(&mut self, homeserver: &str, register: bool) -> OAuthBegin` and change the call `self.rt.block_on(oauth::begin(&hs, &path))` to `self.rt.block_on(oauth::begin(&hs, &path, register))`. Everything else in the method is unchanged.

- [ ] **Step 3: Add `homeserver_supports_registration`.** In `sdk/src/client.rs`, near `oauth_begin` (single definition, not `#[cfg]`-gated, matching `oauth_begin`):

```rust
    pub fn homeserver_supports_registration(&mut self, homeserver: &str) -> bool {
        let hs = homeserver.to_owned();
        self.rt.block_on(oauth::supports_registration(&hs))
    }
```

- [ ] **Step 4: Build + test.**

Run: `cargo test -p tesseract-sdk-ffi 2>&1 | tail -3`
Expected: all pass (no existing test calls `oauth_begin`, so the new param doesn't break tests).

- [ ] **Step 5: Commit.**

```bash
git add sdk/src/bridge.rs sdk/src/client.rs
git commit -m "feat(ffi): oauth_begin(register) + homeserver_supports_registration"
```

---

## Task 3: C++ `Client` wrapper

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`

- [ ] **Step 1: Update the declarations.** In `client/include/tesseract/client.h`, change `begin_oauth` and add the probe (near line 153):

```cpp
    /// Begin OAuth login, or account registration when `register_account` is
    /// true (OIDC prompt=create). Returns the auth URL to open in a browser.
    OAuthFlow begin_oauth(const std::string& homeserver,
                          bool register_account = false);

    /// Best-effort: does `homeserver` advertise OIDC registration support?
    /// Blocks the calling thread — invoke from a worker thread.
    bool homeserver_supports_registration(const std::string& homeserver);
```

(The default arg keeps existing `begin_oauth(hs)` call sites compiling.)

- [ ] **Step 2: Update the implementations.** In `client/src/client.cpp`, change `Client::begin_oauth` and add the probe:

```cpp
Client::OAuthFlow Client::begin_oauth(const std::string& homeserver,
                                      bool register_account)
{
    auto r = impl_->ffi->oauth_begin(homeserver, register_account);
    return OAuthFlow{
        .ok = r.ok,
        .message = std::string(r.message),
        .auth_url = std::string(r.auth_url),
        .redirect_uri = std::string(r.redirect_uri),
    };
}

bool Client::homeserver_supports_registration(const std::string& homeserver)
{
    return impl_->ffi->homeserver_supports_registration(homeserver);
}
```

- [ ] **Step 3: Build.**

Run: `cmake --build build/linux-qt6-debug 2>&1 | tail -15`
Expected: clean build (recompiles the Rust bridge + relinks; the LoginView still calls `begin_oauth(hs)` via the default arg).

- [ ] **Step 4: Commit.**

```bash
git add client/include/tesseract/client.h client/src/client.cpp
git commit -m "feat(client): begin_oauth(register_account) + registration probe"
```

---

## Task 4: Shared LoginView — register link, capability gate, shared OAuth routine

**Files:**
- Modify: `ui/shared/views/LoginView.h`
- Modify: `ui/shared/views/LoginView.cpp`

- [ ] **Step 1: Header members.** In `ui/shared/views/LoginView.h`, in the `private:` members near `sign_in_btn_` / `cancel_btn_`, add:

```cpp
    tk::Button* register_link_ = nullptr;
    bool registration_supported_ = false;
    std::atomic<uint32_t> registration_gen_{0};
```

And near the private method declarations (where `sign_in_()` / `hs_changed_()` are declared) add:

```cpp
    void start_oauth_(bool register_account);
    void probe_registration_support_(const std::string& base_url);
```

(`<atomic>` is already included.)

- [ ] **Step 2: Create the link widget.** In `ui/shared/views/LoginView.cpp`, in the ctor where the card children are added (after `cancel` is created/added, before `status`), add a Subtle button that starts hidden:

```cpp
    auto register_link = std::make_unique<tk::Button>(
        "New here? Create an account", std::function<void()>{},
        tk::Button::Variant::Subtle);
    register_link->set_min_size({0, kButtonHeight});
    register_link->set_on_click([this] { start_oauth_(true); });
    register_link->set_visible(false);
    register_link_ = card->add_child(std::move(register_link));
```

Place the `register_link_ = card->add_child(...)` line among the other `card->add_child` assignments so it sits below the Sign in button in the card (e.g. right after `sign_in_btn_ = card->add_child(std::move(sign_in));`, or after `cancel_btn_` — pick the position that reads as "under Sign in").

- [ ] **Step 3: Refactor `sign_in_` into `start_oauth_(bool)`.** Rename the body of `LoginView::sign_in_()` to `LoginView::start_oauth_(bool register_account)`, changing only the `begin_oauth` call:

```cpp
void LoginView::start_oauth_(bool register_account)
{
    // ... identical to the current sign_in_() body up to the worker thread ...
    auto* c = client_; // snapshot
    worker_ = std::thread(
        [this, hs, c, register_account]
        {
            auto flow = c->begin_oauth(hs, register_account);
            if (cancelled_.load())
                return;
            bool        ok      = static_cast<bool>(flow);
            std::string payload = ok ? flow.auth_url : flow.message;
            post_to_ui_(
                [this, ok, payload = std::move(payload)]
                {
                    begin_completed_(ok, payload);
                });
        });
}

void LoginView::sign_in_()
{
    start_oauth_(false);
}
```

Keep `sign_in_()` as the thin `false` wrapper so the existing callers — the Sign in button (`sign_in->set_on_click([this]{ sign_in_(); })`) and the field submit (`hs_field_->set_on_submit([this]{ sign_in_(); })`) — are unchanged. (Everything between the start of the old `sign_in_()` and the worker thread — the `hs` resolution, status/Waiting state, `join_worker_()`, `on_begin_oauth_()` — moves verbatim into `start_oauth_`.)

- [ ] **Step 4: Gate link visibility in `set_state`.** In `LoginView::set_state`, after the existing button visibility lines, add:

```cpp
    if (register_link_)
        register_link_->set_visible(s == State::Form && registration_supported_);
```

- [ ] **Step 5: Drive the probe from `set_discovery_state`.** In `LoginView::set_discovery_state`, after it updates `resolved_base_url_` / `discovery_lbl_`, manage the registration gate. At the TOP of the function (right after `discovery_state_ = s;`), bump the generation (cancels any in-flight probe for a previous homeserver) and hide the link for any non-resolved state:

```cpp
    ++registration_gen_;
    if (s != DiscoveryState::Resolved)
    {
        registration_supported_ = false;
        if (register_link_)
            register_link_->set_visible(false);
    }
```

Then at the END of the function, when resolved, kick the probe (which reads the freshly-bumped `registration_gen_` for its staleness guard — see Step 6):

```cpp
    if (s == DiscoveryState::Resolved)
    {
        probe_registration_support_(resolved_base_url_);
    }
```

- [ ] **Step 6: Implement the probe.** Add to `LoginView.cpp` (mirrors the `hs_changed_` discovery worker: `run_async_` + generation guard + `post_to_ui_`):

```cpp
void LoginView::probe_registration_support_(const std::string& base_url)
{
    auto* snap = client_;
    if (!snap || base_url.empty())
        return;
    uint32_t gen = registration_gen_.load();
    auto body = [this, gen, snap, base_url]
    {
        if (gen != registration_gen_.load())
            return;
        bool supported = snap->homeserver_supports_registration(base_url);
        if (gen != registration_gen_.load())
            return;
        post_to_ui_(
            [this, gen, supported]
            {
                if (gen != registration_gen_.load())
                    return;
                registration_supported_ = supported;
                if (register_link_)
                    register_link_->set_visible(state_ == State::Form &&
                                                supported);
                if (relayout_)
                    relayout_();
            });
    };
    if (run_async_)
        run_async_(std::move(body));
    else
        std::thread(std::move(body)).detach();
}
```

- [ ] **Step 7: Build + ctest (qt6).**

Run: `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure`
Expected: clean build, 100% tests pass.

- [ ] **Step 8: Build GTK.**

Run: `cmake --build build/linux-gtk-debug 2>&1 | tail -3`
Expected: clean (the LoginView is shared; GTK consumes it unchanged).

- [ ] **Step 9: Manual smoke (qt6).** Launch `./build/linux-qt6-debug/ui/linux-qt/tesseract`. Type a homeserver that allows signups (e.g. a MAS instance with registration on) → after the "✓ resolved" line, the "New here? Create an account" link appears → click it → browser opens the provider's **signup** page → completing it logs you in. Type a homeserver with signups off (or a non-OAuth server) → the link stays hidden. Confirm normal Sign in still works.

- [ ] **Step 10: Commit.**

```bash
git add ui/shared/views/LoginView.h ui/shared/views/LoginView.cpp
git commit -m "feat(login): capability-gated 'Create an account' link (OIDC prompt=create)"
```

---

## Final verification

- [ ] **Step 1: Rust.** `cargo test -p tesseract-sdk-ffi` → pass.
- [ ] **Step 2: qt6.** `cmake --build build/linux-qt6-debug && ctest --test-dir build/linux-qt6-debug --output-on-failure` → clean + all pass.
- [ ] **Step 3: GTK.** `cmake --build build/linux-gtk-debug 2>&1 | tail -3` → clean.
- [ ] **Step 4: No shell API break.** Windows/macOS shells aren't built on this Linux host; confirm by inspection that they only consume the unchanged LoginView hook API (`set_client`/`set_run_async`/etc.) and the `begin_oauth(hs)` default-arg call site still compiles — no shell file needs editing. (Verified in platform CI.)
- [ ] **Step 5: STATUS.md.** Add an account-registration entry + refresh the "Last updated" date (this is a user-facing feature). Commit.

> Per repo policy (CLAUDE.md): do not push or merge until the user confirms the change works.

---

## Notes on API confirmation during implementation

- `Prompt` / `AuthorizationServerMetadata` path: `matrix_sdk::ruma::api::client::discovery::get_authorization_server_metadata::v1::{Prompt, AuthorizationServerMetadata}`. `OAuthAuthCodeUrlBuilder::prompt(Vec<Prompt>)` and `client.oauth().server_metadata()` confirmed in matrix-sdk 0.17.
- `Client::builder()` for the throwaway probe (Task 1 Step 3) may need a store to `build()`; if so, use the SDK's in-memory/default store — the probe persists nothing. The compile/build error will indicate the exact requirement.
- The register link is a shared-canvas `tk::Button` (Subtle), NOT a native overlay — so no per-shell native wiring is needed (unlike the homeserver text field).
