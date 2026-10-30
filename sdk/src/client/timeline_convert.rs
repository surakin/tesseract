//! Convert matrix-sdk-ui timeline items into the FFI `TimelineEvent`
//! struct consumed by the C++ bridge. Includes the per-msgtype dispatcher
//! and supporting helpers (media-source splitting, reaction/receipt
//! aggregation, embedded-event preview, geo URI parsing).
//!
//! Split out of `client/mod.rs` in the modularization refactor; behavior unchanged.

use super::{text_code, text_code_arg, user_text};
#[cfg(not(test))]
use crate::ffi::{ReactionGroup, ReadReceipt};

// `map_bundled_url_previews`, `message_type_fields` and the forward helpers
// (and their unit tests) are compiled in both configurations, so these must
// be imported unconditionally — under `cfg(test)` they resolve to the
// pure-Rust stubs in `crate::ffi`.
use crate::ffi::{TimelineEvent, UrlPreviewFfi};
use crate::msc::msc4553;
use matrix_sdk::ruma::events::room::message::{MessageType, RoomMessageEventContent};
use matrix_sdk::ruma::events::sticker::StickerEventContent;

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

/// Sender display name, avatar and MSC4426 status flattened for the FFI.
/// All empty while the profile is still pending.
#[cfg(not(test))]
struct SenderProfileFfi {
    sender_name: String,
    sender_avatar_url: String,
    sender_status_emoji: String,
    sender_status_text: String,
}

#[cfg(not(test))]
fn sender_profile_ffi(
    details: &TimelineDetails<matrix_sdk_ui::timeline::Profile>,
) -> SenderProfileFfi {
    let TimelineDetails::Ready(p) = details else {
        return SenderProfileFfi {
            sender_name: String::new(),
            sender_avatar_url: String::new(),
            sender_status_emoji: String::new(),
            sender_status_text: String::new(),
        };
    };
    let (sender_status_emoji, sender_status_text) = status_ffi(p.status.as_ref());
    SenderProfileFfi {
        sender_name: crate::text_utils::strip_bidi_overrides(
            p.display_name.as_deref().unwrap_or_default(),
        ),
        sender_avatar_url: p
            .avatar_url
            .as_ref()
            .map(|u| u.to_string())
            .unwrap_or_default(),
        sender_status_emoji,
        sender_status_text,
    }
}

/// `(emoji, text)` of an MSC4426 status; both empty when unset.
pub(super) fn status_ffi(
    status: Option<&matrix_sdk::ruma::profile::StatusProfileField>,
) -> (String, String) {
    status
        .map(|s| (s.emoji.clone(), s.text.clone()))
        .unwrap_or_default()
}

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
        sender_status_emoji: String::new(),
        sender_status_text: String::new(),
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
        image_mime: String::new(),
        file_mime: String::new(),
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
        bundled_url_previews: Vec::new(),
        bundled_url_previews_present: false,
        reactions: Vec::new(),
        read_receipts: Vec::new(),
        in_reply_to_id: String::new(),
        in_reply_to_sender_name: String::new(),
        in_reply_to_body: String::new(),
        in_reply_to_formatted_body: String::new(),
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
        pending_error_users: Vec::new(),
        location_lat: 0.0,
        location_lon: 0.0,
        location_description: String::new(),
        location_live_share: false,
        location_live: false,
        location_live_expires_ms: 0,
        location_updated_ms: 0,
        location_awaiting_fix: false,
        poll_answers: Vec::new(),
        poll_max_selections: 0,
        poll_ended: false,
        poll_results_visible: false,
        poll_total_votes: 0,
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
        membership_reason: String::new(),
        room_name_new: String::new(),
        room_name_old: String::new(),
        replacement_room_id: String::new(),
        gallery_items: Vec::new(),
        forwarded: false,
        forwarded_sender: String::new(),
        forwarded_sender_name: String::new(),
        forwarded_room_id: String::new(),
        forwarded_event_id: String::new(),
        forwarded_origin_ts: 0,
    }
}

#[cfg(test)]
pub(super) fn ffi_event_defaults() -> TimelineEvent {
    TimelineEvent::default()
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
        UtdCause::SentBeforeWeJoined => "\u{1}utd_sent_before_join",
        UtdCause::VerificationViolation => "\u{1}utd_identity_changed",
        UtdCause::UnsignedDevice => "\u{1}utd_unsigned_device",
        UtdCause::UnknownDevice => "\u{1}utd_unknown_device",
        UtdCause::HistoricalMessageAndBackupIsDisabled => "\u{1}utd_backup_off",
        UtdCause::WithheldForUnverifiedOrInsecureDevice => "\u{1}utd_withheld_insecure",
        UtdCause::WithheldBySender => "\u{1}utd_withheld_sender",
        UtdCause::HistoricalMessageAndDeviceIsUnverified => "\u{1}utd_verify_device",
        UtdCause::Unknown => "\u{1}utd_unknown",
    }
}

/// Machine-readable `pending_error` for a local echo that matrix-sdk parked
/// because of an MSC4153 encryption check, or `None` for any other failure.
/// It reaches the UI as `pending_error`, with the users the check names (none
/// for `own_verification_required`) as `pending_error_users`.
#[cfg(not(test))]
fn crypto_send_block_code(error: &matrix_sdk::Error) -> Option<(&'static str, Vec<String>)> {
    use matrix_sdk_base::store::QueueWedgeError;
    match QueueWedgeError::from(error) {
        QueueWedgeError::IdentityViolations { users } => Some((
            "identity_violation",
            users.iter().map(|u| u.to_string()).collect(),
        )),
        QueueWedgeError::InsecureDevices { user_device_map } => Some((
            "insecure_devices",
            user_device_map.keys().map(|u| u.to_string()).collect(),
        )),
        QueueWedgeError::CrossVerificationRequired => {
            Some(("own_verification_required", Vec::new()))
        }
        _ => None,
    }
}

#[cfg(not(test))]
pub(super) async fn collect_reactions(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    room: &Room,
    me: Option<&UserId>,
) -> Vec<ReactionGroup> {
    let table = event_item.reactions();
    if table.is_empty() {
        return Vec::new();
    }

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
    for (uid, receipt) in table.iter() {
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
        let timestamp_ms = receipt.ts.map(|ts| ts.get().into()).unwrap_or(0);
        out.push(ReadReceipt {
            user_id: uid.to_string(),
            display_name,
            avatar_url,
            timestamp_ms,
        });
    }
    // Newest receipt first; unknown timestamps (0) sort last. Ties break on
    // user_id for a deterministic order across calls.
    out.sort_by(|a, b| {
        b.timestamp_ms
            .cmp(&a.timestamp_ms)
            .then_with(|| a.user_id.cmp(&b.user_id))
    });
    out
}

/// Split a `MediaSource` into the two FFI fields used by the C++ bridge:
/// - `url`:           the plain mxc:// URI (both plain and encrypted carry this)
/// - `encrypted_json`: non-empty only when the source is encrypted; full JSON blob
///                     understood by `fetch_source_bytes` for decryption
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
pub(super) fn split_source_opt(
    source: Option<&matrix_sdk::ruma::events::room::MediaSource>,
) -> (String, String) {
    match source {
        Some(s) => split_source(s),
        None => (String::new(), String::new()),
    }
}

/// True for an absolute http/https URL. `matched_url` is sender-controlled
/// and a card click hands it to the OS URL opener, so the body-substring
/// check alone is not enough. `Url::parse` lowercases the scheme.
fn is_http_url(s: &str) -> bool {
    url::Url::parse(s)
        .map(|u| matches!(u.scheme(), "http" | "https"))
        .unwrap_or(false)
}

/// Map the MSC4095 bundled URL previews carried on an `m.text` message's
/// content (`com.beeper.linkpreviews` / `m.url_previews`) into the FFI shape.
///
/// An entry is kept only when it has a `matched_url` that appears verbatim in
/// `body` as a real link span — not merely as a substring, so a URL that only
/// occurs inside a Markdown code region (fenced block or inline code span) in
/// `body` doesn't qualify, matching the sender-side exclusion in
/// `url_preview_gen::preview_candidate_urls`. This is the MSC's recommended
/// receiver-side guard against a sender attaching a preview card for a URL
/// that isn't meant as a link in the visible text. Entries with no
/// `matched_url`, or one that doesn't qualify, are dropped. Entries that carry
/// only a `matched_url` (no title/description/image) are kept as-is; the C++
/// side recognises them as "ask the homeserver to preview this URL instead".
///
/// The encrypted-image branch serialises the `EncryptedFile` through the same
/// `MediaSource::Encrypted` JSON shape `split_source` emits, so the C++/Rust
/// media-fetch path is identical to every other encrypted thumbnail.
fn map_bundled_url_previews(
    previews: &[matrix_sdk::ruma::events::room::message::UrlPreview],
    body: &str,
) -> Vec<UrlPreviewFfi> {
    use matrix_sdk::ruma::events::room::message::PreviewImageSource;
    use matrix_sdk::ruma::events::room::MediaSource;

    let preview_spans = crate::text_utils::find_preview_url_spans(body);

    previews
        .iter()
        .filter_map(|p| {
            let matched_url = p.matched_url.clone()?;
            if !preview_spans.iter().any(|s| s.url == matched_url) {
                return None;
            }
            if !is_http_url(&matched_url) {
                return None;
            }
            let (image_url, image_encrypted_json, image_width, image_height) = match &p.image {
                None => (String::new(), String::new(), 0u64, 0u64),
                Some(img) => {
                    let w = img.width.map(u64::from).unwrap_or(0);
                    let h = img.height.map(u64::from).unwrap_or(0);
                    match &img.source {
                        PreviewImageSource::Url(uri) => (uri.to_string(), String::new(), w, h),
                        PreviewImageSource::EncryptedImage(file) => {
                            let src = MediaSource::Encrypted(Box::new(file.clone()));
                            (
                                file.url.to_string(),
                                serde_json::to_string(&src).unwrap_or_default(),
                                w,
                                h,
                            )
                        }
                    }
                }
            };
            Some(UrlPreviewFfi {
                matched_url,
                title: p.title.clone().unwrap_or_default(),
                description: p.description.clone().unwrap_or_default(),
                canonical_url: p.url.clone().unwrap_or_default(),
                image_url,
                image_encrypted_json,
                image_width,
                image_height,
            })
        })
        .collect()
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
    let remove_reply_fallback = json
        .pointer("/content/m.relates_to/m.in_reply_to")
        .is_some();
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
    // f64::parse accepts NaN, inf and 1e999; the map code cannot place those.
    crate::maps_link::checked_coords(lat, lon)
}

/// Plain-data view of a live-location share for the FFI event. Primitive
/// inputs keep this testable: matrix-sdk-ui's `BeaconInfo` can't be built
/// outside that crate.
pub(crate) struct LiveLocationFields {
    pub lat: f64,
    pub lon: f64,
    pub live: bool,
    pub expires_ms: u64,
    pub updated_ms: u64,
    pub awaiting_fix: bool,
}

pub(crate) fn live_location_fields(
    latest_geo_uri: Option<&str>,
    latest_ts_ms: u64,
    is_live: bool,
    start_ts_ms: u64,
    timeout_ms: u64,
) -> LiveLocationFields {
    let fix = latest_geo_uri.and_then(parse_geo_uri);
    LiveLocationFields {
        lat: fix.map_or(0.0, |p| p.0),
        lon: fix.map_or(0.0, |p| p.1),
        live: is_live,
        expires_ms: start_ts_ms.saturating_add(timeout_ms),
        updated_ms: if fix.is_some() { latest_ts_ms } else { 0 },
        awaiting_fix: fix.is_none(),
    }
}

/// MSC3381: anything that is not explicitly "disclosed" (including unknown
/// custom kinds) is treated as undisclosed.
pub(crate) fn poll_is_disclosed(kind: &matrix_sdk::ruma::events::poll::start::PollKind) -> bool {
    matches!(
        kind,
        matrix_sdk::ruma::events::poll::start::PollKind::Disclosed
    )
}

pub(crate) struct PollFields {
    pub answers: Vec<crate::ffi::PollAnswerFfi>,
    pub total_votes: u32,
    pub results_visible: bool,
}

/// Map a poll's answers + voter lists to the FFI shape. Counts are withheld
/// (zeroed) while an undisclosed poll is open — MSC3381 leaves enforcing that
/// to clients, and doing it here means the UI never holds the counts. The
/// user's own selection is always reported.
pub(crate) fn poll_fields(
    answers: &[(String, String, Vec<String>)],
    disclosed: bool,
    ended: bool,
    me: &str,
) -> PollFields {
    let results_visible = disclosed || ended;
    let mut total_votes = 0u32;
    let answers = answers
        .iter()
        .map(|(id, text, voters)| {
            let n = u32::try_from(voters.len()).unwrap_or(u32::MAX);
            if results_visible {
                total_votes = total_votes.saturating_add(n);
            }
            crate::ffi::PollAnswerFfi {
                id: id.clone(),
                text: text.clone(),
                votes: if results_visible { n } else { 0 },
                mine: voters.iter().any(|v| v == me),
            }
        })
        .collect();
    PollFields {
        answers,
        total_votes,
        results_visible,
    }
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
        text_code("pin_cleared")
    } else if added == 1 && removed == 0 {
        text_code("pin_added")
    } else if added == 0 && removed == 1 {
        text_code("pin_removed")
    } else if added > 1 && removed == 0 {
        text_code_arg("pin_added_n", added)
    } else if added == 0 && removed > 1 {
        text_code_arg("pin_removed_n", removed)
    } else {
        text_code("pin_changed")
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

/// Discriminant for a profile-only `m.room.member` change (the user was already
/// joined; display name and/or avatar changed). Each argument is `None` when
/// that field did not change, else whether the NEW value is set (`false` =
/// removed). Both changing yields `profile_changed`. Stable, English-free, like
/// `membership_action_str`; the C++ side owns the user-facing phrases.
pub(crate) fn profile_change_action(
    avatar_new_is_set: Option<bool>,
    name_new_is_set: Option<bool>,
) -> Option<&'static str> {
    match (avatar_new_is_set, name_new_is_set) {
        (None, None) => None,
        (Some(_), Some(_)) => Some("profile_changed"),
        (Some(true), None) => Some("avatar_changed"),
        (Some(false), None) => Some("avatar_removed"),
        (None, Some(true)) => Some("display_name_changed"),
        (None, Some(false)) => Some("display_name_removed"),
    }
}

/// Whether an `m.room.member` row reaches the UI. Membership transitions are
/// gated by "Show room join/leave events" (`show`); the user's own profile
/// changes (display name / avatar) are always shown so a command like
/// `/myroomavatar` gives visible confirmation. Everything that is not an
/// `m.room.member` row is unaffected.
pub(crate) fn membership_event_visible(
    msg_type: &str,
    action: &str,
    target_user_id: &str,
    show: bool,
    me: Option<&str>,
) -> bool {
    show || msg_type != "m.room.member"
        || (is_profile_action(action) && me.is_some_and(|m| m == target_user_id))
}

/// True for the actions `profile_change_action` produces.
pub(crate) fn is_profile_action(action: &str) -> bool {
    matches!(
        action,
        "avatar_changed"
            | "avatar_removed"
            | "display_name_changed"
            | "display_name_removed"
            | "profile_changed"
    )
}

/// Extract the FFI shape for one `m.gallery` (MSC4274) item. Pure function —
/// no live SDK connection state — so it's unit-testable directly against a
/// synthetic `GalleryItemType`, unlike the full per-event dispatcher below
/// which needs a live `EventTimelineItem`. Mirrors the field-extraction
/// logic in the singular `MessageType::Image/File/Audio/Video` arms of
/// `timeline_item_to_ffi`, since `GalleryItemType` wraps the exact same
/// inner content types (`ImageMessageEventContent` etc).
pub(crate) fn gallery_item_ffi_from_type(
    item: &matrix_sdk::ruma::events::room::message::GalleryItemType,
) -> crate::ffi::GalleryItemFfi {
    use matrix_sdk::ruma::events::room::message::GalleryItemType;

    let defaults = crate::ffi::GalleryItemFfi {
        itemtype: String::new(),
        body: String::new(),
        source_url: String::new(),
        source_encrypted_json: String::new(),
        thumbnail_url: String::new(),
        thumbnail_encrypted_json: String::new(),
        width: 0,
        height: 0,
        mime: String::new(),
        filename: String::new(),
        file_size: 0,
        duration_ms: 0,
        waveform: Vec::new(),
        blurhash: String::new(),
        animated: false,
    };

    match item {
        GalleryItemType::Image(i) => {
            let (src_url, src_enc) = split_source(&i.source);
            let (w, h, blurhash, animated) = i
                .info
                .as_ref()
                .map(|info| {
                    (
                        info.width.map(u64::from).unwrap_or(0u64),
                        info.height.map(u64::from).unwrap_or(0u64),
                        info.blurhash.clone().unwrap_or_default(),
                        info.is_animated.unwrap_or(false),
                    )
                })
                .unwrap_or_default();
            crate::ffi::GalleryItemFfi {
                itemtype: "m.image".to_owned(),
                body: i.body.clone(),
                source_url: src_url,
                source_encrypted_json: src_enc,
                width: w,
                height: h,
                filename: i.filename.clone().unwrap_or_default(),
                blurhash,
                animated,
                ..defaults
            }
        }
        GalleryItemType::File(f) => {
            let (file_url, file_enc) = split_source(&f.source);
            let size = f
                .info
                .as_ref()
                .and_then(|info| info.size)
                .map(u64::from)
                .unwrap_or(0u64);
            crate::ffi::GalleryItemFfi {
                itemtype: "m.file".to_owned(),
                body: f.body.clone(),
                source_url: file_url,
                source_encrypted_json: file_enc,
                filename: f.filename.clone().unwrap_or_default(),
                file_size: size,
                ..defaults
            }
        }
        GalleryItemType::Audio(a) => {
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
            crate::ffi::GalleryItemFfi {
                itemtype: "m.audio".to_owned(),
                body: a.body.clone(),
                source_url: aud_url,
                source_encrypted_json: aud_enc,
                filename: a.filename.clone().unwrap_or_default(),
                mime: info_mime,
                duration_ms,
                waveform,
                ..defaults
            }
        }
        GalleryItemType::Video(v) => {
            let (src_url, src_enc) = split_source(&v.source);
            let (w, h, dur_ms, mime, thumb_url, thumb_enc, blurhash) = v
                .info
                .as_ref()
                .map(|info| {
                    let w = info.width.map(u64::from).unwrap_or(0u64);
                    let h = info.height.map(u64::from).unwrap_or(0u64);
                    let dur = info.duration.map(|d| d.as_millis() as u64).unwrap_or(0u64);
                    let mime = info.mimetype.clone().unwrap_or_default();
                    let (tu, te) = split_source_opt(info.thumbnail_source.as_ref());
                    let bh = info.blurhash.clone().unwrap_or_default();
                    (w, h, dur, mime, tu, te, bh)
                })
                .unwrap_or_default();
            crate::ffi::GalleryItemFfi {
                itemtype: "m.video".to_owned(),
                body: v.body.clone(),
                source_url: src_url,
                source_encrypted_json: src_enc,
                width: w,
                height: h,
                filename: v.filename.clone().unwrap_or_default(),
                mime,
                duration_ms: dur_ms,
                thumbnail_url: thumb_url,
                thumbnail_encrypted_json: thumb_enc,
                blurhash,
                ..defaults
            }
        }
        // Unknown/future itemtype (including MSC4274's `_Custom`): fall back
        // to the public itemtype()/body() accessors, which work for every
        // variant including hidden ones. GalleryItemType is #[non_exhaustive]
        // so this arm also covers itemtypes ruma adds after this match was
        // written.
        _ => crate::ffi::GalleryItemFfi {
            itemtype: item.itemtype().to_owned(),
            body: item.body().to_owned(),
            ..defaults
        },
    }
}

/// The snippet text of a message type: the body of text, a code for the rest.
pub(crate) fn message_snippet(msgtype: &MessageType) -> String {
    match msgtype {
        MessageType::Text(t) => user_text(&t.body),
        MessageType::Image(_) => text_code("image"),
        MessageType::File(_) => text_code("file"),
        MessageType::Audio(a) => {
            if a.voice.is_some() {
                text_code("voice")
            } else {
                text_code("audio")
            }
        }
        MessageType::Video(_) => text_code("video"),
        MessageType::Gallery(_) => text_code("gallery"),
        _ => text_code("message"),
    }
}

/// The sanitized HTML `formatted_body` of a message type, or "" when it has
/// none (non-text, plain-text, or non-HTML format). Reply-fallback
/// (`<mx-reply>`) is stripped, matching the main message path.
pub(crate) fn message_snippet_html(msgtype: &MessageType) -> String {
    use matrix_sdk::ruma::events::room::message::MessageFormat;
    let MessageType::Text(t) = msgtype else {
        return String::new();
    };
    match t.formatted.as_ref() {
        Some(f) if f.format == MessageFormat::Html => {
            crate::html_sanitize::sanitize_formatted_body(&f.body, true)
        }
        _ => String::new(),
    }
}

/// Shared so the in-reply-to quote block and the thread latest-event preview
/// emit identical snippet text.
#[cfg(not(test))]
pub(crate) fn msglike_snippet(content: &TimelineItemContent) -> String {
    match content {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(m),
            ..
        }) => message_snippet(m.msgtype()),
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Sticker(_),
            ..
        }) => text_code("sticker"),
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Poll(_),
            ..
        }) => text_code("poll"),
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Redacted,
            ..
        }) => text_code("deleted"),
        _ => String::new(),
    }
}

/// The sanitized HTML `formatted_body` of a msg-like event, or "" when it
/// has none. See [`message_snippet_html`].
#[cfg(not(test))]
pub(crate) fn msglike_snippet_html(content: &TimelineItemContent) -> String {
    match content {
        TimelineItemContent::MsgLike(MsgLikeContent {
            kind: MsgLikeKind::Message(m),
            ..
        }) => message_snippet_html(m.msgtype()),
        _ => String::new(),
    }
}

/// The (body, html) snippet of a forward (MSC4553) from the raw JSON of its
/// event: the forwarded content, not the "Forwarded from ..." fallback the
/// typed content holds. `None` when the event is not a forward with valid
/// message content; the caller then keeps the snippet it has.
pub(crate) fn forwarded_snippet_from_event_json(json: &str) -> Option<(String, String)> {
    if !msc4553::might_be_forward(json) {
        return None;
    }
    let event: serde_json::Value = serde_json::from_str(json).ok()?;
    if event["type"].as_str() != Some("m.room.message") {
        return None;
    }
    let shown = msc4553::display_event(&event)?;
    let content: RoomMessageEventContent =
        serde_json::from_value(shown["content"].clone()).ok()?;
    Some((
        message_snippet(&content.msgtype),
        message_snippet_html(&content.msgtype),
    ))
}

/// Whether a snippet of this message type may be a forward's fallback text,
/// so worth a look at the raw event. Media forwards already snippet as codes.
pub(crate) fn may_be_forward_fallback(msgtype: &MessageType) -> bool {
    matches!(msgtype, MessageType::Text(t) if msc4553::fallback_may_be_forward(&t.body))
}

/// The (body, html) snippet of the forward `event_id` in `room`, read from the
/// local event cache (no network), when `msgtype` looks like a forward's
/// fallback text. `None` keeps the caller's own snippet.
#[cfg(not(test))]
pub(crate) async fn forwarded_snippet(
    room: &Room,
    event_id: &matrix_sdk::ruma::EventId,
    msgtype: &MessageType,
) -> Option<(String, String)> {
    if !may_be_forward_fallback(msgtype) {
        return None;
    }
    let (cache, _handles) = room.event_cache().await.ok()?;
    let ev = cache.find_event(event_id).await.ok().flatten()?;
    forwarded_snippet_from_event_json(ev.raw().json().get())
}

/// [`forwarded_snippet`] for an embedded event (a reply target or a thread's
/// latest reply). Local echoes, which have no event id yet, are not looked up.
#[cfg(not(test))]
async fn forwarded_embedded_snippet(
    room: &Room,
    embedded: &matrix_sdk_ui::timeline::EmbeddedEvent,
) -> Option<(String, String)> {
    use matrix_sdk_ui::timeline::TimelineEventItemId;
    let TimelineItemContent::MsgLike(MsgLikeContent {
        kind: MsgLikeKind::Message(m),
        ..
    }) = &embedded.content
    else {
        return None;
    };
    let TimelineEventItemId::EventId(event_id) = &embedded.identifier else {
        return None;
    };
    forwarded_snippet(room, event_id, m.msgtype()).await
}

/// Extract (sender_display_name, body_snippet, formatted_body, timestamp_ms)
/// from an embedded event (a thread's latest reply, or an in-reply-to target).
#[cfg(not(test))]
pub(super) async fn embedded_event_preview(
    room: &Room,
    embedded: &matrix_sdk_ui::timeline::EmbeddedEvent,
) -> (String, String, String, u64) {
    let name = match &embedded.sender_profile {
        TimelineDetails::Ready(p) => p
            .display_name
            .clone()
            .unwrap_or_else(|| embedded.sender.to_string()),
        _ => embedded.sender.to_string(),
    };
    let (body, formatted) = match forwarded_embedded_snippet(room, embedded).await {
        Some(shown) => shown,
        None => (
            msglike_snippet(&embedded.content),
            msglike_snippet_html(&embedded.content),
        ),
    };
    let ts: u64 = embedded.timestamp.get().into();
    (name, body, formatted, ts)
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

/// Extract (event_id, sender_display_name, body_snippet, image_url,
/// image_encrypted_json) for the event this item is replying to, if any.
/// `TimelineItemContent::in_reply_to()` is generic over every `MsgLikeKind`
/// (text messages, stickers, ...), so this is safe to call for any msg-like
/// event, not just `m.room.message`.
#[cfg(not(test))]
pub(super) async fn extract_in_reply_to(
    room: &Room,
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
) -> (String, String, String, String, String, String) {
    match event_item.content().in_reply_to() {
        None => (
            String::new(),
            String::new(),
            String::new(),
            String::new(),
            String::new(),
            String::new(),
        ),
        Some(details) => {
            let id = details.event_id.to_string();
            let (rname, rbody, rfmt, img_url, img_enc) = match &details.event {
                TimelineDetails::Ready(replied) => {
                    let (name, snippet, formatted, _ts) =
                        embedded_event_preview(room, replied).await;
                    let (iu, ie) = reply_image_source(replied);
                    (name, snippet, formatted, iu, ie)
                }
                _ => (
                    String::new(),
                    String::new(),
                    String::new(),
                    String::new(),
                    String::new(),
                ),
            };
            (id, rname, rbody, rfmt, img_url, img_enc)
        }
    }
}

// ---------------------------------------------------------------------------
// MSC4553 forwarded messages
// ---------------------------------------------------------------------------

/// What an event with an MSC4553 `forwarded` object shows in place of its own
/// (fallback) content.
pub(crate) struct ForwardView {
    pub forwarded: msc4553::Forwarded,
    /// The content to render: the forwarded content when it is valid, else
    /// the event's own original content; without its relation and mentions.
    pub content: serde_json::Value,
}

/// The forward view of an event, from the `content` of its *original* JSON.
/// Never the latest edit: a forward can't be edited, so an edit of one is not
/// applied. `None` when the event is not a forward.
pub(crate) fn forward_view(
    original_content: &serde_json::Value,
    event_type: &str,
) -> Option<ForwardView> {
    let forwarded = msc4553::parse(original_content, event_type)?;
    let content =
        msc4553::without_relations(msc4553::display_content(original_content, &forwarded).clone());
    Some(ForwardView { forwarded, content })
}

/// Mark `fields` as a forward and fill the attribution. `sender_name` is the
/// original sender's display name as far as the caller could resolve it. The
/// ids come from the sender of the forward, so one that is not a valid Matrix
/// ID is dropped rather than handed to the UI.
pub(crate) fn apply_forward(
    fields: &mut TimelineEvent,
    fwd: &msc4553::Forwarded,
    sender_name: String,
) {
    use matrix_sdk::ruma::{EventId, RoomId, UserId};
    let valid = |id: &Option<String>, ok: fn(&str) -> bool| -> String {
        id.as_deref()
            .filter(|s| ok(s))
            .unwrap_or_default()
            .to_owned()
    };
    fields.forwarded = true;
    fields.forwarded_sender = valid(&fwd.sender, |s| UserId::parse(s).is_ok());
    fields.forwarded_sender_name = if fields.forwarded_sender.is_empty() {
        String::new()
    } else {
        sender_name
    };
    fields.forwarded_room_id = valid(&fwd.room_id, |s| RoomId::parse(s).is_ok());
    fields.forwarded_event_id = valid(&fwd.event_id, |s| EventId::parse(s).is_ok());
    fields.forwarded_origin_ts = fwd.origin_server_ts.filter(|ts| *ts > 0).unwrap_or(0);
}

/// The sanitized HTML `formatted_body` of raw message `content`; empty when
/// it has none. A forward never keeps a reply fallback, so it is always cut.
fn sanitized_html(content: &serde_json::Value) -> String {
    match (
        content["format"].as_str(),
        content["formatted_body"].as_str(),
    ) {
        (Some("org.matrix.custom.html"), Some(html)) => {
            crate::html_sanitize::sanitize_formatted_body(html, true)
        }
        _ => String::new(),
    }
}

/// The FFI fields of a forwarded message, mapped from the content it shows
/// exactly as an ordinary message is. `None` when that content isn't a
/// message the timeline shows; the caller then uses the typed content.
fn forwarded_message_fields(view: &ForwardView) -> Option<TimelineEvent> {
    let content: RoomMessageEventContent = serde_json::from_value(view.content.clone()).ok()?;
    message_type_fields(
        &content.msgtype,
        &|_| sanitized_html(&view.content),
        view.content.get("info"),
    )
}

/// Same for a forwarded sticker.
fn forwarded_sticker_fields(view: &ForwardView) -> Option<TimelineEvent> {
    let content: StickerEventContent = serde_json::from_value(view.content.clone()).ok()?;
    Some(sticker_fields(&content))
}

/// The per-sticker FFI fields; the caller layers the sender, reply, thread and
/// pending fields on top, as for `message_type_fields`.
fn sticker_fields(c: &StickerEventContent) -> TimelineEvent {
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
    TimelineEvent {
        body: c.body.clone(),
        msg_type: "m.sticker".to_owned(),
        source_url,
        source_encrypted_json,
        width: c.info.width.map(u64::from).unwrap_or(0),
        height: c.info.height.map(u64::from).unwrap_or(0),
        image_thumbnail_url,
        image_thumbnail_encrypted_json,
        blurhash: c.info.blurhash.as_deref().unwrap_or("").to_owned(),
        sticker_info_json: serde_json::to_string(&c.info).unwrap_or_else(|_| "{}".to_owned()),
        image_animated: c.info.is_animated.unwrap_or(false),
        ..ffi_event_defaults()
    }
}

/// The forward view of a timeline item, from its original JSON. Parses the
/// JSON only when it mentions a `forwarded` key at all.
#[cfg(not(test))]
fn original_forward(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
    event_type: &str,
) -> Option<ForwardView> {
    let raw = event_item.original_json()?.json().get();
    if !msc4553::might_be_forward(raw) {
        return None;
    }
    let json: serde_json::Value = serde_json::from_str(raw).ok()?;
    forward_view(&json["content"], event_type)
}

/// The raw `content.info` object of a timeline item's original JSON.
#[cfg(not(test))]
fn original_content_info(
    event_item: &matrix_sdk_ui::timeline::EventTimelineItem,
) -> Option<serde_json::Value> {
    event_item
        .original_json()
        .and_then(|raw| serde_json::from_str::<serde_json::Value>(raw.json().get()).ok())
        .and_then(|json| json.pointer("/content/info").cloned())
}

/// Display name of a forward's original sender, from the current room's
/// member cache (no network). Empty when the sender isn't known there; the UI
/// then shows the ID.
#[cfg(not(test))]
async fn forwarded_sender_name(room: &Room, fwd: &msc4553::Forwarded) -> String {
    let Some(uid) = fwd.sender.as_deref().and_then(|s| UserId::parse(s).ok()) else {
        return String::new();
    };
    match room.get_member_no_sync(&uid).await {
        Ok(Some(m)) => {
            crate::text_utils::strip_bidi_overrides(m.display_name().unwrap_or_default())
        }
        _ => String::new(),
    }
}

/// The per-msgtype FFI fields of a message: each arm sets only what differs
/// from the zeroed base, and the caller layers the sender, reaction, reply,
/// thread and pending fields on top. Shared by the ordinary path (the
/// timeline's typed content) and the forward path (the forwarded content,
/// deserialized), so both map every message type the same way.
///
/// `sanitize` turns the `formatted_body` HTML the type carries into what the UI
/// may render; `info_json` is the raw `info` object, where the `fi.mau.*`
/// video hints live. `None` for message types the timeline doesn't show.
fn message_type_fields(
    msgtype: &MessageType,
    sanitize: &dyn Fn(String) -> String,
    info_json: Option<&serde_json::Value>,
) -> Option<TimelineEvent> {
    let mut fields = match msgtype {
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
            let fmt = sanitize(fallback);
            // MSC4095: bundled URL previews. `Some` (even empty) means the
            // sender controls previews for this message; `None` leaves the
            // legacy homeserver-preview path in charge.
            let (bundled_url_previews, bundled_url_previews_present) =
                match t.url_previews.as_deref() {
                    Some(previews) => (map_bundled_url_previews(previews, &t.body), true),
                    None => (Vec::new(), false),
                };
            TimelineEvent {
                body: t.body.clone(),
                formatted_body: fmt,
                msg_type: "m.text".to_owned(),
                bundled_url_previews,
                bundled_url_previews_present,
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
                image_mime: i
                    .info
                    .as_ref()
                    .and_then(|info| info.mimetype.clone())
                    .unwrap_or_default(),
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
                file_mime: f
                    .info
                    .as_ref()
                    .and_then(|info| info.mimetype.clone())
                    .unwrap_or_default(),
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
            // fi.mau.* vendor hints live in the raw `info` object.
            let mau = |key: &str| -> bool {
                info_json
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
            let fmt = sanitize(fallback);
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
            let fmt = sanitize(fallback);
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
        MessageType::Gallery(g) => {
            let fallback = g
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
            // Caption HTML goes through the same sanitiser as text: matrix-sdk-ui
            // does not sanitise gallery content, so raw tags would reach the parser.
            // The fallback is sanitised too, for when the event JSON is unavailable.
            let fallback = crate::html_sanitize::sanitize_formatted_body(&fallback, false);
            let formatted_body = sanitize(fallback);
            TimelineEvent {
                body: g.body.clone(),
                formatted_body,
                msg_type: "m.gallery".to_owned(),
                gallery_items: g.itemtypes.iter().map(gallery_item_ffi_from_type).collect(),
                ..ffi_event_defaults()
            }
        }
        _ => return None,
    };

    let blurhash = match msgtype {
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

    let image_animated = match msgtype {
        MessageType::Image(i) => i
            .info
            .as_ref()
            .and_then(|info| info.is_animated)
            .unwrap_or(false),
        _ => false,
    };

    let image_thumbnail: (String, String) = match msgtype {
        MessageType::Image(i) => split_source_opt(
            i.info
                .as_ref()
                .and_then(|info| info.thumbnail_source.as_ref()),
        ),
        _ => (String::new(), String::new()),
    };

    fields.blurhash = blurhash;
    fields.image_animated = image_animated;
    (
        fields.image_thumbnail_url,
        fields.image_thumbnail_encrypted_json,
    ) = image_thumbnail;
    Some(fields)
}

#[cfg(not(test))]
pub(super) async fn timeline_item_to_ffi(
    item: &Arc<TimelineItem>,
    room_id: &str,
    room: &Room,
    me: Option<&UserId>,
) -> Option<TimelineEvent> {
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

    // m.room.pinned_events and m.room.name state events: surface as a
    // labelled timeline row. All other state events (membership excluded,
    // handled separately below) remain filtered.
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
                let SenderProfileFfi {
                    sender_name,
                    sender_avatar_url,
                    sender_status_emoji,
                    sender_status_text,
                } = sender_profile_ffi(event_item.sender_profile());
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
                    sender_status_emoji,
                    sender_status_text,
                    body,
                    timestamp: event_item.timestamp().get().into(),
                    ..ffi_event_defaults()
                });
            }
        }
        if let AnyOtherStateEventContentChange::RoomName(full) = state.content() {
            if let StateEventContentChange::Original {
                content,
                prev_content,
            } = full
            {
                let room_name_new = content.name.clone();
                let room_name_old = prev_content
                    .as_ref()
                    .and_then(|pc| pc.name.clone())
                    .unwrap_or_default();
                let SenderProfileFfi {
                    sender_name,
                    sender_avatar_url,
                    sender_status_emoji,
                    sender_status_text,
                } = sender_profile_ffi(event_item.sender_profile());
                return Some(TimelineEvent {
                    room_id: room_id.to_owned(),
                    msg_type: "m.room.name".to_owned(),
                    event_id: event_item
                        .event_id()
                        .map(|id| id.to_string())
                        .unwrap_or_default(),
                    sender: event_item.sender().to_string(),
                    sender_name,
                    sender_avatar_url,
                    sender_status_emoji,
                    sender_status_text,
                    room_name_new,
                    room_name_old,
                    timestamp: event_item.timestamp().get().into(),
                    ..ffi_event_defaults()
                });
            }
        }
        if let AnyOtherStateEventContentChange::RoomTombstone(full) = state.content() {
            if let StateEventContentChange::Original { content, .. } = full {
                let SenderProfileFfi {
                    sender_name,
                    sender_avatar_url,
                    sender_status_emoji,
                    sender_status_text,
                } = sender_profile_ffi(event_item.sender_profile());
                return Some(TimelineEvent {
                    room_id: room_id.to_owned(),
                    msg_type: "m.room.tombstone".to_owned(),
                    event_id: event_item
                        .event_id()
                        .map(|id| id.to_string())
                        .unwrap_or_default(),
                    sender: event_item.sender().to_string(),
                    sender_name,
                    sender_avatar_url,
                    sender_status_emoji,
                    sender_status_text,
                    body: content.body.clone(),
                    replacement_room_id: content.replacement_room.to_string(),
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
    // TimelineItemContent::ProfileChange — handled just below.
    // Profile-only changes (display name / avatar while joined) become the same
    // `m.room.member` event shape so the timeline can show "X changed their
    // avatar"; visibility is gated by `membership_event_visible` (own changes
    // always, others' only with "Show room join/leave events").
    if let TimelineItemContent::ProfileChange(p) = event_item.content() {
        let avatar = p.avatar_url_change();
        let name = p.displayname_change();
        let Some(action) = profile_change_action(
            avatar.map(|c| c.new.is_some()),
            name.map(|c| c.new.as_ref().is_some_and(|n| !n.is_empty())),
        ) else {
            return None;
        };
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
        // Display-name changes carry the NEW name; for an avatar-only change
        // the target's name is the member's current display name.
        let target_name = match name {
            Some(c) => c.new.clone().unwrap_or_default(),
            None => sender_name.clone(),
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
            sender_status_emoji,
            sender_status_text,
            membership_action: action.to_owned(),
            membership_target_user_id: p.user_id().to_string(),
            membership_target_name: target_name,
            membership_target_avatar_url: avatar
                .and_then(|c| c.new.as_ref())
                .map(|u| u.to_string())
                .unwrap_or_default(),
            timestamp: event_item.timestamp().get().into(),
            ..ffi_event_defaults()
        });
    }

    if let TimelineItemContent::MembershipChange(change) = event_item.content() {
        let Some(action) = change.change().and_then(membership_action_str) else {
            // None/Error/NotImplemented, or a redacted state event whose
            // `.change()` cannot be computed — not a real transition, drop.
            return None;
        };
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
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
            sender_status_emoji,
            sender_status_text,
            membership_action: action.to_owned(),
            membership_target_user_id: change.user_id().to_string(),
            membership_target_name: change.display_name().unwrap_or_default(),
            membership_target_avatar_url: change
                .avatar_url()
                .map(|u| u.to_string())
                .unwrap_or_default(),
            membership_reason: match change.content() {
                matrix_sdk::ruma::events::StateEventContentChange::Original { content, .. } => {
                    content.reason.clone().unwrap_or_default()
                }
                _ => String::new(),
            },
            timestamp: event_item.timestamp().get().into(),
            ..ffi_event_defaults()
        });
    }

    // Compute pending fields once for all non-virtual event paths.
    let (pending_state, pending_error, pending_recoverable, pending_txn_id, pending_error_users) =
        if event_item.is_local_echo() {
            let txn = event_item
                .transaction_id()
                .map(|t| t.to_string())
                .unwrap_or_default();
            match event_item.send_state() {
                Some(EventSendState::NotSentYet { .. }) => {
                    ("sending".to_owned(), String::new(), false, txn, Vec::new())
                }
                Some(EventSendState::SendingFailed {
                    error,
                    is_recoverable,
                }) => match crypto_send_block_code(error) {
                    // Parked by an encryption check the user can resolve
                    // (identity-change warning, verifying this device):
                    // offer Retry, which unwedges it (`retry_send`).
                    Some((code, users)) => ("failed".to_owned(), code.to_owned(), true, txn, users),
                    None => (
                        "failed".to_owned(),
                        error.to_string(),
                        *is_recoverable,
                        txn,
                        Vec::new(),
                    ),
                },
                _ => (String::new(), String::new(), false, txn, Vec::new()),
            }
        } else {
            (
                String::new(),
                String::new(),
                false,
                String::new(),
                Vec::new(),
            )
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
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
        return Some(TimelineEvent {
            event_id: event_item
                .event_id()
                .map(|id| id.to_string())
                .unwrap_or_default(),
            room_id: room_id.to_owned(),
            sender: event_item.sender().to_string(),
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
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
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
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
            sender_status_emoji,
            sender_status_text,
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
        // MSC4553: a forward renders the sticker it carries.
        let forward = original_forward(event_item, "m.sticker");
        let mut fields = forward
            .as_ref()
            .and_then(forwarded_sticker_fields)
            .unwrap_or_else(|| sticker_fields(s.content()));
        if let Some(view) = &forward {
            let name = forwarded_sender_name(room, &view.forwarded).await;
            apply_forward(&mut fields, &view.forwarded, name);
        }
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
        let reactions = collect_reactions(event_item, room, me).await;
        let read_receipts = collect_read_receipts(event_item, room, me).await;
        let (
            in_reply_to_id,
            in_reply_to_sender_name,
            in_reply_to_body,
            in_reply_to_formatted_body,
            in_reply_to_image_url,
            in_reply_to_image_encrypted_json,
        ) = extract_in_reply_to(room, event_item).await;
        // MSC3440 thread metadata — mirrors the m.room.message branch below.
        // Without this, a sticker sent as a thread reply (or thread root)
        // converts with an empty thread_root_id regardless of its actual
        // m.relates_to, so it never gets recognized as a thread event.
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
                let (name, body, _formatted, ts) = match &summary.latest_event {
                    TimelineDetails::Ready(embedded) => {
                        embedded_event_preview(room, embedded).await
                    }
                    _ => (String::new(), String::new(), String::new(), 0u64),
                };
                (true, count, name, body, ts)
            }
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
            sender_status_emoji,
            sender_status_text,
            timestamp: event_item.timestamp().get().into(),
            reactions,
            read_receipts,
            in_reply_to_id,
            in_reply_to_sender_name,
            in_reply_to_body,
            in_reply_to_formatted_body,
            in_reply_to_image_url,
            in_reply_to_image_encrypted_json,
            pending_state: pending_state.clone(),
            pending_error: pending_error.clone(),
            pending_recoverable,
            pending_txn_id: pending_txn_id.clone(),
            pending_error_users: pending_error_users.clone(),
            thread_root_id,
            is_thread_root,
            thread_reply_count,
            thread_latest_sender_name,
            thread_latest_body,
            thread_latest_ts,
            ..fields
        });
    }

    // MSC3381 poll. matrix-sdk-ui aggregates responses/ends into PollState and
    // updates this one item in place (VectorDiff::Set) as votes arrive.
    if let TimelineItemContent::MsgLike(MsgLikeContent {
        kind: MsgLikeKind::Poll(state),
        ..
    }) = event_item.content()
    {
        let r = state.results();
        // Anything that is not explicitly "disclosed" is treated as
        // undisclosed (MSC3381: unknown kinds fall back to undisclosed).
        let disclosed = poll_is_disclosed(&r.kind);
        let ended = r.end_time.is_some();
        let answers: Vec<(String, String, Vec<String>)> = r
            .answers
            .iter()
            .map(|a| {
                (
                    a.id.clone(),
                    a.text.clone(),
                    r.votes.get(&a.id).cloned().unwrap_or_default(),
                )
            })
            .collect();
        let f = poll_fields(&answers, disclosed, ended, me.map_or("", |u| u.as_str()));
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
        let reactions = collect_reactions(event_item, room, me).await;
        let read_receipts = collect_read_receipts(event_item, room, me).await;
        let (
            in_reply_to_id,
            in_reply_to_sender_name,
            in_reply_to_body,
            in_reply_to_formatted_body,
            in_reply_to_image_url,
            in_reply_to_image_encrypted_json,
        ) = extract_in_reply_to(room, event_item).await;
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
                let (name, body, _formatted, ts) = match &summary.latest_event {
                    TimelineDetails::Ready(embedded) => {
                        embedded_event_preview(room, embedded).await
                    }
                    _ => (String::new(), String::new(), String::new(), 0u64),
                };
                (true, count, name, body, ts)
            }
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
            sender_status_emoji,
            sender_status_text,
            body: r.question.clone(),
            timestamp: event_item.timestamp().get().into(),
            msg_type: "m.poll".to_owned(),
            is_edited: r.has_been_edited,
            reactions,
            read_receipts,
            in_reply_to_id,
            in_reply_to_sender_name,
            in_reply_to_body,
            in_reply_to_formatted_body,
            in_reply_to_image_url,
            in_reply_to_image_encrypted_json,
            pending_state: pending_state.clone(),
            pending_error: pending_error.clone(),
            pending_recoverable,
            pending_txn_id: pending_txn_id.clone(),
            pending_error_users: pending_error_users.clone(),
            thread_root_id,
            is_thread_root,
            thread_reply_count,
            thread_latest_sender_name,
            thread_latest_body,
            thread_latest_ts,
            poll_answers: f.answers,
            poll_max_selections: u32::try_from(r.max_selections).unwrap_or(u32::MAX).max(1),
            poll_ended: ended,
            poll_results_visible: f.results_visible,
            poll_total_votes: f.total_votes,
            ..ffi_event_defaults()
        });
    }

    // MSC3489 live location share: one item per share; beacon updates and the
    // stop event arrive as VectorDiff::Set on this same item. Surface it as an
    // m.location event with the live-share fields set so it renders through
    // the existing Kind::Location row.
    if let TimelineItemContent::MsgLike(MsgLikeContent {
        kind: MsgLikeKind::LiveLocation(state),
        ..
    }) = event_item.content()
    {
        let latest = state.latest_location();
        let f = live_location_fields(
            latest.map(|l| l.geo_uri()),
            latest.map_or(0, |l| u64::from(l.ts().get())),
            state.is_live(),
            u64::from(state.ts().get()),
            state.timeout().as_millis().min(u128::from(u64::MAX)) as u64,
        );
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
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
            sender_status_emoji,
            sender_status_text,
            timestamp: event_item.timestamp().get().into(),
            msg_type: "m.location".to_owned(),
            location_lat: f.lat,
            location_lon: f.lon,
            location_description: state.description().unwrap_or_default().to_owned(),
            location_live_share: true,
            location_live: f.live,
            location_live_expires_ms: f.expires_ms,
            location_updated_ms: f.updated_ms,
            location_awaiting_fix: f.awaiting_fix,
            reactions,
            read_receipts,
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
        let SenderProfileFfi {
            sender_name,
            sender_avatar_url,
            sender_status_emoji,
            sender_status_text,
        } = sender_profile_ffi(event_item.sender_profile());
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
            sender_status_emoji,
            sender_status_text,
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

    // MSC4553: a forward renders the content it carries (read from the original
    // JSON, so never an edit of it) with the original sender's attribution. Its
    // relation and mentions are ignored; the reply and thread data below come
    // from the forward event itself.
    let forward = original_forward(event_item, "m.room.message");
    let mut msg_fields = forward
        .as_ref()
        .and_then(forwarded_message_fields)
        .or_else(|| {
            let info = match msg_content.msgtype() {
                MessageType::Video(_) => original_content_info(event_item),
                _ => None,
            };
            message_type_fields(
                msg_content.msgtype(),
                &|fallback| resanitized_formatted_body(event_item, fallback),
                info.as_ref(),
            )
        })?;
    if let Some(view) = &forward {
        let name = forwarded_sender_name(room, &view.forwarded).await;
        apply_forward(&mut msg_fields, &view.forwarded, name);
    }

    let SenderProfileFfi {
        sender_name,
        sender_avatar_url,
        sender_status_emoji,
        sender_status_text,
    } = sender_profile_ffi(event_item.sender_profile());

    // m.in_reply_to — extract the event_id and, when the replied-to item is
    // present in the local timeline cache, its sender display name, a brief
    // body snippet, and (for m.image replies) the image thumbnail source.
    let (
        in_reply_to_id,
        in_reply_to_sender_name,
        in_reply_to_body,
        in_reply_to_formatted_body,
        in_reply_to_image_url,
        in_reply_to_image_encrypted_json,
    ) = extract_in_reply_to(room, event_item).await;

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
            let (name, body, _formatted, ts) = match &summary.latest_event {
                TimelineDetails::Ready(embedded) => {
                    embedded_event_preview(room, embedded).await
                }
                _ => (String::new(), String::new(), String::new(), 0u64),
            };
            (true, count, name, body, ts)
        }
    };

    // Layer the common (sender / reaction / reply / thread / pending) fields on
    // top of the per-msgtype partial. Fields `message_type_fields` set (body,
    // msg_type, source_*, width/height, file_*, audio_*, video_*, location_*) flow through
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
        sender_status_emoji,
        sender_status_text,
        timestamp: event_item.timestamp().get().into(),
        reactions,
        read_receipts,
        in_reply_to_id,
        in_reply_to_sender_name,
        in_reply_to_body,
        in_reply_to_formatted_body,
        in_reply_to_image_url,
        in_reply_to_image_encrypted_json,
        is_edited: forward.is_none() && msg_content.is_edited(),
        pending_state,
        pending_error,
        pending_recoverable,
        pending_txn_id,
        pending_error_users,
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
            "\u{1}pin_added"
        );
    }

    #[test]
    fn unpin_one() {
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a"]),
            "\u{1}pin_removed"
        );
    }

    #[test]
    fn pin_many() {
        assert_eq!(
            pinned_events_action(&["$a", "$b", "$c"], &[] as &[&str]),
            "\u{1}pin_added_n\u{1}3"
        );
    }

    #[test]
    fn unpin_many() {
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a", "$b"]),
            "\u{1}pin_removed_n\u{1}2"
        );
    }

    #[test]
    fn clear_all_three() {
        // "cleared" fires only when new list is empty AND old list had >=3 items.
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a", "$b", "$c"]),
            "\u{1}pin_cleared"
        );
    }

    #[test]
    fn unpin_two_below_threshold() {
        // Only 2 removed => threshold not met, so "unpinned N messages" fires instead.
        assert_eq!(
            pinned_events_action(&[] as &[&str], &["$a", "$b"]),
            "\u{1}pin_removed_n\u{1}2"
        );
    }

    #[test]
    fn mixed_change() {
        assert_eq!(pinned_events_action(&["$b"], &["$a"]), "\u{1}pin_changed");
    }

    #[test]
    fn no_change() {
        // Callers should filter out no-op state events (old == new) before calling.
        // The fallthrough branch returns this, but a real caller won't reach it.
        assert_eq!(pinned_events_action(&["$a"], &["$a"]), "\u{1}pin_changed");
    }
}

#[cfg(test)]
mod profile_change_tests {
    use super::{is_profile_action, membership_event_visible, profile_change_action};

    #[test]
    fn maps_each_combination() {
        assert_eq!(profile_change_action(None, None), None);
        assert_eq!(
            profile_change_action(Some(true), None),
            Some("avatar_changed")
        );
        assert_eq!(
            profile_change_action(Some(false), None),
            Some("avatar_removed")
        );
        assert_eq!(
            profile_change_action(None, Some(true)),
            Some("display_name_changed")
        );
        assert_eq!(
            profile_change_action(None, Some(false)),
            Some("display_name_removed")
        );
        assert_eq!(
            profile_change_action(Some(true), Some(false)),
            Some("profile_changed")
        );
        assert_eq!(
            profile_change_action(Some(false), Some(true)),
            Some("profile_changed")
        );
    }

    #[test]
    fn own_profile_changes_are_always_visible() {
        let me = Some("@me:x");
        // Hidden membership events stay hidden...
        assert!(!membership_event_visible(
            "m.room.member",
            "joined",
            "@me:x",
            false,
            me
        ));
        assert!(!membership_event_visible(
            "m.room.member",
            "left",
            "@other:x",
            false,
            me
        ));
        // ...others' profile changes too...
        assert!(!membership_event_visible(
            "m.room.member",
            "avatar_changed",
            "@other:x",
            false,
            me
        ));
        // ...but your own profile changes show regardless of the setting.
        for a in [
            "avatar_changed",
            "avatar_removed",
            "display_name_changed",
            "display_name_removed",
            "profile_changed",
        ] {
            assert!(
                membership_event_visible("m.room.member", a, "@me:x", false, me),
                "{a}"
            );
        }
        // No known user: nothing is "own".
        assert!(!membership_event_visible(
            "m.room.member",
            "avatar_changed",
            "@me:x",
            false,
            None
        ));
        // The setting shows everything; other event types are unaffected.
        assert!(membership_event_visible(
            "m.room.member",
            "joined",
            "@o:x",
            true,
            me
        ));
        assert!(membership_event_visible(
            "m.room.message",
            "",
            "",
            false,
            me
        ));
    }

    #[test]
    fn every_produced_action_is_a_profile_action() {
        for a in [Some(true), Some(false), None] {
            for n in [Some(true), Some(false), None] {
                if let Some(action) = profile_change_action(a, n) {
                    assert!(is_profile_action(action), "{action}");
                }
            }
        }
        assert!(!is_profile_action("joined"));
        assert!(!is_profile_action(""));
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
    fn gallery_caption_html_is_sanitised() {
        // A gallery caption is not sanitised by matrix-sdk-ui, so the markup
        // has to be cleaned here: no remote image, no script.
        let json = serde_json::json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.gallery",
                "body": "caption",
                "format": "org.matrix.custom.html",
                "formatted_body": "<b>hi</b><img src=\"https://evil.example/x.png\"><script>x()</script>"
            }
        });
        let out = resanitized_formatted_body_from_json(&json, String::new());
        assert!(out.contains("<b>hi</b>"), "{out}");
        assert!(!out.contains("evil.example"), "{out}");
        assert!(!out.contains("<script"), "{out}");
    }

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

#[cfg(test)]
mod bundled_url_preview_tests {
    use super::map_bundled_url_previews;
    use matrix_sdk::ruma::events::room::message::{TextMessageEventContent, UrlPreview};

    fn previews_from(json: serde_json::Value) -> Vec<UrlPreview> {
        let content: TextMessageEventContent = serde_json::from_value(json).unwrap();
        content.url_previews.unwrap_or_default()
    }

    #[test]
    fn stable_field_plain_image_full_preview() {
        let body = "see https://matrix.org for details";
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": body,
            "m.url_previews": [{
                "matrix:matched_url": "https://matrix.org",
                "og:title": "Matrix.org",
                "og:description": "The open protocol",
                "og:url": "https://matrix.org/",
                "og:image": "mxc://maunium.net/abc",
                "og:image:width": 800,
                "og:image:height": 400
            }]
        }));
        let out = map_bundled_url_previews(&previews, body);
        assert_eq!(out.len(), 1);
        let p = &out[0];
        assert_eq!(p.matched_url, "https://matrix.org");
        assert_eq!(p.title, "Matrix.org");
        assert_eq!(p.description, "The open protocol");
        assert_eq!(p.canonical_url, "https://matrix.org/");
        assert_eq!(p.image_url, "mxc://maunium.net/abc");
        assert!(p.image_encrypted_json.is_empty());
        assert_eq!(p.image_width, 800);
        assert_eq!(p.image_height, 400);
    }

    #[test]
    fn unstable_beeper_field_with_encrypted_image() {
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": "https://matrix.org",
            "com.beeper.linkpreviews": [{
                "matched_url": "https://matrix.org",
                "og:title": "Matrix.org",
                "beeper:image:encryption": {
                    "key": {
                        "k": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                        "alg": "A256CTR", "ext": true, "kty": "oct",
                        "key_ops": ["encrypt", "decrypt"]
                    },
                    "iv": "AQEBAQEBAQEBAQEBAQEBAQ",
                    "hashes": { "sha256": "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI" },
                    "v": "v2",
                    "url": "mxc://beeper.com/enc123"
                }
            }]
        }));
        let out = map_bundled_url_previews(&previews, "https://matrix.org");
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].image_url, "mxc://beeper.com/enc123");
        // Same JSON shape `split_source` emits, so the media-fetch path is shared.
        assert!(out[0]
            .image_encrypted_json
            .contains("mxc://beeper.com/enc123"));
        assert!(out[0].image_encrypted_json.contains("A256CTR"));
    }

    #[test]
    fn entry_with_matched_url_not_in_body_is_filtered() {
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": "nothing to see here",
            "m.url_previews": [{
                "matrix:matched_url": "https://evil.example",
                "og:title": "Spoofed"
            }]
        }));
        assert!(map_bundled_url_previews(&previews, "nothing to see here").is_empty());
    }

    #[test]
    fn entry_with_matched_url_only_in_code_span_is_filtered() {
        let body = "see\n```\nhttps://matrix.org\n```\nthanks";
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": body,
            "m.url_previews": [{
                "matrix:matched_url": "https://matrix.org",
                "og:title": "Matrix.org"
            }]
        }));
        assert!(map_bundled_url_previews(&previews, body).is_empty());
    }

    #[test]
    fn matched_url_only_entry_is_kept_without_content() {
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": "https://matrix.org",
            "m.url_previews": [{ "matrix:matched_url": "https://matrix.org" }]
        }));
        let out = map_bundled_url_previews(&previews, "https://matrix.org");
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].matched_url, "https://matrix.org");
        assert!(out[0].title.is_empty());
        assert!(out[0].description.is_empty());
        assert!(out[0].image_url.is_empty());
    }

    #[test]
    fn entry_without_matched_url_is_dropped() {
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": "https://matrix.org",
            "m.url_previews": [{ "og:title": "no matched_url" }]
        }));
        assert!(map_bundled_url_previews(&previews, "https://matrix.org").is_empty());
    }

    #[test]
    fn empty_array_yields_no_entries() {
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": "https://matrix.org",
            "m.url_previews": []
        }));
        assert!(map_bundled_url_previews(&previews, "https://matrix.org").is_empty());
    }

    #[test]
    fn non_http_matched_url_is_filtered_even_when_in_body() {
        // A sender controls both body and matched_url; the substring check
        // alone lets any scheme through to the OS URL opener.
        for hostile in [
            "file:///etc/passwd",
            "search-ms:query=x&crumb=location:\\\\evil\\share",
            "ms-msdt:/id PCWDiagnostic",
            "-aCalculator",
            "javascript:alert(1)",
        ] {
            let body = format!("look {hostile}");
            let previews = previews_from(serde_json::json!({
                "msgtype": "m.text",
                "body": body,
                "m.url_previews": [{ "matrix:matched_url": hostile, "og:title": "x" }]
            }));
            assert!(
                map_bundled_url_previews(&previews, &body).is_empty(),
                "kept hostile matched_url {hostile}"
            );
        }
    }

    #[test]
    fn mixed_previews_keep_only_the_http_one() {
        let body = "https://matrix.org and file:///etc/passwd";
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": body,
            "m.url_previews": [
                { "matrix:matched_url": "https://matrix.org", "og:title": "ok" },
                { "matrix:matched_url": "file:///etc/passwd", "og:title": "bad" }
            ]
        }));
        let out = map_bundled_url_previews(&previews, body);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].matched_url, "https://matrix.org");
    }

    #[test]
    fn uppercase_http_scheme_is_kept() {
        let body = "HTTPS://matrix.org";
        let previews = previews_from(serde_json::json!({
            "msgtype": "m.text",
            "body": body,
            "m.url_previews": [{ "matrix:matched_url": "HTTPS://matrix.org", "og:title": "ok" }]
        }));
        assert_eq!(map_bundled_url_previews(&previews, body).len(), 1);
    }
}

#[cfg(test)]
mod status_ffi_tests {
    use super::status_ffi;
    use matrix_sdk::ruma::profile::StatusProfileField;

    #[test]
    fn maps_emoji_and_text() {
        let s = StatusProfileField::new("On holiday".to_owned(), "🌴".to_owned());
        assert_eq!(
            status_ffi(Some(&s)),
            ("🌴".to_owned(), "On holiday".to_owned())
        );
    }

    #[test]
    fn unset_is_empty() {
        assert_eq!(status_ffi(None), (String::new(), String::new()));
    }
}

#[cfg(test)]
mod live_location_tests {
    use super::live_location_fields;

    #[test]
    fn no_beacon_is_awaiting_fix() {
        let f = live_location_fields(None, 0, true, 1_000, 60_000);
        assert!(f.awaiting_fix);
        assert!(f.live);
        assert_eq!(f.expires_ms, 61_000);
        assert_eq!(f.updated_ms, 0);
    }

    #[test]
    fn valid_fix_is_parsed() {
        let f = live_location_fields(Some("geo:51.5008,0.1247;u=35"), 5_000, true, 1_000, 60_000);
        assert!(!f.awaiting_fix);
        assert_eq!((f.lat, f.lon), (51.5008, 0.1247));
        assert_eq!(f.updated_ms, 5_000);
    }

    #[test]
    fn malformed_geo_uri_is_awaiting_fix_not_origin() {
        let f = live_location_fields(Some("geo:nope"), 5_000, true, 1_000, 60_000);
        assert!(f.awaiting_fix);
    }

    #[test]
    fn stopped_share_keeps_last_fix() {
        let f = live_location_fields(Some("geo:1.0,2.0"), 5_000, false, 1_000, 60_000);
        assert!(!f.live);
        assert!(!f.awaiting_fix);
    }

    #[test]
    fn expiry_saturates() {
        let f = live_location_fields(None, 0, true, u64::MAX - 1, u64::MAX);
        assert_eq!(f.expires_ms, u64::MAX);
    }
}

#[cfg(test)]
mod poll_fields_tests {
    use super::*;
    use matrix_sdk::ruma::events::poll::start::PollKind;

    #[test]
    fn poll_kind_disclosed_mapping() {
        assert!(poll_is_disclosed(&PollKind::Disclosed));
        assert!(!poll_is_disclosed(&PollKind::Undisclosed));
        let custom: PollKind = serde_json::from_str("\"org.example.weird\"").unwrap();
        assert!(!poll_is_disclosed(&custom));
    }

    fn answers() -> Vec<(String, String, Vec<String>)> {
        vec![
            (
                "a".into(),
                "Alpha".into(),
                vec!["@me:x".into(), "@bob:x".into()],
            ),
            ("b".into(), "Beta".into(), vec!["@carol:x".into()]),
            ("c".into(), "Gamma".into(), vec![]),
        ]
    }

    #[test]
    fn disclosed_open_poll_shows_counts_and_my_vote() {
        let f = poll_fields(&answers(), true, false, "@me:x");
        assert!(f.results_visible);
        assert_eq!(f.total_votes, 3);
        assert_eq!(f.answers[0].votes, 2);
        assert!(f.answers[0].mine);
        assert!(!f.answers[1].mine);
        assert_eq!(f.answers[2].votes, 0);
    }

    #[test]
    fn undisclosed_open_poll_hides_counts_but_keeps_my_vote() {
        let f = poll_fields(&answers(), false, false, "@me:x");
        assert!(!f.results_visible);
        assert_eq!(f.total_votes, 0);
        assert!(f.answers.iter().all(|a| a.votes == 0));
        assert!(f.answers[0].mine, "own selection must still show");
    }

    #[test]
    fn ended_poll_reveals_counts_even_if_undisclosed() {
        let f = poll_fields(&answers(), false, true, "@me:x");
        assert!(f.results_visible);
        assert_eq!(f.total_votes, 3);
        assert_eq!(f.answers[0].votes, 2);
    }

    #[test]
    fn order_ids_and_text_are_preserved() {
        let f = poll_fields(&answers(), true, false, "@nobody:x");
        let ids: Vec<&str> = f.answers.iter().map(|a| a.id.as_str()).collect();
        assert_eq!(ids, ["a", "b", "c"]);
        assert_eq!(f.answers[1].text, "Beta");
        assert!(f.answers.iter().all(|a| !a.mine));
    }

    #[test]
    fn empty_answer_list_is_fine() {
        let f = poll_fields(&[], true, false, "@me:x");
        assert!(f.answers.is_empty());
        assert_eq!(f.total_votes, 0);
    }
}

#[cfg(test)]
mod gallery_item_ffi_tests {
    use super::gallery_item_ffi_from_type;
    use matrix_sdk::ruma::{
        events::room::{
            message::{
                AudioInfo, AudioMessageEventContent, FileInfo, FileMessageEventContent,
                GalleryItemType, ImageMessageEventContent, VideoInfo, VideoMessageEventContent,
            },
            ImageInfo, MediaSource, ThumbnailInfo,
        },
        OwnedMxcUri, UInt,
    };
    use std::time::Duration;

    fn url(s: &str) -> OwnedMxcUri {
        OwnedMxcUri::from(s)
    }

    #[test]
    fn image_item_extracts_source_dims_filename_blurhash() {
        let mut content =
            ImageMessageEventContent::plain("photo.jpg".to_owned(), url("mxc://example.org/img1"));
        content.filename = Some("photo.jpg".to_owned());
        let mut info = ImageInfo::default();
        info.width = UInt::new(800);
        info.height = UInt::new(600);
        info.blurhash = Some("LEHV6nWB2yk8".to_owned());
        info.is_animated = Some(true);
        content.info = Some(Box::new(info));
        let item = GalleryItemType::Image(content);

        let ffi = gallery_item_ffi_from_type(&item);

        assert_eq!(ffi.itemtype, "m.image");
        assert_eq!(ffi.body, "photo.jpg");
        assert_eq!(ffi.source_url, "mxc://example.org/img1");
        assert_eq!(ffi.source_encrypted_json, "");
        assert_eq!(ffi.width, 800);
        assert_eq!(ffi.height, 600);
        assert_eq!(ffi.filename, "photo.jpg");
        assert_eq!(ffi.blurhash, "LEHV6nWB2yk8");
        assert!(ffi.animated);
    }

    #[test]
    fn file_item_extracts_source_filename_size() {
        let mut content =
            FileMessageEventContent::plain("report.pdf".to_owned(), url("mxc://example.org/file1"));
        content.filename = Some("report.pdf".to_owned());
        let mut info = FileInfo::new();
        info.size = UInt::new(4096);
        content.info = Some(Box::new(info));
        let item = GalleryItemType::File(content);

        let ffi = gallery_item_ffi_from_type(&item);

        assert_eq!(ffi.itemtype, "m.file");
        assert_eq!(ffi.body, "report.pdf");
        assert_eq!(ffi.source_url, "mxc://example.org/file1");
        assert_eq!(ffi.filename, "report.pdf");
        assert_eq!(ffi.file_size, 4096);
    }

    #[test]
    fn audio_item_extracts_source_mime_duration() {
        let mut content =
            AudioMessageEventContent::plain("clip.ogg".to_owned(), url("mxc://example.org/aud1"));
        let mut info = AudioInfo::new();
        info.mimetype = Some("audio/ogg".to_owned());
        info.duration = Some(Duration::from_millis(2500));
        content.info = Some(Box::new(info));
        let item = GalleryItemType::Audio(content);

        let ffi = gallery_item_ffi_from_type(&item);

        assert_eq!(ffi.itemtype, "m.audio");
        assert_eq!(ffi.body, "clip.ogg");
        assert_eq!(ffi.source_url, "mxc://example.org/aud1");
        assert_eq!(ffi.mime, "audio/ogg");
        assert_eq!(ffi.duration_ms, 2500);
    }

    #[test]
    fn video_item_extracts_source_dims_thumbnail_mime_duration() {
        let mut content =
            VideoMessageEventContent::plain("clip.mp4".to_owned(), url("mxc://example.org/vid1"));
        let mut info = VideoInfo::new();
        info.width = UInt::new(1920);
        info.height = UInt::new(1080);
        info.mimetype = Some("video/mp4".to_owned());
        info.duration = Some(Duration::from_millis(30_000));
        info.thumbnail_source = Some(MediaSource::Plain(url("mxc://example.org/vid1-thumb")));
        info.thumbnail_info = Some(Box::new(ThumbnailInfo::new()));
        info.blurhash = Some("LKO2?U%2Tw=w".to_owned());
        content.info = Some(Box::new(info));
        let item = GalleryItemType::Video(content);

        let ffi = gallery_item_ffi_from_type(&item);

        assert_eq!(ffi.itemtype, "m.video");
        assert_eq!(ffi.source_url, "mxc://example.org/vid1");
        assert_eq!(ffi.width, 1920);
        assert_eq!(ffi.height, 1080);
        assert_eq!(ffi.mime, "video/mp4");
        assert_eq!(ffi.duration_ms, 30_000);
        assert_eq!(ffi.thumbnail_url, "mxc://example.org/vid1-thumb");
        assert_eq!(ffi.blurhash, "LKO2?U%2Tw=w");
    }

    #[test]
    fn unknown_item_falls_back_to_itemtype_and_body_methods() {
        let item = GalleryItemType::new(
            "org.example.custom",
            "custom body".to_owned(),
            serde_json::Map::new(),
        )
        .expect("custom item construction should succeed");

        let ffi = gallery_item_ffi_from_type(&item);

        assert_eq!(ffi.itemtype, "org.example.custom");
        assert_eq!(ffi.body, "custom body");
    }
}

#[cfg(test)]
mod geo_uri_tests {
    use super::parse_geo_uri;

    #[test]
    fn non_finite_and_out_of_range_geo_uris_are_refused() {
        for uri in [
            "geo:NaN,0",
            "geo:0,inf",
            "geo:1e999,0",
            "geo:91,0",
            "geo:0,181",
            "geo:-90.5,0",
        ] {
            assert_eq!(parse_geo_uri(uri), None, "{uri}");
        }
        assert_eq!(parse_geo_uri("geo:85,180;u=35"), Some((85.0, 180.0)));
    }
}

#[cfg(test)]
mod forward_tests {
    use super::{
        apply_forward, forward_view, forwarded_message_fields, forwarded_snippet_from_event_json,
        forwarded_sticker_fields, may_be_forward_fallback, message_snippet, message_type_fields,
        ForwardView,
    };
    use crate::ffi::TimelineEvent;
    use crate::msc::msc4553::UNSTABLE_KEY;
    use matrix_sdk::ruma::events::room::message::{MessageType, RoomMessageEventContent};
    use serde_json::{json, Value};

    const FALLBACK: &str =
        "Forwarded from Bob - view original message: https://matrix.to/#/x\nhello";

    fn view(original: &Value) -> ForwardView {
        forward_view(&original["content"], "m.room.message").expect("a forward")
    }

    /// An original event whose top-level content is the fallback and whose
    /// `forwarded.content` is `inner`.
    fn forward_event(inner: Value) -> Value {
        json!({
            "type": "m.room.message",
            "content": {
                "msgtype": "m.text",
                "body": FALLBACK,
                "format": "org.matrix.custom.html",
                "formatted_body": "<strong>Forwarded from Bob</strong><br><blockquote>hello</blockquote>",
                UNSTABLE_KEY: {
                    "content": inner,
                    "sender": "@bob:other.org",
                    "room_id": "!orig:other.org",
                    "event_id": "$orig",
                    "origin_server_ts": 1_690_000_000_000_i64
                }
            }
        })
    }

    fn rendered(original: &Value) -> TimelineEvent {
        forwarded_message_fields(&view(original)).expect("renderable")
    }

    #[test]
    fn a_plain_message_is_not_a_forward() {
        let ev =
            json!({ "type": "m.room.message", "content": { "msgtype": "m.text", "body": "hi" } });
        assert!(forward_view(&ev["content"], "m.room.message").is_none());
    }

    #[test]
    fn a_forward_renders_the_forwarded_content() {
        let ev = forward_event(json!({
            "msgtype": "m.text", "body": "hello *world*",
            "format": "org.matrix.custom.html", "formatted_body": "hello <em>world</em>"
        }));
        let f = rendered(&ev);
        assert_eq!(f.msg_type, "m.text");
        assert_eq!(f.body, "hello *world*");
        assert_eq!(f.formatted_body, "hello <em>world</em>");
        assert!(!f.is_edited);
    }

    #[test]
    fn forwarded_html_is_sanitized_and_loses_a_reply_fallback() {
        let ev = forward_event(json!({
            "msgtype": "m.text", "body": "hi",
            "format": "org.matrix.custom.html",
            "formatted_body": "<mx-reply><blockquote>old</blockquote></mx-reply>hi<script>alert(1)</script>"
        }));
        let f = rendered(&ev);
        assert!(!f.formatted_body.contains("script"), "{}", f.formatted_body);
        assert!(!f.formatted_body.contains("old"), "{}", f.formatted_body);
        assert!(f.formatted_body.contains("hi"));
    }

    #[test]
    fn an_edit_of_a_forward_is_not_applied() {
        // The bundled edit rides on the event, next to the content; only the
        // content is read, so the original text stays.
        let mut ev = forward_event(json!({ "msgtype": "m.text", "body": "original" }));
        ev["unsigned"] = json!({ "m.relations": { "m.replace": { "content": {
            "m.new_content": { "msgtype": "m.text", "body": "edited" } } } } });
        let v = view(&ev);
        assert_eq!(v.content["body"], "original");
        let f = forwarded_message_fields(&v).unwrap();
        assert_eq!(f.body, "original");
        assert!(!f.is_edited);
    }

    #[test]
    fn a_forwarded_emote_maps_to_an_emote() {
        let f = rendered(&forward_event(
            json!({ "msgtype": "m.emote", "body": "waves" }),
        ));
        assert_eq!(f.msg_type, "m.emote");
        assert_eq!(f.body, "waves");
    }

    #[test]
    fn a_forwarded_image_keeps_its_media_fields() {
        let f = rendered(&forward_event(json!({
            "msgtype": "m.image", "body": "cat.png", "url": "mxc://x/img",
            "info": { "w": 640, "h": 480, "mimetype": "image/png", "xyz.amorgan.blurhash": "LKO2" }
        })));
        assert_eq!(f.msg_type, "m.image");
        assert_eq!(f.source_url, "mxc://x/img");
        assert_eq!(f.source_encrypted_json, "");
        assert_eq!((f.width, f.height), (640, 480));
        assert_eq!(f.image_mime, "image/png");
        assert_eq!(f.blurhash, "LKO2");
    }

    #[test]
    fn a_forwarded_encrypted_file_keeps_its_key_for_the_download() {
        let f = rendered(&forward_event(json!({
            "msgtype": "m.file", "body": "report.pdf",
            "file": {
                "url": "mxc://x/file", "v": "v2",
                "key": { "kty": "oct", "key_ops": ["encrypt", "decrypt"], "alg": "A256CTR",
                         "k": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ext": true },
                "iv": "AAAAAAAAAAAAAAAAAAAAAA", "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" }
            },
            "info": { "size": 1234 }
        })));
        assert_eq!(f.msg_type, "m.file");
        assert_eq!(f.file_url, "mxc://x/file");
        assert!(
            f.file_encrypted_json.contains("\"key\""),
            "{}",
            f.file_encrypted_json
        );
        assert_eq!(f.file_name, "report.pdf");
        assert_eq!(f.file_size, 1234);
    }

    #[test]
    fn video_hints_come_from_the_forwarded_info() {
        let f = rendered(&forward_event(json!({
            "msgtype": "m.video", "body": "clip.mp4", "url": "mxc://x/vid",
            "info": { "mimetype": "video/mp4", "fi.mau.gif": true }
        })));
        assert!(f.video_gif && f.video_autoplay && f.video_loop);
    }

    #[test]
    fn invalid_forwarded_content_shows_the_top_level_content_with_the_attribution() {
        // No `msgtype`: not valid forwarded content.
        let ev = forward_event(json!({ "body": "no msgtype" }));
        let v = view(&ev);
        assert!(v.forwarded.content.is_none());
        let mut f = forwarded_message_fields(&v).unwrap();
        assert_eq!(f.body, FALLBACK);
        apply_forward(&mut f, &v.forwarded, String::new());
        assert!(f.forwarded);
        assert_eq!(f.forwarded_sender, "@bob:other.org");
    }

    #[test]
    fn unmappable_forwarded_content_falls_back_to_the_caller() {
        // Valid per the MSC (strings `msgtype` and `body`) but not a message
        // type the timeline shows.
        let ev = forward_event(json!({ "msgtype": "org.example.custom", "body": "x" }));
        assert!(forwarded_message_fields(&view(&ev)).is_none());
    }

    #[test]
    fn a_legacy_forward_has_attribution_and_the_top_level_content() {
        let ev = json!({ "type": "m.room.message", "content": {
            "msgtype": "m.text", "body": "the text",
            "com.famedly.app.forwarded": { "sender": "@bob:other.org" }
        }});
        let v = view(&ev);
        let mut f = forwarded_message_fields(&v).unwrap();
        assert_eq!(f.body, "the text");
        apply_forward(&mut f, &v.forwarded, "Bob".into());
        assert!(f.forwarded);
        assert_eq!(f.forwarded_sender, "@bob:other.org");
        assert_eq!(f.forwarded_sender_name, "Bob");
        assert_eq!(f.forwarded_room_id, "");
        assert_eq!(f.forwarded_event_id, "");
        assert_eq!(f.forwarded_origin_ts, 0);
    }

    #[test]
    fn relations_and_mentions_inside_forwarded_content_are_ignored() {
        let f = rendered(&forward_event(json!({
            "msgtype": "m.text", "body": "hello",
            "m.relates_to": { "rel_type": "m.thread", "event_id": "$root",
                              "m.in_reply_to": { "event_id": "$prev" } },
            "m.mentions": { "user_ids": ["@carol:x"] }
        })));
        assert_eq!(f.body, "hello");
        assert_eq!(f.in_reply_to_id, "");
        assert_eq!(f.thread_root_id, "");

        // An edit relation without its replacement would not even deserialize.
        let f = rendered(&forward_event(json!({
            "msgtype": "m.text", "body": "hello",
            "m.relates_to": { "rel_type": "m.replace", "event_id": "$x" }
        })));
        assert_eq!(f.body, "hello");
    }

    #[test]
    fn bundled_url_previews_come_from_the_forwarded_content() {
        let f = rendered(&forward_event(json!({
            "msgtype": "m.text", "body": "see https://matrix.org",
            "com.beeper.linkpreviews": [ { "matched_url": "https://matrix.org", "og:title": "Matrix" } ]
        })));
        assert!(f.bundled_url_previews_present);
        assert_eq!(f.bundled_url_previews.len(), 1);
        // Without the key the homeserver-preview path stays in charge.
        let f = rendered(&forward_event(
            json!({ "msgtype": "m.text", "body": "see https://matrix.org" }),
        ));
        assert!(!f.bundled_url_previews_present);
    }

    #[test]
    fn attribution_drops_ids_that_are_not_matrix_ids() {
        let mut ev = forward_event(json!({ "msgtype": "m.text", "body": "x" }));
        ev["content"][UNSTABLE_KEY]["sender"] = json!("not an id");
        ev["content"][UNSTABLE_KEY]["room_id"] = json!("javascript:alert(1)");
        ev["content"][UNSTABLE_KEY]["event_id"] = json!("$ok");
        ev["content"][UNSTABLE_KEY]["origin_server_ts"] = json!(-5);
        let v = view(&ev);
        let mut f = TimelineEvent::default();
        apply_forward(&mut f, &v.forwarded, "A name".into());
        assert!(f.forwarded);
        assert_eq!(f.forwarded_sender, "");
        // A name without a sender to belong to is not shown.
        assert_eq!(f.forwarded_sender_name, "");
        assert_eq!(f.forwarded_room_id, "");
        assert_eq!(f.forwarded_event_id, "$ok");
        assert_eq!(f.forwarded_origin_ts, 0);
    }

    #[test]
    fn full_attribution_is_copied() {
        let v = view(&forward_event(json!({ "msgtype": "m.text", "body": "x" })));
        let mut f = TimelineEvent::default();
        apply_forward(&mut f, &v.forwarded, "Bob".into());
        assert_eq!(f.forwarded_sender, "@bob:other.org");
        assert_eq!(f.forwarded_sender_name, "Bob");
        assert_eq!(f.forwarded_room_id, "!orig:other.org");
        assert_eq!(f.forwarded_event_id, "$orig");
        assert_eq!(f.forwarded_origin_ts, 1_690_000_000_000);
    }

    #[test]
    fn a_forwarded_sticker_renders_the_sticker() {
        let ev = json!({ "type": "m.sticker", "content": {
            "body": "Waving", "url": "mxc://x/sticker", "info": { "w": 100, "h": 90 },
            "org.matrix.msc4553.forwarded": { "content": {
                "body": "Waving", "url": "mxc://x/sticker", "info": { "w": 100, "h": 90 } },
                "sender": "@bob:other.org" }
        }});
        let v = forward_view(&ev["content"], "m.sticker").unwrap();
        let f = forwarded_sticker_fields(&v).unwrap();
        assert_eq!(f.msg_type, "m.sticker");
        assert_eq!(f.source_url, "mxc://x/sticker");
        assert_eq!((f.width, f.height), (100, 90));
        assert!(f.sticker_info_json.contains("\"w\""));
    }

    #[test]
    fn a_forwarded_sticker_without_info_is_left_to_the_caller() {
        let ev = json!({ "type": "m.sticker", "content": {
            "body": "S", "url": "mxc://x/s", "info": {},
            "org.matrix.msc4553.forwarded": { "content": { "body": "S", "url": "mxc://x/s" } }
        }});
        let v = forward_view(&ev["content"], "m.sticker").unwrap();
        assert!(forwarded_sticker_fields(&v).is_none());
    }

    #[test]
    fn the_ordinary_path_maps_through_the_same_function() {
        let content: RoomMessageEventContent =
            serde_json::from_value(json!({ "msgtype": "m.notice", "body": "n",
                "format": "org.matrix.custom.html", "formatted_body": "<b>n</b>" }))
            .unwrap();
        let f = message_type_fields(&content.msgtype, &|fb| format!("[{fb}]"), None).unwrap();
        assert_eq!(f.msg_type, "m.notice");
        assert_eq!(f.formatted_body, "[<b>n</b>]");
    }

    fn snippet_of(event: &Value) -> Option<(String, String)> {
        forwarded_snippet_from_event_json(&event.to_string())
    }

    #[test]
    fn a_forward_snippets_as_its_forwarded_content() {
        let ev = forward_event(json!({
            "msgtype": "m.text", "body": "hello *world*",
            "format": "org.matrix.custom.html", "formatted_body": "hello <em>world</em>"
        }));
        let (body, html) = snippet_of(&ev).unwrap();
        assert_eq!(body, "hello *world*");
        assert_eq!(html, "hello <em>world</em>");
    }

    #[test]
    fn a_forwarded_emote_snippets_like_an_ordinary_emote() {
        let (body, html) = snippet_of(&forward_event(
            json!({ "msgtype": "m.emote", "body": "waves" }),
        ))
        .unwrap();
        let ordinary: RoomMessageEventContent =
            serde_json::from_value(json!({ "msgtype": "m.emote", "body": "waves" })).unwrap();
        assert_eq!(body, message_snippet(&ordinary.msgtype));
        assert_eq!(html, "");
    }

    #[test]
    fn a_forward_with_invalid_content_keeps_the_callers_snippet() {
        // No `msgtype`: not valid forwarded content.
        assert!(snippet_of(&forward_event(json!({ "body": "no msgtype" }))).is_none());
    }

    #[test]
    fn a_relation_inside_forwarded_content_does_not_break_the_snippet() {
        let (body, _) = snippet_of(&forward_event(json!({
            "msgtype": "m.text", "body": "hello",
            "m.relates_to": { "rel_type": "m.replace", "event_id": "$x" }
        })))
        .unwrap();
        assert_eq!(body, "hello");
    }

    #[test]
    fn a_forward_without_content_has_no_forwarded_snippet() {
        let legacy = json!({ "type": "m.room.message", "content": {
            "msgtype": "m.text", "body": "Forwarded text",
            "com.famedly.app.forwarded": { "sender": "@bob:other.org" }
        }});
        assert!(snippet_of(&legacy).is_none());
    }

    #[test]
    fn a_text_that_only_says_forwarded_has_no_forwarded_snippet() {
        let ev = json!({ "type": "m.room.message", "content": {
            "msgtype": "m.text", "body": "I forwarded the invoice to Bob" } });
        assert!(snippet_of(&ev).is_none());
    }

    #[test]
    fn only_a_forward_event_of_a_message_has_a_forwarded_snippet() {
        let mut ev = forward_event(json!({ "msgtype": "m.text", "body": "hi" }));
        ev["type"] = json!("m.sticker");
        assert!(snippet_of(&ev).is_none());
    }

    #[test]
    fn a_forwarded_image_snippets_as_the_image_code() {
        let (body, html) = snippet_of(&forward_event(
            json!({ "msgtype": "m.image", "body": "cat.png", "url": "mxc://x/img" }),
        ))
        .unwrap();
        assert_eq!(body, message_snippet(&image_msgtype()));
        assert_eq!(html, "");
    }

    fn image_msgtype() -> MessageType {
        let c: RoomMessageEventContent = serde_json::from_value(
            json!({ "msgtype": "m.image", "body": "cat.png", "url": "mxc://x/img" }),
        )
        .unwrap();
        c.msgtype
    }

    #[test]
    fn only_text_that_may_be_a_fallback_is_looked_up() {
        let msg = |v: Value| serde_json::from_value::<RoomMessageEventContent>(v).unwrap().msgtype;
        assert!(may_be_forward_fallback(&msg(
            json!({ "msgtype": "m.text", "body": "Forwarded from Bob - view original message: x" })
        )));
        assert!(!may_be_forward_fallback(&msg(
            json!({ "msgtype": "m.text", "body": "hello" })
        )));
        assert!(!may_be_forward_fallback(&msg(
            json!({ "msgtype": "m.emote", "body": "Forwarded from Bob" })
        )));
        assert!(!may_be_forward_fallback(&image_msgtype()));
    }
}
