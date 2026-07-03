#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "tesseract/media_source.h"

namespace tesseract
{

enum class EventType
{
    Text,
    Image,
    File,
    Sticker,
    Audio,
    Voice,
    Video,
    Redacted,
    Notice,
    Emote,
    Unhandled,
    DaySeparator,
    ReadMarker,
    TimelineStart,
    Location,
    Utd,    // appended (not slotted near Redacted) so existing enum-int
            // pinned tests stay valid
    PinnedEvent, // m.room.pinned_events state-event timeline row
    CallNotification, // org.matrix.msc4075.rtc.notification
    Membership, // m.room.member state-event row (join/leave/kick/ban/invite/knock/…)
};

/// One `m.room.member` membership transition, computed server-side by
/// matrix-sdk-ui from a state-event diff. Mirrors the discriminant strings
/// produced by the Rust `membership_action_str` helper (see
/// sdk/src/client/timeline_convert.rs) — never English prose; C++ owns all
/// phrase composition via tk::tr()/tk::trn()/tk::trf() per this repo's
/// i18n rule.
enum class MembershipAction
{
    Joined,
    Left,
    Banned,
    Unbanned,
    Kicked,
    Invited,
    KickedAndBanned,
    InvitationAccepted,
    InvitationRejected,
    InvitationRevoked,
    Knocked,
    KnockAccepted,
    KnockRetracted,
    KnockDenied,
};

/// User presence state. Wire encoding (matches sdk/src/bridge.rs on_presence_changed):
///   1 = Online  2 = Unavailable  3 = Offline (also used as "unknown/no data received").
enum class PresenceState : uint8_t
{
    Online,
    Unavailable,
    Offline,
};

/// One aggregated reaction key attached to a timeline event.
/// `senders.size() == count`. The UI uses `senders` to populate the chip's
/// tooltip (e.g. "Reacted by:\n  Alice\n  Bob"). Each sender entry is the
/// member's display name when resolvable from the room state, otherwise the
/// bare Matrix ID.
struct Reaction
{
    std::string key;
    uint64_t count = 0;
    bool reacted_by_me = false;
    /// MediaSource for MSC4027 custom-image reactions; nullptr for plain
    /// Unicode reactions. Always a plain mxc:// source (reactions reference
    /// existing pack images). Pass fetch_token() to fetch_source_bytes.
    MediaSourceRef source;
    std::vector<std::string> senders;
};

/// One user's most recent read receipt landing on a timeline event.
/// `display_name` is the room member's resolved name (falls back to the
/// bare Matrix ID when membership isn't yet hydrated); `avatar_url` is the
/// member's mxc:// URI, empty when unset. Receipts for the *current* user
/// are filtered on the SDK side — the UI never has to render its own
/// avatar on every message it has read.
struct ReadReceipt
{
    std::string user_id;
    std::string display_name;
    std::string avatar_url;
};

/// A joined member of a room. `display_name` resolves to the user's
/// localpart when no display name is set. `avatar_url` is the mxc://
/// URI of the member's avatar, or empty when unset.
struct RoomMember
{
    std::string user_id;
    std::string display_name; ///< resolves to user_id localpart when unset
    std::string avatar_url;   ///< mxc:// or empty
};

/// Result of Client::resolve_user_profile. `exists` is true only when the
/// homeserver returned a profile for the requested mxid (i.e. the user exists).
/// When `exists` is false the other fields are empty.
struct UserProfile
{
    bool        exists = false;
    std::string user_id;
    std::string display_name; ///< resolves to user_id localpart when unset
    std::string avatar_url;   ///< mxc:// or empty
    std::string pronouns;     ///< MSC4247 summary text, empty if not set
    std::string tz;           ///< MSC4175 IANA timezone string, empty if not set
    std::string biography;    ///< MSC4440 plain-text body, empty if not set
    /// Deserialise the JSON produced by `get_extended_profile_async` /
    /// `resolve_user_profile_async`. Returns `exists=false` on parse failure.
    static UserProfile from_json(const std::string& json);
};

struct Event
{
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string sender_name;
    std::string sender_avatar_url;
    std::string body;
    /// HTML body when format == "org.matrix.custom.html"; empty for all other event types.
    std::string formatted_body;
    uint64_t timestamp = 0;
    EventType type = EventType::Unhandled;
    std::vector<Reaction> reactions;
    std::vector<ReadReceipt> read_receipts;
    /// Event ID being replied to. Empty when this event is not a reply.
    std::string in_reply_to_id;
    /// Resolved display name of the replied-to sender (bare Matrix ID as fallback).
    std::string in_reply_to_sender_name;
    /// Short body snippet of the replied-to message. "(image)" / "(file)" /
    /// "(audio)" / "(voice)" / "(sticker)" / "(deleted)" for non-text / redacted originals.
    std::string in_reply_to_body;
    /// mxc:// URI of the replied-to image thumbnail (full-res when no thumbnail).
    /// Non-empty only when the reply target is an m.image event in the local cache.
    std::string in_reply_to_image_url;
    /// JSON-serialised EncryptedFile for in_reply_to_image_url when E2EE. Empty otherwise.
    std::string in_reply_to_image_encrypted_json;
    /// True when the body has been superseded by an m.replace edit.
    /// Only set for TextEvent; always false for other types.
    bool is_edited = false;
    /// MSC3440 threads. Non-empty when this event is an in-thread reply.
    std::string thread_root_id;
    /// True when this event roots a thread.
    bool is_thread_root = false;
    /// Replies in the thread excluding the root; 0 when not a root.
    uint64_t thread_reply_count = 0;
    /// Latest thread reply preview (for a "N replies" affordance on the root).
    std::string thread_latest_sender_name;
    std::string thread_latest_body;
    uint64_t thread_latest_ts = 0;
    /// Local-echo send state. "" = server event; "sending" = in-flight;
    /// "failed" = delivery failed.
    std::string pending_state;
    std::string pending_error;
    bool pending_recoverable = false;
    std::string pending_txn_id;
    virtual ~Event() = default;
};

struct TextEvent : public Event
{
    TextEvent()
    {
        type = EventType::Text;
    }
};

struct NoticeEvent : public Event
{
    NoticeEvent()
    {
        type = EventType::Notice;
    }
};

struct EmoteEvent : public Event
{
    EmoteEvent()
    {
        type = EventType::Emote;
    }
};

struct ImageEvent : public Event
{
    MediaSourceRef source;    // full-resolution media
    MediaSourceRef thumbnail; // nullptr when absent
    uint64_t width = 0;
    uint64_t height = 0;
    /// Non-empty only when the sender supplied an MSC2530 `filename` field.
    /// When set, `body` is a user caption and should be displayed below the image.
    std::string filename;
    std::string blurhash;  // MSC2448: xyz.amorgan.blurhash; empty when absent
    bool animated = false; // MSC4230: is_animated hint from sender

    ImageEvent()
    {
        type = EventType::Image;
    }
};

struct StickerEvent : public Event
{
    MediaSourceRef source;    // full-resolution media
    MediaSourceRef thumbnail; // nullptr when absent
    uint64_t width = 0;
    uint64_t height = 0;
    std::string blurhash;  // MSC2448: xyz.amorgan.blurhash; empty when absent
    std::string info_json; // JSON-serialised ImageInfo from sender
    bool animated = false; // MSC4230: is_animated hint from sender

    StickerEvent()
    {
        type = EventType::Sticker;
    }
};

/// Tombstone placeholder for a redacted (deleted) event. `body` is empty;
/// UIs render an italic "Message deleted" line in its place.
struct RedactedEvent : public Event
{
    RedactedEvent()
    {
        type = EventType::Redacted;
    }
};

/// Placeholder for an event the local crypto store can't decrypt. `body`
/// carries the human-readable single-line reason (e.g. "🔒 Unable to
/// decrypt"). No content fields are populated — UIs paint `body` muted,
/// like the redacted tombstone.
struct UtdEvent : public Event
{
    UtdEvent()
    {
        type = EventType::Utd;
    }
};

struct FileEvent : public Event
{
    MediaSourceRef source; // file attachment source
    uint64_t file_size = 0;
    /// Non-empty only when the sender supplied an MSC2530 `filename` field.
    /// When set, `body` is a user caption and should be displayed below the card.
    /// When empty, `body` is the fallback display name (legacy behaviour).
    std::string filename;

    FileEvent()
    {
        type = EventType::File;
    }
};

/// Plain `m.audio` event (no MSC3245 voice marker). Rendered as an inline
/// audio player card (play/pause + linear scrub track) rather than a download
/// file card.
struct AudioEvent : public Event
{
    MediaSourceRef source; // audio clip source
    std::string mime_type;                     // e.g. "audio/mpeg"; may be empty
    uint64_t duration_ms = 0;
    std::string filename;  // from m.audio body / filename field
    uint64_t file_size = 0;

    AudioEvent()
    {
        type = EventType::Audio;
    }
};

/// MSC3245 voice message. `waveform` holds the sender-supplied MSC1767
/// samples (each clamped to 0..=1024); when empty the UI renders flat
/// placeholder bars.
struct VoiceEvent : public Event
{
    MediaSourceRef source; // voice clip source
    std::string mime_type;                     // e.g. "audio/ogg"; may be empty
    uint64_t duration_ms = 0;
    std::vector<uint16_t> waveform; // MSC1767 amplitudes, 0..=1024

    VoiceEvent()
    {
        type = EventType::Voice;
    }
};

/// `m.video` message. `filename` is the MSC2530 caption filename;
/// empty → no caption below the card.
struct VideoEvent : public Event
{
    MediaSourceRef source;    // video source
    MediaSourceRef thumbnail; // nullptr when absent
    std::string mime_type;                        // e.g. "video/mp4"
    uint64_t width = 0;
    uint64_t height = 0;
    uint64_t duration_ms = 0;
    std::string filename; // MSC2530 caption filename; empty → no caption
    std::string blurhash; // MSC2448: xyz.amorgan.blurhash; empty when absent

    // fi.mau.* vendor hints — each false when absent in content.info.
    bool autoplay = false;      // start playback immediately on load
    bool loop = false;          // restart at end-of-stream
    bool no_audio = false;      // mute the audio track
    bool hide_controls = false; // suppress player controls bar
    bool gif = false;           // composite: implies all four above

    VideoEvent()
    {
        type = EventType::Video;
    }
};

struct LocationEvent : public Event
{
    double lat = 0.0;
    double lon = 0.0;
    std::string description;

    LocationEvent()
    {
        type = EventType::Location;
    }
};

/// Fallback for message types we don't handle yet (reactions, polls, etc.)
struct UnhandledEvent : public Event
{
    std::string msg_type;

    UnhandledEvent()
    {
        type = EventType::Unhandled;
    }
    explicit UnhandledEvent(const std::string& t) : msg_type(t)
    {
        type = EventType::Unhandled;
    }
};

// Virtual timeline items forwarded from matrix-sdk-ui.
// DaySeparatorEvent: `timestamp` = day epoch in ms (local midnight per SDK).
// ReadMarkerEvent:   marks the user's last-read position in the room.
// TimelineStartEvent: no earlier history to paginate (back-pagination complete).
struct DaySeparatorEvent : public Event
{
    DaySeparatorEvent()
    {
        type = EventType::DaySeparator;
    }
};
struct ReadMarkerEvent : public Event
{
    ReadMarkerEvent()
    {
        type = EventType::ReadMarker;
    }
};
struct TimelineStartEvent : public Event
{
    TimelineStartEvent()
    {
        type = EventType::TimelineStart;
    }
};

/// m.room.pinned_events state event surfaced as a timeline row.
/// `sender_name` and `body` (the action: "pinned a message" etc.) come from
/// the base `Event` fields; no additional payload is needed.
struct PinnedStateEvent : public Event
{
    PinnedStateEvent()
    {
        type = EventType::PinnedEvent;
    }
};

/// org.matrix.msc4075.rtc.notification — MatrixRTC call ring/notification event.
/// `body` carries the m.call.intent value: "audio" | "video" | "" (unknown/absent).
struct CallNotificationEvent : public Event
{
    CallNotificationEvent()
    {
        type = EventType::CallNotification;
    }
};

/// m.room.member state event representing a real membership transition
/// (join/leave/kick/ban/invite/knock and their accept/reject/revoke
/// counterparts). `sender`/`sender_name`/`sender_avatar_url` (base Event
/// fields) identify who performed the action; the fields below identify
/// whose membership changed — the *target*, which may differ from the
/// sender (e.g. an admin kicking/banning/inviting a different user).
struct MembershipStateEvent : public Event
{
    MembershipStateEvent()
    {
        type = EventType::Membership;
    }
    MembershipAction action = MembershipAction::Joined;
    std::string target_user_id;
    std::string target_display_name; ///< as recorded in this state event; may be empty
    std::string target_avatar_url;   ///< mxc:// or empty
};

/// Ordered list of timeline events (oldest-first), as passed to
/// IEventHandler::on_timeline_reset and the handle_*_ui_ virtuals.
using EventList = std::vector<std::unique_ptr<Event>>;

/// One thread in a room. `latest_*` fields are empty/0 when no reply summary
/// is available. Mirror of the FFI ThreadInfo.
struct ThreadInfo
{
    std::string root_event_id;
    std::string root_sender_name;
    std::string root_body;
    uint64_t root_timestamp = 0;
    std::string latest_event_id;
    std::string latest_sender_name;
    std::string latest_body;
    uint64_t latest_timestamp = 0;
    uint64_t num_replies = 0;
};

/// One entry from `m.room.pinned_events` resolved against the local event
/// cache for the pinned-events banner. `body_preview` falls back to
/// `(unavailable)` when the id cannot be resolved without a network
/// round-trip — in that case `sender_name` is empty and `timestamp` is 0,
/// but click-to-jump still works for events present in loaded history.
struct PinnedEvent
{
    std::string event_id;
    std::string sender_name;
    std::string body_preview;
    std::uint64_t timestamp = 0;
};

struct RoomInfo
{
    std::string id;
    std::string name;
    std::string topic;
    /// Messages matching a notify push-rule action (server-side count).
    uint64_t notification_count = 0;
    /// Subset of notification_count that matched a highlight/mention action.
    uint64_t highlight_count = 0;
    /// Total unread messages (client-side, regardless of push rules). A
    /// superset of notification_count; drives the room-list "quiet unread" dot
    /// for rooms whose activity doesn't notify (e.g. "mentions only").
    uint64_t unread_count = 0;
    /// True when this room's notification mode is Mute. Muted rooms are excluded
    /// from the quiet-unread dot (the user silenced them on purpose).
    bool muted = false;
    bool is_direct = false;
    std::string avatar_url;
    /// Fallback avatar mxc for an avatar-less DM: the other participant's
    /// avatar (with bridge bots filtered out via MSC4171). Empty when the
    /// room is not a DM, when it already has its own avatar, or when no
    /// real counterpart could be identified. Render sites should prefer
    /// `effective_avatar_url()` rather than reading either field directly.
    std::string dm_avatar_url;
    /// Bare Matrix ID of the DM counterpart (e.g. "@alice:server").
    /// Empty when not a 1:1 DM or no real counterpart was identified.
    /// Use as the key for presence lookups.
    std::string dm_counterpart_user_id;
    std::string last_message_body;
    /// Display name of the last-message sender; empty when the sender is the
    /// current user (render as "You"), or when there is no last message.
    std::string last_message_sender_name;
    /// Kind of the latest event for preview rendering: "text" | "image" |
    /// "video" | "gif" | "file" | "audio" | "sticker" | "" (nothing to preview).
    std::string last_message_kind;
    /// mxc:// URI of the sticker image when last_message_kind == "sticker".
    std::string last_message_sticker_url;
    /// mxc:// URI for the room-list thumbnail chip: set for "image" (the image
    /// mxc) and "sticker" (same as last_message_sticker_url); empty otherwise.
    std::string last_message_thumbnail_url;
    uint64_t last_activity_ts = 0;
    bool is_space = false;
    bool is_favorite = false;
    bool is_low_priority = false;
    /// HTML body from the MSC3765 m.topic block; empty when absent.
    std::string topic_html;
    /// True when the room has encryption enabled.
    bool is_encrypted = false;
    /// True when any participant has an active MatrixRTC call in this room
    /// (`m.call.member` state events with non-empty content).
    bool has_active_call = false;
    /// True when the room has a `uk.half-shot.bridge` state event (MSC2346).
    /// Calls and threads are suppressed for bridged rooms.
    bool is_bridged = false;
    /// Room history visibility: "world_readable" | "shared" | "invited" | "joined".
    std::string history_visibility;
    /// Snapshot of `m.room.pinned_events` resolved against the local event
    /// cache (sender + body snippet + timestamp), sorted newest-first so the
    /// pinned-events banner can render without a separate fetch.
    std::vector<PinnedEvent> pinned_events;
    /// Canonical alias of the room (`#alias:server`), empty when none is set.
    /// Read from local state — no network round-trip.
    std::string canonical_alias;

    /// Effective avatar mxc to render for this room: the room's own avatar
    /// when set, otherwise the DM-counterpart fallback. May be empty (caller
    /// should draw an initials disc in that case).
    const std::string& effective_avatar_url() const
    {
        return avatar_url.empty() ? dm_avatar_url : avatar_url;
    }
};

/// Lightweight descriptor for a pending room invitation, returned by
/// `Client::list_invites()` and carried by `IEventHandler::on_invites_updated()`.
/// `invited_at_ts` is the Unix timestamp in milliseconds of the invite event;
/// 0 when unavailable (stripped-state events omit the timestamp unless the
/// homeserver implements MSC4319).
struct InviteInfo
{
    std::string room_id;
    std::string room_name;
    std::string room_avatar_url;
    std::string room_topic;
    bool        is_direct = false;
    std::string inviter_user_id;
    std::string inviter_display_name;
    std::string inviter_avatar_url;
    uint64_t    invited_at_ts = 0;
};

/// MSC3266 room summary — metadata about a room fetched without joining.
/// `room_id` is empty when the lookup failed (check before using).
struct RoomSummary
{
    std::string room_id;
    std::string canonical_alias; ///< `#alias:server` or empty
    std::string name;
    std::string topic;
    std::string avatar_url; ///< mxc:// URI or empty
    uint32_t num_joined_members = 0;
    /// "public", "invite", "knock", "knock_restricted",
    /// "restricted", "private", or "unknown"
    std::string join_rule;
    bool world_readable = false;
    bool guest_can_join = false;
    std::string
        encryption; ///< encryption algorithm or empty when not encrypted
    bool is_space = false;
    /// Current user's membership in this room: "join", "invite",
    /// "leave", "ban", "knock", or empty when unknown / unauthenticated.
    std::string membership;

    bool ok() const
    {
        return !room_id.empty();
    }
    /// True only for rules that allow joining without prior membership in
    /// another room. "restricted" / "knock_restricted" require belonging to
    /// a specific room and are therefore not freely open.
    bool is_join_open() const
    {
        return join_rule == "public" || join_rule == "knock";
    }
    /// Deserialise a JSON string produced by the Rust `get_space_child_summary_async`
    /// callback. Returns an empty (ok()==false) summary on parse failure or empty input.
    static RoomSummary from_json(const std::string& json);
};

/// MSC4278 media-preview controls, stored in the global
/// `m.media_preview_config` account-data event so they follow the user
/// across devices and clients.
struct MediaPreviewConfig
{
    /// Auto-load policy for media (images/videos/stickers/file thumbnails).
    /// Mirrors the `u8` carried over the FFI: 0 = off, 1 = private, 2 = on.
    enum class Mode : uint8_t
    {
        Off = 0,     ///< never auto-load media
        Private = 1, ///< only in non-public rooms
        On = 2,      ///< always (the MSC default)
    };
    Mode media_previews = Mode::On;
    bool invite_avatars = true; ///< show room/inviter avatars on pending invites
    /// Deserialise `{"media_previews":N,"invite_avatars":bool}` produced by
    /// the Rust `media_preview_config_async` callback. Returns MSC defaults on
    /// parse failure.
    static MediaPreviewConfig from_json(const std::string& json);
};

/// Per-room MSC4278 context for the open room, returned by
/// `Client::room_media_preview_override`.
struct MediaPreviewOverride
{
    /// True when the room's own account-data overrides `media_previews`;
    /// otherwise the caller falls back to the global value.
    bool has_media_previews = false;
    MediaPreviewConfig::Mode media_previews = MediaPreviewConfig::Mode::On;
    /// The room's local join rule ("public", "invite", "knock", "restricted",
    /// "knock_restricted", "private"), or "" when indeterminate. Used to
    /// evaluate `Mode::Private`; empty / unknown is treated as public.
    std::string join_rule;
    /// Deserialise `{"has_media_previews":bool,"media_previews":N,"join_rule":"..."}`.
    static MediaPreviewOverride from_json(const std::string& json);
};

/// Server-side key-backup state. Mirrors the encoding of the `u8`-typed
/// `state` field carried over the FFI in `BackupProgress` (see
/// `sdk/src/bridge.rs`).
enum class BackupState : uint8_t
{
    Unknown = 0,
    Disabled = 1,
    Enabled = 2,
    Downloading = 3,
    Creating = 4,
};

/// Snapshot of server-side key-backup status plus a running count of room
/// keys imported into this device. Carried by `IEventHandler::on_backup_progress`
/// and returned by `Client::backup_state()`.
struct BackupProgress
{
    BackupState state = BackupState::Unknown;
    /// Room keys imported into the local store since `start_sync` began.
    uint64_t imported_keys = 0;
    /// Best-effort total of room keys present on the server-side backup,
    /// or 0 when unknown (currently always 0 — matrix-sdk does not expose
    /// a cheap way to query this).
    uint64_t total_keys = 0;
};

/// One emoji from the 7-emoji Short Authentication String (SAS) used for
/// device cross-signing verification. Mirrors `VerificationEmoji` in the
/// Rust FFI bridge.
struct VerificationEmoji
{
    std::string symbol;      // UTF-8 emoji glyph, e.g. "🐶"
    std::string description; // English label, e.g. "Dog"
};

/// One GIF search result, surfaced via `IEventHandler::on_gif_results`. URLs
/// point at the provider CDN: `preview_url` is a small static JPEG for the
/// inline result strip; `image_url` is the animated form (MP4 preferred, WebP
/// fallback, GIF last resort, per `image_mime`) fetched at send time.
/// Mirrors the `GifResult` cxx bridge struct.
struct GifResult
{
    std::string id;
    std::string preview_url;
    std::uint32_t preview_w = 0;
    std::uint32_t preview_h = 0;
    /// Animated *send* form (MP4 preferred). Uploaded when the user picks a GIF.
    std::string image_url;
    std::uint32_t image_w = 0;
    std::uint32_t image_h = 0;
    std::string image_mime;
    /// Animated form the *strip* displays — WebP/GIF decoded natively (no video
    /// pipeline); falls back to image_url for MP4-only entries.
    std::string strip_url;
    std::string strip_mime;
};

/// One full-text message-search hit, surfaced via
/// `IEventHandler::on_search_results`. Comes from the local FTS5 index (covers
/// encrypted rooms — the bodies are already decrypted). `room_name` is resolved
/// at query time so the UI can render a result row without a lookup; jump to the
/// message via `room_id` + `event_id`. Mirrors the `SearchHit` cxx bridge struct.
struct SearchHit
{
    std::string event_id;
    std::string room_id;
    std::string room_name;
    std::string sender;
    std::string sender_name;
    std::string body;
    std::uint64_t timestamp_ms = 0;
};

/// Summary of the local full-text search index, for the Settings panel.
/// Mirrors the `SearchIndexStats` cxx bridge struct. `backfill_done` is true
/// once the one-time history crawl has finished; `oldest_ts_ms` is 0 when the
/// index is empty.
struct SearchIndexStats
{
    std::uint64_t message_count = 0;
    std::uint64_t room_count = 0;
    std::uint64_t oldest_ts_ms = 0;
    bool backfill_done = false;
    /// On-disk size from `dbstat`; populated C++-side once per panel open (not
    /// carried through the FFI struct to avoid an O(pages) walk on every poll).
    std::uint64_t index_bytes = 0;
};

/// One persisted room-summary-backoff entry loaded from `app_cache.db` at sync-start.
/// Mirrors the `RoomSummaryBackoffEntry` cxx bridge struct.
struct RoomSummaryBackoffEntry
{
    std::string room_id;
    std::uint32_t attempts = 0;
    std::int64_t deadline_secs = 0;
};

/// One persisted media-backoff entry loaded from `app_cache.db` at sync-start.
/// Mirrors the `MediaBackoffEntry` cxx bridge struct.
struct MediaBackoffEntry
{
    /// MXC URI or HTTP URL that previously failed to fetch.
    std::string url;
    /// Clamped attempt count (1–7); determines the next backoff window on
    /// subsequent failure.
    std::uint32_t attempts = 0;
    /// Unix epoch seconds (UTC) after which the URL may be retried.
    std::int64_t deadline_secs = 0;
};

/// Plain-data descriptor for a MatrixRTC call participant.
/// Always defined so the bridge header is unconditional; the fields are only
/// populated when `TESSERACT_CALLS_ENABLED` is set.
struct RtcParticipantInfo
{
    std::string participant_id;
    std::string user_id;
    std::string device_id;
    bool is_audio_muted    = false;
    bool is_video_muted    = false;
    bool is_screen_sharing = false;
};

/// High-level phases of the sliding-sync `RoomListService`. Surfaced via
/// `IEventHandler::on_room_list_state` so UIs can render a "Syncing
/// rooms…" indicator while the joined-room set is still being hydrated.
/// Mirrors the u8 codes in `sdk/src/client.rs` (`ROOM_LIST_STATE_*`).
enum class RoomListState : uint8_t
{
    /// Initial state — no sync has run yet.
    Init = 0,
    /// First sync is in flight; the joined-room set is filling in.
    SettingUp = 1,
    /// Recovering after Error/Terminated or a stale session window.
    Recovering = 2,
    /// Steady-state — all rooms are syncing.
    Running = 3,
    /// Sync stopped due to an error (the SDK will normally retry).
    Error = 4,
    /// Sync stopped intentionally (e.g. shutdown).
    Terminated = 5,
};

} // namespace tesseract