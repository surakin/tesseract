#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward-declare types owned or cached here. Use the same headers that
// ShellBase.h currently includes for these types.
#include <tesseract/account_session.h>
#include "tk/pixmap_cache.h"
#include "tk/anim_image_cache.h"
#include "tk/media_disk_cache.h"
#include "tk/compressed_image_cache.h"
#include "app/SearchBackend.h"
#include "app/MediaPlaybackHub.h"
#include <tesseract/paths.h>           // tesseract::cache_dir()

#include <chrono>
#include <cstdint>

namespace tesseract { class ShellBase; }

namespace tesseract {

class AccountManager
{
public:
    AccountManager();
    ~AccountManager();

    // Sessions — ShellBase creates AccountSession objects (because they
    // contain a platform bridge) and hands them over here for storage.
    void add_account(std::shared_ptr<AccountSession> session);
    void remove_account(std::string_view user_id);
    std::shared_ptr<AccountSession> find(std::string_view user_id) const;
    std::span<std::shared_ptr<AccountSession> const> accounts() const;

    // Tracks user_ids whose AccountSession has been logged out but may still
    // be referenced by a stale run_async_mut_ worker that was queued or
    // in-flight before the logout began (see
    // ShellBase::logout_active_account_impl_). Unlike every other member of
    // this class — deliberately UI-thread-only, unlocked — this piece is
    // also touched from mut_pool_'s worker thread, so it gets its own
    // mutex/condition variable.
    //
    // Call mark_draining() right after remove_account() so there is never a
    // window where a session is neither findable nor flagged as draining.
    void mark_draining(std::string_view user_id);
    // Clears the flag and wakes any wait_until_drained() waiters. Called from
    // the mut_pool_ drain-barrier task once every earlier-queued task that
    // could have captured the session has finished.
    void clear_draining(std::string_view user_id);
    bool is_draining(std::string_view user_id) const;
    // Blocks the calling thread up to `timeout` for user_id to stop
    // draining. Returns true if it was (or became) non-draining in time,
    // false on timeout — the flag is left set on timeout, not force-cleared,
    // so a later caller still sees the truth and can wait again rather than
    // racing the still-in-flight drain.
    bool wait_until_drained(std::string_view user_id,
                            std::chrono::milliseconds timeout) const;

    // Shared media caches — previously owned by ShellBase.
    tk::PixmapCache& thumbnail_cache() { return thumbnail_cache_; }
    tk::PixmapCache& image_cache()     { return image_cache_; }
    tk::AnimImageCache& anim_cache()   { return anim_cache_; }
    tk::MediaDiskCache& media_disk_cache() { return media_disk_cache_; }
    // L1 tier of still-encoded bytes in front of media_disk_cache() — see
    // tk::CompressedImageCache. Keyed by the same disk-cache key.
    tk::CompressedImageCache& compressed_cache() { return compressed_cache_; }

    // ── Image GC gating (see ShellBase::run_image_gc_) ─────────────────────────
    // Every shell forwards user input here. The GC cycle only runs while the
    // user has been doing something recently, so an idle window's on-screen
    // images are frozen (never evicted) rather than aged out.
    void note_image_gc_activity()
    {
        last_gc_activity_ = std::chrono::steady_clock::now();
    }
    // True at most once per ~1.5 s across all windows, and only if there was
    // input within the last ~10 s. Records the run time when it returns true.
    bool image_gc_should_run()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_gc_run_ < std::chrono::milliseconds{1500})
        {
            return false;
        }
        if (now - last_gc_activity_ > std::chrono::seconds{10})
        {
            return false;
        }
        last_gc_run_ = now;
        return true;
    }

    // Window registry
    void register_window(ShellBase* w);
    void unregister_window(ShellBase* w);
    void set_dedicated(std::string_view user_id, ShellBase* w);
    void clear_dedicated(std::string_view user_id);
    ShellBase* dedicated_window(std::string_view user_id) const;
    int window_count() const;
    std::span<ShellBase* const> all_windows() const;

    // The first window to register (the startup window). Stays valid for the
    // app's lifetime under hide-to-tray (the startup window hides rather than
    // closes). Used as the fallback owner when a popped-out window closes and
    // hands its account's event bridge back.
    ShellBase* primary_window() const;

    // App-wide tray ownership: exactly one window owns the single tray icon.
    // claim_tray_owner returns true iff the caller is now (or already) the owner;
    // every later caller gets false and skips tray creation. Multi-window: keeps
    // one tray icon regardless of how many windows are open.
    bool claim_tray_owner(ShellBase* w);
    void release_tray_owner(ShellBase* w);
    bool is_tray_owner(ShellBase* w) const;
    ShellBase* tray_owner() const;

    // App-wide search-provider-D-Bus-service ownership: same single-instance
    // idiom as claim_tray_owner, since the GNOME Shell search provider (GTK4)
    // and KRunner plugin (Qt6) are each one process-wide D-Bus object, not
    // one per window.
    bool claim_search_provider_owner(ShellBase* w);
    void release_search_provider_owner(ShellBase* w);
    bool is_search_provider_owner(ShellBase* w) const;

    // App-wide MPRIS ownership — same idiom, a distinct D-Bus service from
    // the search provider so a window may own one, both, or neither.
    bool claim_mpris_owner(ShellBase* w);
    void release_mpris_owner(ShellBase* w);
    bool is_mpris_owner(ShellBase* w) const;

    // Registry backing the GNOME Shell search provider / KRunner plugin (see
    // SearchBackend.h). One instance per process, alongside the window
    // registry above — every ShellBase registers here regardless of shell,
    // since only the Linux D-Bus adapters ever query it.
    SearchBackend& search_backend() { return search_backend_; }

    // Process-wide "now playing" surface backing MPRIS (GtkMprisPlayer /
    // QtMprisPlayer) — see MediaPlaybackHub.h.
    MediaPlaybackHub& media_playback_hub() { return media_playback_hub_; }

    // Process-wide correlation IDs for user-initiated message-media uploads.
    // Zero is deliberately skipped because older callers used it as a
    // non-correlatable sentinel.
    std::uint64_t next_upload_request_id();

private:
    std::vector<std::shared_ptr<AccountSession>> accounts_;

    // Backing state for mark_draining/clear_draining/is_draining/
    // wait_until_drained — see the doc comment above. mutable so
    // wait_until_drained()/is_draining() can lock from a const method.
    mutable std::mutex draining_mu_;
    mutable std::condition_variable draining_cv_;
    std::unordered_set<std::string> draining_ids_;

    std::vector<ShellBase*>                     all_windows_;
    std::unordered_map<std::string, ShellBase*> dedicated_windows_;
    ShellBase*                                  primary_window_        = nullptr;
    ShellBase*                                  tray_owner_            = nullptr;
    ShellBase*                                  search_provider_owner_ = nullptr;
    ShellBase*                                  mpris_owner_           = nullptr;

    // The decoded (L0) caches are GC'd to the on-screen set by a generational
    // mark-and-sweep (peek() is the mark; ShellBase::run_image_gc_ +
    // PixmapCache::retain_recent are the sweep). The byte budgets are just a
    // safety ceiling. TTL is unused (retain_recent is the eviction policy).
    tk::PixmapCache    thumbnail_cache_{48u * 1024u * 1024u};
    tk::PixmapCache    image_cache_{64u * 1024u * 1024u};
    tk::AnimImageCache anim_cache_;
    tk::MediaDiskCache media_disk_cache_{tesseract::cache_dir() / "media"};
    tk::CompressedImageCache compressed_cache_{32u * 1024u * 1024u};

    std::chrono::steady_clock::time_point last_gc_activity_{};
    std::chrono::steady_clock::time_point last_gc_run_{};
    SearchBackend      search_backend_;
    MediaPlaybackHub   media_playback_hub_;
    std::uint64_t      next_upload_request_id_ = 1;
};

} // namespace tesseract
