//! Message and media sending: text, replies, threads, reactions, edits,
//! redactions, read receipts, stickers, images, files, audio/voice, and video.
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use crate::ffi::OpResult;

#[cfg(not(test))]
use super::{err, ok, send_both_receipts, try_op, ClientFfi};

#[cfg(test)]
use super::{err, ok, ClientFfi};

#[cfg(not(test))]
use super::{encode_voice_ogg, parse_room_id, require_room};

#[cfg(not(test))]
use matrix_sdk::ruma::{events::room::message::RoomMessageEventContent, OwnedRoomId};

#[cfg(not(test))]
use matrix_sdk_ui::timeline::TimelineEventItemId;

#[cfg(not(test))]
use std::sync::atomic::Ordering;
#[cfg(not(test))]
use std::sync::Arc;

// ---------------------------------------------------------------------------
// Free helpers (shared by the send methods below and called from tests in
// `mod.rs`).
// ---------------------------------------------------------------------------

/// Build the raw `m.room.message` content object for an animated image
/// (GIF/WebP). Carries the MSC4230 `org.matrix.msc4230.is_animated` flag and
/// the `fi.mau.gif` vendor hint so capable clients autoplay/loop the
/// animation. `body` is the caption when one is supplied, otherwise the
/// filename; the dedicated MSC2530 `filename` field is only added when a
/// caption is present (matching the rest of the send path). `width`/`height`
/// of 0 are omitted from `info`. A non-empty `reply_event_id` adds an
/// `m.in_reply_to` relation. When `thread_root` is non-empty the relation is
/// emitted as a full MSC3440 `m.thread` block instead of a bare
/// `m.in_reply_to`.
pub(crate) fn build_animated_image_content(
    media: super::gif::GifMedia,
    filename: &str,
    caption: &str,
    mime_type: &str,
    width: u32,
    height: u32,
    size: usize,
    reply_event_id: &str,
    thread_root: &str,
) -> serde_json::Value {
    use super::gif::GifMedia;
    let body = if caption.is_empty() { filename } else { caption };
    let mut info = serde_json::json!({
        "mimetype": mime_type,
        "size": size,
        "fi.mau.gif": true,
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
        "info": info,
        "org.matrix.msc4230.is_animated": true,
    });
    // Plaintext rooms carry an `mxc://` URL; encrypted rooms carry a serialized
    // EncryptedFile `file` block (mirrors build_gif_video_content).
    match media {
        GifMedia::Plain(mxc) => content["url"] = serde_json::json!(mxc),
        GifMedia::Encrypted(file) => content["file"] = file,
    }
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

/// Build the `AttachmentConfig` shared by every `send_image`/`send_file`/
/// `send_audio`/`send_video` path (and their `_async` twins). Wires the
/// per-type `AttachmentInfo`, an optional video `Thumbnail`, the MSC2530
/// caption (only when non-empty), and the `m.in_reply_to` / `m.thread`
/// relation resolved by `build_media_reply`. The only per-type input is
/// `info` (and `thumbnail`, video-only); everything else is identical across
/// the eight methods.
#[cfg(not(test))]
pub(super) fn build_attachment_config(
    info: matrix_sdk::attachment::AttachmentInfo,
    thumbnail: Option<matrix_sdk::attachment::Thumbnail>,
    caption: &str,
    reply_event_id: &str,
    thread_root: &str,
) -> Result<matrix_sdk::attachment::AttachmentConfig, String> {
    use matrix_sdk::attachment::AttachmentConfig;
    use matrix_sdk::ruma::events::room::message::TextMessageEventContent;

    let mut config = AttachmentConfig::new().info(info);
    if let Some(thumb) = thumbnail {
        config = config.thumbnail(Some(thumb));
    }
    if !caption.is_empty() {
        config = config.caption(Some(TextMessageEventContent::plain(caption)));
    }
    if let Some(reply) = build_media_reply(reply_event_id, thread_root)? {
        config = config.reply(Some(reply));
    }
    Ok(config)
}

/// Shared send core for the media-send paths: upload `bytes` and post the
/// attachment built into `config`. Returns the error as a `String` so both
/// the blocking (`OpResult`) and async (`deliver`) callers can shape the
/// result however they need. This is the awaitable body the sync and async
/// twins share — the sync caller drives it with `block_on`, the async caller
/// awaits it inside its spawned task.
#[cfg(not(test))]
pub(super) async fn do_send_attachment(
    room: matrix_sdk::Room,
    filename: String,
    mime: mime::Mime,
    bytes: Vec<u8>,
    config: matrix_sdk::attachment::AttachmentConfig,
) -> Result<(), String> {
    room.send_attachment(filename, &mime, bytes, config)
        .await
        .map(|_| ())
        .map_err(|e| e.to_string())
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
            .find([' ', '>', '\t'])
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

/// Build an `m.room.message` content carrying an `m.thread` relation, used by
/// the `send_thread_inner` fallback when no thread Timeline is subscribed
/// (the subscribed path goes through `Timeline::send` / `Timeline::send_reply`,
/// which apply the relation themselves). When `in_reply_to` is `Some` the
/// relation is a true threaded reply (`is_falling_back: false`); otherwise it
/// is a plain thread message with the recommended `m.in_reply_to → thread_root`
/// fallback for clients that don't yet implement MSC3440
/// (`is_falling_back: true`).
pub(crate) fn build_thread_message_content(
    body: &str,
    formatted_body: &str,
    thread_root: matrix_sdk::ruma::OwnedEventId,
    in_reply_to: Option<matrix_sdk::ruma::OwnedEventId>,
) -> matrix_sdk::ruma::events::room::message::RoomMessageEventContent {
    use matrix_sdk::ruma::events::relation::Thread;
    use matrix_sdk::ruma::events::room::message::{Relation, RoomMessageEventContent};

    let (mentions, html) = derive_mentions(formatted_body);
    let mut msg = if html.is_empty() {
        RoomMessageEventContent::text_plain(body)
    } else {
        RoomMessageEventContent::text_html(body, &html)
    };
    msg.mentions = mentions;

    let thread = match in_reply_to {
        Some(reply_id) => Thread::reply(thread_root, reply_id),
        None => Thread::plain(thread_root.clone(), thread_root),
    };
    msg.relates_to = Some(Relation::Thread(thread));
    msg
}

// ---------------------------------------------------------------------------
// ClientFfi send methods
// ---------------------------------------------------------------------------

impl ClientFfi {
    /// Find the matrix-sdk-ui `Timeline` that currently carries `event_id`
    /// in `room_id`. Searches every subscribed thread Timeline for the room
    /// first (their items aren't visible from the room Timeline because
    /// `hide_threaded_events: true` filters in-thread replies out), then
    /// falls back to the live room Timeline. Returns `None` when no
    /// subscribed Timeline has the event — caller decides what to do (the
    /// thread root will always be found in the room Timeline if subscribed).
    ///
    /// Used by `send_reaction` and `redact_event` because matrix-sdk-ui's
    /// `Timeline::toggle_reaction` / `Timeline::redact` require the target
    /// event to live in *that* Timeline's items vector — they derive
    /// toggle / local-echo state from there.
    #[cfg(not(test))]
    async fn timeline_for_event(
        &self,
        room_id: &matrix_sdk::ruma::OwnedRoomId,
        event_id: &matrix_sdk::ruma::EventId,
    ) -> Option<Arc<matrix_sdk_ui::Timeline>> {
        let candidates: Vec<Arc<matrix_sdk_ui::Timeline>> = {
            let mut out = Vec::new();
            // Thread Timelines for this room first.
            for ((rid, _root), handle) in self.thread_timelines.read().iter() {
                if rid == room_id {
                    out.push(Arc::clone(&handle.timeline));
                }
            }
            // Room Timeline last (fallback for non-threaded events).
            {
                let guard = self.timelines.read();
                if let Some(handle) = guard.get(room_id) {
                    out.push(Arc::clone(&handle.timeline));
                }
            }
            out
        };
        for tl in candidates {
            if tl.item_by_event_id(event_id).await.is_some() {
                return Some(tl);
            }
        }
        None
    }

    /// Shared send path: client lookup, room_id parse, live-timeline routing
    /// with local-echo, and fallback to Room::send for unsubscribed rooms.
    #[cfg(not(test))]
    fn dispatch_room_msg_(&self, room_id: &str, content: RoomMessageEventContent) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id = try_op!(parse_room_id(room_id));
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/message".to_string(),
        );
        // Use the live timeline if subscribed — local echo fires immediately.
        {
            let maybe_tl = {
                let guard = self.timelines.read();
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
        let room = try_op!(client.get_room(&room_id).ok_or_else(|| err("room not found")));
        match self.rt.block_on(async move { room.send(content).await }) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(not(test))]
    pub fn send_message(&self, room_id: &str, body: &str, formatted_body: &str) -> OpResult {
        let (mentions, html) = derive_mentions(formatted_body);
        let mut content = if html.is_empty() {
            RoomMessageEventContent::text_plain(body)
        } else {
            RoomMessageEventContent::text_html(body, &html)
        };
        content.mentions = mentions;
        self.dispatch_room_msg_(room_id, content)
    }

    #[cfg(test)]
    pub fn send_message(&self, _room_id: &str, _body: &str, _formatted_body: &str) -> OpResult {
        err("not logged in")
    }

    /// Send an `m.emote` (the `/me` slash command). Callers strip the
    /// `/me ` prefix before invoking this.
    #[cfg(not(test))]
    pub fn send_emote(&self, room_id: &str, body: &str, formatted_body: &str) -> OpResult {
        let (mentions, html) = derive_mentions(formatted_body);
        let mut content = if html.is_empty() {
            RoomMessageEventContent::emote_plain(body)
        } else {
            RoomMessageEventContent::emote_html(body, &html)
        };
        content.mentions = mentions;
        self.dispatch_room_msg_(room_id, content)
    }

    #[cfg(test)]
    pub fn send_emote(&self, _room_id: &str, _body: &str, _formatted_body: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn retry_send(&self, room_id: &str) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        room.send_queue().set_enabled(true);
        ok("")
    }

    #[cfg(test)]
    pub fn retry_send(&self, _room_id: &str) -> OpResult {
        ok("")
    }

    #[cfg(not(test))]
    pub fn abort_send(&self, room_id: &str, txn_id: &str) -> OpResult {
        let room_id = try_op!(parse_room_id(room_id));
        let txn_id: matrix_sdk::ruma::OwnedTransactionId = txn_id.into();
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/abort".to_string(),
        );
        let timeline = {
            let guard = self.timelines.read();
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
    pub fn abort_send(&self, _room_id: &str, _txn_id: &str) -> OpResult {
        ok("")
    }

    /// Send a typing notice to `room_id`. Fire-and-forget; errors are swallowed.
    #[cfg(not(test))]
    pub fn send_typing_notice(&self, room_id: &str, typing: bool) {
        let Some(client) = self.client.clone() else {
            return;
        };
        let room_id: OwnedRoomId = match room_id.parse() {
            Ok(id) => id,
            Err(_) => return,
        };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "send/typing".to_string(),
            );
            let Some(room) = client.get_room(&room_id) else {
                return;
            };
            let _ = room.typing_notice(typing).await;
        });
    }

    #[cfg(test)]
    pub fn send_typing_notice(&self, _room_id: &str, _typing: bool) {}

    /// Send `body` as an `m.text` reply to `event_id` in `room_id`. Builds the
    /// `m.in_reply_to` relation and sends via `room.send()`. Does not require
    /// `subscribe_room`. Does not add the plain-text fallback body (Tesseract
    /// renders its own quote block).
    #[cfg(not(test))]
    pub fn send_reply(
        &self,
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
        let (_, room) = try_op!(require_room(&client, room_id));
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/reply".to_string(),
        );
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
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
        &self,
        _room_id: &str,
        _event_id: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_thread_message(
        &self,
        room_id: &str,
        thread_root: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        self.send_thread_inner(room_id, thread_root, "", body, formatted_body)
    }

    #[cfg(not(test))]
    pub fn send_thread_reply(
        &self,
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
        &self,
        room_id: &str,
        thread_root: &str,
        in_reply_to: &str,
        body: &str,
        formatted_body: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id = try_op!(parse_room_id(room_id));
        let root: matrix_sdk::ruma::OwnedEventId = match thread_root.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid thread root id: {e}")),
        };
        if !in_reply_to.is_empty()
            && in_reply_to.parse::<matrix_sdk::ruma::OwnedEventId>().is_err()
        {
            return err("invalid in_reply_to id");
        }
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/thread_message".to_string(),
        );
        // When the thread timeline is subscribed, route through it so the
        // local echo arrives via on_thread_inserted immediately:
        // - plain thread send → timeline.send (thread focus auto-tags the relation),
        // - reply within thread → timeline.send_reply (auto-applies
        //   EnforceThread::Threaded(ReplyWithinThread::Yes) on a threaded timeline,
        //   so the event lands with m.relates_to → m.thread + m.in_reply_to and
        //   gets observed by the subscribed timeline).
        let key = (room_id.clone(), root.clone());
        // Clone the timeline Arc out from under the read guard so it is not held
        // across the `block_on` below (that would risk a deadlock).
        let thread_timeline =
            self.thread_timelines.read().get(&key).map(|h| h.timeline.clone());
        if let Some(timeline) = thread_timeline {
            let (mentions, html) = derive_mentions(formatted_body);
            let mut msg = if html.is_empty() {
                RoomMessageEventContent::text_plain(body)
            } else {
                RoomMessageEventContent::text_html(body, &html)
            };
            msg.mentions = mentions;
            if in_reply_to.is_empty() {
                return match self
                    .rt
                    .block_on(async move { timeline.send(msg.into()).await })
                {
                    Ok(_) => ok(""),
                    Err(e) => err(e.to_string()),
                };
            }
            let reply_id: matrix_sdk::ruma::OwnedEventId = match in_reply_to.parse()
            {
                Ok(id) => id,
                Err(e) => return err(format!("invalid in_reply_to id: {e}")),
            };
            return match self.rt.block_on(async move {
                timeline.send_reply(msg.into(), reply_id).await
            }) {
                Ok(_) => ok(""),
                Err(e) => err(e.to_string()),
            };
        }
        // Fallback: no active thread timeline subscription — build the typed
        // RoomMessageEventContent with an m.thread relation and use
        // `Room::send` so matrix-sdk handles the type tag and the local-echo
        // queue. The hand-rolled JSON path used to live here; `Relation::Thread`
        // matches the same wire shape.
        let room = try_op!(client.get_room(&room_id).ok_or_else(|| err("room not found")));
        let reply_owned: Option<matrix_sdk::ruma::OwnedEventId> = if in_reply_to.is_empty() {
            None
        } else {
            in_reply_to.parse().ok()
        };
        let content = build_thread_message_content(body, formatted_body, root, reply_owned);
        match self
            .rt
            .block_on(async move { room.send(content).await })
        {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_thread_message(
        &self,
        _room_id: &str,
        _thread_root: &str,
        _body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(test)]
    pub fn send_thread_reply(
        &self,
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
    pub fn fetch_reply_details(&self, room_id: &str, event_id: &str) -> OpResult {
        let room_id = try_op!(parse_room_id(room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let tl = {
            let guard = self.timelines.read();
            let Some(handle) = guard.get(&room_id) else {
                return err("room not subscribed");
            };
            Arc::clone(&handle.timeline)
        };
        let in_flight = self.in_flight.clone();
        #[cfg(debug_assertions)]
        let in_flight_urls = Arc::clone(&self.in_flight_urls);
        let handler_for_guard = self.handler.clone();
        self.rt.spawn(async move {
            let _guard = super::InFlightGuard::new(
                &in_flight,
                &handler_for_guard,
                #[cfg(debug_assertions)] &in_flight_urls,
                #[cfg(debug_assertions)] "send/fetch_reply_details".to_string(),
            );
            let _ = tl.fetch_details_for_event(&event_id).await;
        });
        ok("")
    }

    #[cfg(test)]
    pub fn fetch_reply_details(&self, _: &str, _: &str) -> OpResult {
        err("not logged in")
    }

    /// Upload `bytes` (already encoded as `mime_type`) and send an `m.image`
    /// event. Caption/filename handling follows MSC2530 — see the FFI doc
    /// comment in `bridge.rs`. Returns `OpResult` with `ok=false` for
    /// invalid IDs, unknown rooms, bad mime strings, upload failures, or
    /// send failures. `width`/`height` of 0 are passed through unset.
    /// When `is_animated` is true the image is sent as a raw `m.image`
    /// event carrying the MSC4230 `org.matrix.msc4230.is_animated` flag and
    /// the `fi.mau.gif` vendor hint so animated GIFs/WebPs autoplay
    /// and loop on capable clients (the standard `send_attachment` path
    /// strips these fields).
    #[cfg(not(test))]
    pub fn send_image(
        &self,
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
        use matrix_sdk::attachment::{AttachmentInfo, BaseImageInfo};
        use matrix_sdk::ruma::UInt;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/image".to_string(),
        );

        // Animated GIF/WebP path: `send_attachment` strips the MSC4230
        // `is_animated` flag and the `fi.mau.gif` vendor hint, so we
        // upload the media ourselves and post a raw `m.image` event whose
        // `info` carries those fields. In encrypted rooms we encrypt the bytes
        // (EncryptedFile `file` block) instead of uploading them plaintext —
        // mirrors send_gif_video.
        if is_animated {
            use super::gif::GifMedia;
            let mime_owned = mime.clone();
            let mime_str = mime_type.to_owned();
            let filename = filename.to_owned();
            let caption = caption.to_owned();
            let reply_event_id = reply_event_id.to_owned();
            let thread_root = thread_root.to_owned();
            let size = bytes.len();
            let bytes_owned = bytes.to_vec();
            let result: Result<(), String> = self.rt.block_on(async move {
                let media = if room.encryption_state().is_encrypted() {
                    let mut cur = std::io::Cursor::new(bytes_owned);
                    let file = client
                        .upload_encrypted_file(&mut cur)
                        .await
                        .map_err(|e| e.to_string())?;
                    GifMedia::Encrypted(
                        serde_json::to_value(&file).map_err(|e| e.to_string())?,
                    )
                } else {
                    let mxc_uri =
                        super::account::upload_bytes(&client, bytes_owned, &mime_owned)
                            .await
                            .map_err(|e| e.to_string())?
                            .to_string();
                    GifMedia::Plain(mxc_uri)
                };
                let content = build_animated_image_content(
                    media,
                    &filename,
                    &caption,
                    &mime_str,
                    width,
                    height,
                    size,
                    &reply_event_id,
                    &thread_root,
                );
                room.send_raw("m.room.message", content)
                    .await
                    .map_err(|e| e.to_string())?;
                Ok(())
            });
            return match result {
                Ok(()) => ok(""),
                Err(e) => err(e),
            };
        }

        let info = AttachmentInfo::Image(BaseImageInfo {
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
        });
        let config = match build_attachment_config(info, None, caption, reply_event_id, thread_root)
        {
            Ok(c) => c,
            Err(e) => return err(e),
        };

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(do_send_attachment(room, filename, mime, data, config))
        {
            Ok(()) => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn send_image(
        &self,
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
        &self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentInfo, BaseFileInfo};
        use matrix_sdk::ruma::UInt;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/file".to_string(),
        );

        let info = AttachmentInfo::File(BaseFileInfo {
            size: UInt::new(bytes.len() as u64),
        });
        let config = match build_attachment_config(info, None, caption, reply_event_id, thread_root)
        {
            Ok(c) => c,
            Err(e) => return err(e),
        };

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(do_send_attachment(room, filename, mime, data, config))
        {
            Ok(()) => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn send_file(
        &self,
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
        &self,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        duration_ms: u64,
        reply_event_id: &str,
        thread_root: &str,
    ) -> OpResult {
        use matrix_sdk::attachment::{AttachmentInfo, BaseAudioInfo};
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/audio".to_string(),
        );

        let info = AttachmentInfo::Audio(BaseAudioInfo {
            duration: if duration_ms > 0 {
                Some(Duration::from_millis(duration_ms))
            } else {
                None
            },
            size: UInt::new(bytes.len() as u64),
            waveform: None,
        });
        let config = match build_attachment_config(info, None, caption, reply_event_id, thread_root)
        {
            Ok(c) => c,
            Err(e) => return err(e),
        };

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(do_send_attachment(room, filename, mime, data, config))
        {
            Ok(()) => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn send_audio(
        &self,
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
        &self,
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
        use matrix_sdk::attachment::{AttachmentInfo, BaseVideoInfo, Thumbnail};
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let mime: mime::Mime = match mime_type.parse() {
            Ok(m) => m,
            Err(e) => return err(format!("invalid mime: {e}")),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/video".to_string(),
        );

        let info = AttachmentInfo::Video(BaseVideoInfo {
            duration: if duration_ms > 0 {
                Some(Duration::from_millis(duration_ms))
            } else {
                None
            },
            height: UInt::new(height as u64),
            width: UInt::new(width as u64),
            size: UInt::new(bytes.len() as u64),
            blurhash: None,
        });
        let thumbnail = (!thumb_bytes.is_empty()).then(|| Thumbnail {
            data: thumb_bytes.to_vec(),
            content_type: mime::IMAGE_JPEG,
            height: UInt::new(thumb_height as u64).unwrap_or_default(),
            width: UInt::new(thumb_width as u64).unwrap_or_default(),
            size: UInt::new(thumb_bytes.len() as u64).unwrap_or_default(),
        });
        let config =
            match build_attachment_config(info, thumbnail, caption, reply_event_id, thread_root) {
                Ok(c) => c,
                Err(e) => return err(e),
            };

        let data = bytes.to_vec();
        let filename = filename.to_owned();

        match self
            .rt
            .block_on(do_send_attachment(room, filename, mime, data, config))
        {
            Ok(()) => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn send_video(
        &self,
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

    // ------------------------------------------------------------------
    // Non-blocking async upload variants
    // ------------------------------------------------------------------

    #[cfg(not(test))]
    pub fn send_image_async(
        &self,
        request_id: u64,
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
    ) {
        use matrix_sdk::attachment::{AttachmentInfo, BaseImageInfo};
        use matrix_sdk::ruma::UInt;

        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();

        let deliver = move |ok: bool, msg: &str| {
            if let Some(h) = &handler {
                { let g = h.lock(); g.on_upload_complete(request_id, ok, msg); }
            }
        };

        let room_id_str = room_id.to_owned();
        let bytes = bytes.to_vec();
        let mime_type = mime_type.to_owned();
        let filename = filename.to_owned();
        let caption = caption.to_owned();
        let reply_event_id = reply_event_id.to_owned();
        let thread_root = thread_root.to_owned();

        self.rt.spawn(async move {
            let (_, room) = match require_room(&client, &room_id_str) {
                Ok(v) => v,
                Err(e) => { deliver(false, &e.message); return; }
            };
            let mime: mime::Mime = match mime_type.parse() {
                Ok(m) => m,
                Err(e) => { deliver(false, &format!("invalid mime: {e}")); return; }
            };

            if is_animated {
                use super::gif::GifMedia;
                let mime_owned = mime.clone();
                let size = bytes.len();
                let result: Result<(), String> = async {
                    let media = if room.encryption_state().is_encrypted() {
                        let mut cur = std::io::Cursor::new(bytes.clone());
                        let file = client
                            .upload_encrypted_file(&mut cur)
                            .await
                            .map_err(|e| e.to_string())?;
                        GifMedia::Encrypted(
                            serde_json::to_value(&file).map_err(|e| e.to_string())?,
                        )
                    } else {
                        let mxc_uri = super::account::upload_bytes(&client, bytes.clone(), &mime_owned)
                            .await
                            .map_err(|e| e.to_string())?
                            .to_string();
                        GifMedia::Plain(mxc_uri)
                    };
                    let content = build_animated_image_content(
                        media, &filename, &caption, &mime_type, width, height, size,
                        &reply_event_id, &thread_root,
                    );
                    room.send_raw("m.room.message", content)
                        .await
                        .map_err(|e| e.to_string())?;
                    Ok(())
                }.await;
                match result {
                    Ok(()) => deliver(true, ""),
                    Err(e) => deliver(false, &e),
                }
                return;
            }

            let info = AttachmentInfo::Image(BaseImageInfo {
                width: UInt::new(width as u64),
                height: UInt::new(height as u64),
                size: UInt::new(bytes.len() as u64),
                blurhash: None,
                is_animated: None,
            });
            let config = match build_attachment_config(info, None, &caption, &reply_event_id, &thread_root) {
                Ok(c) => c,
                Err(e) => { deliver(false, &e); return; }
            };
            match do_send_attachment(room, filename, mime, bytes, config).await {
                Ok(()) => deliver(true, ""),
                Err(e) => deliver(false, &e),
            }
        });
    }

    #[cfg(test)]
    pub fn send_image_async(
        &self,
        _request_id: u64,
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
    ) {}

    #[cfg(not(test))]
    pub fn send_file_async(
        &self,
        request_id: u64,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        reply_event_id: &str,
        thread_root: &str,
    ) {
        use matrix_sdk::attachment::{AttachmentInfo, BaseFileInfo};
        use matrix_sdk::ruma::UInt;

        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();

        let deliver = move |ok: bool, msg: &str| {
            if let Some(h) = &handler {
                { let g = h.lock(); g.on_upload_complete(request_id, ok, msg); }
            }
        };

        let room_id_str = room_id.to_owned();
        let bytes = bytes.to_vec();
        let mime_type = mime_type.to_owned();
        let filename = filename.to_owned();
        let caption = caption.to_owned();
        let reply_event_id = reply_event_id.to_owned();
        let thread_root = thread_root.to_owned();

        self.rt.spawn(async move {
            let (_, room) = match require_room(&client, &room_id_str) {
                Ok(v) => v,
                Err(e) => { deliver(false, &e.message); return; }
            };
            let mime: mime::Mime = match mime_type.parse() {
                Ok(m) => m,
                Err(e) => { deliver(false, &format!("invalid mime: {e}")); return; }
            };
            let info = AttachmentInfo::File(BaseFileInfo { size: UInt::new(bytes.len() as u64) });
            let config = match build_attachment_config(info, None, &caption, &reply_event_id, &thread_root) {
                Ok(c) => c,
                Err(e) => { deliver(false, &e); return; }
            };
            match do_send_attachment(room, filename, mime, bytes, config).await {
                Ok(()) => deliver(true, ""),
                Err(e) => deliver(false, &e),
            }
        });
    }

    #[cfg(test)]
    pub fn send_file_async(
        &self,
        _request_id: u64,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _reply_event_id: &str,
        _thread_root: &str,
    ) {}

    #[cfg(not(test))]
    pub fn send_audio_async(
        &self,
        request_id: u64,
        room_id: &str,
        bytes: &[u8],
        mime_type: &str,
        filename: &str,
        caption: &str,
        duration_ms: u64,
        reply_event_id: &str,
        thread_root: &str,
    ) {
        use matrix_sdk::attachment::{AttachmentInfo, BaseAudioInfo};
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();

        let deliver = move |ok: bool, msg: &str| {
            if let Some(h) = &handler {
                { let g = h.lock(); g.on_upload_complete(request_id, ok, msg); }
            }
        };

        let room_id_str = room_id.to_owned();
        let bytes = bytes.to_vec();
        let mime_type = mime_type.to_owned();
        let filename = filename.to_owned();
        let caption = caption.to_owned();
        let reply_event_id = reply_event_id.to_owned();
        let thread_root = thread_root.to_owned();

        self.rt.spawn(async move {
            let (_, room) = match require_room(&client, &room_id_str) {
                Ok(v) => v,
                Err(e) => { deliver(false, &e.message); return; }
            };
            let mime: mime::Mime = match mime_type.parse() {
                Ok(m) => m,
                Err(e) => { deliver(false, &format!("invalid mime: {e}")); return; }
            };
            let info = AttachmentInfo::Audio(BaseAudioInfo {
                duration: if duration_ms > 0 { Some(Duration::from_millis(duration_ms)) } else { None },
                size: UInt::new(bytes.len() as u64),
                waveform: None,
            });
            let config = match build_attachment_config(info, None, &caption, &reply_event_id, &thread_root) {
                Ok(c) => c,
                Err(e) => { deliver(false, &e); return; }
            };
            match do_send_attachment(room, filename, mime, bytes, config).await {
                Ok(()) => deliver(true, ""),
                Err(e) => deliver(false, &e),
            }
        });
    }

    #[cfg(test)]
    pub fn send_audio_async(
        &self,
        _request_id: u64,
        _room_id: &str,
        _bytes: &[u8],
        _mime_type: &str,
        _filename: &str,
        _caption: &str,
        _duration_ms: u64,
        _reply_event_id: &str,
        _thread_root: &str,
    ) {}

    #[cfg(not(test))]
    pub fn send_video_async(
        &self,
        request_id: u64,
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
    ) {
        use matrix_sdk::attachment::{AttachmentInfo, BaseVideoInfo, Thumbnail};
        use matrix_sdk::ruma::UInt;
        use std::time::Duration;

        let Some(client) = self.client.clone() else { return; };
        let handler = self.handler.clone();

        let deliver = move |ok: bool, msg: &str| {
            if let Some(h) = &handler {
                { let g = h.lock(); g.on_upload_complete(request_id, ok, msg); }
            }
        };

        let room_id_str = room_id.to_owned();
        let bytes = bytes.to_vec();
        let thumb_bytes = thumb_bytes.to_vec();
        let mime_type = mime_type.to_owned();
        let filename = filename.to_owned();
        let caption = caption.to_owned();
        let reply_event_id = reply_event_id.to_owned();
        let thread_root = thread_root.to_owned();

        self.rt.spawn(async move {
            let (_, room) = match require_room(&client, &room_id_str) {
                Ok(v) => v,
                Err(e) => { deliver(false, &e.message); return; }
            };
            let mime: mime::Mime = match mime_type.parse() {
                Ok(m) => m,
                Err(e) => { deliver(false, &format!("invalid mime: {e}")); return; }
            };
            let info = AttachmentInfo::Video(BaseVideoInfo {
                duration: if duration_ms > 0 { Some(Duration::from_millis(duration_ms)) } else { None },
                height: UInt::new(height as u64),
                width: UInt::new(width as u64),
                size: UInt::new(bytes.len() as u64),
                blurhash: None,
            });
            let thumbnail = (!thumb_bytes.is_empty()).then(|| {
                // Capture the length before `data` moves thumb_bytes; matches the
                // blocking send_video path (which uses thumb_bytes.len()).
                let thumb_size = thumb_bytes.len() as u64;
                Thumbnail {
                    data: thumb_bytes,
                    content_type: mime::IMAGE_JPEG,
                    height: UInt::new(thumb_height as u64).unwrap_or_default(),
                    width: UInt::new(thumb_width as u64).unwrap_or_default(),
                    size: UInt::new(thumb_size).unwrap_or_default(),
                }
            });
            let config = match build_attachment_config(info, thumbnail, &caption, &reply_event_id, &thread_root) {
                Ok(c) => c,
                Err(e) => { deliver(false, &e); return; }
            };
            match do_send_attachment(room, filename, mime, bytes, config).await {
                Ok(()) => deliver(true, ""),
                Err(e) => deliver(false, &e),
            }
        });
    }

    #[cfg(test)]
    pub fn send_video_async(
        &self,
        _request_id: u64,
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
    ) {}

    /// Encode `pcm` (raw signed 16-bit mono 48 kHz samples as a byte slice,
    /// little-endian) into an Ogg/Opus stream and send it as an MSC3245
    /// `m.voice` event in `room_id`. `waveform` carries the MSC1767 waveform
    /// samples (clamped to 256 values of 0–1024; stored as f32 normalised to
    /// 0.0–1.0 in the audio info). `duration_ms` populates `info.duration`.
    /// `caption` / `reply_event_id` follow the same MSC2530 / m.in_reply_to
    /// framing as `send_image` and `send_file`.
    #[cfg(not(test))]
    pub fn send_voice(
        &self,
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
        if !pcm.len().is_multiple_of(2) {
            return err("PCM byte count must be even");
        }

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));

        // Decode the little-endian PCM byte stream into i16 samples. A
        // `Vec<u8>` only guarantees 1-byte alignment, so reinterpreting its
        // buffer as `*const i16` was undefined behavior (i16 needs 2-byte
        // alignment) and also assumed host endianness. `chunks_exact(2)` +
        // `from_le_bytes` is safe and endian-explicit; pcm has even length
        // (checked above) so there is no remainder.
        let samples: Vec<i16> = pcm
            .chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]))
            .collect();

        let ogg_bytes = match encode_voice_ogg(&samples, waveform, duration_ms) {
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
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/voice".to_string(),
        );

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
        &self,
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
    pub fn media_upload_limit(&self) -> u64 {
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
    pub fn media_upload_limit(&self) -> u64 {
        0
    }

    /// Toggle the current user's `key` reaction on `event_id` in `room_id`.
    /// First call adds the reaction; second redacts it. Requires that
    /// `room_id` is currently subscribed via `subscribe_room` — we look up
    /// its `Timeline` handle to invoke `toggle_reaction`.
    #[cfg(not(test))]
    pub fn send_reaction(&self, room_id: &str, event_id: &str, key: &str) -> OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }
        if key.is_empty() {
            return err("reaction key is empty");
        }

        let room_id = try_op!(parse_room_id(room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        // Thread replies live only in their thread Timeline (the room
        // Timeline filters them out), so we have to route the toggle to
        // whichever Timeline currently holds the event.
        let tl = match self
            .rt
            .block_on(async { self.timeline_for_event(&room_id, &event_id).await })
        {
            Some(tl) => tl,
            None => return err("event not found in any subscribed timeline"),
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
    pub fn send_reaction(&self, _room_id: &str, _event_id: &str, _key: &str) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_reaction_custom(
        &self,
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

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let event_id = event_id.to_owned();
        let key = key.to_owned();
        let shortcode = shortcode.to_owned();

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
        &self,
        _room_id: &str,
        _event_id: &str,
        _key: &str,
        _shortcode: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    pub fn send_read_receipt(&self, room_id: &str, event_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        match self.rt.block_on(send_both_receipts(&room, event_id)) {
            Ok(_) => ok(""),
            Err(e) => err(e.to_string()),
        }
    }

    #[cfg(test)]
    pub fn send_read_receipt(&self, _room_id: &str, _event_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Send public `m.read` and private `m.read.private` receipts for the
    /// latest cached event in `room_id`. Clears the unread count without
    /// requiring the room to be subscribed via `subscribe_room`.
    #[cfg(not(test))]
    pub fn mark_room_as_read(&self, room_id: &str) -> OpResult {
        let _enter = self.rt.enter();
        let Some(client) = self.client.as_ref() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(client, room_id));
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
    pub fn mark_room_as_read(&self, _room_id: &str) -> OpResult {
        err("not logged in")
    }

    /// Redact (delete) `event_id` in `room_id`. `reason` may be empty.
    /// Wraps matrix-sdk-ui's `Timeline::redact`. The room must currently
    /// be subscribed via `subscribe_room`. Server-side permission errors
    /// (e.g. trying to redact someone else's message without power) surface
    /// as `OpResult { ok: false, message: ... }`.
    #[cfg(not(test))]
    pub fn redact_event(&self, room_id: &str, event_id: &str, reason: &str) -> OpResult {
        if self.client.is_none() {
            return err("not logged in");
        }

        let room_id = try_op!(parse_room_id(room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };

        // Same Timeline-routing rule as `send_reaction`: in-thread replies
        // live in their thread Timeline only.
        let tl = match self
            .rt
            .block_on(async { self.timeline_for_event(&room_id, &event_id).await })
        {
            Some(tl) => tl,
            None => return err("event not found in any subscribed timeline"),
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
    pub fn redact_event(&self, _room_id: &str, _event_id: &str, _reason: &str) -> OpResult {
        err("not logged in")
    }

    /// Edit `event_id` in `room_id` replacing its body with `new_body`.
    /// Uses `Room::make_edit_event` (builds the `m.replace` Replacement
    /// relation) then sends via `RoomSendQueue`. Only own `m.text` events
    /// can be edited; the SDK returns an error for non-own or non-text
    /// events. Does not require `subscribe_room`.
    #[cfg(not(test))]
    pub fn send_edit(
        &self,
        room_id: &str,
        event_id: &str,
        new_body: &str,
        formatted_body: &str,
    ) -> OpResult {
        use matrix_sdk::room::edit::EditedContent;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
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
        &self,
        _room_id: &str,
        _event_id: &str,
        _new_body: &str,
        _formatted_body: &str,
    ) -> OpResult {
        err("not logged in")
    }

    #[cfg(not(test))]
    fn parse_image_info(info_json: &str) -> matrix_sdk::ruma::events::room::ImageInfo {
        use matrix_sdk::ruma::events::room::ImageInfo;
        if info_json.is_empty() || info_json == "{}" {
            ImageInfo::new()
        } else {
            serde_json::from_str(info_json).unwrap_or_else(|_| ImageInfo::new())
        }
    }

    /// Send an `m.sticker` event to `room_id`. Wraps
    /// `room.send(StickerEventContent { .. })`. matrix-sdk encrypts in E2EE
    /// rooms transparently; outgoing stickers always carry a plain
    /// `mxc://` source.
    #[cfg(not(test))]
    pub fn send_sticker(
        &self,
        room_id: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::sticker::{StickerEventContent, StickerMediaSource};
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, room) = try_op!(require_room(&client, room_id));

        if image_url.is_empty() {
            return err("image_url is empty");
        }
        let uri = OwnedMxcUri::from(image_url);
        if !uri.is_valid() {
            return err("image_url is not a valid mxc:// uri");
        }

        let info = Self::parse_image_info(info_json);

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
        &self,
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
        &self,
        room_id: &str,
        thread_root: &str,
        body: &str,
        image_url: &str,
        info_json: &str,
    ) -> OpResult {
        use matrix_sdk::ruma::events::sticker::StickerEventContent;
        use matrix_sdk::ruma::OwnedMxcUri;

        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let room_id_parsed = try_op!(parse_room_id(room_id));
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
        let info = Self::parse_image_info(info_json);

        // Prefer the subscribed thread timeline so the local echo fires via
        // on_thread_inserted (not the room timeline, which would show the
        // sticker in the main message list instead of the thread view).
        let key = (room_id_parsed.clone(), root.clone());
        // Clone the timeline Arc out from under the read guard so it is not held
        // across the `block_on` below.
        let thread_timeline =
            self.thread_timelines.read().get(&key).map(|h| h.timeline.clone());
        if let Some(timeline) = thread_timeline {
            let content = StickerEventContent::new(body.to_owned(), info, uri);
            return match self.rt.block_on(async move {
                timeline.send(content.into()).await
            }) {
                Ok(_) => ok(""),
                Err(e) => err(e.to_string()),
            };
        }

        // Fallback: send via room with manual m.thread relation.
        let room = try_op!(client.get_room(&room_id_parsed).ok_or_else(|| err("room not found")));
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
        &self,
        _room_id: &str,
        _thread_root: &str,
        _body: &str,
        _image_url: &str,
        _info_json: &str,
    ) -> OpResult {
        err("not logged in")
    }

    /// Forward `event_id` from `source_room_id` to `target_room_id`.
    /// Fetches the event (decrypting if the source room is E2EE), strips
    /// `m.relates_to` so the copy lands as a free-standing new event, then
    /// sends it to the target room. Works for all message-like event types.
    #[cfg(not(test))]
    pub fn forward_event(
        &self,
        source_room_id: &str,
        event_id: &str,
        target_room_id: &str,
    ) -> OpResult {
        let Some(client) = self.client.clone() else {
            return err("not logged in");
        };
        let (_, source_room) = try_op!(require_room(&client, source_room_id));
        let (_, target_room) = try_op!(require_room(&client, target_room_id));
        let event_id: matrix_sdk::ruma::OwnedEventId = match event_id.parse() {
            Ok(id) => id,
            Err(e) => return err(format!("invalid event id: {e}")),
        };
        let _guard = super::InFlightGuard::new(
            &self.in_flight,
            &self.handler,
            #[cfg(debug_assertions)] &self.in_flight_urls,
            #[cfg(debug_assertions)] "send/forward".to_string(),
        );
        match self.rt.block_on(async move {
            let tl_event = source_room
                .event(&event_id, None)
                .await
                .map_err(|e| e.to_string())?;
            let json_str = tl_event.raw().json().get();
            let mut envelope: serde_json::Value =
                serde_json::from_str(json_str).map_err(|e| e.to_string())?;
            let event_type = envelope["type"]
                .as_str()
                .ok_or_else(|| "missing event type".to_owned())?
                .to_owned();
            let mut content = envelope["content"].take();
            if let Some(obj) = content.as_object_mut() {
                obj.remove("m.relates_to");
            }
            target_room
                .send_raw(&event_type, content)
                .await
                .map_err(|e| e.to_string())
                .map(|_| ())
        }) {
            Ok(()) => ok(""),
            Err(e) => err(e),
        }
    }

    #[cfg(test)]
    pub fn forward_event(
        &self,
        _source_room_id: &str,
        _event_id: &str,
        _target_room_id: &str,
    ) -> OpResult {
        err("not logged in")
    }
}
