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
    EncryptedMessage, EventSendState, MsgLikeContent, MsgLikeKind,
    TimelineDetails, TimelineItem, TimelineItemContent, TimelineItemKind,
    VirtualTimelineItem,
};

#[cfg(not(test))]
use std::sync::Arc;

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

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
        UtdCause::SentBeforeWeJoined =>
            "🔒 Sent before you joined this room",
        UtdCause::VerificationViolation =>
            "🔒 Sender's identity changed since you verified them",
        UtdCause::UnsignedDevice => "🔒 Sender's device is not signed",
        UtdCause::UnknownDevice => "🔒 Sender's device is unknown",
        UtdCause::HistoricalMessageAndBackupIsDisabled =>
            "🔒 History unavailable (key backup is off)",
        UtdCause::WithheldForUnverifiedOrInsecureDevice =>
            "🔒 Sender blocked this device",
        UtdCause::WithheldBySender =>
            "🔒 Key was not shared with this device",
        UtdCause::HistoricalMessageAndDeviceIsUnverified =>
            "🔒 Verify this device to access history",
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
            let label = match room.get_member_no_sync(uid).await {
                Ok(Some(m)) => m
                    .display_name()
                    .map(str::to_owned)
                    .unwrap_or_else(|| uid.to_string()),
                _ => uid.to_string(),
            };
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
        if me.map_or(false, |m| uid.as_str() == m.as_str()) {
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
                let thumb = i.info.as_ref().and_then(|info| info.thumbnail_source.as_ref());
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
                event_id: String::new(),
                sender: String::new(),
                sender_name: String::new(),
                sender_avatar_url: String::new(),
                body: String::new(),
                source_url: String::new(),
                source_encrypted_json: String::new(),
                width: 0,
                height: 0,
                file_url: String::new(),
                file_encrypted_json: String::new(),
                file_name: String::new(),
                file_size: 0,
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
            });
        }
    };

    // m.room.pinned_events state events: surface as a labelled timeline row.
    // All other state events (membership, room name, etc.) remain filtered.
    if let TimelineItemContent::OtherState(state) = event_item.content() {
        use matrix_sdk::ruma::events::StateEventContentChange;
        use matrix_sdk_ui::timeline::AnyOtherStateEventContentChange;
        if let AnyOtherStateEventContentChange::RoomPinnedEvents(full) = state.content() {
            if let StateEventContentChange::Original { content, prev_content } = full {
                let new_ids: Vec<_> = content.pinned.iter()
                    .map(|id| id.to_string()).collect();
                let old_ids: Vec<_> = prev_content.as_ref()
                    .and_then(|pc| pc.pinned.as_ref())
                    .map(|ids| ids.iter().map(|id| id.to_string()).collect())
                    .unwrap_or_default();
                let body = pinned_events_action(&new_ids, &old_ids);
                let (sender_name, sender_avatar_url) =
                    if let TimelineDetails::Ready(p) = event_item.sender_profile() {
                        (
                            p.display_name.clone().unwrap_or_default(),
                            p.avatar_url.as_ref().map(|u| u.to_string()).unwrap_or_default(),
                        )
                    } else {
                        (String::new(), String::new())
                    };
                return Some(TimelineEvent {
                    room_id: room_id.to_owned(),
                    msg_type: "m.room.pinned_events".to_owned(),
                    event_id: event_item.event_id()
                        .map(|id| id.to_string()).unwrap_or_default(),
                    sender: event_item.sender().to_string(),
                    sender_name,
                    sender_avatar_url,
                    body,
                    timestamp: event_item.timestamp().get().into(),
                    source_url: String::new(),
                    source_encrypted_json: String::new(),
                    width: 0, height: 0,
                    file_url: String::new(), file_encrypted_json: String::new(),
                    file_name: String::new(), file_size: 0,
                    image_filename: String::new(),
                    audio_url: String::new(), audio_encrypted_json: String::new(),
                    audio_duration_ms: 0, audio_waveform: Vec::new(),
                    audio_mime: String::new(),
                    video_thumbnail_url: String::new(),
                    video_thumbnail_encrypted_json: String::new(),
                    image_thumbnail_url: String::new(),
                    image_thumbnail_encrypted_json: String::new(),
                    video_duration_ms: 0, video_mime: String::new(),
                    video_autoplay: false, video_loop: false, video_no_audio: false,
                    video_hide_controls: false, video_gif: false,
                    reactions: Vec::new(), read_receipts: Vec::new(),
                    in_reply_to_id: String::new(), in_reply_to_sender_name: String::new(),
                    in_reply_to_body: String::new(), in_reply_to_image_url: String::new(),
                    in_reply_to_image_encrypted_json: String::new(),
                    is_edited: false, formatted_body: String::new(),
                    blurhash: String::new(), sticker_info_json: String::new(),
                    image_animated: false,
                    pending_state: String::new(), pending_error: String::new(),
                    pending_recoverable: false, pending_txn_id: String::new(),
                    location_lat: 0.0, location_lon: 0.0, location_description: String::new(),
                    thread_root_id: String::new(), is_thread_root: false,
                    thread_reply_count: 0,
                    thread_latest_sender_name: String::new(),
                    thread_latest_body: String::new(), thread_latest_ts: 0,
                });
            }
        }
        // StateEventContentChange::Redacted: no content to diff — silently drop.
        return None; // all other state events (and redacted pins) remain filtered
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
            source_url: String::new(),
            source_encrypted_json: String::new(),
            width: 0u64,
            height: 0u64,
            file_url: String::new(),
            file_encrypted_json: String::new(),
            file_name: String::new(),
            file_size: 0u64,
            image_filename: String::new(),
            audio_url: String::new(),
            audio_encrypted_json: String::new(),
            audio_duration_ms: 0u64,
            audio_waveform: Vec::new(),
            audio_mime: String::new(),
            video_thumbnail_url: String::new(),
            video_thumbnail_encrypted_json: String::new(),
            image_thumbnail_url: String::new(),
            image_thumbnail_encrypted_json: String::new(),
            video_duration_ms: 0u64,
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
        return Some(TimelineEvent {
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            room_id: room_id.to_owned(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            body: String::new(),
            timestamp: event_item.timestamp().get().into(),
            msg_type: "m.redacted".to_owned(),
            source_url: String::new(),
            source_encrypted_json: String::new(),
            width: 0u64,
            height: 0u64,
            file_url: String::new(),
            file_encrypted_json: String::new(),
            file_name: String::new(),
            file_size: 0u64,
            image_filename: String::new(),
            audio_url: String::new(),
            audio_encrypted_json: String::new(),
            audio_duration_ms: 0u64,
            audio_waveform: Vec::new(),
            audio_mime: String::new(),
            video_thumbnail_url: String::new(),
            video_thumbnail_encrypted_json: String::new(),
            image_thumbnail_url: String::new(),
            image_thumbnail_encrypted_json: String::new(),
            video_duration_ms: 0u64,
            video_mime: String::new(),
            video_autoplay: false,
            video_loop: false,
            video_no_audio: false,
            video_hide_controls: false,
            video_gif: false,
            reactions: Vec::new(),
            // Receipts on a tombstone are meaningless — the original event
            // is gone; the redacted placeholder doesn't carry a reading
            // audience worth surfacing.
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
                (file.url.to_string(), serde_json::to_string(&ms).unwrap_or_default())
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
            file_url: String::new(),
            file_encrypted_json: String::new(),
            file_name: String::new(),
            file_size: 0u64,
            image_filename: String::new(),
            audio_url: String::new(),
            audio_encrypted_json: String::new(),
            audio_duration_ms: 0u64,
            audio_waveform: Vec::new(),
            audio_mime: String::new(),
            video_thumbnail_url: String::new(),
            video_thumbnail_encrypted_json: String::new(),
            image_thumbnail_url,
            image_thumbnail_encrypted_json,
            video_duration_ms: 0u64,
            video_mime: String::new(),
            video_autoplay: false,
            video_loop: false,
            video_no_audio: false,
            video_hide_controls: false,
            video_gif: false,
            reactions,
            read_receipts,
            in_reply_to_id: String::new(),
            in_reply_to_sender_name: String::new(),
            in_reply_to_body: String::new(),
            in_reply_to_image_url: String::new(),
            in_reply_to_image_encrypted_json: String::new(),
            is_edited: false,
            formatted_body: String::new(),
            blurhash: c.info.blurhash.as_deref().unwrap_or("").to_owned(),
            sticker_info_json: serde_json::to_string(&c.info).unwrap_or_else(|_| "{}".to_owned()),
            image_animated: c.info.is_animated.unwrap_or(false),
            pending_state: pending_state.clone(),
            pending_error: pending_error.clone(),
            pending_recoverable,
            pending_txn_id: pending_txn_id.clone(),
            location_lat: 0.0,
            location_lon: 0.0,
            location_description: String::new(),
            thread_root_id: String::new(),
            is_thread_root: false,
            thread_reply_count: 0,
            thread_latest_sender_name: String::new(),
            thread_latest_body: String::new(),
            thread_latest_ts: 0,
        });
    }

    let msg_content = match event_item.content() {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(msg),
            ..
        }) => msg,
        _ => return None,
    };

    let (
        body,
        formatted_body,
        msg_type,
        source_url,
        source_encrypted_json,
        width,
        height,
        file_url,
        file_encrypted_json,
        file_name,
        file_size,
        image_filename,
        audio_url,
        audio_encrypted_json,
        audio_duration_ms,
        audio_waveform,
        audio_mime,
        video_thumbnail_url,
        video_thumbnail_encrypted_json,
        video_duration_ms,
        video_mime,
        video_autoplay,
        video_loop,
        video_no_audio,
        video_hide_controls,
        video_gif,
        location_lat,
        location_lon,
        location_description,
    ) = match msg_content.msgtype() {
        MessageType::Text(t) => {
            let fmt = t
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
            (
                t.body.clone(),
                fmt,
                "m.text".to_owned(),
                String::new(), // source_url
                String::new(), // source_encrypted_json
                0u64,
                0u64,
                String::new(), // file_url
                String::new(), // file_encrypted_json
                String::new(),
                0u64,
                String::new(),
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::<u16>::new(),
                String::new(),
                String::new(), // video_thumbnail_url
                String::new(), // video_thumbnail_encrypted_json
                0u64,
                String::new(),
                false,
                false,
                false,
                false,
                false,
                0.0f64,
                0.0f64,
                String::new(),
            )
        }
        MessageType::Image(i) => {
            let (src_url, src_enc) = split_source(&i.source);
            let (w, h) = i
                .info
                .as_ref()
                .map(|info| {
                    (
                        info.width.map(|v| u64::from(v)).unwrap_or(0u64),
                        info.height.map(|v| u64::from(v)).unwrap_or(0u64),
                    )
                })
                .unwrap_or((0u64, 0u64));
            // MSC2530: filename field signals that body is a user caption.
            let img_filename = i.filename.clone().unwrap_or_default();
            (
                i.body.clone(),
                String::new(),
                "m.image".to_owned(),
                src_url,       // source_url
                src_enc,       // source_encrypted_json
                w,
                h,
                String::new(), // file_url
                String::new(), // file_encrypted_json
                String::new(),
                0u64,
                img_filename,
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::new(),
                String::new(),
                String::new(), // video_thumbnail_url
                String::new(), // video_thumbnail_encrypted_json
                0u64,
                String::new(),
                false,
                false,
                false,
                false,
                false,
                0.0f64,
                0.0f64,
                String::new(),
            )
        }
        MessageType::File(f) => {
            let (file_url, file_enc) = split_source(&f.source);
            let name = f.filename.clone().unwrap_or_else(|| f.body.clone());
            let size = f
                .info
                .as_ref()
                .and_then(|info| info.size)
                .map(|v| u64::from(v))
                .unwrap_or(0u64);
            (
                f.body.clone(),
                String::new(),
                "m.file".to_owned(),
                String::new(), // source_url
                String::new(), // source_encrypted_json
                0u64,
                0u64,
                file_url,      // file_url
                file_enc,      // file_encrypted_json
                name,
                size,
                String::new(),
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::new(),
                String::new(),
                String::new(), // video_thumbnail_url
                String::new(), // video_thumbnail_encrypted_json
                0u64,
                String::new(),
                false,
                false,
                false,
                false,
                false,
                0.0f64,
                0.0f64,
                String::new(),
            )
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
                (
                    a.body.clone(),
                    String::new(),
                    "m.voice".to_owned(),
                    String::new(), // source_url
                    String::new(), // source_encrypted_json
                    0u64,
                    0u64,
                    String::new(), // file_url
                    String::new(), // file_encrypted_json
                    String::new(),
                    0u64,
                    String::new(),
                    aud_url,       // audio_url
                    aud_enc,       // audio_encrypted_json
                    duration_ms,
                    waveform,
                    info_mime,
                    String::new(), // video_thumbnail_url
                    String::new(), // video_thumbnail_encrypted_json
                    0u64,
                    String::new(),
                    false,
                    false,
                    false,
                    false,
                    false,
                    0.0f64,
                    0.0f64,
                    String::new(),
                )
            } else {
                let name = a.filename.clone().unwrap_or_else(|| a.body.clone());
                let size = a
                    .info
                    .as_deref()
                    .and_then(|i| i.size)
                    .map(u64::from)
                    .unwrap_or(0u64);
                (
                    a.body.clone(),
                    String::new(),
                    "m.audio".to_owned(),
                    String::new(), // source_url
                    String::new(), // source_encrypted_json
                    0u64,
                    0u64,
                    String::new(), // file_url
                    String::new(), // file_encrypted_json
                    name,
                    size,
                    String::new(),
                    aud_url,       // audio_url
                    aud_enc,       // audio_encrypted_json
                    info_duration_ms,
                    Vec::new(),
                    info_mime,
                    String::new(), // video_thumbnail_url
                    String::new(), // video_thumbnail_encrypted_json
                    0u64,
                    String::new(),
                    false,
                    false,
                    false,
                    false,
                    false,
                    0.0f64,
                    0.0f64,
                    String::new(),
                )
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
            let mau = |key: &str| -> bool {
                let path = format!("/content/info/{}", key);
                event_item
                    .original_json()
                    .and_then(|raw| {
                        serde_json::from_str::<serde_json::Value>(raw.json().get()).ok()
                    })
                    .and_then(|json| json.pointer(&path)?.as_bool())
                    .unwrap_or(false)
            };
            let video_gif = mau("fi.mau.gif");
            let video_autoplay = mau("fi.mau.autoplay") || video_gif;
            let video_loop = mau("fi.mau.loop") || video_gif;
            let video_no_audio = mau("fi.mau.no_audio") || video_gif;
            let video_hide_controls = mau("fi.mau.hide_controls") || video_gif;
            (
                v.body.clone(),
                String::new(),
                "m.video".to_owned(),
                src_url,       // source_url
                src_enc,       // source_encrypted_json
                w,
                h,
                String::new(), // file_url
                String::new(), // file_encrypted_json
                String::new(),
                0u64,
                vid_filename,
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::new(),
                String::new(),
                thumb_url,     // video_thumbnail_url
                thumb_enc,     // video_thumbnail_encrypted_json
                dur_ms,
                mime,
                video_autoplay,
                video_loop,
                video_no_audio,
                video_hide_controls,
                video_gif,
                0.0f64,
                0.0f64,
                String::new(),
            )
        }
        MessageType::Emote(e) => {
            let fmt = e
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
            (
                e.body.clone(),
                fmt,
                "m.emote".to_owned(),
                String::new(), // source_url
                String::new(), // source_encrypted_json
                0u64,
                0u64,
                String::new(), // file_url
                String::new(), // file_encrypted_json
                String::new(),
                0u64,
                String::new(),
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::<u16>::new(),
                String::new(),
                String::new(), // video_thumbnail_url
                String::new(), // video_thumbnail_encrypted_json
                0u64,
                String::new(),
                false,
                false,
                false,
                false,
                false,
                0.0f64,
                0.0f64,
                String::new(),
            )
        }
        MessageType::Notice(n) => {
            let fmt = n
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
            (
                n.body.clone(),
                fmt,
                "m.notice".to_owned(),
                String::new(), // source_url
                String::new(), // source_encrypted_json
                0u64,
                0u64,
                String::new(), // file_url
                String::new(), // file_encrypted_json
                String::new(),
                0u64,
                String::new(),
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::<u16>::new(),
                String::new(),
                String::new(), // video_thumbnail_url
                String::new(), // video_thumbnail_encrypted_json
                0u64,
                String::new(),
                false,
                false,
                false,
                false,
                false,
                0.0f64,
                0.0f64,
                String::new(),
            )
        }
        MessageType::Location(l) => {
            let (lat, lon) = parse_geo_uri(l.geo_uri()).unwrap_or((0.0, 0.0));
            (
                l.body.clone(),
                String::new(),
                "m.location".to_owned(),
                String::new(), // source_url
                String::new(), // source_encrypted_json
                0u64,
                0u64,
                String::new(), // file_url
                String::new(), // file_encrypted_json
                String::new(),
                0u64,
                String::new(),
                String::new(), // audio_url
                String::new(), // audio_encrypted_json
                0u64,
                Vec::<u16>::new(),
                String::new(),
                String::new(), // video_thumbnail_url
                String::new(), // video_thumbnail_encrypted_json
                0u64,
                String::new(),
                false,
                false,
                false,
                false,
                false,
                lat,
                lon,
                l.body.clone(),
            )
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
    let (in_reply_to_id, in_reply_to_sender_name, in_reply_to_body,
         in_reply_to_image_url, in_reply_to_image_encrypted_json) =
        match event_item.content().in_reply_to() {
            None => (String::new(), String::new(), String::new(),
                     String::new(), String::new()),
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

    Some(TimelineEvent {
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
        msg_type,
        source_url,
        source_encrypted_json,
        width,
        height,
        file_url,
        file_encrypted_json,
        file_name,
        file_size,
        image_filename,
        audio_url,
        audio_encrypted_json,
        audio_duration_ms,
        audio_waveform,
        audio_mime,
        video_thumbnail_url,
        video_thumbnail_encrypted_json,
        image_thumbnail_url,
        image_thumbnail_encrypted_json,
        video_duration_ms,
        video_mime,
        video_autoplay,
        video_loop,
        video_no_audio,
        video_hide_controls,
        video_gif,
        reactions,
        read_receipts,
        in_reply_to_id,
        in_reply_to_sender_name,
        in_reply_to_body,
        in_reply_to_image_url,
        in_reply_to_image_encrypted_json,
        is_edited: msg_content.is_edited(),
        formatted_body,
        blurhash,
        sticker_info_json: String::new(),
        image_animated,
        pending_state,
        pending_error,
        pending_recoverable,
        pending_txn_id,
        location_lat,
        location_lon,
        location_description,
        thread_root_id,
        is_thread_root,
        thread_reply_count,
        thread_latest_sender_name,
        thread_latest_body,
        thread_latest_ts,
    })
}

#[cfg(test)]
mod pinned_action_tests {
    use super::pinned_events_action;

    #[test]
    fn pin_one() {
        assert_eq!(pinned_events_action(&["$a"], &[] as &[&str]), "pinned a message");
    }

    #[test]
    fn unpin_one() {
        assert_eq!(pinned_events_action(&[] as &[&str], &["$a"]), "unpinned a message");
    }

    #[test]
    fn pin_many() {
        assert_eq!(pinned_events_action(&["$a","$b","$c"], &[] as &[&str]), "pinned 3 messages");
    }

    #[test]
    fn unpin_many() {
        assert_eq!(pinned_events_action(&[] as &[&str], &["$a","$b"]), "unpinned 2 messages");
    }

    #[test]
    fn clear_all_three() {
        // "cleared" fires only when new list is empty AND old list had >=3 items.
        assert_eq!(pinned_events_action(&[] as &[&str], &["$a","$b","$c"]), "cleared all pinned messages");
    }

    #[test]
    fn unpin_two_below_threshold() {
        // Only 2 removed => threshold not met, so "unpinned N messages" fires instead.
        assert_eq!(pinned_events_action(&[] as &[&str], &["$a","$b"]), "unpinned 2 messages");
    }

    #[test]
    fn mixed_change() {
        assert_eq!(pinned_events_action(&["$b"], &["$a"]), "changed the pinned messages");
    }

    #[test]
    fn no_change() {
        // Callers should filter out no-op state events (old == new) before calling.
        // The fallthrough branch returns this, but a real caller won't reach it.
        assert_eq!(pinned_events_action(&["$a"], &["$a"]), "changed the pinned messages");
    }
}
