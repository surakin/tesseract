#include "app/ShellBase.h"
#include "app/RoomWindowBase.h"
#include "tk/blurhash.h"
#include "tk/theme.h"
#include "views/MainAppWidget.h"
#include "views/RoomListView.h"
#include "views/RoomView.h"
#include "views/UserInfo.h"
#include "views/html_spans.h"
#include "views/map_tiles.h"
#include <tesseract/paths.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
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
    const bool use_room_endpoint = !r.avatar_url.empty();
    const std::string mxc = use_room_endpoint ? r.avatar_url : r.dm_avatar_url;
    if (mxc.empty() || tk_avatars_.count(mxc))
    {
        return;
    }
    if (!media_fetches_in_flight_.insert(mxc).second)
    {
        return;
    }
    const std::string room_id = r.id;
    run_async_(
        [this, room_id, mxc, use_room_endpoint]()
        {
            auto bytes = media_disk_cache_.load(mxc);
            if (bytes.empty())
            {
                // DM fallback avatars are user mxcs, not room avatars, so
                // route them through the generic media endpoint.
                bytes = use_room_endpoint
                            ? client_->fetch_avatar_bytes(room_id)
                            : client_->fetch_media_bytes(mxc);
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

const tk::Image* ShellBase::shell_sticker_(const std::string& mxc)
{
    if (const auto* f = anim_cache_.current_frame(mxc))
    {
        start_anim_tick_(); // visible animated frame → keep the timer running
        return f;
    }
    auto it = tk_images_.find(mxc);
    if (it != tk_images_.end())
    {
        return it->second.get();
    }
    ensure_media_image_(mxc, 64, 64);
    return nullptr;
}

const tk::Image* ShellBase::shell_sticker_no_fetch_(const std::string& mxc)
{
    if (const auto* f = anim_cache_.current_frame(mxc))
    {
        start_anim_tick_(); // visible animated frame → keep the timer running
        return f;
    }
    auto it = tk_images_.find(mxc);
    return it == tk_images_.end() ? nullptr : it->second.get();
}

void ShellBase::set_room_notification_mode_(const std::string& room_id,
                                             const std::string& mode)
{
    if (!client_) return;
    auto* c = client_;
    run_async_([c, room_id, mode]() {
        c->set_room_notification_mode(room_id, mode);
    });
}

void ShellBase::wire_main_app_widget_(views::MainAppWidget* app)
{
    auto avatar_lookup = [this](const std::string& mxc) -> const tk::Image*
    {
        auto it = tk_avatars_.find(mxc);
        return it == tk_avatars_.end() ? nullptr : it->second.get();
    };

    app->set_avatar_provider(avatar_lookup);
    app->room_list_view()->set_avatar_provider(avatar_lookup);
    app->room_list_view()->set_sticker_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_sticker_(mxc);
        });

    app->room_view()->set_avatar_provider(avatar_lookup);
    app->room_view()->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_sticker_no_fetch_(mxc);
        });
    app->room_view()->set_preview_provider(
        [this](const std::string& url) -> const views::UrlPreviewData*
        {
            auto it = url_preview_data_.find(url);
            if (it == url_preview_data_.end())
            {
                return nullptr;
            }
            if (!it->second.image_mxc.empty() &&
                !tk_images_.count(it->second.image_mxc) &&
                !anim_cache_.has(it->second.image_mxc))
            {
                ensure_media_image_(it->second.image_mxc, 64, 64);
            }
            return &it->second;
        });

    app->user_info()->set_image_provider(avatar_lookup);

    auto presence_lookup = [this](const std::string& uid) -> PresenceState
    {
        return presence_for_(uid);
    };
    app->room_list_view()->set_presence_provider(presence_lookup);
    app->room_view()->room_info_panel()->set_presence_provider(presence_lookup);

    // Avatar click in UserProfilePanel → open the image viewer with the
    // (already-cached) avatar. The viewer's image_provider (wired in
    // wire_main_app_viewers_) falls back to tk_avatars_, so passing the mxc
    // URL is enough; no per-shell full-res fetch needed.
    app->room_view()->on_avatar_clicked =
        [app](std::string url, std::string name)
    {
        if (url.empty() || !app->image_viewer())
            return;
        app->image_viewer()->open(url, url, name, 0, 0);
        app->show_image_viewer(true);
        // Trigger the shell-wired relayout so the surface repaints with the
        // viewer visible (mirrors the per-shell on_image_clicked path).
        if (app->room_view()->on_layout_changed)
            app->room_view()->on_layout_changed();
    };

    app->room_view()->on_fetch_notification_mode =
        [this, app](std::string room_id)
    {
        if (!client_) return;
        auto* c = client_;
        run_async_([this, app, c, room_id = std::move(room_id)]() mutable {
            auto mode = c->get_room_notification_mode(room_id);
            post_to_ui_([app, mode = std::move(mode)]() mutable {
                app->room_view()->room_info_panel()->set_notification_mode(
                    std::move(mode));
            });
        });
    };
    app->room_view()->on_notification_mode_changed =
        [this](std::string room_id, std::string mode)
    {
        set_room_notification_mode_(room_id, mode);
    };

    // ── Invite selection and action wiring ────────────────────────────────
    app->room_list_view()->on_invite_selected =
        [this, app, avatar_lookup](const std::string& room_id)
    {
        const InviteInfo* inv = find_invite_(room_id);
        if (!inv)
            return;
        current_invite_room_id_   = inv->room_id;
        current_invite_inviter_id_ = inv->inviter_user_id;
        app->show_invite(*inv, avatar_lookup);
        app->room_list_view()->set_selected_room(""); // clear room highlight
        request_relayout_();
    };

    app->invite_card()->on_accept = [this]
    {
        accept_invite_async_(current_invite_room_id_);
    };
    app->invite_card()->on_decline = [this]
    {
        decline_invite_async_(current_invite_room_id_);
        if (main_app_)
            main_app_->clear_content();
        request_relayout_();
    };
    app->invite_card()->on_block = [this]
    {
        block_invite_async_(current_invite_room_id_, current_invite_inviter_id_);
        if (main_app_)
            main_app_->clear_content();
        request_relayout_();
    };
}

void ShellBase::wire_main_app_viewers_(views::MainAppWidget* app,
                                       tk::Host&             host,
                                       std::function<void()> request_relayout,
                                       std::function<void()> on_image_close,
                                       std::function<void()> on_video_close)
{
    auto image_lookup = [this](const std::string& mxc) -> const tk::Image*
    {
        if (const tk::Image* img = shell_sticker_no_fetch_(mxc))
            return img;
        // Avatars live in a separate cache from media/stickers; let the
        // viewer find them so clicking an avatar in UserProfilePanel /
        // RoomInfoPanel can show whatever resolution is already cached.
        auto it = tk_avatars_.find(mxc);
        return it == tk_avatars_.end() ? nullptr : it->second.get();
    };

    auto* iv = app->image_viewer();
    iv->set_image_provider(image_lookup);
    iv->set_repaint_requester(request_relayout);
    iv->on_close = [app, request_relayout, on_image_close]
    {
        app->show_image_viewer(false);
        request_relayout();
        if (on_image_close)
        {
            on_image_close();
        }
    };

    auto* vv = app->video_viewer();
    vv->set_image_provider(image_lookup);
    vv->set_video_player(host.make_video_player());
    vv->set_repaint_requester(request_relayout);
    vv->on_close = [app, request_relayout, on_video_close]
    {
        app->show_video_viewer(false);
        request_relayout();
        if (on_video_close)
        {
            on_video_close();
        }
    };
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
            if (d->empty())
            {
                media_disk_cache_.evict(url);
            }
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
    if (!waveform_store_inited_)
    {
        waveform_store_inited_ = true;
        tesseract::init_waveform_cache(
            (tesseract::cache_dir() / "waveforms.db").string());
    }
    ensure_user_avatar_(ev.sender_avatar_url);
    for (const auto& rr : ev.read_receipts)
    {
        ensure_user_avatar_(rr.avatar_url);
    }

    if (ev.type == EventType::Image)
    {
        const auto& img = static_cast<const ImageEvent&>(ev);
        if (img.thumbnail)
        {
            ensure_media_image_(img.thumbnail->fetch_token(),
                                visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight);
        }
        if (!img.thumbnail || tesseract::Settings::instance().prefetch_full_media)
        {
            if (img.source)
                ensure_media_image_(img.source->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight);
        }
    }
    else if (ev.type == EventType::Sticker)
    {
        const auto& s = static_cast<const StickerEvent&>(ev);
        if (s.thumbnail)
        {
            ensure_media_image_(s.thumbnail->fetch_token(),
                                visual::kStickerSize, visual::kStickerSize);
        }
        if (!s.thumbnail || tesseract::Settings::instance().prefetch_full_media)
        {
            if (s.source)
                ensure_media_image_(s.source->fetch_token(),
                                    visual::kStickerSize, visual::kStickerSize);
        }
    }
    else if (ev.type == EventType::Voice)
    {
        const auto& v = static_cast<const VoiceEvent&>(ev);
        if (v.source)
        {
            const std::string src = v.source->fetch_token();
            const bool audio_new =
                voice_prefetched_.insert(src).second;
            const bool waveform_new =
                v.waveform.empty() &&
                voice_waveform_in_flight_.insert(src).second;

            if (audio_new || waveform_new)
            {
                const std::string event_id = ev.event_id;
                const std::string room_id  = current_room_id_;
                run_async_(
                    [this, src, event_id, room_id, waveform_new]()
                    {
                        auto bytes = client_->fetch_source_bytes(src);
                        if (!waveform_new || bytes.empty())
                        {
                            return;
                        }
                        auto waveform = tesseract::load_voice_waveform(src);
                        if (waveform.empty())
                        {
                            waveform =
                                tesseract::compute_waveform_from_ogg(bytes);
                            if (!waveform.empty())
                            {
                                tesseract::store_voice_waveform(src, waveform);
                            }
                        }
                        if (!waveform.empty())
                        {
                            post_to_ui_(
                                [this, room_id, event_id,
                                 waveform = std::move(waveform)]() mutable
                                {
                                    handle_voice_waveform_ready_ui_(
                                        room_id, event_id,
                                        std::move(waveform));
                                });
                        }
                    });
            }
        }
    }
    else if (ev.type == EventType::Audio)
    {
        const auto& a = static_cast<const AudioEvent&>(ev);
        if (a.source &&
            tesseract::Settings::instance().prefetch_full_media)
        {
            const std::string src = a.source->fetch_token();
            if (voice_prefetched_.insert(src).second)
                run_async_([this, src]() { client_->fetch_source_bytes(src); });
        }
    }
    else if (ev.type == EventType::Video)
    {
        const auto& vid = static_cast<const VideoEvent&>(ev);
        if (vid.thumbnail)
        {
            ensure_media_image_(vid.thumbnail->fetch_token(),
                                visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight);
        }
        if (!vid.thumbnail && vid.source &&
            video_thumb_in_flight_.insert(ev.event_id).second)
        {
            generate_video_thumbnail_(ev.event_id, vid.source->fetch_token());
        }
    }
    for (const auto& r : ev.reactions)
    {
        if (r.source)
        {
            ensure_media_image_(r.source->fetch_token(), 20, 20);
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
ShellBase::build_rows_(const EventList& snapshot)
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
    const EventList& snapshot)
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
    // The tray aggregate covers every signed-in account, not just the active
    // one — recompute on every account's update, even if it isn't the one
    // whose rooms_ cache is currently live.
    notify_tray_unread_();
    if (user_id != my_user_id_)
    {
        return;
    }
    rooms_ = std::move(rooms);
    update_space_children_cache_();
    on_rooms_updated_();
}

void ShellBase::push_invites_(std::string user_id, std::vector<InviteInfo> invites)
{
    per_account_invites_[user_id] = invites;
    if (user_id != my_user_id_)
    {
        return;
    }
    invites_ = std::move(invites);
    ensure_invite_avatars_();
    on_invites_updated_();
}

void ShellBase::ensure_invite_avatars_()
{
    for (const auto& inv : invites_)
    {
        const std::string& mxc =
            inv.is_direct ? inv.inviter_avatar_url : inv.room_avatar_url;
        ensure_user_avatar_(mxc);
    }
}

const InviteInfo* ShellBase::find_invite_(const std::string& room_id) const
{
    for (const auto& inv : invites_)
    {
        if (inv.room_id == room_id)
        {
            return &inv;
        }
    }
    return nullptr;
}

void ShellBase::accept_invite_async_(const std::string& room_id)
{
    if (room_id.empty() || !client_)
    {
        return;
    }
    auto* c = client_;
    run_async_(
        [this, c, room_id]()
        {
            auto res = c->accept_invite(room_id);
            post_to_ui_(
                [this, room_id, res]()
                {
                    if (res.ok)
                    {
                        tab_select_room(room_id);
                    }
                    else
                    {
                        std::fprintf(stderr, "[invite] accept failed for %s: %s\n",
                                     room_id.c_str(), res.message.c_str());
                    }
                });
        });
}

void ShellBase::decline_invite_async_(const std::string& room_id)
{
    if (room_id.empty() || !client_)
    {
        return;
    }
    // Optimistically remove from the local list for immediate UX; the next
    // on_invites_updated callback from sync will confirm or restore it.
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&room_id](const InviteInfo& inv)
                       {
                           return inv.room_id == room_id;
                       }),
        invites_.end());
    on_invites_updated_();

    auto* c = client_;
    run_async_(
        [this, c, room_id]()
        {
            auto res = c->decline_invite(room_id);
            if (!res.ok)
            {
                post_to_ui_(
                    [room_id, res]()
                    {
                        std::fprintf(stderr,
                                     "[invite] decline failed for %s: %s\n",
                                     room_id.c_str(), res.message.c_str());
                    });
            }
        });
}

void ShellBase::block_invite_async_(const std::string& room_id,
                                    const std::string& inviter_id)
{
    if (room_id.empty() || !client_)
    {
        return;
    }
    // Optimistically remove from the local list for immediate UX; the next
    // on_invites_updated callback from sync will confirm or restore it.
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&room_id](const InviteInfo& inv)
                       {
                           return inv.room_id == room_id;
                       }),
        invites_.end());
    on_invites_updated_();

    auto* c = client_;
    run_async_(
        [this, c, room_id, inviter_id]()
        {
            auto res = c->block_invite(room_id, inviter_id);
            if (!res.ok)
            {
                post_to_ui_(
                    [room_id, res]()
                    {
                        std::fprintf(stderr,
                                     "[invite] block failed for %s: %s\n",
                                     room_id.c_str(), res.message.c_str());
                    });
            }
        });
}

void ShellBase::update_space_children_cache_()
{
    if (!client_)
    {
        space_children_cache_.clear();
        return;
    }
    std::vector<std::string> space_ids;
    for (const auto& r : rooms_)
    {
        if (r.is_space)
        {
            space_ids.push_back(r.id);
        }
    }
    if (space_ids.empty())
    {
        space_children_cache_.clear();
        return;
    }
    auto* c = client_;
    run_async_(
        [this, c, space_ids = std::move(space_ids)]()
        {
            std::unordered_map<std::string, std::vector<std::string>> fresh;
            for (const auto& id : space_ids)
            {
                fresh[id] = c->space_children(id);
            }
            post_to_ui_(
                [this, fresh = std::move(fresh)]() mutable
                {
                    if (fresh != space_children_cache_)
                    {
                        space_children_cache_ = std::move(fresh);
                        on_space_children_cache_ready_ui_();
                    }
                });
        });
}

std::pair<bool, bool> ShellBase::compute_tray_unread(
    const std::unordered_map<std::string, std::vector<RoomInfo>>& by_account)
{
    bool has_unread    = false;
    bool has_highlight = false;
    for (const auto& [_uid, rooms] : by_account)
    {
        for (const auto& r : rooms)
        {
            if (r.notification_count > 0)
            {
                has_unread = true;
            }
            if (r.highlight_count > 0)
            {
                has_highlight = true;
            }
            if (has_unread && has_highlight)
            {
                return {true, true};
            }
        }
    }
    return {has_unread, has_highlight};
}

std::string ShellBase::find_existing_dm(const std::vector<RoomInfo>& rooms,
                                        const std::string&           user_id)
{
    if (user_id.empty())
    {
        return {};
    }
    for (const auto& r : rooms)
    {
        if (r.is_direct && r.dm_counterpart_user_id == user_id)
        {
            return r.id;
        }
    }
    return {};
}

std::string ShellBase::find_existing_dm_(const std::string& user_id) const
{
    return find_existing_dm(rooms_, user_id);
}

void ShellBase::notify_tray_unread_()
{
    auto [u, h] = compute_tray_unread(per_account_rooms_);
    if (u == last_tray_unread_ && h == last_tray_highlight_)
    {
        return;
    }
    last_tray_unread_    = u;
    last_tray_highlight_ = h;
    on_tray_unread_changed_(u, h);
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
            r.notification_count = 0;
            r.highlight_count    = 0;
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
                r.notification_count = 0;
                r.highlight_count    = 0;
                break;
            }
        }
    }
    on_rooms_updated_();
    notify_tray_unread_();
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

void ShellBase::handle_voice_waveform_ready_ui_(
    std::string room_id, std::string event_id,
    std::vector<std::uint16_t> waveform)
{
    if (room_id != current_room_id_)
    {
        return;
    }
    if (room_view_)
    {
        if (auto* ml = room_view_->message_list())
        {
            ml->update_voice_waveform(event_id, std::move(waveform));
        }
    }
}

void ShellBase::on_url_preview_ready_(const std::string& url,
                                      const Client::UrlPreview& preview)
{
    tesseract::views::UrlPreviewData d;
    d.title = preview.title;
    d.description = preview.description;
    d.image_mxc = preview.image_mxc;
    d.image_w = preview.image_w;
    d.image_h = preview.image_h;
    url_preview_data_.emplace(url, std::move(d));

    if (!preview.image_mxc.empty())
    {
        ensure_media_image_(preview.image_mxc, 64, 64);
    }

    // Invalidate cached row heights so the preview card is included in the
    // next measure pass, then relayout to apply the new heights.
    if (room_view_)
    {
        room_view_->notify_url_preview_ready(url);
    }
    request_relayout_();

    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
            w->request_relayout();
        }
    }
}

void ShellBase::on_url_preview_failed_(const std::string& url)
{
    // No card to show (height unchanged) — just release the room-switch gate
    // so it doesn't wait the full timeout on a dead link.
    if (room_view_)
    {
        room_view_->notify_url_preview_ready(url);
    }
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w->room_view())
        {
            w->room_view()->notify_url_preview_ready(url);
        }
    }
}

bool ShellBase::tick_anim_()
{
    // Stop once nothing animated is on-screen — entries linger in the cache
    // after scrolling away / switching rooms, so checking emptiness would keep
    // the 60 Hz timer (and its repaints) running forever.
    if (!anim_cache_.any_visible())
    {
        stop_anim_tick_();
        return false;
    }
    if (anim_cache_.advance(monotonic_ms_()))
    {
        repaint_anim_frame_();
    }
    return true;
}

void ShellBase::handle_timeline_reset_ui_(std::string room_id,
                                          EventList snapshot)
{
    if (room_id == current_room_id_ && room_view_)
    {
        auto rows = build_rows_(snapshot);
        // A genuine switch, OR a re-population of an emptied view (e.g.
        // logout → login → same room): both warrant the display gate.
        const auto* ml = room_view_->message_list();
        const bool room_switch = view_displayed_room_id_ != room_id ||
                                 (ml && ml->messages().empty());
        view_displayed_room_id_ = room_id;
        room_view_->set_messages(std::move(rows), room_switch);
        request_relayout_();
        if (auto* list = room_view_->message_list())
        {
            const auto& pstate = pagination_[room_id];
            if (room_switch && pstate.is_focused)
            {
                list->begin_focused_gate(pstate.focus_event_id);
            }
            list->set_historical_mode(pstate.is_focused);
            if (pstate.is_focused)
            {
                list->scroll_to_event_id(pstate.focus_event_id);
            }
            // Restore saved scroll offset when returning to a tab that had
            // been scrolled up from the bottom.
            if (room_switch && !pstate.is_focused &&
                active_tab_idx_ < tabs_.size() &&
                tabs_[active_tab_idx_].room_id == room_id &&
                tabs_[active_tab_idx_].scroll_offset > 0.f)
            {
                list->scroll_to_offset(tabs_[active_tab_idx_].scroll_offset);
            }
        }
    }

    dispatch_timeline_reset_secondary_(room_id, snapshot);
}

void ShellBase::handle_message_inserted_ui_(std::string room_id,
                                            std::size_t index,
                                            std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    if (room_id == current_room_id_ && room_view_)
    {
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        room_view_->insert_message(
            index, tesseract::views::make_row_data(*ev, my_user_id_));
        request_relayout_();
    }
    dispatch_message_inserted_secondary_(room_id, index, *ev);
}

void ShellBase::handle_message_updated_ui_(std::string room_id,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    if (room_id == current_room_id_ && room_view_)
    {
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        room_view_->update_message(
            index, tesseract::views::make_row_data(*ev, my_user_id_));
        request_relayout_();
    }
    dispatch_message_updated_secondary_(room_id, index, *ev);
}

void ShellBase::handle_message_removed_ui_(std::string room_id,
                                           std::size_t index)
{
    if (room_id == current_room_id_ && room_view_)
    {
        room_view_->remove_message(index);
        request_relayout_();
    }
    dispatch_message_removed_secondary_(room_id, index);
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

std::string ShellBase::shortcode_for_mxc_(const std::string& mxc) const
{
    if (mxc.empty())
    {
        return {};
    }
    for (const auto& e : cached_emoticons_)
    {
        if (e.url == mxc)
        {
            return e.shortcode;
        }
    }
    return {};
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

void ShellBase::handle_presence_changed_ui_(const std::string& user_id,
                                            PresenceState state)
{
    user_presence_[user_id] = state;
    // Always repaint: the room list may show a DM dot, and the RoomInfoPanel
    // (main or pop-out) may show a member dot — both read presence_provider_
    // on every paint. Presence events are low-frequency so the cost is small.
    on_rooms_updated_();
    update_secondary_room_infos_();
}

PresenceState ShellBase::presence_for_(const std::string& user_id) const
{
    auto it = user_presence_.find(user_id);
    return it != user_presence_.end() ? it->second : PresenceState::Offline;
}

// ── Presence (send-side) ──────────────────────────────────────────────────────

namespace
{

// Map PresenceTracker's enum to the Client::PresenceState the FFI accepts.
tesseract::PresenceState to_client_presence(PresenceTracker::State s)
{
    switch (s)
    {
        case PresenceTracker::State::Online:      return PresenceState::Online;
        case PresenceTracker::State::Unavailable: return PresenceState::Unavailable;
        case PresenceTracker::State::Offline:     return PresenceState::Offline;
    }
    return PresenceState::Offline;
}

} // namespace

void ShellBase::notify_user_activity_()
{
    if (!presence_tracker_)
    {
        // Lazily start tracking on the first activity we see *after* sync is
        // up and running. This avoids publishing Online before the homeserver
        // has acknowledged our access token via the sliding-sync handshake.
        if (last_room_list_state_ == RoomListState::Running)
        {
            start_presence_tracking_();
        }
        else
        {
            return;
        }
    }
    presence_tracker_->notify_input();
}

void ShellBase::notify_window_active_(bool active)
{
    if (presence_tracker_)
    {
        presence_tracker_->notify_window_active(active);
    }
}

void ShellBase::notify_presence_tick_()
{
    if (presence_tracker_)
    {
        presence_tracker_->notify_tick();
    }
}

void ShellBase::notify_presence_logout_()
{
    if (!presence_tracker_)
    {
        return;
    }
    // Tear down the tracker first so its on_state_change can't fire and
    // spawn a worker that races with the shell's imminent
    // accounts_.erase() / client destruction. Then PUT Offline
    // *synchronously* on the UI thread: this is a user-initiated logout
    // (already a high-latency action), the brief freeze is acceptable, and
    // doing it inline guarantees we never hold a raw Client* across the
    // destruction boundary.
    presence_tracker_->on_state_change = nullptr;
    presence_tracker_.reset();
    if (client_)
    {
        (void) client_->set_presence(PresenceState::Offline);
    }
}

void ShellBase::start_presence_tracking_()
{
    if (presence_tracker_)
    {
        return;
    }
    if (!tesseract::Settings::instance().send_presence)
    {
        return;
    }
    presence_tracker_ = std::make_unique<PresenceTracker>();
    presence_tracker_->on_state_change =
        [this](PresenceTracker::State s)
    {
        // Online ↔ Unavailable transitions: send the PUT through run_async_
        // so app shutdown drains it (shutting_down_ + workers_in_flight_
        // protect against accessing a destroyed Client during ~ShellBase).
        // We access client_ inside the worker — same pattern as
        // ensure_room_avatar_ — so it picks up the currently-active
        // account, which is what we want.
        const auto target = to_client_presence(s);
        run_async_(
            [this, target]
            {
                if (client_)
                {
                    (void) client_->set_presence(target);
                }
            });
    };
    presence_tracker_->notify_sync_started();
}

void ShellBase::handle_send_presence_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    s.send_presence = enabled;
    s.save_to_disk(tesseract::config_dir());

    if (client_)
    {
        client_->set_presence_polling_enabled(enabled);
    }

    if (enabled)
    {
        start_presence_tracking_();
    }
    else
    {
        notify_presence_logout_();
        presence_tracker_.reset();
    }
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
    // Dismiss any visible InviteCard so the chat panel shows the room view.
    if (main_app_)
        main_app_->show_room();
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
    rv->on_mic_clicked =
        [this, rv, get_room_id, clear_text_fn, request_repaint]() mutable
    {
        auto* cb = rv->compose_bar();
        if (!capture_->is_recording())
        {
            const std::string rid = get_room_id();
            // Assign on_stopped before start() so WASAPI init failures that
            // call fire_error_() synchronously see a valid callback and can
            // reset the UI rather than being silently dropped.
            capture_->on_stopped =
                [this, rv, rid, clear_text_fn, request_repaint](
                    std::vector<std::uint8_t>  pcm,
                    std::vector<std::uint16_t> waveform,
                    std::uint64_t              duration_ms) mutable
            {
                auto* cb2 = rv->compose_bar();
                cb2->set_recording(false);
                request_repaint();
                if (pcm.empty() || duration_ms < 500)
                    return;

                // Capture UI state and clear compose bar before going async,
                // so the user can start a new recording immediately.
                std::string caption  = cb2->current_text();
                std::string reply_id = cb2->reply_event_id();
                cb2->clear_reply();
                clear_text_fn();

                // Encoding (Opus) and upload both block; run off the UI thread.
                run_async_(
                    [this, rid,
                     pcm      = std::move(pcm),
                     waveform  = std::move(waveform),
                     duration_ms, caption, reply_id]() mutable
                    {
                        const std::uint64_t est   = duration_ms * 3;
                        const std::uint64_t limit = client_->media_upload_limit();
                        if (limit > 0 && est > limit)
                            return;
                        auto res = client_->send_voice(
                            rid, pcm.data(), pcm.size(),
                            duration_ms, waveform,
                            caption, reply_id);
                        if (!res.ok)
                            std::fprintf(stderr, "[voice] send failed: %s\n",
                                         res.message.c_str());
                    });
            };
            capture_->start();
            if (!capture_->is_recording())
                return; // WASAPI init failed; fire_error_ queued on_stopped({},{},0)
            cb->set_recording(true);
            request_repaint();
        }
        else
        {
            capture_->stop();
        }
    };

    rv->on_cancel_voice = [this, rv, request_repaint]()
    {
        if (!capture_)
            return;
        capture_->cancel();
        rv->compose_bar()->set_recording(false);
        request_repaint();
    };

    capture_->on_amplitude = [rv, request_repaint](std::uint16_t amp) mutable
    {
        rv->compose_bar()->push_amplitude(amp);
        request_repaint();
    };
}

void ShellBase::compute_cache_sizes_(
    std::function<void(uint64_t, uint64_t)> callback)
{
    if (my_user_id_.empty() || !callback)
        return;
    const auto uid = my_user_id_;
    run_async_([this, uid, cb = std::move(callback)]
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        uint64_t local = 0;
        for (const auto& de :
             fs::recursive_directory_iterator(tesseract::cache_dir(), ec))
        {
            if (de.is_regular_file(ec))
                local += de.file_size(ec);
        }

        uint64_t sdk = 0;
        const auto sdk_dir = tesseract::SessionStore::sdk_store_dir(uid);
        for (const auto& de :
             fs::recursive_directory_iterator(sdk_dir, ec))
        {
            if (de.is_regular_file(ec))
                sdk += de.file_size(ec);
        }

        post_to_ui_([cb, local, sdk] { cb(local, sdk); });
    });
}

void ShellBase::clear_all_caches_(
    std::function<void(uint64_t, uint64_t)> recompute_callback)
{
    if (my_user_id_.empty())
        return;
    const auto uid = my_user_id_;
    run_async_([this, uid, recalc = std::move(recompute_callback)]
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Media disk cache — clear() removes all files and recreates the dir.
        media_disk_cache_.clear();

        // Waveform SQLite — best-effort (locked on Windows if WAL is open).
        fs::remove(tesseract::cache_dir() / "waveforms.db", ec);

        // Clear in-memory image caches, reinit the waveform store, and restart
        // the SDK (which clears the SDK event cache + state store and starts a
        // fresh sync).
        post_to_ui_([this]
        {
            tk_avatars_.clear();
            tk_images_.clear();
            anim_cache_ = {};
            tesseract::init_waveform_cache(
                (tesseract::cache_dir() / "waveforms.db").string());
            restart_sdk_();
        });

        // Recompute sizes so the UI reflects the cleared state.
        if (recalc)
            compute_cache_sizes_(std::move(recalc));
    });
}

void ShellBase::restart_sdk_()
{
    if (!client_ || my_user_id_.empty())
        return;
    auto json = tesseract::SessionStore::load_account(my_user_id_);
    if (!json)
        return;

    // Deselect the active room — cached timeline data is about to be wiped.
    current_room_id_.clear();
    tabs_.clear();
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();
    message_cache_.clear();
    message_cache_lru_.clear();
    on_tab_state_changed_ui_();

    client_->stop_sync();
    client_->clear_caches();
    if (client_->restore_session(*json))
        client_->start_sync(event_handler_);
}

} // namespace tesseract
