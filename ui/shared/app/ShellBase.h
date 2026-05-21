#pragma once
#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/image_pack.h>
#include <tesseract/paths.h>
#include <tesseract/screen_lock.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>
#include <tesseract/visual.h>
#include <tesseract/waveform_cache.h>
#include "tk/anim_image_cache.h"
#include "tk/audio_capture.h"
#include "tk/canvas.h"
#include "tk/media_disk_cache.h"
#include "tk/theme.h"
#include "app/RoomWindowBase.h"
#include "views/MessageListView.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tk
{
class Host;
} // namespace tk

namespace tesseract
{

namespace views
{
class MainAppWidget;
}

// ShellBase holds all state and platform-agnostic logic that is identical
// across the Qt6, GTK4, Win32, and macOS shells. Platform-specific concerns
// (UI widget manipulation, image decode, thread-dispatch mechanism) are
// isolated to a small set of pure-virtual hooks that each shell overrides.
//
// Wiring a new shell: inherit publicly from ShellBase, remove the member
// variables it owns, implement the virtual hooks below, and call the
// concrete helpers (ensure_row_media_, push_rooms_, etc.) instead of the
// per-shell duplicates.
class ShellBase
{
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

    // ── Tab management (call from the UI thread only) ─────────────────────────

    // Ctrl+click: open room_id in a new tab, or switch to it if already open.
    // Bootstraps a first tab from current_room_id_ if tabs_ is empty.
    void tab_open_room(const std::string& room_id);

    // Normal click: replace the current tab's room if not already open,
    // or switch to the existing tab if it is.
    void tab_select_room(const std::string& room_id);

    // Notification click: replace current tab if only one open;
    // open a new tab if multiple tabs are open.
    // Switches to the existing tab if the room is already open.
    void tab_navigate_room(const std::string& room_id);

    // Close the tab for room_id. No-op when tabs_.size() <= 1.
    void tab_close(const std::string& room_id);

protected:
    // ── Multi-account ─────────────────────────────────────────────────────────
    std::vector<std::unique_ptr<AccountSession>> accounts_;
    int active_account_index_ = -1;
    Client* client_ = nullptr;               // non-owning alias
    IEventHandler* event_handler_ = nullptr; // non-owning alias

    std::unordered_map<std::string, std::vector<RoomInfo>> per_account_rooms_;

    // Last tray indicator state pushed to on_tray_unread_changed_().  Used to
    // suppress redundant hook calls (and the per-shell icon repaint they
    // trigger) when a sync tick produces no net change to the aggregate.
    bool last_tray_unread_    = false;
    bool last_tray_highlight_ = false;

    std::unique_ptr<Client> pending_login_client_;
    std::filesystem::path pending_login_temp_dir_;
    bool pending_login_is_add_account_ = false;
    int add_account_return_idx_ = -1;

    // ── Active-account identity ───────────────────────────────────────────────
    std::string my_user_id_;
    std::string my_display_name_;
    std::string my_avatar_url_;

    // ── Tab state ─────────────────────────────────────────────────────────────
    struct TabState
    {
        std::string room_id;
        float scroll_offset = 0.f; // fractional [0,1]: 0=top, 1=bottom
        std::string compose_draft;
    };
    std::vector<TabState> tabs_;
    size_t active_tab_idx_ = 0;

    // ── Per-room message row cache ────────────────────────────────────────────
    // Stores the last-seen MessageRowData snapshot for the N most recently
    // visited rooms so that switching back to a room shows content instantly
    // (before the SDK's on_timeline_reset callback arrives). Keyed by room_id;
    // message_cache_lru_ tracks recency for eviction (front = most recent).
    static constexpr int kMsgCacheCapacity = 10;
    std::deque<std::string>                                              message_cache_lru_;
    std::unordered_map<std::string, std::vector<views::MessageRowData>> message_cache_;

    // ── Rooms ─────────────────────────────────────────────────────────────────
    std::vector<RoomInfo> rooms_;
    std::string current_room_id_;
    // The room whose timeline the message view currently displays. Differs
    // from current_room_id_ between a room switch and the timeline-reset
    // that fills it. Used to tell a genuine switch (gate the display) from
    // an in-place reconnect / gappy-sync reset of the room already shown
    // (refresh in place, no blank). Updated by each shell's
    // handle_timeline_reset_ui_.
    std::string view_displayed_room_id_;
    std::string pending_restore_room_;
    std::vector<std::string> space_stack_;
    bool compose_typing_active_ = false;
    bool typing_bar_visible_ = false;

    // ── Image caches ──────────────────────────────────────────────────────────
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_avatars_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_images_;
    tk::AnimImageCache anim_cache_;
    tk::MediaDiskCache media_disk_cache_{tesseract::cache_dir() / "media"};
    bool media_disk_cache_pruned_ = false;
    bool waveform_store_inited_ = false;

    // MSC2545 emoticon flat list (shortcode popup source). Rebuilt on
    // handle_image_packs_updated_ui_.
    std::vector<tesseract::ImagePackImage> cached_emoticons_;

    // ── Media fetch dedup sets ────────────────────────────────────────────────
    std::unordered_set<std::string> voice_prefetched_;
    std::unordered_set<std::string> voice_waveform_in_flight_;
    std::unordered_set<std::string> video_thumb_in_flight_;
    std::unordered_set<std::string> reply_details_requested_;
    std::unordered_set<std::string> media_fetches_in_flight_;
    std::unordered_set<std::string> sticker_fetches_in_flight_;
    std::unordered_set<std::string> emoji_fetches_in_flight_;
    std::unordered_set<std::string> tile_fetches_in_flight_;
    std::unordered_set<std::string> tile_fetch_failed_;

    // ── URL preview cache ─────────────────────────────────────────────────────
    std::unordered_map<std::string, tesseract::Client::UrlPreview>
        url_previews_;
    std::unordered_set<std::string> url_preview_in_flight_;

    // Decoded UrlPreviewData (title/description/image_mxc + dims) cached for
    // every URL the SDK has resolved. Populated by each shell's
    // on_url_preview_ready_ and looked up by RoomWindowBase::preview_lookup_
    // for both main-window and pop-out room views.
    std::unordered_map<std::string, tesseract::views::UrlPreviewData>
        url_preview_data_;

    // ── BlurHash decode dedup ────────────────────────────────────────────────
    std::unordered_set<std::string> blurhash_attempted_;

    // ── Server capabilities ───────────────────────────────────────────────────
    tesseract::ServerInfo server_info_;        ///< populated after first sync
    bool server_info_fetch_started_ = false;  ///< guards begin_server_info_fetch_

    // ── Sync / backup state ───────────────────────────────────────────────────
    RoomListState last_room_list_state_ = RoomListState::Init;
    BackupState last_backup_state_ = BackupState::Unknown;
    std::uint64_t last_imported_keys_ = 0;
    bool sync_progress_shown_ = false;

    // ── Recovery ──────────────────────────────────────────────────────────────
    bool recovery_banner_dismissed_ = false;

    // ── Cross-signing / SAS device verification ───────────────────────────────
    bool verification_banner_dismissed_ = false;
    std::string active_verification_flow_id_; // "" = no flow in progress

    // ── Pagination ────────────────────────────────────────────────────────────
    struct PaginationState
    {
        bool in_flight = false;
        bool reached_start = false;
        bool fwd_in_flight = false; // forward paginate guard
        bool reached_end = false;
        bool is_focused = false;    // true = using with_focus timeline
        std::string focus_event_id; // scroll target after timeline reset
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
    std::vector<std::unique_ptr<RoomWindowBase>> owned_secondary_windows_;
    std::unordered_map<std::string, RoomWindowBase*> secondary_windows_;
    // Ref-count of active subscriptions per room_id across all secondary windows.
    std::unordered_map<std::string, int> room_subscription_refs_;

    // ── Worker threads ────────────────────────────────────────────────────────
    std::atomic<bool> shutting_down_{false};
    std::mutex workers_mu_;
    std::condition_variable workers_cv_;
    int workers_in_flight_ = 0;

    // ── Media kind tag ────────────────────────────────────────────────────────
    enum class MediaKind : std::uint8_t
    {
        RoomAvatar, // → tk_avatars_, triggers room-list repaint
        UserAvatar, // → tk_avatars_, triggers message-list repaint
        MediaImage, // → anim_cache_ or tk_images_, triggers message-list repaint
        Tile, // → tk_images_["tile:z/x/y"], triggers full message-list repaint
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

    // ── Theme ─────────────────────────────────────────────────────────────────

    // Returns the OS-preferred color scheme. Default: Light.
    // Each platform shell overrides with its native API.
    virtual tk::ThemeMode os_color_scheme_() const
    {
        return tk::ThemeMode::Light;
    }

    // Apply theme to all surfaces owned by this shell. Called on the UI thread.
    // Each platform shell overrides to call set_theme() on each of its surfaces.
    virtual void apply_theme_ui_(const tk::Theme&)
    {
    }

    // Re-theme every open pop-out room window. Each shell's apply_theme_ui_()
    // calls this so secondary windows follow the theme setting.
    void apply_theme_to_secondary_windows_(const tk::Theme& t);

    // The theme last resolved by apply_current_theme_(). Lets surfaces
    // created lazily (e.g. a pop-out room window opened while in dark mode,
    // with no subsequent theme change) start out correctly themed.
    tk::Theme current_theme_ = tk::Theme::light();

    // Per-shell microphone capture backend. Null when unavailable or
    // unsupported on the current platform. Initialised in each shell
    // constructor immediately after make_audio_player().
    std::unique_ptr<tk::AudioCapture> capture_;

    // Wire voice-capture callbacks onto rv. Call once per shell after capture_
    // is initialised (not from RoomWindowBase::wire_room_view_() — pop-out
    // windows hide the mic button instead). `request_repaint` is called each
    // time an amplitude sample arrives. `get_room_id` is invoked when the user
    // starts recording so the message targets the room active at that moment,
    // not when the callback was registered. `clear_text_fn` clears the compose
    // field (and any native text widget) after a successful voice send.
    void wire_voice_capture_(views::RoomView*             rv,
                             std::function<void()>        request_repaint,
                             std::function<std::string()> get_room_id,
                             std::function<void()>        clear_text_fn);

    // Platform screen-lock probe for the notification-image privacy gate.
    // Defaults to the fail-safe Null impl until the concrete shell installs
    // a real one via set_screen_lock_().
    std::unique_ptr<IScreenLock> screen_lock_ =
        std::make_unique<NullScreenLock>();

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

    // Called on the UI thread when the aggregate unread/highlight state across
    // all signed-in accounts changes. Each shell overrides to forward to its
    // tray icon. Default no-op so shells without a tray (or with a tray that
    // failed to register) silently skip the update.
    virtual void on_tray_unread_changed_(bool /*has_unread*/,
                                         bool /*has_highlight*/)
    {
    }

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
                                           const std::string& /*video_url*/)
    {
    }

    // Called on the UI thread when a URL preview fetch completes successfully.
    // Default is a no-op; shells override to update their preview cache and
    // request a repaint.
    virtual void on_url_preview_ready_(const std::string& /*url*/,
                                       const Client::UrlPreview& /*preview*/)
    {
    }

    // Called on the UI thread when a URL preview fetch finished but produced
    // no usable card (failed / no metadata). Default no-op; shells override
    // to ping the message list so its room-switch gate stops waiting on this
    // URL (the row's height is unaffected — it never gained a preview card).
    virtual void on_url_preview_failed_(const std::string& /*url*/)
    {
    }

    // MSC2448: store a decoded RGBA8888 buffer as a tk::Image in tk_images_.
    // Default is a no-op; each platform shell overrides with native image creation.
    virtual void cache_rgba_image_(const std::string& /*key*/, int /*w*/,
                                   int /*h*/, std::vector<uint8_t> /*rgba*/)
    {
    }

    // Decode `bytes` into a tk::Image (or animated frames). Scaled so the
    // longest side is ≤ max(max_w, max_h). MUST be safe to call on a
    // worker thread (no UI/device context): every backend's decoder is
    // thread-safe; Win32 wraps a device-independent IWICBitmap. Used by
    // ensure_picker_image_ (worker) and on_media_bytes_ready_ (UI).
    virtual DecodedImage decode_image_(const std::vector<uint8_t>& bytes,
                                       int max_w, int max_h) = 0;

    // Monotonic clock in ms from the SAME epoch the shell's animation
    // timer / anim_cache_.advance() uses (Qt: QDateTime msecs; GTK:
    // g_get_monotonic_time/1000; macOS: NSDate*1000; Win32: GetTickCount64).
    virtual std::int64_t monotonic_ms_() = 0;

    // Start the shell's shared animation frame-tick timer if it is not
    // already running. Default no-op (shells with no animated content).
    virtual void start_anim_tick_()
    {
    }

    // Repaint whichever picker surfaces are visible (relayout + invalidate).
    // Default no-op.
    virtual void repaint_pickers_()
    {
    }

    // ── Tab state hooks ───────────────────────────────────────────────────────
    // Called after tabs_ and current_room_id_ have been updated. The shell must:
    //   1. Sync the TabBar widget (add/remove/set_active).
    //   2. Show/hide TabBar; set RoomHeader condensed mode.
    //   3. Call its room-switch method when current_room_id_ != view_displayed_room_id_.
    //   4. Restore compose_draft for the newly active tab.
    virtual void on_tab_state_changed_ui_() = 0;

    // Read the current fractional scroll position [0,1] of the message list.
    virtual float get_message_scroll_fraction_()
    {
        return 0.f;
    }
    // Seek the message list to fractional position t.
    virtual void set_message_scroll_fraction_(float /*t*/)
    {
    }
    // Read the current compose-bar draft text.
    virtual std::string get_compose_draft_()
    {
        return {};
    }
    // Write text into the compose bar (called after a tab switch restores draft).
    virtual void set_compose_draft_(const std::string& /*s*/)
    {
    }

    // Return a pointer to the message list currently displayed, or nullptr.
    // Each shell overrides to return &room_view_->message_list()->messages().
    virtual const std::vector<views::MessageRowData>* get_current_messages_()
    {
        return nullptr;
    }
    // Apply msgs to the view without arming a room-switch gate, then relayout.
    // Each shell overrides to call room_view_->set_messages(msgs, false) and
    // trigger a surface relayout. Called on the UI thread only.
    virtual void apply_cached_messages_(
        const std::vector<views::MessageRowData>& /*msgs*/)
    {
    }

    // ── EventHandlerBase UI-thread hooks ─────────────────────────────────────
    // Called on the UI thread by EventHandlerBase after marshaling. Default
    // implementations are no-ops; each shell overrides what it needs.

    virtual void
    handle_timeline_reset_ui_(std::string /*room_id*/,
                              std::vector<std::unique_ptr<Event>> /*snapshot*/)
    {
    }
    virtual void handle_message_inserted_ui_(std::string /*room_id*/,
                                             std::size_t /*index*/,
                                             std::unique_ptr<Event> /*ev*/)
    {
    }
    virtual void handle_message_updated_ui_(std::string /*room_id*/,
                                            std::size_t /*index*/,
                                            std::unique_ptr<Event> /*ev*/)
    {
    }
    virtual void handle_message_removed_ui_(std::string /*room_id*/,
                                            std::size_t /*index*/)
    {
    }
    virtual void handle_sync_error_ui_(std::string /*context*/,
                                       std::string /*user_id*/,
                                       std::string /*description*/,
                                       bool /*soft_logout*/)
    {
    }
    virtual void handle_backup_progress_ui_(BackupProgress /*progress*/)
    {
    }
    // Per-shell native sticker/emoji picker refresh prologue. Default no-op.
    virtual void refresh_pickers_packs_()
    {
    }
    // Concrete: runs the per-shell picker-refresh prologue, then rebuilds the
    // MSC2545 emoticon flat list (cached_emoticons_).
    virtual void handle_image_packs_updated_ui_();

    // Look up the bare shortcode (no surrounding colons) for an mxc:// URI
    // by scanning cached_emoticons_. Used by the MSC4027 reaction path so
    // image-reaction chips can be rendered as `:shortcode:` and the chip-
    // re-tap toggle can carry the shortcode out on the wire. Returns an
    // empty string when the mxc isn't in any of the user's emoticon packs.
    std::string shortcode_for_mxc_(const std::string& mxc) const;

    // Per-shell media-prefetch for one row. Default = ensure_row_media_.
    // Qt6 overrides to also record decode-size hints (mediaImageSizes_).
    virtual void prep_row_media_(const Event& ev)
    {
        ensure_row_media_(ev);
    }
    // Concrete: only the active account's prefs set the pending restore room.
    virtual void handle_account_prefs_updated_ui_(std::string user_id,
                                                  std::string json);
    virtual void
    handle_notification_ui_(std::string /*user_id*/, std::string /*room_id*/,
                            std::string /*room_name*/, std::string /*sender*/,
                            std::string /*body*/, bool /*is_mention*/,
                            std::vector<uint8_t> /*avatar_bytes*/,
                            std::vector<uint8_t> /*image_bytes*/)
    {
    }

    // Called on the UI thread when a locally generated waveform is ready for
    // a voice message that arrived without MSC1767 waveform data. Each shell
    // overrides to call room_view_->message_list()->update_voice_waveform().
    virtual void handle_voice_waveform_ready_ui_(std::string /*room_id*/,
                                                 std::string /*event_id*/,
                                                 std::vector<std::uint16_t> /*waveform*/)
    {
    }

    // Install the platform screen-lock probe (called once by the concrete
    // shell at startup, mirroring the per-account INotifier injection).
    void set_screen_lock_(std::unique_ptr<IScreenLock> sl)
    {
        if (sl)
        {
            screen_lock_ = std::move(sl);
        }
    }
    // Centralised notification-image privacy gate. Each shell calls this
    // when building the Notification: the message picture is shown only
    // when previews are enabled in settings AND the screen is unlocked.
    // Room avatars are intentionally NOT gated (low-sensitivity room
    // metadata). Returns true → keep image_bytes; false → clear it.
    bool notification_image_allowed_() const
    {
        return tesseract::Settings::instance().notification_image_previews &&
               !(screen_lock_ && screen_lock_->is_locked());
    }
    // Called after push_room_list_state_() — shell refreshes its sync-status display.
    virtual void on_room_list_state_ui_()
    {
    }

    /// Called on the UI thread after `server_info_` has been populated.
    /// Override in shells that gate UI elements on server capabilities.
    virtual void on_server_info_ready_ui_() {}

    /// Reset server-info state on logout / account-switch. Call this instead of
    /// touching server_info_ and server_info_fetch_started_ directly from shells.
    void reset_server_info_()
    {
        server_info_fetch_started_ = false;
        server_info_ = tesseract::ServerInfo{};
    }

    /// Spawn a detached thread to call Client::get_server_info(), then
    /// marshal the result back to the UI thread. Only fetches once per session.
    void begin_server_info_fetch_()
    {
        if (server_info_fetch_started_ || !client_)
            return;
        server_info_fetch_started_ = true;
        auto* c = client_;
        run_async_([this, c] {
            auto info = c->get_server_info();
            post_to_ui_([this, info = std::move(info)] {
                server_info_ = std::move(info);
                on_server_info_ready_ui_();
            });
        });
    }

    // ── Verification banner hooks (default no-op) ──────────────────────────────
    virtual void handle_verification_request_ui_(std::string /*flow_id*/,
                                                 std::string /*user_id*/,
                                                 std::string /*device_id*/,
                                                 bool /*incoming*/)
    {
    }
    virtual void handle_sas_ready_ui_(std::string /*flow_id*/,
                                      std::vector<VerificationEmoji> /*emojis*/)
    {
    }
    virtual void handle_verification_done_ui_(std::string /*flow_id*/)
    {
    }
    virtual void handle_verification_cancelled_ui_(std::string /*flow_id*/,
                                                   std::string /*reason*/)
    {
    }
    virtual void handle_verification_state_ui_(bool /*is_verified*/)
    {
    }

    // ── Typing notification hooks ─────────────────────────────────────────────
    // Called on the UI thread by EventHandlerBase. Filters by current_room_id_,
    // formats the display text, and calls update_typing_bar_.
    void handle_typing_changed_ui_(std::string room_id,
                                   std::vector<std::string> names);
    // Override in each shell to push text into the platform typing-bar widget.
    // text is empty when no one is typing.
    virtual void update_typing_bar_(const std::string& /*text*/,
                                    bool /*visible*/)
    {
    }

    // ── Compose typing send helpers ───────────────────────────────────────────
    // Call from the shell's NativeTextArea on_changed callback.
    void handle_compose_text_changed_(const std::string& text);
    // Call BEFORE updating current_room_id_ on room switch / account switch.
    void handle_compose_room_leaving_(const std::string& old_room_id);

    // ── Secondary window registry ─────────────────────────────────────────────

    // Register/unregister a secondary window. Called by RoomWindowBase.
    void register_room_window_(RoomWindowBase* w);
    void unregister_room_window_(RoomWindowBase* w);
    // Remove the owning unique_ptr for w from owned_secondary_windows_,
    // destroying the C++ object. Called by RoomWindowBase::schedule_self_close_()
    // via post_to_ui_ so deletion happens outside the window's own message handler.
    void release_owned_window_(RoomWindowBase* w);

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
    virtual RoomWindowBase*
    create_secondary_room_window_(const std::string& /*room_id*/)
    {
        return nullptr;
    }

    // ── Concrete helpers ──────────────────────────────────────────────────────

    // Spawn a worker thread.  fn() runs off the UI thread and must not touch
    // UI state directly — use post_to_ui_ to bounce results back.
    void run_async_(std::function<void()> fn);

    // Avatar / media prefetch — each method is idempotent (dedup via the
    // media_fetches_in_flight_ set + cache-presence check).
    void ensure_room_avatar_(const RoomInfo& r);
    void ensure_user_avatar_(const std::string& mxc);
    void ensure_media_image_(const std::string& url, int max_w, int max_h);

    // Sticker / animated-media lookup: anim_cache_ → tk_images_ fallback.
    // shell_sticker_ kicks an ensure_media_image_(mxc, 64, 64) fetch on miss
    // (used by RoomListView's sticker_provider, where the row hasn't yet
    // pre-warmed the cache). shell_sticker_no_fetch_ is the pure-lookup
    // variant (used by RoomView's image_provider, where ensure_row_media_
    // has already kicked the fetch).
    const tk::Image* shell_sticker_(const std::string& mxc);
    const tk::Image* shell_sticker_no_fetch_(const std::string& mxc) const;

    // Wire MainAppWidget-level + RoomListView/RoomView/UserInfo providers
    // that read from tk_avatars_, tk_images_, anim_cache_, and
    // url_preview_data_. Each shell calls this once during construction after
    // creating its MainAppWidget. Does NOT touch image_viewer/video_viewer
    // (see wire_main_app_viewers_) nor non-provider callbacks
    // (on_room_selected, on_scroll, on_search_clear, etc.) — those touch
    // shell-specific state and stay in the per-shell ctor.
    void wire_main_app_widget_(views::MainAppWidget* app);

    // Wire image_viewer() + video_viewer() provider/repaint/close callbacks.
    // `host` builds the video player. `request_relayout` is each shell's
    // surface relayout primitive. `on_image_close` / `on_video_close`
    // (optional) run AFTER the standard hide+relayout sequence — Qt6 uses
    // these to restore focus to its native compose text area; other shells
    // pass {} when they don't need a tail action.
    void wire_main_app_viewers_(views::MainAppWidget* app,
                                tk::Host&             host,
                                std::function<void()> request_relayout,
                                std::function<void()> on_image_close = {},
                                std::function<void()> on_video_close = {});

    // Shared async picker-image path. Idempotent: no-op if already in
    // tk_images_ / anim_cache_ / in-flight. Dedups via
    // emoji_fetches_in_flight_ (is_sticker == false) or
    // sticker_fetches_in_flight_ (true). Worker: media_disk_cache_ →
    // client_->fetch_source_bytes → media_disk_cache_.store →
    // decode_image_ (OFF the UI thread) → post finalize_picker_image_.
    void ensure_picker_image_(const std::string& url, bool is_sticker);

    // UI-thread tail of ensure_picker_image_. Erases the in-flight key,
    // routes `d` into anim_cache_ (animated) or tk_images_ (still),
    // calls start_anim_tick_() / repaint_pickers_(). Public-testable
    // logic (see test_picker_image_cache.cpp). Safe if `d.empty()`.
    void finalize_picker_image_(std::string url, bool is_sticker,
                                DecodedImage d);

    /// Fetch an OSM tile (z/x/y) asynchronously. Idempotent — no-op if already
    /// in tk_images_, in-flight, or previously failed. On success: stores bytes
    /// via on_media_bytes_ready_(key, MediaKind::Tile, bytes). On failure:
    /// inserts key into tile_fetch_failed_ to suppress retries this session.
    void ensure_tile_async(int z, int x, int y);

    // Snapshot the current room's message rows into message_cache_ (LRU).
    // Called at every point where the active tab is about to change rooms.
    void save_tab_message_cache_();

    // If message_cache_ has a non-empty entry for room_id, apply it via
    // apply_cached_messages_() and pre-populate view_displayed_room_id_ so
    // the subsequent on_timeline_reset lands as a quiet in-place update
    // (no room-switch gate). Returns true when a cache hit was found.
    bool try_restore_message_cache_(const std::string& room_id);

    // Fire a synchronous SDK call to fetch reply-to metadata.
    void ensure_reply_details_(const std::string& event_id);

    // Fetch OpenGraph preview metadata for `url` from the homeserver.
    // Idempotent — deduplicates in-flight fetches and skips already-cached URLs.
    void ensure_url_preview_(const std::string& url);

    // Decode the BlurHash for `event_id` and store the result via cache_rgba_image_.
    // Idempotent — skips events already attempted or already cached.
    void ensure_blurhash_image_(const std::string& event_id,
                                const std::string& hash, int media_w,
                                int media_h);

    // Walk all media references in ev and call ensure_*_ for each.
    void ensure_row_media_(const Event& ev);

    // Build MessageRowData rows from an event snapshot: prep media, request
    // reply details, make_row_data. Used by every shell's timeline-reset and
    // message handlers (primary + secondary-window paths).
    std::vector<views::MessageRowData>
    build_rows_(const std::vector<std::unique_ptr<Event>>& snapshot);
    // macOS hands primary-path events across the ObjC boundary as raw
    // pointers; this overload serves that path.
    std::vector<views::MessageRowData>
    build_rows_(const std::vector<Event*>& snapshot);

    // Secondary-window fan-out (primary-window mutation stays per-shell).
    void dispatch_timeline_reset_secondary_(
        const std::string& room_id,
        const std::vector<std::unique_ptr<Event>>& snapshot);
    void dispatch_message_inserted_secondary_(const std::string& room_id,
                                              std::size_t index,
                                              const Event& ev);
    void dispatch_message_updated_secondary_(const std::string& room_id,
                                             std::size_t index,
                                             const Event& ev);
    void dispatch_message_removed_secondary_(const std::string& room_id,
                                             std::size_t index);
    // Refresh open pop-out windows' room metadata from rooms_.
    void update_secondary_room_infos_();

    // Update the rooms cache and call on_rooms_updated_() for the active account.
    void push_rooms_(std::string user_id, std::vector<RoomInfo> rooms);

    // Recompute the aggregate from per_account_rooms_ and fire
    // on_tray_unread_changed_ only when the value differs from the last call.
    // Called from push_rooms_ and mark_room_read_.
    void notify_tray_unread_();

public:
    // Pure function: returns {has_unread, has_highlight} computed across every
    // account's room list. has_unread is true iff some room has
    // notification_count > 0; has_highlight is true iff some room has
    // highlight_count > 0. Exposed as a public static so the unit test can
    // exercise it without standing up a real shell.
    static std::pair<bool, bool> compute_tray_unread(
        const std::unordered_map<std::string, std::vector<RoomInfo>>& by_account);

protected:

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

private:
    // intentionally empty — all state is protected so shells can reset it on
    // logout / account-switch without needing friend declarations.
};

} // namespace tesseract
