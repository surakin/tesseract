#include "app/ShellBase.h"
#include "app/RoomWindowBase.h"
#include "app/SlashCommands.h"
#include "app/media_preview_policy.h"
#include "tk/blurhash.h"
#include "tk/host.h"
#include "tk/theme.h"
#include "views/EncryptionSetupOverlay.h"
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

void ShellBase::debounce_(DebounceSlot slot, int ms, std::function<void()> fn)
{
    const int key = static_cast<int>(slot);
    const std::uint64_t gen = ++debounce_gen_[key];
    post_to_ui_after_(ms,
                      [this, key, gen, fn = std::move(fn)]()
                      {
                          // Honour only the most recent schedule on this slot;
                          // earlier pending fires were superseded.
                          auto it = debounce_gen_.find(key);
                          if (it != debounce_gen_.end() && it->second == gen)
                          {
                              fn();
                          }
                      });
}

void ShellBase::cancel_debounce_(DebounceSlot slot)
{
    ++debounce_gen_[static_cast<int>(slot)];
}

void ShellBase::populate_pending_restore_popouts_()
{
    if (!pending_restore_popouts_.empty())
        return;
    for (const auto& e : Settings::instance().popout_windows)
    {
        if (e.room_id.empty())
            continue;
        // Skip entries that explicitly belong to a different account.
        // An empty user_id means the entry predates the multi-account field
        // (pre-migration data) and should be restored for the active account.
        if (!e.user_id.empty() && active_account_ &&
            e.user_id != active_account_->user_id)
            continue;
        pending_restore_popouts_.push_back(e.room_id);
    }
}

void ShellBase::save_settings_debounced_()
{
    debounce_(DebounceSlot::SaveSettings, 500,
              []() {
                  tesseract::Settings::instance().save_to_disk(
                      tesseract::config_dir());
              });
}

Settings::WindowGeometry ShellBase::clamp_to_screens_(
    const Settings::WindowGeometry& saved,
    int default_w,
    int default_h,
    const std::vector<tk::Rect>& screens)
{
    if (!saved.valid)
        return {};

    // Check whether the title-bar strip overlaps any screen work area.
    const float kTitleH = 50.f;
    const float sx = static_cast<float>(saved.x);
    const float sy = static_cast<float>(saved.y);
    const float sw = static_cast<float>(saved.w);
    bool on_screen = false;
    for (const auto& s : screens)
    {
        const float ox = std::max(sx, s.x);
        const float oy = std::max(sy, s.y);
        const float ow = std::min(sx + sw, s.x + s.w) - ox;
        const float oh = std::min(sy + kTitleH, s.y + s.h) - oy;
        if (ow > 0.f && oh > 0.f)
        {
            on_screen = true;
            break;
        }
    }
    if (on_screen)
        return saved;

    // Re-centre on the first available screen with the saved size.
    const tk::Rect fallback{0.f, 0.f,
                            static_cast<float>(default_w),
                            static_cast<float>(default_h)};
    const tk::Rect& primary = screens.empty() ? fallback : screens[0];
    const int w = saved.w > 0
                      ? std::min(saved.w, static_cast<int>(primary.w * 0.9f))
                      : std::min(default_w, static_cast<int>(primary.w * 0.9f));
    const int h = saved.h > 0
                      ? std::min(saved.h, static_cast<int>(primary.h * 0.9f))
                      : std::min(default_h, static_cast<int>(primary.h * 0.9f));
    Settings::WindowGeometry result;
    result.x     = static_cast<int>(primary.x) + (static_cast<int>(primary.w) - w) / 2;
    result.y     = static_cast<int>(primary.y) + (static_cast<int>(primary.h) - h) / 2;
    result.w     = w;
    result.h     = h;
    result.valid = true;
    return result;
}

void ShellBase::show_status_message_(std::string msg, int auto_clear_ms)
{
    const std::uint32_t gen = ++status_msg_gen_;
    post_to_ui_alive_([this, msg = std::move(msg)] { on_show_status_message_ui_(msg); });
    post_to_ui_after_(auto_clear_ms,
                      [this, gen]
                      {
                          if (status_msg_gen_ == gen)
                              on_restore_status_ui_();
                      });
}

// ── WorkerPool ─────────────────────────────────────────────────────────────

ShellBase::WorkerPool::WorkerPool(int threads)
{
    for (int i = 0; i < threads; ++i)
    {
        threads_.emplace_back(
            [this]
            {
                for (;;)
                {
                    std::function<void()> task;
                    std::function<void()> notify;
                    {
                        std::unique_lock<std::mutex> lk(mu_);
                        cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                        if (stop_ && queue_.empty())
                            return;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                        pending_.fetch_sub(1, std::memory_order_relaxed);
                        notify = on_change_;
                    }
                    if (notify)
                        notify();
                    task();
                }
            });
    }
}

void ShellBase::WorkerPool::drain()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        // Disable change notifications before clearing the queue so no
        // spurious UI updates fire during or after shutdown.
        on_change_ = nullptr;
        // Clear the pending queue so threads don't start new work after the
        // stop flag is set — matching the previous shutting_down_ guard.
        queue_.clear();
        pending_.store(0, std::memory_order_relaxed);
    }
    cv_.notify_all();
    for (auto& t : threads_)
    {
        if (t.joinable())
            t.join();
    }
}

ShellBase::WorkerPool::~WorkerPool()
{
    drain();
}

void ShellBase::WorkerPool::post(std::function<void()> fn)
{
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_)
            return;
        pending_.fetch_add(1, std::memory_order_relaxed);
        queue_.push_back(std::move(fn));
        notify = on_change_;
    }
    cv_.notify_one();
    if (notify)
        notify();
}

void ShellBase::run_async_(std::function<void()> fn)
{
    pool_.post(std::move(fn));
}

void ShellBase::run_async_mut_(std::function<void()> fn)
{
    mut_pool_.post(std::move(fn));
}

void ShellBase::init_pool_callbacks_()
{
    auto notify = [this] { post_to_ui_alive_([this] { on_inflight_ui_(); }); };
    {
        std::lock_guard<std::mutex> lk(pool_.mu_);
        pool_.on_change_ = notify;
    }
    {
        std::lock_guard<std::mutex> lk(mut_pool_.mu_);
        mut_pool_.on_change_ = notify;
    }
}

void ShellBase::fetch_media_pipeline_(
    std::string cache_key, std::string disk_key, std::string inflight_key,
    std::uint64_t group_id, tesseract::Client::MediaReqKind kind,
    std::string source, std::uint32_t w, std::uint32_t h, bool animated,
    MediaKind out_kind)
{
    run_async_(
        [this, cache_key, disk_key, inflight_key, group_id, kind, source, w, h,
         animated, out_kind]() mutable
        {
            // io pool: only the (fast, local) disk-cache read happens here.
            auto disk = account_manager_.media_disk_cache().load(disk_key);
            post_to_ui_alive_(
                [this, cache_key, disk_key, inflight_key, group_id, kind,
                 source, w, h, animated, out_kind,
                 disk = std::move(disk)]() mutable
                {
                    if (!disk.empty())
                    {
                        media_fetches_in_flight_.erase(inflight_key);
                        note_media_fetch_ok_(cache_key);
                        on_media_bytes_ready_(cache_key, out_kind,
                                              std::move(disk));
                        return;
                    }
                    if (!client_)
                    {
                        media_fetches_in_flight_.erase(inflight_key);
                        return;
                    }
                    // The room may have been switched away during the disk-read
                    // hop above. The pending_media_ entry (and its cancel) only
                    // exists after this point, so cancel_media_group_ couldn't
                    // have caught it yet — suppress the now-stale download here.
                    if (group_id != 0 && group_id != active_media_group_)
                    {
                        media_fetches_in_flight_.erase(inflight_key);
                        return;
                    }
                    // Disk miss → non-blocking network fetch. The completion
                    // runs on the UI thread via on_media_ready → pending_media_.
                    auto id = begin_media_req_(
                        group_id,
                        [this, cache_key, disk_key, inflight_key, out_kind](
                            std::vector<std::uint8_t>&& net)
                        {
                            media_fetches_in_flight_.erase(inflight_key);
                            if (net.empty())
                            {
                                note_media_fetch_failed_(cache_key);
                                on_media_bytes_ready_(cache_key, out_kind, {});
                                return;
                            }
                            note_media_fetch_ok_(cache_key);
                            // Persist to disk off the UI thread, then deliver —
                            // the buffer moves through (no large-image copy).
                            run_async_(
                                [this, cache_key, disk_key, out_kind,
                                 net = std::move(net)]() mutable
                                {
                                    account_manager_.media_disk_cache().store(disk_key, net);
                                    post_to_ui_alive_(
                                        [this, cache_key, out_kind,
                                         net = std::move(net)]() mutable
                                        {
                                            on_media_bytes_ready_(
                                                cache_key, out_kind,
                                                std::move(net));
                                        });
                                });
                        },
                        // on_cancel (room switch): free the dedup key so a
                        // re-entry re-requests this media.
                        [this, inflight_key]
                        { media_fetches_in_flight_.erase(inflight_key); });
                    client_->fetch_media_async(id, group_id, kind, source, w, h,
                                               animated);
                });
        });
}

std::vector<std::uint8_t>
ShellBase::voice_bytes_or_fetch_(const std::string& token,
                                std::function<void()> on_ready)
{
    if (token.empty() || !client_)
        return {};
    // Warmed already → hand the bytes to the player and drop our copy.
    auto it = voice_bytes_cache_.find(token);
    if (it != voice_bytes_cache_.end())
    {
        auto bytes = std::move(it->second);
        voice_bytes_cache_.erase(it);
        return bytes;
    }
    // Cold → kick a one-shot non-blocking download (full source, bulk lane,
    // group 0). The UI stays responsive; the user replays once it lands.
    if (voice_bytes_in_flight_.insert(token).second)
    {
        auto id = begin_media_req_(
            /*group_id=*/0,
            [this, token, on_ready](std::vector<std::uint8_t>&& bytes)
            {
                voice_bytes_in_flight_.erase(token);
                if (!bytes.empty())
                {
                    // Normal use consumes each warmed clip on the next replay
                    // (move-out + erase above). Bound the cache so clips that
                    // are warmed but never replayed can't retain full audio
                    // files indefinitely — drop the lot if too many pile up.
                    constexpr std::size_t kVoiceWarmCacheMax = 8;
                    if (voice_bytes_cache_.size() >= kVoiceWarmCacheMax)
                        voice_bytes_cache_.clear();
                    voice_bytes_cache_.emplace(token, std::move(bytes));
                }
                if (on_ready)
                    on_ready();
            });
        client_->fetch_media_async(id, /*group_id=*/0,
                                   tesseract::Client::MediaReqKind::SourceFull,
                                   token, 0, 0, false);
    }
    return {};
}

void ShellBase::ensure_room_avatar_(const RoomInfo& r)
{
    // Must be called on the UI thread — accesses account_manager_.thumbnail_cache() and
    // media_fetches_in_flight_ without synchronization.
    const bool use_room_endpoint = !r.avatar_url.empty();
    const std::string mxc = use_room_endpoint ? r.avatar_url : r.dm_avatar_url;
    if (mxc.empty() || media_decode_failed_.count(mxc) ||
        media_fetch_backed_off_(mxc))
    {
        return;
    }
    // When the user opts into prefetching full media, warm account_manager_.image_cache() with
    // the full-size avatar so opening it in the viewer is instant. Idempotent.
    if (tesseract::Settings::instance().prefetch_full_media)
    {
        ensure_media_image_(mxc, 0, 0);
    }
    if (account_manager_.thumbnail_cache().contains(mxc))
    {
        return;
    }
    // Thumbnail and full-size fetches of the same mxc must not collide on the
    // disk cache or in the in-flight set — namespace the thumbnail keys.
    const std::string tkey =
        thumb_key(mxc, visual::kAvatarCacheSize, visual::kAvatarCacheSize);
    if (!media_fetches_in_flight_.insert(tkey).second)
    {
        return;
    }
    // DM fallback avatars are user mxcs, not room avatars, so route them
    // through the generic mxc-thumbnail endpoint (source = mxc) rather than the
    // room-avatar endpoint (source = room_id). Room-list avatars are not
    // room-scoped, so they use group 0 (never cancelled on room switch).
    const std::string source = use_room_endpoint ? r.id : mxc;
    const auto kind = use_room_endpoint
                          ? tesseract::Client::MediaReqKind::RoomAvatar
                          : tesseract::Client::MediaReqKind::MxcThumbnail;
    fetch_media_pipeline_(mxc, tkey, tkey, /*group_id=*/0, kind, source,
                          visual::kAvatarCacheSize, visual::kAvatarCacheSize,
                          /*animated=*/false, MediaKind::RoomAvatar);
}

void ShellBase::ensure_user_avatar_(const std::string& mxc)
{
    if (mxc.empty() || media_decode_failed_.count(mxc) ||
        media_fetch_backed_off_(mxc))
    {
        return;
    }
    if (tesseract::Settings::instance().prefetch_full_media)
    {
        ensure_media_image_(mxc, 0, 0);
    }
    if (account_manager_.thumbnail_cache().contains(mxc))
    {
        return;
    }
    const std::string tkey =
        thumb_key(mxc, visual::kAvatarCacheSize, visual::kAvatarCacheSize);
    if (!media_fetches_in_flight_.insert(tkey).second)
    {
        return;
    }
    // Avatars are small, shared across rooms, and cheap to re-fetch (disk
    // cached), so they are never cancelled on room switch → group 0.
    fetch_media_pipeline_(mxc, tkey, tkey, /*group_id=*/0,
                          tesseract::Client::MediaReqKind::MxcThumbnail, mxc,
                          visual::kAvatarCacheSize, visual::kAvatarCacheSize,
                          /*animated=*/false, MediaKind::UserAvatar);
}

void ShellBase::ensure_media_image_(const std::string& url, int /*max_w*/,
                                    int /*max_h*/, std::uint64_t group_id)
{
    if (url.empty() || account_manager_.image_cache().contains(url) || account_manager_.anim_cache().has(url) ||
        media_decode_failed_.count(url) || media_fetch_backed_off_(url))
    {
        return;
    }
    if (!media_fetches_in_flight_.insert(url).second)
    {
        return;
    }
    // Full-size source → bulk lane. group_id is the originating room (so a
    // switch cancels it) for timeline media, or 0 for avatar/preview prefetch.
    fetch_media_pipeline_(url, url, url, group_id,
                          tesseract::Client::MediaReqKind::SourceFull, url,
                          /*w=*/0, /*h=*/0, /*animated=*/false,
                          MediaKind::MediaImage);
}

const tk::Image* ShellBase::viewer_image_lookup_(const std::string& mxc)
{
    // Full-resolution lightbox decode wins when present.
    if (auto it = viewer_fullres_.find(mxc); it != viewer_fullres_.end())
    {
        return it->second.get();
    }
    // Otherwise the existing fallthrough: animated frame → inline full-size
    // image → server thumbnail.
    if (const auto* f = account_manager_.anim_cache().current_frame(mxc))
    {
        start_anim_tick_(); // visible animated frame → keep the timer running
        return f;
    }
    if (const auto* img = account_manager_.image_cache().peek(mxc))
    {
        return img;
    }
    return account_manager_.thumbnail_cache().peek(mxc);
}

void ShellBase::ensure_viewer_fullres_(const std::string& url)
{
    // Animated sources keep animating from account_manager_.anim_cache(); the viewer already
    // pulls frames from there, so we don't produce a full-res still for them —
    // just make sure the animated bytes are fetched into account_manager_.anim_cache().
    if (account_manager_.anim_cache().has(url))
    {
        ensure_media_image_(url, 0, 0);
        return;
    }
    const std::string fkey = fullres_key_(url);
    if (url.empty() || viewer_fullres_.count(url) ||
        media_decode_failed_.count(fkey) || media_fetch_backed_off_(fkey))
    {
        return;
    }
    if (!viewer_fullres_in_flight_.insert(fkey).second)
    {
        return;
    }
    if (!client_)
    {
        viewer_fullres_in_flight_.erase(fkey);
        return;
    }
    // io pool: read the namespaced disk cache first. On a miss, issue a
    // non-blocking SourceFull download (bulk lane, group 0 so a room switch
    // does not cancel an open lightbox). Decode at the large viewer bound OFF
    // the UI thread, then store + relayout everywhere.
    run_async_(
        [this, url, fkey]() mutable
        {
            auto disk = account_manager_.media_disk_cache().load(fkey);
            post_to_ui_alive_(
                [this, url, fkey, disk = std::move(disk)]() mutable
                {
                    if (!disk.empty())
                    {
                        decode_fullres_and_store_(url, fkey, std::move(disk),
                                                  /*persist=*/false);
                        return;
                    }
                    if (!client_)
                    {
                        viewer_fullres_in_flight_.erase(fkey);
                        return;
                    }
                    auto id = begin_media_req_(
                        /*group_id=*/0,
                        [this, url, fkey](std::vector<std::uint8_t>&& net)
                        {
                            if (net.empty())
                            {
                                viewer_fullres_in_flight_.erase(fkey);
                                note_media_fetch_failed_(fkey);
                                return;
                            }
                            note_media_fetch_ok_(fkey);
                            decode_fullres_and_store_(url, fkey, std::move(net),
                                                      /*persist=*/true);
                        },
                        // on_cancel: free the dedup key so a re-open re-requests.
                        [this, fkey] { viewer_fullres_in_flight_.erase(fkey); });
                    client_->fetch_media_async(
                        id, /*group_id=*/0,
                        tesseract::Client::MediaReqKind::SourceFull, url,
                        /*w=*/0, /*h=*/0, /*animated=*/false);
                });
        });
}

void ShellBase::decode_fullres_and_store_(std::string url, std::string fkey,
                                          std::vector<std::uint8_t> bytes,
                                          bool persist)
{
    run_async_(
        [this, url, fkey, persist, bytes = std::move(bytes)]() mutable
        {
            if (persist)
            {
                account_manager_.media_disk_cache().store(fkey, bytes);
            }
            // DecodedImage is move-only (holds unique_ptr<tk::Image>); wrap in a
            // shared_ptr so the post_to_ui_ std::function lambda stays
            // copy-constructible (mirrors decode_and_finalize_picker_).
            auto d = std::make_shared<DecodedImage>(decode_image_(
                bytes, visual::kViewerFullresMax, visual::kViewerFullresMax));
            if (d->empty() && persist)
            {
                account_manager_.media_disk_cache().evict(fkey);
            }
            post_to_ui_alive_(
                [this, url, fkey, d]() mutable
                {
                    viewer_fullres_in_flight_.erase(fkey);
                    if (viewer_fullres_.count(url))
                    {
                        return;
                    }
                    // An unexpectedly-animated decode → defer to the anim path.
                    if (!d->frames.empty())
                    {
                        ensure_media_image_(url, 0, 0);
                        return;
                    }
                    if (!d->still)
                    {
                        media_decode_failed_.insert(fkey);
                        return;
                    }
                    // FIFO-evict if at cap (never evict the entry we're adding).
                    while (viewer_fullres_.size() >= kViewerFullresCacheMax_ &&
                           !viewer_fullres_order_.empty())
                    {
                        const std::string victim = viewer_fullres_order_.front();
                        viewer_fullres_order_.erase(viewer_fullres_order_.begin());
                        if (victim != url)
                        {
                            viewer_fullres_.erase(victim);
                            break;
                        }
                    }
                    viewer_fullres_.emplace(url, std::move(d->still));
                    viewer_fullres_order_.push_back(url);
                    // Relayout the main surface (its viewer re-fits the larger
                    // image in arrange and polls the provider) and every pop-out
                    // (notify_image_ready + relayout).
                    request_relayout_();
                    notify_secondary_media_ready_(url, MediaKind::MediaImage);
                });
        });
}

void ShellBase::ensure_media_thumbnail_(const std::string& url, int w, int h,
                                        bool animated, std::uint64_t group_id)
{
    if (url.empty() || account_manager_.image_cache().contains(url) ||
        account_manager_.thumbnail_cache().contains(url) || account_manager_.anim_cache().has(url) ||
        media_decode_failed_.count(url) || media_fetch_backed_off_(url))
    {
        return;
    }
    const std::string tkey = thumb_key(url, w, h);
    if (!media_fetches_in_flight_.insert(tkey).second)
    {
        return;
    }
    fetch_media_pipeline_(url, tkey, tkey, group_id,
                          tesseract::Client::MediaReqKind::SourceThumb, url,
                          static_cast<std::uint32_t>(w),
                          static_cast<std::uint32_t>(h), animated,
                          MediaKind::MediaThumbnail);
}

const tk::Image* ShellBase::shell_sticker_(const std::string& mxc)
{
    if (const auto* f = account_manager_.anim_cache().current_frame(mxc))
    {
        start_anim_tick_(); // visible animated frame → keep the timer running
        return f;
    }
    if (const auto* img = account_manager_.image_cache().peek(mxc))
    {
        return img;
    }
    ensure_media_image_(mxc, 64, 64);
    return nullptr;
}

void ShellBase::set_room_notification_mode_(const std::string& room_id,
                                             const std::string& mode)
{
    if (!client_) return;
    auto sess = active_account_;
    run_async_mut_([sess, room_id, mode]() {
        if (!sess || !sess->client) return;
        sess->client->set_room_notification_mode(room_id, mode);
    });
}

void ShellBase::set_room_favourite_(const std::string& room_id, bool value)
{
    if (!client_) return;
    auto sess = active_account_;
    run_async_mut_([sess, room_id, value]() {
        if (!sess || !sess->client) return;
        sess->client->set_room_favourite(room_id, value);
    });
}

void ShellBase::set_room_low_priority_(const std::string& room_id, bool value)
{
    if (!client_) return;
    auto sess = active_account_;
    run_async_mut_([sess, room_id, value]() {
        if (!sess || !sess->client) return;
        sess->client->set_room_low_priority(room_id, value);
    });
}

void ShellBase::wire_main_app_widget_(views::MainAppWidget* app)
{
    auto avatar_lookup = [this](const std::string& mxc) -> const tk::Image*
    { return account_manager_.thumbnail_cache().peek(mxc); };

    app->set_avatar_provider(avatar_lookup);
    app->room_list_view()->set_avatar_provider(avatar_lookup);
    // Lazy avatar fetching: the provider above is a pure cache peek, so the
    // room list requests an avatar only when a row is first painted (visible).
    // Avatars for rooms in collapsed / off-screen sections are never fetched.
    app->room_list_view()->on_room_avatar_needed =
        [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };

    // Quick switcher (Ctrl+K): data + activation are shared. The native search
    // field, the keyboard accelerator, and on_close stay per-shell.
    if (auto* qs = app->quick_switcher())
    {
        qs->set_rooms_provider(
            [this]() -> std::vector<tesseract::RoomInfo>
            {
                auto v = rooms_;
                std::sort(v.begin(), v.end(),
                          [](const tesseract::RoomInfo& a,
                             const tesseract::RoomInfo& b)
                          { return a.last_activity_ts > b.last_activity_ts; });
                return v;
            });
        qs->set_recent_provider(
            [this]() -> std::vector<tesseract::RoomInfo>
            {
                std::vector<tesseract::RoomInfo> out;
                out.reserve(recent_room_ids_.size());
                for (const auto& id : recent_room_ids_)
                {
                    auto it = std::find_if(
                        rooms_.begin(), rooms_.end(),
                        [&](const tesseract::RoomInfo& r) { return r.id == id; });
                    if (it != rooms_.end())
                        out.push_back(*it);
                }
                return out;
            });
        qs->on_room_avatar_needed =
            [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
        qs->on_room_selected =
            [this](const std::string& room_id) { tab_select_room(room_id); };
    }

    app->room_list_view()->set_sticker_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            return shell_sticker_(mxc);
        });

    // Restore section collapsed state from the previous session.
    {
        auto& s = tesseract::Settings::instance();
        const bool init[views::RoomListView::kNumSections] = {
            s.room_section_invites_collapsed,
            s.room_section_favorites_collapsed,
            s.room_section_dms_collapsed,
            s.room_section_rooms_collapsed,
            s.room_section_spaces_collapsed,
            s.room_section_inactive_collapsed,
        };
        for (int sec = 0; sec < views::RoomListView::kNumSections; ++sec)
            app->room_list_view()->set_section_collapsed(sec, init[sec]);
    }
    app->room_list_view()->on_section_toggled =
        [](int section, bool collapsed)
    {
        auto& s = tesseract::Settings::instance();
        switch (section)
        {
        case views::RoomListView::kSecInvites:
            s.room_section_invites_collapsed   = collapsed; break;
        case views::RoomListView::kSecFavorites:
            s.room_section_favorites_collapsed = collapsed; break;
        case views::RoomListView::kSecDMs:
            s.room_section_dms_collapsed       = collapsed; break;
        case views::RoomListView::kSecRooms:
            s.room_section_rooms_collapsed     = collapsed; break;
        case views::RoomListView::kSecSpaces:
            s.room_section_spaces_collapsed    = collapsed; break;
        case views::RoomListView::kSecInactive:
            s.room_section_inactive_collapsed  = collapsed; break;
        default: break;
        }
        s.save_to_disk(tesseract::config_dir());
    };

    app->room_view()->set_avatar_provider(avatar_lookup);
    app->room_view()->on_room_avatar_needed =
        [this](const tesseract::RoomInfo& r) { ensure_room_avatar_(r); };
    app->room_view()->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image*
        {
            if (const auto* f = account_manager_.anim_cache().current_frame(mxc))
            {
                start_anim_tick_();
                return f;
            }
            if (const auto* img = account_manager_.image_cache().peek(mxc))
                return img;
            if (const auto* img = account_manager_.thumbnail_cache().peek(mxc))
                return img;
            // Cache miss after eviction — re-fetch. Deduplicated by the
            // in-flight set; uses the disk cache when bytes were previously
            // downloaded, so re-display is usually instant.
            ensure_media_image_(mxc, visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight,
                                media_group_for_room_(current_room_id_));
            return nullptr;
        });
    // MSC4278: gate inline media behind the media-preview config + reveal set.
    wire_media_preview_gating_(app->room_view()->message_list());
    // Whole-room pinning: message rows hold an ImageRef from the cache so the
    // images they display are never evicted while the room is open.
    app->room_view()->set_image_acquirer(
        [this](const std::string& mxc) -> tk::ImageRef
        {
            if (auto ref = account_manager_.image_cache().acquire(mxc))
                return ref;
            return account_manager_.thumbnail_cache().acquire(mxc);
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
                !account_manager_.image_cache().contains(it->second.image_mxc) &&
                !account_manager_.anim_cache().has(it->second.image_mxc))
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

    // Avatar click in UserProfilePanel → open the image viewer. The list only
    // holds an ≤80px thumbnail, so kick a full-size fetch into account_manager_.image_cache();
    // the viewer's image_provider returns the thumbnail instantly and swaps to
    // full-res when it arrives.
    app->room_view()->on_avatar_clicked =
        [this, app](std::string url, std::string name)
    {
        if (url.empty() || !app->image_viewer())
            return;
        ensure_viewer_fullres_(url);
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
        auto sess = active_account_;
        run_async_([this, app, sess, room_id = std::move(room_id)]() mutable {
            if (!sess || !sess->client) return;
            auto mode = sess->client->get_room_notification_mode(room_id);
            post_to_ui_alive_([app, mode = std::move(mode)]() mutable {
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
    app->room_view()->on_favourite_changed =
        [this](std::string room_id, bool on)
    {
        set_room_favourite_(room_id, on);
    };
    app->room_view()->on_low_priority_changed =
        [this](std::string room_id, bool on)
    {
        set_room_low_priority_(room_id, on);
    };

    // ── Invite selection and action wiring ────────────────────────────────
    app->room_list_view()->on_invite_selected =
        [this, app, avatar_lookup](const std::string& room_id)
    {
        const InviteInfo* inv = find_invite_(room_id);
        if (!inv)
            return;
        current_invite_ = InviteContext{ inv->room_id, inv->inviter_user_id };
        app->show_invite(*inv, avatar_lookup);
        app->room_list_view()->set_selected_room(""); // clear room highlight
        request_relayout_();
    };

    app->invite_card()->on_accept = [this]
    {
        if (current_invite_) accept_invite_async_(current_invite_->room_id);
    };
    app->invite_card()->on_decline = [this]
    {
        if (current_invite_) decline_invite_async_(current_invite_->room_id);
        if (main_app_)
            main_app_->clear_content();
        request_relayout_();
    };
    app->invite_card()->on_block = [this]
    {
        if (current_invite_)
            block_invite_async_(current_invite_->room_id, current_invite_->inviter_id);
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
    // viewer_image_lookup_ consults the full-res lightbox cache first, then
    // falls through account_manager_.anim_cache() → account_manager_.image_cache() → account_manager_.thumbnail_cache(), so the viewer
    // shows the full-res image once the avatar/image click has fetched it and
    // the inline thumbnail until then.
    auto image_lookup = [this](const std::string& mxc) -> const tk::Image*
    {
        return viewer_image_lookup_(mxc);
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

void ShellBase::decode_and_finalize_picker_(std::string url, bool is_sticker,
                                            std::vector<std::uint8_t> bytes,
                                            bool persist)
{
    // Decode OFF the UI thread. Picker cells are bounded; reuse the inline-image
    // bound so picker bitmaps are reusable by the message list (same shared
    // tk_images_ key = the mxc url). DecodedImage is move-only (holds
    // unique_ptr<tk::Image>); wrap it in a shared_ptr so the post_to_ui_ lambda
    // is copy-constructible (post_to_ui_ takes std::function).
    run_async_(
        [this, url, is_sticker, persist, bytes = std::move(bytes)]() mutable
        {
            if (persist)
            {
                account_manager_.media_disk_cache().store(url, bytes);
            }
            auto d = std::make_shared<DecodedImage>(
                decode_image_(bytes, visual::kMaxInlineImageWidth,
                              visual::kMaxInlineImageHeight));
            if (d->empty())
            {
                account_manager_.media_disk_cache().evict(url);
            }
            post_to_ui_alive_(
                [this, url, is_sticker, d]() mutable
                {
                    finalize_picker_image_(url, is_sticker, std::move(*d));
                });
        });
}

void ShellBase::ensure_picker_image_(const std::string& url, bool is_sticker)
{
    if (url.empty() || account_manager_.image_cache().contains(url) || account_manager_.anim_cache().has(url))
    {
        return;
    }
    auto& inflight =
        is_sticker ? sticker_fetches_in_flight_ : emoji_fetches_in_flight_;
    if (!inflight.insert(url).second)
    {
        return;
    }
    // io pool: read the disk cache. On a hit, decode+finalize directly. On a
    // miss, issue a non-blocking network download (bulk lane, group 0 — picker
    // images aren't room-scoped) and decode+finalize on completion.
    run_async_(
        [this, url, is_sticker]() mutable
        {
            auto disk = account_manager_.media_disk_cache().load(url);
            post_to_ui_alive_(
                [this, url, is_sticker, disk = std::move(disk)]() mutable
                {
                    if (!disk.empty())
                    {
                        decode_and_finalize_picker_(url, is_sticker,
                                                    std::move(disk),
                                                    /*persist=*/false);
                        return;
                    }
                    if (!client_)
                    {
                        (is_sticker ? sticker_fetches_in_flight_
                                    : emoji_fetches_in_flight_)
                            .erase(url);
                        return;
                    }
                    auto id = begin_media_req_(
                        /*group_id=*/0,
                        [this, url, is_sticker](std::vector<std::uint8_t>&& net)
                        {
                            if (net.empty())
                            {
                                (is_sticker ? sticker_fetches_in_flight_
                                            : emoji_fetches_in_flight_)
                                    .erase(url);
                                return;
                            }
                            decode_and_finalize_picker_(url, is_sticker,
                                                        std::move(net),
                                                        /*persist=*/true);
                        });
                    client_->fetch_media_async(
                        id, /*group_id=*/0,
                        tesseract::Client::MediaReqKind::SourceFull, url, 0, 0,
                        false);
                });
        });
}

void ShellBase::finalize_picker_image_(std::string url, bool is_sticker,
                                       DecodedImage d)
{
    (is_sticker ? sticker_fetches_in_flight_ : emoji_fetches_in_flight_)
        .erase(url);
    if (account_manager_.image_cache().contains(url) || account_manager_.anim_cache().has(url))
    {
        return;
    }
    if (!d.frames.empty())
    {
        account_manager_.anim_cache().store(url, std::move(d.frames), std::move(d.delays_ms),
                          monotonic_ms_());
        start_anim_tick_();
    }
    else if (d.still)
    {
        account_manager_.image_cache().store(url, std::move(d.still));
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
    if (account_manager_.image_cache().contains(key) || tile_fetch_failed_.count(key))
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

    // io pool: read the on-disk tile cache. On a miss, fall back to a
    // non-blocking network fetch (fetch_url_async) so the 30 s tile timeout
    // never pins a pool thread. Tiles are not room-scoped → group 0.
    run_async_(
        [this, key, url, disk_path]()
        {
            std::vector<uint8_t> bytes;
            if (std::filesystem::exists(disk_path))
            {
                std::ifstream f(disk_path, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(f), {});
            }
            post_to_ui_alive_(
                [this, key, url, disk_path, bytes = std::move(bytes)]() mutable
                {
                    if (!bytes.empty())
                    {
                        tile_fetches_in_flight_.erase(key);
                        on_media_bytes_ready_(key, MediaKind::Tile,
                                              std::move(bytes));
                        return;
                    }
                    if (!client_)
                    {
                        tile_fetches_in_flight_.erase(key);
                        return;
                    }
                    auto id = begin_media_req_(
                        /*group_id=*/0,
                        [this, key, disk_path](std::vector<std::uint8_t>&& net)
                        {
                            tile_fetches_in_flight_.erase(key);
                            if (net.empty())
                            {
                                tile_fetch_failed_.insert(key);
                                return;
                            }
                            // Persist to disk off the UI thread, then deliver.
                            run_async_(
                                [this, key, disk_path,
                                 net = std::move(net)]() mutable
                                {
                                    std::error_code ec;
                                    std::filesystem::create_directories(
                                        disk_path.parent_path(), ec);
                                    if (!ec)
                                    {
                                        std::ofstream f(disk_path,
                                                        std::ios::binary);
                                        f.write(reinterpret_cast<const char*>(
                                                    net.data()),
                                                static_cast<std::streamsize>(
                                                    net.size()));
                                    }
                                    post_to_ui_alive_(
                                        [this, key,
                                         net = std::move(net)]() mutable
                                        {
                                            on_media_bytes_ready_(
                                                key, MediaKind::Tile,
                                                std::move(net));
                                        });
                                });
                        },
                        [this, key] { tile_fetches_in_flight_.erase(key); });
                    client_->fetch_url_async(id, /*group_id=*/0, url);
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
    // URL previews can be slow (dead OpenGraph servers, 30 s timeout) and are
    // per-message in a room, so group them under the room and cancel on switch.
    const std::uint64_t group = media_group_for_room_(current_room_id_);
    auto id = begin_url_preview_req_(
        group,
        [this, url](std::string&& json)
        {
            url_preview_in_flight_.erase(url);
            url_previews_.emplace(url,
                                  tesseract::Client::parse_url_preview(json));
            if (!url_previews_.at(url).failed)
                on_url_preview_ready_(url, url_previews_.at(url));
            else
                on_url_preview_failed_(url);
        },
        [this, url] { url_preview_in_flight_.erase(url); });
    client_->get_url_preview_async(id, group, url);
}

void ShellBase::ensure_blurhash_image_(const std::string& event_id,
                                       const std::string& hash, int media_w,
                                       int media_h)
{
    const std::string key = "blurhash::" + event_id;
    if (account_manager_.image_cache().contains(key) || !blurhash_attempted_.insert(key).second)
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
                account_manager_.media_disk_cache().prune();
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

    // MSC4278: gate media (image/sticker/video thumbnails + URL previews)
    // behind the media-preview config. A suppressed item is not fetched until
    // the user reveals it individually. Sender avatars, reactions, voice/audio,
    // and the BlurHash placeholder are not gated.
    const std::string& gate_room =
        ev.room_id.empty() ? current_room_id_ : ev.room_id;
    // The user's own media is exempt from public-room suppression (Private
    // mode), so it is fetched here just like revealed media — otherwise the
    // placeholder would be gone but the bytes never fetched.
    const bool preview =
        media_allowed_(gate_room, !my_user_id_.empty() &&
                                      ev.sender == my_user_id_) ||
        revealed_events_.count(ev.event_id) != 0;
    // Inline media (image/sticker/video) is large, slow, and room-specific, so
    // it is grouped under the originating room and cancelled when the user
    // switches away — see cancel_media_group_ in after_active_room_changed_.
    const std::uint64_t media_group = media_group_for_room_(gate_room);

    if (ev.type == EventType::Image)
    {
        const auto& img = static_cast<const ImageEvent&>(ev);
        if (preview && img.thumbnail)
        {
            // animated=true so capable servers keep animated GIFs moving.
            ensure_media_thumbnail_(img.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, true,
                                    media_group);
        }
        if (preview &&
            (!img.thumbnail || tesseract::Settings::instance().prefetch_full_media))
        {
            if (img.source)
                ensure_media_image_(img.source->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, media_group);
        }
    }
    else if (ev.type == EventType::Sticker)
    {
        const auto& s = static_cast<const StickerEvent&>(ev);
        if (preview && s.thumbnail)
        {
            ensure_media_image_(s.thumbnail->fetch_token(),
                                visual::kStickerSize, visual::kStickerSize,
                                media_group);
        }
        if (preview &&
            (!s.thumbnail || tesseract::Settings::instance().prefetch_full_media))
        {
            if (s.source)
                ensure_media_image_(s.source->fetch_token(),
                                    visual::kStickerSize, visual::kStickerSize,
                                    media_group);
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
                // Non-blocking full-source download (bulk lane). The Opus decode
                // for the waveform is CPU work, so it runs on the io pool inside
                // the completion — never on the UI thread. group 0: voice isn't
                // part of the room-switch flood and its dedup markers are
                // permanent, so it is not cancelled on switch.
                auto id = begin_media_req_(
                    /*group_id=*/0,
                    [this, src, event_id, room_id,
                     waveform_new](std::vector<std::uint8_t>&& bytes)
                    {
                        if (!waveform_new || bytes.empty())
                            return; // audio cache warmed; nothing more to do.
                        run_async_(
                            [this, src, event_id, room_id,
                             bytes = std::move(bytes)]() mutable
                            {
                                auto waveform =
                                    tesseract::load_voice_waveform(src);
                                if (waveform.empty())
                                {
                                    waveform =
                                        tesseract::compute_waveform_from_ogg(
                                            bytes);
                                    if (!waveform.empty())
                                        tesseract::store_voice_waveform(
                                            src, waveform);
                                }
                                if (waveform.empty())
                                    return;
                                post_to_ui_alive_(
                                    [this, room_id, event_id,
                                     waveform = std::move(waveform)]() mutable
                                    {
                                        handle_voice_waveform_ready_ui_(
                                            room_id, event_id,
                                            std::move(waveform));
                                    });
                            });
                    });
                client_->fetch_media_async(
                    id, /*group_id=*/0,
                    tesseract::Client::MediaReqKind::SourceFull, src, 0, 0,
                    false);
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
            {
                // Warm the SDK media cache without pinning a thread; discard
                // the bytes (playback re-reads from the warmed cache).
                auto id = begin_media_req_(
                    /*group_id=*/0, [](std::vector<std::uint8_t>&&) {});
                client_->fetch_media_async(
                    id, /*group_id=*/0,
                    tesseract::Client::MediaReqKind::SourceFull, src, 0, 0,
                    false);
            }
        }
    }
    else if (ev.type == EventType::Video)
    {
        const auto& vid = static_cast<const VideoEvent&>(ev);
        if (preview && vid.thumbnail)
        {
            ensure_media_thumbnail_(vid.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, false,
                                    media_group);
        }
        if (preview && !vid.thumbnail && vid.source &&
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

    if (preview && (ev.type == EventType::Text || ev.type == EventType::Unhandled))
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

namespace
{
// Resolve a MediaPreviewConfig::Mode → the Settings mirror enum (identical
// order, but kept explicit so the two stay decoupled).
tesseract::Settings::MediaPreviews
mode_to_settings_(tesseract::MediaPreviewConfig::Mode m)
{
    switch (m)
    {
    case tesseract::MediaPreviewConfig::Mode::Off:
        return tesseract::Settings::MediaPreviews::Off;
    case tesseract::MediaPreviewConfig::Mode::Private:
        return tesseract::Settings::MediaPreviews::Private;
    case tesseract::MediaPreviewConfig::Mode::On:
    default:
        return tesseract::Settings::MediaPreviews::On;
    }
}
} // namespace

tesseract::Settings::MediaPreviews
ShellBase::effective_preview_mode_(const std::string& room_id,
                                   std::string& join_rule_out) const
{
    tesseract::Settings::MediaPreviews mode =
        tesseract::Settings::instance().media_previews;
    join_rule_out.clear();

    auto it = room_preview_overrides_.find(room_id);
    if (it != room_preview_overrides_.end())
    {
        join_rule_out = it->second.join_rule;
        if (it->second.has_media_previews)
        {
            mode = mode_to_settings_(it->second.media_previews);
        }
    }
    return mode;
}

bool ShellBase::should_auto_preview_(const std::string& room_id) const
{
    // Room-level gate (no per-message ownership): equivalent to media_allowed_
    // for someone else's media. Used by the bulk re-fetch loops.
    std::string join_rule;
    auto mode = effective_preview_mode_(room_id, join_rule);
    return tesseract::app::media_allowed(mode, join_rule, /*is_own=*/false,
                                         /*revealed=*/false);
}

bool ShellBase::media_allowed_(const std::string& room_id, bool is_own) const
{
    std::string join_rule;
    auto mode = effective_preview_mode_(room_id, join_rule);
    return tesseract::app::media_allowed(mode, join_rule, is_own,
                                         /*revealed=*/false);
}

bool ShellBase::media_preview_hidden_(const std::string& room_id,
                                      const std::string& event_id,
                                      bool is_own) const
{
    if (revealed_events_.count(event_id) != 0)
    {
        return false;
    }
    return !media_allowed_(room_id, is_own);
}

void ShellBase::ensure_room_preview_override_(const std::string& room_id)
{
    if (!client_ || room_id.empty())
    {
        return;
    }
    if (room_preview_overrides_.count(room_id) != 0)
    {
        return;
    }
    if (!room_preview_override_in_flight_.insert(room_id).second)
    {
        return;
    }
    auto sess = active_account_;
    run_async_mut_(
        [this, sess, room_id]()
        {
            if (!sess || !sess->client) return;
            auto ov = sess->client->room_media_preview_override(room_id);
            post_to_ui_alive_(
                [this, room_id, ov = std::move(ov)]() mutable
                {
                    room_preview_override_in_flight_.erase(room_id);
                    room_preview_overrides_[room_id] = std::move(ov);
                    // Now that the join rule + override are known, fetch any
                    // media that turned out to be allowed in this room and
                    // re-evaluate the placeholders.
                    if (room_view_ && room_id == current_room_id_ &&
                        should_auto_preview_(room_id))
                    {
                        if (auto* ml = room_view_->message_list())
                        {
                            for (const auto& row : ml->messages())
                            {
                                reveal_media_fetch_(row);
                            }
                        }
                    }
                    request_relayout_();
                });
        });
}

void ShellBase::reveal_media_fetch_(const views::MessageRowData& row)
{
    using K = views::MessageRowData::Kind;
    // Reveal is always for the active room's rows → group under it so a switch
    // cancels any still-loading revealed media.
    const std::uint64_t media_group = media_group_for_room_(current_room_id_);
    if (row.kind == K::Image)
    {
        if (row.thumbnail)
            ensure_media_thumbnail_(row.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, true,
                                    media_group);
        else if (row.source)
            ensure_media_image_(row.source->fetch_token(),
                                visual::kMaxInlineImageWidth,
                                visual::kMaxInlineImageHeight, media_group);
    }
    else if (row.kind == K::Sticker)
    {
        if (row.thumbnail)
            ensure_media_image_(row.thumbnail->fetch_token(),
                                visual::kStickerSize, visual::kStickerSize,
                                media_group);
        else if (row.source)
            ensure_media_image_(row.source->fetch_token(),
                                visual::kStickerSize, visual::kStickerSize,
                                media_group);
    }
    else if (row.kind == K::Video)
    {
        if (row.thumbnail)
            ensure_media_thumbnail_(row.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, false,
                                    media_group);
        else if (row.source &&
                 video_thumb_in_flight_.insert(row.event_id).second)
            generate_video_thumbnail_(row.event_id, row.source->fetch_token());
    }
}

void ShellBase::wire_media_preview_gating_(views::MessageListView* ml)
{
    if (!ml)
    {
        return;
    }
    ml->set_media_hidden_predicate(
        [this](const std::string& event_id, bool is_own)
        { return media_preview_hidden_(current_room_id_, event_id, is_own); });
    ml->on_reveal_media = [this, ml](const std::string& event_id)
    {
        revealed_events_.insert(event_id);
        for (const auto& row : ml->messages())
        {
            if (row.event_id == event_id)
            {
                reveal_media_fetch_(row);
                break;
            }
        }
        request_relayout_();
    };
}

void ShellBase::apply_media_preview_config_(
    tesseract::Settings::MediaPreviews mode, bool invite_avatars)
{
    auto& s = tesseract::Settings::instance();
    s.media_previews = mode;
    s.invite_avatars = invite_avatars;

    if (client_)
    {
        tesseract::MediaPreviewConfig::Mode m =
            tesseract::MediaPreviewConfig::Mode::On;
        switch (mode)
        {
        case tesseract::Settings::MediaPreviews::Off:
            m = tesseract::MediaPreviewConfig::Mode::Off;
            break;
        case tesseract::Settings::MediaPreviews::Private:
            m = tesseract::MediaPreviewConfig::Mode::Private;
            break;
        case tesseract::Settings::MediaPreviews::On:
            m = tesseract::MediaPreviewConfig::Mode::On;
            break;
        }
        client_->save_media_preview_config(m, invite_avatars);
    }

    // Fetch media that just became allowed in the open room.
    if (room_view_ && should_auto_preview_(current_room_id_))
    {
        if (auto* ml = room_view_->message_list())
        {
            for (const auto& row : ml->messages())
            {
                reveal_media_fetch_(row);
            }
        }
    }
    if (invite_avatars)
    {
        ensure_invite_avatars_();
    }
    request_relayout_();
}

void ShellBase::handle_media_preview_config_updated_ui_(std::string user_id,
                                                        std::string /*json*/)
{
    // Only the active account's config drives the UI.
    if (!active_account_ || !client_ ||
        active_account_->user_id != user_id)
    {
        return;
    }
    auto cfg = client_->media_preview_config();
    auto& s = tesseract::Settings::instance();
    s.media_previews = mode_to_settings_(cfg.media_previews);
    s.invite_avatars = cfg.invite_avatars;

    // Fetch media that just became allowed in the open room.
    if (room_view_ && should_auto_preview_(current_room_id_))
    {
        if (auto* ml = room_view_->message_list())
        {
            for (const auto& row : ml->messages())
            {
                reveal_media_fetch_(row);
            }
        }
    }
    // Fetch invite avatars that just became allowed.
    if (s.invite_avatars)
    {
        ensure_invite_avatars_();
    }
    request_relayout_();
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
    // Refresh the pinned-events banner from the now-updated cache. Picks up
    // both pin/unpin state-event changes and PL changes that flip can_pin.
    refresh_pinned_for_current_room_();

    // When inactive grouping is enabled, ensure every room (not just the
    // visible slice) has its last_activity_ts populated so the inactive
    // section can classify all rooms correctly. Idempotent: Rust skips rooms
    // already in backfill_ts and returns immediately if a task is running.
    if (client_ && tesseract::Settings::instance().group_inactive_rooms)
        client_->start_background_backfill_all_uncached();

    // One-time encryption setup check — raises the overlay on the first
    // eligible sync tick after login (Disabled → Fresh, Incomplete → Recover).
    check_encryption_setup_();

    // Replay a matrix link that arrived before we were logged in.
    if (!pending_matrix_link_.empty())
    {
        auto uri = std::move(pending_matrix_link_);
        pending_matrix_link_.clear();
        open_matrix_link(uri);
    }
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
    // MSC4278: don't fetch invite avatars the UI won't show.
    if (!tesseract::Settings::instance().invite_avatars)
    {
        return;
    }
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
        return;
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::Accept};
    client_->accept_invite_async(req_id, room_id);
}

void ShellBase::decline_invite_async_(const std::string& room_id)
{
    if (room_id.empty() || !client_)
        return;
    // Optimistically remove from the local list for immediate UX; the next
    // on_invites_updated callback from sync will confirm or restore it.
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&room_id](const InviteInfo& inv)
                       { return inv.room_id == room_id; }),
        invites_.end());
    on_invites_updated_();
    client_->decline_invite_async(room_id);
}

void ShellBase::block_invite_async_(const std::string& room_id,
                                    const std::string& inviter_id)
{
    if (room_id.empty() || !client_)
        return;
    // Optimistically remove from the local list for immediate UX; the next
    // on_invites_updated callback from sync will confirm or restore it.
    invites_.erase(
        std::remove_if(invites_.begin(), invites_.end(),
                       [&room_id](const InviteInfo& inv)
                       { return inv.room_id == room_id; }),
        invites_.end());
    on_invites_updated_();
    client_->block_invite_async(room_id, inviter_id);
}

void ShellBase::leave_room_command_(const std::string& room_id)
{
    if (room_id.empty() || !client_)
        return;
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id, RoomActionKind::Leave};
    client_->leave_room_async(req_id, room_id);
}

void ShellBase::join_room_command_(const std::string& room_id_or_alias)
{
    if (room_id_or_alias.empty() || !client_)
        return;
    auto req_id = next_room_action_id_++;
    pending_room_actions_[req_id] = {room_id_or_alias, RoomActionKind::Join};
    client_->join_room_async(req_id, room_id_or_alias);
}

void ShellBase::invite_user_command_(const std::string& room_id,
                                     const std::string& user_id)
{
    if (room_id.empty() || user_id.empty() || !client_)
        return;
    client_->invite_user_async(room_id, user_id);
}

ShellBase::RoomSendOutcome ShellBase::dispatch_room_send_(
    const std::string& room_id, const std::string& body,
    const std::string& formatted_body)
{
    RoomSendOutcome out;
    if (room_id.empty() || !client_)
    {
        // No active room/client: treat as consumed so callers no-op without
        // clearing on a failed send result.
        out.handled_as_command = true;
        return out;
    }
    // Commands that open a native dialog or enqueue async room actions are
    // intercepted here; everything else falls through to dispatch_compose_send.
    if (tesseract::is_slash_command_no_arg(body, "myroomavatar"))
    {
        pick_and_set_room_avatar_(room_id);
        out.handled_as_command = true;
        return out;
    }
    if (tesseract::is_slash_command_no_arg(body, "leave"))
    {
        leave_room_command_(room_id);
        out.handled_as_command = true;
        return out;
    }
    if (auto target = tesseract::parse_slash_arg(body, "join"))
    {
        join_room_command_(*target);
        out.handled_as_command = true;
        return out;
    }
    if (auto user = tesseract::parse_slash_arg(body, "invite"))
    {
        invite_user_command_(room_id, *user);
        out.handled_as_command = true;
        return out;
    }
    out.handled_as_command = false;
    out.send_result =
        tesseract::dispatch_compose_send(*client_, room_id, body,
                                         formatted_body);
    return out;
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
    auto sess = active_account_;
    run_async_(
        [this, sess, space_ids = std::move(space_ids)]()
        {
            if (!sess || !sess->client) return;
            std::unordered_map<std::string, std::vector<std::string>> fresh;
            for (const auto& id : space_ids)
            {
                fresh[id] = sess->client->space_children(id);
            }
            post_to_ui_alive_(
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

void ShellBase::apply_space_child_counts_(std::vector<RoomInfo>& rooms) const
{
    if (space_children_cache_.empty())
        return;

    struct ChildCounts
    {
        uint64_t notification_count;
        uint64_t highlight_count;
        uint64_t last_activity_ts;
    };
    std::unordered_map<std::string, ChildCounts> counts;
    counts.reserve(rooms_.size());
    for (const auto& r : rooms_)
        counts[r.id] = {r.notification_count, r.highlight_count,
                        r.last_activity_ts};

    for (auto& r : rooms)
    {
        if (!r.is_space)
            continue;
        auto it = space_children_cache_.find(r.id);
        if (it == space_children_cache_.end())
            continue;
        uint64_t nc = 0, hc = 0, newest_unread_ts = 0;
        for (const auto& child_id : it->second)
        {
            auto ci = counts.find(child_id);
            if (ci != counts.end())
            {
                nc += ci->second.notification_count;
                hc += ci->second.highlight_count;
                // Track the most recent activity among *unread* children so the
                // room list can treat the space as a recency-ranked unread
                // candidate (its own last_activity_ts is not meaningful).
                if (ci->second.notification_count > 0)
                    newest_unread_ts =
                        std::max(newest_unread_ts, ci->second.last_activity_ts);
            }
        }
        r.notification_count = nc;
        r.highlight_count    = hc;
        if (nc > 0)
            r.last_activity_ts = std::max(r.last_activity_ts, newest_unread_ts);
    }
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

void ShellBase::setup_link_clicked_(views::RoomView* rv)
{
    if (!rv) return;
    rv->on_link_clicked = [this](const std::string& url)
    {
        if (Client::parse_matrix_link(url).kind != Client::MatrixLink::Kind::Unknown)
            open_matrix_link(url);
        else
            Client::open_in_browser(url);
    };
}

void ShellBase::setup_dm_callbacks()
{
    if (!room_view_) return;
    room_view_->on_open_dm = [this](std::string user_id)
    {
        handle_open_dm_(std::move(user_id));
    };
    room_view_->on_has_dm = [this](const std::string& user_id)
    {
        return !find_existing_dm_(user_id).empty();
    };
}

void ShellBase::handle_open_dm_(const std::string& user_id)
{
    if (user_id.empty() || !client_) return;

    // Fast path: DM already in rooms_ — navigate immediately.
    if (auto existing = find_existing_dm_(user_id); !existing.empty())
    {
        if (room_view_) room_view_->close_user_profile();
        navigate_to_room_(existing);
        return;
    }

    // In-flight guard: suppress duplicate async calls for the same user.
    if (dm_in_flight_user_ids_.count(user_id)) return;
    dm_in_flight_user_ids_.insert(user_id);

    // Show loading state while the async call runs.
    if (room_view_)
    {
        room_view_->set_dm_button_state(
            views::UserProfilePanel::DmButtonState::Sending);
        request_repaint_();
    }

    auto sess = active_account_;
    run_async_mut_([this, sess, user_id]()
    {
        if (!sess || !sess->client) return;
        auto dm_id = sess->client->get_or_create_dm(user_id);
        post_to_ui_alive_([this, user_id, dm_id = std::move(dm_id)]() mutable
        {
            dm_in_flight_user_ids_.erase(user_id);
            if (!dm_id.empty())
            {
                if (room_view_) room_view_->close_user_profile();
                navigate_to_room_(dm_id);
            }
            else
            {
                // Reset so the user can retry.
                if (room_view_)
                {
                    room_view_->set_dm_button_state(
                        views::UserProfilePanel::DmButtonState::Normal);
                    request_repaint_();
                }
            }
        });
    });
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

const RoomInfo* ShellBase::best_unread_room_() const
{
    const RoomInfo* best = nullptr;
    for (const auto& r : rooms_)
    {
        if (r.notification_count == 0 && r.highlight_count == 0)
            continue;
        if (!best
            || (r.highlight_count > 0 && best->highlight_count == 0)
            || (r.highlight_count == best->highlight_count
                && r.last_activity_ts > best->last_activity_ts))
            best = &r;
    }
    return best;
}

void ShellBase::open_matrix_link(const std::string& uri)
{
    if (!client_ || rooms_.empty())
    {
        // Not yet logged in or rooms haven't arrived yet — replay on first update.
        pending_matrix_link_ = uri;
        return;
    }
    pending_matrix_link_.clear();

    auto link = Client::parse_matrix_link(uri);
    using Kind = Client::MatrixLink::Kind;

    switch (link.kind)
    {
    case Kind::User:
        if (room_view_)
            room_view_->open_user_profile(link.primary);
        break;

    case Kind::Room:
    {
        auto it = std::find_if(rooms_.begin(), rooms_.end(),
                               [&](const RoomInfo& r) { return r.id == link.primary; });
        if (it != rooms_.end())
            tab_navigate_room(link.primary);
        else
            open_join_room_dialog_ui_(link.primary);
        break;
    }

    case Kind::RoomAlias:
    {
        auto it = std::find_if(rooms_.begin(), rooms_.end(),
                               [&](const RoomInfo& r)
                               { return r.canonical_alias == link.primary; });
        if (it != rooms_.end())
            tab_navigate_room(it->id);
        else
            open_join_room_dialog_ui_(link.primary);
        break;
    }

    case Kind::Event:
    {
        auto it = std::find_if(rooms_.begin(), rooms_.end(),
                               [&](const RoomInfo& r) { return r.id == link.primary; });
        if (it != rooms_.end())
        {
            tab_navigate_room(link.primary);
            if (room_view_ && !link.event_id.empty())
                room_view_->scroll_to_event_id(link.event_id);
        }
        else
        {
            open_join_room_dialog_ui_(link.primary);
        }
        break;
    }

    default:
        // Not a matrix link — ignore.
        break;
    }
}

void ShellBase::navigate_tray_unread_()
{
    if (const RoomInfo* best = best_unread_room_())
        tab_navigate_room(best->id);
}

bool ShellBase::focus_tray_unread_popout_()
{
    const RoomInfo* best = best_unread_room_();
    return best && focus_secondary_window_(best->id);
}

void ShellBase::push_paginate_result_(std::string room_id, bool reached_start)
{
    auto& state = pagination_[room_id];
    state.in_flight = false;
    state.reached_start = reached_start;
    if (room_id == current_room_id_ && room_view_)
    {
        room_view_->set_paginating(false);
        schedule_relayout_(); // flush_pending_prepends_ may have armed the scroll anchor
    }
}

void ShellBase::handle_paginate_result_ui_(std::uint64_t request_id, bool ok,
                                           bool reached_start, bool reached_end,
                                           std::string /*message*/)
{
    auto it = pending_paginates_.find(request_id);
    if (it == pending_paginates_.end())
        return;
    auto [room_id, is_backward] = it->second;
    pending_paginates_.erase(it);

    auto& state = pagination_[room_id];
    if (is_backward)
    {
        state.in_flight = false;
        if (ok)
            state.reached_start = reached_start;
        if (room_id == current_room_id_ && room_view_)
        {
            room_view_->set_paginating(false);
            schedule_relayout_();
        }
    }
    else
    {
        state.fwd_in_flight = false;
        if (ok)
        {
            state.reached_end = reached_end;
            if (reached_end)
                return_to_live_(room_id);
        }
    }
}

void ShellBase::handle_room_action_complete_ui_(std::uint64_t request_id,
                                                 bool ok,
                                                 std::string joined_room_id,
                                                 std::string message)
{
    auto it = pending_room_actions_.find(request_id);
    if (it == pending_room_actions_.end())
        return;
    auto [room_id, kind] = std::move(it->second);
    pending_room_actions_.erase(it);

    if (!ok)
    {
        std::fprintf(stderr, "[room-action] failed for %s: %s\n",
                     room_id.c_str(), message.c_str());
        const char* verb = "complete room action";
        switch (kind)
        {
        case RoomActionKind::Accept:
            verb = "accept invite";
            break;
        case RoomActionKind::Join:
            verb = "join room";
            break;
        case RoomActionKind::Leave:
            verb = "leave room";
            break;
        }
        std::string status = std::string("Couldn't ") + verb;
        if (!message.empty())
            status += ": " + message;
        show_status_message_(std::move(status));
        return;
    }

    switch (kind)
    {
    case RoomActionKind::Accept:
        tab_select_room(room_id);
        break;
    case RoomActionKind::Join:
        if (!joined_room_id.empty())
            tab_navigate_room(joined_room_id);
        break;
    case RoomActionKind::Leave:
        if (tabs_.size() > 1)
        {
            tab_close(room_id);
        }
        else
        {
            // Deselecting the active room: tear down the thread panel and let
            // after_active_room_changed_() cancel the leaving room's pending
            // media downloads, mirroring the canonical deselect path.
            {
                auto _tt = compute_thread_transition_(
                    thread_panel_, thread_panel_prev_, current_thread_root_,
                    ThreadTrigger::RoomSwitch, {});
                apply_thread_transition_(_tt);
            }
            current_room_id_.clear();
            after_active_room_changed_();
            if (room_view_)
                room_view_->clear_room();
            request_relayout_();
        }
        break;
    }
}

void ShellBase::handle_upload_complete_ui_(std::uint64_t /*request_id*/,
                                            bool ok,
                                            std::string message)
{
    if (!ok)
    {
        std::fprintf(stderr, "[upload] failed: %s\n", message.c_str());
        std::string status = "Upload failed";
        if (!message.empty())
            status += ": " + message;
        show_status_message_(std::move(status));
    }
}

void ShellBase::try_scroll_to_room_event_(const std::string& event_id)
{
    if (event_id.empty() || !room_view_ || current_room_id_.empty() || !client_)
        return;
    auto* ml = room_view_->message_list();
    if (!ml)
        return;

    pending_scroll_room_event_id_ = event_id;
    ml->set_pending_scroll_event_id(event_id);
    request_repaint_();

    // If the event is already in the loaded timeline the deferred scroll in
    // arrange() will apply it on the next paint — nothing else needed.
    for (const auto& m : ml->messages())
    {
        if (m.event_id == event_id)
            return;
    }

    // Event is not in the current window. Use subscribe_room_at to rebuild
    // the timeline centred on the target event (one /context fetch) rather
    // than paginating backwards batch-by-batch, which would flood the UI with
    // every intermediate event and can stall the main thread.
    const auto rid = current_room_id_;
    begin_focused_subscription_(rid, event_id);
    auto sess = active_account_;
    run_async_mut_([sess, rid, event_id]()
    {
        if (!sess || !sess->client) return;
        sess->client->subscribe_room_at(rid, event_id);
    });
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
        return;
    if (!state.is_focused)
        return;
    state.fwd_in_flight = true;

    auto req_id = next_paginate_id_++;
    pending_paginates_[req_id] = {room_id, false};
    client_->paginate_forward_async(req_id, room_id, kPaginationBatch);
}

void ShellBase::return_to_live_(const std::string& room_id)
{
    auto& state = pagination_[room_id];
    state.is_focused = false;
    state.focus_event_id.clear();
    state.reached_end = false;
    state.fwd_in_flight = false;
    state.in_flight = true;

    // subscribe_room is CPU-bound (&mut); keep it on mut_pool_.
    // paginate_back_async fires a tokio task and returns immediately so
    // mut_pool_ is freed before the HTTP round-trip begins.
    auto sess = active_account_;
    run_async_mut_(
        [this, sess, room_id]()
        {
            if (!sess || !sess->client) return;
            sess->client->subscribe_room(room_id);
            post_to_ui_alive_(
                [this, room_id]()
                {
                    auto req_id = next_paginate_id_++;
                    pending_paginates_[req_id] = {room_id, true};
                    client_->paginate_back_async(req_id, room_id,
                                                 kPaginationBatch);
                });
        });
}

void ShellBase::push_room_list_state_(RoomListState state)
{
    last_room_list_state_ = state;
}

// ── Secondary window registry ─────────────────────────────────────────────────

ShellBase::ShellBase(AccountManager& account_manager)
    : account_manager_(account_manager)
{
}

void ShellBase::broadcast_rebuild_tray_()
{
    for (ShellBase* win : account_manager_.all_windows())
        win->rebuild_tray_();
}

std::vector<std::pair<std::string, std::function<void()>>>
ShellBase::build_tray_items_() const
{
    std::vector<std::pair<std::string, std::function<void()>>> items;
    for (tesseract::ShellBase* win : account_manager_.all_windows())
    {
        std::string label;
        auto acc = win->active_account();
        if (acc)
        {
            label = acc->display_name.empty()
                        ? acc->user_id
                        : acc->display_name + " (" + acc->user_id + ")";
        }
        else
        {
            label = "Tesseract";
        }
        items.emplace_back(std::move(label),
                           [win] { win->raise_and_activate_(); });
    }
    return items;
}

void ShellBase::arm_pending_login_()
{
    if (!pending_login_temp_dir_.empty())
    {
        return;
    }
    // The timestamp only needs to make the temp dir name unique for this login
    // attempt; nothing parses it back, so a portable std::chrono source serves
    // every platform (no need for QDateTime / per-shell clocks).
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    pending_login_temp_dir_ =
        tesseract::SessionStore::account_dir("pending-" + std::to_string(ms));
    std::error_code ec;
    std::filesystem::create_directories(pending_login_temp_dir_, ec);
    pending_login_client_->set_data_dir(
        (pending_login_temp_dir_ / "matrix-store").string());
}

ShellBase::RestoreResult ShellBase::restore_all_accounts_()
{
    RestoreResult result;

    // One-shot migration from the legacy single-account layout. Runs once per
    // install before any Client is constructed; idempotent on every subsequent
    // launch.
    tesseract::SessionStore::migrate_legacy_layout();

    // Restore every account on disk, in index order, so notifications fire for
    // any of them while the user works in the foreground one.
    auto index = tesseract::SessionStore::load_index();

    for (const auto& uid : index.user_ids)
    {
        auto json = tesseract::SessionStore::load_account(uid);
        if (!json)
        {
            continue;
        }

        auto session    = std::make_unique<tesseract::AccountSession>();
        session->client = std::make_unique<tesseract::Client>();
        session->client->set_data_dir(
            tesseract::SessionStore::sdk_store_dir(uid).string());

        auto res = session->client->restore_session(*json);
        if (!res)
        {
            result.restore_error      = res.message;
            result.any_restore_failed = true;
            continue;
        }

        session->user_id      = session->client->get_user_id();
        session->display_name = session->client->get_display_name();
        session->avatar_url   = session->client->get_avatar_url();
        {
            auto prefs = tesseract::Prefs::parse(
                session->client->load_prefs_json());
            session->last_room  = prefs.last_room;
            session->open_rooms = prefs.open_rooms;
        }

        // Per-account event bridge (native type) + background sync.
        session->bridge = make_account_bridge_(session->user_id);
        session->client->start_sync(session->bridge.get());
        session->sync_started = true;

        // Per-account notifier (native) and Linux-only UnifiedPush connector.
        install_account_notifier_(*session);
        install_account_up_connector_(*session);

        if (session->user_id == index.active_user_id)
        {
            result.active_uid = session->user_id;
        }
        account_manager_.add_account(std::move(session));
    }

    result.any_accounts = !account_manager_.accounts().empty();
    if (result.active_uid.empty() && result.any_accounts)
    {
        result.active_uid = account_manager_.accounts().front()->user_id;
    }
    return result;
}

ShellBase::FinalizeLoginResult ShellBase::finalize_login_()
{
    FinalizeLoginResult out;

    if (!pending_login_client_)
    {
        return out; // defensive — !ok, nothing to report
    }

    // OAuth completed on pending_login_client_, which the LoginView drove against
    // the temp data dir (pending_login_temp_dir_/matrix-store). We don't yet know
    // the user_id; ask the client now.
    const std::string user_id = pending_login_client_->get_user_id();
    if (user_id.empty())
    {
        out.error = "no user id";
        return out;
    }
    out.user_id = user_id;

    // If an account with this user_id is already signed in (the user added an
    // account they're already logged into), refuse rather than colliding on disk.
    if (account_manager_.find(user_id))
    {
        out.rejected_duplicate = true;
        pending_login_client_.reset();
        std::error_code ec;
        std::filesystem::remove_all(pending_login_temp_dir_, ec);
        pending_login_temp_dir_.clear();
        return out;
    }

    // Snapshot the session blob before dropping the in-flight Client — re-opening
    // it below restores from this JSON.
    const std::string session_json = pending_login_client_->export_session();
    if (session_json.empty())
    {
        out.error = "empty session";
        return out;
    }

    // Drop the in-flight Client so its SQLite handles are released before we
    // rename the directory underneath it. (The shell has already cleared any raw
    // login-view alias to this client.)
    pending_login_client_.reset();

    // Move the temp account directory into its final per-user-id home. The rename
    // is atomic on the same filesystem; on a cross-filesystem move it fails with
    // EXDEV, so fall back to a recursive copy + remove.
    const std::filesystem::path final_dir =
        tesseract::SessionStore::account_dir(user_id);
    {
        std::error_code ec;
        std::filesystem::create_directories(final_dir.parent_path(), ec);
        std::filesystem::rename(pending_login_temp_dir_, final_dir, ec);
        if (ec)
        {
            std::error_code ec2;
            std::filesystem::copy(
                pending_login_temp_dir_, final_dir,
                std::filesystem::copy_options::recursive |
                    std::filesystem::copy_options::overwrite_existing,
                ec2);
            if (ec2)
            {
                out.error = "couldn't persist matrix store: " + ec2.message();
                return out;
            }
            std::filesystem::remove_all(pending_login_temp_dir_, ec2);
        }
    }
    pending_login_temp_dir_.clear();

    // Persist the session blob into the final per-account dir.
    if (!tesseract::SessionStore::save_account(user_id, session_json))
    {
        out.error = "couldn't persist session";
        return out;
    }

    // Open a fresh Client against the final store path and restore from the
    // just-exported session JSON (matrix-sdk reuses the moved SQLite store
    // transparently — no resync).
    auto session    = std::make_unique<tesseract::AccountSession>();
    session->user_id = user_id;
    session->client = std::make_unique<tesseract::Client>();
    session->client->set_data_dir(
        tesseract::SessionStore::sdk_store_dir(user_id).string());
    auto res = session->client->restore_session(session_json);
    if (!res)
    {
        out.error = "restore: " + res.message;
        tesseract::SessionStore::clear_account(user_id);
        return out;
    }
    session->display_name = session->client->get_display_name();
    session->avatar_url   = session->client->get_avatar_url();
    {
        auto prefs = tesseract::Prefs::parse(session->client->load_prefs_json());
        session->last_room  = prefs.last_room;
        session->open_rooms = prefs.open_rooms;
    }

    // Per-account event bridge (native type) + background sync.
    session->bridge = make_account_bridge_(session->user_id);
    session->client->start_sync(session->bridge.get());
    session->sync_started = true;

    // Per-account notifier (native) and Linux-only UnifiedPush connector.
    install_account_notifier_(*session);
    install_account_up_connector_(*session);

    account_manager_.add_account(std::move(session));

    // Update the on-disk index. Active = the account we just added.
    auto index = tesseract::SessionStore::load_index();
    if (std::find(index.user_ids.begin(), index.user_ids.end(), user_id) ==
        index.user_ids.end())
    {
        index.user_ids.push_back(user_id);
    }
    index.active_user_id = user_id;
    tesseract::SessionStore::save_index(index);

    out.ok = true;
    return out;
}

bool ShellBase::switch_active_account_impl_(const std::string& user_id)
{
    auto new_session = account_manager_.find(user_id);
    if (!new_session)
    {
        return false;
    }
    if (new_session == active_account_ && client_)
    {
        return false;
    }

    // Unsubscribe the previous account's open room so its timeline stops
    // streaming updates to the message list when we swap surfaces. Skip rooms
    // that are pinned by a pop-out subscription. Must happen before client_ is
    // reassigned to the incoming account. (Phase-1.2 fix, now shared so every
    // shell — Windows included — gets it.)
    if (client_ && !current_room_id_.empty() &&
        room_subscription_refs_.count(current_room_id_) == 0)
    {
        client_->unsubscribe_room(current_room_id_);
    }

    // Drop per-account, room-id-keyed state so it can't bleed into the next
    // account (a room id present in both accounts would otherwise inherit stale
    // pagination / space-drill / reply-fetch state).
    current_room_id_.clear();
    tabs_.clear();
    active_tab_idx_ = 0;
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();

    // Save the outgoing account's banner state before switching.
    if (active_account_)
    {
        active_account_->verification_banner_dismissed =
            verification_banner_dismissed_;
    }

    reset_server_info_();
    active_account_ = new_session;
    auto& sess = *active_account_;

    client_ = sess.client.get();
    event_handler_ = sess.bridge.get(); // keep ShellBase's non-owning alias in sync

    my_user_id_ = sess.user_id;
    my_display_name_ = sess.display_name;
    my_avatar_url_ = sess.avatar_url;
    pending_restore_rooms_ = sess.open_rooms.empty()
        ? (sess.last_room.empty() ? std::vector<std::string>{}
                                  : std::vector<std::string>{sess.last_room})
        : sess.open_rooms;
    // Rotate last_room to [0] so it opens as the active tab.
    if (!sess.last_room.empty() && !pending_restore_rooms_.empty() &&
        pending_restore_rooms_[0] != sess.last_room)
    {
        auto it = std::find(pending_restore_rooms_.begin(),
                            pending_restore_rooms_.end(), sess.last_room);
        if (it != pending_restore_rooms_.end())
            std::rotate(pending_restore_rooms_.begin(), it, it + 1);
    }
    pending_restore_popouts_.clear();
    populate_pending_restore_popouts_();

    if (settings_controller_)
    {
        settings_controller_->set_client(client_);
        settings_controller_->set_up_connector(sess.up_connector.get());
    }

    // Use this account's last-known rooms snapshot if cached; otherwise leave
    // rooms_ empty and wait for the next on_rooms_updated_ callback. The native
    // room-list refresh happens in refresh_account_ui_after_switch_().
    auto it = per_account_rooms_.find(my_user_id_);
    rooms_ = (it != per_account_rooms_.end()) ? it->second
                                              : std::vector<tesseract::RoomInfo>{};

    // Restore the invite snapshot for the incoming account (parallel to rooms_).
    auto inv_it = per_account_invites_.find(my_user_id_);
    invites_ = (inv_it != per_account_invites_.end())
                   ? inv_it->second
                   : std::vector<tesseract::InviteInfo>{};
    on_invites_updated_();

    // Dismiss any stale InviteCard from the previous account.
    current_invite_.reset();

    // Load the incoming account's banner state.
    verification_banner_dismissed_ = sess.verification_banner_dismissed;

    // Persist the active selection on disk (active = the new uid).
    auto index = tesseract::SessionStore::load_index();
    index.active_user_id = my_user_id_;
    tesseract::SessionStore::save_index(index);

    return true;
}

ShellBase::~ShellBase()
{
    // Signal any UI-thread continuations queued via post_to_ui_alive_ that this
    // shell is gone; they will no-op rather than dereference freed members.
    *alive_ = false;

    // Tear down pop-out windows while the registries they unregister from are
    // still alive. Members are destroyed in reverse declaration order, so the
    // owned-window vector (declared before secondary_windows_ /
    // room_subscription_refs_) would otherwise be destroyed *last* — each
    // ~RoomWindowBase would then call unregister_room_window_() /
    // release_room_subscription_() on already-freed maps, a use-after-free that
    // crashes on shutdown whenever a pop-out is open. Clearing here runs every
    // ~RoomWindowBase while those maps are intact.
    owned_secondary_windows_.clear();
}

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

bool ShellBase::focus_secondary_window_(const std::string& room_id)
{
    auto it = secondary_windows_.find(room_id);
    if (it != secondary_windows_.end() && it->second)
    {
        it->second->bring_to_front();
        return true;
    }
    return false;
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
    auto sess = active_account_;
    run_async_mut_(
        [sess, room_id]
        {
            if (sess && sess->client)
            {
                sess->client->subscribe_room(room_id);
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
    auto sess = active_account_;
    run_async_mut_(
        [sess, room_id]
        {
            if (sess && sess->client)
            {
                sess->client->unsubscribe_room(room_id);
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

        // Record that this room is now open as a popout so it can be
        // restored on the next launch. Geometry is written by the window
        // itself on resize/move; initialise with valid=false so the first
        // save_popout_geometry_() call writes real coordinates.
        auto& pops = Settings::instance().popout_windows;
        auto pit = std::find_if(pops.begin(), pops.end(),
                                [&room_id](const Settings::PopoutEntry& e)
                                { return e.room_id == room_id; });
        if (pit == pops.end())
        {
            Settings::PopoutEntry e;
            e.room_id = room_id;
            e.user_id = active_account_ ? active_account_->user_id : std::string{};
            pops.push_back(std::move(e));
            save_settings_debounced_();
        }
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
    auto sess = active_account_;
    run_async_mut_(
        [sess, room_id, event_id]()
        {
            if (sess && sess->client)
            {
                sess->client->send_read_receipt(room_id, event_id);
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
    auto sess = active_account_;
    run_async_mut_(
        [sess, room_id]()
        {
            if (sess && sess->client)
            {
                sess->client->mark_room_as_read(room_id);
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
    // Only the active account's prefs set the pending restore rooms.
    if (!active_account_ || active_account_->user_id != user_id)
    {
        return;
    }
    auto prefs = tesseract::Prefs::parse(json);
    if (!prefs.open_rooms.empty() && pending_restore_rooms_.empty() &&
        current_room_id_.empty())
    {
        pending_restore_rooms_ = prefs.open_rooms;
        // Ensure last_room (active tab) is at [0].
        if (!prefs.last_room.empty() && pending_restore_rooms_[0] != prefs.last_room)
        {
            auto it = std::find(pending_restore_rooms_.begin(),
                                pending_restore_rooms_.end(), prefs.last_room);
            if (it != pending_restore_rooms_.end())
                std::rotate(pending_restore_rooms_.begin(), it, it + 1);
        }
        // Populate popouts-to-restore from local settings (device-local, not
        // synced). Only do this on the first account-prefs arrival so that a
        // manual pop-out/close during a session doesn't re-queue stale entries.
        populate_pending_restore_popouts_();
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
        ensure_media_thumbnail_(preview.image_mxc, 64, 64, false);
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

void ShellBase::notify_secondary_media_ready_(const std::string& cache_key,
                                              MediaKind kind)
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        views::RoomView* rv = w->room_view();
        if (!rv)
        {
            continue;
        }
        switch (kind)
        {
        case MediaKind::MediaImage:
        case MediaKind::MediaThumbnail:
            rv->notify_image_ready(cache_key);
            w->request_relayout();
            break;
        case MediaKind::Tile:
            if (auto* ml = rv->message_list())
            {
                ml->invalidate_data();
            }
            w->request_relayout();
            break;
        case MediaKind::RoomAvatar:
        case MediaKind::UserAvatar:
            // No height change; a relayout/repaint pulls the new avatar from
            // the shared cache on next paint.
            w->request_relayout();
            break;
        }
    }
}

void ShellBase::dispatch_gif_to_secondary_windows_(
    std::uint64_t request_id, const std::vector<GifResult>& results)
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        w->on_gif_results(request_id, results); // copy per window
    }
}

void ShellBase::dispatch_gif_failed_to_secondary_windows_(
    std::uint64_t request_id, const std::string& message)
{
    for (const auto& [rid, w] : secondary_windows_)
    {
        w->on_gif_search_failed(request_id, message);
    }
}

std::vector<std::uint8_t>
ShellBase::cached_gif_source_bytes_(const std::string& url) const
{
    return account_manager_.media_disk_cache().load(gif_src_disk_key_(url));
}

bool ShellBase::tick_anim_()
{
    // Stop once nothing animated is on-screen — entries linger in the cache
    // after scrolling away / switching rooms, so checking emptiness would keep
    // the 60 Hz timer (and its repaints) running forever.
    if (!account_manager_.anim_cache().any_visible())
    {
        stop_anim_tick_();
        return false;
    }
    if (account_manager_.anim_cache().advance(monotonic_ms_()))
    {
        repaint_anim_frame_();
        // Pop-out windows have their own surfaces (and pickers) the shell's
        // repaint_anim_frame_ doesn't know about — advance them too.
        for (auto& w : owned_secondary_windows_)
        {
            if (w)
                w->repaint_anim_frame();
        }
    }
    return true;
}

void ShellBase::schedule_relayout_()
{
    if (relayout_scheduled_)
    {
        return; // a flush is already queued — fold this request into it
    }
    relayout_scheduled_ = true;
    post_to_ui_alive_(
        [this]
        {
            relayout_scheduled_ = false;
            request_relayout_();
        });
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
        // set_messages() clears MessageListView::pending_scroll_event_id_.
        // Re-arm it so arrange() still applies the deferred scroll with
        // up-to-date row_offsets_ (handles the subscribe_room_at reset path).
        if (!pending_scroll_room_event_id_.empty() &&
            room_view_->message_list())
        {
            room_view_->message_list()->set_pending_scroll_event_id(
                pending_scroll_room_event_id_);
        }
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
    // In-thread replies belong to a thread, not the main timeline. The main
    // window's list excludes them; pop-out main lists must do the same, or
    // their rows diverge from the main window and later update/remove indices
    // (which arrive in main-timeline coordinates) land on the wrong rows.
    const bool in_thread = !ev->thread_root_id.empty();
    if (room_id == current_room_id_ && !in_thread && room_view_)
    {
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        room_view_->insert_message(
            index, tesseract::views::make_row_data(*ev, my_user_id_));
        schedule_relayout_(); // coalesce bursts into one layout pass
    }
    if (!in_thread)
    {
        dispatch_message_inserted_secondary_(room_id, index, *ev);
    }
}

void ShellBase::handle_message_updated_ui_(std::string room_id,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
    {
        return;
    }
    // See handle_message_inserted_ui_: in-thread replies are excluded from the
    // main timeline on both the main window and pop-outs, keeping their rows
    // aligned with the main-timeline indices used by updates/removals.
    const bool in_thread = !ev->thread_root_id.empty();
    if (room_id == current_room_id_ && !in_thread && room_view_)
    {
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
        {
            ensure_reply_details_(ev->event_id);
        }
        room_view_->update_message(
            index, tesseract::views::make_row_data(*ev, my_user_id_));
        schedule_relayout_(); // coalesce bursts into one layout pass
    }
    if (!in_thread)
    {
        dispatch_message_updated_secondary_(room_id, index, *ev);
    }
}

void ShellBase::handle_message_removed_ui_(std::string room_id,
                                           std::size_t index)
{
    if (room_id == current_room_id_)
    {
        if (room_view_)
        {
            room_view_->remove_message(index);
            schedule_relayout_(); // coalesce bursts into one layout pass
        }
    }
    dispatch_message_removed_secondary_(room_id, index);
}

void ShellBase::handle_thread_reset_ui_(std::string room_id,
                                        std::string thread_root,
                                        EventList snapshot)
{
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    std::vector<views::MessageRowData> rows;
    rows.reserve(snapshot.size());
    for (auto& ev : snapshot)
    {
        if (!ev || ev->type == tesseract::EventType::Unhandled)
            continue;
        prep_row_media_(*ev);
        if (!ev->in_reply_to_id.empty())
            ensure_reply_details_(ev->event_id);
        rows.push_back(tesseract::views::make_row_data(*ev, my_user_id_));
    }
    apply_thread_messages_(thread_root, std::move(rows), /*room_switch=*/true);
}

void ShellBase::handle_thread_inserted_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
        return;
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    prep_row_media_(*ev);
    if (!ev->in_reply_to_id.empty())
        ensure_reply_details_(ev->event_id);
    apply_thread_message_insert_(
        thread_root, index,
        tesseract::views::make_row_data(*ev, my_user_id_));
}

void ShellBase::handle_thread_updated_ui_(std::string room_id,
                                          std::string thread_root,
                                          std::size_t index,
                                          std::unique_ptr<Event> ev)
{
    if (!ev || ev->type == tesseract::EventType::Unhandled)
        return;
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    prep_row_media_(*ev);
    if (!ev->in_reply_to_id.empty())
        ensure_reply_details_(ev->event_id);
    apply_thread_message_update_(
        thread_root, index,
        tesseract::views::make_row_data(*ev, my_user_id_));
}

void ShellBase::handle_thread_removed_ui_(std::string room_id,
                                          std::string thread_root,
                                          std::size_t index)
{
    if (room_id != current_room_id_ || thread_root != current_thread_root_)
        return;
    apply_thread_message_remove_(thread_root, index);
}

void ShellBase::handle_threads_updated_ui_(std::string room_id)
{
    // Update visibility regardless of panel state — the threads button needs
    // the latest list to decide whether to render. apply_threads_list_ no-ops
    // cheaply when the thread-list panel widget isn't around.
    if (room_id != current_room_id_ || !client_)
        return;
    apply_threads_list_(client_->list_room_threads(room_id));
    // on_near_bottom only fires on user scroll, so it can't bootstrap the
    // initial fill when the first SDK page fits within the viewport. Drive
    // pagination here instead: each completed page triggers this callback,
    // which requests the next one — stopping when threads_reached_start_.
    if (thread_panel_ == ThreadPanel::List)
        paginate_threads_();
}

void ShellBase::handle_media_ready_ui_(std::uint64_t request_id,
                                       std::vector<std::uint8_t> bytes)
{
    auto it = pending_media_.find(request_id);
    if (it == pending_media_.end())
        return; // Cancelled / superseded — drop the late callback.
    PendingMediaReq req = std::move(it->second);
    pending_media_.erase(it);
    on_inflight_ui_();
    if (req.on_bytes)
        req.on_bytes(std::move(bytes));
}

void ShellBase::handle_url_preview_ready_ui_(std::uint64_t request_id,
                                             std::string preview_json)
{
    auto it = pending_media_.find(request_id);
    if (it == pending_media_.end())
        return;
    PendingMediaReq req = std::move(it->second);
    pending_media_.erase(it);
    on_inflight_ui_();
    if (req.on_preview)
        req.on_preview(std::move(preview_json));
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
    // The DM-presence polling loop in the Rust SDK only produces data that's
    // visible to the user while the window is on-screen, so suspend it while
    // the window is hidden/minimized/unfocused. The kick on re-focus avoids
    // up to 60 s of stale presence after the loop is re-enabled. Gated by
    // the same `send_presence` Privacy setting that `handle_send_presence_toggle_`
    // owns — if the user has presence disabled, never re-enable polling here.
    if (client_ && tesseract::Settings::instance().send_presence)
    {
        auto sess = active_account_;
        run_async_mut_([sess, active]() {
            if (!sess || !sess->client) return;
            sess->client->set_presence_polling_enabled(active);
            if (active)
                sess->client->poll_presence_now();
        });
    }
}

void ShellBase::notify_presence_tick_()
{
    // Piggyback the 30 s presence tick (fired by every shell) to reclaim
    // images that scrolled off / rooms switched away from: drop expired,
    // unreferenced entries and trim over-budget. Runs even when presence
    // tracking is off, so it must precede the tracker guard.
    account_manager_.image_cache().sweep();
    account_manager_.thumbnail_cache().sweep();
    account_manager_.anim_cache().sweep();

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
        // so app shutdown drains it (WorkerPool destructor joins threads
        // before ~ShellBase completes, protecting Client lifetime).
        // Capture a strong ref to the account active at dispatch time so the
        // PUT targets that account's Client (and keeps it alive) even if the
        // user logs out / switches before the worker runs.
        const auto target = to_client_presence(s);
        auto sess = active_account_;
        run_async_mut_(
            [sess, target]
            {
                if (sess && sess->client)
                {
                    (void) sess->client->set_presence(target);
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
        auto sess = active_account_;
        run_async_mut_([sess, enabled]() {
            if (!sess || !sess->client) return;
            sess->client->set_presence_polling_enabled(enabled);
        });
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

void ShellBase::tab_open_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Already open in a pop-out window: raise it instead of re-opening here.
    if (focus_secondary_window_(room_id))
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
        }
        active_tab_idx_ = existing;
        {
            auto _tt = compute_thread_transition_(
                thread_panel_, thread_panel_prev_, current_thread_root_,
                ThreadTrigger::RoomSwitch, {});
            apply_thread_transition_(_tt);
        }
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        after_active_room_changed_();
        on_tab_state_changed_ui_();
        return;
    }
    if (!tabs_.empty())
    {
        tabs_[active_tab_idx_].scroll_offset = get_message_scroll_fraction_();
        tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
    }
    // Bootstrap: wrap current_room_id_ as first tab if tabs_ is empty.
    if (tabs_.empty() && !current_room_id_.empty())
    {
        tabs_.push_back({current_room_id_, 0.f, {}});
    }
    tabs_.push_back({room_id, 0.f, {}});
    active_tab_idx_ = tabs_.size() - 1;
    {
        auto _tt = compute_thread_transition_(
            thread_panel_, thread_panel_prev_, current_thread_root_,
            ThreadTrigger::RoomSwitch, {});
        apply_thread_transition_(_tt);
    }
    current_room_id_ = room_id;
    after_active_room_changed_();
    on_tab_state_changed_ui_();
}

void ShellBase::tab_select_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Already open in a pop-out window: raise it and leave the main app as-is.
    if (focus_secondary_window_(room_id))
    {
        return;
    }
    // Dismiss any visible InviteCard so the chat panel shows the room view.
    if (main_app_)
        main_app_->show_room();
    size_t existing = find_tab_(tabs_, room_id);
    if (existing != SIZE_MAX)
    {
        if (existing == active_tab_idx_)
            return;
        if (active_tab_idx_ < tabs_.size())
        {
            tabs_[active_tab_idx_].scroll_offset =
                get_message_scroll_fraction_();
            tabs_[active_tab_idx_].compose_draft = get_compose_draft_();
        }
        active_tab_idx_ = existing;
        {
            auto _tt = compute_thread_transition_(
                thread_panel_, thread_panel_prev_, current_thread_root_,
                ThreadTrigger::RoomSwitch, {});
            apply_thread_transition_(_tt);
        }
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        after_active_room_changed_();
        on_tab_state_changed_ui_();
        return;
    }
    if (tabs_.empty())
    {
        tabs_.push_back({room_id, 0.f, {}});
    }
    else
    {
        tabs_[active_tab_idx_] = {room_id, 0.f, {}};
    }
    {
        auto _tt = compute_thread_transition_(
            thread_panel_, thread_panel_prev_, current_thread_root_,
            ThreadTrigger::RoomSwitch, {});
        apply_thread_transition_(_tt);
    }
    current_room_id_ = room_id;
    after_active_room_changed_();
    on_tab_state_changed_ui_();
}

void ShellBase::tab_navigate_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return;
    }
    // Already open in a pop-out window: raise it instead of navigating here.
    if (focus_secondary_window_(room_id))
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
        }
        active_tab_idx_ = existing;
        {
            auto _tt = compute_thread_transition_(
                thread_panel_, thread_panel_prev_, current_thread_root_,
                ThreadTrigger::RoomSwitch, {});
            apply_thread_transition_(_tt);
        }
        current_room_id_ = tabs_[active_tab_idx_].room_id;
        after_active_room_changed_();
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
    {
        auto _tt = compute_thread_transition_(
            thread_panel_, thread_panel_prev_, current_thread_root_,
            ThreadTrigger::RoomSwitch, {});
        apply_thread_transition_(_tt);
    }
    current_room_id_ = tabs_[active_tab_idx_].room_id;
    after_active_room_changed_();
    on_tab_state_changed_ui_();
}

void ShellBase::tab_popout_room(const std::string& room_id)
{
    // Copy first: the callers pass a reference to a TabBar TabItem's room_id
    // string, and tab_close() below rebuilds the tab bar via
    // on_tab_state_changed_ui_(), freeing that string. Using the reference
    // afterwards would be a use-after-free (it manifested as the active room
    // popping out instead of the clicked one, and closing the active tab doing
    // nothing because the dangling read came back empty).
    const std::string id = room_id;
    if (id.empty())
    {
        return;
    }
    tab_close(id);               // no-op when this is the last tab
    open_room_in_new_window(id); // raises an existing pop-out if present
}

bool ShellBase::try_restore_tab_session_(
    const std::vector<std::string>& room_ids,
    const std::string&              active_room_id)
{
    std::vector<TabState> new_tabs;
    for (const auto& id : room_ids)
    {
        for (const auto& r : rooms_)
        {
            if (r.id == id && !r.is_space)
            {
                new_tabs.push_back({id, 0.f, {}});
                break;
            }
        }
    }
    if (new_tabs.empty())
        return false;

    tabs_          = std::move(new_tabs);
    active_tab_idx_ = 0;
    if (!active_room_id.empty())
    {
        for (size_t i = 0; i < tabs_.size(); ++i)
        {
            if (tabs_[i].room_id == active_room_id)
            {
                active_tab_idx_ = i;
                break;
            }
        }
    }
    {
        auto _tt = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                              current_thread_root_,
                                              ThreadTrigger::RoomSwitch, {});
        apply_thread_transition_(_tt);
    }
    current_room_id_ = tabs_[active_tab_idx_].room_id;
    after_active_room_changed_();
    on_tab_state_changed_ui_();

    // Restore pop-out windows saved from the previous session. Only rooms
    // that are now in the room list are opened; rooms not yet known are kept
    // in pending_restore_popouts_ and retried on the next rooms update.
    if (!pending_restore_popouts_.empty())
    {
        std::vector<std::string> still_pending;
        for (const auto& id : pending_restore_popouts_)
        {
            bool known = std::any_of(rooms_.begin(), rooms_.end(),
                                     [&id](const RoomInfo& r)
                                     { return r.id == id; });
            if (known)
                open_room_in_new_window(id);
            else
                still_pending.push_back(id);
        }
        pending_restore_popouts_ = std::move(still_pending);
    }

    return true;
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
                auto sess = active_account_;
                run_async_mut_(
                    [sess, rid,
                     pcm      = std::move(pcm),
                     waveform  = std::move(waveform),
                     duration_ms, caption, reply_id]() mutable
                    {
                        if (!sess || !sess->client) return;
                        const std::uint64_t est   = duration_ms * 3;
                        const std::uint64_t limit = sess->client->media_upload_limit();
                        if (limit > 0 && est > limit)
                            return;
                        auto res = sess->client->send_voice(
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
    std::function<void(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t)> callback)
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

        // Read in-memory cache totals and hit/miss stats on the UI thread.
        post_to_ui_alive_([this, cb, local, sdk]
        {
            const uint64_t memory =
                static_cast<uint64_t>(account_manager_.image_cache().current_bytes()) +
                account_manager_.thumbnail_cache().current_bytes() + account_manager_.anim_cache().current_bytes();
            const uint64_t mem_hits =
                account_manager_.image_cache().hits() + account_manager_.thumbnail_cache().hits() +
                account_manager_.anim_cache().hits();
            const uint64_t mem_misses =
                account_manager_.image_cache().misses() + account_manager_.thumbnail_cache().misses() +
                account_manager_.anim_cache().misses();
            const uint64_t disk_hits   = account_manager_.media_disk_cache().hits();
            const uint64_t disk_misses = account_manager_.media_disk_cache().misses();
            cb(local, sdk, memory, mem_hits, mem_misses, disk_hits, disk_misses);
        });
    });
}

void ShellBase::clear_all_caches_(
    std::function<void(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t)> recompute_callback)
{
    if (my_user_id_.empty())
        return;
    run_async_([this, recalc = std::move(recompute_callback)]() mutable
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Waveform SQLite — best-effort (locked on Windows if WAL is open).
        fs::remove(tesseract::cache_dir() / "waveforms.db", ec);

        // The MediaDiskCache is owned by the shared AccountManager and is
        // touched (load/store/prune/evict) from every window's worker pool.
        // It has no internal lock — concurrent ops are only safe because each
        // op is filesystem-atomic on a distinct key; clear() (remove_all +
        // recreate dir) is *not* in that set and would race a concurrent
        // load/store/prune on another window's worker. Defer it to the UI
        // thread alongside the in-memory clears, then queue the recompute from
        // there so it observes the cleared state.
        post_to_ui_alive_([this, recalc = std::move(recalc)]() mutable
        {
            account_manager_.media_disk_cache().clear();
            account_manager_.thumbnail_cache().clear();
            account_manager_.image_cache().clear();
            account_manager_.anim_cache() = tk::AnimImageCache{};
            media_decode_failed_.clear();
            media_fetch_failed_.clear();
            voice_bytes_cache_.clear();
            tesseract::init_waveform_cache(
                (tesseract::cache_dir() / "waveforms.db").string());
            restart_sdk_();

            // Recompute sizes so the UI reflects the cleared state.
            if (recalc)
                compute_cache_sizes_(std::move(recalc));
        });
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
    {
        auto _tt = compute_thread_transition_(
            thread_panel_, thread_panel_prev_, current_thread_root_,
            ThreadTrigger::RoomSwitch, {});
        apply_thread_transition_(_tt);
    }
    current_room_id_.clear();
    tabs_.clear();
    space_stack_.clear();
    pagination_.clear();
    reply_details_requested_.clear();
    // MSC4278 per-account gating state.
    room_preview_overrides_.clear();
    room_preview_override_in_flight_.clear();
    revealed_events_.clear();
    on_tab_state_changed_ui_();

    client_->stop_sync();
    client_->clear_caches();
    if (client_->restore_session(*json))
        client_->start_sync(event_handler_);
}

// static
ShellBase::ThreadTransition ShellBase::compute_thread_transition_(
    ThreadPanel cur, ThreadPanel prev, const std::string& current_root,
    ThreadTrigger trigger, const std::string& trigger_root)
{
    ThreadTransition t;
    t.new_state = cur;
    t.new_prev  = prev;
    t.new_root  = current_root;

    switch (trigger)
    {
    case ThreadTrigger::ToggleList:
        if (cur == ThreadPanel::Closed)
        {
            t.new_state = ThreadPanel::List;
            t.new_prev  = ThreadPanel::Closed;
            t.new_root.clear();
            t.subscribe_room_threads_ = true;
        }
        else if (cur == ThreadPanel::List)
        {
            t.new_state = ThreadPanel::Closed;
            t.new_prev  = ThreadPanel::Closed;
            t.new_root.clear();
            t.unsubscribe_room_threads_ = true;
        }
        else // Open
        {
            // Toggle while a thread is open: close everything (matches
            // the spec — main button is a Closed<->List toggle and any
            // thread sub gets released).
            t.new_state = ThreadPanel::Closed;
            t.new_prev  = ThreadPanel::Closed;
            t.threads_to_unsubscribe.push_back(current_root);
            t.new_root.clear();
            if (prev == ThreadPanel::List)
                t.unsubscribe_room_threads_ = true;
        }
        break;

    case ThreadTrigger::OpenFromList:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = ThreadPanel::Open;
        t.new_prev  = ThreadPanel::List;
        t.new_root  = trigger_root;
        t.threads_to_subscribe.push_back(trigger_root);
        break;

    case ThreadTrigger::OpenFromMain:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = ThreadPanel::Open;
        t.new_prev  = (cur == ThreadPanel::List) ? ThreadPanel::List
                                                 : ThreadPanel::Closed;
        t.new_root  = trigger_root;
        t.threads_to_subscribe.push_back(trigger_root);
        break;

    case ThreadTrigger::CloseThread:
        if (cur != ThreadPanel::Open)
            break; // no-op
        t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = prev;            // back to whatever opened us
        t.new_prev  = ThreadPanel::Closed;
        t.new_root.clear();
        if (prev == ThreadPanel::Closed)
            t.unsubscribe_room_threads_ = false; // never subscribed
        break;

    case ThreadTrigger::RoomSwitch:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        // Always release the outgoing room's thread-list subscription. The
        // shell now keeps a background subscription on the active room (for
        // the threads-button visibility check) regardless of panel state, so
        // every room switch must clean it up — even when the panel was closed.
        // unsubscribe_room_threads is a no-op when no handle exists.
        t.unsubscribe_room_threads_ = true;
        t.new_state = ThreadPanel::Closed;
        t.new_prev  = ThreadPanel::Closed;
        t.new_root.clear();
        break;
    }
    return t;
}

// ── Thread panel applier + public entry points ────────────────────────────

void ShellBase::apply_thread_transition_(const ThreadTransition& t)
{
    if (client_)
    {
        for (const auto& root : t.threads_to_unsubscribe)
            client_->unsubscribe_thread(current_room_id_, root);
        if (t.unsubscribe_room_threads_)
            client_->unsubscribe_room_threads(current_room_id_);
        if (t.subscribe_room_threads_)
            client_->subscribe_room_threads(current_room_id_);
        for (const auto& root : t.threads_to_subscribe)
            client_->subscribe_thread(current_room_id_, root);
    }

    thread_panel_         = t.new_state;
    thread_panel_prev_    = t.new_prev;
    current_thread_root_  = t.new_root;

    // Cancel any in-progress scroll-to-event when the thread panel closes or
    // switches rooms; a new scroll will be requested below if opening a thread.
    if (t.new_state != ThreadPanel::Open)
    {
        pending_scroll_room_event_id_.clear();
        if (room_view_ && room_view_->message_list())
            room_view_->message_list()->set_pending_scroll_event_id({});
    }

    if (room_view_)
    {
        using S = views::RoomView::ThreadPanelState;
        const S vs = (t.new_state == ThreadPanel::Closed) ? S::Closed
                  : (t.new_state == ThreadPanel::List)    ? S::List
                                                          : S::Open;
        room_view_->set_thread_panel(vs, t.new_root);

        // set_thread_panel lazily creates thread_list_view_ on the first List
        // transition — wire its on_near_top immediately after so older threads
        // paginate in when the user scrolls up toward the oldest. (Newest sit
        // at the bottom, matching the message timeline.)
        if (auto* tlv = room_view_->thread_list_view())
            tlv->on_near_top = [this] { paginate_threads_(); };

        request_relayout_();
    }

    // After set_thread_panel has synchronously re-laid out the message list
    // (via on_layout_changed), scroll to and highlight the thread root.
    // try_scroll_to_room_event_ paginates backwards if the event isn't loaded.
    if (t.new_state == ThreadPanel::Open && !t.new_root.empty())
        try_scroll_to_room_event_(t.new_root);

    if (client_ && t.new_state == ThreadPanel::List)
    {
        apply_threads_list_(client_->list_room_threads(current_room_id_));
        if (auto* tlv = room_view_->thread_list_view())
            tlv->scroll_to_bottom();
        // Re-arm backfill on every open: if the service window shrank (e.g.
        // after a reconnect or room re-entry), threads_reached_start_ would
        // incorrectly block re-pagination.  The extra paginate_room_threads
        // call is a cheap no-op when the service already has all history.
        // Newest threads sit at the bottom (scroll_to_bottom above pins the
        // view there via stick_to_bottom_); backfill grows the list upward.
        threads_reached_start_ = false;
        paginate_threads_();
    }
}

void ShellBase::on_threads_button_clicked()
{
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_,
                                        ThreadTrigger::ToggleList, {});
    apply_thread_transition_(t);
}

void ShellBase::on_thread_open_requested(const std::string& root_event_id)
{
    const auto trigger = (thread_panel_ == ThreadPanel::List)
                             ? ThreadTrigger::OpenFromList
                             : ThreadTrigger::OpenFromMain;
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_, trigger,
                                        root_event_id);
    apply_thread_transition_(t);
}

void ShellBase::on_thread_close_requested()
{
    auto t = compute_thread_transition_(thread_panel_, thread_panel_prev_,
                                        current_thread_root_,
                                        ThreadTrigger::CloseThread, {});
    apply_thread_transition_(t);
}

void ShellBase::on_thread_send_requested(const std::string& body,
                                         const std::string& formatted_body)
{
    if (!client_ || current_room_id_.empty() || current_thread_root_.empty())
        return;
    auto sess = active_account_;
    auto rid = current_room_id_;
    auto root = current_thread_root_;
    auto body_copy = body;
    auto fmt_copy = formatted_body;
    run_async_mut_([sess, rid, root, body_copy, fmt_copy]() mutable {
        if (!sess || !sess->client) return;
        sess->client->send_thread_message(rid, root, body_copy, fmt_copy);
    });
}

void ShellBase::on_thread_send_reply_requested(
    const std::string& in_reply_to_event_id,
    const std::string& body,
    const std::string& formatted_body)
{
    if (!client_ || current_room_id_.empty() || current_thread_root_.empty() ||
        in_reply_to_event_id.empty())
        return;
    auto sess = active_account_;
    auto rid = current_room_id_;
    auto root = current_thread_root_;
    auto reply_to = in_reply_to_event_id;
    auto body_copy = body;
    auto fmt_copy = formatted_body;
    run_async_mut_([sess, rid, root, reply_to, body_copy, fmt_copy]() mutable {
        if (!sess || !sess->client) return;
        sess->client->send_thread_reply(rid, root, reply_to, body_copy, fmt_copy);
    });
}

void ShellBase::on_pin_requested(const std::string& event_id)
{
    if (!client_ || current_room_id_.empty() || event_id.empty())
        return;
    auto sess = active_account_;
    auto rid = current_room_id_;
    auto eid = event_id;
    run_async_mut_([sess, rid, eid]() mutable {
        if (!sess || !sess->client) return;
        auto r = sess->client->pin_event(rid, eid);
        if (!r.ok)
        {
            // TODO: surface this via a transient status mechanism once one exists
            std::fprintf(stderr, "[pin] pin failed for %s in %s: %s\n",
                         eid.c_str(), rid.c_str(), r.message.c_str());
        }
    });
}

void ShellBase::on_unpin_requested(const std::string& event_id)
{
    if (!client_ || current_room_id_.empty() || event_id.empty())
        return;
    auto r = client_->unpin_event(current_room_id_, event_id);
    if (!r.ok)
    {
        // TODO: surface this via a transient status mechanism once one exists
        std::fprintf(stderr, "[pin] unpin failed for %s in %s: %s\n",
                     event_id.c_str(), current_room_id_.c_str(),
                     r.message.c_str());
    }
}

void ShellBase::refresh_pinned_for_current_room_()
{
    if (!room_view_)
        return;
    if (current_room_id_.empty())
    {
        room_view_->set_pinned({});
        room_view_->set_can_pin(false);
        return;
    }
    for (const auto& r : rooms_)
    {
        if (r.id == current_room_id_)
        {
            room_view_->set_pinned(r.pinned_events);
            room_view_->set_can_pin(
                client_ ? client_->can_pin_in_room(current_room_id_) : false);
            return;
        }
    }
    // Room not in cache (rare — e.g. mid-switch before push_rooms_ runs).
    // Clear so the previous room's banner doesn't bleed through.
    room_view_->set_pinned({});
    room_view_->set_can_pin(false);
}

// ── Concrete apply_thread_*_ virtuals (route into room_view_->thread_view) ─

void ShellBase::apply_thread_messages_(const std::string& /*thread_root*/,
                                       std::vector<views::MessageRowData> rows,
                                       bool room_switch)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->set_messages(std::move(rows), room_switch);
        request_relayout_();
    }
}

void ShellBase::apply_thread_message_insert_(const std::string& /*thread_root*/,
                                             std::size_t index,
                                             views::MessageRowData row)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->insert_message(index, std::move(row));
        request_relayout_();
    }
}

void ShellBase::apply_thread_message_update_(const std::string& /*thread_root*/,
                                             std::size_t index,
                                             views::MessageRowData row)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->update_message(index, std::move(row));
        request_relayout_();
    }
}

void ShellBase::apply_thread_message_remove_(const std::string& /*thread_root*/,
                                             std::size_t index)
{
    if (room_view_ && room_view_->thread_view())
    {
        room_view_->thread_view()->remove_message(index);
        request_relayout_();
    }
}

void ShellBase::apply_threads_list_(std::vector<ThreadInfo> threads)
{
    if (!room_view_)
        return;

    // Drive header-button visibility off the latest snapshot. This runs on
    // every on_threads_updated tick (including the initial empty tick after
    // subscribe), so the button reveals when the SDK paginates non-empty and
    // hides when the list goes empty (e.g., redactions, room switch).
    room_view_->set_show_threads_button(!threads.empty());

    if (room_view_->thread_list_view())
    {
        room_view_->thread_list_view()->set_threads(std::move(threads));
        request_relayout_();
    }
}

void ShellBase::paginate_threads_()
{
    if (!client_ || current_room_id_.empty() || threads_reached_start_ ||
        threads_paginating_)
        return;
    threads_paginating_ = true;
    auto* c       = client_;
    auto  sess    = active_account_;
    auto  room_id = current_room_id_;
    run_async_mut_([this, c, sess, room_id]
    {
        if (!sess || !sess->client) return;
        auto r = sess->client->paginate_room_threads(room_id);
        post_to_ui_alive_([this, c, room = room_id, reached = r.reached_start]
        {
            if (c != client_ || room != current_room_id_)
                return;
            threads_paginating_ = false;
            if (reached)
                threads_reached_start_ = true;
            else if (thread_panel_ == ThreadPanel::List)
                paginate_threads_();
        });
    });
}

void ShellBase::navigate_history_back()
{
    if (room_nav_history_.empty() || room_nav_history_cursor_ == 0)
        return;
    --room_nav_history_cursor_;
    room_nav_in_progress_ = true;
    navigate_to_room_(room_nav_history_[room_nav_history_cursor_]);
    room_nav_in_progress_ = false;
}

void ShellBase::navigate_history_forward()
{
    if (room_nav_history_.empty() || room_nav_history_cursor_ + 1 >= room_nav_history_.size())
        return;
    ++room_nav_history_cursor_;
    room_nav_in_progress_ = true;
    navigate_to_room_(room_nav_history_[room_nav_history_cursor_]);
    room_nav_in_progress_ = false;
}

void ShellBase::after_active_room_changed_()
{
    // Navigation history for Alt+Left / Alt+Right. Must be first so it
    // executes in tests (no client_) and before any early return.
    if (!current_room_id_.empty() && !room_nav_in_progress_)
    {
        // Truncate any forward entries the user had navigated back into.
        if (!room_nav_history_.empty())
        {
            room_nav_history_.erase(
                room_nav_history_.begin() +
                    static_cast<std::ptrdiff_t>(room_nav_history_cursor_) + 1,
                room_nav_history_.end());
        }
        room_nav_history_.push_back(current_room_id_);
        room_nav_history_cursor_ = room_nav_history_.size() - 1;
        // Evict oldest entry when cap is reached.
        if (room_nav_history_.size() > kNavHistoryMax)
        {
            room_nav_history_.erase(room_nav_history_.begin());
            if (room_nav_history_cursor_ > 0)
                --room_nav_history_cursor_;
        }
    }

    // Drop the room we just left: cancel its still-pending timeline media
    // downloads (full-size images, thumbnails) so the room we're switching to
    // gets the semaphore slots instead of queueing behind the old room's flood.
    // Done before the early-return so closing the last tab also cancels.
    const std::uint64_t new_group = media_group_for_room_(current_room_id_);
    if (active_media_group_ != 0 && active_media_group_ != new_group)
        cancel_media_group_(active_media_group_);
    active_media_group_ = new_group;

    if (!client_ || current_room_id_.empty())
        return;

    // Record the visit for the quick switcher's "Recent" strip — move this
    // room to the front of the MRU list (visit order), de-duplicating and
    // capping the length.
    {
        auto& v = recent_room_ids_;
        v.erase(std::remove(v.begin(), v.end(), current_room_id_), v.end());
        v.insert(v.begin(), current_room_id_);
        if (v.size() > kRecentRoomsMax)
            v.resize(kRecentRoomsMax);
    }

    // Each new room starts with an unknown thread history — allow pagination.
    threads_reached_start_ = false;
    threads_paginating_    = false;
    // Keep an always-on background subscription on the active room so the
    // threads button reflects whether the room contains threads — long before
    // the user opens the panel. subscribe_room_threads is idempotent (aborts
    // any prior subscription for this room) and the RoomSwitch transition
    // already released the outgoing room's handle.
    client_->subscribe_room_threads(current_room_id_);
    // Seed the button from the local snapshot. On a first visit the service
    // hasn't paginated yet so this is empty, which correctly hides the
    // button; on_threads_updated will reveal it once roots are discovered.
    apply_threads_list_(client_->list_room_threads(current_room_id_));
    // Refresh the pinned-events banner immediately on room switch so the new
    // room's pin state appears without waiting for the next sync tick.
    refresh_pinned_for_current_room_();
    // MSC4278: prefetch the room's media-preview override + join rule so the
    // timeline gating (and Private-mode evaluation) is ready before the
    // timeline reset builds rows.
    ensure_room_preview_override_(current_room_id_);
}

void ShellBase::ensure_settings_controller_()
{
    settings_controller_ = std::make_unique<tesseract::SettingsController>(
        client_,
        [this](std::function<void()> fn) { post_to_ui_(std::move(fn)); },
        [this](std::function<void()> fn) { run_async_(std::move(fn)); },
        [this](std::function<void(std::vector<uint8_t>, std::string)> cb)
        { pick_image_file_(std::move(cb)); });
    // UnifiedPush up-connector (Linux only); nullptr elsewhere — a no-op.
    settings_controller_->set_up_connector(
        active_account_ ? active_account_->up_connector.get() : nullptr);
    bind_settings_controller_();
}

void ShellBase::pick_and_set_room_avatar_(const std::string& room_id)
{
    auto* c = client_;
    if (!c)
        return;

    pick_image_file_(
        [this, c, room_id](std::vector<uint8_t> bytes, std::string mime) mutable
        {
            if (bytes.empty())
                return; // cancelled
            if (c != client_)
                return; // logged out between pick and callback
            auto sess = active_account_;
            run_async_mut_(
                [sess, room_id,
                 bytes = std::move(bytes),
                 mime  = std::move(mime)]() mutable
                {
                    if (!sess || !sess->client)
                        return;
                    auto upload = sess->client->upload_media(bytes, mime);
                    if (!upload.ok)
                        return;
                    sess->client->set_room_avatar(room_id, upload.message);
                });
        });
}

// ── Encryption setup detection ───────────────────────────────────────────────

uint8_t ShellBase::read_recovery_state_() const
{
    return client_ ? static_cast<uint8_t>(client_->recovery_state()) : 0u;
}

bool ShellBase::read_own_identity_exists_() const
{
    return client_ ? client_->own_identity_exists() : false;
}

bool ShellBase::read_device_verified_() const
{
    return client_ ? client_->device_verified() : false;
}

void ShellBase::handle_offline_ui_()
{
    offline_ = true;
    if (main_app_) main_app_->set_offline(true);
    request_relayout_();
}

void ShellBase::handle_online_ui_()
{
    offline_ = false;
    if (main_app_) main_app_->set_offline(false);
    request_relayout_();
}

void ShellBase::handle_enable_recovery_progress_ui_(uint8_t  step,
                                                    std::string recovery_key,
                                                    uint32_t backed_up,
                                                    uint32_t total)
{
    if (auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr)
        ov->advance_progress(step, recovery_key, backed_up, total);
}

void ShellBase::wire_encryption_setup_callbacks_(
    views::EncryptionSetupOverlay& ov,
    tk::Host&                      host,
    tk::NativeTextField*           passphrase_field,
    tk::NativeTextField*           key_field)
{
    tk::Host* host_ptr = &host;

    ov.get_passphrase = [passphrase_field]() -> std::string {
        return passphrase_field ? passphrase_field->text() : std::string();
    };
    ov.get_key_input = [key_field]() -> std::string {
        return key_field ? key_field->text() : std::string();
    };

    ov.on_enable_recovery = [this](std::string passphrase) {
        auto sess = active_account_;
        run_async_mut_([sess, passphrase]() {
            if (!sess || !sess->client) return;
            sess->client->enable_recovery(passphrase);
        });
    };

    ov.on_recover = [this](std::string key) {
        auto sess = active_account_;
        run_async_mut_([this, sess, key]() {
            if (!sess || !sess->client) return;
            auto res = sess->client->recover(key);
            if (!res.ok)
            {
                post_to_ui_alive_([this, msg = std::string(res.message)]() {
                    if (auto* o =
                            main_app_ ? main_app_->encryption_setup() : nullptr)
                        o->advance_progress(5, msg, 0, 0);
                });
            }
        });
    };

    ov.on_request_sas = [this]() {
        encryption_setup_dismissed_ = true;
        if (main_app_) main_app_->show_encryption_setup(false);
        auto sess = active_account_;
        run_async_mut_([sess]() {
            if (!sess || !sess->client) return;
            sess->client->request_self_verification();
        });
        request_relayout_();
    };

    ov.on_close = [this]() {
        encryption_setup_dismissed_ = true;
        if (main_app_) main_app_->show_encryption_setup(false);
        request_relayout_();
    };

    ov.on_copy_to_clipboard = [host_ptr](std::string text) {
        host_ptr->set_clipboard_text(text);
    };

    ov.on_layout_changed = [this]() { request_relayout_(); };
}

void ShellBase::check_encryption_setup_()
{
    if (encryption_setup_shown_ || encryption_setup_dismissed_)
        return;

    using Mode      = tesseract::views::EncryptionSetupOverlay::Mode;
    const uint8_t state = read_recovery_state_();

    if (state == 1) // Disabled → secret storage not set up on this account
    {
        // Disabled is ambiguous: it covers a truly fresh account (no
        // cross-signing anywhere → bootstrap a new identity via the Fresh
        // path) AND an account whose cross-signing identity was set up on
        // another device but without server-side secret storage. In the
        // latter case this device can't bootstrap over the existing identity;
        // the Fresh path would only create an inconsistent secret store and
        // would never verify this device. Route those to Recover (enter the
        // recovery key, or hand off to SAS) instead.
        //
        // We only treat the identity as "foreign" when it exists AND this
        // device is not verified. On a pristine account our own login-time
        // bootstrap creates the identity and signs this device together, so
        // identity-exists implies verified there → Fresh, as intended.
        const bool foreign_identity =
            read_own_identity_exists_() && !read_device_verified_();
        encryption_setup_shown_ = true;
        show_encryption_setup_overlay_(foreign_identity ? Mode::Recover
                                                        : Mode::Fresh);
    }
    else if (state == 3) // Incomplete → existing encryption, device needs secrets
    {
        encryption_setup_shown_ = true;
        show_encryption_setup_overlay_(Mode::Recover);
    }
    // Unknown (0) and Enabled (2): do nothing; re-checked on next tick.
}

void ShellBase::begin_crypto_identity_reset_()
{
    if (!client_ || !main_app_)
        return;

    // User-initiated, so bypass the recovery_state gating in
    // check_encryption_setup_ and drive the overlay directly. Reuse the
    // per-shell overlay setup (Fresh mode wires the post-approval recovery-key
    // flow + native fields), then put it into the reset-approval wait.
    encryption_setup_dismissed_ = false;
    encryption_setup_shown_     = true;
    show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode::Fresh);

    auto* ov = main_app_->encryption_setup();
    if (!ov)
        return;

    // show_encryption_setup_overlay_ → reset() cleared callbacks, so set the
    // cancel hook now (after wiring).
    ov->on_cancel_reset = [this]() {
        auto sess = active_account_;
        run_async_mut_([sess]() {
            if (!sess || !sess->client) return;
            sess->client->cancel_reset_crypto_identity();
        });
        encryption_setup_dismissed_ = true;
        if (main_app_)
            main_app_->show_encryption_setup(false);
        request_relayout_();
    };
    ov->begin_reset_wait();
    request_relayout_();

    auto sess = active_account_;
    run_async_mut_([this, sess]() {
        if (!sess || !sess->client) return;
        auto r = sess->client->begin_reset_crypto_identity();
        post_to_ui_alive_([this, ok = r.ok, needs = r.needs_approval,
                     msg = std::string(r.message),
                     url = std::string(r.approval_url)]() {
            auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
            if (!o)
                return;
            if (!ok)
            {
                o->report_reset_error(msg);
            }
            else if (!needs)
            {
                // Reset completed with no browser approval needed — go straight
                // to recovery-key setup.
                o->reset_approved();
            }
            else
            {
                // Wait for the user to approve in the browser; the SDK polls
                // and fires on_crypto_reset_result when it resolves.
                tesseract::Client::open_in_browser(url);
            }
            request_relayout_();
        });
    });
}

void ShellBase::handle_crypto_reset_result_ui_(bool ok, std::string message)
{
    auto* o = main_app_ ? main_app_->encryption_setup() : nullptr;
    if (!o)
        return;
    if (ok)
        o->reset_approved(); // → Fresh recovery-key setup (ChooseMethod)
    else
        o->report_reset_error(message);
    request_relayout_();
}

void ShellBase::set_initial_account(std::shared_ptr<AccountSession> account)
{
    active_account_ = std::move(account);
}

bool ShellBase::is_secondary_window_startup_() const
{
    return !account_manager_.accounts().empty() && active_account_ && !client_;
}

void ShellBase::on_account_picker_select_(const std::string& uid)
{
    if (auto* win = account_manager_.dedicated_window(uid))
    {
        win->raise_and_activate_();
        return;
    }
    if (is_ctrl_held_())
    {
        auto session = account_manager_.find(uid);
        if (session)
            spawn_main_window_(session);
        return;
    }
    switch_active_account_(uid);
}

// ---------------------------------------------------------------------------
// Sync-error handling (reconnect / soft-logout recovery / relogin)
// ---------------------------------------------------------------------------

void ShellBase::restart_account_sync_(const std::string& user_id)
{
    auto sess = account_manager_.find(user_id);
    if (sess && !sess->sync_started && sess->client)
    {
        sess->sync_started = true;
        sess->client->start_sync(sess->bridge.get());
    }
}

void ShellBase::schedule_sync_restart_(const std::string& user_id, int delay_ms)
{
    // The timer is native (post_to_ui_after_ → QTimer / g_timeout_add /
    // SetTimer / dispatch_after); the body is the shared restart helper.
    post_to_ui_after_(delay_ms,
                      [this, user_id]() { restart_account_sync_(user_id); });
}

void ShellBase::handle_sync_error_impl_(std::string context,
                                        std::string user_id,
                                        std::string description,
                                        bool soft_logout)
{
    auto affected = account_manager_.find(user_id);

    if (context == "sync_reconnect")
    {
        show_status_message_("Sync error: reconnecting\xe2\x80\xa6");
        if (affected && affected->client)
        {
            affected->client->stop_sync();
            affected->sync_started = false;
            schedule_sync_restart_(affected->user_id, 5000);
        }
        return;
    }

    if (context == "sync_auth_error")
    {
        if (soft_logout && affected && affected->client)
        {
            if (auto saved =
                    tesseract::SessionStore::load_account(affected->user_id))
            {
                show_status_message_("Reconnecting session\xe2\x80\xa6");
                if (affected->client->restore_session(*saved))
                {
                    // Re-fetch identity onto the AccountSession. This is the
                    // piece macOS used to skip, leaving a stale display name.
                    affected->display_name =
                        affected->client->get_display_name();
                    affected->avatar_url = affected->client->get_avatar_url();
                    // Re-bind this window's identity strip when the affected
                    // account is the one currently shown here.
                    if (active_account_ && affected == active_account_)
                    {
                        my_user_id_ = affected->user_id;
                        my_display_name_ = affected->display_name;
                        my_avatar_url_ = affected->avatar_url;
                        refresh_user_strip_();
                    }
                    affected->sync_started = true;
                    affected->client->start_sync(affected->bridge.get());
                    show_status_message_("Reconnected");
                    return;
                }
            }
        }
        // Unrecoverable: clear the stored account, stop sync, relogin.
        if (affected)
        {
            tesseract::SessionStore::clear_account(affected->user_id);
            if (affected->client)
            {
                affected->client->stop_sync();
            }
            affected->sync_started = false;
        }
        show_status_message_("Session expired; please log in again.");
        request_relogin_(user_id);
        return;
    }

    // Any other sync error: surface the description.
    show_status_message_(std::move(description));
}

} // namespace tesseract
