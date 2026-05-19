# Image / Sticker Thumbnail Fetch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Use the server-provided `info.thumbnail_source` for `m.image` and `m.sticker` timeline rows instead of always fetching the full media, so inline row display is faster and lighter on bandwidth.

**Architecture:** Three-layer change: (1) Rust SDK exposes a new `image_thumbnail_json` field in `TimelineEvent` by extracting `info.thumbnail_source` for image and sticker events; (2) the C++ FFI bridge populates a new `thumbnail_url` field on `ImageEvent`/`StickerEvent`; (3) `MessageRowData` gains a parallel `thumbnail_url` field that `ShellBase` pre-fetches and `MessageListView` uses for inline display, while `media_url` (the full-resolution source) is untouched and is still passed to `ImageViewerOverlay` when the user clicks an image.

**Tech Stack:** Rust / ruma (`ImageMessageEventContent`, `StickerEventContent`, `MediaSource`), C++17, existing `ensure_media_image_` / `image_provider_` pipeline.

---

## File Map

| File | Change |
|---|---|
| `sdk/src/lib.rs` | Add `pub image_thumbnail_json: String` to `TimelineEvent` |
| `sdk/src/client.rs` | Populate it for `m.image` and `m.sticker`; add Rust unit tests |
| `client/include/tesseract/types.h` | Add `std::string thumbnail_url` to `ImageEvent` and `StickerEvent` |
| `client/src/ffi_convert.h` | Assign `e.image_thumbnail_json` → `ev->thumbnail_url` in both handlers |
| `ui/shared/views/MessageListView.h` | Add `std::string thumbnail_url` to `MessageRowData` |
| `ui/shared/views/MessageListView.cpp` | Populate `thumbnail_url` in `make_row_data`; prefer it in inline image paint |
| `ui/shared/app/ShellBase.cpp` | Pre-fetch thumbnail (when present) for inline display; still pre-fetch full image for viewer readiness |
| `tests/cpp/test_types.cpp` | Add `ImageEvent`/`StickerEvent` `thumbnail_url` default + round-trip tests |

---

## Background: existing patterns

**`video_thumbnail_json` pipeline (the model to replicate):**
- `sdk/src/lib.rs` line 75: `pub video_thumbnail_json: String` in `TimelineEvent`
- `sdk/src/client.rs` ~line 5803: extracted from `info.thumbnail_source` as plain URI or serialised JSON
- `client/src/ffi_convert.h` line 244: `ev->thumbnail_url = std::string(e.video_thumbnail_json);`
- `client/include/tesseract/types.h` line 197: `std::string thumbnail_url;` on `VideoEvent`
- `ui/shared/views/MessageListView.cpp` line 118: `row.video_thumb_url = vid.thumbnail_url.empty() ? ("thumb::" + ev.event_id) : vid.thumbnail_url;`
- `ui/shared/app/ShellBase.cpp` line 405-408: `ensure_media_image_(vid.thumbnail_url, ...)` when non-empty

**`ensure_media_image_` deduplicates by URL** — calling it for two different URLs starts two independent fetches keyed separately; no conflict.

**Inline image display uses `m.media_url` as the `image_provider_` key** (MessageListView.cpp ~lines 1464-1465):
```cpp
(owner_.image_provider_ && !m.media_url.empty())
    ? owner_.image_provider_(m.media_url)
    : nullptr;
```
This is what we change to prefer `thumbnail_url` when available.

**`ImageHit::media_url` is the full-resolution URL** passed to `ImageViewerOverlay::open` on click. It must stay as `media_url` (full), not the thumbnail.

---

### Task 1: Rust SDK — add `image_thumbnail_json` to `TimelineEvent`

**Files:**
- Modify: `sdk/src/lib.rs:75`
- Modify: `sdk/src/client.rs` (4 locations detailed below)
- Test: `sdk/src/client.rs` (add Rust unit tests in the `#[cfg(test)] mod tests` block)

- [ ] **Step 1: Write two failing Rust unit tests**

At the bottom of the `#[cfg(test)] mod tests` block in `sdk/src/client.rs`, add:

```rust
#[test]
fn image_info_thumbnail_source_round_trips_plain_uri() {
    use matrix_sdk::ruma::events::room::message::ImageMessageEventContent;
    let json = serde_json::json!({
        "body": "photo.jpg",
        "msgtype": "m.image",
        "url": "mxc://example.org/full",
        "info": {
            "w": 1920, "h": 1080, "mimetype": "image/jpeg",
            "thumbnail_url": "mxc://example.org/thumb",
            "thumbnail_info": { "w": 320, "h": 200, "mimetype": "image/jpeg" }
        }
    });
    let content: ImageMessageEventContent =
        serde_json::from_value(json).expect("deserialises");
    let thumb_src = content
        .info
        .as_ref()
        .and_then(|info| info.thumbnail_source.as_ref());
    assert!(thumb_src.is_some(), "thumbnail_source must be populated from thumbnail_url");
    match thumb_src.unwrap() {
        matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => {
            assert_eq!(uri.to_string(), "mxc://example.org/thumb");
        }
        _ => panic!("expected Plain MediaSource"),
    }
}

#[test]
fn image_info_without_thumbnail_has_none_thumbnail_source() {
    use matrix_sdk::ruma::events::room::message::ImageMessageEventContent;
    let json = serde_json::json!({
        "body": "photo.jpg",
        "msgtype": "m.image",
        "url": "mxc://example.org/full",
        "info": { "w": 800, "h": 600, "mimetype": "image/png" }
    });
    let content: ImageMessageEventContent =
        serde_json::from_value(json).expect("deserialises");
    let thumb_src = content
        .info
        .as_ref()
        .and_then(|info| info.thumbnail_source.as_ref());
    assert!(thumb_src.is_none(), "no thumbnail → None");
}
```

- [ ] **Step 2: Run to verify they compile and pass** (they test ruma parsing, not our new field yet)

```bash
cargo test -p tesseract-sdk-ffi image_info_thumbnail 2>&1 | tail -5
```

Expected: both PASS.

- [ ] **Step 3: Add `image_thumbnail_json` field to `TimelineEvent` in `sdk/src/lib.rs`**

After line 75 (`pub video_thumbnail_json: String,`), add:

```rust
        pub image_thumbnail_json: String,
```

- [ ] **Step 4: Add `image_thumbnail_json: String::new()` to the three early-return `TimelineEvent` constructions in `sdk/src/client.rs`**

**Early return 1** (~line 5315, the virtual-event / local-echo path): after `video_thumbnail_json: String::new(),`, add:
```rust
                image_thumbnail_json: String::new(),
```

**Early return 2** (~line 5409, the redacted/tombstone path): after `video_thumbnail_json: String::new(),`, add:
```rust
                image_thumbnail_json: String::new(),
```

**Sticker early return** (~line 5498): after `video_thumbnail_json: String::new(),`, add:
```rust
                image_thumbnail_json: c.info.thumbnail_source
                    .as_ref()
                    .map(|ts| match ts {
                        matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => {
                            uri.to_string()
                        }
                        matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) => {
                            serde_json::to_string(ts).unwrap_or_default()
                        }
                    })
                    .unwrap_or_default(),
```

- [ ] **Step 5: Add image thumbnail extraction for the main message path in `sdk/src/client.rs`**

Between the end of `let (in_reply_to_id, in_reply_to_sender_name, in_reply_to_body) = ...;` block (~line 6061) and `let reactions = ...` (~line 6063), add:

```rust
    let image_thumbnail_json: String = match msg_content.msgtype() {
        MessageType::Image(i) => i
            .info
            .as_ref()
            .and_then(|info| info.thumbnail_source.as_ref())
            .map(|ts| match ts {
                matrix_sdk::ruma::events::room::MediaSource::Plain(uri) => uri.to_string(),
                matrix_sdk::ruma::events::room::MediaSource::Encrypted(_) => {
                    serde_json::to_string(ts).unwrap_or_default()
                }
            })
            .unwrap_or_default(),
        _ => String::new(),
    };
```

- [ ] **Step 6: Use `image_thumbnail_json` in the `TimelineEvent { ... }` struct init (~line 6089)**

After `video_thumbnail_json,`, add:
```rust
        image_thumbnail_json,
```

- [ ] **Step 7: Build to verify no compile errors**

```bash
cargo build -p tesseract-sdk-ffi 2>&1 | tail -5
```

Expected: `Finished` with no errors. (If the compiler says `image_thumbnail_json` not initialised in a `TimelineEvent` literal, check Step 4 — all three early-return blocks must include it.)

- [ ] **Step 8: Run Rust tests**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
```

Expected: all tests pass (count should be 96+2 = 98 or whatever the current count + 2 new).

- [ ] **Step 9: Commit**

```bash
git add sdk/src/lib.rs sdk/src/client.rs
git commit -m "feat(sdk): expose image_thumbnail_json in TimelineEvent for m.image and m.sticker"
```

---

### Task 2: C++ types + FFI bridge

**Files:**
- Modify: `client/include/tesseract/types.h:115-130` (ImageEvent), `132-145` (StickerEvent)
- Modify: `client/src/ffi_convert.h:181-204` (m.image and m.sticker handlers)
- Test: `tests/cpp/test_types.cpp`

- [ ] **Step 1: Write failing C++ tests**

In `tests/cpp/test_types.cpp`, after the existing `StickerEvent fields are settable` test (around line 258), add:

```cpp
TEST_CASE("ImageEvent thumbnail_url defaults to empty", "[types]")
{
    tesseract::ImageEvent ev{};
    CHECK(ev.thumbnail_url.empty());
}

TEST_CASE("ImageEvent thumbnail_url and image_url are independent", "[types]")
{
    tesseract::ImageEvent ev{};
    ev.image_url = "mxc://example.org/full";
    ev.thumbnail_url = "mxc://example.org/thumb";
    CHECK(ev.image_url == "mxc://example.org/full");
    CHECK(ev.thumbnail_url == "mxc://example.org/thumb");
}

TEST_CASE("StickerEvent thumbnail_url defaults to empty", "[types]")
{
    tesseract::StickerEvent ev{};
    CHECK(ev.thumbnail_url.empty());
}

TEST_CASE("StickerEvent thumbnail_url and image_url are independent", "[types]")
{
    tesseract::StickerEvent ev{};
    ev.image_url = "mxc://example.org/sticker";
    ev.thumbnail_url = "mxc://example.org/sticker-thumb";
    CHECK(ev.image_url == "mxc://example.org/sticker");
    CHECK(ev.thumbnail_url == "mxc://example.org/sticker-thumb");
}
```

- [ ] **Step 2: Run to verify they fail to compile**

```bash
cmake --build build/linux-gtk-debug --target tesseract_tests 2>&1 | grep "error:" | head -5
```

Expected: `error: 'class tesseract::ImageEvent' has no member named 'thumbnail_url'`

- [ ] **Step 3: Add `thumbnail_url` to `ImageEvent` in `client/include/tesseract/types.h`**

After `std::string image_url; // mxc:// URI` (line 117), add:

```cpp
    /// Thumbnail MediaSource JSON (plain mxc:// URI or serialised encrypted
    /// source); empty when the sender omitted `info.thumbnail_url`. Pass to
    /// `image_provider_` for inline timeline display. `image_url` always
    /// holds the full-resolution source for `ImageViewerOverlay`.
    std::string thumbnail_url;
```

- [ ] **Step 4: Add `thumbnail_url` to `StickerEvent` in `client/include/tesseract/types.h`**

After `std::string image_url; // mxc:// URI` (line 134), add:

```cpp
    /// Thumbnail MediaSource JSON; empty when absent. Same semantics as
    /// `ImageEvent::thumbnail_url`.
    std::string thumbnail_url;
```

- [ ] **Step 5: Populate `thumbnail_url` in `ffi_convert.h` for `m.image`**

In `client/src/ffi_convert.h`, in the `if (msg_type == "m.image")` block (~line 181), after `ev->animated = e.image_animated;` (line 190), add:

```cpp
        ev->thumbnail_url = std::string(e.image_thumbnail_json);
```

- [ ] **Step 6: Populate `thumbnail_url` in `ffi_convert.h` for `m.sticker`**

In the `if (msg_type == "m.sticker")` block (~line 194), after `ev->animated = e.image_animated;` (line 203), add:

```cpp
        ev->thumbnail_url = std::string(e.image_thumbnail_json);
```

- [ ] **Step 7: Build and run the C++ tests**

```bash
cmake --build build/linux-gtk-debug --target tesseract_tests 2>&1 | tail -5
ctest --test-dir build/linux-gtk-debug -R "thumbnail_url" --output-on-failure 2>&1 | tail -10
```

Expected: all 4 new tests PASS.

- [ ] **Step 8: Run full test suite**

```bash
ctest --test-dir build/linux-gtk-debug --output-on-failure 2>&1 | tail -5
```

Expected: 100% tests passed.

- [ ] **Step 9: Commit**

```bash
git add client/include/tesseract/types.h client/src/ffi_convert.h tests/cpp/test_types.cpp
git commit -m "feat(client): add thumbnail_url to ImageEvent and StickerEvent; wire from image_thumbnail_json FFI field"
```

---

### Task 3: MessageRowData + MessageListView + ShellBase

**Files:**
- Modify: `ui/shared/views/MessageListView.h:63-74` (`MessageRowData` image/sticker section)
- Modify: `ui/shared/views/MessageListView.cpp:70-92` (`make_row_data` image/sticker arms); `~1460-1490` (inline paint lookup)
- Modify: `ui/shared/app/ShellBase.cpp:377-388` (`ensure_row_media_` image/sticker branches)

> There is no clean unit-test seam for the ShellBase pre-fetch or the MessageListView paint lookup (both require a running Surface + image provider). Correctness is verified by compile + build.

- [ ] **Step 1: Add `thumbnail_url` to `MessageRowData` in `MessageListView.h`**

After the comment `// MSC2530 caption ...` block (~line 70), in the `// Image / Sticker` section, add after `std::string media_url; // mxc` (line 64):

```cpp
    /// Server-provided thumbnail MediaSource JSON for `m.image` and
    /// `m.sticker`. When non-empty, `image_provider_` is keyed by this
    /// string for inline row display; `media_url` is always the
    /// full-resolution source used by `ImageViewerOverlay`.
    std::string thumbnail_url;
```

- [ ] **Step 2: Populate `thumbnail_url` in `make_row_data` in `MessageListView.cpp`**

In the `case tesseract::EventType::Image:` arm (~line 70-80), after `row.media_url = img.image_url;` (line 74), add:

```cpp
        row.thumbnail_url = img.thumbnail_url;
```

In the `case tesseract::EventType::Sticker:` arm (~line 82-92), after `row.media_url = s.image_url;` (line 86), add:

```cpp
        row.thumbnail_url = s.thumbnail_url;
```

- [ ] **Step 3: Update the inline image paint lookup in `MessageListView.cpp`**

There are two places where `image_provider_` is called with `m.media_url` for image/sticker rows. Find them by searching for `owner_.image_provider_` near the image/sticker paint path (~lines 1464 and 1482).

**First occurrence** (~line 1464-1466):
```cpp
                (owner_.image_provider_ && !m.media_url.empty())
                    ? owner_.image_provider_(m.media_url)
                    : nullptr;
```

Replace with:
```cpp
                {
                    const std::string& display_key =
                        m.thumbnail_url.empty() ? m.media_url : m.thumbnail_url;
                    (owner_.image_provider_ && !display_key.empty())
                        ? owner_.image_provider_(display_key)
                        : nullptr;
                }
```

**Second occurrence** (~line 1482-1484), same pattern — apply the same replacement using a local `display_key`.

> Note: if either block is a single expression used as an initialiser (e.g., `const auto* img = ...`), wrap the replacement so the variable is still correctly initialised. The pattern is:
> ```cpp
> const std::string& display_key =
>     m.thumbnail_url.empty() ? m.media_url : m.thumbnail_url;
> const auto* img =
>     (owner_.image_provider_ && !display_key.empty())
>         ? owner_.image_provider_(display_key)
>         : nullptr;
> ```

- [ ] **Step 4: Update `ShellBase::ensure_row_media_` to pre-fetch thumbnail when available**

In `ui/shared/app/ShellBase.cpp`, in the `if (ev.type == EventType::Image)` branch (~line 377-382):

Current:
```cpp
    if (ev.type == EventType::Image)
    {
        const auto& img = static_cast<const ImageEvent&>(ev);
        ensure_media_image_(img.image_url, visual::kMaxInlineImageWidth,
                            visual::kMaxInlineImageHeight);
    }
```

Replace with:
```cpp
    if (ev.type == EventType::Image)
    {
        const auto& img = static_cast<const ImageEvent&>(ev);
        if (!img.thumbnail_url.empty())
        {
            // Pre-fetch the thumbnail for fast inline display.
            ensure_media_image_(img.thumbnail_url, visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight);
        }
        // Always pre-fetch the full image so ImageViewerOverlay opens instantly.
        ensure_media_image_(img.image_url, visual::kMaxInlineImageWidth,
                            visual::kMaxInlineImageHeight);
    }
```

In the `else if (ev.type == EventType::Sticker)` branch (~line 383-388):

Current:
```cpp
    else if (ev.type == EventType::Sticker)
    {
        const auto& s = static_cast<const StickerEvent&>(ev);
        ensure_media_image_(s.image_url, visual::kStickerSize,
                            visual::kStickerSize);
    }
```

Replace with:
```cpp
    else if (ev.type == EventType::Sticker)
    {
        const auto& s = static_cast<const StickerEvent&>(ev);
        if (!s.thumbnail_url.empty())
        {
            ensure_media_image_(s.thumbnail_url, visual::kStickerSize,
                                visual::kStickerSize);
        }
        ensure_media_image_(s.image_url, visual::kStickerSize,
                            visual::kStickerSize);
    }
```

- [ ] **Step 5: Build Qt6 target**

```bash
cmake --build build/linux-qt6-debug --target tesseract 2>&1 | tail -5
```

Expected: `Linking CXX executable ui/linux-qt/tesseract` with no errors.

- [ ] **Step 6: Build GTK4 target and run all C++ tests**

```bash
cmake --build build/linux-gtk-debug 2>&1 | tail -5
ctest --test-dir build/linux-gtk-debug --output-on-failure 2>&1 | tail -5
```

Expected: `Linking CXX executable tests/tesseract_tests`, then `100% tests passed`.

- [ ] **Step 7: Run Rust tests**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```bash
git add ui/shared/views/MessageListView.h ui/shared/views/MessageListView.cpp ui/shared/app/ShellBase.cpp
git commit -m "feat(ui): use thumbnail_url for inline image/sticker display in MessageListView"
```

---

## Self-Review

**Spec coverage:**
- ✅ `image_thumbnail_json` field in `TimelineEvent` (Task 1)
- ✅ Extraction for `m.image` (Task 1 Steps 5-6)
- ✅ Extraction for `m.sticker` (Task 1 Step 4 sticker block)
- ✅ `thumbnail_url` on `ImageEvent` and `StickerEvent` (Task 2)
- ✅ FFI bridge populates it (Task 2 Steps 5-6)
- ✅ `MessageRowData::thumbnail_url` (Task 3 Step 1)
- ✅ `make_row_data` populates it (Task 3 Step 2)
- ✅ Inline display uses thumbnail key (Task 3 Step 3)
- ✅ ShellBase pre-fetches thumbnail (Task 3 Step 4)
- ✅ Full-resolution image still pre-fetched for viewer (Task 3 Step 4 comments)
- ✅ `ImageHit::media_url` stays as full URL — no change needed (`ImageHit` reads `m.media_url`, not `m.thumbnail_url`)

**Type consistency:**
- `image_thumbnail_json` (Rust) → `e.image_thumbnail_json` (FFI) → `ev->thumbnail_url` (C++ ImageEvent/StickerEvent) → `row.thumbnail_url` (MessageRowData) → `display_key` in paint. Consistent throughout.
- `media_url` is never overwritten — it always holds the full-resolution source.
