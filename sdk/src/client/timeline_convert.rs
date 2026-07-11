//! Convert matrix-sdk-ui timeline items into the FFI `TimelineEvent`
//! struct consumed by the C++ bridge. Includes the per-msgtype dispatcher
//! and supporting helpers (media-source splitting, reaction/receipt
//! aggregation, embedded-event preview, geo URI parsing).
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

#[cfg(not(test))]
use crate::ffi::{ReactionGroup, ReadReceipt, TimelineEvent};

#[cfg(not(test))]
use matrix_sdk::{ruma::UserId, Room};

#[cfg(not(test))]
use matrix_sdk_ui::timeline::{
    EncryptedMessage, EventSendState, MsgLikeContent, MsgLikeKind, TimelineDetails, TimelineItem,
    TimelineItemContent, TimelineItemKind, VirtualTimelineItem,
};

#[cfg(not(test))]
use std::sync::Arc;

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

/// Zero-valued `TimelineEvent` used as the base for struct update syntax.
/// Every construction site only needs to list the fields that differ from zero.
#[cfg(not(test))]
pub(super) fn ffi_event_defaults() -> TimelineEvent {
    TimelineEvent {
        event_id: String::new(),
        room_id: String::new(),
        sender: String::new(),
        sender_name: String::new(),
        sender_avatar_url: String::new(),
        body: String::new(),
        timestamp: 0,
        msg_type: String::new(),
        source_url: String::new(),
        source_encrypted_json: String::new(),
        width: 0,
        height: 0,
        file_url: String::new(),
        file_encrypted_json: String::new(),
        file_name: String::new(),
        file_size: 0,
        file_filename: String::new(),
        image_filename: String::new(),
        audio_url: String::new(),
        audio_encrypted_json: String::new(),
        audio_duration_ms: 0,
        audio_waveform: Vec::new(),
        audio_mime: String::new(),
        video_thumbnail_url: String::new(),
        video_thumbnail_encrypted_json: String::new(),
        image_thumbnail_url: String::new(),
        image_thumbnail_encrypted_json: String::new(),
        video_duration_ms: 0,
        video_mime: String::new(),
        video_autoplay: false,
        video_loop: false,
        video_no_audio: false,
        video_hide_controls: false,
        video_gif: false,
        reactions: Vec::new(),
        read_receipts: Vec::new(),
        in_reply_to_id: String::new(),
        in_reply_to_sender_name: String::new(),
        in_reply_to_body: String::new(),
        in_reply_to_image_url: String::new(),
        in_reply_to_image_encrypted_json: String::new(),
        is_edited: false,
        formatted_body: String::new(),
        blurhash: String::new(),
        sticker_info_json: String::new(),
        image_animated: false,
        pending_state: String::new(),
        pending_error: String::new(),
        pending_recoverable: false,
        pending_txn_id: String::new(),
        location_lat: 0.0,
        location_lon: 0.0,
        location_description: String::new(),
        thread_root_id: String::new(),
        is_thread_root: false,
        thread_reply_count: 0,
        thread_latest_sender_name: String::new(),
        thread_latest_body: String::new(),
        thread_latest_ts: 0,
        membership_action: String::new(),
        membership_target_user_id: String::new(),
        membership_target_name: String::new(),
        membership_target_avatar_url: String::new(),
    }
}

/// Map a UTD cause to a single-line user-facing message. Padlock glyph
/// matches the system-message style already used for "Message deleted".
/// Used by the timeline converter when matrix-sdk-ui surfaces an
/// `UnableToDecrypt` item so the row can render a proper explanation
/// instead of being silently dropped.
pub(crate) fn utd_message_for_cause(
    cause: matrix_sdk_base::crypto::types::events::UtdCause,
) -> &'static str {
    use matrix_sdk_base::crypto::types::events::UtdCause;
    match cause {
        UtdCause::SentBeforeWeJoined => "🔒 Sent before you joined this room",
        UtdCause::VerificationViolation => "🔒 Sender's identity changed since you verified them",
        UtdCause::UnsignedDevice => "🔒 Sender's device is not signed",
        UtdCause::UnknownDevice => "🔒 Sender's device is unknown",
        UtdCause::HistoricalMessageAndBackupIsDisabled => {
            "🔒 History unavailable (key backup is off)"
        }
        UtdCause::WithheldForUnverifiedOrInsecureDevice => "🔒 Sender blocked this device",
        UtdCause::WithheldBySender => "🔒 Key was not shared with this device",
        UtdCause::HistoricalMessageAndDeviceIsUnverified => {
            "🔒 Verify this device to access history"
        }
        UtdCause::Unknown => "🔒 Unable to decrypt",
    }
}

#[cfg(not(test))]
pub(super) async fn collect_reactions(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    room: &Room,
    me: Option<&UserId>,
) -> Vec<ReactionGroup> {
    let Some(table) = event_item.content().reactions() else {
        return Vec::new();
    };

    let mut out: Vec<ReactionGroup> = Vec::with_capacity(table.len());
    for (key, by_sender) in table.iter() {
        let count = by_sender.len() as u64;
        let reacted_by_me = me
            .as_ref()
            .map(|uid| by_sender.contains_key(*uid))
            .unwrap_or(false);

        let mut senders: Vec<String> = Vec::with_capacity(by_sender.len());
        for uid in by_sender.keys() {
            // Cheap-ish lookup: hits the SDK's in-memory state store. No
            // network. Falls back to the bare Matrix ID when membership
            // for this user hasn't been hydrated yet.
            let label = super::member_display_name(room, uid).await;
            senders.push(label);
        }

        // MSC4027: when the reaction key is an mxc:// URI it IS the image URL.
        // Reactions reference existing pack images, which are always plain mxc://.
        let source_url = if key.starts_with("mxc://") {
            key.clone()
        } else {
            String::new()
        };
        out.push(ReactionGroup {
            key: key.clone(),
            count,
            reacted_by_me,
            source_url,
            senders,
        });
    }
    out
}

#[cfg(not(test))]
pub(super) async fn collect_read_receipts(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    room: &Room,
    me: Option<&UserId>,
) -> Vec<ReadReceipt> {
    let table = event_item.read_receipts();
    if table.is_empty() {
        return Vec::new();
    }
    let mut out: Vec<ReadReceipt> = Vec::with_capacity(table.len());
    for uid in table.keys() {
        // Hide the current user's own receipt: they don't need to see their
        // own avatar marching down every message they've read.
        if me.is_some_and(|m| uid.as_str() == m.as_str()) {
            continue;
        }
        // Cheap-ish lookup against the SDK's in-memory state store. Same
        // pattern `collect_reactions` uses for resolving sender labels.
        let (display_name, avatar_url) = match room.get_member_no_sync(uid).await {
            Ok(Some(m)) => (
                m.display_name()
                    .map(str::to_owned)
                    .unwrap_or_else(|| uid.to_string()),
                m.avatar_url().map(|u| u.to_string()).unwrap_or_default(),
            ),
            _ => (uid.to_string(), String::new()),
        };
        out.push(ReadReceipt {
            user_id: uid.to_string(),
            display_name,
            avatar_url,
        });
    }
    out
}

/// Split a `MediaSource` into the two FFI fields used by the C++ bridge:
/// - `url`:           the plain mxc:// URI (both plain and encrypted carry this)
/// - `encrypted_json`: non-empty only when the source is encrypted; full JSON blob
///                     understood by `fetch_source_bytes` for decryption
#[cfg(not(test))]
pub(super) fn split_source(
    source: &matrix_sdk::ruma::events::room::MediaSource,
) -> (String, String) {
    use matrix_sdk::ruma::events::room::MediaSource;
    match source {
        MediaSource::Plain(uri) => (uri.to_string(), String::new()),
        MediaSource::Encrypted(file) => (
            file.url.to_string(),
            serde_json::to_string(source).unwrap_or_default(),
        ),
    }
}

/// Same as `split_source` but for an `Option<&MediaSource>`; returns ("","")
/// when absent.
#[cfg(not(test))]
pub(super) fn split_source_opt(
    source: Option<&matrix_sdk::ruma::events::room::MediaSource>,
) -> (String, String) {
    match source {
        Some(s) => split_source(s),
        None => (String::new(), String::new()),
    }
}

/// Recover `content.formatted_body` from the raw (pre-sanitization) event
/// JSON and re-sanitize it with `data-mx-emoticon` preserved, so MSC2545
/// inline custom emoticons survive matrix-sdk-ui's Timeline sanitization
/// (`html_sanitize.rs`'s doc comment explains why this bypass exists —
/// matrix-sdk-ui's own sanitizer has no hook to extend its allow-list, and
/// its `<img>` model has no field for `data-mx-emoticon` at all).
///
/// `fallback` is the Timeline-provided (already-sanitized) formatted_body —
/// used verbatim when the raw JSON isn't available yet (a local echo not
/// yet synced back from the server — `latest_json()` returns `None` in
/// that case) or doesn't parse / lacks a `formatted_body`. `latest_json()`
/// (not `original_json()`) is used so this also applies to the current
/// state of an edited message, not just its original content.
#[cfg(not(test))]
pub(super) fn resanitized_formatted_body(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    fallback: String,
) -> String {
    let Some(raw) = event_item.latest_json() else {
        return fallback;
    };
    let Ok(json) = serde_json::from_str::<serde_json::Value>(raw.json().get()) else {
        return fallback;
    };
    resanitized_formatted_body_from_json(&json, fallback)
}

/// JSON-pointer logic for `resanitized_formatted_body`, split out so it can
/// be unit-tested without constructing an `EventTimelineItem`.
///
/// For a replacement (`m.replace`) event, `latest_json()` is the *edit*
/// event, whose top-level `content.format`/`content.formatted_body` are only
/// a legacy fallback for clients that don't understand edits — and
/// ruma-events' `make_replacement_body()` unconditionally stamps a synthetic
/// `"* "` HTML fallback there even for plain-text edits with no real HTML.
/// The real content lives under `content["m.new_content"]`, so replacements
/// must read from there instead of the top level.
pub(super) fn resanitized_formatted_body_from_json(
    json: &serde_json::Value,
    fallback: String,
) -> String {
    let is_replacement = json
        .pointer("/content/m.relates_to/rel_type")
        .and_then(|v| v.as_str())
        == Some("m.replace");
    let content_ptr = if is_replacement {
        "/content/m.new_content"
    } else {
        "/content"
    };
    let is_html = json
        .pointer(&format!("{content_ptr}/format"))
        .and_then(|f| f.as_str())
        == Some("org.matrix.custom.html");
    let Some(raw_html) = json
        .pointer(&format!("{content_ptr}/formatted_body"))
        .and_then(|f| f.as_str())
        .filter(|_| is_html)
    else {
        return fallback;
    };
    let remove_reply_fallback =
        json.pointer("/content/m.relates_to/m.in_reply_to").is_some();
    crate::html_sanitize::sanitize_formatted_body(raw_html, remove_reply_fallback)
}

/// Parse a `geo:lat,lon` or `geo:lat,lon,alt` URI.
/// Returns `(lat, lon)` or `None` on parse failure.
pub(crate) fn parse_geo_uri(uri: &str) -> Option<(f64, f64)> {
    let coords = uri.strip_prefix("geo:")?;
    // Strip uncertainty params (after ';')
    let coords = coords.split(';').next()?;
    let mut parts = coords.split(',');
    let lat: f64 = parts.next()?.parse().ok()?;
    let lon: f64 = parts.next()?.parse().ok()?;
    Some((lat, lon))
}

/// Compute the human-readable action for a m.room.pinned_events state-event
/// change. `new_pinned` and `old_pinned` are the new and previous event-ID
/// lists respectively (as any `AsRef<str>` slice — `&[&str]` or
/// `&[OwnedEventId]` both work).
pub(crate) fn pinned_events_action(
    new_pinned: &[impl AsRef<str>],
    old_pinned: &[impl AsRef<str>],
) -> String {
    use std::collections::HashSet;
    let new_set: HashSet<&str> = new_pinned.iter().map(|id| id.as_ref()).collect();
    let old_set: HashSet<&str> = old_pinned.iter().map(|id| id.as_ref()).collect();
    let added = new_set.difference(&old_set).count();
    let removed = old_set.difference(&new_set).count();
    // "cleared" when >=3 items disappear all at once; smaller bulk removals
    // are reported as "unpinned N messages" so the message stays concise.
    if new_set.is_empty() && old_set.len() >= 3 {
        "cleared all pinned messages".to_owned()
    } else if added == 1 && removed == 0 {
        "pinned a message".to_owned()
    } else if added == 0 && removed == 1 {
        "unpinned a message".to_owned()
    } else if added > 1 && removed == 0 {
        format!("pinned {} messages", added)
    } else if added == 0 && removed > 1 {
        format!("unpinned {} messages", removed)
    } else {
        "changed the pinned messages".to_owned()
    }
}

/// Map a matrix-sdk-ui `MembershipChange` (computed from an `m.room.member`
/// state-event diff) to the stable, English-free FFI discriminant consumed
/// by the C++ i18n layer (see CLAUDE.md's i18n rule — this must never be
/// user-facing prose; C++ owns all phrase composition via `tk::tr()`).
/// Returns `None` for `None`/`Error`/`NotImplemented`, which are not real
/// user-facing transitions.
pub(crate) fn membership_action_str(
    change: matrix_sdk_ui::timeline::MembershipChange,
) -> Option<&'static str> {
    use matrix_sdk_ui::timeline::MembershipChange as M;
    Some(match change {
        M::Joined => "joined",
        M::Left => "left",
        M::Banned => "banned",
        M::Unbanned => "unbanned",
        M::Kicked => "kicked",
        M::Invited => "invited",
        M::KickedAndBanned => "kicked_and_banned",
        M::InvitationAccepted => "invitation_accepted",
        M::InvitationRejected => "invitation_rejected",
        M::InvitationRevoked => "invitation_revoked",
        M::Knocked => "knocked",
        M::KnockAccepted => "knock_accepted",
        M::KnockRetracted => "knock_retracted",
        M::KnockDenied => "knock_denied",
        M::None | M::Error | M::NotImplemented => return None,
    })
}

/// Shared so the in-reply-to quote block and the thread latest-event preview
/// emit identical snippet text.
#[cfg(not(test))]
pub(crate) fn msglike_snippet(content: &TimelineItemContent) -> String {
    use matrix_sdk::ruma::events::room::message::MessageType;
    match content {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(m),
            ..
        }) => match m.msgtype() {
            MessageType::Text(t) => t.body.clone(),
            MessageType::Image(_) => "(image)".to_owned(),
            MessageType::File(_) => "(file)".to_owned(),
            MessageType::Audio(a) => {
                if a.voice.is_some() {
                    "(voice)".to_owned()
                } else {
                    "(audio)".to_owned()
                }
            }
            MessageType::Video(_) => "(video)".to_owned(),
            _ => "(message)".to_owned(),
        },
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Sticker(_),
            ..
        }) => "(sticker)".to_owned(),
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Redacted,
            ..
        }) => "(deleted)".to_owned(),
        _ => String::new(),
    }
}

/// Extract (sender_display_name, body_snippet, timestamp_ms) from an embedded
/// event (a thread's latest reply, or an in-reply-to target).
#[cfg(not(test))]
pub(super) fn embedded_event_preview(
    embedded: &matrix_sdk_ui::timeline::EmbeddedEvent,
) -> (String, String, u64) {
    let name = match &embedded.sender_profile {
        TimelineDetails::Ready(p) => p
            .display_name
            .clone()
            .unwrap_or_else(|| embedded.sender.to_string()),
        _ => embedded.sender.to_string(),
    };
    let body = msglike_snippet(&embedded.content);
    let ts: u64 = embedded.timestamp.get().into();
    (name, body, ts)
}

/// Returns (url, encrypted_json) for the thumbnail (or full-res when no
/// thumbnail is present) when the embedded event is an `m.image` message.
/// Returns ("", "") for all other message types.
#[cfg(not(test))]
pub(super) fn reply_image_source(
    embedded: &matrix_sdk_ui::timeline::EmbeddedEvent,
) -> (String, String) {
    use matrix_sdk::ruma::events::room::message::MessageType;
    match &embedded.content {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(m),
            ..
        }) => match m.msgtype() {
            MessageType::Image(i) => {
                let thumb = i
                    .info
                    .as_ref()
                    .and_then(|info| info.thumbnail_source.as_ref());
                if let Some(src) = thumb {
                    split_source(src)
                } else {
                    split_source(&i.source)
                }
            }
            _ => (String::new(), String::new()),
        },
        _ => (String::new(), String::new()),
    }
}

#[cfg(not(test))]
pub(super) async fn timeline_item_to_ffi(
    item: &Arc<TimelineItem>,
    room_id: &str,
    room: &Room,
    me: Option<&UserId>,
) -> Option<TimelineEvent> {
    use matrix_sdk::ruma::events::room::message::MessageType;

    let event_item = match item.kind() {
        TimelineItemKind::Event(e) => e,
        TimelineItemKind::Virtual(v) => {
            let (msg_type, timestamp): (&str, u64) = match v {
                VirtualTimelineItem::DateDivider(ts) => ("virtual.date_divider", ts.get().into()),
                VirtualTimelineItem::ReadMarker => ("virtual.read_marker", 0),
                VirtualTimelineItem::TimelineStart => ("virtual.timeline_start", 0),
            };
            return Some(TimelineEvent {
                room_id: room_id.to_owned(),
                msg_type: msg_type.to_owned(),
                timestamp,
                ..ffi_event_defaults()
            });
        }
    };

    // m.room.pinned_events state events: surface as a labelled timeline row.
    // All other state events (membership, room name, etc.) remain filtered.
    if let TimelineItemContent::OtherState(state) = event_item.content() {
        use matrix_sdk::ruma::events::StateEventContentChange;
        use matrix_sdk_ui::timeline::AnyOtherStateEventContentChange;
        if let AnyOtherStateEventContentChange::RoomPinnedEvents(full) = state.content() {
            if let StateEventContentChange::Original {
                content,
                prev_content,
            } = full
            {
                let new_ids: Vec<_> = content.pinned.iter().map(|id| id.to_string()).collect();
                let old_ids: Vec<_> = prev_content
                    .as_ref()
                    .and_then(|pc| pc.pinned.as_ref())
                    .map(|ids| ids.iter().map(|id| id.to_string()).collect())
                    .unwrap_or_default();
                let body = pinned_events_action(&new_ids, &old_ids);
                let (sender_name, sender_avatar_url) =
                    if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                        (
                            p.display_name.clone().unwrap_or_default(),
                            p.avatar_url
                                .as_ref()
                                .map(|u| u.to_string())
                                .unwrap_or_default(),
                        )
                    } else {
                        (String::new(), String::new())
                    };
                return Some(TimelineEvent {
                    room_id: room_id.to_owned(),
                    msg_type: "m.room.pinned_events".to_owned(),
                    event_id: event_item
                        .event_id()
                        .map(|id| id.to_string())
                        .unwrap_or_default(),
                    sender: event_item.sender().to_string(),
                    sender_name,
                    sender_avatar_url,
                    body,
                    timestamp: event_item.timestamp().get().into(),
                    ..ffi_event_defaults()
                });
            }
        }
        // StateEventContentChange::Redacted: no content to diff — silently drop.
        return None; // all other state events (and redacted pins) remain filtered
    }

    // m.room.member state events representing an actual membership
    // transition (join/leave/kick/ban/invite/knock and their
    // accept/reject/revoke counterparts). Visibility is gated downstream by
    // the caller (see `filter_membership` in timeline.rs) — this conversion
    // is unconditional, matching the m.room.pinned_events precedent above.
    // Profile-only changes while already joined are
    // TimelineItemContent::ProfileChange — a distinct variant never matched
    // here; it falls through to the catch-all `None` below, by design.
    if let TimelineItemContent::MembershipChange(change) = event_item.content() {
        let Some(action) = change.change().and_then(membership_action_str) else {
            // None/Error/NotImplemented, or a redacted state event whose
            // `.change()` cannot be computed — not a real transition, drop.
            return None;
        };
        let (sender_name, sender_avatar_url) =
            if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                (
                    p.display_name.clone().unwrap_or_default(),
                    p.avatar_url
                        .as_ref()
                        .map(|u| u.to_string())
                        .unwrap_or_default(),
                )
            } else {
                (String::new(), String::new())
            };
        return Some(TimelineEvent {
            room_id: room_id.to_owned(),
            msg_type: "m.room.member".to_owned(),
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            membership_action: action.to_owned(),
            membership_target_user_id: change.user_id().to_string(),
            membership_target_name: change.display_name().unwrap_or_default(),
            membership_target_avatar_url: change
                .avatar_url()
                .map(|u| u.to_string())
                .unwrap_or_default(),
            timestamp: event_item.timestamp().get().into(),
            ..ffi_event_defaults()
        });
    }

    // Compute pending fields once for all non-virtual event paths.
    let (pending_state, pending_error, pending_recoverable, pending_txn_id) =
        if event_item.is_local_echo() {
            let txn = event_item
                .transaction_id()
                .map(|t| t.to_string())
                .unwrap_or_default();
            match event_item.send_state() {
                Some(EventSendState::NotSentYet { .. }) => {
                    ("sending".to_owned(), String::new(), false, txn)
                }
                Some(EventSendState::SendingFailed {
                    error,
                    is_recoverable,
                }) => ("failed".to_owned(), error.to_string(), *is_recoverable, txn),
                _ => (String::new(), String::new(), false, txn),
            }
        } else {
            (String::new(), String::new(), false, String::new())
        };

    // Undecryptable messages: matrix-sdk-ui surfaces them as a MsgLikeKind
    // variant we'd otherwise drop with the rest of the fall-through. Map the
    // crypto `UtdCause` to a single-line user-facing reason and emit an
    // "m.utd" tombstone so the UI can paint a muted row instead of leaving
    // a gap where a message exists on the server.
    if let TimelineItemContent::MsgLike(MsgLikeContent {
        kind: MsgLikeKind::UnableToDecrypt(encrypted_message),
        ..
    }) = event_item.content()
    {
        use matrix_sdk_base::crypto::types::events::UtdCause;
        let cause = match encrypted_message {
            EncryptedMessage::MegolmV1AesSha2 { cause, .. } => *cause,
            _ => UtdCause::Unknown,
        };
        let body = utd_message_for_cause(cause).to_owned();
        let (sender_name, sender_avatar_url) =
            if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                (
                    p.display_name.clone().unwrap_or_default(),
                    p.avatar_url
                        .as_ref()
                        .map(|u| u.to_string())
                        .unwrap_or_default(),
                )
            } else {
                (String::new(), String::new())
            };
        return Some(TimelineEvent {
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            room_id: room_id.to_owned(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            body,
            timestamp: event_item.timestamp().get().into(),
            msg_type: "m.utd".to_owned(),
            ..ffi_event_defaults()
        });
    }

    // Redactions: matrix-sdk-ui replaces the original item with
    // MsgLikeKind::Redacted in place. Surface it as a tombstone (msg_type
    // "m.redacted") so the UI can swap the existing row to a placeholder
    // instead of leaving the stale body on screen.
    if let TimelineItemContent::MsgLike(MsgLikeContent {
        kind: MsgLikeKind::Redacted,
        ..
    }) = event_item.content()
    {
        let (sender_name, sender_avatar_url) =
            if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                (
                    p.display_name.clone().unwrap_or_default(),
                    p.avatar_url
                        .as_ref()
                        .map(|u| u.to_string())
                        .unwrap_or_default(),
                )
            } else {
                (String::new(), String::new())
            };
        // Receipts on a tombstone are meaningless — the original event
        // is gone; the redacted placeholder doesn't carry a reading
        // audience worth surfacing.
        return Some(TimelineEvent {
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            room_id: room_id.to_owned(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            timestamp: event_item.timestamp().get().into(),
            msg_type: "m.redacted".to_owned(),
            ..ffi_event_defaults()
        });
    }

    // Sticker events are MsgLikeKind::Sticker, not MsgLikeKind::Message.
    // Handle them before falling through to the message-only path.
    if let TimelineItemContent::MsgLike(MsgLikeContent {
        kind: MsgLikeKind::Sticker(s),
        ..
    }) = event_item.content()
    {
        let c = s.content();
        let (source_url, source_encrypted_json) = match &c.source {
            matrix_sdk::ruma::events::sticker::StickerMediaSource::Plain(uri) => {
                (uri.to_string(), String::new())
            }
            matrix_sdk::ruma::events::sticker::StickerMediaSource::Encrypted(file) => {
                let ms = matrix_sdk::ruma::events::room::MediaSource::Encrypted(file.clone());
                (
                    file.url.to_string(),
                    serde_json::to_string(&ms).unwrap_or_default(),
                )
            }
            _ => (String::new(), String::new()),
        };
        let (image_thumbnail_url, image_thumbnail_encrypted_json) =
            split_source_opt(c.info.thumbnail_source.as_ref());
        let w = c.info.width.map(u64::from).unwrap_or(0);
        let h = c.info.height.map(u64::from).unwrap_or(0);
        let (sender_name, sender_avatar_url) =
            if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                (
                    p.display_name.clone().unwrap_or_default(),
                    p.avatar_url
                        .as_ref()
                        .map(|u| u.to_string())
                        .unwrap_or_default(),
                )
            } else {
                (String::new(), String::new())
            };
        let reactions = collect_reactions(event_item, room, me).await;
        let read_receipts = collect_read_receipts(event_item, room, me).await;
        return Some(TimelineEvent {
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            room_id: room_id.to_owned(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            body: c.body.clone(),
            timestamp: event_item.timestamp().get().into(),
            msg_type: "m.sticker".to_owned(),
            source_url,
            source_encrypted_json,
            width: w,
            height: h,
            image_thumbnail_url,
            image_thumbnail_encrypted_json,
            reactions,
            read_receipts,
            blurhash: c.info.blurhash.as_deref().unwrap_or("").to_owned(),
            sticker_info_json: serde_json::to_string(&c.info).unwrap_or_else(|_| "{}".to_owned()),
            image_animated: c.info.is_animated.unwrap_or(false),
            pending_state: pending_state.clone(),
            pending_error: pending_error.clone(),
            pending_recoverable,
            pending_txn_id: pending_txn_id.clone(),
            ..ffi_event_defaults()
        });
    }

    // org.matrix.msc4075.rtc.notification — matrix-sdk-ui 0.18 surfaces this
    // as a dedicated TimelineItemContent::RtcNotification variant (not Other).
    // Map call_intent (Audio/Video/None) to a body string for the C++ layer.
    if let TimelineItemContent::RtcNotification { call_intent, .. } = event_item.content() {
        use matrix_sdk::ruma::events::rtc::notification::CallIntent;
        let intent_str = match call_intent {
            Some(CallIntent::Audio) => "audio",
            Some(CallIntent::Video) => "video",
            _ => "",
        };
        let (sender_name, sender_avatar_url) =
            if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                (
                    p.display_name.clone().unwrap_or_default(),
                    p.avatar_url
                        .as_ref()
                        .map(|u| u.to_string())
                        .unwrap_or_default(),
                )
            } else {
                (String::new(), String::new())
            };
        return Some(TimelineEvent {
            room_id: room_id.to_owned(),
            msg_type: "org.matrix.msc4075.rtc.notification".to_owned(),
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            body: intent_str.to_owned(), // "audio" | "video" | ""
            timestamp: event_item.timestamp().get().into(),
            ..ffi_event_defaults()
        });
    }

    let msg_content = match event_item.content() {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(msg),
            ..
        }) => msg,
        _ => return None,
    };

    // Per-msgtype partial: each arm sets only the fields that differ from the
    // zeroed base; everything else comes from `ffi_event_defaults()`. The
    // post-match code below layers sender/reaction/thread/etc. fields on top of
    // this partial via struct-update syntax (`..msg_fields`), so this arm must
    // NOT set any of those fields.
    let msg_fields: TimelineEvent = match msg_content.msgtype() {
        MessageType::Text(t) => {
            let fallback = t
                .formatted
                .as_ref()
                .filter(|f| {
                    matches!(
                        f.format,
                        matrix_sdk::ruma::events::room::message::MessageFormat::Html
                    )
                })
                .map(|f| f.body.clone())
                .unwrap_or_default();
            let fmt = resanitized_formatted_body(event_item, fallback);
            TimelineEvent {
                body: t.body.clone(),
                formatted_body: fmt,
                msg_type: "m.text".to_owned(),
                ..ffi_event_defaults()
            }
        }
        MessageType::Image(i) => {
            let (src_url, src_enc) = split_source(&i.source);
            let (w, h) = i
                .info
                .as_ref()
                .map(|info| {
                    (
                        info.width.map(u64::from).unwrap_or(0u64),
                        info.height.map(u64::from).unwrap_or(0u64),
                    )
                })
                .unwrap_or((0u64, 0u64));
            // MSC2530: filename field signals that body is a user caption.
            let img_filename = i.filename.clone().unwrap_or_default();
            TimelineEvent {
                body: i.body.clone(),
                msg_type: "m.image".to_owned(),
                source_url: src_url,
                source_encrypted_json: src_enc,
                width: w,
                height: h,
                image_filename: img_filename,
                ..ffi_event_defaults()
            }
        }
        MessageType::File(f) => {
            let (file_url, file_enc) = split_source(&f.source);
            // MSC2530: when `filename` is present it holds the real file name
            // and `body` is the user-supplied caption. When absent, `body` is
            // the fallback filename and there is no caption.
            let file_filename = f.filename.clone().unwrap_or_default();
            let name = if file_filename.is_empty() {
                f.body.clone()
            } else {
                file_filename.clone()
            };
            let size = f
                .info
                .as_ref()
                .and_then(|info| info.size)
                .map(u64::from)
                .unwrap_or(0u64);
            TimelineEvent {
                body: f.body.clone(),
                msg_type: "m.file".to_owned(),
                file_url,
                file_encrypted_json: file_enc,
                file_name: name,
                file_size: size,
                file_filename,
                ..ffi_event_defaults()
            }
        }
        // MSC3245: voice messages are `m.audio` events tagged with
        // `org.matrix.msc3245.voice`; the MSC1767 `audio` block carries
        // duration + waveform. Plain `m.audio` (no voice marker) is surfaced
        // as msg_type "m.audio" and rendered as an inline audio-player card.
        MessageType::Audio(a) => {
            let (aud_url, aud_enc) = split_source(&a.source);
            let info_mime = a
                .info
                .as_deref()
                .and_then(|i| i.mimetype.clone())
                .unwrap_or_default();
            let info_duration_ms = a
                .info
                .as_deref()
                .and_then(|i| i.duration)
                .map(|d| d.as_millis() as u64)
                .unwrap_or(0u64);
            if a.voice.is_some() {
                let (duration_ms, waveform) = match &a.audio {
                    Some(block) => {
                        let dur = block.duration.as_millis() as u64;
                        let wf: Vec<u16> = block
                            .waveform
                            .iter()
                            .map(|amp| u16::try_from(u64::from(amp.get())).unwrap_or(0))
                            .collect();
                        (if dur != 0 { dur } else { info_duration_ms }, wf)
                    }
                    None => (info_duration_ms, Vec::new()),
                };
                TimelineEvent {
                    body: a.body.clone(),
                    msg_type: "m.voice".to_owned(),
                    audio_url: aud_url,
                    audio_encrypted_json: aud_enc,
                    audio_duration_ms: duration_ms,
                    audio_waveform: waveform,
                    audio_mime: info_mime,
                    ..ffi_event_defaults()
                }
            } else {
                let name = a.filename.clone().unwrap_or_else(|| a.body.clone());
                let size = a
                    .info
                    .as_deref()
                    .and_then(|i| i.size)
                    .map(u64::from)
                    .unwrap_or(0u64);
                TimelineEvent {
                    body: a.body.clone(),
                    msg_type: "m.audio".to_owned(),
                    file_name: name,
                    file_size: size,
                    audio_url: aud_url,
                    audio_encrypted_json: aud_enc,
                    audio_duration_ms: info_duration_ms,
                    audio_mime: info_mime,
                    ..ffi_event_defaults()
                }
            }
        }
        MessageType::Video(v) => {
            let (src_url, src_enc) = split_source(&v.source);
            let (w, h, dur_ms, mime, thumb_url, thumb_enc) = v
                .info
                .as_ref()
                .map(|info| {
                    let w = info.width.map(u64::from).unwrap_or(0u64);
                    let h = info.height.map(u64::from).unwrap_or(0u64);
                    let dur = info.duration.map(|d| d.as_millis() as u64).unwrap_or(0u64);
                    let mime = info.mimetype.clone().unwrap_or_default();
                    let (tu, te) = split_source_opt(info.thumbnail_source.as_ref());
                    (w, h, dur, mime, tu, te)
                })
                .unwrap_or_default();
            let vid_filename = v.filename.clone().unwrap_or_default();
            // Parse fi.mau.* vendor hints from the raw event JSON.
            // Parse once and extract all flags from the single Value.
            let mau_info: Option<serde_json::Value> = event_item
                .original_json()
                .and_then(|raw| serde_json::from_str::<serde_json::Value>(raw.json().get()).ok())
                .and_then(|json| json.pointer("/content/info").cloned());
            let mau = |key: &str| -> bool {
                mau_info
                    .as_ref()
                    .and_then(|info| info.get(key)?.as_bool())
                    .unwrap_or(false)
            };
            let video_gif = mau("fi.mau.gif");
            let video_autoplay = mau("fi.mau.autoplay") || video_gif;
            let video_loop = mau("fi.mau.loop") || video_gif;
            let video_no_audio = mau("fi.mau.no_audio") || video_gif;
            let video_hide_controls = mau("fi.mau.hide_controls") || video_gif;
            TimelineEvent {
                body: v.body.clone(),
                msg_type: "m.video".to_owned(),
                source_url: src_url,
                source_encrypted_json: src_enc,
                width: w,
                height: h,
                image_filename: vid_filename,
                video_thumbnail_url: thumb_url,
                video_thumbnail_encrypted_json: thumb_enc,
                video_duration_ms: dur_ms,
                video_mime: mime,
                video_autoplay,
                video_loop,
                video_no_audio,
                video_hide_controls,
                video_gif,
                ..ffi_event_defaults()
            }
        }
        MessageType::Emote(e) => {
            let fallback = e
                .formatted
                .as_ref()
                .filter(|f| {
                    matches!(
                        f.format,
                        matrix_sdk::ruma::events::room::message::MessageFormat::Html
                    )
                })
                .map(|f| f.body.clone())
                .unwrap_or_default();
            let fmt = resanitized_formatted_body(event_item, fallback);
            TimelineEvent {
                body: e.body.clone(),
                formatted_body: fmt,
                msg_type: "m.emote".to_owned(),
                ..ffi_event_defaults()
            }
        }
        MessageType::Notice(n) => {
            let fallback = n
                .formatted
                .as_ref()
                .filter(|f| {
                    matches!(
                        f.format,
                        matrix_sdk::ruma::events::room::message::MessageFormat::Html
                    )
                })
                .map(|f| f.body.clone())
                .unwrap_or_default();
            let fmt = resanitized_formatted_body(event_item, fallback);
            TimelineEvent {
                body: n.body.clone(),
                formatted_body: fmt,
                msg_type: "m.notice".to_owned(),
                ..ffi_event_defaults()
            }
        }
        MessageType::Location(l) => {
            let (lat, lon) = parse_geo_uri(l.geo_uri()).unwrap_or((0.0, 0.0));
            TimelineEvent {
                body: l.body.clone(),
                msg_type: "m.location".to_owned(),
                location_lat: lat,
                location_lon: lon,
                location_description: l.body.clone(),
                ..ffi_event_defaults()
            }
        }
        _ => return None,
    };

    let blurhash = match msg_content.msgtype() {
        MessageType::Image(i) => i
            .info
            .as_ref()
            .and_then(|info| info.blurhash.as_deref())
            .unwrap_or("")
            .to_owned(),
        MessageType::Video(v) => v
            .info
            .as_ref()
            .and_then(|info| info.blurhash.as_deref())
            .unwrap_or("")
            .to_owned(),
        _ => String::new(),
    };

    let image_animated = match msg_content.msgtype() {
        MessageType::Image(i) => i
            .info
            .as_ref()
            .and_then(|info| info.is_animated)
            .unwrap_or(false),
        _ => false,
    };

    let (sender_name, sender_avatar_url) =
        if let TimelineDetails::Ready(p) = event_item.sender_profile() {
            (
                p.display_name.clone().unwrap_or_default(),
                p.avatar_url
                    .as_ref()
                    .map(|u| u.to_string())
                    .unwrap_or_default(),
            )
        } else {
            (String::new(), String::new())
        };

    // m.in_reply_to — extract the event_id and, when the replied-to item is
    // present in the local timeline cache, its sender display name, a brief
    // body snippet, and (for m.image replies) the image thumbnail source.
    let (
        in_reply_to_id,
        in_reply_to_sender_name,
        in_reply_to_body,
        in_reply_to_image_url,
        in_reply_to_image_encrypted_json,
    ) = match event_item.content().in_reply_to() {
        None => (
            String::new(),
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        ),
        Some(details) => {
            let id = details.event_id.to_string();
            let (rname, rbody, img_url, img_enc) = match &details.event {
                TimelineDetails::Ready(replied) => {
                    let (name, snippet, _ts) = embedded_event_preview(replied);
                    let (iu, ie) = reply_image_source(replied);
                    (name, snippet, iu, ie)
                }
                _ => (String::new(), String::new(), String::new(), String::new()),
            };
            (id, rname, rbody, img_url, img_enc)
        }
    };

    let (image_thumbnail_url, image_thumbnail_encrypted_json): (String, String) =
        match msg_content.msgtype() {
            MessageType::Image(i) => split_source_opt(
                i.info
                    .as_ref()
                    .and_then(|info| info.thumbnail_source.as_ref()),
            ),
            _ => (String::new(), String::new()),
        };

    let reactions = collect_reactions(event_item, room, me).await;
    let read_receipts = collect_read_receipts(event_item, room, me).await;

    // MSC3440 thread metadata.
    let thread_root_id = event_item
        .content()
        .thread_root()
        .map(|id| id.to_string())
        .unwrap_or_default();
    let (
        is_thread_root,
        thread_reply_count,
        thread_latest_sender_name,
        thread_latest_body,
        thread_latest_ts,
    ) = match event_item.content().thread_summary() {
        None => (false, 0u64, String::new(), String::new(), 0u64),
        Some(summary) => {
            let count = summary.num_replies as u64;
            let (name, body, ts) = match &summary.latest_event {
                TimelineDetails::Ready(embedded) => embedded_event_preview(embedded),
                _ => (String::new(), String::new(), 0u64),
            };
            (true, count, name, body, ts)
        }
    };

    // Layer the common (sender / reaction / reply / thread / pending) fields on
    // top of the per-msgtype partial. Fields the match arm set (body, msg_type,
    // source_*, width/height, file_*, audio_*, video_*, location_*) flow through
    // unchanged via `..msg_fields`; only the fields named here are overridden.
    Some(TimelineEvent {
        event_id: event_item
            .event_id()
            .map(|id| id.to_string())
            .unwrap_or_default(),
        room_id: room_id.to_owned(),
        sender: event_item.sender().to_string(),
        sender_name,
        sender_avatar_url,
        timestamp: event_item.timestamp().get().into(),
        image_thumbnail_url,
        image_thumbnail_encrypted_json,
        reactions,
        read_receipts,
        in_reply_to_id,
        in_reply_to_sender_name,
        in_reply_to_body,
        in_reply_to_image_url,
        in_reply_to_image_encrypted_json,
        is_edited: msg_content.is_edited(),
        blurhash,
        image_animated,
        pending_state,
        pending_error,
        pending_recoverable,
        pending_txn_id,
        thread_root_id,
        is_thread_root,
        thread_reply_count,
        thread_latest_sender_name,
        thread_latest_body,
        thread_latest_ts,
        ..msg_fields
    })
}

#[cfg(test)]
mod pinned_action_tests {
    use super::pinned_events_action;

    #[test]
    fn pin_one() {
        assert_eq!(
            pinned_events_action(&["$a"], &[] as &[&str]),
            "pinned a message"
        );
    }

    #[test]
    fn unpin_one() {
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a"]),
            "unpinned a message"
        );
    }

    #[test]
    fn pin_many() {
        assert_eq!(
            pinned_events_action(&["$a", "$b", "$c"], &[] as &[&str]),
            "pinned 3 messages"
        );
    }

    #[test]
    fn unpin_many() {
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a", "$b"]),
            "unpinned 2 messages"
        );
    }

    #[test]
    fn clear_all_three() {
        // "cleared" fires only when new list is empty AND old list had >=3 items.
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a", "$b", "$c"]),
            "cleared all pinned messages"
        );
    }

    #[test]
    fn unpin_two_below_threshold() {
        // Only 2 removed => threshold not met, so "unpinned N messages" fires instead.
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a", "$b"]),
            "unpinned 2 messages"
        );
    }

    #[test]
    fn mixed_change() {
        assert_eq!(
            pinned_events_action(&["$b"], &["$a"]),
            "changed the pinned messages"
        );
    }

    #[test]
    fn no_change() {
        // Callers should filter out no-op state events (old == new) before calling.
        // The fallthrough branch returns this, but a real caller won't reach it.
        assert_eq!(
            pinned_events_action(&["$a"], &["$a"]),
            "changed the pinned messages"
        );
    }
}

#[cfg(test)]
mod membership_action_tests {
    use super::membership_action_str;
    use matrix_sdk_ui::timeline::MembershipChange as M;

    #[test]
    fn real_transitions_map_to_stable_discriminants() {
        assert_eq!(membership_action_str(M::Joined), Some("joined"));
        assert_eq!(membership_action_str(M::Left), Some("left"));
        assert_eq!(membership_action_str(M::Banned), Some("banned"));
        assert_eq!(membership_action_str(M::Unbanned), Some("unbanned"));
        assert_eq!(membership_action_str(M::Kicked), Some("kicked"));
        assert_eq!(membership_action_str(M::Invited), Some("invited"));
        assert_eq!(
            membership_action_str(M::KickedAndBanned),
            Some("kicked_and_banned")
        );
        assert_eq!(
            membership_action_str(M::InvitationAccepted),
            Some("invitation_accepted")
        );
        assert_eq!(
            membership_action_str(M::InvitationRejected),
            Some("invitation_rejected")
        );
        assert_eq!(
            membership_action_str(M::InvitationRevoked),
            Some("invitation_revoked")
        );
        assert_eq!(membership_action_str(M::Knocked), Some("knocked"));
        assert_eq!(
            membership_action_str(M::KnockAccepted),
            Some("knock_accepted")
        );
        assert_eq!(
            membership_action_str(M::KnockRetracted),
            Some("knock_retracted")
        );
        assert_eq!(membership_action_str(M::KnockDenied), Some("knock_denied"));
    }

    #[test]
    fn non_transitions_drop() {
        assert_eq!(membership_action_str(M::None), None);
        assert_eq!(membership_action_str(M::Error), None);
        assert_eq!(membership_action_str(M::NotImplemented), None);
    }
}

#[cfg(test)]
mod resanitized_formatted_body_tests {
    use super::resanitized_formatted_body_from_json;

    #[test]
    fn plain_text_edit_ignores_synthetic_fallback_html() {
        // ruma-events' make_replacement_body() unconditionally stamps a
        // synthetic "* " HTML fallback on the top-level content of an edit
        // event, even when the edit itself has no real HTML. The resolved
        // fallback (passed in from the aggregated content) must win, not
        // that synthetic top-level "*".
        let json = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "* edited body",
                "format": "org.matrix.custom.html",
                "formatted_body": "* ",
                "m.new_content": { "msgtype": "m.text", "body": "edited body" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig" }
            }
        });
        assert_eq!(
            resanitized_formatted_body_from_json(&json, String::new()),
            ""
        );
    }

    #[test]
    fn html_edit_reads_new_content_formatted_body() {
        let json = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "* edited body",
                "format": "org.matrix.custom.html",
                "formatted_body": "* ",
                "m.new_content": {
                    "msgtype": "m.text",
                    "body": "edited body",
                    "format": "org.matrix.custom.html",
                    "formatted_body": "<p>edited body</p>"
                },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig" }
            }
        });
        assert_eq!(
            resanitized_formatted_body_from_json(&json, String::new()),
            "<p>edited body</p>"
        );
    }

    #[test]
    fn non_edit_event_still_reads_top_level_formatted_body() {
        let json = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "edited body",
                "format": "org.matrix.custom.html",
                "formatted_body": "<p>edited body</p>"
            }
        });
        assert_eq!(
            resanitized_formatted_body_from_json(&json, String::new()),
            "<p>edited body</p>"
        );
    }

    #[test]
    fn plain_text_edit_with_no_formatted_new_content_falls_back() {
        let json = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": "* edited body",
                "m.new_content": { "msgtype": "m.text", "body": "edited body" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$orig" }
            }
        });
        assert_eq!(
            resanitized_formatted_body_from_json(&json, "fallback".to_owned()),
            "fallback"
        );
    }
}
