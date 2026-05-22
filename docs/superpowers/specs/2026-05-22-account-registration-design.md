# Account registration (OIDC prompt=create) — design

**Date:** 2026-05-22
**Scope:** Let users create a new Matrix account from the login screen via the
OIDC `prompt=create` flow, reusing the existing OAuth browser-loopback path. A
"New here? Create an account" link appears under Sign in, gated on the
homeserver advertising registration support. No legacy `/register` / UIA, no
non-OAuth homeservers.

## Goal

- A **"New here? Create an account"** text link below the Sign in button in the
  shared `LoginView`.
- The link is **shown only when the resolved homeserver supports registration**
  (its OAuth authorization-server metadata advertises `prompt_values_supported`
  containing `create`).
- Clicking it runs the **same** OAuth browser-loopback flow as Sign in, but adds
  the OIDC `prompt=create` parameter so the provider shows its signup page. The
  same `/callback` listener completes it and the user ends up logged in.

## Why this shape

The app is OAuth/OIDC-only: `oauth::begin` builds the auth URL via
`client.oauth().login(...)` and fails on non-OAuth homeservers; the sync path
requires Simplified Sliding Sync. In that world, "registration" is the OIDC
*Initiating User Registration* flow (`prompt=create`), which `matrix-sdk`
supports directly (`OAuthAuthCodeUrlBuilder::prompt(vec![Prompt::Create])`). MAS
shows its signup page for that prompt and advertises support via
`prompt_values_supported: ["create"]` in its authorization-server metadata.
Legacy password registration (`/register` + User-Interactive Auth) is a much
larger, architecture-divergent effort and is out of scope.

## Relevant existing code

- `sdk/src/oauth.rs` — `begin(homeserver, sqlite_path)` builds the loopback
  listener + auth URL via `client.oauth().login(redirect_url, None,
  Some(registration_data), None).build()`. `await_callback` does the token
  exchange (`finish_login`) and device rename. `cancel` aborts.
- `sdk/src/bridge.rs` — `oauth_begin(homeserver) -> OAuthBegin`,
  `oauth_await_callback`, `oauth_cancel`, `discover_homeserver(server) -> String`
  (JSON `{base_url,error}`), plus `set_data_dir` (called before `oauth_begin`).
- `sdk/src/client.rs` — `ClientFfi::oauth_begin` calls `oauth::begin`; holds the
  `PendingFlow`.
- `client/include/tesseract/client.h` — `Client::begin_oauth(homeserver) ->
  OAuthFlow`, `await_oauth`, `cancel_oauth`, `discover_homeserver`.
- `ui/shared/views/LoginView.{h,cpp}` — drives the WHOLE flow internally: a
  debounced `discover_homeserver` (sets `DiscoveryState` Idle/Discovering/
  Resolved/Failed, stores `resolved_base_url_`), then on the Sign in button
  calls `client_->begin_oauth(hs)` → `open_browser_(url)` → `client_->await_oauth()`
  → `post_to_ui_(on_success_)`, all on `run_async_` workers guarded by
  `alive_token()`. Hooks injected by each shell: `set_client`, `set_post_to_ui`,
  `set_run_async`, `set_relayout`, `set_open_browser`, `set_on_begin_oauth`,
  `set_on_success`, `set_on_cancel_done`. `on_begin_oauth_` is a notification
  hook only. The homeserver field is a native overlay (`init_with_field`); other
  controls (buttons/labels) are shared-canvas `tk` widgets.
- The four shells (`ui/linux-qt`, `ui/linux-gtk`, `ui/windows`, `ui/macos`) each
  construct the shared LoginView and inject the hooks; they do not drive OAuth.

SDK APIs confirmed in matrix-sdk 0.17 / ruma-client-api:
- `OAuthAuthCodeUrlBuilder::prompt(Vec<Prompt>)`; `Prompt::Create`
  (`matrix_sdk::authentication::oauth`).
- `client.oauth().server_metadata() -> Result<AuthorizationServerMetadata,
  OAuthDiscoveryError>`; `AuthorizationServerMetadata.prompt_values_supported:
  Vec<Prompt>`.

## Architectural decision — registration-support detection

**Chosen: a separate capability probe fired after discovery resolves.** Keep the
debounced `discover_homeserver` (well-known base-URL resolution) lightweight and
unchanged. Add a dedicated FFI `homeserver_supports_registration(homeserver)`
that builds a throwaway client and checks `server_metadata().prompt_values_supported`.
The LoginView fires it once when `DiscoveryState` becomes `Resolved`.

Rejected: folding registration support into `discover_homeserver` — that would
make the per-keystroke debounced discovery do an extra OAuth-metadata round trip
every time.

## Design

### 1. Rust SDK + FFI

- `sdk/src/oauth.rs`: change `begin(homeserver, sqlite_path, register: bool)`.
  When `register`, call `.prompt(vec![Prompt::Create])` on the
  `client.oauth().login(...)` builder before `.build()` (import
  `matrix_sdk::authentication::oauth::Prompt`). All other behavior unchanged;
  the `device_display_name` query append and `await_callback`/`finish_login`
  path are shared by both login and registration.
- New `sdk/src/oauth.rs` helper `supports_registration(homeserver) -> bool`:
  build a lightweight/throwaway `Client` for the homeserver (no persistent
  session needed), call `client.oauth().server_metadata().await`, and return
  `metadata.prompt_values_supported.contains(&Prompt::Create)`. Return `false`
  on any error (non-OAuth server, network failure, missing metadata).
- `sdk/src/bridge.rs`:
  - change `oauth_begin(self, homeserver: &str, register: bool) -> OAuthBegin`;
  - add `homeserver_supports_registration(self: &mut ClientFfi, homeserver: &str)
    -> bool`.
- `sdk/src/client.rs`: thread `register` into `ClientFfi::oauth_begin` →
  `oauth::begin(..., register)`; implement `homeserver_supports_registration`
  (`#[cfg(not(test))]` real + `#[cfg(test)]` stub returning `false`). `oauth_begin`
  keeps its existing `#[cfg(test)]` stub shape (now taking the bool).

### 2. C++ client

`client/include/tesseract/client.h` + `client.cpp`:
- `OAuthFlow begin_oauth(const std::string& homeserver, bool register_account =
  false)` — default keeps existing call sites compiling; forwards the bool to the
  FFI.
- `bool homeserver_supports_registration(const std::string& homeserver)` — thin
  wrapper over the new FFI (blocks; callers use a worker thread).

(Name the C++ param `register_account` since `register` is a C++ keyword.)

### 3. Shared LoginView

`ui/shared/views/LoginView.{h,cpp}`:
- Add a **"New here? Create an account"** clickable text link below the Sign in
  button — a shared-canvas `tk` widget (no native overlay). Hidden by default.
- Add `bool registration_supported_ = false`. When the internal discovery
  transitions to `DiscoveryState::Resolved`, spawn (via `run_async_`) a call to
  `client_->homeserver_supports_registration(resolved_base_url_)`, then
  `post_to_ui_` (guarded by `alive_token()`) to set `registration_supported_`,
  show the link, and `relayout_()`. Reset `registration_supported_ = false`
  (hide the link) whenever the homeserver field changes / re-discovery starts, so
  the link never lingers from a previous homeserver.
- Extract the existing Sign in OAuth sequence (begin → open_browser → await →
  success) into one shared routine parameterized by `register_account`. The Sign
  in button calls it with `false`; the Create-account link calls it with `true`
  (→ `client_->begin_oauth(hs, true)`). Both share the Waiting state, cancel
  path, and `on_success_`.
- The link only fires when visible (i.e. `registration_supported_` is true), so a
  click can't reach an unsupported homeserver.

### 4. Per-shell wiring

None required. The LoginView drives OAuth itself and the link is a shared-canvas
widget, so the four shells need no new hooks — they already inject `client_`,
`run_async`, `post_to_ui`, `open_browser`, `relayout`, and `on_success`. Verify
the link lays out correctly on each shell's LoginView (qt/gtk built locally;
windows/macOS by inspection + CI).

## Testing

- Rust: `#[cfg(test)]` stubs for `oauth_begin` (now with the bool) and
  `homeserver_supports_registration` (returns `false`). Unit-test the URL
  building: assert the built auth URL contains `prompt=create` when registering
  and not otherwise (a pure helper that builds/inspects the URL, mirroring the
  existing `extract_query` tests). Live `server_metadata` fetch is not
  unit-testable (matches the existing OAuth limitation).
- C++: if the LoginView test surface allows, a focused test that the link is
  hidden until `registration_supported_` is set; otherwise covered by manual
  smoke.
- Manual smoke (qt6): resolve a MAS homeserver with signups enabled → the link
  appears → click → browser opens the provider signup page → completing it logs
  in (same `/callback`); resolve a homeserver with signups disabled → link stays
  hidden; resolve a non-OAuth homeserver → link stays hidden.

## Out of scope

- Legacy `/register` + User-Interactive Auth (password, recaptcha, email, ToS)
  and any non-OAuth homeserver support.
- Email verification / terms acceptance (handled by the provider's web page).
- Caching registration-support across sessions or beyond the per-resolved-
  homeserver probe.

## Files touched (anticipated)

- `sdk/src/oauth.rs` — `begin(..., register)` + `prompt(Prompt::Create)`;
  `supports_registration` helper.
- `sdk/src/bridge.rs` — `oauth_begin` bool param; new
  `homeserver_supports_registration`.
- `sdk/src/client.rs` — thread `register`; impl + `#[cfg(test)]` stubs; URL test.
- `client/include/tesseract/client.h` + `client/src/client.cpp` — `begin_oauth`
  bool default arg; `homeserver_supports_registration` wrapper.
- `ui/shared/views/LoginView.h` + `ui/shared/views/LoginView.cpp` — the link,
  the gating probe, the shared register/login OAuth routine.
