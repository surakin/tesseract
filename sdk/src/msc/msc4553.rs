//! MSC4553: forwarded messages.
//! <https://github.com/matrix-org/matrix-spec-proposals/pull/4553>
//!
//! A forward is a new `m.room.message` or `m.sticker` carrying the unstable
//! `org.matrix.msc4553.forwarded` object: a copy of the original `content`
//! plus optional `sender`, `room_id`, `event_id` and `origin_server_ts`. The
//! top-level content holds a plain/HTML "Forwarded from ..." fallback for
//! clients that don't know the MSC; it is left out for bridged rooms, where
//! the bridge would relay it as message text. Famedly's
//! `com.famedly.app.forwarded` is read as the same thing but never sent (it
//! lacks `content`).
//!
//! Pure functions over `serde_json::Value`: no client or room access. The
//! caller supplies the display name and permalinks in a [`FallbackCtx`].

use crate::text_utils::html_escape;
use ruma::events::room::message::sanitize::remove_plain_reply_fallback;
use ruma::html::remove_html_reply_fallback;
use serde_json::{json, Map, Value};

pub(crate) const UNSTABLE_KEY: &str = "org.matrix.msc4553.forwarded";
/// Read only, never sent.
pub(crate) const LEGACY_KEY: &str = "com.famedly.app.forwarded";
/// The key the MSC will use once stable. Not read or sent yet, but a forward
/// of a message that carries it must not copy it into its own content.
const STABLE_KEY: &str = "m.forwarded";

const HTML_FORMAT: &str = "org.matrix.custom.html";
/// Event content over this size drops the HTML fallback (the event limit is
/// 65536 bytes, which also covers the envelope).
const MAX_CONTENT_BYTES: usize = 60_000;

/// A parsed `forwarded` object.
pub(crate) struct Forwarded {
    /// The original content, `None` when it is not valid for the event type.
    pub content: Option<Value>,
    pub sender: Option<String>,
    pub room_id: Option<String>,
    pub event_id: Option<String>,
    pub origin_server_ts: Option<i64>,
}

/// Where a forward's original came from.
pub(crate) struct Origin {
    pub sender: Option<String>,
    pub room_id: Option<String>,
    pub event_id: Option<String>,
}

/// What the caller resolves for the fallback text: the original sender's
/// display name (or user ID) and the `matrix.to` links for the sender and the
/// original event. `None` leaves that part out of the fallback.
#[derive(Default)]
pub(crate) struct FallbackCtx {
    pub sender_label: Option<String>,
    pub sender_permalink: Option<String>,
    pub event_permalink: Option<String>,
}

/// The `forwarded` object of an event's `content`, from the unstable key
/// and then the legacy one. `event_type` is the (decrypted) event type, which
/// decides whether the object's `content` is valid.
pub(crate) fn parse(content: &Value, event_type: &str) -> Option<Forwarded> {
    let obj = forwarded_object(content)?;
    let string = |key: &str| obj.get(key).and_then(Value::as_str).map(str::to_owned);
    Some(Forwarded {
        content: obj
            .get("content")
            .filter(|c| is_valid_content(c, event_type))
            .cloned(),
        sender: string("sender"),
        room_id: string("room_id"),
        event_id: string("event_id"),
        origin_server_ts: obj.get("origin_server_ts").and_then(Value::as_i64),
    })
}

/// The content to render for an event: the forwarded content when valid,
/// else the event's own top-level content.
pub(crate) fn display_content<'a>(content: &'a Value, fwd: &'a Forwarded) -> &'a Value {
    fwd.content.as_ref().unwrap_or(content)
}

/// Whether `raw_json` (an event's JSON text) mentions a `forwarded` key at
/// all. A cheap check to skip parsing the JSON of the ordinary message.
pub(crate) fn might_be_forward(raw_json: &str) -> bool {
    raw_json.contains(UNSTABLE_KEY) || raw_json.contains(LEGACY_KEY)
}

/// Whether a text `body` could be the "Forwarded from ..." / "Forwarded message"
/// fallback of a forward. Matches on the word only, as other senders word the
/// fallback differently. A cheap check before looking at the raw event.
pub(crate) fn fallback_may_be_forward(body: &str) -> bool {
    body.contains("orwarded")
}

/// `event` as the original sender would have sent it, when it is a forward
/// with valid content: the same event with its content replaced by the
/// forwarded one. The relation and mentions inside it are dropped, as they
/// refer to the original's room and don't apply here. `None` for any other
/// event (including a forward that carries no valid content).
pub(crate) fn display_event(event: &Value) -> Option<Value> {
    let event_type = event["type"].as_str()?;
    let shown = parse(&event["content"], event_type)?.content?;
    let mut out = event.clone();
    out["content"] = without_relations(shown);
    Some(out)
}

/// `content` without its relation and mentions. In forwarded content they
/// refer to the original's room and don't apply to the forward, and left in
/// they can make the content fail to deserialize (an edit without its
/// replacement) or be taken for an edit.
pub(crate) fn without_relations(mut content: Value) -> Value {
    if let Some(obj) = content.as_object_mut() {
        obj.remove("m.relates_to");
        obj.remove("m.mentions");
    }
    content
}

/// The `body` of the forwarded content of an event's `content`, for a
/// one-line preview. `None` when it is not a forward with valid content.
pub(crate) fn display_body(content: &Value, event_type: &str) -> Option<String> {
    let fwd = parse(content, event_type)?;
    fwd.content.as_ref()?["body"].as_str().map(str::to_owned)
}

/// The sender and ids a forward of `envelope` points at: the original's when
/// `envelope` is itself a forward with valid content, else its own. The
/// caller resolves the name and permalinks for this to fill a [`FallbackCtx`].
pub(crate) fn origin_of(envelope: &Value) -> Origin {
    let event_type = envelope["type"].as_str().unwrap_or_default();
    if let Some(fwd) = parse(&envelope["content"], event_type).filter(|f| f.content.is_some()) {
        return Origin {
            sender: fwd.sender,
            room_id: fwd.room_id,
            event_id: fwd.event_id,
        };
    }
    let string = |key: &str| envelope[key].as_str().map(str::to_owned);
    Origin {
        sender: string("sender"),
        room_id: string("room_id"),
        event_id: string("event_id"),
    }
}

/// The event type and content to send to forward `envelope` (a full event as
/// fetched, already decrypted, carrying `room_id` and `event_id`).
///
/// A forward of a forward copies the existing `forwarded` object unchanged, so
/// it keeps pointing at the original; `fallback` must then describe that
/// original (see [`origin_of`]). Otherwise the object is built from the newest
/// same-sender edit of the event, with only the forwarded keys and the reply
/// fallback removed.
///
/// `fallback` `None` leaves the text fallback out, so the top level is just the
/// original content (an emote stays `m.emote`, media keep their own body and
/// file name) and only the `forwarded` object carries the attribution. Used for
/// bridged rooms: bridges ignore the `forwarded` object and relay the top-level
/// text, which would show the other network a "Forwarded from ..." header with
/// a `matrix.to` link.
pub(crate) fn build_forward(
    envelope: &Value,
    fallback: Option<&FallbackCtx>,
) -> Result<(String, Value), String> {
    let event_type = match envelope["type"].as_str() {
        Some(t @ ("m.room.message" | "m.sticker")) => t,
        Some("m.room.encrypted") => {
            return Err("this message could not be decrypted, so it can't be forwarded".into())
        }
        Some(other) => return Err(format!("{other} events can't be forwarded")),
        None => return Err("missing event type".into()),
    };

    let (src, forwarded) = match parse(&envelope["content"], event_type)
        .and_then(|f| Some((f.content?, forwarded_object(&envelope["content"])?.clone())))
    {
        Some(pair) => pair,
        None => {
            let src = original_content(envelope, event_type)?;
            let forwarded = forwarded_for(envelope, &src);
            (src, forwarded)
        }
    };

    let mut top: Map<String, Value> = src.as_object().cloned().unwrap_or_default();
    for key in [
        "m.relates_to",
        "m.mentions",
        "m.new_content",
        UNSTABLE_KEY,
        LEGACY_KEY,
        STABLE_KEY,
    ] {
        top.remove(key);
    }
    let has_html_fallback = match fallback {
        Some(ctx) => apply_fallback(&mut top, event_type, &src, ctx),
        None => false,
    };
    top.insert("m.mentions".into(), json!({}));
    top.insert(UNSTABLE_KEY.into(), forwarded);

    let mut content = Value::Object(top);
    if has_html_fallback && serialized_len(&content) > MAX_CONTENT_BYTES {
        if let Some(obj) = content.as_object_mut() {
            obj.remove("format");
            obj.remove("formatted_body");
        }
    }
    if serialized_len(&content) > MAX_CONTENT_BYTES {
        return Err("this message is too large to forward".into());
    }
    Ok((event_type.to_owned(), content))
}

/// The `forwarded` object itself, from the unstable key else the legacy one.
fn forwarded_object(content: &Value) -> Option<&Value> {
    [UNSTABLE_KEY, LEGACY_KEY]
        .into_iter()
        .filter_map(|key| content.get(key))
        .find(|v| v.is_object())
}

/// Whether `content` meets the content requirements of `event_type`.
fn is_valid_content(content: &Value, event_type: &str) -> bool {
    let is_str = |key: &str| content.get(key).is_some_and(Value::is_string);
    match event_type {
        "m.room.message" => is_str("msgtype") && is_str("body"),
        // A sticker's image is `file` when it is encrypted.
        "m.sticker" => is_str("body") && (is_str("url") || content["file"].is_object()),
        _ => false,
    }
}

/// The original content of a message that is not itself a forward: its
/// newest same-sender edit, minus any forwarded keys and the reply fallback.
fn original_content(envelope: &Value, event_type: &str) -> Result<Value, String> {
    let mut content = crate::event_json::latest_content(envelope);
    let replies = |c: &Value| c["m.relates_to"].get("m.in_reply_to").is_some();
    let is_reply = replies(&content) || replies(&envelope["content"]);
    if let Some(obj) = content.as_object_mut() {
        for key in [UNSTABLE_KEY, LEGACY_KEY, STABLE_KEY] {
            obj.remove(key);
        }
        if is_reply {
            strip_reply_fallback(obj);
        }
    }
    if is_valid_content(&content, event_type) {
        Ok(content)
    } else {
        Err("this message has no content that can be forwarded".into())
    }
}

fn strip_reply_fallback(obj: &mut Map<String, Value>) {
    if let Some(body) = obj.get("body").and_then(Value::as_str) {
        let stripped = remove_plain_reply_fallback(body).to_owned();
        obj.insert("body".into(), Value::String(stripped));
    }
    if obj.get("format").and_then(Value::as_str) == Some(HTML_FORMAT) {
        if let Some(html) = obj.get("formatted_body").and_then(Value::as_str) {
            let stripped = remove_html_reply_fallback(html);
            obj.insert("formatted_body".into(), Value::String(stripped));
        }
    }
}

/// The `forwarded` object for a fresh forward of `envelope`.
fn forwarded_for(envelope: &Value, content: &Value) -> Value {
    let mut obj = Map::new();
    obj.insert("content".into(), content.clone());
    for key in ["sender", "room_id", "event_id"] {
        if let Some(s) = envelope[key].as_str() {
            obj.insert(key.into(), Value::String(s.to_owned()));
        }
    }
    if let Some(ts) = envelope["origin_server_ts"].as_i64() {
        obj.insert("origin_server_ts".into(), json!(ts));
    }
    Value::Object(obj)
}

fn serialized_len(v: &Value) -> usize {
    serde_json::to_vec(v).map_or(usize::MAX, |b| b.len())
}

#[derive(PartialEq)]
enum Kind {
    /// `m.text` / `m.notice`: the body is the message.
    Text,
    Emote,
    /// Image, file, audio and video: the body is a caption or a file name.
    Media,
    /// Stickers, `m.location` and unknown types keep their own text.
    Other,
}

fn kind_of(event_type: &str, src: &Value) -> Kind {
    if event_type != "m.room.message" {
        return Kind::Other;
    }
    match src["msgtype"].as_str() {
        Some("m.text" | "m.notice") => Kind::Text,
        Some("m.emote") => Kind::Emote,
        Some("m.image" | "m.file" | "m.audio" | "m.video") => Kind::Media,
        _ => Kind::Other,
    }
}

/// Write the fallback text into `top` (a copy of `src` less its relations).
/// Returns whether an HTML fallback was written.
fn apply_fallback(
    top: &mut Map<String, Value>,
    event_type: &str,
    src: &Value,
    ctx: &FallbackCtx,
) -> bool {
    let kind = kind_of(event_type, src);
    if kind == Kind::Other {
        return false;
    }
    let body = src["body"].as_str().unwrap_or_default();
    let source_html = Some(src)
        .filter(|s| s["format"].as_str() == Some(HTML_FORMAT))
        .and_then(|s| s["formatted_body"].as_str())
        .filter(|h| !h.is_empty());

    // The text to quote: the message, or a media caption. A file name that
    // was sent as the body is not a caption.
    let (text, html) = match kind {
        Kind::Media => {
            let filename = src["filename"].as_str();
            top.insert("filename".into(), json!(filename.unwrap_or(body)));
            if filename.is_some_and(|f| f != body) {
                (body, source_html)
            } else {
                ("", None)
            }
        }
        _ => (body, source_html),
    };

    let (plain, quoted_html) = match (text.is_empty(), kind == Kind::Emote) {
        (true, _) => (None, None),
        (false, true) => {
            let name = ctx.sender_label.as_deref().unwrap_or("Sender");
            let name_html = match &ctx.sender_permalink {
                Some(link) if ctx.sender_label.is_some() => link_html(link, name),
                _ => html_escape(name),
            };
            (
                Some(format!("{name} {text}")),
                Some(format!(
                    "<strong>{name_html}</strong> {}",
                    html.map_or_else(|| text_html(text), str::to_owned)
                )),
            )
        }
        (false, false) => (
            Some(text.to_owned()),
            Some(html.map_or_else(|| text_html(text), str::to_owned)),
        ),
    };

    let mut header = String::from(match &ctx.sender_label {
        Some(_) => "Forwarded from ",
        None => "Forwarded message",
    });
    let mut header_html = header.clone();
    if let Some(label) = &ctx.sender_label {
        header.push_str(label);
        header_html.push_str(&match &ctx.sender_permalink {
            Some(link) => link_html(link, label),
            None => html_escape(label),
        });
    }
    if let Some(link) = &ctx.event_permalink {
        header.push_str(&format!(" - view original message: {link}"));
        header_html.push_str(&format!(" - {}", link_html(link, "view original message")));
    }

    let mut body_out = header;
    if let Some(p) = plain {
        body_out.push('\n');
        body_out.push_str(&p);
    }
    let mut html_out = format!("<strong>{header_html}</strong>");
    if let Some(h) = quoted_html {
        html_out.push_str(&format!("<br><blockquote>{h}</blockquote>"));
    }

    top.insert("body".into(), Value::String(body_out));
    top.insert("format".into(), json!(HTML_FORMAT));
    top.insert("formatted_body".into(), Value::String(html_out));
    if kind == Kind::Emote {
        top.insert("msgtype".into(), json!("m.text"));
    }
    true
}

fn link_html(href: &str, text: &str) -> String {
    format!(
        r#"<a href="{}">{}</a>"#,
        html_escape(href),
        html_escape(text)
    )
}

/// Plain text as HTML, with line breaks.
fn text_html(text: &str) -> String {
    html_escape(text).replace('\n', "<br>")
}

#[cfg(test)]
mod tests {
    use super::*;

    const ALICE: &str = "@alice:example.org";
    const ROOM: &str = "!room:example.org";
    const EVENT_LINK: &str = "https://matrix.to/#/!room:example.org/$e1?via=example.org";
    const ALICE_LINK: &str = "https://matrix.to/#/@alice:example.org";

    fn envelope(event_type: &str, content: Value) -> Value {
        json!({
            "type": event_type,
            "content": content,
            "sender": ALICE,
            "room_id": ROOM,
            "event_id": "$e1",
            "origin_server_ts": 1_700_000_000_000_i64,
        })
    }

    fn ctx() -> FallbackCtx {
        FallbackCtx {
            sender_label: Some("Alice".into()),
            sender_permalink: Some(ALICE_LINK.into()),
            event_permalink: Some(EVENT_LINK.into()),
        }
    }

    fn forward(env: &Value, ctx: &FallbackCtx) -> (String, Value) {
        build_forward(env, Some(ctx)).unwrap()
    }

    /// A forward with no text fallback, as sent into a bridged room.
    fn forward_bare(env: &Value) -> (String, Value) {
        build_forward(env, None).unwrap()
    }

    // ---- parse ----

    #[test]
    fn parse_reads_all_fields() {
        let content = json!({ "msgtype": "m.text", "body": "fallback", UNSTABLE_KEY: {
            "content": { "msgtype": "m.text", "body": "hi" },
            "sender": ALICE, "room_id": ROOM, "event_id": "$e1", "origin_server_ts": 5
        }});
        let f = parse(&content, "m.room.message").unwrap();
        assert_eq!(f.content.unwrap()["body"], "hi");
        assert_eq!(f.sender.as_deref(), Some(ALICE));
        assert_eq!(f.room_id.as_deref(), Some(ROOM));
        assert_eq!(f.event_id.as_deref(), Some("$e1"));
        assert_eq!(f.origin_server_ts, Some(5));
    }

    #[test]
    fn parse_is_none_without_the_key() {
        let content = json!({ "msgtype": "m.text", "body": "hi" });
        assert!(parse(&content, "m.room.message").is_none());
        let not_an_object = json!({ UNSTABLE_KEY: "nope" });
        assert!(parse(&not_an_object, "m.room.message").is_none());
    }

    #[test]
    fn parse_reads_the_legacy_key() {
        let content = json!({ "msgtype": "m.text", "body": "x", LEGACY_KEY: { "sender": ALICE } });
        let f = parse(&content, "m.room.message").unwrap();
        assert_eq!(f.sender.as_deref(), Some(ALICE));
        assert!(f.content.is_none());
    }

    #[test]
    fn parse_unstable_key_wins_over_legacy() {
        let content = json!({
            UNSTABLE_KEY: { "sender": "@new:x" },
            LEGACY_KEY: { "sender": "@old:x" }
        });
        let f = parse(&content, "m.room.message").unwrap();
        assert_eq!(f.sender.as_deref(), Some("@new:x"));
    }

    #[test]
    fn parse_drops_content_invalid_for_the_event_type() {
        let msg = |c: Value| json!({ UNSTABLE_KEY: { "content": c } });
        // m.room.message needs msgtype and body.
        assert!(parse(&msg(json!({ "body": "x" })), "m.room.message")
            .unwrap()
            .content
            .is_none());
        assert!(
            parse(&msg(json!({ "msgtype": "m.text" })), "m.room.message")
                .unwrap()
                .content
                .is_none()
        );
        assert!(parse(
            &msg(json!({ "msgtype": "m.text", "body": 3 })),
            "m.room.message"
        )
        .unwrap()
        .content
        .is_none());
        // A sticker's content is not valid for a message and vice versa.
        let sticker = json!({ "body": "s", "url": "mxc://x/y" });
        assert!(parse(&msg(sticker.clone()), "m.room.message")
            .unwrap()
            .content
            .is_none());
        assert!(parse(&msg(sticker), "m.sticker").unwrap().content.is_some());
        let encrypted = json!({ "body": "s", "file": { "url": "mxc://x/y" } });
        assert!(parse(&msg(encrypted), "m.sticker")
            .unwrap()
            .content
            .is_some());
        assert!(parse(&msg(json!({ "body": "s" })), "m.sticker")
            .unwrap()
            .content
            .is_none());
    }

    #[test]
    fn display_content_prefers_valid_forwarded_content() {
        let top = json!({ "msgtype": "m.text", "body": "fallback", UNSTABLE_KEY: {
            "content": { "msgtype": "m.text", "body": "real" }
        }});
        let f = parse(&top, "m.room.message").unwrap();
        assert_eq!(display_content(&top, &f)["body"], "real");

        let bad = json!({ "msgtype": "m.text", "body": "fallback", UNSTABLE_KEY: {
            "content": { "body": "no msgtype" }
        }});
        let f = parse(&bad, "m.room.message").unwrap();
        assert_eq!(display_content(&bad, &f)["body"], "fallback");
    }

    #[test]
    fn origin_of_follows_a_forward_to_its_original() {
        let plain = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x" }),
        );
        let o = origin_of(&plain);
        assert_eq!(o.sender.as_deref(), Some(ALICE));
        assert_eq!(o.room_id.as_deref(), Some(ROOM));
        assert_eq!(o.event_id.as_deref(), Some("$e1"));

        let fwd = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x", UNSTABLE_KEY: {
                "content": { "msgtype": "m.text", "body": "x" },
                "sender": "@bob:other.org", "event_id": "$orig"
            }}),
        );
        let o = origin_of(&fwd);
        assert_eq!(o.sender.as_deref(), Some("@bob:other.org"));
        assert_eq!(o.room_id, None);
        assert_eq!(o.event_id.as_deref(), Some("$orig"));

        // A forward whose content is invalid is treated as a plain message.
        let invalid = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x", UNSTABLE_KEY: { "sender": "@bob:other.org" } }),
        );
        assert_eq!(origin_of(&invalid).sender.as_deref(), Some(ALICE));
    }

    // ---- build_forward: text ----

    #[test]
    fn text_with_full_metadata() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "Hello world" }),
        );
        let (ty, c) = forward(&env, &ctx());
        assert_eq!(ty, "m.room.message");
        assert_eq!(c["msgtype"], "m.text");
        assert_eq!(
            c["body"],
            format!("Forwarded from Alice - view original message: {EVENT_LINK}\nHello world")
        );
        assert_eq!(c["format"], HTML_FORMAT);
        assert_eq!(
            c["formatted_body"],
            format!(
                "<strong>Forwarded from <a href=\"{ALICE_LINK}\">Alice</a> - \
                 <a href=\"{EVENT_LINK}\">view original message</a></strong>\
                 <br><blockquote>Hello world</blockquote>"
            )
        );
        assert_eq!(c["m.mentions"], json!({}));
        assert_eq!(
            c[UNSTABLE_KEY],
            json!({
                "content": { "msgtype": "m.text", "body": "Hello world" },
                "sender": ALICE,
                "room_id": ROOM,
                "event_id": "$e1",
                "origin_server_ts": 1_700_000_000_000_i64,
            })
        );
    }

    #[test]
    fn text_keeps_its_own_html_in_the_quote() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.notice", "body": "hi *you*", "format": HTML_FORMAT,
                    "formatted_body": "hi <em>you</em>" }),
        );
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c["msgtype"], "m.notice");
        assert!(c["formatted_body"]
            .as_str()
            .unwrap()
            .ends_with("<br><blockquote>hi <em>you</em></blockquote>"));
        // The copy keeps the source HTML untouched.
        assert_eq!(
            c[UNSTABLE_KEY]["content"]["formatted_body"],
            "hi <em>you</em>"
        );
    }

    #[test]
    fn multiline_text_becomes_html_line_breaks() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "a <b>\nb" }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(c["formatted_body"]
            .as_str()
            .unwrap()
            .ends_with("<blockquote>a &lt;b&gt;<br>b</blockquote>"));
    }

    #[test]
    fn content_only_has_no_sender_or_ids() {
        let env = json!({
            "type": "m.room.message",
            "content": { "msgtype": "m.text", "body": "Hello" }
        });
        let (_, c) = forward(&env, &FallbackCtx::default());
        assert_eq!(c["body"], "Forwarded message\nHello");
        assert_eq!(
            c["formatted_body"],
            "<strong>Forwarded message</strong><br><blockquote>Hello</blockquote>"
        );
        assert_eq!(
            c[UNSTABLE_KEY],
            json!({ "content": { "msgtype": "m.text", "body": "Hello" } })
        );
    }

    #[test]
    fn sender_without_ids_and_ids_without_sender() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x" }),
        );
        let sender_only = FallbackCtx {
            event_permalink: None,
            ..ctx()
        };
        let (_, c) = forward(&env, &sender_only);
        assert_eq!(c["body"], "Forwarded from Alice\nx");
        assert_eq!(
            c["formatted_body"],
            format!(
                "<strong>Forwarded from <a href=\"{ALICE_LINK}\">Alice</a></strong>\
                 <br><blockquote>x</blockquote>"
            )
        );

        let ids_only = FallbackCtx {
            sender_label: None,
            sender_permalink: None,
            ..ctx()
        };
        let (_, c) = forward(&env, &ids_only);
        assert_eq!(
            c["body"],
            format!("Forwarded message - view original message: {EVENT_LINK}\nx")
        );
        assert_eq!(
            c["formatted_body"],
            format!(
                "<strong>Forwarded message - <a href=\"{EVENT_LINK}\">view original message</a>\
                 </strong><br><blockquote>x</blockquote>"
            )
        );
    }

    #[test]
    fn empty_text_has_no_quote_line() {
        let env = envelope("m.room.message", json!({ "msgtype": "m.text", "body": "" }));
        let (_, c) = forward(&env, &FallbackCtx::default());
        assert_eq!(c["body"], "Forwarded message");
        assert_eq!(c["formatted_body"], "<strong>Forwarded message</strong>");
    }

    // ---- emote ----

    #[test]
    fn emote_is_sent_as_text_but_keeps_emote_inside() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.emote", "body": "waves" }),
        );
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c["msgtype"], "m.text");
        assert_eq!(
            c["body"],
            format!("Forwarded from Alice - view original message: {EVENT_LINK}\nAlice waves")
        );
        assert_eq!(
            c["formatted_body"],
            format!(
                "<strong>Forwarded from <a href=\"{ALICE_LINK}\">Alice</a> - \
                 <a href=\"{EVENT_LINK}\">view original message</a></strong>\
                 <br><blockquote><strong><a href=\"{ALICE_LINK}\">Alice</a></strong> waves\
                 </blockquote>"
            )
        );
        assert_eq!(c[UNSTABLE_KEY]["content"]["msgtype"], "m.emote");
    }

    #[test]
    fn emote_without_sender_uses_the_word_sender() {
        let env = json!({
            "type": "m.room.message",
            "content": { "msgtype": "m.emote", "body": "waves" }
        });
        let (_, c) = forward(&env, &FallbackCtx::default());
        assert_eq!(c["msgtype"], "m.text");
        assert_eq!(c["body"], "Forwarded message\nSender waves");
        assert_eq!(
            c["formatted_body"],
            "<strong>Forwarded message</strong>\
             <br><blockquote><strong>Sender</strong> waves</blockquote>"
        );
    }

    // ---- media ----

    #[test]
    fn file_without_caption_quotes_nothing() {
        // Body is the file name and no `filename`: not a caption.
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.file", "body": "report.pdf", "url": "mxc://x/y",
                    "info": { "mimetype": "application/pdf" } }),
        );
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c["msgtype"], "m.file");
        assert_eq!(c["filename"], "report.pdf");
        assert_eq!(
            c["body"],
            format!("Forwarded from Alice - view original message: {EVENT_LINK}")
        );
        assert!(!c["formatted_body"].as_str().unwrap().contains("blockquote"));
        assert_eq!(c["url"], "mxc://x/y");
        assert_eq!(c["info"]["mimetype"], "application/pdf");
        // `filename` is a top-level addition; the copy is the original.
        assert!(c[UNSTABLE_KEY]["content"].get("filename").is_none());
    }

    #[test]
    fn file_whose_filename_equals_body_has_no_caption() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.image", "body": "a.png", "filename": "a.png", "url": "mxc://x/y" }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(!c["body"].as_str().unwrap().contains('\n'));
    }

    #[test]
    fn file_with_caption_quotes_the_caption() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.image", "body": "look at this", "filename": "cat.png",
                    "format": HTML_FORMAT, "formatted_body": "look at <b>this</b>",
                    "file": { "url": "mxc://x/y", "key": { "k": "abc" } } }),
        );
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c["filename"], "cat.png");
        assert_eq!(
            c["body"],
            format!("Forwarded from Alice - view original message: {EVENT_LINK}\nlook at this")
        );
        assert!(c["formatted_body"]
            .as_str()
            .unwrap()
            .ends_with("<br><blockquote>look at <b>this</b></blockquote>"));
        // Encrypted media fields are copied as-is.
        assert_eq!(c["file"]["key"]["k"], "abc");
    }

    // ---- build_forward: no text fallback (bridged rooms) ----

    #[test]
    fn bare_text_keeps_the_original_top_level() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.notice", "body": "hi *you*", "format": HTML_FORMAT,
                    "formatted_body": "hi <em>you</em>" }),
        );
        let (ty, c) = forward_bare(&env);
        assert_eq!(ty, "m.room.message");
        assert_eq!(c["msgtype"], "m.notice");
        assert_eq!(c["body"], "hi *you*");
        assert_eq!(c["format"], HTML_FORMAT);
        assert_eq!(c["formatted_body"], "hi <em>you</em>");
        assert_eq!(c["m.mentions"], json!({}));
        assert_eq!(
            c[UNSTABLE_KEY],
            json!({
                "content": { "msgtype": "m.notice", "body": "hi *you*", "format": HTML_FORMAT,
                             "formatted_body": "hi <em>you</em>" },
                "sender": ALICE,
                "room_id": ROOM,
                "event_id": "$e1",
                "origin_server_ts": 1_700_000_000_000_i64,
            })
        );
        assert!(!c.to_string().contains("Forwarded"));
        assert!(!c.to_string().contains("matrix.to"));
    }

    #[test]
    fn bare_plain_text_gets_no_html() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "Hello" }),
        );
        let (_, c) = forward_bare(&env);
        assert_eq!(c["body"], "Hello");
        assert!(c.get("format").is_none());
        assert!(c.get("formatted_body").is_none());
    }

    #[test]
    fn bare_emote_stays_an_emote() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.emote", "body": "waves" }),
        );
        let (_, c) = forward_bare(&env);
        assert_eq!(c["msgtype"], "m.emote");
        assert_eq!(c["body"], "waves");
        assert!(c.get("formatted_body").is_none());
        assert_eq!(c[UNSTABLE_KEY]["content"]["msgtype"], "m.emote");
    }

    #[test]
    fn bare_media_keeps_its_body_and_file_name() {
        // A caption, with the file name in `filename`.
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.image", "body": "look at this", "filename": "cat.png",
                    "file": { "url": "mxc://x/y", "key": { "k": "abc" } } }),
        );
        let (_, c) = forward_bare(&env);
        assert_eq!(c["msgtype"], "m.image");
        assert_eq!(c["body"], "look at this");
        assert_eq!(c["filename"], "cat.png");
        assert_eq!(c["file"]["key"]["k"], "abc");
        assert!(c.get("format").is_none());
        assert!(c.get("formatted_body").is_none());

        // No caption, and no `filename`: none is added.
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.file", "body": "report.pdf", "url": "mxc://x/y" }),
        );
        let (_, c) = forward_bare(&env);
        assert_eq!(c["body"], "report.pdf");
        assert!(c.get("filename").is_none());
        assert!(c.get("format").is_none());
        assert!(c.get("formatted_body").is_none());
        assert_eq!(c["url"], "mxc://x/y");
    }

    #[test]
    fn bare_sticker_is_the_original() {
        let env = envelope(
            "m.sticker",
            json!({ "body": "Waving", "url": "mxc://x/y", "info": { "w": 100 } }),
        );
        let (ty, c) = forward_bare(&env);
        assert_eq!(ty, "m.sticker");
        assert_eq!(c["body"], "Waving");
        assert_eq!(c["url"], "mxc://x/y");
        assert!(c.get("format").is_none());
        assert_eq!(c["m.mentions"], json!({}));
        assert_eq!(c[UNSTABLE_KEY]["content"]["url"], "mxc://x/y");
    }

    #[test]
    fn bare_forward_of_a_forward_uses_the_inner_content() {
        let object = json!({
            "content": { "msgtype": "m.text", "body": "orig", "x.custom": 1 },
            "sender": "@bob:other.org",
            "room_id": "!other:other.org",
            "event_id": "$orig",
            "origin_server_ts": 42
        });
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text",
                    "body": "Forwarded from Bob - view original message: https://matrix.to/#/!other:other.org/$orig\norig",
                    "format": HTML_FORMAT,
                    "formatted_body": "<strong>Forwarded from Bob</strong><br><blockquote>orig</blockquote>",
                    UNSTABLE_KEY: object }),
        );
        let (_, c) = forward_bare(&env);
        // The top level is the original, not the previous forwarder's fallback.
        assert_eq!(c["body"], "orig");
        assert_eq!(c["x.custom"], 1);
        assert!(c.get("format").is_none());
        assert!(c.get("formatted_body").is_none());
        assert_eq!(c[UNSTABLE_KEY], object);
    }

    #[test]
    fn bare_reply_has_its_fallback_and_relation_removed() {
        let env = envelope(
            "m.room.message",
            json!({
                "msgtype": "m.text",
                "body": "> <@bob:x> earlier\n\nmy answer",
                "format": HTML_FORMAT,
                "formatted_body": "<mx-reply><blockquote>earlier</blockquote></mx-reply>my answer",
                "m.relates_to": { "m.in_reply_to": { "event_id": "$prev" } }
            }),
        );
        let (_, c) = forward_bare(&env);
        assert_eq!(c["body"], "my answer");
        assert_eq!(c["formatted_body"], "my answer");
        assert!(c.get("m.relates_to").is_none());
        assert_eq!(c[UNSTABLE_KEY]["content"]["body"], "my answer");
    }

    #[test]
    fn bare_oversized_content_errors() {
        // No HTML fallback to drop: too large is an error straight away.
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "a".repeat(35_000) }),
        );
        assert_eq!(
            build_forward(&env, None).unwrap_err(),
            "this message is too large to forward"
        );
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "a".repeat(25_000) }),
        );
        assert!(build_forward(&env, None).is_ok());
    }

    // ---- sticker and other types ----

    #[test]
    fn sticker_has_no_fallback() {
        let env = envelope(
            "m.sticker",
            json!({ "body": "Waving", "url": "mxc://x/y", "info": { "w": 100, "h": 100 } }),
        );
        let (ty, c) = forward(&env, &ctx());
        assert_eq!(ty, "m.sticker");
        assert_eq!(c["body"], "Waving");
        assert_eq!(c["url"], "mxc://x/y");
        assert_eq!(c["info"]["w"], 100);
        assert!(c.get("format").is_none());
        assert!(c.get("formatted_body").is_none());
        assert!(c.get("filename").is_none());
        assert_eq!(c["m.mentions"], json!({}));
        assert_eq!(c[UNSTABLE_KEY]["content"]["url"], "mxc://x/y");
        assert_eq!(c[UNSTABLE_KEY]["event_id"], "$e1");
    }

    #[test]
    fn location_keeps_its_body() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.location", "body": "Cafe", "geo_uri": "geo:1,2" }),
        );
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c["msgtype"], "m.location");
        assert_eq!(c["body"], "Cafe");
        assert_eq!(c["geo_uri"], "geo:1,2");
        assert!(c.get("formatted_body").is_none());
        assert!(c.get("format").is_none());
    }

    #[test]
    fn unsupported_types_are_refused() {
        let enc = json!({ "type": "m.room.encrypted", "content": {} });
        assert!(build_forward(&enc, Some(&ctx())).is_err());
        let poll = json!({ "type": "m.poll.start", "content": {} });
        assert!(build_forward(&poll, Some(&ctx())).is_err());
        let bad = envelope("m.room.message", json!({ "body": "no msgtype" }));
        assert!(build_forward(&bad, Some(&ctx())).is_err());
    }

    // ---- forwards of forwards ----

    #[test]
    fn forward_of_a_forward_copies_the_object_unchanged() {
        let object = json!({
            "content": { "msgtype": "m.text", "body": "orig", "x.custom": 1 },
            "sender": "@bob:other.org",
            "room_id": "!other:other.org",
            "event_id": "$orig",
            "origin_server_ts": 42,
            "x.extra": true
        });
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "Forwarded from Bob\norig", UNSTABLE_KEY: object }),
        );
        let ctx = FallbackCtx {
            sender_label: Some("Bob".into()),
            sender_permalink: Some("https://matrix.to/#/@bob:other.org".into()),
            event_permalink: Some("https://matrix.to/#/!other:other.org/$orig".into()),
        };
        let (_, c) = forward(&env, &ctx);
        assert_eq!(c[UNSTABLE_KEY], object);
        assert_eq!(
            c["body"],
            "Forwarded from Bob - view original message: https://matrix.to/#/!other:other.org/$orig\norig"
        );
    }

    #[test]
    fn forward_with_invalid_content_is_rebuilt_from_the_event() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "top level", UNSTABLE_KEY: {
                "content": { "body": "no msgtype" }, "sender": "@bob:other.org" } }),
        );
        let (_, c) = forward(&env, &ctx());
        let object = &c[UNSTABLE_KEY];
        assert_eq!(
            object["content"],
            json!({ "msgtype": "m.text", "body": "top level" })
        );
        assert_eq!(object["sender"], ALICE);
    }

    #[test]
    fn legacy_key_is_stripped_and_never_emitted() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "hi", LEGACY_KEY: { "sender": "@bob:x" },
                    STABLE_KEY: { "content": {} } }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(c.get(LEGACY_KEY).is_none());
        assert!(c.get(STABLE_KEY).is_none());
        assert!(c[UNSTABLE_KEY]["content"].get(LEGACY_KEY).is_none());
        assert!(c[UNSTABLE_KEY]["content"].get(STABLE_KEY).is_none());
        assert_eq!(c[UNSTABLE_KEY]["sender"], ALICE);

        // A legacy object that does carry valid content is re-emitted under
        // the unstable key.
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "hi", LEGACY_KEY: {
                "content": { "msgtype": "m.text", "body": "orig" }, "sender": "@bob:x" } }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(c.get(LEGACY_KEY).is_none());
        assert_eq!(c[UNSTABLE_KEY]["sender"], "@bob:x");
        assert_eq!(c[UNSTABLE_KEY]["content"]["body"], "orig");
    }

    // ---- content handling ----

    #[test]
    fn reply_fallback_is_stripped_from_the_copy() {
        let env = envelope(
            "m.room.message",
            json!({
                "msgtype": "m.text",
                "body": "> <@bob:x> earlier\n\nmy answer",
                "format": HTML_FORMAT,
                "formatted_body": "<mx-reply><blockquote>earlier</blockquote></mx-reply>my answer",
                "m.relates_to": { "m.in_reply_to": { "event_id": "$prev" } }
            }),
        );
        let (_, c) = forward(&env, &ctx());
        let inner = &c[UNSTABLE_KEY]["content"];
        assert_eq!(inner["body"], "my answer");
        assert_eq!(inner["formatted_body"], "my answer");
        assert!(c["body"].as_str().unwrap().ends_with("\nmy answer"));
        assert!(!c["body"].as_str().unwrap().contains("earlier"));
    }

    #[test]
    fn a_quote_that_is_not_a_reply_is_left_alone() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "> <@bob:x> hi\n\nI agree" }),
        );
        let (_, c) = forward(&env, &ctx());
        assert_eq!(
            c[UNSTABLE_KEY]["content"]["body"],
            "> <@bob:x> hi\n\nI agree"
        );
    }

    #[test]
    fn relations_mentions_and_new_content_are_dropped_from_the_top_level() {
        let env = envelope(
            "m.room.message",
            json!({
                "msgtype": "m.text", "body": "hi",
                "m.mentions": { "user_ids": ["@carol:x"] },
                "m.relates_to": { "rel_type": "m.thread", "event_id": "$root" },
                "m.new_content": { "msgtype": "m.text", "body": "x" }
            }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(c.get("m.relates_to").is_none());
        assert!(c.get("m.new_content").is_none());
        assert_eq!(c["m.mentions"], json!({}));
    }

    #[test]
    fn edited_source_forwards_the_latest_same_sender_edit() {
        let mut env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "old" }),
        );
        env["unsigned"] = json!({ "m.relations": { "m.replace": { "sender": ALICE, "content": {
            "m.new_content": { "msgtype": "m.text", "body": "newest" } } } } });
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c[UNSTABLE_KEY]["content"]["body"], "newest");
        assert!(c["body"].as_str().unwrap().ends_with("\nnewest"));

        // An edit by anyone else is not an edit.
        env["unsigned"]["m.relations"]["m.replace"]["sender"] = json!("@evil:x");
        let (_, c) = forward(&env, &ctx());
        assert_eq!(c[UNSTABLE_KEY]["content"]["body"], "old");
    }

    // ---- escaping and size ----

    #[test]
    fn sender_name_and_links_are_html_escaped() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x" }),
        );
        let ctx = FallbackCtx {
            sender_label: Some(r#"<b>A&"l"</b>"#.into()),
            sender_permalink: Some(r#"https://x/?a=1&b="2""#.into()),
            event_permalink: Some("https://x/?via=a&via=b".into()),
        };
        let (_, c) = forward(&env, &ctx);
        let html = c["formatted_body"].as_str().unwrap();
        assert!(html.contains(
            r#"<a href="https://x/?a=1&amp;b=&quot;2&quot;">&lt;b&gt;A&amp;&quot;l&quot;&lt;/b&gt;</a>"#
        ));
        assert!(html.contains(r#"href="https://x/?via=a&amp;via=b""#));
        // The plain fallback is not escaped.
        assert!(c["body"]
            .as_str()
            .unwrap()
            .starts_with(r#"Forwarded from <b>A&"l"</b> - "#));
    }

    #[test]
    fn oversized_content_drops_the_html_fallback_then_errors() {
        // The text appears in the plain fallback, the HTML fallback and the
        // forwarded copy; without the HTML copy it fits.
        let text = "a".repeat(18_000);
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": text }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(c.get("formatted_body").is_some());

        let text = "a".repeat(21_000);
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": text }),
        );
        let (_, c) = forward(&env, &ctx());
        assert!(c.get("formatted_body").is_none());
        assert!(c.get("format").is_none());
        assert!(serialized_len(&c) <= MAX_CONTENT_BYTES);
        assert_eq!(
            c[UNSTABLE_KEY]["content"]["body"].as_str().unwrap().len(),
            21_000
        );

        let text = "a".repeat(30_000);
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": text }),
        );
        assert_eq!(
            build_forward(&env, Some(&ctx())).unwrap_err(),
            "this message is too large to forward"
        );
    }

    // ---- receive-side helpers ----

    #[test]
    fn might_be_forward_spots_either_key() {
        assert!(might_be_forward(&format!(
            r#"{{"content":{{"{UNSTABLE_KEY}":{{}}}}}}"#
        )));
        assert!(might_be_forward(&format!(
            r#"{{"content":{{"{LEGACY_KEY}":{{}}}}}}"#
        )));
        assert!(!might_be_forward(r#"{"content":{"body":"hi"}}"#));
    }

    #[test]
    fn display_event_swaps_in_the_forwarded_content() {
        let env = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "Forwarded from Bob\nhi", UNSTABLE_KEY: {
                "content": { "msgtype": "m.emote", "body": "waves",
                             "m.relates_to": { "rel_type": "m.replace", "event_id": "$x" },
                             "m.mentions": { "room": true } },
                "sender": "@bob:other.org"
            }}),
        );
        let shown = display_event(&env).unwrap();
        assert_eq!(shown["content"]["msgtype"], "m.emote");
        assert_eq!(shown["content"]["body"], "waves");
        // Relation and mentions of the original don't apply here.
        assert!(shown["content"].get("m.relates_to").is_none());
        assert!(shown["content"].get("m.mentions").is_none());
        // The rest of the event is the forward's own.
        assert_eq!(shown["sender"], ALICE);
        assert_eq!(shown["event_id"], "$e1");
    }

    #[test]
    fn display_event_is_none_without_valid_forwarded_content() {
        let plain = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x" }),
        );
        assert!(display_event(&plain).is_none());
        let legacy = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x", LEGACY_KEY: { "sender": "@bob:x" } }),
        );
        assert!(display_event(&legacy).is_none());
        let invalid = envelope(
            "m.room.message",
            json!({ "msgtype": "m.text", "body": "x", UNSTABLE_KEY: { "content": { "body": "y" } } }),
        );
        assert!(display_event(&invalid).is_none());
    }

    #[test]
    fn display_body_is_the_forwarded_body() {
        let content = json!({ "msgtype": "m.text", "body": "Forwarded from Bob\nhi", UNSTABLE_KEY: {
            "content": { "msgtype": "m.text", "body": "hi" }
        }});
        assert_eq!(
            display_body(&content, "m.room.message").as_deref(),
            Some("hi")
        );
        let plain = json!({ "msgtype": "m.text", "body": "hi" });
        assert_eq!(display_body(&plain, "m.room.message"), None);
        // Valid for a message is not valid for a sticker.
        assert_eq!(display_body(&content, "m.sticker"), None);
    }

    #[test]
    fn fallback_text_is_recognized_by_its_word() {
        assert!(fallback_may_be_forward(
            "Forwarded from Bob - view original message: https://matrix.to/#/x"
        ));
        assert!(fallback_may_be_forward("Forwarded message\nhi"));
        assert!(fallback_may_be_forward("message forwarded by Bob"));
        assert!(!fallback_may_be_forward("hello there"));
        assert!(!fallback_may_be_forward(""));
    }
}
