#pragma once
#include "tesseract/client.h"
#include "tesseract/image_pack.h"
#include "tesseract_sdk_bridge_cxx/bridge.h"
#include "tesseract/types.h"
#include <memory>
#include <string>
#include <vector>

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

inline Client::QrGrantBitmap from_ffi(const tesseract_ffi::QrGrantBitmap& r)
{
    return {
        .ok      = r.ok,
        .message = std::string(r.message),
        .pixels  = std::vector<uint8_t>(r.pixels.begin(), r.pixels.end()),
        .side    = r.side,
    };
}

inline Client::QrGrantAuth from_ffi(const tesseract_ffi::QrGrantAuth& r)
{
    return {
        .ok               = r.ok,
        .message          = std::string(r.message),
        .verification_uri = std::string(r.verification_uri),
    };
}

inline SearchIndexStats from_ffi(const tesseract_ffi::SearchIndexStats& s)
{
    return {s.message_count, s.room_count, s.oldest_ts_ms, s.backfill_done, 0};
}

inline MediaBackoffEntry from_ffi(const tesseract_ffi::MediaBackoffEntry& e)
{
    return {std::string(e.url), e.attempts, e.deadline_secs};
}

inline RtcParticipantInfo from_ffi(const tesseract_ffi::RtcParticipantInfo& p)
{
    return {
        std::string(p.participant_id),
        std::string(p.user_id),
        std::string(p.device_id),
        p.is_audio_muted,
        p.is_video_muted,
        p.is_screen_sharing,
    };
}

inline RoomSummaryBackoffEntry from_ffi(const tesseract_ffi::RoomSummaryBackoffEntry& e)
{
    return {std::string(e.room_id), e.attempts, e.deadline_secs};
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

inline tesseract::ThreadInfo from_ffi(const tesseract_ffi::ThreadInfo& t)
{
    tesseract::ThreadInfo out;
    out.root_event_id = std::string(t.root_event_id);
    out.root_sender_name = std::string(t.root_sender_name);
    out.root_body = std::string(t.root_body);
    out.root_timestamp = t.root_timestamp;
    out.latest_event_id = std::string(t.latest_event_id);
    out.latest_sender_name = std::string(t.latest_sender_name);
    out.latest_body = std::string(t.latest_body);
    out.latest_timestamp = t.latest_timestamp;
    out.num_replies = t.num_replies;
    return out;
}

inline PinnedEvent from_ffi(const tesseract_ffi::PinnedEvent& p)
{
    return PinnedEvent{
        std::string(p.event_id),
        std::string(p.sender_name),
        std::string(p.body_preview),
        p.timestamp,
    };
}

inline RoomInfo from_ffi(const tesseract_ffi::RoomInfo& r)
{
    RoomInfo out{
        .id = std::string(r.id),
        .name = std::string(r.name),
        .topic = std::string(r.topic),
        .notification_count = r.notification_count,
        .highlight_count    = r.highlight_count,
        .unread_count       = r.unread_count,
        .muted              = r.muted,
        .is_direct = r.is_direct,
        .avatar_url = std::string(r.avatar_url),
        .dm_avatar_url = std::string(r.dm_avatar_url),
        .dm_counterpart_user_id = std::string(r.dm_counterpart_user_id),
        .last_message_body = std::string(r.last_message_body),
        .last_message_sender_name = std::string(r.last_message_sender_name),
        .last_message_kind = std::string(r.last_message_kind),
        .last_message_sticker_url = std::string(r.last_message_sticker_url),
        .last_message_thumbnail_url = std::string(r.last_message_thumbnail_url),
        .last_activity_ts = r.last_activity_ts,
        .is_space = r.is_space,
        .is_favorite = r.is_favorite,
        .is_low_priority = r.is_low_priority,
        .topic_html = std::string(r.topic_html),
        .is_encrypted = r.is_encrypted,
        .has_active_call = r.has_active_call,
        .is_bridged = r.is_bridged,
        .history_visibility = std::string(r.history_visibility),
    };
    out.pinned_events.reserve(r.pinned_events.size());
    for (const auto& p : r.pinned_events)
    {
        out.pinned_events.push_back(from_ffi(p));
    }
    out.canonical_alias = std::string(r.canonical_alias);
    return out;
}

inline InviteInfo from_ffi(const tesseract_ffi::InviteInfo& i)
{
    return {
        .room_id               = std::string(i.room_id),
        .room_name             = std::string(i.room_name),
        .room_avatar_url       = std::string(i.room_avatar_url),
        .room_topic            = std::string(i.room_topic),
        .is_direct             = i.is_direct,
        .inviter_user_id       = std::string(i.inviter_user_id),
        .inviter_display_name  = std::string(i.inviter_display_name),
        .inviter_avatar_url    = std::string(i.inviter_avatar_url),
        .invited_at_ts         = i.invited_at_ts,
    };
}

/// Build a MediaSource from the split url/encrypted_json pair carried over
/// the FFI.  Returns nullptr when url is empty (= absent source).
inline MediaSourceRef make_source(rust::Str url,
                                                       rust::Str enc_json)
{
    if (url.empty())
        return nullptr;
    if (enc_json.empty())
        return MediaSource::plain(std::string(url));
    return MediaSource::encrypted(std::string(url), std::string(enc_json));
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
        Reaction out;
        out.key = std::string(r.key);
        out.count = r.count;
        out.reacted_by_me = r.reacted_by_me;
        if (!r.source_url.empty())
            out.source = MediaSource::plain(std::string(r.source_url));
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
    ev.in_reply_to_image_url = std::string(e.in_reply_to_image_url);
    ev.in_reply_to_image_encrypted_json = std::string(e.in_reply_to_image_encrypted_json);
    ev.is_edited = e.is_edited;
    ev.thread_root_id = std::string(e.thread_root_id);
    ev.is_thread_root = e.is_thread_root;
    ev.thread_reply_count = e.thread_reply_count;
    ev.thread_latest_sender_name = std::string(e.thread_latest_sender_name);
    ev.thread_latest_body = std::string(e.thread_latest_body);
    ev.thread_latest_ts = e.thread_latest_ts;
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
        ev->source    = make_source(e.source_url, e.source_encrypted_json);
        ev->thumbnail = make_source(e.image_thumbnail_url,
                                    e.image_thumbnail_encrypted_json);
        ev->width    = e.width;
        ev->height   = e.height;
        ev->filename = std::string(e.image_filename);
        ev->blurhash = std::string(e.blurhash);
        ev->animated = e.image_animated;
        return ev;
    }

    if (msg_type == "m.sticker")
    {
        auto ev = std::make_unique<StickerEvent>();
        assign_base(*ev, e);
        ev->source    = make_source(e.source_url, e.source_encrypted_json);
        ev->thumbnail = make_source(e.image_thumbnail_url,
                                    e.image_thumbnail_encrypted_json);
        ev->width    = e.width;
        ev->height   = e.height;
        ev->blurhash  = std::string(e.blurhash);
        ev->info_json = std::string(e.sticker_info_json);
        ev->animated  = e.image_animated;
        return ev;
    }

    if (msg_type == "m.redacted")
    {
        auto ev = std::make_unique<RedactedEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.utd")
    {
        auto ev = std::make_unique<UtdEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "m.file")
    {
        auto ev = std::make_unique<FileEvent>();
        assign_base(*ev, e);
        ev->source    = make_source(e.file_url, e.file_encrypted_json);
        ev->filename  = std::string(e.file_filename);
        ev->file_size = e.file_size;
        return ev;
    }

    if (msg_type == "m.audio")
    {
        auto ev = std::make_unique<AudioEvent>();
        assign_base(*ev, e);
        ev->source     = make_source(e.audio_url, e.audio_encrypted_json);
        ev->mime_type  = std::string(e.audio_mime);
        ev->duration_ms = e.audio_duration_ms;
        ev->filename   = std::string(e.file_name);
        ev->file_size  = e.file_size;
        return ev;
    }

    if (msg_type == "m.voice")
    {
        auto ev = std::make_unique<VoiceEvent>();
        assign_base(*ev, e);
        ev->source     = make_source(e.audio_url, e.audio_encrypted_json);
        ev->mime_type  = std::string(e.audio_mime);
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
        ev->source    = make_source(e.source_url, e.source_encrypted_json);
        ev->thumbnail = make_source(e.video_thumbnail_url,
                                    e.video_thumbnail_encrypted_json);
        ev->mime_type    = std::string(e.video_mime);
        ev->width        = e.width;
        ev->height       = e.height;
        ev->duration_ms  = e.video_duration_ms;
        ev->filename     = std::string(e.image_filename);
        ev->autoplay     = e.video_autoplay;
        ev->loop         = e.video_loop;
        ev->no_audio     = e.video_no_audio;
        ev->hide_controls = e.video_hide_controls;
        ev->gif          = e.video_gif;
        ev->blurhash     = std::string(e.blurhash);
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

    if (msg_type == "m.room.pinned_events")
    {
        auto ev = std::make_unique<PinnedStateEvent>();
        assign_base(*ev, e);
        return ev;
    }

    if (msg_type == "org.matrix.msc4075.rtc.notification")
    {
        auto ev = std::make_unique<CallNotificationEvent>();
        assign_base(*ev, e);
        return ev;
    }

    // Fallback for unhandled message types
    auto ev = std::make_unique<UnhandledEvent>(msg_type);
    assign_base(*ev, e);
    return ev;
}

template <typename T, typename U>
std::vector<T> ffi_vec(const rust::Vec<U>& src)
{
    std::vector<T> out;
    out.reserve(src.size());
    for (const auto& x : src)
        out.push_back(from_ffi(x));
    return out;
}

} // namespace tesseract