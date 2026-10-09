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

void ShellBase::run_media_fetch_(MediaFetchSpec spec)
{
    // Shared across the nested worker/UI continuations; held by shared_ptr so
    // every (copyable) std::function lambda can capture it without cloning the
    // callbacks. The Phase-1 alive_-token guarding lives in post_to_ui_alive_.
    auto s = std::make_shared<MediaFetchSpec>(std::move(spec));
    run_async_(
        "media-fetch-disk-read",
        [this, s]() mutable
        {
            // io pool: only the (fast, local) disk-cache read happens here.
            auto disk = s->load_disk_();
            post_to_ui_alive_(
                [this, s, disk = std::move(disk)]() mutable
                {
                    if (!disk.empty())
                    {
                        s->erase_inflight_();
                        s->deliver_(std::move(disk));
                        return;
                    }
                    if (!client_)
                    {
                        s->erase_inflight_();
                        return;
                    }
                    // The room may have been switched away during the disk-read
                    // hop above. The pending_media_ entry (and its cancel) only
                    // exists after this point, so cancel_media_group_ couldn't
                    // have caught it yet — suppress the now-stale download here.
                    if (s->should_deliver_ && !s->should_deliver_())
                    {
                        s->erase_inflight_();
                        return;
                    }
                    // Disk miss → non-blocking network fetch. The completion
                    // runs on the UI thread via on_media_ready → pending_media_.
                    auto id = begin_media_req_(
                        s->group_id,
                        [this, s](std::vector<std::uint8_t>&& net)
                        {
                            if (net.empty())
                            {
                                // Immediate failure — erase right away so a
                                // retry isn't blocked forever.
                                s->erase_inflight_();
                                s->on_empty_();
                                return;
                            }
                            // Persist to disk off the UI thread, then deliver —
                            // the buffer moves through (no large-image copy).
                            // erase_inflight_() is deferred until right before
                            // deliver_(), NOT here at network completion: an
                            // early erase left a window — between "bytes
                            // arrived" and "disk-cache write + deliver_
                            // actually ran" — where a second, redundant
                            // ensure_media_image_() call for the same key saw
                            // neither the in-flight guard (already cleared)
                            // nor a populated cache (store_disk_ hadn't run
                            // yet), so it dispatched its own independent
                            // fetch+decode for the same media. Two decodes
                            // racing for the same key, sometimes disagreeing
                            // on the source bytes, was the actual cause of an
                            // animated sticker's playback getting reset by a
                            // second decode splicing/overwriting the first's
                            // progress.
                            run_async_(
                                [this, s, net = std::move(net)]() mutable
                                {
                                    s->store_disk_(net);
                                    post_to_ui_alive_(
                                        [s, net = std::move(net)]() mutable
                                        {
                                            s->erase_inflight_();
                                            s->deliver_(std::move(net));
                                        });
                                });
                        },
                        // on_cancel (room switch): free the dedup key so a
                        // re-entry re-requests this media.
                        [s] { s->erase_inflight_(); },
                        s->priority_key);
                    s->start_fetch_(id);
                });
        });
}

void ShellBase::note_media_fetch_failed_(const std::string& key)
{
    auto& e    = media_fetch_failed_[key];
    e.attempts = std::min<std::uint32_t>(e.attempts + 1, 7);
    const auto delay = std::min<std::chrono::seconds>(
        std::chrono::minutes(5) * (1u << (e.attempts - 1)),
        std::chrono::minutes(30));
    e.retry_after = std::chrono::steady_clock::now() + delay;

    const auto deadline_secs =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()
        + std::chrono::duration_cast<std::chrono::seconds>(delay).count();
    // Dispatch SH_FFI off the UI thread (ffi_mu must not be acquired on the
    // UI thread while SH_FFI network calls may be holding it shared).
    const auto attempts = e.attempts;
    auto sess = active_account_;
    run_async_([sess, key, attempts, deadline_secs]()
    {
        if (sess && sess->client)
            sess->client->note_media_backoff_failed(key, attempts, deadline_secs);
    });
}

void ShellBase::note_media_fetch_ok_(const std::string& key)
{
    media_fetch_failed_.erase(key);
    auto sess = active_account_;
    run_async_([sess, key]()
    {
        if (sess && sess->client)
            sess->client->note_media_backoff_ok(key);
    });
}

// Compressed-bytes cache (L1) in front of media_disk_cache_ (L2), keyed by
// the same disk-cache key. Safe to call from the media io pool
// (compressed_cache() is internally synchronised; each media_disk_cache_ op
// is filesystem-atomic on a distinct key). Every media fetch/decode path
// goes through these instead of touching media_disk_cache_ directly.
//   load: L1 hit → return it; else disk read, populating L1 on a disk hit.
//   store: write both tiers.  evict: drop from both tiers.
std::vector<std::uint8_t>
ShellBase::load_media_bytes_(const tk::CacheKey& key) const
{
    if (auto cached = account_manager_.compressed_cache().get(key))
    {
        return *cached;
    }
    auto disk = account_manager_.media_disk_cache().load(key);
    if (!disk.empty())
    {
        account_manager_.compressed_cache().put(key, disk);
    }
    return disk;
}

void ShellBase::store_media_bytes_(const tk::CacheKey& key,
                                   const std::vector<std::uint8_t>& bytes) const
{
    account_manager_.compressed_cache().put(key, bytes);
    account_manager_.media_disk_cache().store(key, bytes);
}

void ShellBase::evict_media_bytes_(const tk::CacheKey& key) const
{
    account_manager_.compressed_cache().evict(key);
    account_manager_.media_disk_cache().evict(key);
}

// ── Pre-paint disk-cache media prefetch ─────────────────────────────────────

std::pair<int, int> ShellBase::media_prefetch_decode_clamp_(MediaKind kind)
{
    switch (kind)
    {
    case MediaKind::Sticker:
        return {tesseract::visual::kStickerSize, tesseract::visual::kStickerSize};
    case MediaKind::Reaction:
        return {20, 20};
    case MediaKind::RoomAvatar:
    case MediaKind::UserAvatar:
        // Matches each shell's avatar-decode branch of on_media_bytes_ready_
        // (e.g. ui/linux-qt/src/MainWindow.cpp) — a FIXED clamp, unscaled by
        // current_scale_ (unlike the disk key, which IS scaled — see
        // run_media_prefetch_impl_'s is_thumb-style branch below).
        return {tesseract::visual::kAvatarCacheSize,
                tesseract::visual::kAvatarCacheSize};
    default:
        return {tesseract::visual::kMaxInlineImageWidth,
                tesseract::visual::kMaxInlineImageHeight};
    }
}

bool ShellBase::media_prefetch_supports_kind_(MediaKind kind)
{
    return kind == MediaKind::MediaImage || kind == MediaKind::MediaThumbnail ||
           kind == MediaKind::Sticker || kind == MediaKind::Reaction ||
           kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar;
}

bool ShellBase::store_decoded_media_(const tk::CacheKey& cache_key, MediaKind kind,
                                      DecodedImage&& decoded)
{
    const bool is_avatar = (kind == MediaKind::RoomAvatar ||
                            kind == MediaKind::UserAvatar);
    const bool is_thumb = is_avatar || (kind == MediaKind::MediaThumbnail);
    if (is_avatar)
        avatar_mxcs_.insert(cache_key.id);
    auto& still_cache = is_thumb ? account_manager_.thumbnail_cache()
                                 : account_manager_.image_cache();
    if (still_cache.contains(cache_key) || account_manager_.anim_cache().has(cache_key) ||
        (is_avatar && account_manager_.anim_cache().has(avatar_anim_key_(cache_key.id))))
    {
        return true; // already warm (e.g. raced with the lazy-fetch path)
    }
    if (is_avatar && decoded.frames.size() > 1 && animate_avatars_effective_())
    {
        account_manager_.anim_cache().store(
            avatar_anim_key_(cache_key.id), std::move(decoded.frames),
            std::move(decoded.delays_ms), monotonic_ms_());
        start_anim_tick_();
        return true;
    }
    // Past the animated-avatar case above (setting on): an avatar that is a
    // multi-frame image is stored as its first-frame still — what the shells'
    // own on_media_bytes_ready_ RoomAvatar/UserAvatar branch does too.
    //
    // Deliberately NOT storing a multi-frame (animated) result here: this
    // prefetch pass always decodes via the plain, unwindowed decode_image_,
    // and AnimImageCache::store() refuses to let a *later* call replace an
    // entry once it already has more than one frame (see its own doc
    // comment) — specifically to stop a redundant decode from clobbering
    // real progress. If this stored the result, it would permanently lock
    // the entry into a full, unwindowed decode for whichever animated
    // sticker happened to win the race — which, since this prefetch pass is
    // scoped to exactly the visible viewport and runs every pre-paint pass
    // *before* that cell's own paint-triggered lazy fetch, is essentially
    // every on-screen animated sticker — defeating windowed decode for
    // exactly the content it matters most for. Leaving it unstored (return
    // false) lets the lazy path (ensure_media_image_ →
    // decode_image_streamed_windowed_, the only one that knows how to
    // window a long animation) handle it instead once the cell is actually
    // painted; the compressed bytes are already warm in compressed_cache_
    // from load_media_bytes_ above, so that fetch is a fast cache hit — only
    // the decode step is deferred, not the byte fetch. Callers must release
    // media_decode_pending_until_ms_ for this key when this returns false
    // (see its call sites in run_media_prefetch_impl_), so the deferred
    // lazy decode isn't itself blocked by the guard this prefetch pass set.
    if (!is_avatar && !decoded.frames.empty())
    {
        return false;
    }
    // An animated source stored as a still (setting off / low power): use its
    // first frame when the decoder reported frames only.
    if (is_avatar && !decoded.still && !decoded.frames.empty())
    {
        decoded.still = std::move(decoded.frames.front());
    }
    if (decoded.still)
    {
        still_cache.store(cache_key, std::move(decoded.still));
        return true;
    }
    return false; // decode failed
}

void ShellBase::run_media_prefetch_impl_(
    const std::vector<MediaPrefetchKey>& keys,
    std::chrono::steady_clock::time_point deadline, int max_items)
{
    // (memory key, kind, disk key) — disk key is computed here, on the UI
    // thread, because MediaThumbnail's disk key depends on current_scale_
    // (see thumb_key() below), which is only ever read/written on the UI
    // thread elsewhere; reading it from a pool_ worker task would be a race.
    struct Filtered
    {
        tk::CacheKey mem_key;
        MediaKind    kind;
        std::string  disk_key;       // in-flight-set bookkeeping identity
        tk::CacheKey disk_cache_key; // load_media_bytes_ argument
        std::uint64_t group_id = 0;  // for ensure_media_image_'s direct hand-off
    };
    std::vector<Filtered> filtered;
    filtered.reserve(std::min<std::size_t>(keys.size(),
                                            static_cast<std::size_t>(std::max(max_items, 0))));
    for (const auto& k : keys)
    {
        if (static_cast<int>(filtered.size()) >= max_items)
        {
            break;
        }
        if (!media_prefetch_supports_kind_(k.kind))
        {
            continue;
        }
        // The client-generated video-thumbnail sentinel (CacheUsage::
        // VideoThumbnail) is never fetchable this way — see
        // wire_main_app_widget_'s image_provider_ and
        // ShellBase::generate_video_thumbnail_.
        if (k.key.id.empty() || k.key.usage == tk::CacheUsage::VideoThumbnail)
        {
            continue;
        }
        const bool is_avatar = (k.kind == MediaKind::RoomAvatar ||
                                k.kind == MediaKind::UserAvatar);
        const bool is_thumb = is_avatar || (k.kind == MediaKind::MediaThumbnail);
        auto& still_cache = is_thumb ? account_manager_.thumbnail_cache()
                                     : account_manager_.image_cache();
        const tk::CacheKey mem_key = tk::CacheKey::media(k.key.id);
        if (still_cache.contains(mem_key) || account_manager_.anim_cache().has(mem_key) ||
            (is_avatar && account_manager_.anim_cache().has(avatar_anim_key_(k.key.id))))
        {
            continue; // already warm
        }
        // Mirrors ensure_media_thumbnail_'s / ensure_room_avatar_'s /
        // ensure_user_avatar_'s exact key derivation (ShellBase.cpp) — a
        // thumbnail's or an avatar's disk key is namespaced by
        // display-scaled size; every other kind's disk key is the plain
        // memory key (ensure_media_image_ uses the same string for both).
        std::string disk_key = k.key.id;
        tk::CacheKey disk_cache_key = tk::CacheKey::media(k.key.id);
        if (is_thumb)
        {
            const int sw = static_cast<int>(std::lround(k.key.w * current_scale_));
            const int sh = static_cast<int>(std::lround(k.key.h * current_scale_));
            // Avatars fetched as animations are cached under their own disk
            // namespace (see ensure_room_avatar_), so look there.
            const std::string disk_id =
                (is_avatar && animate_avatars_effective_()) ? "anim:" + k.key.id : k.key.id;
            disk_key = thumb_key(disk_id, sw, sh);
            disk_cache_key = tk::CacheKey::thumbnail(disk_id, sw, sh);
        }
        // If the lazy/network path is already fetching this key, leave it be
        // — that fetch will populate the cache and this key drops out of the
        // candidate set on its own. Racing it with a duplicate disk-only
        // task just starves its decode step on the shared pool_.
        if (media_fetches_in_flight_.count(disk_key))
        {
            continue;
        }
        // TEMP (see media_decode_pending_until_ms_'s doc comment): the lazy
        // path's media_fetches_in_flight_ guard above only covers the FETCH,
        // not the DECODE — it can already be cleared while
        // ensure_media_image_'s own decode_image_streamed_windowed_ call is
        // still in flight (e.g. the bytes just landed on disk, which is
        // exactly when this prefetch pass would find them). Without this,
        // this prefetch task can independently decode the same bytes via
        // the plain decode_image_() below — a second, entirely separate
        // decode of the same key, racing the lazy path's real one. This was
        // confirmed to be the actual remaining cause of an animated
        // sticker's playback getting corrupted/truncated even after
        // media_fetches_in_flight_'s own race was closed.
        if (auto it = media_decode_pending_until_ms_.find(disk_key);
            it != media_decode_pending_until_ms_.end() &&
            monotonic_ms_() < it->second)
        {
            continue;
        }
        // Prefetch's OWN single-flight guard (NOT media_fetches_in_flight_ —
        // see media_prefetch_in_flight_'s comment in ShellBase.h for why the
        // sets must stay separate). Stops the same key being redispatched on
        // every paint pass while its disk task is still in flight.
        if (!media_prefetch_in_flight_.insert(disk_key).second)
        {
            continue;
        }
        // Set the same guard ensure_media_image_() checks/sets, so a lazy
        // fetch triggered *after* this prefetch dispatch (rather than
        // before it, the case the check above handles) doesn't race this
        // task's own decode_image_() call either.
        media_decode_pending_until_ms_[disk_key] =
            monotonic_ms_() + kDecodePendingWindowMs;
        filtered.push_back({mem_key, k.kind, std::move(disk_key),
                            std::move(disk_cache_key), k.group_id});
    }
    if (filtered.empty())
    {
        return;
    }

    auto batch = std::make_shared<MediaPrefetchBatch>();
    batch->remaining.store(static_cast<int>(filtered.size()), std::memory_order_relaxed);
    for (auto& f : filtered)
    {
        media_prefetch_pool_.post(
            [this, batch, key = std::move(f.mem_key), kind = f.kind,
             disk_key = std::move(f.disk_key),
             disk_cache_key = std::move(f.disk_cache_key),
             group_id = f.group_id]() mutable
            {
                auto bytes = load_media_bytes_(disk_cache_key); // disk-only, no SDK/network
                std::optional<DecodedImage> decoded;
                if (!bytes.empty())
                {
                    const auto [max_w, max_h] = media_prefetch_decode_clamp_(kind);
                    decoded = decode_image_(bytes, max_w, max_h);
                }
                bool became_ready = false;
                {
                    std::lock_guard<std::mutex> lock(batch->mu);
                    if (decoded && !decoded->empty())
                    {
                        batch->ready.emplace_back(std::move(key), kind,
                                                   std::move(*decoded), group_id);
                        became_ready = true;
                    }
                }
                if (batch->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    std::lock_guard<std::mutex> lock(batch->mu);
                    batch->cv.notify_all();
                }
                // One UI hop that does two things, in order:
                //
                //  1. Frees the prefetch single-flight slot — unconditionally,
                //     regardless of success/failure or whether this finished
                //     within budget or as a straggler — so a key that couldn't
                //     resolve this pass (still decoding elsewhere, or a genuine
                //     disk-cache miss) is eligible for one redispatch on the
                //     next pass instead of never being retried (if left in the
                //     set) or being retried every pass concurrently with
                //     itself (if never inserted at all — see the insert in
                //     run_media_prefetch_impl_'s filter loop).
                //
                //  2. Runs the straggler store, but only once the UI thread
                //     has actually stopped waiting on this batch (see
                //     MediaPrefetchBatch::deadline_passed's doc comment) — so
                //     exactly one of {this straggler dispatch, the synchronous
                //     drain below} stores any given entry, instead of racing
                //     both unconditionally, which could return an on-time
                //     result to the caller before this dispatch's own
                //     store_decoded_media_ call had actually run.
                //
                // Kept as a single post_to_ui_ (not two) so the straggler
                // store stays the one and only UI callback this task posts —
                // media_prefetch_in_flight_ and the batch caches are all
                // UI-thread-only, hence the hop.
                const std::string key_id = key.id;
                post_to_ui_(
                    [this, batch, became_ready, kind, group_id, key_id,
                     disk_key = std::move(disk_key)]
                    {
                        media_prefetch_in_flight_.erase(disk_key);
                        if (!became_ready)
                        {
                            // Genuine disk-cache miss — nothing was decoded,
                            // so release the shared decode-dedup guard too
                            // (see media_decode_pending_until_ms_'s doc
                            // comment): otherwise a real fetch for this key
                            // dispatched shortly after (e.g. the user
                            // actually scrolls to it) is silently blocked
                            // for the rest of the guard's window even
                            // though nothing is actually in flight.
                            media_decode_pending_until_ms_.erase(disk_key);
                            // Hand off to the lazy fetch directly: the paint
                            // that ran right after this prefetch dispatch hit
                            // the guard above and returned without fetching,
                            // and the next paint's prefetch would just
                            // re-dispatch and re-set it, so a view with no
                            // fetch trigger outside paint (the sticker
                            // picker) would never load this key.
                            if (kind != MediaKind::RoomAvatar &&
                                kind != MediaKind::UserAvatar)
                            {
                                ensure_media_image_(key_id, 0, 0, group_id,
                                                    kind);
                            }
                            return;
                        }
                        if (!batch->deadline_passed.load(std::memory_order_acquire))
                        {
                            return;
                        }
                        std::vector<std::tuple<tk::CacheKey, MediaKind, DecodedImage,
                                               std::uint64_t>>
                            drained;
                        {
                            std::lock_guard<std::mutex> lock(batch->mu);
                            drained = std::move(batch->ready);
                            batch->ready.clear();
                        }
                        for (auto& [k2, kind2, decoded2, group_id2] : drained)
                        {
                            // Computed before store_decoded_media_ moves
                            // decoded2 — matches its own is_avatar/frames
                            // check exactly, so we know afterward whether a
                            // false return means "genuine failure" or "this
                            // animated result was deliberately deferred".
                            const bool is_avatar2 =
                                (kind2 == MediaKind::RoomAvatar ||
                                 kind2 == MediaKind::UserAvatar);
                            const bool would_defer_animated =
                                !is_avatar2 && !decoded2.frames.empty();
                            if (store_decoded_media_(k2, kind2, std::move(decoded2)))
                            {
                                if (room_view_)
                                {
                                    room_view_->notify_image_ready(k2.id);
                                }
                                notify_secondary_media_ready_(k2.id, kind2);
                            }
                            else
                            {
                                // Not stored — either a genuine failure, or
                                // (for animated content) deliberately
                                // deferred to the lazy path's windowed
                                // decoder (see store_decoded_media_'s doc
                                // comment). Release the guard either way so
                                // a fresh decode for this key isn't itself
                                // blocked by it.
                                media_decode_pending_until_ms_.erase(k2.id);
                                if (would_defer_animated)
                                {
                                    // Directly hand off to the lazy path
                                    // instead of just releasing the guard
                                    // and hoping this row's next paint pass
                                    // notices the miss: since this prefetch
                                    // pass runs before every paint, if it
                                    // remains a candidate it can keep
                                    // re-decoding and re-discarding the
                                    // same key pass after pass, faster than
                                    // ensure_media_image_'s own fetch round
                                    // trip completes — starving the one
                                    // path actually allowed to keep the
                                    // result (see this function's earlier
                                    // TEMP-guard comment and
                                    // store_decoded_media_'s doc comment).
                                    ensure_media_image_(k2.id, 0, 0, group_id2,
                                                        kind2);
                                }
                            }
                        }
                        if (!drained.empty())
                        {
                            schedule_relayout_();
                        }
                    });
            });
    }

    std::unique_lock<std::mutex> lock(batch->mu);
    const bool all_done = batch->cv.wait_until(lock, deadline, [&batch]
    { return batch->remaining.load(std::memory_order_acquire) == 0; });
    if (!all_done)
    {
        // Any task still in flight past this point must drain-and-store
        // for itself (the straggler path) — see
        // MediaPrefetchBatch::deadline_passed's doc comment.
        batch->deadline_passed.store(true, std::memory_order_release);
    }
    auto ready = std::move(batch->ready);
    batch->ready.clear();
    lock.unlock();
    // No notify_image_ready/repaint call needed here — this runs
    // synchronously before root_->paint(ctx), so the imminent paint is the
    // "repaint" that benefits from the now-warm cache.
    for (auto& [key, kind, decoded, group_id] : ready)
    {
        const bool is_avatar =
            (kind == MediaKind::RoomAvatar || kind == MediaKind::UserAvatar);
        const bool would_defer_animated = !is_avatar && !decoded.frames.empty();
        if (!store_decoded_media_(key, kind, std::move(decoded)))
        {
            // Not stored — either a genuine failure, or (for animated
            // content) deliberately deferred to the lazy path's windowed
            // decoder (see store_decoded_media_'s doc comment). Release the
            // shared decode-dedup guard either way so a fresh decode for
            // this key isn't itself blocked by it.
            media_decode_pending_until_ms_.erase(key.id);
            if (would_defer_animated)
            {
                // See the straggler drain path's identical hand-off above
                // for why this direct call — not just releasing the guard
                // — is needed.
                ensure_media_image_(key.id, 0, 0, group_id, kind);
            }
        }
    }
}

void ShellBase::run_media_prefetch_()
{
    if (!main_app_)
    {
        return;
    }
    std::vector<MediaPrefetchKey> keys;
    auto* rv = main_app_->room_view();
    if (rv)
    {
        if (auto* ml = rv->message_list(); ml && ml->visible_in_tree())
        {
            for (auto& k : ml->collect_prefetchable_media_keys())
            {
                // Room-scoped — matches ensure_media_image_'s own group_id
                // for this same row (see media_group_for_room_ call sites),
                // so a discarded animated decode's direct hand-off (see
                // store_decoded_media_'s doc comment) dispatches under the
                // group the row's own lazy fetch would use.
                k.group_id = active_media_group_;
                keys.push_back(std::move(k));
            }
        }
        if (auto* rmv = rv->room_media_view(); rmv && rmv->visible_in_tree())
        {
            for (auto& k : rmv->collect_prefetchable_media_keys())
            {
                k.group_id = active_media_group_;
                keys.push_back(std::move(k));
            }
        }
        // Neither picker is ever add_child'd into the widget tree (see
        // RoomView::emoji_picker()'s doc comment) — visibility lives on
        // RoomView itself, not the picker's own visible_in_tree().
        if (auto* sp = rv->sticker_picker(); sp && rv->sticker_picker_visible())
        {
            for (auto& k : sp->collect_prefetchable_media_keys())
            {
                keys.push_back(std::move(k));
            }
        }
        if (auto* ep = rv->emoji_picker(); ep && rv->emoji_picker_visible())
        {
            for (auto& k : ep->collect_prefetchable_media_keys())
            {
                keys.push_back(std::move(k));
            }
        }
    }
    if (auto* rlv = main_app_->room_list_view(); rlv && rlv->visible_in_tree())
    {
        for (auto& k : rlv->collect_prefetchable_media_keys())
        {
            keys.push_back(std::move(k));
        }
    }
    if (keys.empty())
    {
        return;
    }
    run_media_prefetch_impl_(keys, std::chrono::steady_clock::now() + kMediaPrefetchBudget,
                              media_prefetch_max_items_);
}

// Shared async media pipeline used by the ensure_* helpers. The network
// download runs as a non-blocking tokio task (fetch_media_async) so it does
// NOT pin a worker thread; only the small disk-cache read/write and the
// decode (inside on_media_bytes_ready_) touch the io pool. Steps:
//   1. io pool: read the C++ disk cache for `disk_key`.
//   2. UI: on a hit, deliver immediately; on a miss, register a pending
//      request and issue client_->fetch_media_async (returns at once).
//   3. UI (on_media_ready): persist to disk off-thread, then deliver via
//      on_media_bytes_ready_(cache_key, out_kind, bytes).
// Clears `inflight_key` from media_fetches_in_flight_ and runs the
// failure/ok backoff bookkeeping on `cache_key`. The caller must have
// already done the in-memory cache check and inserted `inflight_key`.
// `group_id` is the cancellation group (0 = never cancelled).
void ShellBase::fetch_media_pipeline_(
    std::string cache_key, tk::CacheKey disk_key, std::string inflight_key,
    std::uint64_t group_id, tesseract::Client::MediaReqKind kind,
    std::string source, std::uint32_t w, std::uint32_t h, bool animated,
    MediaKind out_kind)
{
    MediaFetchSpec spec;
    spec.group_id = group_id;
    // cache_key is the row's media fetch_token (what the view's image_provider
    // looks up), so registering the request under it lets a visible-row scroll
    // re-prioritize this fetch. Room-scoped media only — group 0 (avatars,
    // tiles) is never re-prioritized, so leave its key empty.
    if (group_id != 0)
        spec.priority_key = cache_key;
    spec.load_disk_ = [this, disk_key]
    { return load_media_bytes_(disk_key); };
    spec.store_disk_ = [this, disk_key](const std::vector<std::uint8_t>& b)
    { store_media_bytes_(disk_key, b); };
    spec.erase_inflight_ = [this, inflight_key]
    { media_fetches_in_flight_.erase(inflight_key); };
    // Suppress a now-stale download if the room was switched away during the
    // disk-read hop. Avatars/previews use group 0 (never cancelled) → always
    // deliver. Only the room-scoped pipeline gates on active_media_group_.
    spec.should_deliver_ = [this, group_id]
    {
        return group_id == 0 || group_id == active_media_group_ ||
               active_media_view_groups_.count(group_id) != 0 ||
               active_popout_media_groups_.count(group_id) != 0;
    };
    spec.start_fetch_ =
        [this, group_id, kind, source, w, h, animated, cache_key](std::uint64_t id)
    {
        const auto prio = media_fetch_failed_.count(cache_key)
            ? tesseract::Client::MediaPriority::Backoff
            : tesseract::Client::MediaPriority::Normal;
        client_->fetch_media_async(id, group_id, kind, source, w, h, animated, prio);
    };
    spec.on_empty_ = [this, cache_key, out_kind]
    {
        note_media_fetch_failed_(cache_key);
        // Release the decode-dedup guard set at dispatch time so a retry
        // (backoff or the user reopening the room) isn't silently dropped
        // for up to kDecodePendingWindowMs. No-op for kinds that never set
        // it (avatars/tiles).
        media_decode_pending_until_ms_.erase(cache_key);
        on_media_bytes_ready_(tk::CacheKey::media(cache_key), out_kind, {});
    };
    spec.deliver_ =
        [this, cache_key, out_kind, animated,
         gen = avatar_mode_gen_](std::vector<std::uint8_t>&& bytes)
    {
        note_media_fetch_ok_(cache_key);
        // Fetched under a different avatar animation mode than the current one
        // (setting toggled / low power entered or left mid-flight): the bytes
        // are the wrong variant — a still would mask the animation, an
        // animation would be decoded just to be shown as a still. Drop them;
        // the repaint re-requests the avatar in the current mode.
        if ((out_kind == MediaKind::RoomAvatar || out_kind == MediaKind::UserAvatar) &&
            gen != avatar_mode_gen_)
        {
            request_repaint_();
            return;
        }
        // An animated avatar bypasses the shells' still-only avatar branch.
        if (animated &&
            (out_kind == MediaKind::RoomAvatar || out_kind == MediaKind::UserAvatar) &&
            tk::bytes_may_be_animated(bytes))
        {
            deliver_animated_avatar_(cache_key, out_kind, std::move(bytes));
            return;
        }
        on_media_bytes_ready_(tk::CacheKey::media(cache_key), out_kind, std::move(bytes));
    };
    run_media_fetch_(std::move(spec));
}

// Non-blocking voice/audio byte provider for the playback path. Returns the
// clip's bytes if already warmed (moving them out of voice_bytes_cache_),
// otherwise kicks a one-shot async download (fetch_media_async) and returns
// empty; `on_ready` fires on the UI thread when the download lands so the
// caller can repaint and the user can replay. Replaces the blocking
// fetch_source_bytes that previously froze the UI on an uncached clip.
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

// Estimate how many trailing rows of a freshly-loaded snapshot could
// plausibly be on screen, for build_rows_()'s synchronous media-prefetch
// window. Real per-row heights (text wrap, inline images) aren't known
// until the new rows are laid out, so this uses the message list's
// current (stable, content-independent) viewport height divided by a
// deliberately small per-row estimate — biased to overestimate rather
// than under-fetch. Any row this window misses still gets its media via
// on_visible_rows_changed_ once the real layout runs.
std::size_t ShellBase::media_prefetch_window_() const
{
    // Deliberately small: shorter than any real row (which always has at
    // least avatar-height + padding, or a line of body text), so dividing by
    // it overestimates row count rather than under-fetching.
    constexpr float kConservativeRowHeightPx = 20.0f;
    // Fallback for the rare case bounds aren't established yet (e.g. before
    // the shell's first layout pass) — small, since on_visible_rows_changed_
    // covers anything this window misses once real heights are known.
    constexpr std::size_t kFallbackWindow = 20;
    if (!room_view_ || !room_view_->message_list())
        return kFallbackWindow;
    const float h = room_view_->message_list()->bounds().h;
    if (h <= 0.0f)
        return kFallbackWindow;
    return static_cast<std::size_t>(std::ceil(h / kConservativeRowHeightPx));
}

std::uint64_t ShellBase::video_memory_bytes_() const
{
    std::uint64_t total = 0;
    if (room_view_)
        total += room_view_->video_memory_bytes();
    if (main_app_ && main_app_->video_viewer())
        total += main_app_->video_viewer()->memory_bytes();
    for (const auto& w : owned_secondary_windows_)
    {
        if (!w)
            continue;
        if (w->room_view())
            total += w->room_view()->video_memory_bytes();
        if (w->video_viewer())
            total += w->video_viewer()->memory_bytes();
    }
    return total;
}

void ShellBase::run_image_gc_()
{
    auto activity_scope = activity_.begin("image-gc", "UI housekeeping", "periodic");
    // Retired inline video players hold a whole clip each; free the idle ones
    // on every tick, whether or not the activity gate below lets a cycle run.
    for_each_room_view_([](views::RoomView& rv)
                        { rv.release_idle_video_players(kVideoRetiredTtl); });
    // Generational mark-and-sweep of the decoded (L0) image caches. peek() (which
    // every image provider calls) stamps the current generation onto the touched
    // entry. Here we: gate on recent user activity (idle ⇒ no cycle ⇒ nothing
    // evicted ⇒ on-screen images frozen), bump the generation, force every
    // surface to fully repaint (re-peeking everything visible at the new gen),
    // then a beat later evict entries not peeked within the last 2 generations.
    // image_gc_should_run() dedupes across all windows so this body runs once
    // per ~2 s regardless of how many shells tick their timer.
    //
    // anim_cache().sweep() reclaims by wall-clock TTL since an entry's last
    // paint (see AnimImageCache::sweep()'s doc comment), completely
    // independent of image_gc_should_run()'s own activity gate — which is
    // exactly the bug: while every window is hidden/minimized, nothing
    // paints, so nothing refreshes an entry's last_seen_ms, but this timer
    // keeps ticking every ~2s regardless (unlike tick_anim_(), which stops
    // outright once any_window_visible_() is false). A sticker that was
    // happily animating right before the window was hidden gets its entry
    // deleted out from under it once ttl_ms_ elapses — restoring the window
    // then shows one freshly-decoded frame and, having nothing left to
    // build on, is right back where it started. Skip both sweep() calls
    // below while nothing is visible anywhere in the app: nothing can be
    // reclaimed as "not looked at in a while" when nothing could possibly
    // have been looked at.
    bool any_visible = false;
    for (ShellBase* w : account_manager_.all_windows())
    {
        if (w && w->any_window_visible_())
        {
            any_visible = true;
            break;
        }
    }

    if (!account_manager_.image_gc_should_run())
    {
        // Even when the GC is idle-gated off, keep the animated cache's own
        // visibility sweep running — it is cheap and self-contained.
        if (any_visible)
        {
            account_manager_.anim_cache().sweep();
        }
        return;
    }

    account_manager_.image_cache().advance_generation();
    account_manager_.thumbnail_cache().advance_generation();
    // Video mark pass rides the same forced full repaint: painting a video row
    // touch()es its inline player at the new generation.
    for (ShellBase* w : account_manager_.all_windows())
    {
        w->for_each_room_view_([](views::RoomView& rv)
                               { rv.advance_video_generation(); });
    }

    for (ShellBase* w : account_manager_.all_windows())
    {
        w->force_full_repaint_all_surfaces_();
    }

    post_to_ui_after_(50, guarded(
                              [this, any_visible]
                              {
                                  account_manager_.image_cache().retain_recent(2);
                                  account_manager_.thumbnail_cache().retain_recent(2);
                                  for (ShellBase* w : account_manager_.all_windows())
                                  {
                                      w->for_each_room_view_(
                                          [](views::RoomView& rv)
                                          {
                                              rv.sweep_video_players(
                                                  kVideoKeepGenerations,
                                                  kVideoRetiredTtl);
                                          });
                                  }
                                  if (any_visible)
                                  {
                                      account_manager_.anim_cache().sweep();
                                  }
                              }));
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
            uint64_t video_bytes = 0;
            for (ShellBase* w : account_manager_.all_windows())
                video_bytes += w->video_memory_bytes_();
            uint64_t voice_bytes = 0;
            for (const auto& kv : voice_bytes_cache_)
                voice_bytes += kv.second.size();
            // PixmapCache::total_bytes_all_instances() covers the avatar and
            // thumbnail caches plus the small per-host/per-view pill caches.
            const uint64_t memory =
                static_cast<uint64_t>(tk::PixmapCache::total_bytes_all_instances()) +
                account_manager_.anim_cache().current_bytes() +
                account_manager_.compressed_cache().current_bytes() +
                sum_image_map_bytes_(viewer_fullres_) + voice_bytes + video_bytes +
                shell_extra_memory_bytes_();
            const uint64_t mem_hits =
                account_manager_.image_cache().hits() + account_manager_.thumbnail_cache().hits() +
                account_manager_.anim_cache().hits() +
                account_manager_.compressed_cache().hits();
            const uint64_t mem_misses =
                account_manager_.image_cache().misses() + account_manager_.thumbnail_cache().misses() +
                account_manager_.anim_cache().misses() +
                account_manager_.compressed_cache().misses();
            const uint64_t disk_hits   = account_manager_.media_disk_cache().hits();
            const uint64_t disk_misses = account_manager_.media_disk_cache().misses();
            cb(local, sdk, memory, mem_hits, mem_misses, disk_hits, disk_misses);
        });
    });
}

// Async: delete all on-disk caches best-effort (media files, waveform DB),
// clear in-memory image maps, reinit the waveform store, then hand off to
// restart_sdk_begin_() for the full SDK wipe + in-place re-restore and UI
// rebuild, which calls recompute_callback with fresh sizes when it lands.
// Refuses (status message, no-op) while a call or device-verification is in
// flight. No-op when not signed in.
void ShellBase::clear_all_caches_(
    std::function<void(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t, uint64_t, uint64_t)> recompute_callback)
{
    if (my_user_id_.empty())
        return;

    // The wipe drops the matrix Client an active call / device-verification
    // depends on — refuse rather than yank it out from under them.
    if (active_call())
    {
        show_status_message_(tk::tr("End your call before clearing the cache."));
        return;
    }
    if (encryption_flow_.has_flow())
    {
        show_status_message_(
            tk::tr("Finish verifying your device before clearing the cache."));
        return;
    }

    run_async_([this, recalc = std::move(recompute_callback)]() mutable
    {
        // Waveform SQLite — best-effort (locked on Windows if WAL is open).
        std::error_code ec;
        std::filesystem::remove(tesseract::cache_dir() / "waveforms.db", ec);

        // The MediaDiskCache is owned by the shared AccountManager and is
        // touched (load/store/prune/evict) from every window's worker pool.
        // It has no internal lock — concurrent ops are only safe because each
        // op is filesystem-atomic on a distinct key; clear() (remove_all +
        // recreate dir) is *not* in that set and would race a concurrent
        // load/store/prune on another window's worker. Defer it to the UI
        // thread alongside the in-memory clears.
        post_to_ui_alive_([this, recalc = std::move(recalc)]() mutable
        {
            account_manager_.media_disk_cache().clear();
            account_manager_.compressed_cache().clear();
            account_manager_.thumbnail_cache().clear();
            account_manager_.image_cache().clear();
            account_manager_.anim_cache().clear();
            media_decode_failed_.clear();
            media_fetch_failed_.clear();
            url_previews_.clear();
            url_preview_data_.clear();
            url_preview_in_flight_.clear();
            blurhash_attempted_.clear();
            tile_fetch_failed_.clear();
            voice_bytes_cache_.clear();
            tesseract::init_waveform_cache(
                (tesseract::cache_dir() / "waveforms.db").string());

            // The media-fetch backoff table lives in app_cache.db, which
            // clear_caches() now deletes wholesale — no separate clear needed.

            // SDK wipe + in-place re-restore + UI rebuild; the recompute runs
            // from its final (UI-thread) phase so it observes the cleared state.
            restart_sdk_begin_(std::move(recalc));
        });
    });
}
} // namespace tesseract
