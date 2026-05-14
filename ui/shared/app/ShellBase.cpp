#include "app/ShellBase.h"
#include <tesseract/visual.h>
#include <thread>

namespace tesseract {

void ShellBase::run_async_(std::function<void()> fn) {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(workers_mu_);
        ++workers_in_flight_;
    }
    std::thread([this, fn = std::move(fn)]() mutable {
        if (!shutting_down_.load(std::memory_order_acquire))
            fn();
        std::lock_guard<std::mutex> lk(workers_mu_);
        if (--workers_in_flight_ == 0) workers_cv_.notify_all();
    }).detach();
}

void ShellBase::ensure_room_avatar_(const RoomInfo& r) {
    if (r.avatar_url.empty() || tk_avatars_.count(r.avatar_url)) return;
    if (!media_fetches_in_flight_.insert(r.avatar_url).second) return;
    const std::string room_id = r.id;
    const std::string mxc     = r.avatar_url;
    run_async_([this, room_id, mxc]() {
        auto bytes = client_->fetch_avatar_bytes(room_id);
        post_to_ui_([this, mxc, bytes = std::move(bytes)]() mutable {
            media_fetches_in_flight_.erase(mxc);
            on_media_bytes_ready_(mxc, MediaKind::RoomAvatar, std::move(bytes));
        });
    });
}

void ShellBase::ensure_user_avatar_(const std::string& mxc) {
    if (mxc.empty() || tk_avatars_.count(mxc)) return;
    if (!media_fetches_in_flight_.insert(mxc).second) return;
    run_async_([this, mxc]() {
        auto bytes = client_->fetch_media_bytes(mxc);
        post_to_ui_([this, mxc, bytes = std::move(bytes)]() mutable {
            media_fetches_in_flight_.erase(mxc);
            on_media_bytes_ready_(mxc, MediaKind::UserAvatar, std::move(bytes));
        });
    });
}

void ShellBase::ensure_media_image_(const std::string& url,
                                     int /*max_w*/, int /*max_h*/) {
    if (url.empty() || tk_images_.count(url) || anim_cache_.has(url)) return;
    if (!media_fetches_in_flight_.insert(url).second) return;
    run_async_([this, url]() {
        auto bytes = client_->fetch_source_bytes(url);
        post_to_ui_([this, url, bytes = std::move(bytes)]() mutable {
            media_fetches_in_flight_.erase(url);
            on_media_bytes_ready_(url, MediaKind::MediaImage, std::move(bytes));
        });
    });
}

void ShellBase::ensure_reply_details_(const std::string& event_id) {
    if (event_id.empty() || current_room_id_.empty()) return;
    if (!reply_details_requested_.insert(event_id).second) return;
    client_->fetch_reply_details(current_room_id_, event_id);
}

void ShellBase::ensure_row_media_(const Event& ev) {
    ensure_user_avatar_(ev.sender_avatar_url);
    for (const auto& rr : ev.read_receipts)
        ensure_user_avatar_(rr.avatar_url);

    if (ev.type == EventType::Image) {
        const auto& img = static_cast<const ImageEvent&>(ev);
        ensure_media_image_(img.image_url,
                             visual::kMaxInlineImageWidth,
                             visual::kMaxInlineImageHeight);
    } else if (ev.type == EventType::Sticker) {
        const auto& s = static_cast<const StickerEvent&>(ev);
        ensure_media_image_(s.image_url, visual::kStickerSize, visual::kStickerSize);
    } else if (ev.type == EventType::Voice) {
        const auto& v = static_cast<const VoiceEvent&>(ev);
        if (!v.audio_source.empty() &&
            voice_prefetched_.insert(v.audio_source).second)
        {
            run_async_([this, src = v.audio_source]() {
                (void)client_->fetch_source_bytes(src);
            });
        }
    } else if (ev.type == EventType::Video) {
        const auto& vid = static_cast<const VideoEvent&>(ev);
        if (!vid.thumbnail_url.empty())
            ensure_media_image_(vid.thumbnail_url,
                                 visual::kMaxInlineImageWidth,
                                 visual::kMaxInlineImageHeight);
        if (vid.thumbnail_url.empty() && !vid.video_url.empty() &&
            video_thumb_in_flight_.insert(ev.event_id).second)
        {
            generate_video_thumbnail_(ev.event_id, vid.video_url);
        }
    }
    for (const auto& r : ev.reactions)
        if (!r.source_json.empty())
            ensure_media_image_(r.source_json, 20, 20);
}

void ShellBase::push_rooms_(std::string user_id, std::vector<RoomInfo> rooms) {
    per_account_rooms_[user_id] = rooms;
    if (user_id != my_user_id_) return;
    rooms_ = std::move(rooms);
    on_rooms_updated_();
}

void ShellBase::push_paginate_result_(std::string room_id, bool reached_start) {
    auto& state = pagination_[room_id];
    state.in_flight     = false;
    state.reached_start = reached_start;
}

void ShellBase::push_room_list_state_(RoomListState state) {
    last_room_list_state_ = state;
}

void ShellBase::maybe_send_read_receipt_(const std::string& room_id,
                                          const std::string& event_id) {
    if (room_id.empty() || event_id.empty()) return;
    auto& last = last_sent_receipt_[room_id];
    if (last == event_id) return;
    last = event_id;
    run_async_([this, room_id, event_id]() {
        if (client_) client_->send_read_receipt(room_id, event_id);
    });
}

} // namespace tesseract
