#pragma once
#include "tesseract/client.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"
#include "tesseract/types.h"
#include <memory>
#include <string>

namespace tesseract {

inline Result from_ffi(const tesseract_ffi::OpResult& r) {
    return { r.ok, std::string(r.message) };
}

inline BackupProgress from_ffi(const tesseract_ffi::BackupProgress& p) {
    BackupState state;
    switch (p.state) {
        case 1:  state = BackupState::Disabled;    break;
        case 2:  state = BackupState::Enabled;     break;
        case 3:  state = BackupState::Downloading; break;
        case 4:  state = BackupState::Creating;    break;
        default: state = BackupState::Unknown;     break;
    }
    return {
        .state         = state,
        .imported_keys = p.imported_keys,
        .total_keys    = p.total_keys,
    };
}

inline RoomInfo from_ffi(const tesseract_ffi::RoomInfo& r) {
    return {
        .id                = std::string(r.id),
        .name              = std::string(r.name),
        .topic             = std::string(r.topic),
        .unread_count      = r.unread_count,
        .is_direct         = r.is_direct,
        .avatar_url        = std::string(r.avatar_url),
        .last_message_body = std::string(r.last_message_body),
        .last_activity_ts  = r.last_activity_ts,
        .is_space          = r.is_space,
    };
}

/// Copy every base-`Event` field (including `reactions`) from an FFI
/// `TimelineEvent` onto a freshly allocated subtype. Each branch in
/// `make_event` calls this before filling in its subtype-specific fields,
/// so adding a new base field (like `reactions`) only needs to be wired up
/// here once.
inline void assign_base(Event& ev, const tesseract_ffi::TimelineEvent& e) {
    ev.event_id          = std::string(e.event_id);
    ev.room_id           = std::string(e.room_id);
    ev.sender            = std::string(e.sender);
    ev.sender_name       = std::string(e.sender_name);
    ev.sender_avatar_url = std::string(e.sender_avatar_url);
    ev.body              = std::string(e.body);
    ev.timestamp         = e.timestamp;

    ev.reactions.clear();
    ev.reactions.reserve(e.reactions.size());
    for (const auto& r : e.reactions) {
        Reaction out{
            std::string(r.key),
            r.count,
            r.reacted_by_me,
            std::string(r.source_json),
            {},
        };
        out.senders.reserve(r.senders.size());
        for (const auto& s : r.senders)
            out.senders.emplace_back(std::string(s));
        ev.reactions.push_back(std::move(out));
    }
}

inline std::unique_ptr<Event> make_event(const tesseract_ffi::TimelineEvent& e) {
    std::string msg_type(e.msg_type);

    if (msg_type == "m.text") {
        auto ev = std::make_unique<TextEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.image") {
        auto ev = std::make_unique<ImageEvent>();
        assign_base(*ev, e);
        ev->image_url = std::string(e.source_json);
        ev->width     = e.width;
        ev->height    = e.height;
        ev->filename  = std::string(e.image_filename);
        return ev;
    }

    if (msg_type == "m.sticker") {
        auto ev = std::make_unique<StickerEvent>();
        assign_base(*ev, e);
        ev->image_url = std::string(e.source_json);
        ev->width     = e.width;
        ev->height    = e.height;
        return ev;
    }

    if (msg_type == "m.file") {
        auto ev = std::make_unique<FileEvent>();
        assign_base(*ev, e);
        ev->file_url  = std::string(e.file_json);
        ev->file_name = std::string(e.file_name);
        ev->file_size = e.file_size;
        return ev;
    }

    // Fallback for unhandled message types
    auto ev = std::make_unique<UnhandledEvent>(msg_type);
    assign_base(*ev, e);
    return ev;
}

} // namespace tesseract