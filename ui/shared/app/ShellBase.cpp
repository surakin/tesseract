#include "app/ShellBase.h"
#include "tk/blurhash.h"
#include "tk/theme.h"
#include "views/html_spans.h"
#include <tesseract/paths.h>
#include <tesseract/settings.h>
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
    // Must be called on the UI thread — accesses tk_avatars_ and
    // media_fetches_in_flight_ without synchronization.
    if (r.avatar_url.empty() || tk_avatars_.count(r.avatar_url)) return;
    if (!media_fetches_in_flight_.insert(r.avatar_url).second) return;
    const std::string room_id = r.id;
    const std::string mxc     = r.avatar_url;
    run_async_([this, room_id, mxc]() {
        auto bytes = media_disk_cache_.load(mxc);
        if (bytes.empty()) {
            bytes = client_->fetch_avatar_bytes(room_id);
            if (!bytes.empty()) media_disk_cache_.store(mxc, bytes);
        }
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
        auto bytes = media_disk_cache_.load(mxc);
        if (bytes.empty()) {
            bytes = client_->fetch_media_bytes(mxc);
            if (!bytes.empty()) media_disk_cache_.store(mxc, bytes);
        }
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
        auto bytes = media_disk_cache_.load(url);
        if (bytes.empty()) {
            bytes = client_->fetch_source_bytes(url);
            if (!bytes.empty()) media_disk_cache_.store(url, bytes);
        }
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

void ShellBase::ensure_url_preview_(const std::string& url) {
    if (url.empty() || url_previews_.count(url)) return;
    if (!url_preview_in_flight_.insert(url).second) return;
    run_async_([this, url]() {
        auto preview = client_->get_url_preview(url);
        post_to_ui_([this, url, preview = std::move(preview)]() mutable {
            url_preview_in_flight_.erase(url);
            url_previews_.emplace(url, std::move(preview));
            if (!url_previews_.at(url).failed)
                on_url_preview_ready_(url, url_previews_.at(url));
        });
    });
}

void ShellBase::ensure_blurhash_image_(const std::string& event_id,
                                        const std::string& hash,
                                        int media_w, int media_h) {
    const std::string key = "blurhash::" + event_id;
    if (tk_images_.count(key) || !blurhash_attempted_.insert(key).second) return;
    constexpr int kMaxDim = 32;
    int kW = kMaxDim, kH = kMaxDim;
    if (media_w > 0 && media_h > 0) {
        if (media_w >= media_h) kH = std::max(1, kMaxDim * media_h / media_w);
        else                    kW = std::max(1, kMaxDim * media_w / media_h);
    }
    std::vector<uint8_t> rgba;
    if (!tk::decode_blurhash(hash, kW, kH, rgba)) return;
    cache_rgba_image_(key, kW, kH, std::move(rgba));
}

void ShellBase::ensure_row_media_(const Event& ev) {
    if (!media_disk_cache_pruned_) {
        media_disk_cache_pruned_ = true;
        run_async_([this]() { media_disk_cache_.prune(); });
    }
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

    // MSC2448: decode and cache BlurHash placeholder for media types.
    {
        std::string bh;
        int bw = 0, bh_dim = 0;
        if (ev.type == EventType::Image) {
            const auto& img = static_cast<const ImageEvent&>(ev);
            bh = img.blurhash; bw = static_cast<int>(img.width); bh_dim = static_cast<int>(img.height);
        } else if (ev.type == EventType::Sticker) {
            const auto& s = static_cast<const StickerEvent&>(ev);
            bh = s.blurhash; bw = static_cast<int>(s.width); bh_dim = static_cast<int>(s.height);
        } else if (ev.type == EventType::Video) {
            const auto& vid = static_cast<const VideoEvent&>(ev);
            bh = vid.blurhash; bw = static_cast<int>(vid.width); bh_dim = static_cast<int>(vid.height);
        }
        if (!bh.empty())
            ensure_blurhash_image_(ev.event_id, bh, bw, bh_dim);
    }

    if (ev.type == EventType::Text || ev.type == EventType::Unhandled) {
        std::string url;
        if (!ev.formatted_body.empty())
            url = views::first_url_from_html(ev.formatted_body);
        if (url.empty() && !ev.body.empty())
            url = views::first_url_from_plain(ev.body);
        if (!url.empty())
            ensure_url_preview_(url);
    }
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

void ShellBase::begin_focused_subscription_(const std::string& room_id,
                                             const std::string& event_id) {
    auto& state           = pagination_[room_id];
    state.is_focused      = true;
    state.focus_event_id  = event_id;
    state.fwd_in_flight   = false;
    state.reached_end     = false;
}

void ShellBase::clear_focused_state_(const std::string& room_id) {
    auto& state           = pagination_[room_id];
    state.is_focused      = false;
    state.focus_event_id.clear();
    state.reached_end     = false;
    state.fwd_in_flight   = false;
}

void ShellBase::request_forward_history_(const std::string& room_id) {
    auto& state = pagination_[room_id];
    if (state.fwd_in_flight || state.reached_end) return;
    if (!state.is_focused) return;
    state.fwd_in_flight = true;

    run_async_([this, room_id]() {
        auto res = client_->paginate_forward(room_id, kPaginationBatch);
        post_to_ui_([this, room_id, res]() {
            pagination_[room_id].fwd_in_flight = false;
            if (res.ok) {
                pagination_[room_id].reached_end = res.reached_end;
                if (res.reached_end)
                    return_to_live_(room_id);
            }
        });
    });
}

void ShellBase::return_to_live_(const std::string& room_id) {
    auto& state        = pagination_[room_id];
    state.is_focused   = false;
    state.focus_event_id.clear();
    state.reached_end  = false;
    state.fwd_in_flight = false;
    state.in_flight    = true;

    run_async_([this, room_id]() {
        client_->subscribe_room(room_id);
        auto pr = client_->paginate_back_with_status(room_id, kPaginationBatch);
        post_to_ui_([this, room_id, pr]() {
            pagination_[room_id].reached_start = pr.reached_start;
            pagination_[room_id].in_flight     = false;
        });
    });
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

void ShellBase::mark_room_read_(const std::string& room_id) {
    if (room_id.empty()) return;
    for (auto& r : rooms_) {
        if (r.id == room_id) { r.unread_count = 0; break; }
    }
    auto it = per_account_rooms_.find(my_user_id_);
    if (it != per_account_rooms_.end()) {
        for (auto& r : it->second) {
            if (r.id == room_id) { r.unread_count = 0; break; }
        }
    }
    on_rooms_updated_();
    run_async_([this, room_id]() {
        if (client_) client_->mark_room_as_read(room_id);
    });
}

static std::string format_typing_text(const std::vector<std::string>& names) {
    if (names.empty()) return {};
    if (names.size() == 1) return names[0] + " is typing\xe2\x80\xa6";
    if (names.size() == 2)
        return names[0] + " and " + names[1] + " are typing\xe2\x80\xa6";
    return names[0] + ", " + names[1] + ", and "
         + std::to_string(names.size() - 2) + " others are typing\xe2\x80\xa6";
}

void ShellBase::handle_typing_changed_ui_(std::string room_id,
                                           std::vector<std::string> names) {
    if (room_id != current_room_id_) return;
    typing_bar_visible_ = !names.empty();
    update_typing_bar_(format_typing_text(names), typing_bar_visible_);
}

void ShellBase::handle_compose_text_changed_(const std::string& text) {
    bool typing = !text.empty();
    if (typing == compose_typing_active_) return;
    compose_typing_active_ = typing;
    if (!current_room_id_.empty() && client_)
        client_->send_typing_notice(current_room_id_, typing);
}

void ShellBase::handle_compose_room_leaving_(const std::string& old_room_id) {
    if (!compose_typing_active_ || old_room_id.empty() || !client_) return;
    compose_typing_active_ = false;
    client_->send_typing_notice(old_room_id, false);
}

void ShellBase::apply_current_theme_() {
    auto& s = tesseract::Settings::instance();
    tk::ThemeMode mode =
        s.theme_pref == tesseract::Settings::ThemePreference::Dark   ? tk::ThemeMode::Dark  :
        s.theme_pref == tesseract::Settings::ThemePreference::Light  ? tk::ThemeMode::Light :
        os_color_scheme_();   // System → ask the OS
    apply_theme_ui_(mode == tk::ThemeMode::Dark ? tk::Theme::dark() : tk::Theme::light());
}

void ShellBase::set_theme_preference_(tesseract::Settings::ThemePreference pref) {
    tesseract::Settings::instance().theme_pref = pref;
    tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
    apply_current_theme_();
}

} // namespace tesseract
