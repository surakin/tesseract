# Room-List Last-Message Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the room-list last-message preview use `formatted_body`'s first plain line for text, render "<user> sent an image/video/file/voice message" for media, and draw a small inline sticker thumbnail for sticker last-messages.

**Architecture:** The SDK pre-renders the preview today as one plain `last_message_body` string with no type info. We replace `latest_event_body` with `latest_event_preview` returning `(kind, text, sticker_url)`, add two FFI fields (`last_message_kind`, `last_message_sticker_url`) crossing the cxx bridge into the C++ `RoomInfo`, and make `RoomListView::paint_room` compose the preview per-kind — drawing a ~28 px sticker thumbnail via a new `sticker_provider_` lambda (mirroring the existing `avatar_provider_`) backed by the shells' shared image cache.

**Tech Stack:** Rust (matrix-sdk `LatestEventValue`, ruma events), cxx FFI, C++ (`tesseract_tk`, Catch2), 4 native shells (Qt6/GTK4 build-verified here; Win32/macOS parity-only on Linux).

---

## Background (verified current state)

- `sdk/src/client.rs:4558` `latest_event_body(&LatestEventValue) -> Option<String>` — only matches `AnySyncMessageLikeEvent::RoomMessage`; uses **plain** body; `m.sticker`/`m.notice`/`m.emote` → `None`.
- `sdk/src/client.rs:4598` `latest_event_sender` — RoomMessage only (no Sticker).
- `sdk/src/client.rs:4618` `extract_local_body` — local-echo RoomMessage only.
- `sdk/src/client.rs:4686-4717` `build_room_infos` — `last_message_sender_name` is gated on `!last_message_body.is_empty()`.
- Sticker mxc extraction pattern (handler `sdk/src/client.rs:857-868`): `ev.content.source` is `StickerMediaSource::{Plain(OwnedMxcUri) | Encrypted(Box<EncryptedFile>)}`.
- FFI `RoomInfo`: `sdk/src/bridge.rs:14-35`, test mirror `sdk/src/lib.rs:20-33`, C++ `client/include/tesseract/types.h:271-288`, conversion `client/src/ffi_convert.h:83-99`.
- `RoomListView`: `AvatarProvider = std::function<const tk::Image*(const std::string&)>` (`RoomListView.h:35-36`), `set_avatar_provider` (`:52`), member `avatar_provider_` (`:146`). `paint_room` (`RoomListView.cpp:212-331`); `has_preview = !room.last_message_body.empty()`; preview built as `is_direct ? body : (sender|"You")+": "+body`; constants `kRowH=48 kAvatarSize=36 kPadX=6 kPadY=4 kAvatarGap=12`.
- Canvas: `virtual void draw_image(const Image&, Rect dst) = 0;` (`ui/shared/tk/canvas.h:215`).
- Per-shell avatar wiring: Qt6/GTK4/Win32 `room_list_view_->set_avatar_provider([this]{ tk_avatars_.find ... })`; macOS `_mainApp->room_list_view()->set_avatar_provider([weakSelf]{ s->_shell->tk_avatars_.find ... })`.
- Repaint on media bytes: Qt6 `mainAppSurface_->update()`, GTK4/Win32 `main_app_surface_->relayout()` (single main surface hosts the room list — already repaints). **macOS is different**: `MediaImage` path calls `[c _relayoutChatSurface]` only; the room list is a **separate** surface relayouted via `[c _relayoutRoomSurface]` (RoomAvatar path). macOS Task must add `[c _relayoutRoomSurface]` to the `MediaImage` branch.
- `ShellBase::ensure_media_image_(const std::string& url, int max_w, int max_h)` (`ui/shared/app/ShellBase.h`). Provider pattern: `anim_cache_.current_frame(k)` → `tk_images_.find(k)` → `ensure_media_image_(k, w, h)` → `nullptr`.

---

## File Inventory

| File | Change |
|------|--------|
| `sdk/src/bridge.rs` | +2 `RoomInfo` fields (`last_message_kind`, `last_message_sticker_url`) |
| `sdk/src/lib.rs` | mirror the +2 fields in `#[cfg(test)] mod ffi::RoomInfo` |
| `sdk/src/client.rs` | `LatestPreview` struct; `first_line`/`html_first_line` helpers; `latest_event_preview` (replaces `latest_event_body`); `latest_event_sender` +Sticker; `extract_local_preview` (replaces `extract_local_body`); `build_room_infos` sets new fields + ungated sender; update + extend `tests_latest_event_body` |
| `client/include/tesseract/types.h` | +2 `RoomInfo` fields |
| `client/src/ffi_convert.h` | copy +2 fields in `from_ffi(RoomInfo)` |
| `ui/shared/views/RoomListView.h` | `StickerProvider` alias, `set_sticker_provider`, `sticker_provider_` member |
| `ui/shared/views/RoomListView.cpp` | kind-driven preview compose + ~28 px sticker thumb |
| `ui/linux-qt/src/MainWindow.cpp` | wire `set_sticker_provider` |
| `ui/linux-gtk/src/MainWindow.cpp` | wire `set_sticker_provider` |
| `ui/windows/src/MainWindow.cpp` | wire `set_sticker_provider` (parity-only build) |
| `ui/macos/src/MainWindowController.mm` | wire `set_sticker_provider` + add `_relayoutRoomSurface` to `MediaImage` path (parity-only build) |
| `CHANGES.md`, `STATUS.md` | document |

---

## Task 1: SDK — `latest_event_preview` + FFI fields + tests

**Files:**
- Modify: `sdk/src/bridge.rs` (RoomInfo struct ~14-35)
- Modify: `sdk/src/lib.rs` (`#[cfg(test)] mod ffi` RoomInfo ~20-33)
- Modify: `sdk/src/client.rs` (4558-4730 region + tests 6528-6677)

- [ ] **Step 1: Add the 2 FFI fields to `sdk/src/bridge.rs`**

In `sdk/src/bridge.rs`, inside `struct RoomInfo { ... }`, immediately after the `last_message_sender_name: String,` field (and its doc comment), add:

```rust
        /// Kind of the latest event for preview rendering:
        /// "text" | "image" | "video" | "file" | "audio" | "sticker" | "".
        last_message_kind: String,
        /// mxc:// URI of the sticker image when last_message_kind == "sticker";
        /// empty otherwise.
        last_message_sticker_url: String,
```

- [ ] **Step 2: Mirror the 2 fields in the test FFI stub `sdk/src/lib.rs`**

In `sdk/src/lib.rs`, inside `#[cfg(test)] pub mod ffi { pub struct RoomInfo { ... } }`, after `pub last_message_sender_name: String,`, add:

```rust
        pub last_message_kind: String,
        pub last_message_sticker_url: String,
```

- [ ] **Step 3: Add the `LatestPreview` struct + line/HTML helpers in `sdk/src/client.rs`**

In `sdk/src/client.rs`, immediately **before** `fn latest_event_body(` (line 4558), insert:

```rust
/// Preview of a room's latest event for the room-list sidebar.
/// `kind`: "text" | "image" | "video" | "file" | "audio" | "sticker" | ""
/// (empty = nothing to preview). `text` is the first plain line (text-like
/// kinds only). `sticker_url` is the mxc URI for "sticker".
#[derive(Debug, Default, PartialEq)]
struct LatestPreview {
    kind: String,
    text: String,
    sticker_url: String,
}

/// First non-empty line of `s`, trimmed. Splits on the first '\n'.
fn first_line(s: &str) -> String {
    s.lines()
        .map(str::trim)
        .find(|l| !l.is_empty())
        .unwrap_or("")
        .to_owned()
}

/// Strip a Matrix `formatted_body` (HTML subset) to its first plain-text
/// line. Block/break tags become newlines; all other tags are dropped;
/// the handful of entities Matrix uses are decoded. No external deps.
fn html_first_line(html: &str) -> String {
    let mut text = String::with_capacity(html.len());
    let bytes = html.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'<' {
            // Read the tag up to '>'.
            let start = i + 1;
            let mut j = start;
            while j < bytes.len() && bytes[j] != b'>' {
                j += 1;
            }
            let tag = html[start..j.min(html.len())].to_ascii_lowercase();
            let name: String = tag
                .trim_start_matches('/')
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric())
                .collect();
            // Tags that introduce a visual line break.
            let breaks = matches!(
                name.as_str(),
                "br" | "p" | "div" | "li" | "tr" | "blockquote"
                    | "h1" | "h2" | "h3" | "h4" | "h5" | "h6"
            );
            if breaks {
                text.push('\n');
            }
            i = j + 1;
        } else {
            text.push(bytes[i] as char);
            i += 1;
        }
    }
    let decoded = text
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&#39;", "'")
        .replace("&nbsp;", " ")
        .replace("&amp;", "&");
    first_line(&decoded)
}
```

(`&amp;` is replaced **last** so e.g. `&amp;lt;` does not become `<`.)

- [ ] **Step 4: Replace `latest_event_body` with `latest_event_preview`**

In `sdk/src/client.rs`, replace the entire `fn latest_event_body(...) -> Option<String> { ... }` (4558-4596) with:

```rust
fn latest_event_preview(value: &matrix_sdk::latest_events::LatestEventValue) -> LatestPreview {
    use matrix_sdk::latest_events::LatestEventValue;
    use matrix_sdk::ruma::events::{
        room::message::MessageType, sticker::StickerMediaSource,
        AnySyncMessageLikeEvent, AnySyncTimelineEvent,
    };

    let text_kind = |body: &str, formatted: Option<&str>| -> LatestPreview {
        let line = match formatted {
            Some(html) if !html.trim().is_empty() => html_first_line(html),
            _ => String::new(),
        };
        let line = if line.is_empty() {
            first_line(body)
        } else {
            line
        };
        if line.is_empty() {
            LatestPreview::default()
        } else {
            LatestPreview {
                kind: "text".to_owned(),
                text: line,
                sticker_url: String::new(),
            }
        }
    };
    let media_kind = |k: &str| LatestPreview {
        kind: k.to_owned(),
        text: String::new(),
        sticker_url: String::new(),
    };

    match value {
        LatestEventValue::Remote(timeline_event) => {
            let Some(event) = timeline_event.raw().deserialize().ok() else {
                return LatestPreview::default();
            };
            match event {
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(ev)) => {
                    let Some(orig) = ev.as_original() else {
                        return LatestPreview::default();
                    };
                    match &orig.content.msgtype {
                        MessageType::Text(t) => text_kind(
                            &t.body,
                            t.formatted.as_ref().map(|f| f.body.as_str()),
                        ),
                        MessageType::Notice(n) => text_kind(
                            &n.body,
                            n.formatted.as_ref().map(|f| f.body.as_str()),
                        ),
                        MessageType::Emote(e) => text_kind(
                            &e.body,
                            e.formatted.as_ref().map(|f| f.body.as_str()),
                        ),
                        MessageType::Image(_) => media_kind("image"),
                        MessageType::Video(_) => media_kind("video"),
                        MessageType::File(_) => media_kind("file"),
                        MessageType::Audio(_) => media_kind("audio"),
                        _ => LatestPreview::default(),
                    }
                }
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(ev)) => {
                    let Some(orig) = ev.as_original() else {
                        return LatestPreview::default();
                    };
                    let url = match &orig.content.source {
                        StickerMediaSource::Plain(uri) => uri.to_string(),
                        StickerMediaSource::Encrypted(f) => f.url.to_string(),
                        _ => String::new(),
                    };
                    LatestPreview {
                        kind: "sticker".to_owned(),
                        text: String::new(),
                        sticker_url: url,
                    }
                }
                _ => LatestPreview::default(),
            }
        }
        LatestEventValue::LocalIsSending(local)
        | LatestEventValue::LocalCannotBeSent(local) => {
            extract_local_preview(&local.content)
        }
        LatestEventValue::LocalHasBeenSent { value: local, .. } => {
            extract_local_preview(&local.content)
        }
        LatestEventValue::None | LatestEventValue::RemoteInvite { .. } => {
            LatestPreview::default()
        }
    }
}
```

- [ ] **Step 5: Extend `latest_event_sender` for stickers**

In `sdk/src/client.rs` `latest_event_sender` (4604-4615), replace the inner `match event { ... }` so the Sticker variant also yields a sender:

```rust
        LatestEventValue::Remote(timeline_event) => {
            let event = timeline_event.raw().deserialize().ok()?;
            match event {
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::RoomMessage(ev)) => {
                    ev.as_original().map(|e| e.sender.clone())
                }
                AnySyncTimelineEvent::MessageLike(AnySyncMessageLikeEvent::Sticker(ev)) => {
                    ev.as_original().map(|e| e.sender.clone())
                }
                _ => None,
            }
        }
```

- [ ] **Step 6: Replace `extract_local_body` with `extract_local_preview`**

Replace the entire `fn extract_local_body(...) -> Option<String> { ... }` (4618-4637) with:

```rust
fn extract_local_preview(
    content: &matrix_sdk::store::SerializableEventContent,
) -> LatestPreview {
    use matrix_sdk::ruma::events::{
        room::message::MessageType, AnyMessageLikeEventContent,
    };
    let Some(c) = content.deserialize().ok() else {
        return LatestPreview::default();
    };
    let msgtype = match c {
        AnyMessageLikeEventContent::RoomMessage(c) => c.msgtype,
        _ => return LatestPreview::default(),
    };
    let text_kind = |body: &str, formatted: Option<&str>| -> LatestPreview {
        let line = match formatted {
            Some(html) if !html.trim().is_empty() => html_first_line(html),
            _ => String::new(),
        };
        let line = if line.is_empty() {
            first_line(body)
        } else {
            line
        };
        if line.is_empty() {
            LatestPreview::default()
        } else {
            LatestPreview {
                kind: "text".to_owned(),
                text: line,
                sticker_url: String::new(),
            }
        }
    };
    let media_kind = |k: &str| LatestPreview {
        kind: k.to_owned(),
        text: String::new(),
        sticker_url: String::new(),
    };
    match msgtype {
        MessageType::Text(t) => {
            text_kind(&t.body, t.formatted.as_ref().map(|f| f.body.as_str()))
        }
        MessageType::Notice(n) => {
            text_kind(&n.body, n.formatted.as_ref().map(|f| f.body.as_str()))
        }
        MessageType::Emote(e) => {
            text_kind(&e.body, e.formatted.as_ref().map(|f| f.body.as_str()))
        }
        MessageType::Image(_) => media_kind("image"),
        MessageType::Video(_) => media_kind("video"),
        MessageType::File(_) => media_kind("file"),
        MessageType::Audio(_) => media_kind("audio"),
        _ => LatestPreview::default(),
    }
}
```

- [ ] **Step 7: Update `build_room_infos` to set the new fields + ungate sender**

In `sdk/src/client.rs` `build_room_infos`, replace the block from `let lev = ...` through the `result.push(...)` (4686-4717) with:

```rust
        let lev = std::ops::Deref::deref(&room).latest_event();
        let preview = latest_event_preview(&lev);
        let last_message_kind = preview.kind.clone();
        let last_message_sticker_url = preview.sticker_url.clone();
        let last_message_body = preview.text.clone();
        let last_message_sender_name = if preview.kind.is_empty() {
            String::new()
        } else {
            match latest_event_sender(&lev) {
                Some(sender) if client.user_id().is_some_and(|me| me != &*sender) => {
                    match room.get_member_no_sync(&sender).await {
                        Ok(Some(m)) => m
                            .display_name()
                            .map(str::to_owned)
                            .unwrap_or_else(|| sender.localpart().to_string()),
                        _ => sender.localpart().to_string(),
                    }
                }
                _ => String::new(),
            }
        };
        result.push(crate::ffi::RoomInfo {
            id: room.room_id().to_string(),
            name,
            topic: room.topic().unwrap_or_default(),
            topic_html,
            unread_count,
            is_direct: room.is_direct().await.unwrap_or(false),
            avatar_url: room.avatar_url().map(|u| u.to_string()).unwrap_or_default(),
            last_message_body,
            last_message_sender_name,
            last_message_kind,
            last_message_sticker_url,
            last_activity_ts,
            is_space,
            is_favorite,
        });
```

- [ ] **Step 8: Update + extend the tests**

In `sdk/src/client.rs`, the test module is `mod tests_latest_event_body` (6528). Rename its `use super::latest_event_body;` to `use super::{latest_event_preview, LatestPreview};` and rewrite each existing assertion from `assert_eq!(latest_event_body(&v), Some("x".into()))` / `None` to compare a `LatestPreview`. Replace the **entire** `#[cfg(test)] mod tests_latest_event_body { ... }` body with:

```rust
#[cfg(test)]
mod tests_latest_event_body {
    use super::{latest_event_preview, LatestPreview};
    use matrix_sdk::latest_events::{
        LatestEventValue, LocalLatestEventValue, RemoteLatestEventValue,
    };
    use matrix_sdk::ruma::events::{
        room::message::RoomMessageEventContent, AnyMessageLikeEventContent,
    };
    use matrix_sdk::ruma::{events::AnySyncTimelineEvent, serde::Raw, MilliSecondsSinceUnixEpoch};
    use matrix_sdk::store::SerializableEventContent;

    fn remote(json: serde_json::Value) -> LatestEventValue {
        let raw = Raw::<AnySyncTimelineEvent>::from_json_string(json.to_string()).unwrap();
        LatestEventValue::Remote(RemoteLatestEventValue::from_plaintext(raw))
    }
    fn text(t: &str) -> LatestPreview {
        LatestPreview { kind: "text".into(), text: t.into(), sticker_url: String::new() }
    }
    fn media(k: &str) -> LatestPreview {
        LatestPreview { kind: k.into(), text: String::new(), sticker_url: String::new() }
    }

    #[test]
    fn none_returns_default() {
        assert_eq!(latest_event_preview(&LatestEventValue::None), LatestPreview::default());
    }

    #[test]
    fn remote_invite_returns_default() {
        let v = LatestEventValue::RemoteInvite {
            event_id: None,
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            inviter: None,
        };
        assert_eq!(latest_event_preview(&v), LatestPreview::default());
    }

    #[test]
    fn remote_text_plain() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": { "msgtype": "m.text", "body": "hello world" }
        }));
        assert_eq!(latest_event_preview(&v), text("hello world"));
    }

    #[test]
    fn remote_text_prefers_formatted_first_line() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.text",
                "body": "plain line one\nplain line two",
                "format": "org.matrix.custom.html",
                "formatted_body": "<p><b>bold</b> first</p><p>second para</p>"
            }
        }));
        assert_eq!(latest_event_preview(&v), text("bold first"));
    }

    #[test]
    fn remote_text_html_entities_decoded() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "msgtype": "m.text", "body": "x",
                "format": "org.matrix.custom.html",
                "formatted_body": "a &amp; b &lt;c&gt;<br>next"
            }
        }));
        assert_eq!(latest_event_preview(&v), text("a & b <c>"));
    }

    #[test]
    fn remote_notice_and_emote_are_text() {
        for mt in ["m.notice", "m.emote"] {
            let v = remote(serde_json::json!({
                "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
                "sender": "@a:e.com", "origin_server_ts": 1,
                "content": { "msgtype": mt, "body": "notice/emote body" }
            }));
            assert_eq!(latest_event_preview(&v), text("notice/emote body"));
        }
    }

    #[test]
    fn remote_image_video_file_audio_kinds() {
        let cases = [
            ("m.image", "image", "url"),
            ("m.video", "video", "url"),
            ("m.file", "file", "url"),
            ("m.audio", "audio", "url"),
        ];
        for (mt, kind, _) in cases {
            let v = remote(serde_json::json!({
                "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
                "sender": "@a:e.com", "origin_server_ts": 1,
                "content": { "msgtype": mt, "body": "f.bin", "url": "mxc://e.com/x" }
            }));
            assert_eq!(latest_event_preview(&v), media(kind));
        }
    }

    #[test]
    fn remote_sticker_kind_and_url() {
        let v = remote(serde_json::json!({
            "type": "m.sticker", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": {
                "body": "wave",
                "url": "mxc://e.com/stick1",
                "info": { "w": 128, "h": 128, "mimetype": "image/png" }
            }
        }));
        assert_eq!(
            latest_event_preview(&v),
            LatestPreview {
                kind: "sticker".into(),
                text: String::new(),
                sticker_url: "mxc://e.com/stick1".into(),
            }
        );
    }

    #[test]
    fn remote_state_event_returns_default() {
        let v = remote(serde_json::json!({
            "type": "m.room.member", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "state_key": "@a:e.com", "origin_server_ts": 1,
            "content": { "membership": "join" }
        }));
        assert_eq!(latest_event_preview(&v), LatestPreview::default());
    }

    #[test]
    fn remote_empty_body_returns_default() {
        let v = remote(serde_json::json!({
            "type": "m.room.message", "event_id": "$e", "room_id": "!r:e.com",
            "sender": "@a:e.com", "origin_server_ts": 1,
            "content": { "msgtype": "m.text", "body": "   " }
        }));
        assert_eq!(latest_event_preview(&v), LatestPreview::default());
    }

    #[test]
    fn local_is_sending_text() {
        let content = SerializableEventContent::new(&AnyMessageLikeEventContent::RoomMessage(
            RoomMessageEventContent::text_plain("sending\u{2026}"),
        ))
        .unwrap();
        let v = LatestEventValue::LocalIsSending(LocalLatestEventValue {
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            content,
        });
        assert_eq!(latest_event_preview(&v), text("sending\u{2026}"));
    }

    #[test]
    fn local_cannot_be_sent_text() {
        let content = SerializableEventContent::new(&AnyMessageLikeEventContent::RoomMessage(
            RoomMessageEventContent::text_plain("stuck message"),
        ))
        .unwrap();
        let v = LatestEventValue::LocalCannotBeSent(LocalLatestEventValue {
            timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
            content,
        });
        assert_eq!(latest_event_preview(&v), text("stuck message"));
    }

    #[test]
    fn local_has_been_sent_text() {
        use matrix_sdk::ruma::owned_event_id;
        let content = SerializableEventContent::new(&AnyMessageLikeEventContent::RoomMessage(
            RoomMessageEventContent::text_plain("sent!"),
        ))
        .unwrap();
        let v = LatestEventValue::LocalHasBeenSent {
            event_id: owned_event_id!("$ev_sent"),
            value: LocalLatestEventValue {
                timestamp: MilliSecondsSinceUnixEpoch(ruma::uint!(0)),
                content,
            },
        };
        assert_eq!(latest_event_preview(&v), text("sent!"));
    }
}
```

- [ ] **Step 9: Build + test the sdk crate**

```bash
cd sdk && cargo test -p tesseract-sdk-ffi 2>&1 | tail -8
```
Expected: compiles; all tests pass (the existing 87 plus the new/rewritten `tests_latest_event_body` cases). If `StickerMediaSource` variants differ in this matrix-sdk version, match the exact variants (the production handler at `client.rs:857` uses `StickerMediaSource::Plain(uri)` / `StickerMediaSource::Encrypted(f)` with `f.url` — mirror it). Run `cargo fmt` in `sdk/` (repo-wide rustfmt is enforced) before committing.

- [ ] **Step 10: Commit**

```bash
cd /home/rayden/src/tesseract
git add sdk/src/bridge.rs sdk/src/lib.rs sdk/src/client.rs
git commit -m "feat(sdk): latest_event_preview — kind + first-line + sticker url for room list"
```

---

## Task 2: C++ FFI — propagate the 2 new RoomInfo fields

**Files:**
- Modify: `client/include/tesseract/types.h:271-288`
- Modify: `client/src/ffi_convert.h:83-99`

- [ ] **Step 1: Add the 2 fields to the C++ `RoomInfo`**

In `client/include/tesseract/types.h`, in `struct RoomInfo`, immediately after the `last_message_sender_name` field (and its doc comment, ending line 282), add:

```cpp
    /// Kind of the latest event for preview rendering: "text" | "image" |
    /// "video" | "file" | "audio" | "sticker" | "" (nothing to preview).
    std::string last_message_kind;
    /// mxc:// URI of the sticker image when last_message_kind == "sticker".
    std::string last_message_sticker_url;
```

- [ ] **Step 2: Copy the 2 fields in `from_ffi`**

In `client/src/ffi_convert.h`, in `inline RoomInfo from_ffi(const tesseract_ffi::RoomInfo& r)`, add after `.last_message_sender_name = std::string(r.last_message_sender_name),`:

```cpp
        .last_message_kind = std::string(r.last_message_kind),
        .last_message_sticker_url = std::string(r.last_message_sticker_url),
```

(Designated initializers must follow declaration order — `types.h` declares `last_message_kind`/`last_message_sticker_url` right after `last_message_sender_name` and before `last_activity_ts`, so place these two lines accordingly in the `from_ffi` return list.)

- [ ] **Step 3: Build the client lib**

```bash
cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep -E "error:" | head
```
Expected: no errors. Run `clang-format -i client/include/tesseract/types.h client/src/ffi_convert.h` (repo-wide clang-format enforced).

- [ ] **Step 4: Commit**

```bash
git add client/include/tesseract/types.h client/src/ffi_convert.h
git commit -m "feat(client): propagate last_message_kind + last_message_sticker_url"
```

---

## Task 3: RoomListView — sticker provider + kind-driven preview

**Files:**
- Modify: `ui/shared/views/RoomListView.h`
- Modify: `ui/shared/views/RoomListView.cpp` (`paint_room`, 212-331)

- [ ] **Step 1: Add the sticker provider to `RoomListView.h`**

In `ui/shared/views/RoomListView.h`, immediately after the `AvatarProvider` alias (lines 35-36):

```cpp
    using StickerProvider =
        std::function<const tk::Image*(const std::string& mxc_url)>;
```

After the `void set_avatar_provider(AvatarProvider p);` declaration (line 52):

```cpp
    void set_sticker_provider(StickerProvider p);
```

After the `AvatarProvider avatar_provider_;` member (line 146):

```cpp
    StickerProvider sticker_provider_;
```

- [ ] **Step 2: Define `set_sticker_provider` in `RoomListView.cpp`**

Find the existing `void RoomListView::set_avatar_provider(AvatarProvider p)` definition in `ui/shared/views/RoomListView.cpp` (it assigns `avatar_provider_ = std::move(p);` and likely calls `list_->invalidate_data();`). Immediately after that function, add an identical one:

```cpp
void RoomListView::set_sticker_provider(StickerProvider p)
{
    sticker_provider_ = std::move(p);
    if (list_)
    {
        list_->invalidate_data();
    }
}
```

(Match the exact body of `set_avatar_provider` — if it does not guard `list_` or uses a different invalidate call, mirror it precisely.)

- [ ] **Step 3: Replace the preview block in `paint_room` with kind-driven rendering**

In `ui/shared/views/RoomListView.cpp` `paint_room`, replace the line:

```cpp
    bool has_preview = !room.last_message_body.empty();
```

with:

```cpp
    bool has_preview = !room.last_message_kind.empty();
```

Then replace the **entire** `if (!room.last_message_body.empty()) { ... }` preview block (the one building `prev_style`/`preview` and calling `draw_text`) with:

```cpp
    if (has_preview)
    {
        const std::string& kind = room.last_message_kind;
        const std::string sender =
            room.last_message_sender_name.empty()
                ? std::string("You")
                : room.last_message_sender_name;

        // Sticker: draw a small thumbnail; fall back to text until it loads.
        if (kind == "sticker")
        {
            const tk::Image* thumb = nullptr;
            if (owner_.sticker_provider_ &&
                !room.last_message_sticker_url.empty())
            {
                thumb = owner_.sticker_provider_(
                    room.last_message_sticker_url);
            }
            if (thumb)
            {
                constexpr float kThumb = 28.0f;
                tk::Rect dst{text_x,
                             bounds.y + bounds.h - kPadY - kThumb, kThumb,
                             kThumb};
                ctx.canvas.draw_image(*thumb, dst);
                if (!badge_text.empty())
                {
                    // (badge drawn below as before)
                }
                return paint_badge(room, ctx, bounds, badge_text);
            }
        }

        std::string preview;
        if (kind == "text")
        {
            preview = room.last_message_body;
            if (!room.is_direct)
            {
                preview = sender + ": " + preview;
            }
        }
        else if (kind == "image")
        {
            preview = sender + " sent an image";
        }
        else if (kind == "video")
        {
            preview = sender + " sent a video";
        }
        else if (kind == "file")
        {
            preview = sender + " sent a file";
        }
        else if (kind == "audio")
        {
            preview = sender + " sent a voice message";
        }
        else if (kind == "sticker")
        {
            preview = sender + " sent a sticker";
        }

        if (!preview.empty())
        {
            tk::TextStyle prev_style{};
            prev_style.role = tk::FontRole::SidebarPreview;
            prev_style.trim = tk::TextTrim::Ellipsis;
            prev_style.max_width = text_w;
            auto prev_layout = ctx.factory.build_text(preview, prev_style);
            if (prev_layout)
            {
                float prev_y = bounds.y + bounds.h - kPadY -
                               prev_layout->measure().h;
                ctx.canvas.draw_text(*prev_layout, {text_x, prev_y},
                                     ctx.theme.palette.text_secondary);
            }
        }
    }
```

**Important — do NOT introduce an undefined `paint_badge` helper.** The above references it only to illustrate "draw the badge then return". Instead, implement the early-return for the sticker-thumb case without a new helper: the badge block already exists later in `paint_room` (the `if (!badge_text.empty()) { ... }`). The cleanest behavior-preserving structure is: when a sticker thumb is drawn, **skip the text preview only** (still fall through to the existing badge block). So replace the sticker-thumb branch body with a flag instead of an early return:

```cpp
        bool sticker_thumb_drawn = false;
        if (kind == "sticker")
        {
            const tk::Image* thumb = nullptr;
            if (owner_.sticker_provider_ &&
                !room.last_message_sticker_url.empty())
            {
                thumb = owner_.sticker_provider_(
                    room.last_message_sticker_url);
            }
            if (thumb)
            {
                constexpr float kThumb = 28.0f;
                tk::Rect dst{text_x,
                             bounds.y + bounds.h - kPadY - kThumb, kThumb,
                             kThumb};
                ctx.canvas.draw_image(*thumb, dst);
                sticker_thumb_drawn = true;
            }
        }

        std::string preview;
        if (!sticker_thumb_drawn)
        {
            if (kind == "text")
            {
                preview = room.last_message_body;
                if (!room.is_direct)
                {
                    preview = sender + ": " + preview;
                }
            }
            else if (kind == "image")
            {
                preview = sender + " sent an image";
            }
            else if (kind == "video")
            {
                preview = sender + " sent a video";
            }
            else if (kind == "file")
            {
                preview = sender + " sent a file";
            }
            else if (kind == "audio")
            {
                preview = sender + " sent a voice message";
            }
            else if (kind == "sticker")
            {
                preview = sender + " sent a sticker";
            }
        }

        if (!preview.empty())
        {
            tk::TextStyle prev_style{};
            prev_style.role = tk::FontRole::SidebarPreview;
            prev_style.trim = tk::TextTrim::Ellipsis;
            prev_style.max_width = text_w;
            auto prev_layout = ctx.factory.build_text(preview, prev_style);
            if (prev_layout)
            {
                float prev_y = bounds.y + bounds.h - kPadY -
                               prev_layout->measure().h;
                ctx.canvas.draw_text(*prev_layout, {text_x, prev_y},
                                     ctx.theme.palette.text_secondary);
            }
        }
    }
```

Use **only** this second (flag-based, no-helper) form. The existing unread-badge block after this is unchanged and still runs for all rooms.

- [ ] **Step 4: Build the toolkit**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head
```
Expected: no errors. `clang-format -i ui/shared/views/RoomListView.h ui/shared/views/RoomListView.cpp`.

- [ ] **Step 5: Commit**

```bash
git add ui/shared/views/RoomListView.h ui/shared/views/RoomListView.cpp
git commit -m "feat(ui): kind-driven room-list preview + inline sticker thumbnail"
```

---

## Task 4: Wire `set_sticker_provider` in Qt6 + GTK4 (build-verified)

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.cpp`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`

- [ ] **Step 1: Qt6 — add the sticker provider next to the avatar provider**

In `ui/linux-qt/src/MainWindow.cpp`, locate the existing `…->set_avatar_provider([this](const std::string& mxc) -> const tk::Image* { auto it = tk_avatars_.find(mxc); return it == tk_avatars_.end() ? nullptr : it->second.get(); });` call on the room list view. Immediately after it, add (using the same receiver expression the avatar call used — `mainApp_->room_list_view()` or `room_list_view_`, match the existing code exactly):

```cpp
    mainApp_->room_list_view()->set_sticker_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            if (const auto* f = anim_cache_.current_frame(mxc))
            {
                return f;
            }
            auto it = tk_images_.find(mxc);
            if (it != tk_images_.end())
            {
                return it->second.get();
            }
            ensure_media_image_(mxc, 64, 64);
            return nullptr;
        });
```

- [ ] **Step 2: GTK4 — same wiring**

In `ui/linux-gtk/src/MainWindow.cpp`, after the room list view `set_avatar_provider(...)` call, add (match the existing receiver, `room_list_view_`):

```cpp
    room_list_view_->set_sticker_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            if (const auto* f = anim_cache_.current_frame(mxc))
            {
                return f;
            }
            auto it = tk_images_.find(mxc);
            if (it != tk_images_.end())
            {
                return it->second.get();
            }
            ensure_media_image_(mxc, 64, 64);
            return nullptr;
        });
```

(No repaint change needed: Qt6 `mainAppSurface_->update()` and GTK4 `main_app_surface_->relayout()` in the `MediaImage` branch of `on_media_bytes_ready_` already repaint the single main surface that hosts the room list.)

- [ ] **Step 3: Build + full test (Qt6) and GTK4 build**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:" | head
ctest --test-dir build/linux-qt6-debug 2>&1 | grep -E "% tests passed|failed out of"
cmake --build build/linux-gtk-debug --target tesseract 2>&1 | grep -E "error:" | head
```
Expected: no errors; **323/323 tests pass** (no new C++ tests; the SDK tests from Task 1 cover the data path; this verifies no regression). GTK4 clean. Run `clang-format -i` on both modified files.

- [ ] **Step 4: Commit**

```bash
git add ui/linux-qt/src/MainWindow.cpp ui/linux-gtk/src/MainWindow.cpp
git commit -m "feat(ui/qt6,gtk4): wire room-list sticker provider"
```

---

## Task 5: Wire Win32 + macOS (parity-only on Linux)

**Files:**
- Modify: `ui/windows/src/MainWindow.cpp`
- Modify: `ui/macos/src/MainWindowController.mm`

These targets cannot be built on this Linux host (Win32 is CMake-hard-gated; macOS needs Xcode). Implement by mirroring Task 4 + the verified avatar wiring, then parity-review the diff against Qt6/GTK4.

- [ ] **Step 1: Win32 — sticker provider**

In `ui/windows/src/MainWindow.cpp`, after the room list `set_avatar_provider(...)` call (`room_list_view_->set_avatar_provider([this]{ tk_avatars_.find ... })`), add:

```cpp
    room_list_view_->set_sticker_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            if (const auto* f = anim_cache_.current_frame(mxc))
            {
                return f;
            }
            auto it = tk_images_.find(mxc);
            if (it != tk_images_.end())
            {
                return it->second.get();
            }
            ensure_media_image_(mxc, 64, 64);
            return nullptr;
        });
```

Win32's `on_media_bytes_ready_` `MediaImage` branch already calls `main_app_surface_->relayout()` + `InvalidateRect(...)` on the single main surface (hosts the room list) — no repaint change needed.

- [ ] **Step 2: macOS — sticker provider (weakSelf pattern)**

In `ui/macos/src/MainWindowController.mm`, after the `_mainApp->room_list_view()->set_avatar_provider([weakSelf]{ ... s->_shell->tk_avatars_.find ... });` call, add (mirror the avatar lambda's `weakSelf`/`_shell` access exactly):

```objc
    _mainApp->room_list_view()->set_sticker_provider(
        [weakSelf](const std::string& mxc) -> const tk::Image*
        {
            MainWindowController* s = weakSelf;
            if (!s)
            {
                return nullptr;
            }
            if (const auto* f = s->_shell->anim_cache_.current_frame(mxc))
            {
                return f;
            }
            auto it = s->_shell->tk_images_.find(mxc);
            if (it != s->_shell->tk_images_.end())
            {
                return it->second.get();
            }
            s->_shell->ensure_media_image_(mxc, 64, 64);
            return nullptr;
        });
```

(`anim_cache_`, `tk_images_`, `ensure_media_image_` are `ShellBase` members reached via `s->_shell->`; confirm `ensure_media_image_` is exposed to ObjC++ — it is a `protected` ShellBase method and `MacShell` already re-exposes `tk_images_`/`anim_cache_` via `using`; if `ensure_media_image_` is not yet in `MacShell`'s public `using` block, add `using ShellBase::ensure_media_image_;` there, mirroring how the picker work exposed `ensure_picker_image_`.)

- [ ] **Step 3: macOS — repaint the room-list surface when sticker bytes arrive**

macOS is the only shell whose room list is a **separate** surface. In `ui/macos/src/MainWindowController.mm`, in `MacShell::on_media_bytes_ready_`, the `MediaImage` branch currently is:

```cpp
    if (kind == MediaKind::MediaImage)
    {
        [c _decodeMediaBytes:bytes forKey:key];
        [c _relayoutChatSurface];
        [c _relayoutShortcodePopupIfVisible];
        return;
    }
```

Add a room-surface relayout so an async sticker thumbnail repaints the sidebar:

```cpp
    if (kind == MediaKind::MediaImage)
    {
        [c _decodeMediaBytes:bytes forKey:key];
        [c _relayoutChatSurface];
        [c _relayoutRoomSurface];
        [c _relayoutShortcodePopupIfVisible];
        return;
    }
```

(`_relayoutRoomSurface` already exists — it is used by the `RoomAvatar` branch.)

- [ ] **Step 4: Parity review (no Linux build possible)**

`clang-format -i ui/windows/src/MainWindow.cpp ui/macos/src/MainWindowController.mm`. Diff-review the two lambdas against the Qt6/GTK4 ones from Task 4 (same cache lookup order, correct `is_sticker`-free media provider, `ensure_media_image_(mxc,64,64)`), and confirm the macOS `_relayoutRoomSurface` addition mirrors the existing `RoomAvatar`-path usage. Re-run the Linux suite to prove shared code is untouched:

```bash
ctest --test-dir build/linux-qt6-debug 2>&1 | grep -E "% tests passed|failed out of"
```
Expected: 323/323 (these two files are Win32/macOS-only TUs, not compiled on Linux — confirms no contamination).

- [ ] **Step 5: Commit**

```bash
git add ui/windows/src/MainWindow.cpp ui/macos/src/MainWindowController.mm
git commit -m "feat(ui/win32,macos): wire room-list sticker provider (parity)"
```

---

## Task 6: Docs + final verification

**Files:**
- Modify: `CHANGES.md`, `STATUS.md`

- [ ] **Step 1: Full Linux verification on the merged result**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -2
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -1
ctest --test-dir build/linux-qt6-debug 2>&1 | grep -E "% tests passed|failed out of"
cmake --build build/linux-gtk-debug --target tesseract 2>&1 | grep -E "error:" | head
cd sdk && cargo test -p tesseract-sdk-ffi 2>&1 | tail -2 && cd ..
```
Expected: clean builds; ctest 323/323; cargo all-pass (87 + the new `tests_latest_event_body` cases).

- [ ] **Step 2: CHANGES.md**

Under the `## Unreleased` → `### 2026-05-18` section (it already exists from the picker-cache entry; add a new bullet beneath it), add:

```markdown
- feat(ui): room-list last-message preview now uses `formatted_body`'s first plain line for text/notice/emote; renders "<user> sent an image/video/file/voice message" for media; and draws a small inline ~28 px thumbnail for sticker last-messages (SDK exposes `last_message_kind` + `last_message_sticker_url`; `RoomListView` gains a `sticker_provider_` backed by the shared image cache, wired in all four shells)
```

- [ ] **Step 3: STATUS.md**

Bump `Last updated` to `2026-05-18`; refresh the C++/Rust test counts from the actual `ctest`/`cargo test` runs (C++ stays 323 — no new C++ tests; Rust gains the rewritten/added `tests_latest_event_body` cases — state the new total from the cargo run); add a one-line note in the room-list/sidebar area describing the kind-aware preview + sticker thumbnail.

- [ ] **Step 4: Commit**

```bash
git add CHANGES.md STATUS.md
git commit -m "docs: record room-list kind-aware preview + sticker thumbnail"
```

---

## Task 7: Final code-review pass

- [ ] **Step 1: Review the data path**

Read the Task 1 diff. Verify: `latest_event_preview` handles every `MessageType` arm + `Sticker` event; `html_first_line` decodes `&amp;` **last**; `first_line` skips leading blank lines and trims; `build_room_infos` sets all 3 fields and resolves the sender for **every** non-empty kind (image/sticker included), with `""` sender ⇒ C++ renders "You"; `latest_event_sender` covers Sticker; `extract_local_preview` parity with the remote path; all `tests_latest_event_body` cases pass and assert the full `LatestPreview`.

- [ ] **Step 2: Review the FFI + render path**

Verify the 2 fields exist in all four `RoomInfo` definitions (bridge.rs, lib.rs stub, types.h) with matching names and that `from_ffi` copies them in declaration order. In `RoomListView::paint_room`: `has_preview` is `!last_message_kind.empty()` (so the name top-aligns and the preview row shows for media too); the no-helper flag-based sticker branch is the one used (no undefined `paint_badge`); the unread-badge block still runs unconditionally; "You"/sender resolution matches the approved design ("You sent an image" for self, no `Name:` prefix for media kinds, `Name: text` retained for group text, bare text for DM text); the ~28 px thumb is bottom-aligned within the row and `draw_image` is used (not `draw_circle_image`).

- [ ] **Step 3: Review the shell wiring**

All four shells call `set_sticker_provider` with the cache-lookup-then-`ensure_media_image_` pattern matching their `set_avatar_provider` style; macOS additionally relayouts the room surface on `MediaImage`; `ensure_media_image_` reachable on macOS (`using` if needed). Confirm only the intended files changed across all commits and the Linux suite is 323/323 + cargo green.

- [ ] **Step 4: Fix anything found, re-verify, commit fixes**

---

## Self-Review (author)

- **Spec coverage:** formatted-body first line (Task 1 `html_first_line`/`text_kind`, tested) ✓; "<user> sent an image" + video/file/audio (Task 1 kinds + Task 3 compose, approved option) ✓; sticker thumbnail ~28 px + "<user> sent a sticker" fallback (Task 3 + Task 4/5 providers) ✓; "You sent an image" self / no DM special-case (Task 3 sender resolution, approved) ✓; notice/emote as text (Task 1, approved minor improvement) ✓.
- **Placeholder scan:** the only "TBD-shaped" spot — the deliberately-rejected `paint_badge` early-return form — is explicitly called out and the concrete flag-based replacement is mandated; no other placeholders.
- **Type consistency:** `LatestPreview{kind,text,sticker_url}`, `last_message_kind`/`last_message_sticker_url`, `StickerProvider`/`set_sticker_provider`/`sticker_provider_` used identically across Tasks 1-6. `ensure_media_image_(mxc, 64, 64)` consistent in all 4 shell lambdas. `has_preview` redefinition consistent with the kind field.
- **Risk:** macOS `_relayoutRoomSurface` add is the one behavioural change to an existing path (separate-surface shell); confined and mirrors the RoomAvatar path. Win32/macOS unverifiable here — parity-review, standing caveat.
