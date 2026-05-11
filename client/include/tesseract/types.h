#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract {

enum class EventType { Text, Image, File, Sticker, Unhandled };

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

struct FileEvent : public Event {
    std::string file_url;   // mxc:// URI
    std::string file_name;
    uint64_t    file_size = 0;

    FileEvent() { type = EventType::File; }
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