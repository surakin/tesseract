#pragma once

// Value types and pure key helpers for ShellBase's media pipeline (request
// registry, fetch/prefetch plumbing, streamed decode). Split out of ShellBase.h
// unchanged; ShellBase re-exports each type as a nested alias, and keeps
// forwarding statics for the key helpers, so shells and tests are untouched.

#include <tesseract/visual.h>
#include "tk/anim_decode_session.h"
#include "tk/canvas.h"
#include "tk/media_kind.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace tesseract
{

using MediaKind = tk::MediaKind;

struct MediaFetchBackoff
{
    std::uint32_t attempts = 0;
    std::chrono::steady_clock::time_point retry_after;
};

struct PendingMediaReq
{
    std::uint64_t group_id = 0;
    // Exactly one is set, matching the request type.
    std::function<void(std::vector<std::uint8_t>&&)> on_bytes;
    std::function<void(std::string&&)> on_preview;
    // Run when the request is cancelled (room switch) instead of completing.
    // Clears the caller's dedup-set key so the media can be re-requested on
    // re-entry; without this the key would stay stuck in-flight forever.
    std::function<void()> on_cancel;
    // Display/cache key this request feeds (the row's media fetch_token), or
    // empty for requests not tied to a visible row. Used to drop the
    // media_key_to_req_ reverse-map entry when the request ends.
    std::string priority_key;
};

struct PendingMediaStream
{
    std::uint64_t group_id = 0;
    // total_size is the declared HTTP Content-Length (0 if unknown) —
    // see IEventHandler::on_media_chunk's doc comment.
    std::function<void(std::vector<std::uint8_t>&&, std::uint64_t)> on_chunk;
    std::function<void()> on_done;
    // status is 2 (STREAM_FAILED) or 3 (STREAM_FAILED_HASH) — see
    // IEventHandler::on_media_chunk's doc comment.
    std::function<void(std::uint8_t)> on_failed;
};

// Result of a worker-thread decode. Exactly one of `still` /
// `frames` is populated (frames non-empty ⇒ animated).
struct DecodedImage
{
    std::unique_ptr<tk::Image> still;
    std::vector<std::unique_ptr<tk::Image>> frames;
    std::vector<int> delays_ms;
    bool empty() const
    {
        return !still && frames.empty();
    }
};

// ── Unified raw-bytes media-fetch pipeline ────────────────────────────────
// The variable bits of the disk-load → UI hop → hit-deliver / miss-fetch →
// persist → deliver async dance shared by fetch_media_pipeline_ and
// ensure_tile_async. Each callback runs on the thread noted below; the
// worker-thread ones (load_disk_/store_disk_) execute on the io pool, the
// rest on the UI thread (already guarded by post_to_ui_alive_). The helper
// owns the alive_-token lifetime guarding for every UI-thread continuation.
struct MediaFetchSpec
{
    // Worker thread: read the backing cache for this entry. Empty ⇒ miss.
    std::function<std::vector<std::uint8_t>()> load_disk_;
    // Worker thread: persist freshly-fetched bytes before delivery.
    std::function<void(const std::vector<std::uint8_t>&)> store_disk_;
    // UI thread: clear the caller's in-flight/dedup key.
    std::function<void()> erase_inflight_;
    // UI thread: the cancellation group for this request (0 = never cancel).
    std::uint64_t group_id = 0;
    // UI thread: still want this delivery? Returns false ⇒ suppress (stale).
    // Defaults to always-deliver; only the room-scoped pipeline overrides it.
    std::function<bool()> should_deliver_;
    // UI thread: issue the SDK fetch for the allocated request id.
    std::function<void(std::uint64_t /*req_id*/)> start_fetch_;
    // UI thread: a miss-fetch returned empty bytes (network failure).
    std::function<void()> on_empty_;
    // UI thread: deliver final bytes (hit or post-fetch). The helper has
    // already erased the in-flight key before calling this.
    std::function<void(std::vector<std::uint8_t>&&)> deliver_;
    // UI thread: the row display/cache key this fetch feeds, registered in
    // media_key_to_req_ so a visible-row scroll can re-prioritize it. Empty
    // for fetches not tied to a visible row (e.g. map tiles).
    std::string priority_key;
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    // Screenshot mode: served from a local file, so no client_ is needed.
    bool screenshot_local_ = false;
#endif
#ifdef TESSERACT_SCREENSHOT_MODE_ENABLED
    bool screenshot_local() const
    {
        return screenshot_local_;
    }
#else
    static bool screenshot_local()
    {
        return false;
    }
#endif
};

// Shared state for one frame's prefetch batch. Lives only for the
// duration of run_media_prefetch_impl_(), held alive by shared_ptr in
// every task posted to pool_ so a straggler task that outlives the
// UI-thread wait is still safe to complete into.
struct MediaPrefetchBatch
{
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int> remaining{0};
    // Set by the UI thread iff its wait_until() timed out (did NOT see
    // remaining reach 0 in time) — i.e. "I have already stopped
    // draining this batch, any task that finishes from here on must
    // drain-and-store for itself". Without this, a task that finishes
    // within budget could still race its own post_to_ui_ dispatch
    // against the UI thread's synchronous drain, sometimes "winning"
    // and deferring an on-time result to a later UI-thread turn for no
    // reason (harmless, but pointless) — or, worse, the UI thread could
    // observe remaining==0 and return before that same straggler
    // dispatch has actually run store_decoded_media_, making a result
    // that decoded in time still miss this frame's paint. Gating the
    // straggler dispatch on this flag makes exactly one of the two
    // drains responsible for any given entry, deterministically.
    std::atomic<bool> deadline_passed{false};
    // Decoded results not yet applied to a cache. Populated by worker
    // tasks under mu; drained by whichever of (a) the UI-thread bounded
    // wait in run_media_prefetch_impl_ (only when it did NOT time out),
    // (b) a straggler task's own post_to_ui_ callback (only when
    // deadline_passed is set) — never both, so every entry is drained
    // exactly once.
    std::vector<std::tuple<tk::CacheKey, MediaKind, DecodedImage, std::uint64_t>> ready;
};

// The (on_first, on_extra) pair make_streamed_decode_callbacks_windowed_
// builds. on_first's signature matches decode_image_streamed_windowed_'s
// on_first_frame parameter (session/total_frames delivered as arguments,
// not via an out-param — see that function's doc comment for why).
struct WindowedStreamedDecodeCallbacks
{
    std::function<void(std::unique_ptr<tk::Image>, int, std::shared_ptr<tk::AnimDecodeSession>,
                       std::size_t)>
        on_first;
    std::function<void(int, std::unique_ptr<tk::Image>, int)> on_extra;
};

// Size-namespaced cache key for thumbnail fetches (disk + in-flight set).
// Canonical format lives in tesseract::visual::thumb_key (also used by the
// desktop search D-Bus adapters, which aren't ShellBase subclasses).
inline std::string media_thumb_key(const std::string& key, int w, int h)
{
    return tesseract::visual::thumb_key(key, w, h);
}

// Disk-cache + in-flight key for the full-resolution viewer fetch. Namespaced
// so it never collides with the inline ensure_media_image_ entry (plain url).
inline std::string media_fullres_key(const std::string& url)
{
    return "fullres:" + url;
}

// Disk-cache key for a GIF strip's source bytes. Namespaced so a Klipy CDN URL
// never collides with an mxc:// media key in the shared media_disk_cache_.
inline std::string media_gif_src_disk_key(const std::string& url)
{
    return "gifsrc:" + url;
}

// Stable non-zero group id for a room's media (so a switch cancels the right
// set). 0 is reserved for ungrouped / never-cancelled requests.
inline std::uint64_t media_group_for_room(const std::string& room_id)
{
    if (room_id.empty())
    {
        return 0;
    }
    std::uint64_t h = std::hash<std::string>{}(room_id);
    return h == 0 ? 1 : h;
}

} // namespace tesseract
