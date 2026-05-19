#include "app/ShellBase.h"
#include "app/RoomWindowBase.h"
#include "tk/blurhash.h"
#include "tk/theme.h"
#include "views/html_spans.h"
#include "views/map_tiles.h"
#include <tesseract/paths.h>
#include <tesseract/prefs.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <fstream>
#include <thread>

namespace tesseract
{

void ShellBase::run_async_(std::function<void()> fn)
{
    if (shutting_down_.load(std::memory_order_acquire))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(workers_mu_);
        ++workers_in_flight_;
    }
    std::thread(
        [this, fn = std::move(fn)]() mutable
        {
            if (!shutting_down_.load(std::memory_order_acquire))
            {
                fn();
            }
            std::lock_guard<std::mutex> lk(workers_mu_);
            if (--workers_in_flight_ == 0)
            {
                workers_cv_.notify_all();
            }
        })
        .detach();
}

void ShellBase::ensure_room_avatar_(const RoomInfo& r)
{
    // Must be called on the UI thread — accesses tk_avatars_ and
    // media_fetches_in_flight_ without synchronization.
    if (r.avatar_url.empty() || tk_avatars_.count(r.avatar_url))
    {
        return;
    }
    if (!media_fetches_in_flight_.insert(r.avatar_url).second)
    {
        return;
    }
    const std::string room_id = r.id;
    const std::string mxc = r.avatar_url;
    run_async_(
        [this, room_id, mxc]()
        {
            auto bytes = media_disk_cache_.load(mxc);
            if (bytes.empty())
            {
                bytes = client_->fetch_avatar_bytes(room_id);
                if (!bytes.empty())
                {
                    media_disk_cache_.store(mxc, bytes);
                }
            }
            post_to_ui_(
                [this, mxc, bytes = std::move(bytes)]() mutable
                {
                    media_fetches_in_flight_.erase(mxc);
                    on_media_bytes_ready_(mxc, MediaKind::RoomAvatar,
                                          std::move(bytes));
                });
        });
}

void ShellBase::ensure_user_avatar_(const std::string& mxc)
{
    if (mxc.empty() || tk_avatars_.count(mxc))
    {
        return;
    }
    if (!media_fetches_in_flight_.insert(mxc).second)
    {
        return;
    }
    run_async_(
        [this, mxc]()
        {
            auto bytes = media_disk_cache_.load(mxc);
            if (bytes.empty())
            {
                bytes = client_->fetch_media_bytes(mxc);
                if (!bytes.empty())
                {
                    media_disk_cache_.store(mxc, bytes);
                }
            }
            post_to_ui_(
                [this, mxc, bytes = std::move(bytes)]() mutable
                {
                    media_fetches_in_flight_.erase(mxc);
                    on_media_bytes_ready_(mxc, MediaKind::UserAvatar,
                                          std::move(bytes));
                });
        });
}

void ShellBase::ensure_media_image_(const std::string& url, int /*max_w*/,
                                    int /*max_h*/)
{
    if (url.empty() || tk_images_.count(url) || anim_cache_.has(url))
    {
        return;
    }
    if (!media_fetches_in_flight_.insert(url).second)
    {
        return;
    }
    run_async_(
        [this, url]()
        {
            auto bytes = media_disk_cache_.load(url);
            if (bytes.empty())
            {
                bytes = client_->fetch_source_bytes(url);
                if (!bytes.empty())
                {
                    media_disk_cache_.store(url, bytes);
                }
            }
            post_to_ui_(
                [this, url, bytes = std::move(bytes)]() mutable
                {
                    media_fetches_in_flight_.erase(url);
                    on_media_bytes_ready_(url, MediaKind::MediaImage,
                                          std::move(bytes));
                });
        });
}

void ShellBase::ensure_picker_image_(const std::string& url, bool is_sticker)
{
    if (url.empty() || tk_images_.count(url) || anim_cache_.has(url))
    {
        return;
    }
    auto& inflight =
        is_sticker ? sticker_fetches_in_flight_ : emoji_fetches_in_flight_;
    if (!inflight.insert(url).second)
    {
        return;
    }
    run_async_(
        [this, url, is_sticker]()
        {
            auto bytes = media_disk_cache_.load(url);
            if (bytes.empty())
            {
                bytes = client_->fetch_source_bytes(url);
                if (!bytes.empty())
                {
                    media_disk_cache_.store(url, bytes);
                }
            }
            if (bytes.empty())
            {
                post_to_ui_(
                    [this, url, is_sticker]()
                    {
                        (is_sticker ? sticker_fetches_in_flight_
                                    : emoji_fetches_in_flight_)
                            .erase(url);
                    });
                return;
            }
            // Decode OFF the UI thread. Picker cells are bounded; reuse the
            // inline-image bound so picker bitmaps are reusable by the
            // message list (same shared tk_images_ key = the mxc url).
            // DecodedImage is move-only (holds unique_ptr<tk::Image>); wrap it
            // in a shared_ptr so the post_to_ui_ lambda is copy-constructible
            // (post_to_ui_ takes std::function, which requires that).
            auto d = std::make_shared<DecodedImage>(
                decode_image_(bytes, visual::kMaxInlineImageWidth,
                              visual::kMaxInlineImageHeight));
            post_to_ui_(
                [this, url, is_sticker, d]() mutable
                {
                    finalize_picker_image_(url, is_sticker, std::move(*d));
                });
        });
}

void ShellBase::finalize_picker_image_(std::string url, bool is_sticker,
                                       DecodedImage d)
{
    (is_sticker ? sticker_fetches_in_flight_ : emoji_fetches_in_flight_)
        .erase(url);
    if (tk_images_.count(url) || anim_cache_.has(url))
    {
        return;
    }
    if (!d.frames.empty())
    {
        anim_cache_.store(url, std::move(d.frames), std::move(d.delays_ms),
                          monotonic_ms_());
        start_anim_tick_();
    }
    else if (d.still)
    {
        tk_images_.emplace(std::move(url), std::move(d.still));
    }
    else
    {
        return; // decode failed — leave uncached so a later paint retries
    }
    repaint_pickers_();
}

void ShellBase::ensure_tile_async(int z, int x, int y)
{
    const std::string key = tesseract::views::tile_cache_key({z, x, y});
    if (tk_images_.count(key) || tile_fetch_failed_.count(key))
    {
        return;
    }
    if (!tile_fetches_in_flight_.insert(key).second)
    {
        return;
    }

    const std::string url = tesseract::views::tile_url({z, x, y});
    const std::filesystem::path disk_path =
        tesseract::cache_dir() / "tiles" / std::to_string(z) /
        std::to_string(x) / (std::to_string(y) + ".png");

    run_async_(
        [this, key, url, disk_path]()
        {
            std::vector<uint8_t> bytes;
            // Check disk cache first.
            if (std::filesystem::exists(disk_path))
            {
                std::ifstream f(disk_path, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(f), {});
            }
            // Fetch from network if not on disk.
            if (bytes.empty())
            {
                bytes = client_->fetch_url_bytes(url);
                if (!bytes.empty())
                {
                    std::error_code ec;
                    std::filesystem::create_directories(disk_path.parent_path(),
                                                        ec);
                    if (!ec)
                    {
                        std::ofstream f(disk_path, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()));
                    }
                }
            }
            post_to_ui_(
                [this, key, bytes = std::move(bytes)]() mutable
                {
                    tile_fetches_in_flight_.erase(key);
                    if (bytes.empty())
                    {
                        tile_fetch_failed_.insert(key);
                        return;
                    }
                    on_media_bytes_ready_(key, MediaKind::Tile,
                                          std::move(bytes));
                });
        });
}

void ShellBase::ensure_reply_details_(const std::string& event_id)
{
    if (event_id.empty() || current_room_id_.empty())
    {
        return;
    }
    if (!reply_details_requested_.insert(event_id).second)
    {
        return;
    }
    client_->fetch_reply_details(current_room_id_, event_id);
}

void ShellBase::ensure_url_preview_(const std::string& url)
{
    if (url.empty() || url_previews_.count(url))
    {
        return;
    }
    if (!url_preview_in_flight_.insert(url).second)
    {
        return;
    }
    run_async_(
        [this, url]()
        {
            auto preview = client_->get_url_preview(url);
            post_to_ui_(
                [this, url, preview = std::move(preview)]() mutable
                {
                    url_preview_in_flight_.erase(url);
                    url_previews_.emplace(url, std::move(preview));
                    if (!url_previews_.at(url).failed)
                    {
                        on_url_preview_ready_(url, url_previews_.at(url));
                    }
                    else
                    {
                        on_url_preview_failed_(url);
                    }
                });
        });
}

void ShellBase::ensure_blurhash_image_(const std::string& event_id,
                                       const std::string& hash, int media_w,
                                       int media_h)
{
    const std::string key = "blurhash::" + event_id;
    if (tk_images_.count(key) || !blurhash_attempted_.insert(key).second)
    {
        return;
    }
    constexpr int kMaxDim = 32;
    int kW = kMaxDim, kH = kMaxDim;
    if (media_w > 0 && media_h > 0)
    {
        if (media_w >= media_h)
        {
            kH = std::max(1, kMaxDim * media_h / media_w);
        }
        else
        {
            kW = std::max(1, kMaxDim * media_w / media_h);
        }
    }
    std::vector<uint8_t> rgba;
    if (!tk::decode_blurhash(hash, kW, kH, rgba))
    {
        return;
    }
    cache_rgba_image_(key, kW, kH, std::move(rgba));
}

void ShellBase::ensure_row_media_(const Event& ev)
{
    if (!media_disk_cache_pruned_)
    {
        media_disk_cache_pruned_ = true;
        run_async_(
            [this]()
            {
                media_disk_cache_.prune();
            });
    }
    ensure_user_avatar_(ev.sender_avatar_url);
    for (const auto& rr : ev.read_receipts)
    {
        ensure_user_avatar_(rr.avatar_url);
    }

    if (ev.type == EventType::Image)
    {
        const auto& img = static_cast<const ImageEvent&>(ev);
        ensure_media_image_(img.image_url, visual::kMaxInlineImageWidth,
                            visual::kMaxInlineImageHeight);
    }
    else if (ev.type == EventType::Sticker)
    {
        const auto& s = static_cast<const StickerEvent&>(ev);
        ensure_media_image_(s.image_url, visual::kStickerSize,
                            visual::kStickerSize);
    }
    else if (ev.type == EventType::Voice)
    {
        const auto& v = static_cast<const VoiceEvent&>(ev);
        if (!v.audio_source.empty() &&
            voice_prefetched_.insert(v.audio_source).second)
        {
            run_async_(
                [this, src = v.audio_source]()
                {
                    (void)client_->fetch_source_bytes(src);
                });
        }
    }
    else if (ev.type == EventType::Video)
    {
        const auto& vid = static_cast<const VideoEvent&>(ev);
        if (!vid.thumbnail_url.empty())
        {
            ensure_media_image_(vid.thumbnail_url, visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight);
        }
        if (vid.thumbnail_url.empty() && !vid.video_url.empty() &&
            video_thumb_in_flight_.insert(ev.event_id).second)
        {
            generate_video_thumbnail_(ev.event_id, vid.video_url);
        }
    }
    for (const auto& r : ev.reactions)
    {
        if (!r.source_json.empty())
        {
            ensure_media_image_(r.source_json, 20, 20);
        }
    }

    // MSC2448: decode and cache BlurHash placeholder for media types.
    {
        std::string bh;
        int bw = 0, bh_dim = 0;
        if (ev.type == EventType::Image)
        {
            const auto& img = static_cast<const ImageEvent&>(ev);
            bh = img.blurhash;
            bw = static_cast<int>(img.width);
            bh_dim = static_cast<int>(img.height);
        }
        else if (ev.type == EventType::Sticker)
        {
            const auto& s = static_cast<const StickerEvent&>(ev);
            bh = s.blurhash;
            bw = static_cast<int>(s.width);
            bh_dim = static_cast<int>(s.height);
        }
        else if (ev.type == EventType::Video)
        {
            const auto& vid = static_cast<const VideoEvent&>(ev);
            bh = vid.blurhash;
            bw = static_cast<int>(vid.width);
            bh_dim = static_cast<int>(vid.height);
        }
        if (!bh.empty())
        {
            ensure_blurhash_image_(ev.event_id, bh, bw, bh_dim);
        }
    }

    if (ev.type == EventType::Text || ev.type == EventType::Unhandled)
    {
        std::string url;
        if (!ev.formatted_body.empty())
        {
            url = views::first_url_from_html(ev.formatted_body);
        }
        if (url.empty() && !ev.body.empty())
        {
            url = views::first_url_from_plain(ev.body);
        }
        if (!url.empty())
        {
            ensure_url_preview_(url);
        }
    }
}

std::vector<views::MessageRowData>
ShellBase::build_rows_(const std::vector<std::unique_ptr<Event>>& snapshot)
{
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (const auto& ev : snapshot)
    {
        if (!ev)
        {
            continue;
        }
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        rows.push_back(views::make_row_data(*ev, my_user_id_));
    }
    return rows;
}

std::vector<views::MessageRowData>
ShellBase::build_rows_(const std::vector<Event*>& snapshot)
{
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (auto* ev : snapshot)
    {
        if (!ev)
        {
            continue;
        }
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        rows.push_back(views::make_row_data(*ev, my_user_id_));
    }
    return rows;
}

void ShellBase::dispatch_timeline_reset_secondary_(
    const std::string& room_id,
    const std::vector<std::unique_ptr<Event>>& snapshot)
{
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_timeline_reset(
                                           build_rows_(snapshot));
                                   });
}

void ShellBase::dispatch_message_inserted_secondary_(const std::string& room_id,
                                                     std::size_t index,
                                                     const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty())
            {
                ensure_reply_details_(ev.event_id);
            }
            w->on_message_inserted(index,
                                   views::make_row_data(ev, my_user_id_));
        });
}

void ShellBase::dispatch_message_updated_secondary_(const std::string& room_id,
                                                    std::size_t index,
                                                    const Event& ev)
{
    dispatch_to_secondary_windows_(
        room_id,
        [&](RoomWindowBase* w)
        {
            prep_row_media_(ev);
            if (!ev.in_reply_to_id.empty())
            {
                ensure_reply_details_(ev.event_id);
            }
            w->on_message_updated(index, views::make_row_data(ev, my_user_id_));
        });
}

void ShellBase::dispatch_message_removed_secondary_(const std::string& room_id,
                                                    std::size_t index)
{
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_message_removed(index);
                                   });
}

void ShellBase::update_secondary_room_infos_()
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == rid)
            {
                w->on_room_info_updated(r);
                break;
            }
        }
    }
}

void ShellBase::push_rooms_(std::string user_id, std::vector<RoomInfo> rooms)
{
    per_account_rooms_[user_id] = rooms;
    if (user_id != my_user_id_)
    {
        return;
    }
    rooms_ = std::move(rooms);
    on_rooms_updated_();
}

void ShellBase::push_paginate_result_(std::string room_id, bool reached_start)
{
    auto& state = pagination_[room_id];
    state.in_flight = false;
    state.reached_start = reached_start;
}

void ShellBase::begin_focused_subscription_(const std::string& room_id,
                                            const std::string& event_id)
{
    auto& state = pagination_[room_id];
    state.is_focused = true;
    state.focus_event_id = event_id;
    state.fwd_in_flight = false;
    state.reached_end = false;
}

void ShellBase::clear_focused_state_(const std::string& room_id)
{
    auto& state = pagination_[room_id];
    state.is_focused = false;
    state.focus_event_id.clear();
    state.reached_end = false;
    state.fwd_in_flight = false;
}

void ShellBase::request_forward_history_(const std::string& room_id)
{
    auto& state = pagination_[room_id];
    if (state.fwd_in_flight || state.reached_end)
    {
        return;
    }
    if (!state.is_focused)
    {
        return;
    }
    state.fwd_in_flight = true;

    run_async_(
        [this, room_id]()
        {
            auto res = client_->paginate_forward(room_id, kPaginationBatch);
            post_to_ui_(
                [this, room_id, res]()
                {
                    pagination_[room_id].fwd_in_flight = false;
                    if (res.ok)
                    {
                        pagination_[room_id].reached_end = res.reached_end;
                        if (res.reached_end)
                        {
                            return_to_live_(room_id);
                        }
                    }
                });
        });
}

void ShellBase::return_to_live_(const std::string& room_id)
{
    auto& state = pagination_[room_id];
    state.is_focused = false;
    state.focus_event_id.clear();
    state.reached_end = false;
    state.fwd_in_flight = false;
    state.in_flight = true;

    run_async_(
        [this, room_id]()
        {
            client_->subscribe_room(room_id);
            auto pr =
                client_->paginate_back_with_status(room_id, kPaginationBatch);
            post_to_ui_(
                [this, room_id, pr]()
                {
                    pagination_[room_id].reached_start = pr.reached_start;
                    pagination_[room_id].in_flight = false;
                });
        });
}

void ShellBase::push_room_list_state_(RoomListState state)
{
    last_room_list_state_ = state;
}

// ── Secondary window registry ─────────────────────────────────────────────────

void ShellBase::register_room_window_(RoomWindowBase* w)
{
    secondary_windows_[w->room_id()] = w;
}

void ShellBase::unregister_room_window_(RoomWindowBase* w)
{
    auto it = secondary_windows_.find(w->room_id());
    if (it != secondary_windows_.end() && it->second == w)
    {
        secondary_windows_.erase(it);
    }
}

void ShellBase::acquire_room_subscription_(const std::string& room_id)
{
    int& refs = room_subscription_refs_[room_id];
    if (++refs > 1)
    {
        return;
    }
    // If the main window is already showing this room its subscription is live.
    if (room_id == current_room_id_)
    {
        return;
    }
    run_async_(
        [this, room_id]
        {
            if (client_)
            {
                client_->subscribe_room(room_id);
            }
        });
}

void ShellBase::release_room_subscription_(const std::string& room_id)
{
    auto it = room_subscription_refs_.find(room_id);
    if (it == room_subscription_refs_.end())
    {
        return;
    }
    if (--it->second > 0)
    {
        return;
    }
    room_subscription_refs_.erase(it);
    if (room_id == current_room_id_)
    {
        return;
    }
    run_async_(
        [this, room_id]
        {
            if (client_)
            {
                client_->unsubscribe_room(room_id);
            }
        });
}

void ShellBase::dispatch_to_secondary_windows_(
    const std::string& room_id, const std::function<void(RoomWindowBase*)>& fn)
{
    auto it = secondary_windows_.find(room_id);
    if (it != secondary_windows_.end())
    {
        fn(it->second);
    }
}

void ShellBase::open_room_in_new_window(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    auto it = secondary_windows_.find(room_id);
    if (it != secondary_windows_.end())
    {
        it->second->bring_to_front();
        return;
    }
    RoomWindowBase* w = create_secondary_room_window_(room_id);
    if (w)
    {
        // A pop-out opened while in dark mode (with no later theme change)
        // would otherwise be stuck on its constructor's default theme.
        w->apply_theme(current_theme_);
        owned_secondary_windows_.emplace_back(w);
    }
}

void ShellBase::release_owned_window_(RoomWindowBase* w)
{
    auto it = std::find_if(owned_secondary_windows_.begin(),
                           owned_secondary_windows_.end(),
                           [w](const auto& up)
                           {
                               return up.get() == w;
                           });
    if (it != owned_secondary_windows_.end())
    {
        owned_secondary_windows_.erase(it); // unique_ptr destructor runs here
    }
}

void ShellBase::maybe_send_read_receipt_(const std::string& room_id,
                                         const std::string& event_id)
{
    if (room_id.empty() || event_id.empty())
    {
        return;
    }
    auto& last = last_sent_receipt_[room_id];
    if (last == event_id)
    {
        return;
    }
    last = event_id;
    run_async_(
        [this, room_id, event_id]()
        {
            if (client_)
            {
                client_->send_read_receipt(room_id, event_id);
            }
        });
}

void ShellBase::mark_room_read_(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    for (auto& r : rooms_)
    {
        if (r.id == room_id)
        {
            r.unread_count = 0;
            break;
        }
    }
    auto it = per_account_rooms_.find(my_user_id_);
    if (it != per_account_rooms_.end())
    {
        for (auto& r : it->second)
        {
            if (r.id == room_id)
            {
                r.unread_count = 0;
                break;
            }
        }
    }
    on_rooms_updated_();
    run_async_(
        [this, room_id]()
        {
            if (client_)
            {
                client_->mark_room_as_read(room_id);
            }
        });
}

static std::string format_typing_text(const std::vector<std::string>& names)
{
    if (names.empty())
    {
        return {};
    }
    if (names.size() == 1)
    {
        return names[0] + " is typing\xe2\x80\xa6";
    }
    if (names.size() == 2)
    {
        return names[0] + " and " + names[1] + " are typing\xe2\x80\xa6";
    }
    return names[0] + ", " + names[1] + ", and " +
           std::to_string(names.size() - 2) + " others are typing\xe2\x80\xa6";
}

void ShellBase::handle_account_prefs_updated_ui_(std::string user_id,
                                                 std::string json)
{
    // Only the active account's prefs set the pending restore room.
    if (active_account_index_ < 0 ||
        accounts_[active_account_index_]->user_id != user_id)
    {
        return;
    }
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.last_room.empty() && pending_restore_room_.empty() &&
        current_room_id_.empty())
    {
        pending_restore_room_ = prefs.last_room;
    }
}

void ShellBase::handle_image_packs_updated_ui_()
{
    refresh_pickers_packs_();
    cached_emoticons_.clear();
    if (client_)
    {
        for (auto& pack : client_->list_image_packs())
        {
            for (auto& img : client_->list_pack_images(
                     pack.id, tesseract::PackUsageFilter::Emoticon))
            {
                cached_emoticons_.push_back(std::move(img));
            }
        }
    }
}

void ShellBase::handle_typing_changed_ui_(std::string room_id,
                                          std::vector<std::string> names)
{
    const std::string text = format_typing_text(names);
    const bool visible = !names.empty();
    if (room_id == current_room_id_)
    {
        typing_bar_visible_ = visible;
        update_typing_bar_(text, visible);
    }
    dispatch_to_secondary_windows_(room_id,
                                   [&](RoomWindowBase* w)
                                   {
                                       w->on_typing_changed(text, visible);
                                   });
}

void ShellBase::handle_compose_text_changed_(const std::string& text)
{
    bool typing = !text.empty();
    if (typing == compose_typing_active_)
    {
        return;
    }
    compose_typing_active_ = typing;
    if (!current_room_id_.empty() && client_)
    {
        client_->send_typing_notice(current_room_id_, typing);
    }
}

void ShellBase::handle_compose_room_leaving_(const std::string& old_room_id)
{
    if (!compose_typing_active_ || old_room_id.empty() || !client_)
    {
        return;
    }
    compose_typing_active_ = false;
    client_->send_typing_notice(old_room_id, false);
}

void ShellBase::apply_current_theme_()
{
    auto& s = tesseract::Settings::instance();
    tk::ThemeMode mode =
        s.theme_pref == tesseract::Settings::ThemePreference::Dark
            ? tk::ThemeMode::Dark
        : s.theme_pref == tesseract::Settings::ThemePreference::Light
            ? tk::ThemeMode::Light
            : os_color_scheme_(); // System → ask the OS
    current_theme_ =
        (mode == tk::ThemeMode::Dark) ? tk::Theme::dark() : tk::Theme::light();
    apply_theme_ui_(current_theme_);
}

void ShellBase::apply_theme_to_secondary_windows_(const tk::Theme& t)
{
    for (auto& w : owned_secondary_windows_)
    {
        if (w)
        {
            w->apply_theme(t);
        }
    }
}

void ShellBase::set_theme_preference_(tesseract::Settings::ThemePreference pref)
{
    tesseract::Settings::instance().theme_pref = pref;
    tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
    apply_current_theme_();
}

// ── Tab management ─────────────────────────────────────────────────────────

namespace
{

// Works with any vector whose element has a .room_id field.
template <typename Tab>
size_t find_tab_(const std::vector<Tab>& tabs, const std::string& room_id)
{
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        if (tabs[i].room_id == room_id)
        {
            return i;
        }
    }
    return SIZE_MAX;
}

} // namespace

void ShellBase::save_tab_message_cache_()
{
    const auto* msgs = get_current_messages_();
    if (!msgs || msgs->empty() || current_room_id_.empty())
    {
        return;
    }
    auto map_it = message_cache_.find(current_room_id_);
    if (map_it != message_cache_.end())
    {
        auto lru_it = std::find(message_cache_lru_.begin(),
                                message_cache_lru_.end(), current_room_id_);
        if (lru_it != message_cache_lru_.end())
        {
            message_cache_lru_.erase(lru_it);
        }
    }
    else if (static_cast<int>(message_cache_.size()) >= kMsgCacheCapacity)
    {
        message_cache_.erase(message_cache_lru_.back());
        message_cache_lru_.pop_back();
    }
    message_cache_lru_.push_front(current_room_id_);
    message_cache_[current_room_id_] = *msgs;
}

bool ShellBase::try_restore_message_cache_(const std::string& room_id)
{
    auto it = message_cache_.find(room_id);
    if (it == message_cache_.end() || it->second.empty())
    {
        return false;
    }
    view_displayed_room_id_ = room_id;
    apply_cached_messages_(it->second);
    return true;
}

void ShellBase::tab_open_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
            save_tab_message_cache_();
        }
        active_tab_idx_ = existing;
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        on_tab_state_changed_ui_();
        return;
    }
    if (!tabs_.empty())
    {
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
        tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
        save_tab_message_cache_();
    }
    // Bootstrap: wrap current_room_id_ as first tab if tabs_ is empty.
    if (tabs_.empty() && !current_room_id_.empty())
    {
        tabs_.push_back({current_room_id_, 0.f, {}});
    }
    tabs_.push_back({room_id, 0.f, {}});
    active_tab_idx_ = tabs_.size() - 1;
    current_room_id_ = room_id;
    on_tab_state_changed_ui_();
}

void ShellBase::tab_select_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
            save_tab_message_cache_();
        }
        active_tab_idx_ = existing;
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        on_tab_state_changed_ui_();
        return;
    }
    if (tabs_.empty())
    {
        tabs_.push_back({room_id, 0.f, {}});
    }
    else
    {
        save_tab_message_cache_();
        tabs_[active_tab_idx_] = {room_id, 0.f, {}};
    }
    current_room_id_ = room_id;
    on_tab_state_changed_ui_();
}

void ShellBase::tab_navigate_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
            save_tab_message_cache_();
        }
        active_tab_idx_ = existing;
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        on_tab_state_changed_ui_();
        return;
    }
    if (tabs_.size() <= 1)
    {
        tab_select_room(room_id);
    }
    else
    {
        tab_open_room(room_id);
    }
}

void ShellBase::tab_close(const std::string& room_id)
{
    if (tabs_.size() <= 1)
    {
        return;
    }
    size_t idx = find_tab_(tabs_, room_id);
    if (idx == SIZE_MAX)
    {
        return;
    }

    const bool closing_active = (idx == active_tab_idx_);
    if (!closing_active)
    {
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
        tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
    }
    size_t new_active = active_tab_idx_;
    if (closing_active)
    {
        new_active = (idx > 0) ? idx - 1 : 0;
    }
    else if (idx < active_tab_idx_)
    {
        --new_active;
    }

    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(idx));
    active_tab_idx_ = new_active;
    current_room_id_ = tabs_[active_tab_idx_].room_id;
    on_tab_state_changed_ui_();
}

void ShellBase::wire_voice_capture_(
    views::RoomView*             rv,
    std::function<void()>        request_repaint,
    std::function<std::string()> get_room_id,
    std::function<void()>        clear_text_fn)
{
    rv->compose_bar()->set_mic_available(capture_ != nullptr);
    if (!capture_)
        return;

    // on_mic_clicked: start or stop recording. When starting, snapshot the
    // target room so a mid-recording room switch doesn't mis-deliver the
    // voice message. Assign on_stopped fresh each start so it carries the
    // correct room_id.
    rv->on_mic_clicked = [this, rv, get_room_id, clear_text_fn]() mutable
    {
        auto* cb = rv->compose_bar();
        if (!capture_->is_recording())
        {
            const std::string rid = get_room_id();
            capture_->start();
            cb->set_recording(true);
            capture_->on_stopped =
                [this, rv, rid, clear_text_fn](
                    std::vector<std::uint8_t>  pcm,
                    std::vector<std::uint16_t> waveform,
                    std::uint64_t              duration_ms) mutable
            {
                auto* cb2 = rv->compose_bar();
                cb2->set_recording(false);
                if (pcm.empty())
                    return;
                if (duration_ms < 500)
                    return;
                const std::uint64_t est   = duration_ms * 3;
                const std::uint64_t limit = client_->media_upload_limit();
                if (limit > 0 && est > limit)
                    return;
                auto res = client_->send_voice(
                    rid, pcm.data(), pcm.size(),
                    duration_ms, waveform,
                    cb2->current_text(),
                    cb2->reply_event_id());
                cb2->clear_reply();
                clear_text_fn();
                (void)res;
            };
        }
        else
        {
            capture_->stop();
        }
    };

    rv->on_cancel_voice = [this, rv]()
    {
        if (!capture_)
            return;
        capture_->cancel();
        rv->compose_bar()->set_recording(false);
    };

    capture_->on_amplitude = [rv, request_repaint](std::uint16_t amp) mutable
    {
        rv->compose_bar()->push_amplitude(amp);
        request_repaint();
    };
}

} // namespace tesseract
