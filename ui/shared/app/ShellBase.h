#pragma once
#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/paths.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>
#include <tesseract/visual.h>
#include "tk/anim_image_cache.h"
#include "tk/canvas.h"
#include "tk/media_disk_cache.h"
#include "tk/theme.h"
#include "app/RoomWindowBase.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tesseract {

// ShellBase holds all state and platform-agnostic logic that is identical
// across the Qt6, GTK4, Win32, and macOS shells. Platform-specific concerns
// (UI widget manipulation, image decode, thread-dispatch mechanism) are
// isolated to a small set of pure-virtual hooks that each shell overrides.
//
// Wiring a new shell: inherit publicly from ShellBase, remove the member
// variables it owns, implement the virtual hooks below, and call the
// concrete helpers (ensure_row_media_, push_rooms_, etc.) instead of the
// per-shell duplicates.
class ShellBase {
    // EventHandlerBase calls post_to_ui_, push_rooms_, push_room_list_state_,
    // and all the handle_*_ui_ / on_room_list_state_ui_ virtuals from lambdas
    // captured on the worker thread — these are protected but EventHandlerBase
    // is not a subclass, so grant friendship.
    friend class EventHandlerBase;
    // RoomWindowBase accesses client_, rooms_, current_room_id_,
    // pagination_, my_user_id_, and post_to_ui_ from its helper methods.
    friend class RoomWindowBase;

public:
    virtual ~ShellBase() = default;

    // Open room_id in a new native window. If a secondary window for that room
    // is already open, it is raised instead of duplicated. The platform shell
    // must override create_secondary_room_window_() for this to have effect.
    void open_room_in_new_window(const std::string& room_id);

protected:
    // ── Multi-account ─────────────────────────────────────────────────────────
    std::vector<std::unique_ptr<AccountSession>> accounts_;
    int            active_account_index_       = -1;
    Client*        client_                     = nullptr; // non-owning alias
    IEventHandler* event_handler_              = nullptr; // non-owning alias

    std::unordered_map<std::string, std::vector<RoomInfo>> per_account_rooms_;

    std::unique_ptr<Client>   pending_login_client_;
    std::filesystem::path     pending_login_temp_dir_;
    bool pending_login_is_add_account_ = false;
    int  add_account_return_idx_       = -1;

    // ── Active-account identity ───────────────────────────────────────────────
    std::string my_user_id_;
    std::string my_display_name_;
    std::string my_avatar_url_;

    // ── Rooms ─────────────────────────────────────────────────────────────────
    std::vector<RoomInfo> rooms_;
    std::string           current_room_id_;
    std::string           pending_restore_room_;
    std::vector<std::string> space_stack_;
    bool                  compose_typing_active_   = false;
    bool                  typing_bar_visible_      = false;

    // ── Image caches ──────────────────────────────────────────────────────────
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_avatars_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_images_;
    tk::AnimImageCache    anim_cache_;
    tk::MediaDiskCache    media_disk_cache_{tesseract::cache_dir() / "media"};
    bool                  media_disk_cache_pruned_ = false;

    // ── Media fetch dedup sets ────────────────────────────────────────────────
    std::unordered_set<std::string> voice_prefetched_;
    std::unordered_set<std::string> video_thumb_in_flight_;
    std::unordered_set<std::string> reply_details_requested_;
    std::unordered_set<std::string> media_fetches_in_flight_;
    std::unordered_set<std::string> sticker_fetches_in_flight_;

    // ── URL preview cache ─────────────────────────────────────────────────────
    std::unordered_map<std::string, tesseract::Client::UrlPreview> url_previews_;
    std::unordered_set<std::string> url_preview_in_flight_;

    // ── BlurHash decode dedup ────────────────────────────────────────────────
    std::unordered_set<std::string> blurhash_attempted_;

    // ── Sync / backup state ───────────────────────────────────────────────────
    RoomListState  last_room_list_state_ = RoomListState::Init;
    BackupState    last_backup_state_    = BackupState::Unknown;
    std::uint64_t  last_imported_keys_   = 0;
    bool           sync_progress_shown_  = false;

    // ── Recovery ──────────────────────────────────────────────────────────────
    bool recovery_banner_dismissed_ = false;

    // ── Cross-signing / SAS device verification ───────────────────────────────
    bool        verification_banner_dismissed_  = false;
    std::string active_verification_flow_id_;   // "" = no flow in progress

    // ── Pagination ────────────────────────────────────────────────────────────
    struct PaginationState {
        bool        in_flight      = false;
        bool        reached_start  = false;
        bool        fwd_in_flight  = false;   // forward paginate guard
        bool        reached_end    = false;
        bool        is_focused     = false;   // true = using with_focus timeline
        std::string focus_event_id;           // scroll target after timeline reset
    };
    std::unordered_map<std::string, PaginationState> pagination_;

    // ── Read receipts ─────────────────────────────────────────────────────────
    // room_id → last event_id for which a receipt was sent in this session.
    std::unordered_map<std::string, std::string> last_sent_receipt_;
    static constexpr std::uint16_t kPaginationBatch = 50;

    // ── Secondary (pop-out) room windows ──────────────────────────────────────
    // One window per room_id at most (raise-existing policy).
    // owned_secondary_windows_ holds lifetime; secondary_windows_ is a fast-
    // lookup index into it (raw pointers, always valid while owned_ holds them).
    std::vector<std::unique_ptr<RoomWindowBase>>     owned_secondary_windows_;
    std::unordered_map<std::string, RoomWindowBase*> secondary_windows_;
    // Ref-count of active subscriptions per room_id across all secondary windows.
    std::unordered_map<std::string, int> room_subscription_refs_;

    // ── Worker threads ────────────────────────────────────────────────────────
    std::atomic<bool>       shutting_down_{false};
    std::mutex              workers_mu_;
    std::condition_variable workers_cv_;
    int                     workers_in_flight_ = 0;

    // ── Media kind tag ────────────────────────────────────────────────────────
    enum class MediaKind : std::uint8_t {
        RoomAvatar, // → tk_avatars_, triggers room-list repaint
        UserAvatar, // → tk_avatars_, triggers message-list repaint
        MediaImage, // → anim_cache_ or tk_images_, triggers message-list repaint
    };

    // ── Theme ─────────────────────────────────────────────────────────────────

    // Returns the OS-preferred color scheme. Default: Light.
    // Each platform shell overrides with its native API.
    virtual tk::ThemeMode os_color_scheme_() const { return tk::ThemeMode::Light; }

    // Apply theme to all surfaces owned by this shell. Called on the UI thread.
    // Each platform shell overrides to call set_theme() on each of its surfaces.
    virtual void apply_theme_ui_(const tk::Theme&) {}

    // Resolve the current ThemePreference to a concrete ThemeMode (calling
    // os_color_scheme_() for System), then call apply_theme_ui_.
    void apply_current_theme_();

    // Change the stored preference, save to disk, then call apply_current_theme_.
    void set_theme_preference_(tesseract::Settings::ThemePreference pref);

    // ── Abstract platform hooks ───────────────────────────────────────────────

    // Post fn() onto the UI thread.
    // GTK4: g_idle_add   Qt6: QueuedConnection   Win32: PostMessage   macOS: dispatch_async
    virtual void post_to_ui_(std::function<void()> fn) = 0;

    // Called after rooms_ is updated — shell refreshes the room-list widget.
    virtual void on_rooms_updated_() = 0;

    // Called on the UI thread when async media bytes arrive.
    // Shell decodes the bytes, stores a tk::Image in tk_avatars_ or tk_images_
    // (or calls anim_cache_.store), and triggers a repaint.
    virtual void on_media_bytes_ready_(const std::string& cache_key,
                                        MediaKind kind,
                                        std::vector<uint8_t> bytes) = 0;

    // Client-side first-frame generation for m.video when the server provides
    // no thumbnail.  Default is a no-op; shells with a video-decode pipeline
    // (GTK4/GStreamer, Qt6/QMediaPlayer, etc.) override this.
    virtual void generate_video_thumbnail_(const std::string& /*event_id*/,
                                            const std::string& /*video_url*/) {}

    // Called on the UI thread when a URL preview fetch completes successfully.
    // Default is a no-op; shells override to update their preview cache and
    // request a repaint.
    virtual void on_url_preview_ready_(const std::string& /*url*/,
                                        const Client::UrlPreview& /*preview*/) {}

    // MSC2448: store a decoded RGBA8888 buffer as a tk::Image in tk_images_.
    // Default is a no-op; each platform shell overrides with native image creation.
    virtual void cache_rgba_image_(const std::string& /*key*/, int /*w*/, int /*h*/,
                                   std::vector<uint8_t> /*rgba*/) {}

    // ── EventHandlerBase UI-thread hooks ─────────────────────────────────────
    // Called on the UI thread by EventHandlerBase after marshaling. Default
    // implementations are no-ops; each shell overrides what it needs.

    virtual void handle_timeline_reset_ui_(
        std::string /*room_id*/,
        std::vector<std::unique_ptr<Event>> /*snapshot*/) {}
    virtual void handle_message_inserted_ui_(
        std::string /*room_id*/, std::size_t /*index*/,
        std::unique_ptr<Event> /*ev*/) {}
    virtual void handle_message_updated_ui_(
        std::string /*room_id*/, std::size_t /*index*/,
        std::unique_ptr<Event> /*ev*/) {}
    virtual void handle_message_removed_ui_(
        std::string /*room_id*/, std::size_t /*index*/) {}
    virtual void handle_sync_error_ui_(
        std::string /*context*/, std::string /*user_id*/,
        std::string /*description*/, bool /*soft_logout*/) {}
    virtual void handle_backup_progress_ui_(BackupProgress /*progress*/) {}
    virtual void handle_image_packs_updated_ui_() {}
    virtual void handle_account_prefs_updated_ui_(
        std::string /*user_id*/, std::string /*json*/) {}
    virtual void handle_notification_ui_(
        std::string /*user_id*/, std::string /*room_id*/,
        std::string /*room_name*/, std::string /*sender*/,
        std::string /*body*/, bool /*is_mention*/,
        std::vector<uint8_t> /*avatar_bytes*/) {}
    // Called after push_room_list_state_() — shell refreshes its sync-status display.
    virtual void on_room_list_state_ui_() {}

    // ── Verification banner hooks (default no-op) ──────────────────────────────
    virtual void handle_verification_request_ui_(
        std::string /*flow_id*/, std::string /*user_id*/,
        std::string /*device_id*/, bool /*incoming*/) {}
    virtual void handle_sas_ready_ui_(
        std::string /*flow_id*/, std::vector<VerificationEmoji> /*emojis*/) {}
    virtual void handle_verification_done_ui_(std::string /*flow_id*/) {}
    virtual void handle_verification_cancelled_ui_(
        std::string /*flow_id*/, std::string /*reason*/) {}
    virtual void handle_verification_state_ui_(bool /*is_verified*/) {}

    // ── Typing notification hooks ─────────────────────────────────────────────
    // Called on the UI thread by EventHandlerBase. Filters by current_room_id_,
    // formats the display text, and calls update_typing_bar_.
    void handle_typing_changed_ui_(std::string room_id,
                                    std::vector<std::string> names);
    // Override in each shell to push text into the platform typing-bar widget.
    // text is empty when no one is typing.
    virtual void update_typing_bar_(const std::string& /*text*/, bool /*visible*/) {}

    // ── Compose typing send helpers ───────────────────────────────────────────
    // Call from the shell's NativeTextArea on_changed callback.
    void handle_compose_text_changed_(const std::string& text);
    // Call BEFORE updating current_room_id_ on room switch / account switch.
    void handle_compose_room_leaving_(const std::string& old_room_id);

    // ── Secondary window registry ─────────────────────────────────────────────

    // Register/unregister a secondary window. Called by RoomWindowBase.
    void register_room_window_  (RoomWindowBase* w);
    void unregister_room_window_(RoomWindowBase* w);
    // Remove the owning unique_ptr for w from owned_secondary_windows_,
    // destroying the C++ object. Called by RoomWindowBase::schedule_self_close_()
    // via post_to_ui_ so deletion happens outside the window's own message handler.
    void release_owned_window_  (RoomWindowBase* w);

    // Subscription ref-counting for secondary windows. acquire_() starts an
    // async subscribe_room when the ref goes from 0→1 (unless the main window
    // already holds the subscription). release_() unsubscribes when the ref
    // goes from 1→0 and the main window is not showing that room.
    void acquire_room_subscription_(const std::string& room_id);
    void release_room_subscription_(const std::string& room_id);

    // Call fn on the secondary window showing room_id, if one is open.
    void dispatch_to_secondary_windows_(
        const std::string& room_id,
        const std::function<void(RoomWindowBase*)>& fn);

    // Override in a platform shell to instantiate the concrete RoomWindow
    // subclass. The subclass constructor must call finish_init_() before
    // returning. Default: no-op (secondary windows are inert until a shell
    // overrides this).
    virtual RoomWindowBase* create_secondary_room_window_(
        const std::string& /*room_id*/) { return nullptr; }

    // ── Concrete helpers ──────────────────────────────────────────────────────

    // Spawn a worker thread.  fn() runs off the UI thread and must not touch
    // UI state directly — use post_to_ui_ to bounce results back.
    void run_async_(std::function<void()> fn);

    // Avatar / media prefetch — each method is idempotent (dedup via the
    // media_fetches_in_flight_ set + cache-presence check).
    void ensure_room_avatar_(const RoomInfo& r);
    void ensure_user_avatar_(const std::string& mxc);
    void ensure_media_image_(const std::string& url, int max_w, int max_h);

    // Fire a synchronous SDK call to fetch reply-to metadata.
    void ensure_reply_details_(const std::string& event_id);

    // Fetch OpenGraph preview metadata for `url` from the homeserver.
    // Idempotent — deduplicates in-flight fetches and skips already-cached URLs.
    void ensure_url_preview_(const std::string& url);

    // Decode the BlurHash for `event_id` and store the result via cache_rgba_image_.
    // Idempotent — skips events already attempted or already cached.
    void ensure_blurhash_image_(const std::string& event_id,
                                const std::string& hash,
                                int media_w, int media_h);

    // Walk all media references in ev and call ensure_*_ for each.
    void ensure_row_media_(const Event& ev);

    // Update the rooms cache and call on_rooms_updated_() for the active account.
    void push_rooms_(std::string user_id, std::vector<RoomInfo> rooms);

    // Mark pagination as complete for room_id.
    void push_paginate_result_(std::string room_id, bool reached_start);

    // MSC3030: begin a focused-timeline subscription centred on event_id.
    void begin_focused_subscription_(const std::string& room_id,
                                      const std::string& event_id);

    // MSC3030: clear stale focused-timeline state when (re-)entering a room via
    // the live room-selection path.  Must be called before subscribe_room() so
    // that the subsequent handle_timeline_reset_ui_() sees is_focused == false.
    void clear_focused_state_(const std::string& room_id);

    // MSC3030: paginate forward in a focused timeline; switches to live when done.
    void request_forward_history_(const std::string& room_id);

    // MSC3030: tear down focused state and re-subscribe live.
    void return_to_live_(const std::string& room_id);

    // Send public m.read and private m.read.private receipts for event_id in
    // room_id if it differs from the last one sent this session. No-op when
    // either arg is empty.
    void maybe_send_read_receipt_(const std::string& room_id,
                                   const std::string& event_id);

    // Optimistically zero the unread count for room_id in the local room list
    // and dispatch mark_room_as_read asynchronously. Call on room open so the
    // unread badge clears immediately without waiting for a sync round-trip.
    void mark_room_read_(const std::string& room_id);

    // Update last_room_list_state_.  Shells call their own refresh_sync_status
    // implementation after this to update native UI.
    void push_room_list_state_(RoomListState state);
};

} // namespace tesseract
