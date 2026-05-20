# Server Info on Login Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After login or session restore, fetch the homeserver's supported Matrix spec versions (`/_matrix/client/versions`) and enabled capabilities (`/_matrix/client/v3/capabilities`), store the result in an in-memory `ServerInfo` struct that the shell and views can query to conditionally show or hide UI elements, and display the homeserver URL in a read-only "Server" tab in Settings.

**Architecture:** A new blocking `get_server_info()` method on the Rust `ClientFfi` fetches both endpoints concurrently (using reqwest, mirroring the `discover_homeserver` pattern) and returns a JSON blob. The C++ `client` layer parses this into a `ServerInfo` struct. `ShellBase` triggers the fetch on a detached thread when the room-list reaches the Running state (covering both fresh login and session restore), stores the result as `server_info_`, and delivers it to subclasses via a `on_server_info_ready_ui_()` virtual. A new `ServerSection` widget renders only the homeserver URL in a read-only "Server" tab in `SettingsView`. All four platform shells wire the update through their existing `SettingsWidget` populate path. The full struct (including capability booleans) is available to any view for programmatic feature-gating.

**Tech Stack:** C++20, Rust async (tokio `block_on`), reqwest, serde_json, Catch2, cross-platform widget toolkit (`tk`).

---

## File Map

| File | Change |
|------|--------|
| `sdk/src/client.rs` | Add `pub fn get_server_info(&self) -> String` |
| `sdk/src/bridge.rs` | Expose `fn get_server_info(self: &ClientFfi) -> String` |
| `client/include/tesseract/client.h` | Add `ServerInfo` struct; add `get_server_info()` to `Client` |
| `client/src/client.cpp` | Implement `Client::get_server_info()` + JSON parse helpers + `ServerInfo::from_json()` |
| `ui/shared/app/ShellBase.h` | Add `server_info_` member + `begin_server_info_fetch_()` + `on_server_info_ready_ui_()` virtual |
| `ui/shared/app/EventHandlerBase.cpp` | Call `begin_server_info_fetch_()` when state==Running |
| `ui/shared/views/settings/ServerSection.h` | New read-only display widget (homeserver URL only) |
| `ui/shared/views/settings/ServerSection.cpp` | Widget implementation |
| `ui/shared/views/SettingsView.h` | Add `set_server_info()` declaration |
| `ui/shared/views/SettingsView.cpp` | Add "Server" tab + `set_server_info()` implementation |
| `ui/linux-qt/src/MainWindow.cpp` | Override `on_server_info_ready_ui_()` |
| `ui/linux-gtk/src/MainWindow.cpp` | Override `on_server_info_ready_ui_()` |
| `ui/windows/src/MainWindow.cpp` | Override `on_server_info_ready_ui_()` |
| `ui/macos/src/MainWindowController.mm` | Override `on_server_info_ready_ui_()` |
| `tests/cpp/test_server_info.cpp` | New test file |

---

## Task 1: Rust — `get_server_info()` on `ClientFfi`

**Files:**
- Modify: `sdk/src/client.rs`
- Modify: `sdk/src/bridge.rs`

`/_matrix/client/versions` requires no auth; `/_matrix/client/v3/capabilities` requires a Bearer token. Both are fetched concurrently with `tokio::join!`. The versions response also contains `unstable_features` — check `unstable_features["org.matrix.msc3030"]` to detect Jump-to-Date support. Pattern mirrors `discover_homeserver`.

- [ ] **Step 1.1: Write the failing Rust test**

Add to the `#[cfg(test)]` block at the bottom of `sdk/src/client.rs`:

```rust
#[cfg(test)]
mod server_info_tests {
    use super::*;

    #[test]
    fn get_server_info_no_client_returns_empty() {
        let ffi = ClientFfi::new();
        // Client not logged in — should return an empty string, not panic.
        assert!(ffi.get_server_info().is_empty());
    }
}
```

- [ ] **Step 1.2: Run to confirm it fails**

```bash
cargo test -p tesseract-sdk-ffi get_server_info 2>&1 | tail -10
```

Expected: compile error — `get_server_info` not defined.

- [ ] **Step 1.3: Add `get_server_info` to `ClientFfi` in `sdk/src/client.rs`**

Find the `impl ClientFfi` block (search for `pub fn export_session`). Add after `export_session`:

```rust
    pub fn get_server_info(&self) -> String {
        let Some(client) = &self.client else {
            return String::new();
        };
        let client = client.clone();

        self.rt.block_on(async move {
            let base = {
                let url = client.homeserver().to_string();
                url.trim_end_matches('/').to_owned()
            };
            let access_token = client.access_token().unwrap_or_default();
            let http = reqwest::Client::new();

            let (versions_res, caps_res) = tokio::join!(
                http.get(format!("{base}/_matrix/client/versions")).send(),
                http.get(format!("{base}/_matrix/client/v3/capabilities"))
                    .bearer_auth(&access_token)
                    .send()
            );

            let versions_json: serde_json::Value = versions_res
                .ok()
                .and_then(|r| r.json().ok())
                .unwrap_or(serde_json::Value::Null);

            let spec_versions: Vec<String> = versions_json["versions"]
                .as_array()
                .map(|arr| {
                    arr.iter()
                        .filter_map(|s| s.as_str().map(str::to_owned))
                        .collect()
                })
                .unwrap_or_default();

            // MSC3030 (Jump to Date) is advertised in unstable_features.
            let supports_msc3030 = versions_json
                .pointer("/unstable_features/org.matrix.msc3030")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);

            let caps: serde_json::Value = caps_res
                .ok()
                .and_then(|r| r.json().ok())
                .unwrap_or(serde_json::Value::Null);

            let cap_bool = |key: &str| -> bool {
                caps.pointer(&format!("/capabilities/{key}/enabled"))
                    .and_then(|v| v.as_bool())
                    .unwrap_or(true)
            };
            let default_room_ver = caps
                .pointer("/capabilities/m.room_versions/default")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_owned();

            serde_json::json!({
                "homeserver": base,
                "spec_versions": spec_versions,
                "supports_msc3030": supports_msc3030,
                "can_change_password": cap_bool("m.change_password"),
                "can_set_displayname": cap_bool("m.set_displayname"),
                "can_set_avatar": cap_bool("m.set_avatar_url"),
                "default_room_version": default_room_ver
            })
            .to_string()
        })
    }
```

- [ ] **Step 1.4: Expose in the cxx bridge**

In `sdk/src/bridge.rs`, find the `extern "Rust"` block that contains `fn export_session(self: &ClientFfi) -> String;`. Add after it:

```rust
        /// Fetch homeserver spec versions and enabled capabilities.
        /// Returns JSON: `{"homeserver":"...","spec_versions":[...],"supports_msc3030":bool,"can_change_password":bool,...}`
        /// Returns an empty string when not logged in or on network error.
        /// Blocks the calling thread — call only from a worker thread.
        fn get_server_info(self: &ClientFfi) -> String;
```

- [ ] **Step 1.5: Run the Rust tests**

```bash
cargo test -p tesseract-sdk-ffi get_server_info 2>&1 | tail -10
```

Expected: `test server_info_tests::get_server_info_no_client_returns_empty ... ok`

- [ ] **Step 1.6: Commit**

```bash
git add sdk/src/client.rs sdk/src/bridge.rs
git commit -m "feat(sdk): add get_server_info() — fetch spec versions, capabilities, and MSC3030"
```

---

## Task 2: C++ client — `ServerInfo` struct + `Client::get_server_info()`

**Files:**
- Modify: `client/include/tesseract/client.h`
- Modify: `client/src/client.cpp`
- Create: `tests/cpp/test_server_info.cpp`

`session_store.cpp` already contains hand-rolled JSON scanners (`extract_string`, `extract_string_array`). `client.cpp` lives in the same `client/src/` directory but is a separate translation unit — reimplement the two scanner helpers locally in an anonymous namespace in `client.cpp` (they are small, < 30 lines each, and this avoids exposing them as internal API).

- [ ] **Step 2.1: Write the failing C++ tests**

Create `tests/cpp/test_server_info.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "tesseract/client.h"
#include <string>
#include <vector>

using tesseract::ServerInfo;

TEST_CASE("ServerInfo::from_json: full blob", "[server][info]")
{
    const std::string json = R"({
        "homeserver": "https://matrix.org",
        "spec_versions": ["v1.1", "v1.2", "v1.3"],
        "supports_msc3030": true,
        "can_change_password": false,
        "can_set_displayname": true,
        "can_set_avatar": true,
        "default_room_version": "10"
    })";

    ServerInfo info = ServerInfo::from_json(json);
    CHECK(info.homeserver_url == "https://matrix.org");
    REQUIRE(info.spec_versions.size() == 3);
    CHECK(info.spec_versions[0] == "v1.1");
    CHECK(info.spec_versions[2] == "v1.3");
    CHECK(info.supports_msc3030 == true);
    CHECK(info.can_change_password == false);
    CHECK(info.can_set_displayname == true);
    CHECK(info.can_set_avatar == true);
    CHECK(info.default_room_version == "10");
}

TEST_CASE("ServerInfo::from_json: missing caps default to true, msc3030 defaults false",
          "[server][info]")
{
    const std::string json = R"({"homeserver":"https://example.com","spec_versions":[]})";
    ServerInfo info = ServerInfo::from_json(json);
    CHECK(info.supports_msc3030 == false);
    CHECK(info.can_change_password == true);
    CHECK(info.can_set_displayname == true);
    CHECK(info.can_set_avatar == true);
    CHECK(info.default_room_version.empty());
}

TEST_CASE("ServerInfo::from_json: empty string returns default-constructed", "[server][info]")
{
    ServerInfo info = ServerInfo::from_json("");
    CHECK(info.homeserver_url.empty());
    CHECK(info.spec_versions.empty());
    CHECK(info.supports_msc3030 == false);
    CHECK(info.can_change_password == true);
}

TEST_CASE("ServerInfo::from_json: spec_versions array", "[server][info]")
{
    const std::string json =
        R"({"spec_versions":["v1.4","v1.5","v1.6"],"homeserver":"h"})";
    ServerInfo info = ServerInfo::from_json(json);
    REQUIRE(info.spec_versions.size() == 3);
    CHECK(info.spec_versions[1] == "v1.5");
}
```

Add `test_server_info.cpp` to `tests/CMakeLists.txt` (in the `target_sources` block alongside the other test files).

- [ ] **Step 2.2: Run to confirm it fails to compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | head -10
```

Expected: compile error — `tesseract::ServerInfo` not defined.

- [ ] **Step 2.3: Add `ServerInfo` to `client/include/tesseract/client.h`**

Find the line with `struct OAuthFlow` (near the top of the file, before `class Client`). Add `ServerInfo` before `OAuthFlow`:

```cpp
/// Information about the connected homeserver fetched after login.
/// Bool capability fields default to `true` (permissive) when absent —
/// old servers omit capabilities they support.
/// `supports_msc3030` defaults to `false` — opt-in unstable feature.
struct ServerInfo
{
    std::string homeserver_url;
    std::vector<std::string> spec_versions;  ///< e.g. ["v1.1","v1.2","v1.5"]
    bool supports_msc3030    = false;        ///< Jump-to-Date (org.matrix.msc3030)
    bool can_change_password = true;
    bool can_set_displayname = true;
    bool can_set_avatar      = true;
    std::string default_room_version;        ///< e.g. "10"; empty when absent

    /// Parse from the JSON blob returned by `Client::get_server_info()`.
    /// Missing/malformed fields use the defaults above.
    static ServerInfo from_json(const std::string& json);
};
```

Also add `#include <vector>` to `client.h` if not already present.

Add `get_server_info()` to the `Client` class's public section (after `export_session`):

```cpp
    /// Fetch homeserver spec versions and enabled capabilities.
    /// Blocks the calling thread — must be called from a worker thread.
    /// Returns a default-constructed `ServerInfo` on network error or when
    /// not logged in.
    ServerInfo get_server_info() const;
```

- [ ] **Step 2.4: Implement in `client/src/client.cpp`**

Add these helpers to the anonymous namespace at the top of `client.cpp`. These are equivalent to the scanners in `session_store.cpp`:

```cpp
namespace
{

static std::string si_extract_string(std::string_view json,
                                     std::string_view key)
{
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(pos, end - pos));
}

static std::vector<std::string>
si_extract_string_array(std::string_view json, std::string_view key)
{
    std::vector<std::string> out;
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return out;
    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
        ++pos;
    if (pos >= json.size() || json[pos] != '[') return out;
    ++pos;
    while (pos < json.size())
    {
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ',' ||
                json[pos] == '\n' || json[pos] == '\r'))
            ++pos;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '"') break;
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string_view::npos) break;
        out.emplace_back(json.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

static bool si_extract_bool(std::string_view json, std::string_view key,
                             bool default_val)
{
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return default_val;
    pos += needle.size();
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
        ++pos;
    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")
        return true;
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false")
        return false;
    return default_val;
}

} // namespace
```

Then implement `ServerInfo::from_json` and `Client::get_server_info`:

```cpp
// ── ServerInfo ───────────────────────────────────────────────────────────────

ServerInfo ServerInfo::from_json(const std::string& json)
{
    if (json.empty())
        return {};
    ServerInfo info;
    info.homeserver_url       = si_extract_string(json, "homeserver");
    info.spec_versions        = si_extract_string_array(json, "spec_versions");
    info.supports_msc3030     = si_extract_bool(json, "supports_msc3030", false);
    info.can_change_password  = si_extract_bool(json, "can_change_password", true);
    info.can_set_displayname  = si_extract_bool(json, "can_set_displayname", true);
    info.can_set_avatar       = si_extract_bool(json, "can_set_avatar", true);
    info.default_room_version = si_extract_string(json, "default_room_version");
    return info;
}

ServerInfo Client::get_server_info() const
{
    return ServerInfo::from_json(impl_->ffi->get_server_info());
}
```

(Replace `impl_->ffi` with the actual Pimpl accessor — search for `export_session` in `client.cpp` to find the pattern.)

- [ ] **Step 2.5: Build and run tests**

```bash
cmake --preset linux-qt6-debug && \
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "\[server\]\[info\]" --output-on-failure
```

Expected: 4 tests pass.

- [ ] **Step 2.6: Run full suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 2.7: Commit**

```bash
git add client/include/tesseract/client.h \
        client/src/client.cpp \
        tests/cpp/test_server_info.cpp \
        tests/CMakeLists.txt
git commit -m "feat(client): add ServerInfo struct and Client::get_server_info()"
```

---

## Task 3: ShellBase — fetch trigger + storage + virtual delivery

**Files:**
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/EventHandlerBase.cpp`

When the room-list state transitions to Running (value 3), it signals that the matrix-sdk sync is active — the right moment to fetch server info. `EventHandlerBase` already marshals `on_room_list_state` to the UI thread and calls `handle_room_list_state_ui_()`. We add one extra call there.

- [ ] **Step 3.1: Add to `ShellBase.h`**

In `ShellBase.h`, add to the `protected:` members section (alongside other cached state like display name, avatar URL, etc.):

```cpp
    tesseract::ServerInfo server_info_;        ///< populated after first sync
```

Add to the `protected:` virtual methods section (alongside other `handle_*_ui_` virtuals):

```cpp
    /// Called on the UI thread after `server_info_` has been populated.
    /// Override in shells that gate UI elements on server capabilities.
    virtual void on_server_info_ready_ui_() {}
```

Add this **non-virtual** protected method declaration (implemented inline — it is tiny):

```cpp
    /// Spawn a detached thread to call Client::get_server_info(), then
    /// marshal the result back to the UI thread. Only fetches once per session.
    void begin_server_info_fetch_()
    {
        if (server_info_fetch_started_)
            return;
        server_info_fetch_started_ = true;
        std::thread([this] {
            auto info = client_->get_server_info();
            post_to_ui_([this, info = std::move(info)] {
                server_info_ = info;
                on_server_info_ready_ui_();
            });
        }).detach();
    }
```

Also add the guard flag to the `private:` section:

```cpp
    bool server_info_fetch_started_ = false;
```

Make sure `<thread>` is included at the top of `ShellBase.h` — search for the existing `#include` list; add `#include <thread>` if absent.

- [ ] **Step 3.2: Trigger from `EventHandlerBase.cpp`**

In `ui/shared/app/EventHandlerBase.cpp`, find the lambda passed to `post_to_ui_` inside the `on_room_list_state` handler. It will look something like:

```cpp
void EventHandlerBridge::on_room_list_state(uint8_t state)
{
    shell_->post_to_ui_([shell = shell_, state] {
        shell->handle_room_list_state_ui_(state);
    });
}
```

Change it to also trigger the fetch when state == 3 (Running):

```cpp
void EventHandlerBridge::on_room_list_state(uint8_t state)
{
    shell_->post_to_ui_([shell = shell_, state] {
        shell->handle_room_list_state_ui_(state);
        if (state == 3)
            shell->begin_server_info_fetch_();
    });
}
```

- [ ] **Step 3.3: Build to confirm it compiles**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:|warning:" | head -20
```

Expected: clean build, no errors.

- [ ] **Step 3.4: Run full suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests still pass.

- [ ] **Step 3.5: Commit**

```bash
git add ui/shared/app/ShellBase.h ui/shared/app/EventHandlerBase.cpp
git commit -m "feat(shell): fetch server info after room-list reaches Running state"
```

---

## Task 4: `ServerSection` widget + SettingsView "Server" tab

**Files:**
- Create: `ui/shared/views/settings/ServerSection.h`
- Create: `ui/shared/views/settings/ServerSection.cpp`
- Modify: `ui/shared/views/SettingsView.h`
- Modify: `ui/shared/views/SettingsView.cpp`
- Test: `tests/cpp/test_server_info.cpp` (extend existing file)

`ServerSection` follows the same pattern as `AccountSection` — a read-only display widget. It shows only the homeserver URL, using the same two-column label/value layout as other sections.

- [ ] **Step 4.1: Write the failing test**

Add to `tests/cpp/test_server_info.cpp` (after the existing tests). Add the `Stage` fixture from `test_compose_bar_media.cpp` — it provides `layout_ctx()` and `paint_ctx()`:

```cpp
#include "tk_test_surface.h"
#include "views/settings/ServerSection.h"

namespace
{
struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);
    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
};
} // namespace

TEST_CASE("ServerSection: default-constructed has zero height before set_server_info",
          "[server][ui]")
{
    Stage st;
    tesseract::views::ServerSection sec;
    auto lc = st.layout_ctx();
    auto sz = sec.measure(lc, {640.0f, 480.0f});
    CHECK(sz.w >= 0.0f);
    CHECK(sz.h >= 0.0f);
}

TEST_CASE("ServerSection: set_server_info produces non-zero height and paints without crash",
          "[server][ui]")
{
    Stage st;
    tesseract::views::ServerSection sec;

    tesseract::ServerInfo info;
    info.homeserver_url = "https://matrix.org";
    sec.set_server_info(info);

    auto lc = st.layout_ctx();
    auto sz = sec.measure(lc, {640.0f, 480.0f});
    CHECK(sz.h > 0.0f);

    sec.arrange(lc, {0.0f, 0.0f, sz.w, sz.h});
    auto pc = st.paint_ctx();
    sec.paint(pc); // must not crash
}
```

- [ ] **Step 4.2: Run to confirm it fails to compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | head -10
```

Expected: compile error — `tesseract::views::ServerSection` not defined.

- [ ] **Step 4.3: Create `ui/shared/views/settings/ServerSection.h`**

```cpp
#pragma once

#include "tk/widget.h"
#include <tesseract/client.h>

#include <memory>
#include <string>

namespace tk
{
class TextLayout;
}

namespace tesseract::views
{

class ServerSection : public tk::Widget
{
public:
    ServerSection() = default;
    ~ServerSection() override = default;

    void set_server_info(const tesseract::ServerInfo& info);

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    std::string homeserver_url_;
    std::unique_ptr<tk::TextLayout> label_layout_;
    std::unique_ptr<tk::TextLayout> value_layout_;
    bool dirty_ = false;
};

} // namespace tesseract::views
```

- [ ] **Step 4.4: Create `ui/shared/views/settings/ServerSection.cpp`**

```cpp
#include "ServerSection.h"

#include "tk/canvas.h"
#include "tk/widget.h"
#include "visual.h"

namespace
{
constexpr float kPadX   = 24.0f;
constexpr float kPadY   = 16.0f;
constexpr float kRowH   = 22.0f;
constexpr float kLabelW = 160.0f;
} // namespace

namespace tesseract::views
{

void ServerSection::set_server_info(const tesseract::ServerInfo& info)
{
    homeserver_url_ = info.homeserver_url;
    label_layout_   = nullptr;
    value_layout_   = nullptr;
    dirty_          = true;
}

tk::Size ServerSection::measure(tk::LayoutCtx& /*ctx*/, tk::Size constraints)
{
    if (homeserver_url_.empty())
        return {constraints.w, 0.0f};
    return {constraints.w, kPadY * 2.0f + kRowH};
}

void ServerSection::arrange(tk::LayoutCtx& /*ctx*/, tk::Rect /*bounds*/) {}

void ServerSection::paint(tk::PaintCtx& ctx)
{
    if (homeserver_url_.empty())
        return;

    const tk::Rect b = bounds();
    const tk::Color label_col = tk::Theme::is_dark(ctx.theme())
                                    ? tk::Color::rgb(0x9e9e9e)
                                    : tk::Color::rgb(0x757575);
    const tk::Color value_col = visual::kColorPrimary(ctx.theme());

    if (!label_layout_ || dirty_)
    {
        tk::TextStyle ls;
        ls.role      = tk::FontRole::Body;
        ls.trim      = tk::TextTrim::Ellipsis;
        ls.max_width = kLabelW;
        label_layout_ = ctx.factory.build_text("Homeserver", ls);

        tk::TextStyle vs;
        vs.role      = tk::FontRole::Body;
        vs.trim      = tk::TextTrim::Ellipsis;
        vs.max_width = std::max(0.0f, b.w - kPadX * 2.0f - kLabelW - 8.0f);
        value_layout_ = ctx.factory.build_text(homeserver_url_, vs);
        dirty_ = false;
    }

    const float y = b.y + kPadY;
    ctx.canvas.draw_text(*label_layout_, {b.x + kPadX, y}, label_col);
    ctx.canvas.draw_text(*value_layout_, {b.x + kPadX + kLabelW + 8.0f, y},
                         value_col);
}

} // namespace tesseract::views
```

- [ ] **Step 4.5: Add `ServerSection.cpp` to the CMake build**

In `ui/shared/CMakeLists.txt`, find the `target_sources(tesseract_tk …)` block listing other settings source files (e.g. `views/settings/AccountSection.cpp`). Add:

```cmake
    views/settings/ServerSection.cpp
```

- [ ] **Step 4.6: Add `set_server_info` to `SettingsView.h`**

In `ui/shared/views/SettingsView.h`:

Add the include at the top (alongside other settings section includes):
```cpp
#include "settings/ServerSection.h"
```

Add to the public setters section (alongside `set_account_info`, etc.):
```cpp
    void set_server_info(const tesseract::ServerInfo& info);
```

Add to the `private:` section:
```cpp
    ServerSection* server_section_ = nullptr;
```

- [ ] **Step 4.7: Implement in `SettingsView.cpp`**

In the constructor (`SettingsView::SettingsView()`), after the existing last `tabs->add_tab(...)` line, add:

```cpp
    auto server = std::make_unique<ServerSection>();
    server_section_ = server.get();
    tabs->add_tab("Server", std::move(server));
```

Add the implementation of `set_server_info` at the bottom of `SettingsView.cpp`:

```cpp
void SettingsView::set_server_info(const tesseract::ServerInfo& info)
{
    server_section_->set_server_info(info);
}
```

- [ ] **Step 4.8: Build and run tests**

```bash
cmake --preset linux-qt6-debug && \
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "\[server\]" --output-on-failure
```

Expected: 6 tests pass (4 from Task 2 + 2 new UI tests).

- [ ] **Step 4.9: Run full suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 4.10: Commit**

```bash
git add ui/shared/views/settings/ServerSection.h \
        ui/shared/views/settings/ServerSection.cpp \
        ui/shared/views/SettingsView.h \
        ui/shared/views/SettingsView.cpp \
        ui/shared/CMakeLists.txt \
        tests/cpp/test_server_info.cpp
git commit -m "feat(ui): add ServerSection widget and Settings 'Server' tab (homeserver URL)"
```

---

## Task 5: Platform shell wiring

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`
- Modify: `ui/windows/src/MainWindow.cpp`
- Modify: `ui/macos/src/MainWindowController.mm`

Each shell has a `SettingsWidget` (or equivalent) that wraps `SettingsView`. The shell overrides `on_server_info_ready_ui_()` to call `SettingsWidget::set_server_info()`. Each platform's change is identical in structure.

For each shell, first check how the SettingsWidget is accessed — it will be `settings_widget_` or `settings_window_` or similar. Search for `on_theme_changed` or `populate` to find the wiring block.

- [ ] **Step 5.1: Add `set_server_info` to each platform's SettingsWidget**

For **each** of the four platforms, find the SettingsWidget class header (e.g. `ui/linux-qt/src/SettingsWidget.h`) and add:

```cpp
    void set_server_info(const tesseract::ServerInfo& info);
```

And in the corresponding `.cpp` (after the existing `populate()` method):

```cpp
void SettingsWidget::set_server_info(const tesseract::ServerInfo& info)
{
    settings_view_->set_server_info(info);
}
```

- [ ] **Step 5.2: Override `on_server_info_ready_ui_()` in each shell**

### Qt6 (`ui/linux-qt/src/MainWindow.cpp`)

Find where other `on_*_ui_()` overrides are (e.g., `on_room_list_state_ui_`). Add:

```cpp
void MainWindow::on_server_info_ready_ui_()
{
    if (settings_widget_)
        settings_widget_->set_server_info(server_info_);
}
```

Add to `MainWindow.h`:
```cpp
    void on_server_info_ready_ui_() override;
```

### GTK4 (`ui/linux-gtk/src/MainWindow.cpp`)

```cpp
void MainWindow::on_server_info_ready_ui_()
{
    if (settings_widget_)
        settings_widget_->set_server_info(server_info_);
}
```

Add to `MainWindow.h`.

### Win32 (`ui/windows/src/MainWindow.cpp`)

```cpp
void MainWindow::on_server_info_ready_ui_()
{
    if (settings_window_)
        settings_window_->set_server_info(server_info_);
}
```

Add to `MainWindow.h` (Win32 may use `settings_window_` instead of `settings_widget_` — check by searching for `on_theme_changed`).

### macOS (`ui/macos/src/MainWindowController.mm`)

```objc
void MacShell::on_server_info_ready_ui_()
{
    if (controller_->settingsWindowController_)
        [controller_->settingsWindowController_ setServerInfo:server_info_];
}
```

The exact pattern depends on how macOS exposes SettingsView — look for where `on_theme_changed` is forwarded in `MainWindowController.mm` and follow the same structure.

- [ ] **Step 5.3: Build the Qt6 shell**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:" | head -20
```

Expected: clean build.

- [ ] **Step 5.4: Run full suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 5.5: Manual verification**

Launch the app and log in (or relaunch with an existing session):

```bash
./build/linux-qt6-debug/ui/linux-qt/tesseract
```

1. After the room list loads, open Settings.
2. Click the "Server" tab.
3. Verify: "Homeserver" label and the URL value appear.
4. Open Settings before sync finishes (fast machine / cached session) — "Server" tab should show empty content (no crash).
5. Verify other tabs (Account, Appearance, Notifications, Media) are unaffected.

- [ ] **Step 5.6: Commit**

```bash
git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp \
        ui/linux-gtk/src/MainWindow.h ui/linux-gtk/src/MainWindow.cpp \
        ui/windows/src/MainWindow.h   ui/windows/src/MainWindow.cpp \
        ui/macos/src/MainWindowController.mm
# (also add any SettingsWidget.h/.cpp files changed)
git commit -m "feat(shells): wire on_server_info_ready_ui_ to update Settings Server tab"
```

---

## Verification

End-to-end:

1. Build and run: `cmake --build build/linux-qt6-debug && ./build/linux-qt6-debug/ui/linux-qt/tesseract`
2. Log in with a Matrix account.
3. Open Settings → Server tab.
4. Confirm: homeserver URL appears.
5. Quit and relaunch (session restore path) — same data appears.
6. All tests pass: `ctest --test-dir build/linux-qt6-debug --output-on-failure`
