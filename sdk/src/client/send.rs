//! Message and media sending: text, replies, threads, reactions, edits,
//! redactions, read receipts, stickers, images, files, audio/voice, and video.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{
    build_uia_fallback_url, // keep import path alive for downstream re-exports
    err, ok, send_both_receipts, try_op, ClientFfi,
};

#[cfg(test)]
use super::{err, ok, ClientFfi};

#[cfg(not(test))]
use super::{encode_voice_ogg, require_room};

#[cfg(not(test))]
use matrix_sdk::ruma::{events::room::message::RoomMessageEventContent, OwnedRoomId};

#[cfg(not(test))]
use matrix_sdk_ui::timeline::TimelineEventItemId;

#[cfg(not(test))]
use std::sync::atomic::Ordering;
#[cfg(not(test))]
use std::sync::Arc;

// silence unused-import warnings for re-exports kept only to preserve paths
#[cfg(not(test))]
#[allow(unused_imports)]
use build_uia_fallback_url as _unused_uia;

// ---------------------------------------------------------------------------
// Free helpers (shared by the send methods below and called from tests in
// `mod.rs`).
// ---------------------------------------------------------------------------

/// Build the raw `m.room.message` content object for an animated image
/// (GIF/WebP). Carries the MSC4230 `org.matrix.msc4230.is_animated` flag and
/// the `fi.mau.video.gif` vendor hint so capable clients autoplay/loop the
/// animation. `body` is the caption when one is supplied, otherwise the
/// filename; the dedicated MSC2530 `filename` field is only added when a
/// caption is present (matching the rest of the send path). `width`/`height`
/// of 0 are omitted from `info`. A non-empty `reply_event_id` adds an
/// `m.in_reply_to` relation. When `thread_root` is non-empty the relation is
/// emitted as a full MSC3440 `m.thread` block instead of a bare
/// `m.in_reply_to`.
pub(crate) fn build_animated_image_content(
    mxc_uri: &str,
    filename: &str,
    caption: &str,
    mime_type: &str,
    width: u32,
    height: u32,
    size: usize,
    reply_event_id: &str,
    thread_root: &str,
) -> serde_json::Value {
    let body = if caption.is_empty() { filename } else { caption };
    let mut info = serde_json::json!({
        "mimetype": mime_type,
        "size": size,
        "fi.mau.video.gif": true,
    });
    if width != 0 {
        info["w"] = serde_json::json!(width);
    }
    if height != 0 {
        info["h"] = serde_json::json!(height);
    }
    let mut content = serde_json::json!({
        "msgtype": "m.image",
        "body": body,
        "url": mxc_uri,
        "info": info,
        "org.matrix.msc4230.is_animated": true,
    });
    if !caption.is_empty() {
        content["filename"] = serde_json::json!(filename);
    }
    if !thread_root.is_empty() {
        // MSC3440 thread relation.
        let in_reply_to_id = if reply_event_id.is_empty() { thread_root } else { reply_event_id };
        let mut relates = serde_json::json!({
            "rel_type": "m.thread",
            "event_id": thread_root,
            "m.in_reply_to": { "event_id": in_reply_to_id },
        });
        if reply_event_id.is_empty() {
            relates["is_falling_back"] = serde_json::json!(true);
        }
        content["m.relates_to"] = relates;
    } else if !reply_event_id.is_empty() {
        content["m.relates_to"] = serde_json::json!({
            "m.in_reply_to": { "event_id": reply_event_id }
        });
    }
    content
}

/// Compute the optional `Reply` to attach to a media send. Returns `Ok(None)`
/// when neither `reply_event_id` nor `thread_root` is set (no reply needed),
/// `Ok(Some(reply))` with the appropriate `EnforceThread` variant, or
/// `Err(message)` when an event ID fails to parse.
///
/// Semantics:
/// - `thread_root` empty → same as before: reply only when `reply_event_id`
///   is set, and it uses `EnforceThread::Unthreaded`.
/// - `thread_root` non-empty → always attach a reply:
///   `event_id = reply_event_id` (when non-empty) or `thread_root`,
///   `enforce_thread = EnforceThread::Threaded(ReplyWithinThread::Yes/No)`.
#[cfg(not(test))]
pub(super) fn build_media_reply(
    reply_event_id: &str,
    thread_root: &str,
) -> Result<Option<matrix_sdk::room::reply::Reply>, String> {
    use matrix_sdk::room::reply::{EnforceThread, Reply};
    use matrix_sdk::ruma::events::room::message::{AddMentions, ReplyWithinThread};
    if thread_root.is_empty() {
        if reply_event_id.is_empty() {
            return Ok(None);
        }
        let id = reply_event_id
            .parse()
            .map_err(|e| format!("invalid reply event id: {e}"))?;
        return Ok(Some(Reply {
            event_id: id,
            enforce_thread: EnforceThread::Unthreaded,
            add_mentions: AddMentions::No,
        }));
    }
    let (target, within) = if reply_event_id.is_empty() {
        (thread_root, ReplyWithinThread::No)
    } else {
        (reply_event_id, ReplyWithinThread::Yes)
    };
    let id = target
        .parse()
        .map_err(|e| format!("invalid reply event id: {e}"))?;
    Ok(Some(Reply {
        event_id: id,
        enforce_thread: EnforceThread::Threaded(within),
        add_mentions: AddMentions::No,
    }))
}

/// Strip the `https://matrix.to/#/` (or `http://`) prefix from an href and
/// return the permalink target (e.g. `@user:server` or `@room`). Trailing
/// query/fragment params (`?via=…`) are dropped. Returns `None` for non
/// matrix.to links.
pub(super) fn matrix_to_target(href: &str) -> Option<&str> {
    for prefix in ["https://matrix.to/#/", "http://matrix.to/#/"] {
        if let Some(rest) = href.strip_prefix(prefix) {
            return Some(rest.split('?').next().unwrap_or(rest));
        }
    }
    None
}

/// Find the next `<a` opening-tag start (the tag name must be exactly `a`,
/// i.e. followed by whitespace or `>` — not `<abbr` etc.).
pub(super) fn find_anchor_open(s: &str) -> Option<usize> {
    let b = s.as_bytes();
    let mut i = 0usize;
    while i + 1 < b.len() {
        if b[i] == b'<' && (b[i + 1] == b'a' || b[i + 1] == b'A') {
            match b.get(i + 2).copied() {
                Some(c)
                    if c == b' '
                        || c == b'\t'
                        || c == b'\n'
                        || c == b'\r'
                        || c == b'>' =>
                {
                    return Some(i)
                }
                _ => {}
            }
        }
        i += 1;
    }
    None
}

/// Extract the `href` attribute value from an `<a …>` opening tag.
pub(super) fn extract_href(open_tag: &str) -> Option<String> {
    let idx = open_tag.find("href=")?;
    let after = &open_tag[idx + 5..];
    let q = after.chars().next()?;
    if q == '"' || q == '\'' {
        let end = after[1..].find(q)?;
        Some(after[1..1 + end].to_string())
    } else {
        let end = after
            .find(|c: char| c == ' ' || c == '>' || c == '\t')
            .unwrap_or(after.len());
        Some(after[..end].to_string())
    }
}

/// Derive the intentional `m.mentions` (MSC3952 / spec §user-and-room-mentions)
/// from an outgoing HTML `formatted_body`, and return the (possibly rewritten)
/// HTML.
///
/// - `matrix.to/#/@user:server` anchors contribute their user id to
///   `mentions.user_ids` and are kept verbatim.
/// - The `@room` sentinel anchor (`matrix.to/#/@room`, no domain) sets
///   `mentions.room = true` AND is rewritten to its plain inner text, so
///   receivers get spec-standard `@room` output rather than a bogus link.
///
/// This runs on HTML *we* generated (lowercase tags), so anchor matching is
/// case-sensitive on the closing `</a>`.
pub(crate) fn derive_mentions(
    formatted_body: &str,
) -> (Option<matrix_sdk::ruma::events::Mentions>, String) {
    use matrix_sdk::ruma::UserId;
    use std::collections::BTreeSet;

    let mut user_ids: BTreeSet<matrix_sdk::ruma::OwnedUserId> = BTreeSet::new();
    let mut room = false;
    let mut out = String::with_capacity(formatted_body.len());
    let mut rest = formatted_body;

    while let Some(start) = find_anchor_open(rest) {
        out.push_str(&rest[..start]);
        let after = &rest[start..];
        let Some(gt) = after.find('>') else {
            out.push_str(after);
            return (finish(user_ids, room), out);
        };
        let open_tag = &after[..=gt];
        let inner_and_rest = &after[gt + 1..];
        let Some(close_rel) = inner_and_rest.find("</a>") else {
            // Malformed (no closing tag): keep the open tag, continue scanning.
            out.push_str(open_tag);
            rest = inner_and_rest;
            continue;
        };
        let inner = &inner_and_rest[..close_rel];
        let after_close = &inner_and_rest[close_rel + "</a>".len()..];

        match extract_href(open_tag).as_deref().and_then(matrix_to_target) {
            Some("@room") => {
                room = true;
                out.push_str(inner); // strip the sentinel anchor
            }
            Some(target) => {
                if let Ok(uid) = UserId::parse(target) {
                    user_ids.insert(uid);
                }
                out.push_str(open_tag);
                out.push_str(inner);
                out.push_str("</a>");
            }
            None => {
                out.push_str(open_tag);
                out.push_str(inner);
                out.push_str("</a>");
            }
        }
        rest = after_close;
    }
    out.push_str(rest);
    (finish(user_ids, room), out)
}

/// Assemble a `Mentions` from collected ids/room flag, or `None` when empty.
fn finish(
    user_ids: std::collections::BTreeSet<matrix_sdk::ruma::OwnedUserId>,
    room: bool,
) -> Option<matrix_sdk::ruma::events::Mentions> {
    if user_ids.is_empty() && !room {
        return None;
    }
    let mut m = matrix_sdk::ruma::events::Mentions::new();
    m.user_ids = user_ids;
    m.room = room;
    Some(m)
}

/// Build an `m.room.message` content object carrying an `m.thread` relation.
/// `thread_root` is the thread root event id. When `in_reply_to` is non-empty
/// the relation also carries an `m.in_reply_to` (a reply to a specific message
/// within the thread); otherwise it is a plain thread message.
pub(crate) fn build_thread_message_content(
    body: &str,
    formatted_body: &str,
    thread_root: &str,
    in_reply_to: &str,
) -> serde_json::Value {
    let (mentions, html) = derive_mentions(formatted_body);
    let mut content = serde_json::json!({
        "msgtype": "m.text",
        "body": body,
    });
    if !html.is_empty() {
        content["format"] = serde_json::json!("org.matrix.custom.html");
        content["formatted_body"] = serde_json::json!(html);
    }
    if let Some(m) = mentions {
        if let Ok(v) = serde_json::to_value(&m) {
            content["m.mentions"] = v;
        }
    }
    let mut relates = serde_json::json!({
        "rel_type": "m.thread",
        "event_id": thread_root,
    });
    if in_reply_to.is_empty() {
        relates["m.in_reply_to"] = serde_json::json!({ "event_id": thread_root });
        relates["is_falling_back"] = serde_json::json!(true);
    } else {
        relates["m.in_reply_to"] = serde_json::json!({ "event_id": in_reply_to });
    }
    content["m.relates_to"] = relates;
    content
}

// ---------------------------------------------------------------------------
// ClientFfi send methods
// ---------------------------------------------------------------------------

impl ClientFfi {
    #[cfg(not(test))]
    pub fn send_message(&mut self, room_id: &str, body: &str, formatted_body: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let (mentions, html) = derive_mentions(formatted_body);
        let mut content = if html.is_empty() {
            RoomMessageEventContent::text_plain(body)
        } else {
            RoomMessageEventContent::text_html(body, &html)
        };
        content.mentions = mentions;
        // Use the live timeline if subscribed — local echo fires immediately.
        {
            let maybe_tl = {
                let guard = self.timelines.read().unwrap();
                guard.get(&room_id).map(|h| h.timeline.clone())
            };
            if let Some(timeline) = maybe_tl {
                return match self
                    .rt
                    .block_on(async move { timeline.send(content.into()).await })
                {
                    Ok(_) => ok(""),
                    Err(e) => err(e.to_string()),
                };
            }
        }
        // Fallback: no timeline subscribed for this room.
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_message(&mut self, _room_id: &str, _body: &str, _formatted_body: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn retry_send(&mut self, room_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        room.send_queue().set_enabled(true);
        ok("")
    }

    #[cfg(test)]
    pub fn retry_send(&mut self, _room_id: &str) -> OpResult {
        ok("")
    }

    #[cfg(not(test))]
    pub fn abort_send(&mut self, room_id: &str, txn_id: &str) -> OpResult {
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let txn_id: matrix_sdk::ruma::OwnedTransactionId = txn_id.into();
        let timeline = {
            let guard = self.timelines.read().unwrap();
            let Some(handle) = guard.get(&room_id) else {
                return err("no timeline for room");
            };
            handle.timeline.clone()
        };
        let item_id = TimelineEventItemId::TransactionId(txn_id);
        match self
            .rt
            .block_on(async move { timeline.redact(&item_id, None).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn abort_send(&mut self, _room_id: &str, _txn_id: &str) -> OpResult {
        ok("")
    }

    /// Send a typing notice to `room_id`. Fire-and-forget; errors are swallowed.
    #[cfg(not(test))]
    pub fn send_typing_notice(&mut self, room_id: &str, typing: bool) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        self.rt.spawn(async move {
            let Some(room) = client.get_room(&room_id) else {
                return;
            };
            let _ = room.typing_notice(typing).await;
        });
    }

    #[cfg(test)]
    pub fn send_typing_notice(&mut self, _room_id: &str, _typing: bool) {}

    /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds the
    /// `m.in_reply_to` relation and sends via `room.send()`. Does not require
    /// `subscribe_room`. Does not add the plain-text fallback body (Tesseract
    /// renders its own quote block).
    #[cfg(not(test))]
    pub fn send_reply(
        &mut self,
        room_id: &str,
        event_id: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::relation::{InReplyTo, Reply};
        use matrix_sdk::ruma::events::room::message::Relation;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let (mentions, html) = derive_mentions(formatted_body);
        let mut content = if html.is_empty() {
            RoomMessageEventContent::text_plain(body)
        } else {
            RoomMessageEventContent::text_html(body, &html)
        };
        content.mentions = mentions;
        content.relates_to = Some(Relation::Reply(Reply::new(InReplyTo::new(event_id))));
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_reply(
        &mut self,
        _room_id: &str,
        _event_id: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_thread_message(
        &mut self,
        room_id: &str,
        thread_root: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        self.send_thread_inner(room_id, thread_root, "", body, formatted_body)
    }

    #[cfg(not(test))]
    pub fn send_thread_reply(
        &mut self,
        room_id: &str,
        thread_root: &str,
        in_reply_to_event_id: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        if in_reply_to_event_id.is_empty() {
            return err("in_reply_to_event_id required");
        }
        self.send_thread_inner(
            room_id,
            thread_root,
            in_reply_to_event_id,
            body,
            formatted_body,
        )
    }

    #[cfg(not(test))]
    fn send_thread_inner(
        &mut self,
        room_id: &str,
        thread_root: &str,
        in_reply_to: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let root: matrix_sdk::ruma::OwnedEventId = match thread_root.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid thread root id: {e}")),
        };
        if !in_reply_to.is_empty()
            && in_reply_to.parse::<matrix_sdk::ruma::OwnedEventId>().is_err()
        {
            return err("invalid in_reply_to id");
        }
        // For plain thread messages (no explicit in_reply_to), use the subscribed
        // thread timeline so the local echo fires via on_thread_inserted immediately.
        if in_reply_to.is_empty() {
            let key = (room_id.clone(), root.clone());
            if let Some(handle) = self.thread_timelines.get(&key) {
                let timeline = handle.timeline.clone();
                let (mentions, html) = derive_mentions(formatted_body);
                let mut msg = if html.is_empty() {
                    RoomMessageEventContent::text_plain(body)
                } else {
                    RoomMessageEventContent::text_html(body, &html)
                };
                msg.mentions = mentions;
                return match self
                    .rt
                    .block_on(async move { timeline.send(msg.into()).await })
                {
                    Ok(_) => ok(""),
                    Err(e) => err(e.to_string()),
                };
            }
        }
        // Fallback: no active timeline subscription or explicit in_reply_to —
        // send raw with the manual thread relation already encoded in the JSON.
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let content =
            build_thread_message_content(body, formatted_body, thread_root, in_reply_to);
        match self
            .rt
            .block_on(async move { room.send_raw("m.room.message", content).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_thread_message(
        &mut self,
        _room_id: &str,
        _thread_root: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(test)]
    pub fn send_thread_reply(
        &mut self,
        _room_id: &str,
        _thread_root: &str,
        _in_reply_to_event_id: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Trigger an async fetch of the replied-to event's details for all
    /// timeline items in `room_id` that reference `event_id` via
    /// `m.in_reply_to`. When the data arrives, the SDK re-emits every
    /// affected item as an `on_message_updated` callback so the UI can
    /// paint the quote block with the resolved sender name and body snippet.
    /// Requires `subscribe_room`. The call spawns a tokio task and returns
    /// immediately — it never blocks the UI thread.
    #[cfg(not(test))]
    pub fn fetch_reply_details(&mut self, room_id: &str, event_id: &str) -> OpResult {
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let tl = {
            let guard = self.timelines.read().unwrap();
            let Some(handle) = guard.get(&room_id) else {
                return err("room not subscribed");
            };
            Arc::clone(&handle.timeline)
        };
        self.rt.spawn(async move {
            let _ = tl.fetch_details_for_event(&event_id).await;
        });
        ok("")
    }

    #[cfg(test)]
    pub fn fetch_reply_details(&mut self, _: &str, _: &str) -> OpResult {
        err("not logged in")
    }

    /// Upload `bytes` (already encoded as `mime_type`) and send an `m.image`
    /// event. Caption/filename handling follows MSC2530 — see the FFI doc
    /// comment in `bridge.rs`. Returns `OpResult` with `ok=false` for
    /// invalid IDs, unknown rooms, bad mime strings, upload failures, or
    /// send failures. `width`/`height` of 0 are passed through unset.
    /// When `is_animated` is true the image is sent as a raw `m.image`
    /// event carrying the MSC4230 `org.matrix.msc4230.is_animated` flag and
    /// the `fi.mau.video.gif` vendor hint so animated GIFs/WebPs autoplay
    /// and loop on capable clients (the standard `send_attachment` path
    /// strips these fields).
    #[cfg(not(test))]
    pub fn send_image(
        &mut self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        width: u32,
        height: u32,
        is_animated: bool,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseImageInfo};
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::ruma::UInt;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };

        // Animated GIF/WebP path: `send_attachment` strips the MSC4230
        // `is_animated` flag and the `fi.mau.video.gif` vendor hint, so we
        // upload the media ourselves and post a raw `m.image` event whose
        // `info` carries those fields.
        if is_animated {
            let mime_owned = mime.clone();
            let mime_str = mime_type.to_owned();
            let filename = filename.to_owned();
            let caption = caption.to_owned();
            let reply_event_id = reply_event_id.to_owned();
            let thread_root = thread_root.to_owned();
            let size = bytes.len();
            let bytes_owned = bytes.to_vec();
            return match self.rt.block_on(async move {
                let uploaded = client
                    .media()
                    .upload(&mime_owned, bytes_owned, None)
                    .await?;
                let mxc_uri = uploaded.content_uri.to_string();
                let content = build_animated_image_content(
                    &mxc_uri,
                    &filename,
                    &caption,
                    &mime_str,
                    width,
                    height,
                    size,
                    &reply_event_id,
                    &thread_root,
                );
                room.send_raw("m.room.message", content).await
            }) {
                Ok(_) => ok(""),
                Err(e) => err(e.to_string()),
            };
        }

        let info = BaseImageInfo {
            width: if width != 0 {
                UInt::new(width as u64)
            } else {
                None
            },
            height: if height != 0 {
                UInt::new(height as u64)
            } else {
                None
            },
            size: UInt::new(bytes.len() as u64),
            blurhash: None,
            is_animated: None,
        };
        let mut config = AttachmentConfig::new().info(AttachmentInfo::Image(info));
        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        match build_media_reply(reply_event_id, thread_root) {
            Ok(Some(reply)) => config = config.reply(Some(reply)),
            Ok(None) => {}
            Err(e) => return err(e),
        }

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(async move { room.send_attachment(filename, &mime, data, config).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_image(
        &mut self,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _width: u32,
        _height: u32,
        _is_animated: bool,
        _reply_event_id: &str,
        _thread_root: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Upload `bytes` as-is and send an `m.file` event. See the FFI doc
    /// comment in `bridge.rs` for caption/filename framing.
    /// `AttachmentInfo::File` carries `size` so the homeserver includes it in
    /// the `m.file` `info` block. Encryption is handled transparently for E2EE rooms.
    #[cfg(not(test))]
    pub fn send_file(
        &mut self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseFileInfo};
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::ruma::UInt;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };

        let info = BaseFileInfo {
            size: UInt::new(bytes.len() as u64),
        };
        let mut config = AttachmentConfig::new().info(AttachmentInfo::File(info));
        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        match build_media_reply(reply_event_id, thread_root) {
            Ok(Some(reply)) => config = config.reply(Some(reply)),
            Ok(None) => {}
            Err(e) => return err(e),
        }

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(async move { room.send_attachment(filename, &mime, data, config).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_file(
        &mut self,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _reply_event_id: &str,
        _thread_root: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Upload `bytes` and send a plain `m.audio` event (not MSC3245 voice).
    /// `duration_ms` populates `info.duration`; pass 0 when unknown.
    /// `caption` / `reply_event_id` follow the same MSC2530 / m.in_reply_to
    /// framing as `send_image` and `send_file`.
    #[cfg(not(test))]
    pub fn send_audio(
        &mut self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        duration_ms: u64,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseAudioInfo};
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id_parsed = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id_parsed) else {
            return err("room not found");
        };
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };

        let info = BaseAudioInfo {
            duration: if duration_ms > 0 {
                Some(Duration::from_millis(duration_ms))
            } else {
                None
            },
            size: UInt::new(bytes.len() as u64),
            waveform: None,
        };
        let mut config = AttachmentConfig::new().info(AttachmentInfo::Audio(info));
        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        match build_media_reply(reply_event_id, thread_root) {
            Ok(Some(reply)) => config = config.reply(Some(reply)),
            Ok(None) => {}
            Err(e) => return err(e),
        }

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(async move { room.send_attachment(filename, &mime, data, config).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_audio(
        &mut self,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _duration_ms: u64,
        _reply_event_id: &str,
        _thread_root: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Send a video file to `room_id` as an `m.video` event. `width`/`height`
    /// are the video source dimensions; `thumb_bytes` is a JPEG first-frame
    /// thumbnail (empty slice if unavailable); `thumb_width`/`thumb_height`
    /// are its dimensions; `duration_ms` populates `info.duration`. The SDK
    /// uploads the thumbnail as a separate media item and sets
    /// `info.thumbnail_url`. E2EE rooms are handled transparently.
    #[cfg(not(test))]
    pub fn send_video(
        &mut self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        width: u32,
        height: u32,
        thumb_bytes: &[u8],
        thumb_width: u32,
        thumb_height: u32,
        duration_ms: u64,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseVideoInfo, Thumbnail};
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id_parsed = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id_parsed) else {
            return err("room not found");
        };
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };

        let info = BaseVideoInfo {
            duration: if duration_ms > 0 {
                Some(Duration::from_millis(duration_ms))
            } else {
                None
            },
            height: UInt::new(height as u64),
            width: UInt::new(width as u64),
            size: UInt::new(bytes.len() as u64),
            blurhash: None,
        };
        let mut config = AttachmentConfig::new().info(AttachmentInfo::Video(info));
        if !thumb_bytes.is_empty() {
            config = config.thumbnail(Some(Thumbnail {
                data: thumb_bytes.to_vec(),
                content_type: mime::IMAGE_JPEG,
                height: UInt::new(thumb_height as u64).unwrap_or_default(),
                width: UInt::new(thumb_width as u64).unwrap_or_default(),
                size: UInt::new(thumb_bytes.len() as u64).unwrap_or_default(),
            }));
        }
        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        match build_media_reply(reply_event_id, thread_root) {
            Ok(Some(reply)) => config = config.reply(Some(reply)),
            Ok(None) => {}
            Err(e) => return err(e),
        }

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(async move { room.send_attachment(filename, &mime, data, config).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_video(
        &mut self,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _width: u32,
        _height: u32,
        _thumb_bytes: &[u8],
        _thumb_width: u32,
        _thumb_height: u32,
        _duration_ms: u64,
        _reply_event_id: &str,
        _thread_root: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Encode `pcm` (raw signed 16-bit mono 48 kHz samples as a byte slice,
    /// little-endian) into an Ogg/Opus stream and send it as an MSC3245
    /// `m.voice` event in `room_id`. `waveform` carries the MSC1767 waveform
    /// samples (clamped to 256 values of 0–1024; stored as f32 normalised to
    /// 0.0–1.0 in the audio info). `duration_ms` populates `info.duration`.
    /// `caption` / `reply_event_id` follow the same MSC2530 / m.in_reply_to
    /// framing as `send_image` and `send_file`.
    #[cfg(not(test))]
    pub fn send_voice(
        &mut self,
        room_id: &str,
        pcm: &[u8],
        duration_ms: u64,
        waveform: &[u16],
        caption: &str,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentConfig, AttachmentInfo, BaseAudioInfo};
        use matrix_sdk::ruma::events::room::message::TextMessageEventContent;
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        if pcm.is_empty() {
            return err("empty PCM");
        }
        if pcm.len() % 2 != 0 {
            return err("PCM byte count must be even");
        }

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id_parsed = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id_parsed) else {
            return err("room not found");
        };

        // SAFETY: pcm is guaranteed to have even length above; i16 has no
        // alignment requirement beyond u8.
        let samples: &[i16] = unsafe {
            std::slice::from_raw_parts(pcm.as_ptr() as *const i16, pcm.len() / 2)
        };

        let ogg_bytes = match encode_voice_ogg(samples, waveform, duration_ms) {
            Ok(b) => b,
            Err(e) => return err(format!("encode failed: {e}")),
        };

        // Normalise waveform samples (0–1024) into f32 (0.0–1.0), capped at 256.
        let waveform_f32: Vec<f32> = waveform
            .iter()
            .copied()
            .take(256)
            .map(|v| (v as f32) / 1024.0)
            .collect();

        let size = UInt::new(ogg_bytes.len() as u64);
        let dur = if duration_ms > 0 {
            Some(Duration::from_millis(duration_ms))
        } else {
            None
        };
        let info = BaseAudioInfo {
            duration: dur,
            size,
            waveform: if waveform_f32.is_empty() {
                None
            } else {
                Some(waveform_f32)
            },
        };
        let mut config = AttachmentConfig::new().info(AttachmentInfo::Voice(info));

        if !caption.is_empty() {
            config = config.caption(Some(TextMessageEventContent::plain(caption)));
        }
        match build_media_reply(reply_event_id, thread_root) {
            Ok(Some(reply)) => config = config.reply(Some(reply)),
            Ok(None) => {}
            Err(e) => return err(e),
        }

        let mime: mime::Mime = "audio/ogg; codecs=opus".parse().unwrap();

        match self.rt.block_on(async move {
            room.send_attachment("voice-message.ogg".to_owned(), &mime, ogg_bytes, config)
                .await
        }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_voice(
        &mut self,
        _room_id: &str,
        _pcm: &[u8],
        _duration_ms: u64,
        _waveform: &[u16],
        _caption: &str,
        _reply_event_id: &str,
        _thread_root: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Returns the cached homeserver upload limit, lazily fetching it on the
    /// first call after login. The query (`/_matrix/media/v3/config`) is
    /// blocking but happens at most once per session.
    /// Returns 0 when unknown, the server doesn't advertise a limit, or the
    /// client is not logged in.
    #[cfg(not(test))]
    pub fn media_upload_limit(&mut self) -> u64 {
        let cached = self.media_upload_limit.load(Ordering::Relaxed);
        if cached != 0 {
            return cached;
        }

        let Some(client) = self.client.clone() else {
            return 0;
        };
        let limit = self.rt.block_on(async move {
            client
                .load_or_fetch_max_upload_size()
                .await
                .map(u64::from)
                .unwrap_or(0)
        });
        if limit != 0 {
            self.media_upload_limit.store(limit, Ordering::Relaxed);
        }
        limit
    }

    #[cfg(test)]
    pub fn media_upload_limit(&mut self) -> u64 {
        0
    }

    /// Toggle the current user's `key` reaction on `event_id` in `room_id`.
    /// First call adds the reaction; second redacts it. Requires that
    /// `room_id` is currently subscribed via `subscribe_room` — we look up
    /// its `Timeline` handle to invoke `toggle_reaction`.
    #[cfg(not(test))]
    pub fn send_reaction(&mut self, room_id: &str, event_id: &str, key: &str) -> OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }
        if key.is_empty() {
            return err("reaction key is empty");
        }

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        let tl = {
            let guard = self.timelines.read().unwrap();
            let Some(handle) = guard.get(&room_id) else {
                return err("room not subscribed; call subscribe_room first");
            };
            Arc::clone(&handle.timeline)
        };
        let item_id = TimelineEventItemId::EventId(event_id);
        let key = key.to_owned();

        match self
            .rt
            .block_on(async move { tl.toggle_reaction(&item_id, &key).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_reaction(&mut self, _room_id: &str, _event_id: &str, _key: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_reaction_custom(
        &mut self,
        room_id: &str,
        event_id: &str,
        key: &str,
        shortcode: &str,
    ) -> OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }
        if key.is_empty() {
            return err("reaction key is empty");
        }

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id = event_id.to_owned();
        let key = key.to_owned();
        let shortcode = shortcode.to_owned();

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };

        match self.rt.block_on(async move {
            let mut content = serde_json::json!({
                "m.relates_to": {
                    "rel_type": "m.annotation",
                    "event_id": event_id,
                    "key": key,
                }
            });
            if !shortcode.is_empty() {
                content["com.beeper.reaction.shortcode"] = serde_json::Value::String(shortcode);
            }
            room.send_raw("m.reaction", content).await
        }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_reaction_custom(
        &mut self,
        _room_id: &str,
        _event_id: &str,
        _key: &str,
        _shortcode: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_read_receipt(&mut self, room_id: &str, event_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let room = match client.get_room(&room_id) {
            Some(r) => r,
            None => return err("room not found"),
        };
        match self.rt.block_on(send_both_receipts(&room, event_id)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_read_receipt(&mut self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Send public `m.read` and private `m.read.private` receipts for the
    /// latest cached event in `room_id`. Clears the unread count without
    /// requiring the room to be subscribed via `subscribe_room`.
    #[cfg(not(test))]
    pub fn mark_room_as_read(&mut self, room_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let room = match client.get_room(&room_id) {
            Some(r) => r,
            None => return err("room not found"),
        };
        // Deref to the base room type to get the synchronous `latest_event()`
        // with `event_id()`. The `RoomExt` trait (from matrix-sdk-ui, in scope
        // for timeline features) would otherwise shadow it with an async version
        // that doesn't carry the event ID.
        let event_id = match std::ops::Deref::deref(&room).latest_event().event_id() {
            Some(id) => id.to_owned(),
            None => return ok(""),
        };
        match self.rt.block_on(send_both_receipts(&room, event_id)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn mark_room_as_read(&mut self, _room_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
    /// Wraps matrix-sdk-ui's `Timeline::redact`. The room must currently
    /// be subscribed via `subscribe_room`. Server-side permission errors
    /// (e.g. trying to redact someone else's message without power) surface
    /// as `OpResult { ok: false, message: ... }`.
    #[cfg(not(test))]
    pub fn redact_event(&mut self, room_id: &str, event_id: &str, reason: &str) -> OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }

        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        let tl = {
            let guard = self.timelines.read().unwrap();
            let Some(handle) = guard.get(&room_id) else {
                return err("room not subscribed; call subscribe_room first");
            };
            Arc::clone(&handle.timeline)
        };
        let item_id = TimelineEventItemId::EventId(event_id);
        let reason_opt = if reason.is_empty() {
            None
        } else {
            Some(reason.to_owned())
        };

        match self
            .rt
            .block_on(async move { tl.redact(&item_id, reason_opt.as_deref()).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn redact_event(&mut self, _room_id: &str, _event_id: &str, _reason: &str) -> OpResult {
        err("not logged in")
    }

    /// Edit `event_id` in `room_id` replacing its body with `new_body`.
    /// Uses `Room::make_edit_event` (builds the `m.replace` Replacement
    /// relation) then sends via `RoomSendQueue`. Only own `m.text` events
    /// can be edited; the SDK returns an error for non-own or non-text
    /// events. Does not require `subscribe_room`.
    #[cfg(not(test))]
    pub fn send_edit(
        &mut self,
        room_id: &str,
        event_id: &str,
        new_body: &str,
        formatted_body: &str,
    ) -> OpResult {
        use matrix_sdk::room::edit::EditedContent;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };
        let new_content = if formatted_body.is_empty() {
            RoomMessageEventContent::text_plain(new_body)
        } else {
            RoomMessageEventContent::text_html(new_body, formatted_body)
        };
        match self.rt.block_on(async move {
            let edit_event = room
                .make_edit_event(&event_id, EditedContent::RoomMessage(new_content.into()))
                .await
                .map_err(|e| e.to_string())?;
            room.send_queue()
                .send(edit_event)
                .await
                .map_err(|e| e.to_string())
        }) {
            Ok(_) => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn send_edit(
        &mut self,
        _room_id: &str,
        _event_id: &str,
        _new_body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Send an `m.sticker` event to `room_id`. Wraps
    /// `room.send(StickerEventContent { .. })`. matrix-sdk encrypts in E2EE
    /// rooms transparently; outgoing stickers always carry a plain
    /// `mxc://` source.
    #[cfg(not(test))]
    pub fn send_sticker(
        &mut self,
        room_id: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::room::ImageInfo;
        use matrix_sdk::ruma::events::sticker::{StickerEventContent, StickerMediaSource};
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let Some(room) = client.get_room(&room_id) else {
            return err("room not found");
        };

        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let uri = OwnedMxcUri::from(image_url);
        if !uri.is_valid() {
            return err("image_url is not a valid mxc:// uri");
        }

        let info: ImageInfo = if info_json.is_empty() || info_json == "{}" {
            ImageInfo::new()
        } else {
            match serde_json::from_str(info_json) {
                Ok(i) => i,
                Err(_) => ImageInfo::new(),
            }
        };

        // ruma's StickerEventContent::new takes a plain mxc URI directly;
        // matrix-sdk handles E2EE rooms transparently when sending. The
        // `StickerMediaSource` enum (Plain / Encrypted) is only meaningful
        // for received events (parsed under the `compat-encrypted-stickers`
        // feature).
        let _ = StickerMediaSource::Plain(uri.clone()); // keep import path used
        let content = StickerEventContent::new(body.to_owned(), info, uri);

        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_sticker(
        &mut self,
        _room_id: &str,
        _body: &str,
        _image_url: &str,
        _info_json: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Send `m.sticker` into the MSC3440 thread rooted at `thread_root`.
    /// Uses the subscribed thread timeline when available (so the local echo
    /// fires via `on_thread_inserted`, not the room timeline). Falls back to
    /// `room.send_raw` with a manual `m.thread` relation when not subscribed.
    #[cfg(not(test))]
    pub fn send_thread_sticker(
        &mut self,
        room_id: &str,
        thread_root: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::room::ImageInfo;
        use matrix_sdk::ruma::events::sticker::StickerEventContent;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id_parsed = match matrix_sdk::ruma::RoomId::parse(room_id) {
            Ok(id) => id,
            Err(e) => return err(format!("invalid room id: {e}")),
        };
        let root: matrix_sdk::ruma::OwnedEventId = match thread_root.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid thread root id: {e}")),
        };
        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let uri = OwnedMxcUri::from(image_url);
        if !uri.is_valid() {
            return err("image_url is not a valid mxc:// uri");
        }
        let info: ImageInfo = if info_json.is_empty() || info_json == "{}" {
            ImageInfo::new()
        } else {
            match serde_json::from_str(info_json) {
                Ok(i) => i,
                Err(_) => ImageInfo::new(),
            }
        };

        // Prefer the subscribed thread timeline so the local echo fires via
        // on_thread_inserted (not the room timeline, which would show the
        // sticker in the main message list instead of the thread view).
        let key = (room_id_parsed.clone(), root.clone());
        if let Some(handle) = self.thread_timelines.get(&key) {
            let timeline = handle.timeline.clone();
            let content = StickerEventContent::new(body.to_owned(), info, uri);
            return match self.rt.block_on(async move {
                timeline.send(content.into()).await
            }) {
                Ok(_) => ok(""),
                Err(e) => err(e.to_string()),
            };
        }

        // Fallback: send via room with manual m.thread relation.
        let Some(room) = client.get_room(&room_id_parsed) else {
            return err("room not found");
        };
        let info_val = serde_json::to_value(&info).unwrap_or(serde_json::json!({}));
        let content = serde_json::json!({
            "body": body,
            "url": image_url,
            "info": info_val,
            "m.relates_to": {
                "rel_type": "m.thread",
                "event_id": thread_root,
                "m.in_reply_to": { "event_id": thread_root },
                "is_falling_back": true
            }
        });
        match self.rt.block_on(async move { room.send_raw("m.sticker", content).await }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_thread_sticker(
        &mut self,
        _room_id: &str,
        _thread_root: &str,
        _body: &str,
        _image_url: &str,
        _info_json: &str,
    ) -> OpResult {
        err("not logged in")
    }
}
