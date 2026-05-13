#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract {

enum class EventType { Text, Image, File, Sticker, Voice, Redacted, Unhandled };

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
    virtual ~Event() = default;
};

struct TextEvent : public Event {
    TextEvent() { type = EventType::Text; }
};

struct ImageEvent : public Event {
    std::string image_url;   // mxc:// URI
    uint64_t    width  = 0;
    uint64_t    height = 0;
    /// Non-empty only when the sender supplied an MSC2530 `filename` field.
    /// When set, `body` is a user caption and should be displayed below the image.
    std::string filename;

    ImageEvent() { type = EventType::Image; }
};

struct StickerEvent : public Event {
    std::string image_url;   // mxc:// URI
    uint64_t    width  = 0;
    uint64_t    height = 0;

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

/// Fallback for message types we don't handle yet (reactions, polls, etc.)
struct UnhandledEvent : public Event {
    std::string msg_type;

    UnhandledEvent() { type = EventType::Unhandled; }
    explicit UnhandledEvent(const std::string& t) : msg_type(t) { type = EventType::Unhandled; }
};

struct RoomInfo {
    std::string id;
    std::string name;
    std::string topic;
    uint64_t    unread_count       = 0;
    bool        is_direct          = false;
    std::string avatar_url;
    std::string last_message_body;
    uint64_t    last_activity_ts   = 0;
    bool        is_space           = false;
    bool        is_favorite        = false;
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

} // namespace tesseract