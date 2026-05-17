#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract {

enum class EventType {
    Text, Image, File, Sticker, Voice, Video, Redacted, Notice, Emote, Unhandled,
    DaySeparator, ReadMarker, TimelineStart,
    Location,
};

/// One aggregated reaction key attached to a timeline event.
/// `senders.size() == count`. The UI uses `senders` to populate the chip's
/// tooltip (e.g. "Reacted by:\n  Alice\n  Bob"). Each sender entry is the
/// member's display name when resolvable from the room state, otherwise the
/// bare Matrix ID.
struct Reaction {
    std::string key;
    uint64_t    count         = 0;
    bool        reacted_by_me = false;
    /// JSON `MediaSource` for MSC 4027 custom-image reactions; empty for
    /// plain Unicode reactions. Pass to `Client::fetch_media_bytes` (treats
    /// it as an mxc:// URI when not a full JSON blob) to download the chip
    /// icon.
    std::string source_json;
    std::vector<std::string> senders;
};

/// One user's most recent read receipt landing on a timeline event.
/// `display_name` is the room member's resolved name (falls back to the
/// bare Matrix ID when membership isn't yet hydrated); `avatar_url` is the
/// member's mxc:// URI, empty when unset. Receipts for the *current* user
/// are filtered on the SDK side — the UI never has to render its own
/// avatar on every message it has read.
struct ReadReceipt {
    std::string user_id;
    std::string display_name;
    std::string avatar_url;
};

struct Event {
    std::string event_id;
    std::string room_id;
    std::string sender;
    std::string sender_name;
    std::string sender_avatar_url;
    std::string body;
    /// HTML body when format == "org.matrix.custom.html"; empty for all other event types.
    std::string formatted_body;
    uint64_t    timestamp = 0;
    EventType   type = EventType::Unhandled;
    std::vector<Reaction> reactions;
    std::vector<ReadReceipt> read_receipts;
    /// Event ID being replied to. Empty when this event is not a reply.
    std::string in_reply_to_id;
    /// Resolved display name of the replied-to sender (bare Matrix ID as fallback).
    std::string in_reply_to_sender_name;
    /// Short body snippet of the replied-to message. "(image)" / "(file)" /
    /// "(voice)" / "(sticker)" / "(deleted)" for non-text / redacted originals.
    std::string in_reply_to_body;
    /// True when the body has been superseded by an m.replace edit.
    /// Only set for TextEvent; always false for other types.
    bool is_edited = false;
    /// Local-echo send state. "" = server event; "sending" = in-flight;
    /// "failed" = delivery failed.
    std::string pending_state;
    std::string pending_error;
    bool        pending_recoverable = false;
    std::string pending_txn_id;
    virtual ~Event() = default;
};

struct TextEvent : public Event {
    TextEvent() { type = EventType::Text; }
};

struct NoticeEvent : public Event {
    NoticeEvent() { type = EventType::Notice; }
};

struct EmoteEvent : public Event {
    EmoteEvent() { type = EventType::Emote; }
};

struct ImageEvent : public Event {
    std::string image_url;   // mxc:// URI
    uint64_t    width  = 0;
    uint64_t    height = 0;
    /// Non-empty only when the sender supplied an MSC2530 `filename` field.
    /// When set, `body` is a user caption and should be displayed below the image.
    std::string filename;
    std::string blurhash;   // MSC2448: xyz.amorgan.blurhash; empty when absent
    bool        animated = false; // MSC4230: is_animated hint from sender

    ImageEvent() { type = EventType::Image; }
};

struct StickerEvent : public Event {
    std::string image_url;   // mxc:// URI
    uint64_t    width  = 0;
    uint64_t    height = 0;
    std::string blurhash;   // MSC2448: xyz.amorgan.blurhash; empty when absent
    std::string info_json;  // JSON-serialised ImageInfo from sender
    bool        animated = false; // MSC4230: is_animated hint from sender

    StickerEvent() { type = EventType::Sticker; }
};

/// Tombstone placeholder for a redacted (deleted) event. `body` is empty;
/// UIs render an italic "Message deleted" line in its place.
struct RedactedEvent : public Event {
    RedactedEvent() { type = EventType::Redacted; }
};

struct FileEvent : public Event {
    std::string file_url;   // mxc:// URI
    std::string file_name;
    uint64_t    file_size = 0;

    FileEvent() { type = EventType::File; }
};

/// MSC3245 voice message. `audio_source` carries the full JSON-serialised
/// `MediaSource` (plain mxc:// or encrypted), suitable for
/// `Client::fetch_source_bytes`. `waveform` holds the sender-supplied
/// MSC1767 samples (each clamped to 0..=1024); when empty the UI renders
/// flat placeholder bars. Plain `m.audio` events (without the voice marker)
/// surface as `FileEvent`, not `VoiceEvent`.
struct VoiceEvent : public Event {
    std::string audio_source;          // JSON MediaSource for fetch_source_bytes
    std::string mime_type;             // e.g. "audio/ogg"; may be empty
    uint64_t    duration_ms = 0;
    std::vector<uint16_t> waveform;    // MSC1767 amplitudes, 0..=1024

    VoiceEvent() { type = EventType::Voice; }
};

/// `m.video` message. `video_url` carries the full JSON-serialised
/// `MediaSource` (plain mxc:// or encrypted), suitable for
/// `Client::fetch_source_bytes`. `thumbnail_url` carries the thumbnail
/// MediaSource JSON; empty when the server omits it (client generates one
/// from the first frame). `filename` is the MSC2530 caption filename;
/// empty → no caption below the card.
struct VideoEvent : public Event {
    std::string video_url;       // JSON MediaSource for fetch_source_bytes
    std::string thumbnail_url;   // thumbnail MediaSource JSON; empty when absent
    std::string mime_type;       // e.g. "video/mp4", "video/webm"
    uint64_t    width       = 0;
    uint64_t    height      = 0;
    uint64_t    duration_ms = 0;
    std::string filename;        // MSC2530 caption filename; empty → no caption
    std::string blurhash;        // MSC2448: xyz.amorgan.blurhash; empty when absent

    // fi.mau.* vendor hints — each false when absent in content.info.
    bool autoplay      = false;  // start playback immediately on load
    bool loop          = false;  // restart at end-of-stream
    bool no_audio      = false;  // mute the audio track
    bool hide_controls = false;  // suppress player controls bar
    bool gif           = false;  // composite: implies all four above

    VideoEvent() { type = EventType::Video; }
};

struct LocationEvent : public Event {
    double      lat  = 0.0;
    double      lon  = 0.0;
    std::string description;

    LocationEvent() { type = EventType::Location; }
};

/// Fallback for message types we don't handle yet (reactions, polls, etc.)
struct UnhandledEvent : public Event {
    std::string msg_type;

    UnhandledEvent() { type = EventType::Unhandled; }
    explicit UnhandledEvent(const std::string& t) : msg_type(t) { type = EventType::Unhandled; }
};

// Virtual timeline items forwarded from matrix-sdk-ui.
// DaySeparatorEvent: `timestamp` = day epoch in ms (local midnight per SDK).
// ReadMarkerEvent:   marks the user's last-read position in the room.
// TimelineStartEvent: no earlier history to paginate (back-pagination complete).
struct DaySeparatorEvent  : public Event { DaySeparatorEvent()  { type = EventType::DaySeparator;  } };
struct ReadMarkerEvent    : public Event { ReadMarkerEvent()    { type = EventType::ReadMarker;    } };
struct TimelineStartEvent : public Event { TimelineStartEvent() { type = EventType::TimelineStart; } };

struct RoomInfo {
    std::string id;
    std::string name;
    std::string topic;
    uint64_t    unread_count       = 0;
    bool        is_direct          = false;
    std::string avatar_url;
    std::string last_message_body;
    /// Display name of the last-message sender; empty when the sender is the
    /// current user (render as "You"), or when there is no last message.
    std::string last_message_sender_name;
    uint64_t    last_activity_ts   = 0;
    bool        is_space           = false;
    bool        is_favorite        = false;
    /// HTML body from the MSC3765 m.topic block; empty when absent.
    std::string topic_html;
};

/// MSC3266 room summary — metadata about a room fetched without joining.
/// `room_id` is empty when the lookup failed (check before using).
struct RoomSummary {
    std::string room_id;
    std::string canonical_alias;  ///< `#alias:server` or empty
    std::string name;
    std::string topic;
    std::string avatar_url;       ///< mxc:// URI or empty
    uint32_t    num_joined_members = 0;
    /// "public", "invite", "knock", "knock_restricted",
    /// "restricted", "private", or "unknown"
    std::string join_rule;
    bool        world_readable  = false;
    bool        guest_can_join  = false;
    std::string encryption;       ///< encryption algorithm or empty when not encrypted
    bool        is_space         = false;
    /// Current user's membership in this room: "join", "invite",
    /// "leave", "ban", "knock", or empty when unknown / unauthenticated.
    std::string membership;

    bool ok()     const { return !room_id.empty(); }
    /// True only for rules that allow joining without prior membership in
    /// another room. "restricted" / "knock_restricted" require belonging to
    /// a specific room and are therefore not freely open.
    bool is_join_open() const { return join_rule == "public" || join_rule == "knock"; }
};

/// Server-side key-backup state. Mirrors the encoding of the `u8`-typed
/// `state` field carried over the FFI in `BackupProgress` (see
/// `sdk/src/bridge.rs`).
enum class BackupState : uint8_t {
    Unknown     = 0,
    Disabled    = 1,
    Enabled     = 2,
    Downloading = 3,
    Creating    = 4,
};

/// Snapshot of server-side key-backup status plus a running count of room
/// keys imported into this device. Carried by `IEventHandler::on_backup_progress`
/// and returned by `Client::backup_state()`.
struct BackupProgress {
    BackupState state         = BackupState::Unknown;
    /// Room keys imported into the local store since `start_sync` began.
    uint64_t    imported_keys = 0;
    /// Best-effort total of room keys present on the server-side backup,
    /// or 0 when unknown (currently always 0 — matrix-sdk does not expose
    /// a cheap way to query this).
    uint64_t    total_keys    = 0;
};

/// One emoji from the 7-emoji Short Authentication String (SAS) used for
/// device cross-signing verification. Mirrors `VerificationEmoji` in the
/// Rust FFI bridge.
struct VerificationEmoji {
    std::string symbol;       // UTF-8 emoji glyph, e.g. "🐶"
    std::string description;  // English label, e.g. "Dog"
};

/// High-level phases of the sliding-sync `RoomListService`. Surfaced via
/// `IEventHandler::on_room_list_state` so UIs can render a "Syncing
/// rooms…" indicator while the joined-room set is still being hydrated.
/// Mirrors the u8 codes in `sdk/src/client.rs` (`ROOM_LIST_STATE_*`).
enum class RoomListState : uint8_t {
    /// Initial state — no sync has run yet.
    Init       = 0,
    /// First sync is in flight; the joined-room set is filling in.
    SettingUp  = 1,
    /// Recovering after Error/Terminated or a stale session window.
    Recovering = 2,
    /// Steady-state — all rooms are syncing.
    Running    = 3,
    /// Sync stopped due to an error (the SDK will normally retry).
    Error      = 4,
    /// Sync stopped intentionally (e.g. shutdown).
    Terminated = 5,
};

} // namespace tesseract