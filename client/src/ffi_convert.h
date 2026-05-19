#pragma once
#include "tesseract/client.h"
#include "tesseract/image_pack.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"
#include "tesseract/types.h"
#include <memory>
#include <string>

namespace tesseract
{

inline Result from_ffi(const tesseract_ffi::OpResult& r)
{
    return {r.ok, std::string(r.message)};
}

inline PaginateResult from_ffi(const tesseract_ffi::PaginateResult& r)
{
    return {r.ok, std::string(r.message), r.reached_start, r.reached_end};
}

inline BackupProgress from_ffi(const tesseract_ffi::BackupProgress& p)
{
    BackupState state = BackupState::Unknown;
    switch (p.state)
    {
    case 1:
        state = BackupState::Disabled;
        break;
    case 2:
        state = BackupState::Enabled;
        break;
    case 3:
        state = BackupState::Downloading;
        break;
    case 4:
        state = BackupState::Creating;
        break;
    default:
        state = BackupState::Unknown;
        break;
    }
    return {
        .state = state,
        .imported_keys = p.imported_keys,
        .total_keys = p.total_keys,
    };
}

inline ImagePack from_ffi(const tesseract_ffi::ImagePackFfi& p)
{
    PackSourceKind kind = (std::string(p.source_kind) == "room")
                              ? PackSourceKind::Room
                              : PackSourceKind::User;
    return {
        .id = std::string(p.id),
        .display_name = std::string(p.display_name),
        .avatar_url = std::string(p.avatar_url),
        .attribution = std::string(p.attribution),
        // Mask to the defined bits (Sticker|Emoticon) so an out-of-range
        // value from a future Rust extension can't produce an unnamed enum
        // that silently matches no picker tab.
        .usage = static_cast<PackUsage>(p.usage_mask & 0x03),
        .source_kind = kind,
        .source_room = std::string(p.source_room),
        .source_state_key = std::string(p.source_state_key),
    };
}

inline ImagePackImage from_ffi(const tesseract_ffi::ImageEntryFfi& e)
{
    return {
        .pack_id = std::string(e.pack_id),
        .shortcode = std::string(e.shortcode),
        .url = std::string(e.url),
        .body = std::string(e.body),
        .info_json = std::string(e.info_json),
        .usage = static_cast<PackUsage>(e.usage_mask & 0x03),
        .favorite = e.favorite,
    };
}

inline RoomInfo from_ffi(const tesseract_ffi::RoomInfo& r)
{
    return {
        .id = std::string(r.id),
        .name = std::string(r.name),
        .topic = std::string(r.topic),
        .unread_count = r.unread_count,
        .is_direct = r.is_direct,
        .avatar_url = std::string(r.avatar_url),
        .last_message_body = std::string(r.last_message_body),
        .last_message_sender_name = std::string(r.last_message_sender_name),
        .last_message_kind = std::string(r.last_message_kind),
        .last_message_sticker_url = std::string(r.last_message_sticker_url),
        .last_activity_ts = r.last_activity_ts,
        .is_space = r.is_space,
        .is_favorite = r.is_favorite,
        .topic_html = std::string(r.topic_html),
    };
}

/// Copy every base-`Event` field (including `reactions`) from an FFI
/// `TimelineEvent` onto a freshly allocated subtype. Each branch in
/// `make_event` calls this before filling in its subtype-specific fields,
/// so adding a new base field (like `reactions`) only needs to be wired up
/// here once.
inline void assign_base(Event& ev, const tesseract_ffi::TimelineEvent& e)
{
    ev.event_id = std::string(e.event_id);
    ev.room_id = std::string(e.room_id);
    ev.sender = std::string(e.sender);
    ev.sender_name = std::string(e.sender_name);
    ev.sender_avatar_url = std::string(e.sender_avatar_url);
    ev.body = std::string(e.body);
    ev.formatted_body = std::string(e.formatted_body);
    ev.timestamp = e.timestamp;

    ev.reactions.clear();
    ev.reactions.reserve(e.reactions.size());
    for (const auto& r : e.reactions)
    {
        Reaction out{
            std::string(r.key),         r.count, r.reacted_by_me,
            std::string(r.source_json), {},
        };
        out.senders.reserve(r.senders.size());
        for (const auto& s : r.senders)
        {
            out.senders.emplace_back(std::string(s));
        }
        ev.reactions.push_back(std::move(out));
    }

    ev.read_receipts.clear();
    ev.read_receipts.reserve(e.read_receipts.size());
    for (const auto& rr : e.read_receipts)
    {
        ev.read_receipts.push_back(ReadReceipt{
            std::string(rr.user_id),
            std::string(rr.display_name),
            std::string(rr.avatar_url),
        });
    }

    ev.in_reply_to_id = std::string(e.in_reply_to_id);
    ev.in_reply_to_sender_name = std::string(e.in_reply_to_sender_name);
    ev.in_reply_to_body = std::string(e.in_reply_to_body);
    ev.is_edited = e.is_edited;
    ev.pending_state = std::string(e.pending_state);
    ev.pending_error = std::string(e.pending_error);
    ev.pending_recoverable = e.pending_recoverable;
    ev.pending_txn_id = std::string(e.pending_txn_id);
}

inline std::unique_ptr<Event> make_event(const tesseract_ffi::TimelineEvent& e)
{
    std::string msg_type(e.msg_type);

    if (msg_type == "m.text")
    {
        auto ev = std::make_unique<TextEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.notice")
    {
        auto ev = std::make_unique<NoticeEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.emote")
    {
        auto ev = std::make_unique<EmoteEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.image")
    {
        auto ev = std::make_unique<ImageEvent>();
        assign_base(*ev, e);
        ev->image_url = std::string(e.source_json);
        ev->thumbnail_url = std::string(e.image_thumbnail_json);
        ev->width = e.width;
        ev->height = e.height;
        ev->filename = std::string(e.image_filename);
        ev->blurhash = std::string(e.blurhash);
        ev->animated = e.image_animated;
        return ev;
    }

    if (msg_type == "m.sticker")
    {
        auto ev = std::make_unique<StickerEvent>();
        assign_base(*ev, e);
        ev->image_url = std::string(e.source_json);
        ev->thumbnail_url = std::string(e.image_thumbnail_json);
        ev->width = e.width;
        ev->height = e.height;
        ev->blurhash = std::string(e.blurhash);
        ev->info_json = std::string(e.sticker_info_json);
        ev->animated = e.image_animated;
        return ev;
    }

    if (msg_type == "m.redacted")
    {
        auto ev = std::make_unique<RedactedEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.file")
    {
        auto ev = std::make_unique<FileEvent>();
        assign_base(*ev, e);
        ev->file_url = std::string(e.file_json);
        ev->file_name = std::string(e.file_name);
        ev->file_size = e.file_size;
        return ev;
    }

    if (msg_type == "m.voice")
    {
        auto ev = std::make_unique<VoiceEvent>();
        assign_base(*ev, e);
        ev->audio_source = std::string(e.audio_source_json);
        ev->mime_type = std::string(e.audio_mime);
        ev->duration_ms = e.audio_duration_ms;
        ev->waveform.reserve(e.audio_waveform.size());
        for (uint16_t amp : e.audio_waveform)
        {
            ev->waveform.push_back(amp);
        }
        return ev;
    }

    if (msg_type == "m.video")
    {
        auto ev = std::make_unique<VideoEvent>();
        assign_base(*ev, e);
        ev->video_url = std::string(e.source_json);
        ev->thumbnail_url = std::string(e.video_thumbnail_json);
        ev->mime_type = std::string(e.video_mime);
        ev->width = e.width;
        ev->height = e.height;
        ev->duration_ms = e.video_duration_ms;
        ev->filename = std::string(e.image_filename);
        ev->autoplay = e.video_autoplay;
        ev->loop = e.video_loop;
        ev->no_audio = e.video_no_audio;
        ev->hide_controls = e.video_hide_controls;
        ev->gif = e.video_gif;
        ev->blurhash = std::string(e.blurhash);
        return ev;
    }

    if (msg_type == "virtual.date_divider")
    {
        auto ev = std::make_unique<DaySeparatorEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "virtual.read_marker")
    {
        auto ev = std::make_unique<ReadMarkerEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "virtual.timeline_start")
    {
        auto ev = std::make_unique<TimelineStartEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.location")
    {
        auto ev = std::make_unique<LocationEvent>();
        assign_base(*ev, e);
        ev->lat = e.location_lat;
        ev->lon = e.location_lon;
        ev->description = std::string(e.location_description);
        return ev;
    }

    // Fallback for unhandled message types
    auto ev = std::make_unique<UnhandledEvent>(msg_type);
    assign_base(*ev, e);
    return ev;
}

} // namespace tesseract