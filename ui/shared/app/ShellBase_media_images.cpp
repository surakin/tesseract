#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "app/Launch.h"
#include <tesseract/crash_handler.h>
#include <tesseract/version.h>
#include "app/MediaPlaybackHub.h"
#include "app/RoomPane.h"
#include "app/RoomWindowBase.h"
#include "app/SearchBackend.h"
#include "app/SlashCommands.h"
#include "app/shell_helpers.h"
#include "app/UnreadPrefetch.h"
#include "app/media_preview_policy.h"
#include "tk/blurhash.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/text_util.h"
#include "tk/theme.h"
#include "views/AddRoomView.h"
#include "views/CreateRoomView.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/JoinRoomView.h"
#include "views/ConfirmDialog.h"
#include "views/MainAppWidget.h"
#include "views/VideoViewerOverlay.h"
#include "views/RoomListView.h"
#include "views/InviteDialog.h"
#include "views/text_util.h"
#include "views/RoomSearchBar.h"
#include "views/SettingsView.h"
#include "views/RoomView.h"
#include "views/UserInfo.h"
#include "tk/image_sniff.h"
#include "views/html_spans.h"
#include "views/image_pack_order.h"
#include "views/map_tiles.h"
#include "views/pronoun_utils.h"
#include "views/thread_unread.h"
#include <tesseract/paths.h>
#include <tesseract/session_store.h>
#include <tesseract/prefs.h>
#include <tesseract/secret_store.h>
#include <tesseract/settings.h>
#include <tesseract/visual.h>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace tesseract
{

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
    avatar_mxcs_.insert(mxc);
    // When the user opts into prefetching full media, warm account_manager_.image_cache() with
    // the full-size avatar so opening it in the viewer is instant. Idempotent.
    if (tesseract::Settings::instance().prefetch_full_media)
    {
        ensure_media_image_(mxc, 0, 0);
    }
    if (account_manager_.thumbnail_cache().contains(tk::CacheKey::media(mxc)) ||
        account_manager_.anim_cache().has(avatar_anim_key_(mxc)))
    {
        return;
    }
    // Scale the requested pixel size to the display's current scale factor
    // so the server-generated thumbnail stays sharp on HiDPI — see
    // current_scale_'s doc comment in ShellBase.h.
    const int avatar_px = avatar_px_();
    // Thumbnail and full-size fetches of the same mxc must not collide on the
    // disk cache or in the in-flight set — namespace the thumbnail keys. An
    // animated-thumbnail response differs from the still one for the same
    // mxc, so it gets its own namespace too.
    const bool animated = animate_avatars_effective_();
    const std::string disk_id = animated ? "anim:" + mxc : mxc;
    const std::string tkey = thumb_key(disk_id, avatar_px, avatar_px);
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
    fetch_media_pipeline_(mxc, tk::CacheKey::thumbnail(disk_id, avatar_px, avatar_px),
                          tkey, /*group_id=*/0, kind, source,
                          avatar_px, avatar_px,
                          animated, MediaKind::RoomAvatar);
}

void ShellBase::ensure_user_avatar_(const std::string& mxc,
                                    std::uint64_t group_id)
{
    if (mxc.empty() || media_decode_failed_.count(mxc) ||
        media_fetch_backed_off_(mxc))
    {
        return;
    }
    avatar_mxcs_.insert(mxc);
    if (tesseract::Settings::instance().prefetch_full_media)
    {
        ensure_media_image_(mxc, 0, 0);
    }
    if (account_manager_.thumbnail_cache().contains(tk::CacheKey::media(mxc)) ||
        account_manager_.anim_cache().has(avatar_anim_key_(mxc)))
    {
        return;
    }
    const int avatar_px = avatar_px_();
    const bool animated = animate_avatars_effective_();
    const std::string disk_id = animated ? "anim:" + mxc : mxc;
    const std::string tkey = thumb_key(disk_id, avatar_px, avatar_px);
    if (!media_fetches_in_flight_.insert(tkey).second)
    {
        return;
    }
    // Timeline-row callers pass the active room's group so leaving cancels
    // them; account-wide callers (quick switcher roster, invites) pass the
    // default 0 — those avatars are reused across rooms and cheap to
    // re-fetch, so there's nothing to gain from cancelling on room switch.
    fetch_media_pipeline_(mxc, tk::CacheKey::thumbnail(disk_id, avatar_px, avatar_px),
                          tkey, group_id,
                          tesseract::Client::MediaReqKind::MxcThumbnail, mxc,
                          avatar_px, avatar_px,
                          animated, MediaKind::UserAvatar);
}

bool ShellBase::animate_avatars_effective_() const
{
    return tesseract::Settings::instance().animate_avatars && !low_power_active();
}

int ShellBase::avatar_px_() const
{
    return static_cast<int>(std::lround(visual::kAvatarCacheSize * current_scale_));
}

tk::CacheKey ShellBase::avatar_anim_key_(const std::string& mxc) const
{
    return tk::CacheKey::thumbnail(mxc, visual::kAvatarCacheSize, visual::kAvatarCacheSize);
}

const tk::Image* ShellBase::avatar_image_(const std::string& mxc)
{
    auto& anim = account_manager_.anim_cache();
    if (!anim.empty())
    {
        // One lock: marks the entry visible and (un)freezes it for low power.
        if (const auto* f = anim.peek_frame(avatar_anim_key_(mxc), low_power_active()))
        {
            start_anim_tick_();
            return f;
        }
    }
    return account_manager_.thumbnail_cache().peek(tk::CacheKey::media(mxc));
}

void ShellBase::deliver_animated_avatar_(const std::string& mxc, MediaKind kind,
                                         std::vector<std::uint8_t> bytes)
{
    const tk::CacheKey still_key = tk::CacheKey::media(mxc);
    if (account_manager_.thumbnail_cache().contains(still_key) ||
        account_manager_.anim_cache().has(avatar_anim_key_(mxc)))
    {
        return;
    }
    auto shared_bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
    run_async_(
        [this, mxc, kind, shared_bytes]() mutable
        {
            auto decoded = std::make_shared<DecodedImage>(
                decode_image_(*shared_bytes, visual::kAvatarCacheSize, visual::kAvatarCacheSize));
            post_to_ui_alive_(
                [this, mxc, kind, decoded, shared_bytes]() mutable
                {
                    const tk::CacheKey still_key = tk::CacheKey::media(mxc);
                    if (account_manager_.thumbnail_cache().contains(still_key) ||
                        account_manager_.anim_cache().has(avatar_anim_key_(mxc)))
                    {
                        return;
                    }
                    // Multi-frame → animation; single-frame → plain still. When
                    // this decoder can't read the file at all, hand the bytes to
                    // the shell's own still decode (QImageReader / GdkPixbuf /
                    // WIC / ImageIO) rather than blocking the avatar for good.
                    if (decoded->empty() ||
                        !store_decoded_media_(still_key, kind, std::move(*decoded)))
                    {
                        on_media_bytes_ready_(still_key, kind, std::move(*shared_bytes));
                        return;
                    }
                    if (main_app_ && main_app_->room_view())
                        main_app_->room_view()->notify_image_ready(mxc);
                    notify_secondary_media_ready_(mxc, kind);
                    request_repaint_();
                });
        });
}

// Runs from UserInfo::paint (via set_avatar_url_provider). That makes the
// room-change re-read a paint-time side effect, deliberately: it follows the
// lazy fetch-on-paint pattern used for every other avatar (on_avatar_needed),
// only fires while the strip is actually painted, and avoids hooking the ~13
// places current_room_id_ is assigned. The lookups are idempotent and deduped
// (own_room_avatar_in_flight_ / _dirty_), and the per-paint path itself is a
// couple of string compares plus one map find.
std::string ShellBase::strip_avatar_url_()
{
    if (!current_room_id_.empty())
    {
        // Re-read when the active room changed since the last paint: a cached
        // value could predate a change made from another client.
        if (strip_avatar_room_ != current_room_id_ || strip_avatar_user_ != my_user_id_)
        {
            strip_avatar_room_ = current_room_id_;
            strip_avatar_user_ = my_user_id_;
            strip_slot_key_ = my_user_id_ + '\n' + current_room_id_;
            request_own_room_avatar_(current_room_id_);
        }
        if (auto it = own_room_avatar_.find(strip_slot_key_);
            it != own_room_avatar_.end() && !it->second.empty())
        {
            return it->second;
        }
    }
    else
    {
        strip_avatar_room_.clear();
    }
    return my_avatar_url_;
}

void ShellBase::request_own_room_avatar_(const std::string& room_id)
{
    const auto sess = active_account_;
    const std::string slot_key = sess ? sess->user_id + '\n' + room_id : std::string{};
    if (room_id.empty() || !sess || !sess->client)
        return;
    if (!own_room_avatar_in_flight_.insert(slot_key).second)
    {
        // A lookup is already running and may read state from before the
        // change that prompted this one — run again when it finishes.
        own_room_avatar_dirty_.insert(slot_key);
        return;
    }
    run_async_(
        [this, room_id, slot_key, weak = std::weak_ptr<AccountSession>(sess)]
        {
            auto s = weak.lock();
            if (!s || !s->client)
                return;
            std::string mxc = s->client->own_room_avatar(room_id);
            post_to_ui_alive_(
                [this, room_id, slot_key, weak, mxc = std::move(mxc)]
                {
                    own_room_avatar_in_flight_.erase(slot_key);
                    auto s = weak.lock();
                    if (!s || s != active_account_)
                    {
                        own_room_avatar_dirty_.erase(slot_key);
                        return;
                    }
                    auto& slot = own_room_avatar_[slot_key];
                    if (slot != mxc)
                    {
                        slot = mxc;
                        if (room_id == current_room_id_)
                            request_repaint_();
                    }
                    if (own_room_avatar_dirty_.erase(slot_key) != 0)
                        request_own_room_avatar_(room_id);
                });
        });
}

void ShellBase::note_member_event_(const std::string& room_id, const tesseract::Event& ev)
{
    if (ev.type != tesseract::EventType::Membership)
        return;
    const auto& m = static_cast<const tesseract::MembershipStateEvent&>(ev);

    // The strip shows only the active room's own avatar; any other room is
    // re-read when it becomes active (strip_avatar_url_), so don't pay a store
    // read for the own join / profile events a sync or back-pagination
    // replays. Keep showing the old value until the re-read lands.
    if (!my_user_id_.empty() && m.target_user_id == my_user_id_ &&
        room_id == current_room_id_)
        request_own_room_avatar_(room_id);

    // Profile-only changes (display name / avatar by an already-joined member).
    using A = tesseract::MembershipAction;
    std::optional<std::string> name, avatar;
    switch (m.action)
    {
    case A::AvatarChanged:      avatar = m.target_avatar_url; break;
    case A::AvatarRemoved:      avatar = std::string{}; break;
    case A::DisplayNameChanged: name = m.target_display_name; break;
    case A::DisplayNameRemoved: name = std::string{}; break;
    case A::ProfileChanged:
        name = m.target_display_name;
        avatar = m.target_avatar_url;
        break;
    default:
        return;
    }
    // Existing rows keep the sender's old profile, so patch them (main list,
    // thread panel, pop-outs) instead of waiting for the SDK to re-resolve it,
    // and refresh the cached member list behind the room info panel.
    auto patch = [&](views::RoomView* rv)
    {
        if (!rv)
            return;
        if (auto* ml = rv->message_list())
            ml->update_member_profile(m.target_user_id, name, avatar);
        if (auto* tv = rv->thread_view())
            if (auto* tml = tv->message_list())
                tml->update_member_profile(m.target_user_id, name, avatar);
    };
    if (main_window_shows_(room_id))
    {
        patch(room_view_);
        if (main_room_pane_)
            main_room_pane_->refresh_room_members_();
    }
    dispatch_to_secondary_windows_(room_id,
        [&](RoomWindowBase* w)
        {
            patch(w->room_view());
            if (w->pane())
                w->pane()->refresh_room_members_();
        });
    request_repaint_();
}

void ShellBase::handle_animate_avatars_toggle_(bool enabled)
{
    auto& s = tesseract::Settings::instance();
    if (s.animate_avatars == enabled)
        return;
    s.animate_avatars = enabled;
    s.save_to_disk(tesseract::config_dir());
    on_avatar_animation_mode_changed_(/*evict=*/true);
}

void ShellBase::evict_avatar_caches_()
{
    // Only avatar entries go: both caches are shared with timeline thumbnails,
    // stickers and GIFs.
    for (const auto& mxc : avatar_mxcs_)
    {
        account_manager_.thumbnail_cache().evict(tk::CacheKey::media(mxc));
        account_manager_.anim_cache().erase(avatar_anim_key_(mxc));
        media_decode_failed_.erase(mxc);
        media_fetch_failed_.erase(mxc);
    }
}

void ShellBase::on_avatar_animation_mode_changed_(bool evict)
{
    ++avatar_mode_gen_;
    if (evict)
        evict_avatar_caches_();
    auto reset_tracking = [](views::RoomView* rv)
    {
        if (!rv)
            return;
        if (auto* ml = rv->message_list())
            ml->reset_visible_avatar_tracking();
        if (auto* tv = rv->thread_view())
            if (auto* tml = tv->message_list())
                tml->reset_visible_avatar_tracking();
    };
    if (main_app_)
        reset_tracking(main_app_->room_view());
    for (const auto& [rid, w] : secondary_windows_)
    {
        if (w)
            reset_tracking(w->room_view());
    }
    if (main_app_)
        request_relayout_();
}

void ShellBase::ensure_media_image_(const std::string& url, int /*max_w*/,
                                    int /*max_h*/, std::uint64_t group_id,
                                    MediaKind kind)
{
    const tk::CacheKey mem_key = tk::CacheKey::media(url);
    if (url.empty() || account_manager_.image_cache().contains(mem_key) ||
        account_manager_.anim_cache().has(mem_key) ||
        media_decode_failed_.count(url) || media_fetch_backed_off_(url))
    {
        return;
    }
    // Self-expiring decode-dedup guard, on top of (not instead of)
    // media_fetches_in_flight_ — see media_decode_pending_until_ms_'s doc
    // comment for why the set alone isn't sufficient (it dedups the FETCH,
    // not the DECODE, and its guard clears before decode has stored
    // anything). Kept even after the redundant-decode bug this exists for
    // is fixed, if it is fixed by other means, is harmless: a no-op once
    // anim_cache()/image_cache() has() the key.
    const std::int64_t now = monotonic_ms_();
    if (auto it = media_decode_pending_until_ms_.find(url);
        it != media_decode_pending_until_ms_.end() && now < it->second)
    {
        return;
    }
    if (!media_fetches_in_flight_.insert(url).second)
    {
        return;
    }
    media_decode_pending_until_ms_[url] = now + kDecodePendingWindowMs;
    // Full-size source → bulk lane. group_id is the originating room (so a
    // switch cancels it) for timeline media, or 0 for avatar/preview prefetch.
    fetch_media_pipeline_(url, tk::CacheKey::media(url), url, group_id,
                          tesseract::Client::MediaReqKind::SourceFull, url,
                          /*w=*/0, /*h=*/0, /*animated=*/false,
                          kind);
}

void ShellBase::ensure_picker_sticker_(const std::string& url)
{
    const tk::CacheKey key = picker_sticker_key_(url);
    if (url.empty() || account_manager_.image_cache().contains(key) ||
        account_manager_.anim_cache().has(key) || media_decode_failed_.count(url))
    {
        return;
    }
    // In-flight/backoff identity is the picker key's own string so a timeline
    // fetch of the same mxc (plain key) neither blocks nor satisfies this one.
    // Disk bytes are the plain media entry, shared with the timeline.
    const std::string flight_key = key.to_string();
    if (media_fetch_backed_off_(flight_key) ||
        !media_fetches_in_flight_.insert(flight_key).second)
    {
        return;
    }
    fetch_media_pipeline_(flight_key, tk::CacheKey::media(url), flight_key,
                          /*group_id=*/0,
                          tesseract::Client::MediaReqKind::SourceFull, url,
                          /*w=*/0, /*h=*/0, /*animated=*/false,
                          MediaKind::Sticker, key);
}

const tk::Image* ShellBase::viewer_image_lookup_(const std::string& mxc)
{
    // Full-resolution lightbox decode wins when present — a still decoded at
    // kViewerFullresMax (viewer_fullres_), or an animated source decoded at
    // the same bound and cached under the fullres_key_ namespace so it
    // doesn't collide with the smaller inline-capped entry under the plain
    // key (see ensure_viewer_fullres_/decode_fullres_and_store_ below).
    if (auto it = viewer_fullres_.find(tk::CacheKey::fullres(mxc));
        it != viewer_fullres_.end())
    {
        return it->second.get();
    }
    // The viewer always plays, regardless of low power mode — unconditionally
    // unpause on every lookup so it self-heals even if the timeline paused
    // this same plain media key (see the `mem_key` fallback below) before
    // the viewer was opened.
    const tk::CacheKey fullres_key = tk::CacheKey::fullres(mxc);
    account_manager_.anim_cache().set_paused(fullres_key, false);
    if (const auto* f = account_manager_.anim_cache().current_frame(fullres_key))
    {
        start_anim_tick_();
        return f;
    }
    // Otherwise the existing fallthrough: inline-capped animated frame →
    // inline full-size image → server thumbnail.
    const tk::CacheKey mem_key = tk::CacheKey::media(mxc);
    account_manager_.anim_cache().set_paused(mem_key, false);
    if (const auto* f = account_manager_.anim_cache().current_frame(mem_key))
    {
        start_anim_tick_(); // visible animated frame → keep the timer running
        return f;
    }
    if (const auto* img = account_manager_.image_cache().peek(mem_key))
    {
        return img;
    }
    return account_manager_.thumbnail_cache().peek(mem_key);
}

// Fetch + decode the full-resolution image for the lightbox viewer into
// viewer_fullres_ (keyed by the plain source token / avatar mxc), then
// relayout the main surface and every pop-out. Guards on empty / already
// cached / animated (animated falls back to ensure_media_image_ so the GIF
// keeps animating from anim_cache_) / known-decode-failed / in-flight — the
// latter three keyed by fullres_key_(). Uses a DISTINCT disk + in-flight key
// namespace (fullres_key_) from the inline ensure_media_image_ path so the
// 320px inline entry can never pre-empt the full-res decode. group 0 so a
// room switch does not cancel an open lightbox load.
void ShellBase::ensure_viewer_fullres_(const std::string& url)
{
    const std::string fkey = fullres_key_(url);
    const tk::CacheKey fckey = tk::CacheKey::fullres(url);
    // Guard on both the still map (viewer_fullres_) and the fullres-keyed
    // anim_cache entry — an animated source decodes into the latter (see
    // decode_fullres_and_store_ below) rather than the former.
    if (url.empty() || viewer_fullres_.count(fckey) ||
        account_manager_.anim_cache().has(fckey) ||
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
        [this, url, fkey, fckey]() mutable
        {
            auto disk = load_media_bytes_(fckey);
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
            const tk::CacheKey fckey = tk::CacheKey::fullres(url);
            if (persist)
            {
                store_media_bytes_(fckey, bytes);
            }
            // DecodedImage is move-only (holds unique_ptr<tk::Image>); wrap in a
            // shared_ptr so the post_to_ui_ std::function lambda stays
            // copy-constructible (mirrors decode_and_finalize_picker_).
            auto d = std::make_shared<DecodedImage>(decode_image_(
                bytes, visual::kViewerFullresMax, visual::kViewerFullresMax));
            if (d->empty() && persist)
            {
                evict_media_bytes_(fckey);
            }
            post_to_ui_alive_(
                [this, url, fkey, fckey, d]() mutable
                {
                    viewer_fullres_in_flight_.erase(fkey);
                    if (viewer_fullres_.count(fckey) ||
                        account_manager_.anim_cache().has(fckey))
                    {
                        return;
                    }
                    // Animated source: store the full-res-decoded frames
                    // under the fullres_key_ namespace (not the plain url
                    // the inline-capped timeline row uses), so the lightbox
                    // gets its own, larger decode instead of sharing —
                    // and being capped by — the inline entry.
                    if (!d->frames.empty())
                    {
                        account_manager_.anim_cache().store(
                            fckey, std::move(d->frames),
                            std::move(d->delays_ms), monotonic_ms_());
                        start_anim_tick_();
                        request_relayout_();
                        notify_secondary_media_ready_(url, MediaKind::MediaImage);
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
                        const tk::CacheKey victim = viewer_fullres_order_.front();
                        viewer_fullres_order_.erase(viewer_fullres_order_.begin());
                        if (victim != fckey)
                        {
                            viewer_fullres_.erase(victim);
                            break;
                        }
                    }
                    viewer_fullres_.emplace(fckey, std::move(d->still));
                    viewer_fullres_order_.push_back(fckey);
                    // Relayout the main surface (its viewer re-fits the larger
                    // image in arrange and polls the provider) and every pop-out
                    // (notify_image_ready + relayout).
                    request_relayout_();
                    notify_secondary_media_ready_(url, MediaKind::MediaImage);
                });
        });
}

void ShellBase::request_video_thumbnail_(const std::string& event_id,
                                         const std::string& source_token)
{
    if (video_thumb_in_flight_.insert(event_id).second)
        generate_video_thumbnail_(event_id, source_token);
}

void ShellBase::generate_video_thumbnail_(const std::string& event_id,
                                          const std::string& source_token)
{
    // "video_thumb::" (disk) and "thumb::" (memory) are deliberately distinct
    // keys: the memory key matches the sentinel MediaSource the view already
    // uses as its image_provider_ lookup token (see make_row_data), while the
    // disk key is namespaced so it can never collide with a real mxc-keyed
    // disk-cache entry.
    const tk::CacheKey key = tk::CacheKey::video_thumbnail(event_id);
    run_async_(
        [this, event_id, source_token, key]() mutable
        {
            auto disk = load_media_bytes_(key);
            post_to_ui_alive_(
                [this, event_id, source_token,
                 disk = std::move(disk)]() mutable
                {
                    if (!disk.empty())
                    {
                        // Warm path: a prior session already generated this
                        // thumbnail. No network, no video decoder involved.
                        decode_and_cache_video_thumbnail_(
                            event_id, std::move(disk),
                            /*persist=*/false);
                        // This attempt has concluded — clear the in-flight
                        // guard so a later re-trigger (e.g. after the
                        // in-memory image_cache_ entry is evicted by its own
                        // TTL/budget) can retry rather than being silently
                        // and permanently skipped.
                        video_thumb_in_flight_.erase(event_id);
                        return;
                    }
                    if (!client_)
                    {
                        video_thumb_in_flight_.erase(event_id);
                        return;
                    }
                    extract_video_first_frame_jpeg_(
                        source_token,
                        [this, event_id](std::vector<std::uint8_t> bytes)
                        {
                            if (!bytes.empty())
                            {
                                decode_and_cache_video_thumbnail_(
                                    event_id, std::move(bytes),
                                    /*persist=*/true);
                            }
                            // Concluded either way (success or decode/fetch
                            // failure) — see comment above.
                            video_thumb_in_flight_.erase(event_id);
                        });
                });
        });
}

void ShellBase::extract_video_first_frame_jpeg_(
    const std::string& source_token,
    std::function<void(std::vector<std::uint8_t>)> cb)
{
    if (!client_)
    {
        cb({});
        return;
    }
    const std::string src = source_token;
    auto req_id = begin_media_req_(0,
        [this, cb, src](std::vector<std::uint8_t> prefix_bytes) mutable
        {
            if (prefix_bytes.empty())
            {
                cb({});
                return;
            }
            // A prefix shorter than the requested cap is the entire file (the
            // fetch stops only at EOF or the cap), so a full-file fallback
            // could not decode any better.
            const bool whole_file =
                prefix_bytes.size() <
                tesseract::visual::kVideoThumbnailPrefixBytes;
            decode_video_first_frame_(
                std::move(prefix_bytes),
                [this, cb, src, whole_file](
                    std::vector<std::uint8_t> jpeg) mutable
                {
                    if (!jpeg.empty() || whole_file)
                    {
                        cb(std::move(jpeg));
                        return;
                    }
                    // Prefix wasn't enough (e.g. a non-fast-start file with
                    // its moov atom at EOF) — fall back to the full file.
                    // `done` may run on any thread (see decode_video_first_
                    // frame_), but begin_media_req_ mutates UI-thread-only
                    // state, so marshal the whole fallback to the UI thread.
                    post_to_ui_alive_(
                        [this, cb, src]() mutable
                        {
                            if (!client_)
                            {
                                cb({});
                                return;
                            }
                            auto full_req = begin_media_req_(0,
                                [this, cb](
                                    std::vector<std::uint8_t> full_bytes) mutable
                                {
                                    if (full_bytes.empty())
                                    {
                                        cb({});
                                        return;
                                    }
                                    decode_video_first_frame_(
                                        std::move(full_bytes), cb);
                                });
                            client_->fetch_source_bytes_async(full_req, src);
                        });
                });
        });
    client_->fetch_source_prefix_async(
        req_id, src, tesseract::visual::kVideoThumbnailPrefixBytes);
}

// Deliver a dropped file's extracted MediaInfo to the right compose bar.
// Safe to call from ANY thread (typically the probe's worker, or the UI
// thread for Qt's async probes): it marshals via post_to_ui_. `target` (a
// pop-out window's compose bar, guarded by `alive`) takes precedence;
// otherwise the main window's room_view_ compose bar, resolved at run time
// to avoid a dangling pointer and guarded on this shell's lifetime.
void ShellBase::post_pending_attachment_(views::MediaInfo info,
                                          views::ComposeBar* target,
                                          std::shared_ptr<bool> alive)
{
    if (target)
    {
        // Pop-out window: post to its compose bar only while it lives.
        // `alive` is a genuinely independent shared_ptr<bool> (not this
        // shell's own guard) — the pop-out it guards is a different object
        // with its own lifetime; see RoomPane's media_extract_alive_.
        post_to_ui_([target, alive = std::move(alive),
                     info = std::move(info)]() mutable
        {
            if (alive && *alive)
                target->update_pending_attachment(info);
        });
        return;
    }
    // Main window: resolve compose_bar() at run time to avoid any raw-pointer
    // lifetime hazard. guarded() is evaluated here, on the calling thread, at
    // the point the shell's liveness is read.
    post_to_ui_alive_([this, info = std::move(info)]() mutable
    {
        if (room_view_)
            room_view_->compose_bar()->update_pending_attachment(info);
    });
}

void ShellBase::decode_and_cache_video_thumbnail_(std::string event_id,
                                                   std::vector<std::uint8_t> bytes,
                                                   bool persist)
{
    run_async_(
        [this, event_id, bytes = std::move(bytes), persist]() mutable
        {
            const tk::CacheKey key = tk::CacheKey::video_thumbnail(event_id);
            if (persist)
            {
                store_media_bytes_(key, bytes);
            }
            auto d = std::make_shared<DecodedImage>(decode_image_(
                bytes, visual::kMaxInlineImageWidth, visual::kMaxInlineImageHeight));
            post_to_ui_alive_(
                [this, key, d]() mutable
                {
                    if (d->still && !account_manager_.image_cache().contains(key))
                    {
                        account_manager_.image_cache().store(key,
                                                             std::move(d->still));
                        request_relayout_();
                    }
                });
        });
}

void ShellBase::ensure_media_thumbnail_(const std::string& url, int w, int h,
                                        bool animated, std::uint64_t group_id)
{
    const tk::CacheKey mem_key = tk::CacheKey::media(url);
    if (url.empty() || account_manager_.image_cache().contains(mem_key) ||
        account_manager_.thumbnail_cache().contains(mem_key) ||
        account_manager_.anim_cache().has(mem_key) ||
        media_decode_failed_.count(url) || media_fetch_backed_off_(url))
    {
        return;
    }
    // Scale the caller's requested pixel size to the display's current
    // scale factor so the server-generated thumbnail stays sharp on HiDPI
    // — see current_scale_'s doc comment in ShellBase.h. Every
    // ensure_media_thumbnail_ caller (mention/reply avatars, link
    // previews, inline-image previews, video thumbnails, room-list
    // previews) benefits uniformly from scaling here, at the one
    // chokepoint they all share.
    w = static_cast<int>(std::lround(w * current_scale_));
    h = static_cast<int>(std::lround(h * current_scale_));
    const std::string tkey = thumb_key(url, w, h);
    if (!media_fetches_in_flight_.insert(tkey).second)
    {
        return;
    }
    fetch_media_pipeline_(url, tk::CacheKey::thumbnail(url, w, h), tkey, group_id,
                          tesseract::Client::MediaReqKind::SourceThumb, url,
                          static_cast<std::uint32_t>(w),
                          static_cast<std::uint32_t>(h), animated,
                          MediaKind::MediaThumbnail);
}

const tk::Image* ShellBase::shell_sticker_(const std::string& mxc)
{
    const tk::CacheKey key = tk::CacheKey::media(mxc);
    if (const auto* f = account_manager_.anim_cache().current_frame(key))
    {
        start_anim_tick_(); // visible animated frame → keep the timer running
        return f;
    }
    if (const auto* img = account_manager_.image_cache().peek(key))
    {
        return img;
    }
    ensure_media_image_(mxc, 64, 64, 0, MediaKind::Sticker);
    return nullptr;
}

void ShellBase::decode_and_finalize_picker_(std::string url, bool is_sticker,
                                            std::vector<std::uint8_t> bytes,
                                            bool persist)
{
    // Decode OFF the UI thread. Picker cells are bounded; reuse the inline-image
    // bound so picker bitmaps are reusable by the message list (same shared
    // tk_images_ key = the mxc url) — EXCEPT for stickers, which the message
    // list decodes at kStickerSize (see media_prefetch_decode_clamp_), not
    // the inline-image bound. Decoding stickers at a different size here
    // than the timeline does was a real bug, not just a cosmetic mismatch:
    // both paths write into the same AnimImageCache/PixmapCache entry (keyed
    // by the plain mxc, no in-flight coordination between the picker and
    // ensure_media_image_'s fetch path), so a sticker visible in the picker
    // and referenced by a timeline message at the same time raced two
    // differently-sized decodes into one cache slot — current_frame() then
    // alternated between differently-sized frames as both streams'
    // store()/append_frame() calls interleaved. Matching the size removes
    // the mismatch at its source. DecodedImage is move-only (holds
    // unique_ptr<tk::Image>); wrap it in a shared_ptr so the post_to_ui_ lambda
    // is copy-constructible (post_to_ui_ takes std::function).
    const int max_w = is_sticker ? visual::kStickerSize : visual::kMaxInlineImageWidth;
    const int max_h = is_sticker ? visual::kStickerSize : visual::kMaxInlineImageHeight;
    run_async_(
        [this, url, is_sticker, persist, max_w, max_h,
         bytes = std::move(bytes)]() mutable
        {
            const tk::CacheKey mem_key = tk::CacheKey::media(url);
            if (persist)
            {
                store_media_bytes_(mem_key, bytes);
            }
            auto d = std::make_shared<DecodedImage>(
                decode_image_(bytes, max_w, max_h));
            if (d->empty())
            {
                evict_media_bytes_(mem_key);
            }
            post_to_ui_alive_(
                [this, url, is_sticker, d]() mutable
                {
                    finalize_picker_image_(url, is_sticker, std::move(*d));
                });
        });
}

// Shared async picker-image path. Idempotent: no-op if already in
// tk_images_ / anim_cache_ / in-flight. Dedups via
// emoji_fetches_in_flight_ (is_sticker == false) or
// sticker_fetches_in_flight_ (true). io pool reads media_disk_cache_; on a
// miss the network download runs as a non-blocking fetch_media_async (bulk
// lane, group 0) so it never pins a pool thread. The decode runs on the io
// pool via decode_and_finalize_picker_ → finalize_picker_image_ (UI).
void ShellBase::ensure_picker_image_(const std::string& url, bool is_sticker)
{
    const tk::CacheKey mem_key = tk::CacheKey::media(url);
    if (url.empty() || account_manager_.image_cache().contains(mem_key) ||
        account_manager_.anim_cache().has(mem_key))
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
            auto disk = load_media_bytes_(tk::CacheKey::media(url));
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
    const tk::CacheKey mem_key = tk::CacheKey::media(url);
    if (account_manager_.image_cache().contains(mem_key) || account_manager_.anim_cache().has(mem_key))
    {
        return;
    }
    if (!d.frames.empty())
    {
        account_manager_.anim_cache().store(mem_key, std::move(d.frames), std::move(d.delays_ms),
                          monotonic_ms_());
        start_anim_tick_();
    }
    else if (d.still)
    {
        account_manager_.image_cache().store(mem_key, std::move(d.still));
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
    const tk::CacheKey tile_key = tk::CacheKey::tile(z, x, y);
    if (account_manager_.image_cache().contains(tile_key) || tile_fetch_failed_.count(key))
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

    // Read the on-disk tile cache. On a miss, fall back to a non-blocking
    // network fetch (fetch_url_async) so the 30 s tile timeout never pins a
    // pool thread. Tiles are not room-scoped → group 0 (always deliver).
    MediaFetchSpec spec;
    spec.group_id = 0;
    spec.load_disk_ = [disk_path]
    {
        std::vector<std::uint8_t> bytes;
        if (std::filesystem::exists(disk_path))
        {
            std::ifstream f(disk_path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), {});
        }
        return bytes;
    };
    spec.store_disk_ = [disk_path](const std::vector<std::uint8_t>& net)
    {
        std::error_code ec;
        std::filesystem::create_directories(disk_path.parent_path(), ec);
        if (!ec)
        {
            std::ofstream f(disk_path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(net.data()),
                    static_cast<std::streamsize>(net.size()));
        }
    };
    spec.erase_inflight_ = [this, key] { tile_fetches_in_flight_.erase(key); };
    spec.start_fetch_ = [this, url](std::uint64_t id)
    { client_->fetch_url_async(id, /*group_id=*/0, url); };
    spec.on_empty_ = [this, key] { tile_fetch_failed_.insert(key); };
    spec.deliver_ = [this, tile_key](std::vector<std::uint8_t>&& bytes)
    { on_media_bytes_ready_(tile_key, MediaKind::Tile, std::move(bytes)); };
    run_media_fetch_(std::move(spec));
}

void ShellBase::ensure_url_preview_(const std::string& url)
{
    if (url.empty() || url_previews_.count(url))
    {
        return;
    }
    // MSC4452: the homeserver turned /preview_url off, so it would only 403.
    if (!server_info_.preview_url_enabled)
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
    const std::string bh_key = "blurhash::" + event_id;
    const tk::CacheKey key = tk::CacheKey::blurhash(event_id);
    if (account_manager_.image_cache().contains(key) ||
        !blurhash_attempted_.insert(bh_key).second)
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

void ShellBase::ensure_row_media_(const Event& ev, bool fetch_avatars)
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
    // MSC4278: gate media (image/sticker/video thumbnails + URL previews)
    // behind the media-preview config. A suppressed item is not fetched until
    // the user reveals it individually. Sender avatars, reactions, voice/audio,
    // and the BlurHash placeholder are not gated.
    const std::string& gate_room =
        ev.room_id.empty() ? current_room_id_ : ev.room_id;
    // Inline media (image/sticker/video) is large, slow, and room-specific, so
    // it is grouped under the originating room and cancelled when the user
    // switches away — see cancel_media_group_ in after_active_room_changed_.
    // Sender/read-receipt avatars are room-scoped too (the same row), so they
    // share this group and get cancelled along with the rest of the row.
    const std::uint64_t media_group = media_group_for_room_(gate_room);

    if (fetch_avatars)
    {
        ensure_user_avatar_(ev.sender_avatar_url, media_group);
        for (const auto& rr : ev.read_receipts)
        {
            ensure_user_avatar_(rr.avatar_url, media_group);
        }
        if (ev.type == EventType::Membership)
        {
            ensure_user_avatar_(
                static_cast<const MembershipStateEvent&>(ev).target_avatar_url,
                media_group);
        }
    }

    // The user's own media is exempt from public-room suppression (Private
    // mode), so it is fetched here just like revealed media — otherwise the
    // placeholder would be gone but the bytes never fetched.
    const bool preview =
        media_allowed_(gate_room, !my_user_id_.empty() &&
                                      ev.sender == my_user_id_) ||
        revealed_events_.count(ev.event_id) != 0;

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
                                media_group, MediaKind::Sticker);
        }
        if (preview &&
            (!s.thumbnail || tesseract::Settings::instance().prefetch_full_media))
        {
            if (s.source)
                ensure_media_image_(s.source->fetch_token(),
                                    visual::kStickerSize, visual::kStickerSize,
                                    media_group, MediaKind::Sticker);
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
        if (preview && !vid.thumbnail && vid.source)
        {
            request_video_thumbnail_(ev.event_id, vid.source->fetch_token());
        }
    }
    for (const auto& r : ev.reactions)
    {
        if (r.source)
        {
            // Custom-emoji reaction images are row-scoped like the rest of
            // this event's media (a room with heavy reaction use can have
            // dozens of distinct ones), so they share the room's cancel group.
            ensure_media_image_(r.source->fetch_token(), 20, 20, media_group,
                                MediaKind::Reaction);
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
        if (ev.bundled_url_previews_present)
        {
            // MSC4095: sender-bundled previews win. Fetch their thumbnails;
            // only ask the homeserver for an entry that bundled no data.
            for (const auto& p : ev.bundled_url_previews)
            {
                if (p.image)
                    ensure_media_thumbnail_(p.image->fetch_token(), 64, 64,
                                            false, media_group);
                else if (!p.matched_url.empty() && !p.has_content())
                    ensure_url_preview_(p.matched_url);
            }
        }
        else
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
}

void ShellBase::ensure_row_media_(const views::MessageRowData& row,
                                   bool fetch_avatars)
{
    using Kind = views::MessageRowData::Kind;

    const std::uint64_t media_group = media_group_for_room_(current_room_id_);

    if (fetch_avatars)
    {
        ensure_user_avatar_(row.sender_avatar_url, media_group);
        for (const auto& rr : row.read_receipts)
            ensure_user_avatar_(rr.avatar_url, media_group);
        ensure_user_avatar_(row.membership_target_avatar_url, media_group);
    }

    const bool preview =
        media_allowed_(current_room_id_,
                       !my_user_id_.empty() && row.is_own) ||
        revealed_events_.count(row.event_id) != 0;

    if (row.kind == Kind::Image)
    {
        if (preview && row.thumbnail)
            ensure_media_thumbnail_(row.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight,
                                    row.image_animated, media_group);
        if (preview &&
            (!row.thumbnail ||
             tesseract::Settings::instance().prefetch_full_media))
        {
            if (row.source)
                ensure_media_image_(row.source->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, media_group);
        }
    }
    else if (row.kind == Kind::Sticker)
    {
        if (preview && row.thumbnail)
            ensure_media_image_(row.thumbnail->fetch_token(),
                                visual::kStickerSize, visual::kStickerSize,
                                media_group, MediaKind::Sticker);
        if (preview &&
            (!row.thumbnail ||
             tesseract::Settings::instance().prefetch_full_media))
        {
            if (row.source)
                ensure_media_image_(row.source->fetch_token(),
                                    visual::kStickerSize, visual::kStickerSize,
                                    media_group, MediaKind::Sticker);
        }
    }
    else if (row.kind == Kind::Voice)
    {
        if (row.audio_source)
        {
            const std::string src   = row.audio_source->fetch_token();
            const bool audio_new    = voice_prefetched_.insert(src).second;
            const bool waveform_new = row.waveform.empty() &&
                                      voice_waveform_in_flight_.insert(src).second;
            if (audio_new || waveform_new)
            {
                const std::string event_id = row.event_id;
                const std::string room_id  = current_room_id_;
                auto id = begin_media_req_(
                    /*group_id=*/0,
                    [this, src, event_id, room_id,
                     waveform_new](std::vector<std::uint8_t>&& bytes)
                    {
                        if (!waveform_new || bytes.empty())
                            return;
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
    else if (row.kind == Kind::Audio)
    {
        if (row.audio_source &&
            tesseract::Settings::instance().prefetch_full_media)
        {
            const std::string src = row.audio_source->fetch_token();
            if (voice_prefetched_.insert(src).second)
            {
                auto id = begin_media_req_(
                    /*group_id=*/0, [](std::vector<std::uint8_t>&&) {});
                client_->fetch_media_async(
                    id, /*group_id=*/0,
                    tesseract::Client::MediaReqKind::SourceFull, src, 0, 0,
                    false);
            }
        }
    }
    else if (row.kind == Kind::Video)
    {
        if (preview && row.video_has_server_thumbnail && row.thumbnail)
            ensure_media_thumbnail_(row.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, false,
                                    media_group);
        if (preview && !row.video_has_server_thumbnail && row.source)
            request_video_thumbnail_(row.event_id, row.source->fetch_token());
    }

    for (const auto& r : row.reactions)
    {
        if (r.source)
            // Same room-scoped cancel group as the rest of this row's media —
            // see the comment in the Event overload above.
            ensure_media_image_(r.source->fetch_token(), 20, 20, media_group,
                                MediaKind::Reaction);
    }

    if (!row.blurhash.empty())
        ensure_blurhash_image_(row.event_id, row.blurhash,
                               row.media_w, row.media_h);

    if (preview &&
        (row.kind == Kind::Text || row.kind == Kind::Notice ||
         row.kind == Kind::Emote || row.kind == Kind::Unhandled))
    {
        if (row.bundled_previews_present)
        {
            // MSC4095: the sender controls previews for this message. Fetch the
            // bundled thumbnails; only fall through to the homeserver for an
            // entry that bundled a matched_url but no preview data.
            for (const auto& p : row.bundled_previews)
            {
                if (p.image_source)
                    ensure_media_thumbnail_(p.image_source->fetch_token(), 64,
                                            64, false, media_group);
                else if (!p.matched_url.empty() && !p.has_content())
                    ensure_url_preview_(p.matched_url);
            }
        }
        else
        {
            std::string url;
            if (!row.formatted_body.empty())
                url = views::first_url_from_html(row.formatted_body);
            if (url.empty() && !row.body.empty())
                url = views::first_url_from_plain(row.body);
            if (!url.empty())
                ensure_url_preview_(url);
        }
    }
}

std::vector<std::uint64_t> ShellBase::resolve_visible_request_ids_(
    const std::vector<std::string>& keys) const
{
    std::vector<std::uint64_t> ids;
    ids.reserve(keys.size());
    for (const auto& k : keys)
    {
        // A key with no live request (cached, failed, or never requested) is
        // simply absent from the reverse map and skipped.
        auto it = media_key_to_req_.find(k);
        if (it != media_key_to_req_.end())
            ids.push_back(it->second);
    }
    return ids;
}

// The timeline's visible rows changed (scroll / room enter / data update):
// raise the priority of the still-pending media fetches backing the now-
// visible rows so they download ahead of the off-screen backlog. `keys` are
// the visible rows' media fetch tokens (what the view's image_provider looks
// up), as reported by MessageListView::on_visible_range_changed. Keys with
// no in-flight fetch (already cached, or never requested) are skipped.
void ShellBase::on_visible_rows_changed_(const std::vector<std::string>& keys)
{
    if (client_ && active_media_group_ != 0 && !keys.empty() &&
        !media_key_to_req_.empty())
    {
        auto ids = resolve_visible_request_ids_(keys);
        if (!ids.empty())
            client_->prioritize_media(active_media_group_, ids);
    }

    // Lazy media fetch: rows outside the initial prefetch window enter the
    // viewport as the user scrolls. Fetch their media now, deduped by
    // media_prepped_event_ids_ so each event is only processed once.
    if (!room_view_ || !room_view_->message_list())
        return;
    auto* ml = room_view_->message_list();
    auto [first, last] = ml->visible_range();
    if (first < 0)
        return;
    const auto& msgs = ml->messages();
    for (int i = first; i <= last && i < static_cast<int>(msgs.size()); ++i)
    {
        const auto& row = msgs[static_cast<std::size_t>(i)];
        if (media_prepped_event_ids_.insert(row.event_id).second)
            ensure_row_media_(row, /*fetch_avatars=*/true);
    }
}

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
            mode = shell_helpers::mode_to_settings(it->second.media_previews);
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
    auto req_id = next_request_id_++;
    pending_preview_overrides_[req_id] = room_id;
    client_->room_media_preview_override_async(req_id, room_id);
}

void ShellBase::handle_room_preview_override_ready_ui_(std::uint64_t request_id,
                                                       std::string override_json)
{
    auto it = pending_preview_overrides_.find(request_id);
    if (it == pending_preview_overrides_.end())
        return;
    std::string room_id = std::move(it->second);
    pending_preview_overrides_.erase(it);

    room_preview_override_in_flight_.erase(room_id);
    room_preview_overrides_[room_id] = tesseract::MediaPreviewOverride::from_json(override_json);
    seed_room_media_section_(room_id);

    // Now that the join rule + override are known, fetch any media that turned
    // out to be allowed in this room and re-evaluate the placeholders.
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
}

void ShellBase::handle_room_media_preview_override_updated_ui_(
    std::string user_id, std::string room_id, std::string override_json)
{
    // Only the active account's rooms drive the UI (mirrors
    // handle_media_preview_config_updated_ui_'s account guard).
    if (!active_account_ || !client_ || active_account_->user_id != user_id ||
        room_id.empty())
    {
        return;
    }

    room_preview_overrides_[room_id] = tesseract::MediaPreviewOverride::from_json(override_json);
    seed_room_media_section_(room_id);

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
}

void ShellBase::handle_room_security_state_ready_ui_(std::uint64_t request_id,
                                                     tesseract::RoomSecurityState state)
{
    auto it = pending_security_state_requests_.find(request_id);
    if (it == pending_security_state_requests_.end())
        return;
    std::string room_id = std::move(it->second);
    pending_security_state_requests_.erase(it);

    if (!room_view_)
        return;
    auto* v = room_view_->room_settings_view();
    if (!v || !v->is_open() || v->room_id() != room_id)
        return;
    v->set_security_state(state.is_encrypted, state.join_rule, state.guest_access,
                          state.history_visibility);
    request_relayout_();
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
                                media_group, MediaKind::Sticker);
        else if (row.source)
            ensure_media_image_(row.source->fetch_token(),
                                visual::kStickerSize, visual::kStickerSize,
                                media_group, MediaKind::Sticker);
    }
    else if (row.kind == K::Video)
    {
        if (row.video_has_server_thumbnail && row.thumbnail)
            ensure_media_thumbnail_(row.thumbnail->fetch_token(),
                                    visual::kMaxInlineImageWidth,
                                    visual::kMaxInlineImageHeight, false,
                                    media_group);
        else if (!row.video_has_server_thumbnail && row.source)
            request_video_thumbnail_(row.event_id, row.source->fetch_token());
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

// Called once, on the UI thread, after a successful Accept commit whose
// RoomSettingsChanges.media_override was populated (see
// apply_room_settings_, which performs the actual server write on the
// worker thread). Optimistically updates room_preview_overrides_ (so
// effective_preview_mode_ reflects the new value immediately), re-fetches
// any media that just became allowed in the open room, and repaints.
// Each of the five on_accept completion callbacks calls this — never
// called on every combo pick (that would violate the "nothing applies
// until Accept" contract every other room-settings field follows).
void ShellBase::commit_room_media_preview_override_(
    const std::string& room_id, bool has_override,
    tesseract::MediaPreviewConfig::Mode mode)
{
    if (room_id.empty())
        return;

    // Called once, on the UI thread, after a successful Accept whose
    // RoomSettingsChanges.media_override was set — the actual server write
    // already happened inside apply_room_settings_ (on the worker thread).
    // This just keeps the local cache + visible timeline in sync, exactly
    // as the old per-keystroke immediate-apply path used to, but now only
    // once per commit instead of once per combo pick.
    //
    // Optimistic local update: preserve whatever join_rule is already
    // cached (unaffected by this write; operator[] value-inits a fresh
    // MediaPreviewOverride{} — has_media_previews=false, join_rule="" — on
    // a cache miss, which is fine since ensure_room_preview_override_ will
    // typically have already populated this entry by the time the settings
    // dialog is reachable).
    tesseract::MediaPreviewOverride ov = room_preview_overrides_[room_id];
    ov.has_media_previews = has_override;
    if (has_override)
        ov.media_previews = mode;
    room_preview_overrides_[room_id] = ov;

    // Fetch media that just became allowed in the open room.
    if (room_id == current_room_id_ && room_view_ &&
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
}

// Push the effective per-room override (from room_preview_overrides_,
// defaulting to "no override" on a cache miss) into RoomSettingsView's
// Media tab, if that view is currently open and showing `room_id`. Called
// right after RoomSettingsView::open() (see each shell's
// on_room_settings_opened wiring) and again from
// handle_room_preview_override_ready_ui_, so a fetch that resolves after
// the dialog is already open still updates the combo instead of leaving
// it stuck on open()'s "Use global default" placeholder.
void ShellBase::seed_room_media_section_(const std::string& room_id)
{
    if (!room_view_)
        return;
    auto* v = room_view_->room_settings_view();
    if (!v || !v->is_open() || v->room_id() != room_id)
        return;
    auto it = room_preview_overrides_.find(room_id);
    if (it == room_preview_overrides_.end())
    {
        v->set_media_override(false, tesseract::MediaPreviewConfig::Mode::On);
        return;
    }
    v->set_media_override(it->second.has_media_previews, it->second.media_previews);
}

// Kick an async GET /state fetch (Client::fetch_room_security_state_
// async) for the four Security & Privacy tab fields and track its
// request_id in pending_security_state_requests_. No-op if not logged
// in. Called from each on_room_settings_opened handler, right after
// set_security_field_permissions/seed_room_media_section_ — the result
// lands in handle_room_security_state_ready_ui_, which pushes it into
// RoomSettingsView via set_security_state if the dialog is still open.
void ShellBase::fetch_room_security_state_(const std::string& room_id,
    const std::shared_ptr<AccountSession>& on_behalf_of)
{
    const auto acting = acting_session_(on_behalf_of);
    Client* const acting_client = acting_client_(on_behalf_of);
    EventAccountScope acting_scope(*this, acting ? acting->user_id : std::string{});
    if (!acting_client || room_id.empty())
        return;
    auto req_id = next_request_id_++;
    pending_security_state_requests_[req_id] = room_id;
    acting_client->fetch_room_security_state_async(req_id, room_id);
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
    // Kick off an async read; result arrives in handle_media_preview_config_fetched_ui_.
    client_->media_preview_config_async(next_request_id_++);
}

void ShellBase::handle_media_preview_config_fetched_ui_(std::uint64_t /*request_id*/,
                                                        std::string config_json)
{
    auto cfg = tesseract::MediaPreviewConfig::from_json(config_json);
    auto& s = tesseract::Settings::instance();
    s.media_previews = shell_helpers::mode_to_settings(cfg.media_previews);
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

void ShellBase::on_url_preview_ready_(const std::string& url,
                                      const Client::UrlPreview& preview)
{
    tesseract::views::UrlPreviewData d;
    d.title = preview.title;
    d.description = preview.description;
    if (!preview.image_mxc.empty())
        d.image_source = tesseract::MediaSource::plain(preview.image_mxc);
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

void ShellBase::handle_media_ready_ui_(std::uint64_t request_id,
                                       std::vector<std::uint8_t> bytes)
{
    auto it = pending_media_.find(request_id);
    if (it == pending_media_.end())
        return; // Cancelled / superseded — drop the late callback.
    PendingMediaReq req = std::move(it->second);
    pending_media_.erase(it);
    // Drop the reverse-map entry only if it still points at this request (a
    // newer request for the same key must not have its mapping clobbered).
    if (!req.priority_key.empty())
    {
        auto mit = media_key_to_req_.find(req.priority_key);
        if (mit != media_key_to_req_.end() && mit->second == request_id)
            media_key_to_req_.erase(mit);
    }
    on_inflight_ui_();
    if (req.on_bytes)
        req.on_bytes(std::move(bytes));
}

void ShellBase::handle_media_chunk_ui_(std::uint64_t request_id,
                                       std::vector<std::uint8_t> chunk,
                                       std::uint8_t status,
                                       std::uint64_t total_size)
{
    auto it = pending_media_streams_.find(request_id);
    if (it == pending_media_streams_.end())
        return; // Cancelled / unknown — drop the late callback.
    // status: 0 STREAM_CHUNK, 1 STREAM_DONE, 2 STREAM_FAILED,
    // 3 STREAM_FAILED_HASH — see IEventHandler::on_media_chunk.
    if (status == 0)
    {
        if (it->second.on_chunk)
            it->second.on_chunk(std::move(chunk), total_size);
        return;
    }
    PendingMediaStream req = std::move(it->second);
    pending_media_streams_.erase(it);
    on_inflight_ui_();
    if (status == 1)
    {
        if (req.on_done)
            req.on_done();
    }
    else if (req.on_failed)
    {
        req.on_failed(status);
    }
}

void ShellBase::handle_url_preview_ready_ui_(std::uint64_t request_id,
                                             std::string preview_json)
{
    auto it = pending_media_.find(request_id);
    if (it == pending_media_.end())
        return;
    PendingMediaReq req = std::move(it->second);
    pending_media_.erase(it);
    // Keep the reverse map in lockstep with pending_media_ at every removal
    // site (URL previews carry no priority_key today, but this guards against a
    // future preview that does, leaving a dangling key→dead-request entry).
    if (!req.priority_key.empty())
    {
        auto mit = media_key_to_req_.find(req.priority_key);
        if (mit != media_key_to_req_.end() && mit->second == request_id)
            media_key_to_req_.erase(mit);
    }
    on_inflight_ui_();
    if (req.on_preview)
        req.on_preview(std::move(preview_json));
}
} // namespace tesseract
