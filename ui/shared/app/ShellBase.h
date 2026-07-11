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
#include "app/AccountManager.h"
#include "app/GithubUpdateChecker.h"
#include "app/PresenceTracker.h"
#include "app/SettingsController.h"
#include "app/status_links.h"
#include "app/ThreadPanelController.h"
#include "app/UpdateChecker.h"
#include "tk/audio_capture.h"
#include "tk/audio_playback.h"
#ifdef TESSERACT_CALLS_ENABLED
#include <tesseract/call_session.h>
#include "tk/video_capture.h"
#include "tk/screen_capture.h"
#include "app/CallWindowBase.h"
#include "views/CallOverlayWidget.h"
#endif
#include "tk/canvas.h"
#include "tk/inflight_dot.h"
#include "tk/theme.h"
#include "app/RoomWindowBase.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/MessageListView.h"
#include "views/QuickSwitcher.h"
#include "views/RoomListView.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <atomic>
#include <filesystem>
#include <functional>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tk
{
class Host;
class NativeTextField;
} // namespace tk

namespace tesseract
{

namespace views
{
class ComposeBar;
class MainAppWidget;
class RoomSearchBar;
class RoomSettingsView;
class RoomView;
class SettingsView;
class UserPackEditor;
class UserProfilePanel;
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
    explicit ShellBase(AccountManager& account_manager);
    virtual ~ShellBase();

    // Bring this window to the foreground and give it keyboard focus.
    // Platform implementations: Qt6 = raise()+activateWindow(), GTK4 = gtk_window_present(),
    // Win32 = SetForegroundWindow(), macOS = makeKeyAndOrderFront:.
    virtual void raise_and_activate_() = 0;

    // Rebuild the system-tray context menu so it lists one item per open main
    // window before the Quit action. Called whenever the window registry
    // changes. Default is a no-op (e.g. a shell that has no tray).
    virtual void rebuild_tray_() {}

    // Broadcast rebuild_tray_() to every window currently in the
    // AccountManager registry. Call this (on a real ShellBase pointer) after
    // register_window / unregister_window so every window's tray reflects the
    // updated set.
    void broadcast_rebuild_tray_();

    // Build the platform-agnostic tray-menu item list: one (label, callback)
    // entry per open main window, where the label is the window's active
    // account display name + user id (or "Tesseract" when signed out) and the
    // callback raises and activates that window. Each shell's rebuild_tray_()
    // calls this, then pushes the result to its native OS tray.
    std::vector<std::pair<std::string, std::function<void()>>>
    build_tray_items_() const;

    // A single item in the user-strip context menu. An entry whose label is
    // empty is a separator; its callback will be null.
    struct UserMenuItem
    {
        std::string           label;
        std::function<void()> callback;
    };

    // Build the canonical user-strip context-menu item list. The order and
    // Log Out label are defined here; platform shells supply the five action
    // callbacks and iterate the result to build their native menu. The QR
    // item is omitted automatically when server_info_.supports_qr_grant is
    // false. show_qr_grant may be a null std::function even when QR is
    // supported — the item will still be omitted.
    std::vector<UserMenuItem> build_user_menu_items_(
        std::function<void()> open_settings,
        std::function<void()> add_account,
        std::function<void()> show_qr_grant,
        std::function<void()> logout,
        std::function<void()> quit) const;

    // Arm the pending-login OAuth flow's temp directory. Installed (via a
    // shell-native one-liner lambda) as the LoginView's on-begin-oauth
    // callback: the user_id isn't known until await_oauth completes, so the
    // OAuth round-trip runs against a per-attempt "pending-<ms>" directory
    // that finalize-login later renames to accounts/<sanitized-uid>/.
    //
    // Idempotent: returns immediately if pending_login_temp_dir_ is already
    // set. Computes a unique "pending-<ms>" dir under SessionStore::account_dir,
    // creates it, and points pending_login_client_'s data dir at its
    // "matrix-store" subdir. Operates on the ShellBase pending_login_* members.
    void arm_pending_login_();

    // Returns the active account for this window.
    std::shared_ptr<tesseract::AccountSession> active_account() const { return active_account_; }

    // Pre-set active_account_ for a newly-spawned window before any
    // account-dependent initialization runs. Called by spawn_main_window_()
    // implementations immediately after constructing the window.
    void set_initial_account(std::shared_ptr<tesseract::AccountSession> account);

    // Open room_id in a new native window. If a secondary window for that room
    // is already open, it is raised instead of duplicated. The platform shell
    // must override create_secondary_room_window_() for this to have effect.
    void open_room_in_new_window(const std::string& room_id);

#ifdef TESSERACT_CALLS_ENABLED
    // ── MatrixRTC call control (Layer 4, guarded) ────────────────────────────
    // start_call creates a CallSession, wires audio (and video if a camera is
    // available) capture routing, and calls rtc_start_call on the client.
    // No-op when a call is already active.
    void start_call(const std::string& room_id,
                    const std::string& slot_id   = "call#default",
                    bool               audio_only = false);
    // End the active call and tear down all call resources. No-op when idle.
    void end_call();
    // Returns the active CallSession, or nullptr when not in a call.
    CallSession* active_call() const { return call_session_.get(); }
    // Resolve whichever window (main window or a pop-out) is currently
    // displaying room_id, or nullptr if none — used to target the incoming-
    // call banner (and its dismissal) at the right window instead of
    // assuming room_view_ (the main window's own RoomView).
    views::RoomView* room_view_for_room_(const std::string& room_id) const;
#endif // TESSERACT_CALLS_ENABLED

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

    // Ctrl/⌘+click on a tab: pop the room out into its own native window and
    // close the tab. Closes first so current_room_id_ moves off the room before
    // the new window's acquire_room_subscription_ runs (it then takes its own
    // SDK subscription rather than relying on the main window). tab_close is a
    // no-op for the last remaining tab, in which case the room stays shown in
    // the main window alongside the pop-out.
    void tab_popout_room(const std::string& room_id);

    // Navigate to the previous/next room in the global visit history.
    // To be wired to Alt+Left / Alt+Right (Cmd+[ / Cmd+] on macOS).
    void navigate_history_back();
    void navigate_history_forward();

    // Wire room_view_->on_open_dm and room_view_->on_has_dm.
    // Call once per shell after main_app_ and room_view_ are set.
    void setup_dm_callbacks();

    // Wire room_view_->on_link_clicked to intercept matrix: / matrix.to links
    // in-app and send everything else to the system browser.
    // Call once per shell after room_view_ is set.
    void setup_link_clicked_(views::RoomView* rv);

    // Navigate to the target described by a `https://matrix.to/#/…` URL or a
    // `matrix:` URI (MSC2312).  Safe to call from the UI thread at any time,
    // including before login completes — in that case the URI is stored and
    // replayed once the first rooms-update arrives.
    void open_matrix_link(const std::string& uri);

    // ── Debounce ──────────────────────────────────────────────────────────────
    // Independent debounce channels. Each slot tracks its own generation so
    // concurrent debounced actions don't cancel one another.
    enum class DebounceSlot
    {
        RoomSearch,
        SaveSettings,
        MessageSearch,
        SearchStats,
        InRoomSearch,
    };

    // Run fn() on the UI thread `ms` after the most recent call on `slot`,
    // dropping any earlier still-pending call on the same slot. Built on the
    // platform's post_to_ui_after_; the generation guard means the primitive
    // needs no cancel handle — superseded fires simply no-op. fn() always runs
    // on the UI thread.
    void debounce_(DebounceSlot slot, int ms, std::function<void()> fn);

    // Drop any pending debounce on `slot` without scheduling a replacement.
    // Use when an action (e.g. clearing the search box) must take effect now
    // and a queued debounce would otherwise clobber it.
    void cancel_debounce_(DebounceSlot slot);

    // ── Thread panel state machine ────────────────────────────────────────────
    // The panel-mode / trigger enums and the ThreadTransition value type, plus
    // the pure transition computation and backfill pagination guards, live in
    // ThreadPanelController. These aliases keep the historical spellings
    // (ShellBase::ThreadPanel etc.) working for the native shells — which read
    // thread_panel_ / current_thread_root_ directly and pull ThreadPanel in via
    // `using` on macOS — and for the thread-transition / thread-panel tests.
    using ThreadPanel     = ThreadPanelController::ThreadPanel;
    using ThreadTrigger   = ThreadPanelController::ThreadTrigger;
    using ThreadTransition = ThreadPanelController::ThreadTransition;

    // Pure: returns the next state + the subscription side-effects to apply.
    // No Client calls, no UI calls — safe to call from tests. Thin forwarder to
    // ThreadPanelController::compute_transition.
    static ThreadTransition compute_thread_transition_(
        ThreadPanel cur, ThreadPanel prev, const std::string& current_root,
        ThreadTrigger trigger, const std::string& trigger_root)
    {
        return ThreadPanelController::compute_transition(cur, prev, current_root,
                                                         trigger, trigger_root);
    }

    // ── Thread panel public entry points (wired from RoomView callbacks) ──
    // Each computes a transition via compute_thread_transition_() and
    // applies it through apply_thread_transition_().
    void on_threads_button_clicked();
    void on_thread_open_requested(const std::string& root_event_id);
    void on_thread_close_requested();
    void on_thread_send_requested(const std::string& body,
                                  const std::string& formatted_body);
    // Reply variant: send `body` as a reply to `in_reply_to_event_id`
    // inside the currently-open thread. Wired from
    // RoomView::on_thread_send_reply when the thread panel is Open and the
    // compose bar fires on_send_reply.
    void on_thread_send_reply_requested(const std::string& in_reply_to_event_id,
                                        const std::string& body,
                                        const std::string& formatted_body);

    // ── Pinned events public entry points (wired from RoomView callbacks) ──
    // Each forwards to the SDK and logs a failure on error. Idempotent on the
    // SDK side: pin of an already-pinned event / unpin of an already-unpinned
    // event are no-ops.
    void on_pin_requested(const std::string& event_id);
    void on_unpin_requested(const std::string& event_id);

    // Saved room-list state for one level of space navigation.
    // Declared public so all shells (including ObjC++) can name the type.
    struct SpaceNavFrame {
        std::array<bool, views::RoomListView::kNumSections> collapsed = {};
        float scroll_fraction = 0.f;

        static SpaceNavFrame capture(views::RoomListView* rlv);
        void restore(views::RoomListView* rlv) const;
        static void enter(views::RoomListView* rlv);
    };

protected:
    // Per-slot debounce generation counters (see debounce_). Keyed by
    // static_cast<int>(DebounceSlot); a fire is honoured only if its captured
    // generation still matches the slot's current value.
    std::unordered_map<int, std::uint64_t> debounce_gen_;
    // Set at the top of ~ShellBase() so that any save_settings_debounced_()
    // calls from ~RoomWindowBase() skip the debounce (which calls the pure-
    // virtual post_to_ui_after_()) instead of crashing.
    bool tearing_down_ = false;

    // Push the current room's pinned_events + can_pin bit to room_view_,
    // looking up the RoomInfo in the rooms_ cache. Called from push_rooms_
    // (per sync tick) and after_active_room_changed_ (per room switch). When
    // the room is not yet in the cache, clears both so the banner hides.
    void refresh_pinned_for_current_room_();
    // Apply the side-effects of a ThreadTransition: subscribe / unsubscribe
    // threads on the client, update local thread_panel_ state, drive the
    // RoomView's right-side panel, and refresh the thread-list snapshot
    // when entering List mode.
    void apply_thread_transition_(const ThreadTransition& t);

    // Post-switch hook: called by tab_open/tab_select/tab_navigate/tab_close
    // after current_room_id_ has been updated. Subscribes to the new active
    // room's thread list so the threads-button visibility check has up-to-date
    // data, and immediately seeds the button from the local snapshot (empty
    // when this is a first-time visit).
    void after_active_room_changed_();

    // Persist the current room-layout prefs (active room + open tabs) for the
    // logged-in account. Builds the layout fresh from current_room_id_ + tabs_
    // (PrefsData carries only these) and dispatches the async save — it does NOT
    // call the blocking load_prefs_json(), so it's cheap to run on every room
    // switch. Shared by all four shells (replaces a duplicated inline block).
    void persist_room_layout_pref_();

    // Drive the SDK subscription for a room switch. subscribe_room runs on the
    // single-thread mut pool (fast for a warm room — the SDK reuses the live
    // timeline; either way it emits the reset that repopulates the just-cleared
    // view and cancels the loading state). The initial back-pagination then runs
    // on the SHARED pool so its blocking network round-trip never holds the one
    // mut thread — otherwise the next switch's subscribe/reset would queue behind
    // it and the loading spinner would flash on rapid A<->B switching. subscribe
    // is dispatched on every switch (not gated by in_flight) so the reset always
    // arrives; only the network paginate is deduplicated per room. Shared by all
    // four shells. `visible_ids` seeds the background unread prefetch.
    void start_room_subscription_(const std::string&        room_id,
                                  std::vector<std::string>  visible_ids);

    // ── Multi-account ─────────────────────────────────────────────────────────
    AccountManager& account_manager_;
    std::shared_ptr<AccountSession> active_account_;
    // Shared liveness token: set to false in ~ShellBase so worker→UI
    // continuations (routed through post_to_ui_alive_) can detect that this
    // shell is gone and no-op rather than dereferencing freed members. Mirrors
    // RoomWindowBase::alive_. Critical for spawned/secondary windows and
    // account-switch teardown, where a queued continuation may run after the
    // ShellBase is destroyed.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    Client* client_ = nullptr;               // non-owning alias
    IEventHandler* event_handler_ = nullptr; // non-owning alias

    std::unordered_map<std::string, std::vector<RoomInfo>> per_account_rooms_;
    std::unordered_map<std::string, std::vector<InviteInfo>> per_account_invites_;

    // Last tray indicator state pushed to on_tray_unread_changed_().  Used to
    // suppress redundant hook calls (and the per-shell icon repaint they
    // trigger) when a sync tick produces no net change to the aggregate.
    bool last_tray_unread_    = false;
    bool last_tray_highlight_ = false;

    // Last dock badge count pushed to on_dock_badge_changed_().  UINT64_MAX
    // is used as a sentinel meaning "never sent" so the first call always fires.
    uint64_t last_dock_badge_count_ = UINT64_MAX;

    // Owns the per-account settings controller. Rebuilt on every login /
    // account switch via ensure_settings_controller_(); the native widget +
    // dialog-hook binding is delegated to bind_settings_controller_().
    std::unique_ptr<tesseract::SettingsController> settings_controller_;

    std::unique_ptr<Client> pending_login_client_;
    std::filesystem::path pending_login_temp_dir_;
    bool pending_login_is_add_account_ = false;
    int add_account_return_idx_ = -1;
    // URI from open_matrix_link() deferred until the first rooms-update.
    std::string pending_matrix_link_;
    // Event id to scroll to once a room joined from an event permalink finishes
    // joining, keyed by the permalink's room id (matched against joined_room_id
    // in handle_room_action_complete_ui_). Cleared on consume; a stale entry
    // (user edited the join target) is harmless and tiny.
    std::unordered_map<std::string, std::string> pending_event_scroll_after_join_;

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

    // ── Rooms ─────────────────────────────────────────────────────────────────
    std::vector<RoomInfo> rooms_;
    // room_id → index into rooms_, for O(1) lookup instead of a linear scan on
    // the room-switch path (space-detection, set_room, tab-bar metadata). rooms_
    // is only ever wholesale-replaced or cleared (never mutated in place), and
    // those writers call mark_room_index_dirty_(); the index is rebuilt lazily on
    // the next room_by_id_() so frequent sync ticks (which re-sort rooms_ without
    // anyone reading the index between them) don't each pay an O(n) rebuild.
    mutable std::unordered_map<std::string, std::size_t> room_index_by_id_;
    mutable bool room_index_dirty_ = true;
    void rebuild_room_index_() const;
    void mark_room_index_dirty_() { room_index_dirty_ = true; }
    // O(1) room lookup by id; nullptr when not present. The returned pointer is
    // valid until rooms_ is next replaced/cleared.
    const RoomInfo* room_by_id_(const std::string& room_id) const;
    // ── Invites ───────────────────────────────────────────────────────────────
    std::vector<InviteInfo> invites_;
    // Populated asynchronously from update_space_children_cache_(); read
    // synchronously in refresh_room_list_().
    std::unordered_map<std::string, std::vector<std::string>> space_children_cache_;

    // space_id → child room IDs that are NOT in rooms_ (unjoined children).
    // Built alongside space_children_cache_ inside update_space_children_cache_().
    std::unordered_map<std::string, std::vector<std::string>>
        unjoined_space_children_cache_;

    // space_id → per-room summaries for unjoined children.
    // Entries are initially placeholder (room_id only, name empty); real data
    // is filled in per-room as rows are painted via fetch_single_room_summary_.
    std::unordered_map<std::string, std::vector<tesseract::RoomSummary>>
        unjoined_summaries_cache_;
    // Room IDs whose individual summary fetch is currently in flight.
    std::unordered_set<std::string> unjoined_fetch_pending_;
    // Per-room exponential backoff for rooms that returned an error (403/404/…).
    // Cleared whenever the active space changes so stale failure state doesn't
    // persist if the user re-enters the same space later.
    struct UnjoinedRetryState
    {
        int attempts = 0;
        std::chrono::steady_clock::time_point next_retry{};
    };
    std::unordered_map<std::string, UnjoinedRetryState> unjoined_fetch_retry_;
    // Bumped whenever the active space changes; captured by each
    // fetch_single_room_summary_ call so stale completions are discarded.
    std::uint64_t unjoined_fetch_gen_ = 0;
    // Monotonically increasing counter for async FFI request IDs. UI-thread-only.
    std::uint64_t next_request_id_ = 0;
    // request_id → target_room_id for in-flight async forwards.
    std::unordered_map<std::uint64_t, std::string> pending_forwards_;
    // In-flight get_space_child_summary_async requests keyed by request_id.
    struct PendingSummaryRequest
    {
        std::string space_id;
        std::string room_id;
        std::uint64_t gen = 0;
    };
    std::unordered_map<std::uint64_t, PendingSummaryRequest> pending_summaries_;
    // Space ID whose unjoined section is currently displayed in the room list.
    std::string active_space_id_;
    std::string current_room_id_;
    // Most-recently-visited room IDs in visit order (front = most recent),
    // recorded in after_active_room_changed_(). Feeds the Ctrl+K quick
    // switcher's "Recent" strip. In-memory only (not persisted across restarts).
    std::vector<std::string> recent_room_ids_;
    static constexpr std::size_t kRecentRoomsMax = 8;
    // Navigation history for Alt+Left / Alt+Right back-forward traversal.
    // Separate from recent_room_ids_ (MRU). In-memory only.
    std::vector<std::string> room_nav_history_;
    std::size_t              room_nav_history_cursor_ = 0;
    bool                     room_nav_in_progress_    = false;
    static constexpr std::size_t kNavHistoryMax = 100;
    // Tracks the invite currently shown in the InviteCard so the action
    // callbacks (on_accept / on_decline / on_block) can target the right room.
    struct InviteContext { std::string room_id; std::string inviter_id; };
    std::optional<InviteContext> current_invite_;
    // The room whose timeline the message view currently displays. Differs
    // from current_room_id_ between a room switch and the timeline-reset
    // that fills it. Used to tell a genuine switch (gate the display) from
    // an in-place reconnect / gappy-sync reset of the room already shown
    // (refresh in place, no blank). Updated by each shell's
    // handle_timeline_reset_ui_.
    std::string view_displayed_room_id_;
    /// Rooms to restore on next on_rooms_updated_: [0] is the active tab,
    /// [1..N] are background tabs. Cleared once fully consumed.
    std::vector<std::string> pending_restore_rooms_;
    /// Pop-out room IDs to reopen after the room list becomes available.
    /// Populated from Settings::popout_windows at session restore time.
    std::vector<std::string> pending_restore_popouts_;

    // Populate pending_restore_popouts_ from Settings::popout_windows, once
    // per session restore (idempotent: no-op when already populated).
    // Platform shells call this right after setting pending_restore_rooms_.
    void populate_pending_restore_popouts_();
    std::vector<std::string> space_stack_;

    // Saved room-list state for each level of space navigation (parallel to
    // space_stack_). The top entry holds the parent's collapse + scroll state,
    // restored when the user presses back.
    std::vector<SpaceNavFrame> space_nav_frames_;

    // ── Thread panel state ────────────────────────────────────────────────────
    // STAY ON ShellBase: the four native shells read thread_panel_ and
    // current_thread_root_ directly (macOS via `using`). These are written by
    // apply_thread_transition_ from the controller's computed transition.
    ThreadPanel thread_panel_      = ThreadPanel::Closed;
    ThreadPanel thread_panel_prev_ = ThreadPanel::Closed;
    std::string current_thread_root_;
    // Owns the pure transition computation + the thread-list backfill guards
    // (reached_start / paginating) and the paginate() driver. ShellBase keeps
    // thread_panel_ / current_thread_root_ above and applies side-effects.
    ThreadPanelController thread_panel_ctl_;
    bool compose_typing_active_ = false;
    bool relayout_scheduled_ = false; // a coalesced relayout flush is pending

    // ── Image caches ──────────────────────────────────────────────────────────
    // Bounded, TTL'd image caches. Images stay warm for the TTL window after a
    // room switch / scroll-off, then get reclaimed; widgets pin what they
    // display by holding the ImageRef from acquire() (see PixmapCache).
    //
    // Holds server-scaled thumbnails: avatars (≤80px) and inline static media
    // previews (image/video/url-preview). Separate from image_cache_ so small
    // thumbnails kept resident for scrolling are never evicted by a large
    // full-size image, and vice versa. Thumbnails are NOT pinned by any widget
    // (they are protected while painted by peek() refreshing their TTL). A long
    // TTL keeps them resident across idle periods so returning to a static
    // window does not flash blank avatars; the byte budget still bounds memory.
    bool media_disk_cache_pruned_ = false;
    bool waveform_store_inited_ = false;

    // ── Full-resolution lightbox cache ─────────────────────────────────────────
    // Full-res decodes for the image viewer (main window AND pop-out windows,
    // all four shells). Separate from image_cache_, which on Qt6 holds only the
    // 320px inline-bound decode. Keyed by the PLAIN source token / avatar mxc —
    // the same key the viewer overlay looks up via media_url_ — so the viewer
    // providers can consult it first. Bounded by a simple insertion-order FIFO
    // cap; the lightbox shows one image at a time and recently-viewed ones are
    // cheap to re-decode from the namespaced ("fullres:") disk cache.
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> viewer_fullres_;
    std::vector<std::string> viewer_fullres_order_; // FIFO eviction order
    std::unordered_set<std::string> viewer_fullres_in_flight_;
    static constexpr std::size_t kViewerFullresCacheMax_ = 6;

    // ── Shared view pointers ──────────────────────────────────────────────────
    // The root MainAppWidget and its RoomView, set once by each shell right
    // after it builds the widget tree and BEFORE sync starts. ShellBase's
    // concrete handle_*_ui_ implementations drive the view through these (the
    // per-shell native surface is repainted via request_relayout_/repaint_).
    views::MainAppWidget* main_app_ = nullptr;
    views::RoomView* room_view_ = nullptr;

    // MSC2545 emoticon flat list (shortcode popup source). Rebuilt on
    // handle_image_packs_updated_ui_.
    std::vector<tesseract::ImagePackImage> cached_emoticons_;

    // ── Media fetch dedup sets ────────────────────────────────────────────────
    std::unordered_set<std::string> voice_prefetched_;
    // Voice/audio playback bytes warmed asynchronously so the play/scrub UI
    // path never blocks on a network fetch. Filled by voice_bytes_or_fetch_
    // when a download lands; consumed (moved out) by the next play/scrub of the
    // clip. Cleared on logout / cache wipe, and capped (see voice_bytes_or_fetch_)
    // so warmed-but-never-replayed clips can't retain audio files unbounded.
    std::unordered_map<std::string, std::vector<std::uint8_t>> voice_bytes_cache_;
    std::unordered_set<std::string> voice_bytes_in_flight_;
    std::unordered_set<std::string> voice_waveform_in_flight_;
    std::unordered_set<std::string> video_thumb_in_flight_;
    std::unordered_set<std::string> reply_details_requested_;
    std::unordered_set<std::string> media_fetches_in_flight_;
    std::unordered_set<std::string> sticker_fetches_in_flight_;
    std::unordered_set<std::string> emoji_fetches_in_flight_;
    std::unordered_set<std::string> tile_fetches_in_flight_;
    std::unordered_set<std::string> tile_fetch_failed_;
    // Keys for which on_media_bytes_ready_ received non-empty bytes but the
    // platform decoder rejected them (e.g. unsupported format, corrupt data).
    // Guards ensure_media_image_ / ensure_media_thumbnail_ / ensure_*_avatar_
    // from re-queuing fetches that will always fail. Cleared on logout/cache
    // wipe so a re-login or server fix can recover.
    std::unordered_set<std::string> media_decode_failed_;
    // Tracks which event_ids have had ensure_row_media_() called so that the
    // lazy visible-range callback skips events already prepped in build_rows_().
    // Cleared on room switch in after_active_room_changed_().
    std::unordered_set<std::string> media_prepped_event_ids_;

    // Keys whose media fetch returned empty (network error / 5xx / timeout).
    // Unlike media_decode_failed_ (permanent), these back off and recover: a
    // growing cooldown throttles re-requests so a dead-homeserver avatar stops
    // hammering on every sync tick, but a recovered server reloads after the
    // window. Keyed by the same mxc/url the ensure_* guards already use.
    struct MediaFetchBackoff
    {
        std::uint32_t attempts = 0;
        std::chrono::steady_clock::time_point retry_after{};
    };
    std::unordered_map<std::string, MediaFetchBackoff> media_fetch_failed_;

    bool media_fetch_backed_off_(const std::string& key) const
    {
        auto it = media_fetch_failed_.find(key);
        return it != media_fetch_failed_.end() &&
               std::chrono::steady_clock::now() < it->second.retry_after;
    }
    void note_media_fetch_failed_(const std::string& key);
    void note_media_fetch_ok_(const std::string& key);

    // ── Async media request registry ──────────────────────────────────────────
    // Correlates an outstanding fetch_media_async / get_url_preview_async call
    // (by request_id) with the UI-thread completion to run when on_media_ready /
    // on_url_preview_ready fires. UI-thread-only — on_media_ready arrives via
    // post_to_ui_. A request dropped from the map (room-switch cancel) makes any
    // late callback a no-op. group_id (a non-zero hash of the originating room,
    // or 0 for never-cancelled requests like map tiles) lets cancel_media_group_
    // drop a room's pending requests in bulk and abort their Rust tasks.
    struct PendingMediaReq
    {
        std::uint64_t group_id = 0;
        // Exactly one is set, matching the request type.
        std::function<void(std::vector<std::uint8_t>&&)> on_bytes;
        std::function<void(std::string&&)>               on_preview;
        // Run when the request is cancelled (room switch) instead of completing.
        // Clears the caller's dedup-set key so the media can be re-requested on
        // re-entry; without this the key would stay stuck in-flight forever.
        std::function<void()>                            on_cancel;
        // Display/cache key this request feeds (the row's media fetch_token), or
        // empty for requests not tied to a visible row. Used to drop the
        // media_key_to_req_ reverse-map entry when the request ends.
        std::string                                      priority_key;
    };
    std::unordered_map<std::uint64_t, PendingMediaReq> pending_media_;
    std::uint64_t next_media_req_id_ = 1;
    // Reverse map: a media display/cache key → the request_id currently fetching
    // it. Lets on_visible_rows_changed_ translate the visible rows' fetch tokens
    // into the request_ids to raise via client_->prioritize_media. At most one
    // in-flight request per key (the dedup sets enforce this), so the mapping is
    // unambiguous. Populated by begin_media_req_, dropped on completion/cancel.
    std::unordered_map<std::string, std::uint64_t> media_key_to_req_;
    // Cancellation group of the currently-active room's timeline media. When the
    // active room changes, after_active_room_changed_ cancels this group's
    // still-pending downloads before adopting the new room's group.
    std::uint64_t active_media_group_ = 0;

    // Stable non-zero group id for a room's media (so a switch cancels the right
    // set). 0 is reserved for ungrouped / never-cancelled requests.
    static std::uint64_t media_group_for_room_(const std::string& room_id)
    {
        if (room_id.empty())
            return 0;
        std::uint64_t h = std::hash<std::string>{}(room_id);
        return h == 0 ? 1 : h;
    }

    // Allocate a request_id and register a bytes-completion. Returns the id to
    // pass to client_->fetch_media_async. `on_cancel` clears the caller's dedup
    // key if the request is cancelled before completing.
    std::uint64_t begin_media_req_(
        std::uint64_t group_id,
        std::function<void(std::vector<std::uint8_t>&&)> on_bytes,
        std::function<void()> on_cancel = {},
        std::string priority_key = {})
    {
        std::uint64_t id   = next_media_req_id_++;
        if (!priority_key.empty())
            media_key_to_req_[priority_key] = id;
        pending_media_[id] = PendingMediaReq{
            group_id, std::move(on_bytes), {}, std::move(on_cancel),
            std::move(priority_key)};
        on_inflight_ui_();
        return id;
    }

    // Allocate a request_id and register a URL-preview completion.
    std::uint64_t begin_url_preview_req_(
        std::uint64_t group_id, std::function<void(std::string&&)> on_preview,
        std::function<void()> on_cancel = {})
    {
        std::uint64_t id   = next_media_req_id_++;
        pending_media_[id] = PendingMediaReq{
            group_id, {}, std::move(on_preview), std::move(on_cancel)};
        on_inflight_ui_();
        return id;
    }

    // Abort and drop every pending media request in `group_id` (room switch).
    // Runs each dropped request's on_cancel so its dedup key is freed and the
    // media can be re-requested when the room is re-entered.
    void cancel_media_group_(std::uint64_t group_id)
    {
        if (group_id == 0 || !client_)
            return;
        client_->cancel_media_group(group_id);
        for (auto it = pending_media_.begin(); it != pending_media_.end();)
        {
            if (it->second.group_id == group_id)
            {
                if (!it->second.priority_key.empty())
                    media_key_to_req_.erase(it->second.priority_key);
                if (it->second.on_cancel)
                    it->second.on_cancel();
                it = pending_media_.erase(it);
            }
            else
                ++it;
        }
        on_inflight_ui_();
    }

    // DM creation in-flight guard. Keyed by target user_id.
    std::unordered_set<std::string> dm_in_flight_user_ids_;

    // ── Quick-switcher user mode ("@" → start a DM by mxid) ────────────────────
    // Roster of known users (DM partners + members of joined rooms), keyed by
    // mxid. Built lazily on the worker pool the first time the switcher enters
    // user mode; invalidated on account switch and when the active account's
    // room set changes. Live-resolved unseen users are inserted here too.
    // UI-thread access only.
    std::unordered_map<std::string, tesseract::RoomMember> known_users_;
    bool known_users_built_    = false;
    bool known_users_building_ = false;
    // Order-independent fingerprint (XOR of per-id hashes) of the active
    // account's joined-room-id set, so the roster is invalidated when the set
    // *changes* — including a same-cycle join+leave that leaves the count equal.
    std::size_t known_users_room_set_hash_ = 0;
    // Fingerprint of the capped (top-N most-recently-active) set of quiet-unread
    // rooms — combining each (room_id, unread_count) pair — so the one-shot
    // unread prefetch only re-fires when the *prefetch-relevant* set changes (a
    // new unread room enters the top-N, or an existing one's unread_count grows).
    // Avoids a redundant FFI call on every sync tick. UI-thread access only;
    // reset to 0 on account switch so the incoming account re-fires.
    std::size_t unread_prefetch_fingerprint_  = 0;
    std::size_t bridge_check_fingerprint_     = 0;
    // Max unread rooms to one-shot prefetch per reconcile; the rest (lower
    // last_activity_ts) are dropped — this resize *is* the LRU eviction.
    static constexpr std::size_t kUnreadPrefetchCap = 20;
    // The current user-mode needle (query with the leading '@' stripped), so an
    // async profile-resolve can re-emit results against the latest query.
    std::string last_user_query_;
    // Monotonic query generation; bumped on every user-mode query change so a
    // late profile-resolve from a superseded keystroke is dropped. Read from
    // worker threads, written on the UI thread → atomic.
    std::atomic<std::uint64_t> user_resolve_gen_{0};
    // Skip member enumeration for rooms larger than this when building the
    // roster, to keep the one-off build cheap (live-resolve still covers them).
    static constexpr std::size_t kRosterMaxRoomMembers = 512;
    // Emit partial roster results to the switcher after every N rooms scanned,
    // so a long build shows incremental progress instead of nothing-then-all.
    static constexpr std::size_t kRosterEmitBatchRooms = 8;
    // Per-build cancellation token, owned by both the shell and the worker
    // (shared_ptr so it outlives a shell teardown). Flipped to true on account
    // switch / roster invalidation / shell destruction so the worker loop bails
    // between rooms instead of finishing a full sweep (which would block the
    // destructor's thread-pool join). A new build installs a fresh token.
    std::shared_ptr<std::atomic<bool>> roster_build_cancel_;

    // ── MSC4278 media-preview gating ──────────────────────────────────────────
    // Per-room media_previews override + join_rule, keyed by room_id. Populated
    // by ensure_room_preview_override_ (async) on room switch. Absent → use the
    // global Settings value with an unknown (public-treated) join rule.
    std::unordered_map<std::string, tesseract::MediaPreviewOverride>
        room_preview_overrides_;
    std::unordered_set<std::string> room_preview_override_in_flight_;
    // request_id → room_id for in-flight room_media_preview_override_async calls.
    std::unordered_map<std::uint64_t, std::string> pending_preview_overrides_;
    // request_id → room_id for in-flight fetch_room_security_state_async calls
    // (Security & Privacy tab: encryption/join_rule/guest_access/history_
    // visibility are either never delivered via sync at all, or subject to
    // room_list_fingerprint staleness — see handle_room_security_state_ready_ui_).
    std::unordered_map<std::uint64_t, std::string> pending_security_state_requests_;
    // request_id → UserProfilePanel* for in-flight get_extended_profile_async
    // (user panel case). Absence = own-profile fetch.
    std::unordered_map<std::uint64_t, views::UserProfilePanel*>
        pending_user_profiles_;
    // request_id → {mxid, gen} for in-flight resolve_user_profile_async.
    std::unordered_map<std::uint64_t, std::pair<std::string, std::uint64_t>>
        pending_resolve_requests_;
    // Event IDs the user explicitly revealed (click-to-load), bypassing the
    // preview gate for that one item. Cleared on logout / account switch.
    std::unordered_set<std::string> revealed_events_;

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
    /// Last fetched own extended profile (MSC4133). Populated by
    /// fetch_own_extended_profile_async_() after server_info_ confirms support.
    tesseract::ExtendedProfile own_extended_profile_;

    // ── Sync / backup state ───────────────────────────────────────────────────
    RoomListState last_room_list_state_ = RoomListState::Init;
    BackupState last_backup_state_ = BackupState::Unknown;
    std::uint64_t last_imported_keys_ = 0;
    bool sync_progress_shown_ = false;
    bool offline_             = false;
    /// Extra in-flight HTTP request count (excludes the sync long-poll).
    std::uint32_t last_inflight_ = 0;
#ifndef NDEBUG
    /// Newline-joined list of active in-flight operation labels (debug builds
    /// only). Set by EventHandlerBase::on_inflight_changed_debug and read by
    /// on_inflight_ui_() to append to the tooltip.
    std::string last_inflight_urls_;
#endif
    /// Accumulated rotation phase [0,1) for the inflight ring animation.
    float         spin_accum_phase_  = 0.0f;
    std::int64_t  spin_last_tick_ms_ = 0;
    /// Generation counter for show_status_message_ auto-clear: a late-firing
    /// callback only calls on_restore_status_ui_() if the gen still matches.
    std::uint32_t status_msg_gen_ = 0;
    /// True while a persistent (auto_clear_ms=0) status override is active.
    /// refresh_sync_status() checks this to avoid clobbering the override.
    bool status_override_active_ = false;

    // ── Encryption setup overlay ──────────────────────────────────────────────
    // True once show_encryption_setup_overlay_() has been called this session;
    // guards against raising the overlay a second time on subsequent sync ticks.
    bool encryption_setup_shown_     = false;
    // Set when the user dismisses the overlay (Skip or Done). Prevents it from
    // re-appearing if recovery_state() returns Disabled again.
    bool encryption_setup_dismissed_ = false;

    // ── Cross-signing / SAS device verification ───────────────────────────────
    bool verification_banner_dismissed_ = false;
    std::string active_verification_flow_id_; // "" = no flow in progress

    // ── Pagination ────────────────────────────────────────────────────────────
    struct PaginationState
    {
        bool in_flight = false;
        bool reached_start = false;
        bool fwd_in_flight = false;    // forward paginate guard
        bool reached_end = false;
        bool is_focused = false;       // true = using with_focus timeline
        bool returning_to_live = false; // snap to bottom on next timeline reset
        std::string focus_event_id;    // scroll target after timeline reset
    };
    std::unordered_map<std::string, PaginationState> pagination_;

    // Correlation map for in-flight async paginations.
    // request_id → room_id; cleared in handle_paginate_result_ui_.
    // true = backward paginate, false = forward paginate.
    std::unordered_map<std::uint64_t, std::pair<std::string, bool>>
        pending_paginates_;
    std::uint64_t next_paginate_id_ = 1;

    // ── Async room actions ────────────────────────────────────────────────────
    // These two types are public (stateless descriptors) so tests can name them;
    // MSVC does not honor a derived-class `using` re-export of a protected nested
    // enum the way GCC/Clang do.
public:
    enum class RoomActionKind { Accept, Join, Leave };
    struct PendingRoomAction
    {
        std::string room_id;
        RoomActionKind kind;
    };

protected:
    // request_id → action; cleared in handle_room_action_complete_ui_.
    std::unordered_map<std::uint64_t, PendingRoomAction> pending_room_actions_;
    std::uint64_t next_room_action_id_ = 1;

    // ── Read receipts ─────────────────────────────────────────────────────────
    // room_id → last event_id for which a receipt was sent in this session.
    std::unordered_map<std::string, std::string> last_sent_receipt_;
    static constexpr std::uint16_t kPaginationBatch = 50;
    // Larger batch for the initial fill on room open. Pagination is store-first
    // (matrix-sdk only reaches the network at a genuine gap), so a bigger count
    // pulls more already-cached history straight from disk — enough to fill a
    // maximized desktop window without a server round-trip. Scroll-up increments
    // keep using kPaginationBatch.
    static constexpr std::uint16_t kInitialFillBatch = 100;

    // ── Secondary (pop-out) room windows ──────────────────────────────────────
    // One window per room_id at most (raise-existing policy).
    // owned_secondary_windows_ holds lifetime; secondary_windows_ is a fast-
    // lookup index into it (raw pointers, always valid while owned_ holds them).
    std::vector<std::unique_ptr<RoomWindowBase>> owned_secondary_windows_;
    std::unordered_map<std::string, RoomWindowBase*> secondary_windows_;
    // Ref-count of active subscriptions per room_id across all secondary windows.
    std::unordered_map<std::string, int> room_subscription_refs_;

    // ── Warm-subscription LRU ─────────────────────────────────────────────────
    // Rooms stay subscribed after you leave them (their SDK timeline is reused
    // on return — see ShellBase::prune_warm_subscriptions_ + the SDK's
    // subscribe_room reuse). Without a cap a long session would accumulate one
    // live timeline + sliding-sync subscription + streaming task per room ever
    // visited. visited_lru_ tracks recency (front = most recently active); rooms
    // that are the active room, an open tab, or pinned by a pop-out are always
    // kept, and at most kWarmRoomsMax *other* warm rooms are retained — older
    // ones are unsubscribed.
    static constexpr std::size_t kWarmRoomsMax = 4;
    std::vector<std::string> visited_lru_;
    // Move room_id to the front of visited_lru_ (most-recently-active).
    void touch_visited_room_(const std::string& room_id);
    // Pure selection: given the always-keep set and the warm cap, return the
    // rooms to unsubscribe and drop them from visited_lru_. Keeps protected
    // rooms regardless of position; keeps the newest `warm_cap` non-protected.
    std::vector<std::string>
    select_warm_evictions_(const std::unordered_set<std::string>& keep,
                           std::size_t warm_cap);
    // Build the keep-set (active + open tabs + pop-out-pinned) and unsubscribe
    // every room select_warm_evictions_ returns. Cheap; runs on each switch.
    void prune_warm_subscriptions_();

    // ── Worker thread pools ───────────────────────────────────────────────────
    // Two pools with different concurrency levels:
    //   pool_     — 2 threads for &self work: image decode, disk-cache I/O, and
    //               a handful of blocking &self FFI calls (profile reads, config
    //               reads). The high-volume media downloads (avatars, thumbnails,
    //               full images, stickers/emoji picker images, tiles, URL previews,
    //               voice) run as non-blocking tokio tasks via fetch_media_async
    //               and complete on the on_media_ready callback — they never pin
    //               a pool thread. These hold no C++ mutex; both can run in
    //               parallel.
    //   mut_pool_ — 1 thread for &mut FFI (subscribe_room, send_*, etc.).
    //               Serialised by design so ffi_mu is never contended.
    struct WorkerPool
    {
        explicit WorkerPool(int threads);
        ~WorkerPool();

        // Enqueue fn for execution on the next free thread.
        void post(std::function<void()> fn);

        // Stop accepting new work, drop pending tasks, and join all threads.
        // Safe to call multiple times (no-op after the first call).
        void drain();

        // Number of tasks waiting in the queue (not yet executing).
        // Lock-free read; acceptable to see a slightly stale count for display.
        size_t pending_count() const
        {
            return pending_.load(std::memory_order_relaxed);
        }

        std::deque<std::function<void()>> queue_;
        std::mutex                        mu_;
        std::condition_variable           cv_;
        bool                              stop_ = false;
        std::vector<std::thread>          threads_;
        // Tracks tasks waiting in queue_. Mutated under mu_; readable lock-free.
        std::atomic<size_t>               pending_{0};
        // Posted outside mu_ whenever pending_ changes. Cleared in drain().
        std::function<void()>             on_change_;
    };
    WorkerPool pool_{2};
    WorkerPool mut_pool_{1};

    // ── Media kind tag ────────────────────────────────────────────────────────
    enum class MediaKind : std::uint8_t
    {
        RoomAvatar,    // → thumbnail_cache_, triggers room-list repaint
        UserAvatar,    // → thumbnail_cache_, triggers message-list repaint
        MediaImage,    // → anim_cache_ or image_cache_ (full-size)
        MediaThumbnail,// → anim_cache_ or thumbnail_cache_ (inline preview)
        Tile,          // → image_cache_["tile:z/x/y"], triggers full message-list repaint
        Sticker,       // → image_cache_ (full-size), decode clamped to kStickerSize
        Reaction,      // → image_cache_ (full-size), decode clamped to reaction icon size
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
    };

    // Run the shared disk-load → UI hop → hit-deliver / miss-fetch → persist →
    // deliver state machine described by `spec`. Preserves the Phase-1
    // alive_-token UI-thread guarding (all continuations route through
    // post_to_ui_alive_). The caller must have already done the in-memory cache
    // check and inserted its in-flight key.
    void run_media_fetch_(MediaFetchSpec spec);

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

    // Core handler: fast path (existing DM), in-flight dedup, loading state,
    // async get_or_create_dm, navigate on success. Always called on UI thread.
    void handle_open_dm_(const std::string& user_id);

    // ── Quick-switcher user mode helpers (all UI-thread unless noted) ──────────
    // Handle a user-mode query ('@'-prefixed): ensure the roster is built, emit
    // local matches now, and live-resolve a fully-typed unseen mxid (debounced).
    void handle_user_query_(const std::string& query);
    // Build the known-users roster (DM partners + room members) on a worker
    // thread, then swap it in and re-emit results. No-op if already building.
    void build_known_users_roster_();
    // Filter known_users_ by `needle` (matches display name + mxid, case-
    // insensitive substring; empty needle = all), sorted and capped for display.
    std::vector<views::QuickSwitcher::UserEntry>
    filter_known_users_(const std::string& needle) const;
    // Push the current filtered roster into the switcher (no-op if not mounted).
    void emit_user_results_();
    // Insert a live-resolved profile into the roster, then re-emit results.
    void merge_resolved_user_(const tesseract::UserProfile& p);
    // Merge one entry into known_users_, keeping the first non-empty name/avatar.
    void merge_roster_entry_(const std::string& id, std::string display_name,
                             const std::string& avatar_url);
    // Drop the cached roster (account switch / room-set change). Bumps the
    // resolve generation so in-flight resolves are discarded.
    void invalidate_known_users_();

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

    // Returns true if the platform modifier key for "open in new window" is held.
    // Qt6 = Ctrl, GTK4 = Ctrl, Win32 = Ctrl, macOS = Command (⌘).
    virtual bool is_ctrl_held_() const = 0;

    // Switch the active account to `user_id`. Called by on_account_picker_select_
    // after the dedicated-window check. Each platform overrides with its own
    // account-switch logic (switchActiveAccount / switch_active_account / etc.),
    // which now defers to switch_active_account_impl_ + refresh_account_ui_after_switch_.
    virtual void switch_active_account_(const std::string& user_id) = 0;

    // Platform-agnostic account-switch bookkeeping, shared by every shell's
    // switchActiveAccount / switch_active_account / _switchActiveAccount:. Looks
    // up the target AccountSession; returns false (no-op) if it isn't found or is
    // already active with a bound client. Otherwise it:
    //   - unsubscribes the previous account's open room when not pinned
    //     (room_subscription_refs_.count(current_room_id_) == 0) so the old
    //     account's timeline stops streaming after the surface swap — folded in
    //     from the Phase-1.2 fix so ALL shells get it;
    //   - clears per-account, room-id-keyed state (current_room_id_, tabs_,
    //     active_tab_idx_, space_stack_, pagination_, reply_details_requested_)
    //     so it can't bleed into the incoming account;
    //   - saves the outgoing account's verification-banner state, resets server
    //     info, swaps active_account_ + the client_ / event_handler_ aliases and
    //     the my_user_id_ / my_display_name_ / my_avatar_url_ identity;
    //   - computes pending_restore_rooms_ from open_rooms / last_room (rotating
    //     last_room to [0]) and populate_pending_restore_popouts_();
    //   - rebinds settings_controller_ (client + up_connector) when present;
    //   - swaps the per_account_rooms_ / per_account_invites_ snapshots into
    //     rooms_ / invites_, fires on_invites_updated_(), drops current_invite_;
    //   - loads the incoming account's verification_banner_dismissed_;
    //   - persists the on-disk index (active = the new uid).
    // It does NOT touch native widgets (user strip, room-list view, message
    // surface, status bar, tray) — the shell does that in
    // refresh_account_ui_after_switch_(). UI-thread only.
    bool switch_active_account_impl_(const std::string& user_id);

    // Native UI refresh after switch_active_account_impl_ has updated all shared
    // state: each shell repopulates its account-avatar strip, refreshes the
    // room-list view, shows/restores the active room or tab session in its native
    // surface, updates the status bar, and (re)binds native pickers / tray. Called
    // by switch_active_account_ at the tail of a successful switch.
    virtual void refresh_account_ui_after_switch_() = 0;

    // Spawn a new main window pre-assigned to `account`. Called by
    // on_account_picker_select_ on Ctrl+click when no dedicated window exists
    // for the uid yet. Each shell creates its native window, calls
    // set_initial_account, then hand_account_to_spawned_window_() (shared) to take
    // ownership of the account, and shows it.
    virtual void spawn_main_window_(
        std::shared_ptr<tesseract::AccountSession> account) = 0;

    // Shared spawn wiring: hand ownership of `session`'s account to a freshly
    // constructed window `win` (whose set_initial_account() has already run, but
    // whose deferred doLogin() has NOT). Called from spawn_main_window_() on the
    // spawning window. It (1) re-points the account's sole event bridge at `win`
    // so every SDK callback now reaches it, (2) seeds `win`'s room/invite caches
    // from this window so its list paints immediately instead of waiting for the
    // next sync push, (3) marks `win` pinned, and (4) registers `win` as the
    // dedicated window for the account.
    void hand_account_to_spawned_window_(
        ShellBase* win, const std::shared_ptr<tesseract::AccountSession>& session);

    // Copy this window's cached rooms/invites for `uid` into `*this` from `src`.
    // Used to seed a newly-spawned window for instant paint.
    void seed_account_caches_from_(ShellBase* src, const std::string& uid);

    // Register / release this window as the dedicated owner of its active account
    // so the account picker raises this window instead of switching in place.
    // release_ also re-points the mapping to another live window still showing the
    // account, if any.
    void claim_dedicated_for_active_();
    void release_dedicated_for_active_();

    // Called from each shell's window-close handler before teardown. Hands this
    // window's account's sole event bridge back to the primary window (so its SDK
    // callbacks keep reaching a live window), releases this window's dedicated
    // mapping, and releases tray ownership. No-op-safe for the primary window.
    void on_window_closing_();

    // Re-point an account's sole event bridge at `win`. The bridge is stored
    // type-erased as IEventHandler* but is always an EventHandlerBase, so the
    // downcast is safe. Static so close/spawn paths share one definition.
    static void rebind_account_bridge_(tesseract::AccountSession& session,
                                       ShellBase* win);

    // True for windows spawned via spawn_main_window_ (pinned to one account,
    // cannot switch the active account in place). The startup window is false.
    bool is_pinned_window_ = false;
    void mark_pinned_window_() { is_pinned_window_ = true; }
    bool is_pinned_window() const { return is_pinned_window_; }

    // Central picker routing. Each platform's on_select lambda delegates here.
    // Ctrl+click → spawn_main_window_; plain click → switch_active_account_.
    void on_account_picker_select_(const std::string& uid);

    // ── Startup account restore ───────────────────────────────────────────────
    // Outcome of restore_all_accounts_(): lets each shell decide between the
    // empty-accounts login fallback and finishing login on the active account
    // (that decision touches native login_view_ widgets, so it stays in the
    // shell).
    struct RestoreResult
    {
        bool        any_accounts       = false; // at least one account restored
        bool        any_restore_failed = false; // ≥1 stored account failed restore
        std::string restore_error;              // last restore failure message
        std::string active_uid;                 // uid to make active (empty when none)
    };

    // Platform-agnostic startup restore loop, shared by every shell's primary-
    // window startup entry (doLogin / do_login / start_login / beginLogin) AFTER
    // the is_secondary_window_startup_ gate. Runs the legacy-layout migration,
    // loads the account index, and for each stored uid: restores the session
    // (skipping + recording failures), caches display name / avatar / prefs,
    // builds the per-account event bridge (make_account_bridge_) and starts
    // sync, then installs the native per-account notifier
    // (install_account_notifier_) and the Linux-only UnifiedPush connector
    // (install_account_up_connector_), and adds the account to the manager.
    // Returns a RestoreResult; the caller does the native empty-fallback /
    // finish-login decision. UI-thread only.
    RestoreResult restore_all_accounts_();

    // ── Add-account login finalize ────────────────────────────────────────────
    // Outcome of finalize_login_(): lets each shell run the native finish (or the
    // native duplicate-reject UI) without re-deriving state. On a successful add,
    // `ok` is true and `user_id` names the account that was added + made active;
    // on a duplicate (already-signed-in) it is rejected with `rejected_duplicate`
    // true and `user_id` set so the shell can show "Already signed in as <uid>";
    // on any hard failure (empty user id, empty session, persist/restore error)
    // `ok` is false and `error` carries a message (empty when the platform path
    // had nothing to report).
    struct FinalizeLoginResult
    {
        bool        ok                 = false; // account added + made active
        bool        rejected_duplicate = false; // uid already signed in
        std::string user_id;                    // the new (or duplicate) uid
        std::string error;                      // failure detail (when !ok)
    };

    // Platform-agnostic core of each shell's on_login_succeeded, run after OAuth
    // completes for a NEWLY added account on pending_login_client_. Fetches the
    // user_id; rejects (rejected_duplicate) if account_manager_.find(uid); else
    // exports the session, drops the pending client (releasing SQLite handles),
    // renames the pending temp dir → the final per-account dir (copy+remove
    // fallback for cross-filesystem), saves the account JSON, reopens a fresh
    // Client at the final path, restore_session + caches display name / avatar /
    // prefs, builds the per-account bridge (make_account_bridge_) + start_sync,
    // installs the native notifier (install_account_notifier_) and Linux-only
    // UnifiedPush connector (install_account_up_connector_), adds the account, and
    // updates the on-disk index (active = the new uid). On both the duplicate and
    // hard-failure paths it clears pending_login_client_ / pending_login_temp_dir_
    // so the shell only owns the native UI restore. Does NOT touch native widgets
    // (login-view dismiss, surface switch, status bar) — the shell does the
    // native finish using the returned result. The shell must call set_client(
    // nullptr) on its login view BEFORE this when it owns a raw alias to
    // pending_login_client_ (it is reset here). UI-thread only.
    FinalizeLoginResult finalize_login_();

    // ── Active-account logout ─────────────────────────────────────────────────
    // Outcome of logout_active_account_impl_(): lets each shell decide between the
    // empty-accounts native login fallback and the (already-completed) switch to a
    // surviving account. When `logged_out` is false the call was a no-op (no active
    // account) and the shell must do nothing. When `has_remaining` is true the impl
    // has ALREADY switched to `next_uid` (via switch_active_account_impl_ +
    // refresh_account_ui_after_switch_), so the shell needs no native follow-up
    // beyond its own status line; when false, no accounts remain and the shell must
    // show its native login view.
    struct LogoutResult
    {
        bool        logged_out   = false; // an account was actually signed out
        bool        has_remaining = false; // another account exists + is now active
        bool        ok            = false; // client_->logout() succeeded
        std::string logged_out_uid;        // the uid that was signed out
        std::string next_uid;              // the surviving uid switched to (if any)
    };

    // Platform-agnostic teardown for each shell's logoutActiveAccount /
    // logout_active_account / _logoutActiveAccount. Run on the active account; a
    // no-op (logged_out=false) when there is none. It:
    //   - captures the active uid;
    //   - unsubscribes the current open room when not pinned by a pop-out
    //     (room_subscription_refs_.count(current_room_id_) == 0) — same guard as
    //     switch_active_account_impl_, folded in so Qt/Win get it too;
    //   - logs out the UnifiedPush connector (when present) and presence;
    //   - calls client_->logout() and SURFACES a failure via show_status_message_
    //     ("Sign out failed: <msg>") — converged so every shell reports it;
    //   - stop_sync() (BEFORE remove_account, per Phase-1 lifetime ordering);
    //   - clears the on-disk account (SessionStore::clear_account) and the
    //     per_account_rooms_ / per_account_invites_ snapshots;
    //   - refreshes the tray aggregate (notify_tray_unread_) so a stale unread dot
    //     clears — converged so every shell does it;
    //   - removes the account from AccountManager, resets active_account_ / the
    //     client_ / event_handler_ aliases, and the agnostic visible state
    //     (rooms_/invites_/current_invite_/space_stack_/identity/pagination/…);
    //   - updates the on-disk index (removes the logged-out uid; clears
    //     active_user_id when none remain);
    //   - BRANCHES: if other accounts remain it switches to accounts().front()
    //     via switch_active_account_impl_ + refresh_account_ui_after_switch_ (the
    //     shared Task-3.3 path) and returns has_remaining=true / next_uid set;
    //     otherwise returns has_remaining=false and leaves the native login-view
    //     swap to the shell.
    // Does NOT touch native widgets in the empty-accounts branch (login view,
    // surface visibility) — the shell does that using the returned result.
    // UI-thread only.
    LogoutResult logout_active_account_impl_();

    // Build the shell's concrete IEventHandler bridge for `uid`, with set_user_id
    // already called. The bridge TYPE is native: EventBridge (Qt6, a QObject so
    // the marshalling QMetaObject::invokeMethod has a receiver), EventHandlerBase
    // (GTK4 / Win32 / macOS). restore_all_accounts_ then calls start_sync on it.
    virtual std::unique_ptr<IEventHandler>
    make_account_bridge_(const std::string& uid) = 0;

    // Build and store the native per-account notifier on session.notifier. The
    // notifier's on-click closure must capture session.user_id and, when fired,
    // switch the active account to that uid (switch_active_account_) then
    // navigate to the clicked room — with any platform focus token handling
    // (Wayland xdg-activation on Linux). macOS has no in-app notifier, so its
    // override is a no-op. Called once per account during restore.
    virtual void install_account_notifier_(AccountSession& session) = 0;

    // Build, start, and store the native per-account UnifiedPush connector on
    // session.up_connector. Linux-only (registers with the D-Bus distributor);
    // Win32 / macOS have no UnifiedPush, so the default is a no-op.
    virtual void install_account_up_connector_(AccountSession& /*session*/) {}

    // True when this window's startup should reuse the already-restored,
    // already-syncing accounts from the shared AccountManager instead of
    // re-restoring from disk. A spawned (secondary) window finds the manager
    // already populated, has a pinned active_account_ (via set_initial_account),
    // and has not bound a client yet. The first (primary) window finds the
    // manager empty; the primary re-login path runs with client_ already set.
    // Platform startup entries (doLogin / do_login / start_login / beginLogin)
    // check this first and, if true, bind the pinned account without restoring.
    bool is_secondary_window_startup_() const;

    // Post fn() onto the UI thread.
    // GTK4: g_idle_add   Qt6: QueuedConnection   Win32: PostMessage   macOS: dispatch_async
    virtual void post_to_ui_(std::function<void()> fn) = 0;

    // Liveness-guarded post_to_ui_: captures the alive_ token and only invokes
    // fn() if this ShellBase is still alive when the continuation runs on the UI
    // thread. Use this for any continuation enqueued from a worker body or an
    // SDK callback that dereferences `this`/members, so a continuation queued
    // before a spawned/secondary window (or account-switch) teardown safely
    // no-ops instead of using freed state.
    void post_to_ui_alive_(std::function<void()> fn)
    {
        auto a = alive_;
        post_to_ui_(
            [a, fn = std::move(fn)]() mutable
            {
                if (*a) fn();
            });
    }

    // Post fn() onto the UI thread after `ms` milliseconds (one-shot). Used by
    // debounce_(); shells should not need to call it directly.
    // Qt6: QTimer::singleShot   GTK4: g_timeout_add
    // Win32: SetTimer + a timer-id→closure map drained on WM_TIMER
    // macOS: dispatch_after(dispatch_get_main_queue())
    virtual void post_to_ui_after_(int ms, std::function<void()> fn) = 0;

    // Repaint the main app surface. request_relayout_ also re-runs measure +
    // arrange first (use after a change that affects layout — new/changed rows,
    // shown/hidden widgets); request_repaint_ only schedules a redraw.
    //   Qt6:  mainAppSurface_->relayout() / ->update()
    //   GTK4: main_app_surface_->relayout() / host().request_repaint()
    //   Win32: relayout() / InvalidateRect(...)
    //   macOS: [self _relayoutChatSurface] / _mainAppSurface->host().request_repaint()
    virtual void request_relayout_() = 0;
    virtual void request_repaint_() = 0;

    // Coalescing relayout. Instead of running a synchronous measure+arrange of
    // the whole widget tree on every call (which a sync burst does N times),
    // this posts a single deferred flush to the UI thread; further calls before
    // that flush runs are folded into it. The flush still calls the synchronous
    // request_relayout_() exactly once, so native-overlay positioning timing is
    // unchanged — only the redundant per-message passes are eliminated. Use for
    // hot, high-frequency paths (incoming-message handlers); keep
    // request_relayout_() where a later step in the same turn reads geometry.
    void schedule_relayout_();

    // Navigate the shell to room_id. Called on the UI thread.
    // Qt6/GTK4/Win32: delegates to navigate_to_room(id).
    // macOS (MacShell): delegates to tab_navigate_room(id).
    virtual void navigate_to_room_(const std::string& room_id) = 0;

    // Open the platform join-room dialog pre-filled with `prefill` (a room ID
    // or alias).  Default no-op — shells that have a join dialog override this.
    virtual void open_join_room_dialog_ui_(const std::string& /*prefill*/) {}

    // Returns true while a room-list search is active (search text is non-empty).
    // Shells that implement room search override this to suppress space-child
    // filtering during search (all rooms are shown unfiltered). Default: false.
    virtual bool is_room_search_active_() const { return false; }

    // Compute the filtered room list and push it to the shared RoomListView.
    // Shells call this from their own refreshRoomList() wrapper to avoid
    // duplicating the space-child filter and unread-override logic.
    void refresh_room_list_();

    // Show the chat-panel root view for a joined space. No-op if the room is
    // unknown or is not a space.
    void show_space_root_(const std::string& space_id);

    // Called after rooms_ is updated — shell refreshes the room-list widget.
    virtual void on_rooms_updated_() = 0;

    // Raise the encryption-setup modal overlay in the appropriate mode.
    // Each platform shell implements this to show EncryptionSetupOverlay
    // as a full-window overlay on its MainAppWidget. Pure virtual so every
    // shell is required to implement it (Tasks 9–12).
    virtual void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) = 0;

    // Start the MSC4108 QR grant login flow: wires all callbacks on QRGrantView
    // and shows the overlay. Each shell overrides show/hide to manage the native
    // check-code text field overlay.
    void start_qr_grant_overlay();
    virtual void show_qr_grant_overlay_() {}
    virtual void hide_qr_grant_overlay_() {}

    // Called when the forward picker opens: shell focuses its native text field.
    // Called when the forward picker closes: shell hides its native text field.
    virtual void focus_forward_picker_field_() {}
    virtual void hide_forward_picker_field_() {}

    // Wires every platform-agnostic callback on the encryption-setup overlay
    // (recovery/verify actions, clipboard, field readers, layout/dismiss).
    // Each shell only creates+stores the two native text fields then calls
    // this, instead of duplicating ~40 lines of identical wiring. Dismissal
    // hides the native fields via request_relayout_() (the on-layout pass
    // reads the overlay's field-visibility), so the fields don't need to be
    // destroyed here. `host` and the field pointers must outlive the overlay.
    void wire_encryption_setup_callbacks_(
        views::EncryptionSetupOverlay& ov,
        tk::Host&                      host,
        tk::NativeTextField*           passphrase_field,
        tk::NativeTextField*           key_field);

    // User-initiated "Reset cryptographic identity" (from Settings → Privacy).
    // Shows the encryption-setup overlay in its reset-approval wait state,
    // starts the SDK cross-signing reset, opens the browser approval URL, and
    // (on success) hands off to the Fresh recovery-key setup. Shells wire their
    // SettingsView::on_reset_identity to this after closing the settings UI.
    void begin_crypto_identity_reset_();
    // Marshalled result of the in-progress cross-signing reset (from
    // EventHandlerBase::on_crypto_reset_result). Advances the overlay into
    // recovery setup on success, or shows the error on failure / cancellation.
    void handle_crypto_reset_result_ui_(bool ok, std::string message);

    // Returns the current RecoveryState byte from the SDK client.
    // Virtual so tests can inject a stub without a real client_.
    // 0=Unknown, 1=Disabled, 2=Enabled, 3=Incomplete.
    virtual uint8_t read_recovery_state_() const;

    // Whether a cross-signing identity already exists for our user, whether
    // this device is currently verified, and whether the cross-signing PRIVATE
    // keys are present locally. Used to disambiguate the Disabled recovery state
    // (see check_encryption_setup_). Virtual so tests can stub them without a
    // real client_.
    virtual bool read_own_identity_exists_() const;
    virtual bool read_device_verified_() const;
    virtual bool read_have_cross_signing_keys_() const;

    // True when a cross-signing identity exists for our user but its private
    // keys are NOT held locally — i.e. the identity was created on another
    // device and this one must verify/recover against it (vs. a fresh first
    // device whose own login-time bootstrap holds the keys). Shared by
    // check_encryption_setup_ (Fresh vs Recover) and the verification-banner
    // gating in the platform shells.
    bool foreign_cross_signing_identity_() const;

    // Called after invites_ is updated — shell refreshes the invite UI.
    // Non-pure: shells are added in Task H; until then the default no-op
    // prevents compilation errors across the four platform shells.
    virtual void on_invites_updated_()
    {
    }

    // Called on the UI thread when the aggregate unread/highlight state across
    // all signed-in accounts changes. Each shell overrides to forward to its
    // tray icon. Default no-op so shells without a tray (or with a tray that
    // failed to register) silently skip the update.
    virtual void on_tray_unread_changed_(bool /*has_unread*/,
                                         bool /*has_highlight*/)
    {
    }

    // Called on the UI thread when the total notification_count across all
    // signed-in accounts changes. Shells that expose a dock/taskbar badge
    // override this; others leave the default no-op.
    virtual void on_dock_badge_changed_(uint64_t /*count*/) {}

    // Navigate to the highest-priority unread room in the active account,
    // or no-op if none. Call on the UI thread from tray-click handlers.
    // Priority: highlight_count > 0 beats notification-only; ties broken by
    // most-recent last_activity_ts.
    void navigate_tray_unread_();

    // The highest-priority unread room (same selection as navigate_tray_unread_),
    // or nullptr if none. Borrowed; valid only until rooms_ changes.
    const RoomInfo* best_unread_room_() const;

    // If the highest-priority unread room is open in a pop-out window, raise
    // that window and return true. Tray-click handlers call this FIRST and
    // return early on true, so the pop-out is focused instead of the main
    // window. Returns false (no side effect) when there's no unread or it
    // isn't popped out.
    bool focus_tray_unread_popout_();

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

    // Drag-and-drop media probe. Each shell overrides this to detect gif/webp
    // animation and extract a video/audio thumbnail + duration off the UI
    // thread, then posts update_pending_attachment() back to `target` (a
    // pop-out window's compose bar, guarded by `alive`) or, when `target` is
    // null, the main window's compose bar. Default no-op so a shell can opt
    // out; pop-out windows call this through ShellBase so the same platform
    // probe serves the main and secondary windows. Pairs with
    // views::dispatch_file_drop, which invokes it for gif/webp/video/audio.
    virtual void extract_drop_media_(std::uint32_t /*pending_gen*/,
                                     std::vector<std::uint8_t> /*bytes*/,
                                     std::string /*mime*/,
                                     views::ComposeBar* /*target*/ = nullptr,
                                     std::shared_ptr<bool> /*alive*/ = nullptr)
    {
    }

    // Called on the UI thread when a URL preview fetch completes successfully.
    // Concrete: cache the preview, kick the image fetch, ping the message list
    // (main + secondary windows) and relayout. Identical for every shell.
    virtual void on_url_preview_ready_(const std::string& url,
                                       const Client::UrlPreview& preview);

    // Called on the UI thread when a URL preview fetch finished but produced
    // no usable card (failed / no metadata). Concrete: ping the message list
    // (main + secondary windows) so its room-switch gate stops waiting on this
    // URL (the row's height is unaffected — it never gained a preview card).
    virtual void on_url_preview_failed_(const std::string& url);

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

    // Open a platform image file picker (png/jpg/gif/webp filter) and deliver
    // (bytes, mime) to `cb` on the UI thread. Empty bytes signal cancellation.
    // Called from pick_and_set_room_avatar_ and SettingsController.
    virtual void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)> cb) = 0;

    // (Re)construct settings_controller_ with the three standard callbacks
    // (forwarding to post_to_ui_ / run_async_ / pick_image_file_) and wire its
    // UnifiedPush up-connector from the active account (nullptr on platforms
    // without UnifiedPush — a no-op there). Then calls bind_settings_controller_
    // for the native widget + dialog-hook binding. Rebuilds on every call to
    // match the per-login / per-account-switch behavior of the old inline sites.
    void ensure_settings_controller_();

    // Native binding hook invoked at the tail of ensure_settings_controller_():
    // bind settings_controller_ to the shell's native settings widget/view and
    // install native key/file dialog hooks (passphrase prompt, save/open file
    // dialogs, export/import result alerts). settings_controller_ is non-null
    // when this runs.
    virtual void bind_settings_controller_() = 0;

    // Open a file picker, upload the selected image as raw media, and set it
    // as the current user's avatar in `room_id`. No-op if not logged in.
    // Call from the UI thread (e.g. when /myroomavatar is sent with no args).
    void pick_and_set_room_avatar_(const std::string& room_id);

    // Room Settings view support ------------------------------------------

    // Open a file picker, upload the selected image as raw media (never
    // committing it to any room/profile state), and stage the resulting
    // mxc:// URI into `target` via set_staged_avatar(). The room-level
    // m.room.avatar state event is only sent when the user clicks Accept
    // (see apply_room_settings_). No-op if not logged in or `target` is
    // null. Call from the UI thread. `target` is whichever RoomSettingsView
    // instance requested the upload — room_view_->room_settings_view() for
    // a normal room, or main_app_->space_root()->settings_view() for a
    // space root — both operate on room ids generically, so this one
    // implementation serves both without duplicating the upload/retry logic.
    void stage_room_settings_avatar_upload_(const std::string& room_id,
                                            views::RoomSettingsView* target);

    // Outcome of a RoomSettingsView Accept commit.
    struct RoomSettingsCommitOutcome
    {
        bool ok = false;
        std::string error; // joined per-field failures, e.g. "name: M_FORBIDDEN"
    };

    // Send a state event for each populated optional field in `changes`,
    // attempting every one even if an earlier call fails so a partial
    // success (e.g. topic saved, avatar denied) isn't silently lost. The
    // media-override write (personal account data, not a state event) is
    // fire-and-forget and never contributes to the joined error string —
    // its optimistic cache update happens separately, in
    // commit_room_media_preview_override_, called by the caller only after
    // this function reports success. Takes the whole RoomSettingsChanges
    // (rather than exploding it into one param per field) since it's
    // already the exact aggregate RoomSettingsView produces from Accept.
    // Blocks — call from a worker thread (run_async_mut_).
    static RoomSettingsCommitOutcome apply_room_settings_(
        tesseract::Client* client, const std::string& room_id,
        const views::RoomSettingsChanges& changes);

    // Monotonic clock in ms from the SAME epoch the shell's animation
    // timer / anim_cache_.advance() uses (Qt: QDateTime msecs; GTK:
    // g_get_monotonic_time/1000; macOS: NSDate*1000; Win32: GetTickCount64).
    virtual std::int64_t monotonic_ms_() = 0;

    // Start the shell's shared animation frame-tick timer if it is not
    // already running. Default no-op (shells with no animated content).
    virtual void start_anim_tick_()
    {
    }

    // Stop the animation frame-tick timer. Default no-op. (Counterpart of
    // start_anim_tick_; called by tick_anim_ when nothing animated remains
    // visible.)
    virtual void stop_anim_tick_()
    {
    }

    // Repaint the regions changed by an animation frame: the main app surface
    // (partially, where the backend supports it) plus any visible picker
    // surfaces. Default no-op; each shell with animated content overrides.
    virtual void repaint_anim_frame_()
    {
    }

    // Start the shell's inflight-spinner tick timer if not already running.
    // Separate from the GIF animation timer so the two concerns are independent.
    virtual void start_inflight_tick_() {}

    // Stop the inflight-spinner tick timer.
    virtual void stop_inflight_tick_() {}

    // Repaint only the spinning inflight-dot widget/region. Called by
    // inflight_tick_() each tick. Default no-op; overridden in each shell.
    virtual void repaint_inflight_spinner_() {}

    // Returns true if the main application window is currently visible and not
    // minimized/iconified. Default: true — conservative, so shells that do not
    // override never accidentally prevent the timer from running.
    virtual bool is_main_window_visible_() const { return true; }

    // True if any shell-owned window (main or any secondary/pop-out window) is
    // currently visible. Used by tick_anim_() to gate the animation timer.
    bool any_window_visible_() const;

    // Concrete shared body of every shell's 60 Hz animation timer callback:
    // stop when nothing animated is on-screen, otherwise advance the frames
    // and repaint. Returns false when the timer was stopped (GTK uses this to
    // return G_SOURCE_REMOVE). Each shell's platform timer callback simply
    // calls this.
    bool tick_anim_();

    // Concrete shared body of every shell's inflight-spinner timer callback.
    // Advances the spin phase and repaints the inflight dot; stops the timer
    // when the dot is no longer needed or all windows are hidden.
    bool inflight_tick_();

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

    // Push a fresh thread reply timeline into the currently-open ThreadView.
    // Default implementations route into room_view_->thread_view() /
    // thread_list_view() when present; shells (Qt6/GTK4/Win32) inherit
    // unchanged. macOS / tests may override to redirect into their own
    // view tree.
    virtual void apply_thread_messages_(
        const std::string& thread_root,
        std::vector<views::MessageRowData> rows, bool room_switch);
    virtual void apply_thread_message_insert_(
        const std::string& thread_root, std::size_t index,
        views::MessageRowData row);
    virtual void apply_thread_message_update_(
        const std::string& thread_root, std::size_t index,
        views::MessageRowData row);
    virtual void apply_thread_message_remove_(
        const std::string& thread_root, std::size_t index);
    virtual void apply_threads_list_(std::vector<ThreadInfo> threads);

    // Call paginate_room_threads() on the background thread for the active room.
    // Guards (in thread_panel_ctl_) stop once the server reports no more pages.
    // Wired as ThreadListView::on_near_bottom in apply_thread_transition_.
    void paginate_threads_();

    // ── EventHandlerBase UI-thread hooks ─────────────────────────────────────
    // Called on the UI thread by EventHandlerBase after marshaling. Default
    // implementations are no-ops; each shell overrides what it needs.

    // Concrete: rebuild/insert/update/remove rows for the displayed room and
    // dispatch to secondary windows. Drives the view through room_view_ +
    // request_relayout_, so shells that own their RoomView directly (Qt6, GTK4)
    // inherit this. Win32 (message-pump payloads) and macOS (ObjC view) keep
    // their own overrides because they marshal through a platform layer.
    virtual void handle_timeline_reset_ui_(std::string room_id,
                                           EventList snapshot);
    virtual void handle_message_inserted_ui_(std::string room_id,
                                             std::size_t index,
                                             std::unique_ptr<Event> ev);
    virtual void handle_message_updated_ui_(std::string room_id,
                                            std::size_t index,
                                            std::unique_ptr<Event> ev);
    virtual void handle_message_removed_ui_(std::string room_id,
                                            std::size_t index);
    virtual void handle_messages_prepended_ui_(std::string room_id,
                                               EventList events);
    virtual void handle_messages_appended_ui_(std::string room_id,
                                              EventList events);
    virtual void handle_messages_updated_batch_ui_(std::string room_id,
                                                   std::vector<std::size_t> indices,
                                                   EventList events);
    virtual void handle_thread_reset_ui_(std::string room_id,
                                         std::string thread_root,
                                         EventList snapshot);
    virtual void handle_thread_inserted_ui_(std::string room_id,
                                            std::string thread_root,
                                            std::size_t index,
                                            std::unique_ptr<Event> ev);
    virtual void handle_thread_updated_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index,
                                           std::unique_ptr<Event> ev);
    virtual void handle_thread_removed_ui_(std::string room_id,
                                           std::string thread_root,
                                           std::size_t index);
    virtual void handle_thread_messages_prepended_ui_(std::string room_id,
                                                      std::string thread_root,
                                                      EventList events);
    virtual void handle_thread_messages_appended_ui_(std::string room_id,
                                                     std::string thread_root,
                                                     EventList events);
    virtual void handle_threads_updated_ui_(std::string room_id);
    // Completion for an async fetch_media_async download. Looks up the pending
    // request by id (ignoring late callbacks for cancelled/superseded requests)
    // and runs its registered bytes-completion. Concrete shared logic.
    void handle_media_ready_ui_(std::uint64_t request_id,
                                std::vector<std::uint8_t> bytes);
    // Completion for an async get_space_child_summary_async fetch. Applies the
    // result (or failure) to unjoined_summaries_cache_ and notifies the view.
    void handle_space_child_summary_ready_ui_(std::uint64_t request_id,
                                              std::string summary_json);
    // Completion for an async get_server_info_async fetch. Populates
    // server_info_ and drives the same post-processing as the sync path.
    void handle_server_info_async_ready_ui_(std::uint64_t request_id,
                                            std::string info_json);
    // Completion for an async get_url_preview_async fetch.
    void handle_url_preview_ready_ui_(std::uint64_t request_id,
                                      std::string preview_json);
    // Completion for an async gif_search. Each shell forwards to its GIF
    // controller(s); the controller drops superseded request_ids. Default
    // no-op so shells opt in.
    virtual void handle_gif_results_ui_(std::uint64_t /*request_id*/,
                                        std::vector<GifResult> /*results*/)
    {
    }
    virtual void handle_gif_search_failed_ui_(std::uint64_t /*request_id*/,
                                              std::string /*message*/)
    {
    }
    // Forward an async GIF search result/failure to every open pop-out window
    // (in addition to the main window's own handle_gif_results_ui_). request_ids
    // are process-global, so only the pop-out whose GifController issued the
    // search consumes it; the rest drop it. Called by EventHandlerBase on the
    // UI thread.
    void dispatch_gif_to_secondary_windows_(
        std::uint64_t request_id, const std::vector<GifResult>& results);
    void dispatch_gif_failed_to_secondary_windows_(std::uint64_t request_id,
                                                    const std::string& message);

    // Render one GIF strip cell. Each shell overrides with its backend-specific
    // two-stage fetch/decode/animate provider; `repaint` is invoked (on the UI
    // thread) when an async preview/animation lands so the caller's surface
    // refreshes. Shared by the shell's own GIF strip and every pop-out's strip
    // (a pop-out passes a repaint targeting its own popup surface). Default
    // null = no GIF strip on this shell.
    virtual const tk::Image*
    gif_strip_image_(const GifResult& /*result*/,
                     const std::function<void()>& /*repaint*/)
    {
        return nullptr;
    }
    // Source bytes the GIF strip persisted to the media disk cache on fetch,
    // reused by a pop-out's GifController so a selected GIF sends without a
    // second download. Forwards to media_disk_cache_ via gif_src_disk_key_.
    std::vector<std::uint8_t>
    cached_gif_source_bytes_(const std::string& url) const;
    // Completion for paginate_back_async / paginate_forward_async.
    void handle_paginate_result_ui_(std::uint64_t request_id, bool ok,
                                    bool reached_start, bool reached_end,
                                    std::string message);
    void handle_room_action_complete_ui_(std::uint64_t request_id, bool ok,
                                         std::string joined_room_id,
                                         std::string message);
    void handle_upload_complete_ui_(std::uint64_t request_id, bool ok,
                                    std::string message);
    virtual void handle_sync_error_ui_(std::string /*context*/,
                                       std::string /*user_id*/,
                                       std::string /*description*/,
                                       bool /*soft_logout*/)
    {
    }

    // Agnostic sync-error state machine, shared by every shell. Reacts to the
    // SDK sync-error callback's three contexts:
    //   - "sync_reconnect"   (transient): stop the affected account's sync and
    //     schedule a delayed restart via schedule_sync_restart_().
    //   - "sync_auth_error"  + soft_logout: restore the soft-logged-out session
    //     (refresh-token flow), re-fetch display_name / avatar_url onto the
    //     AccountSession, re-bind this window's identity strip when the affected
    //     account is the active one, and restart sync. If the session can't be
    //     restored (or this isn't a soft logout), clear the stored account, stop
    //     sync, and ask the shell to relogin via request_relogin_().
    //   - else: surface `description` in the status bar.
    // Centralizing this fixes prior per-shell drift (notably macOS, which
    // skipped the post-refresh display-name/avatar re-fetch + strip re-bind).
    void handle_sync_error_impl_(std::string context, std::string user_id,
                                 std::string description, bool soft_logout);

    // Restart the named account's sync if it is registered and not already
    // syncing. Called by schedule_sync_restart_'s timer body. Concrete and
    // shared — operates purely on AccountManager + AccountSession.
    void restart_account_sync_(const std::string& user_id);

    // Delayed sync-restart after a transient reconnect error. The timer itself
    // is native; the default implementation routes through post_to_ui_after_
    // (each shell's native one-shot timer) and then restart_account_sync_().
    // A shell may override to use a different native timer, but should not need
    // to. delay_ms is the reconnect backoff (~5s).
    virtual void schedule_sync_restart_(const std::string& user_id,
                                        int delay_ms);

    // Re-bind this window's identity strip (user-info widget) from the active
    // account after its profile was re-fetched on a soft-logout recovery. Each
    // shell repaints its native user strip (Qt/GTK populate_user_strip,
    // macOS/Win equivalents). Default no-op for windows without a strip.
    virtual void refresh_user_strip_()
    {
    }

    // Drive the shell to its login flow after an unrecoverable auth error
    // (session expired / soft-logout recovery failed). Each shell maps this to
    // its existing relogin path (doLogin / do_login / logout_active_account /
    // _logoutActiveAccount).
    virtual void request_relogin_(const std::string& user_id) = 0;

    // Show the offline connectivity banner in the main app widget and schedule
    // a relayout. Called when the sync service signals a network outage.
    virtual void handle_offline_ui_();

    // Hide the offline banner. Called when RoomListState transitions back to
    // Running after having been offline.
    virtual void handle_online_ui_();
    virtual void handle_backup_progress_ui_(BackupProgress /*progress*/)
    {
    }
    // Forwards encryption-setup progress to the overlay. Identical across all
    // shells, so it lives here (concrete) rather than being re-overridden.
    virtual void handle_enable_recovery_progress_ui_(uint8_t  step,
                                                     std::string recovery_key,
                                                     uint32_t backed_up,
                                                     uint32_t total);
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
    // Pass fetch_avatars=false when processing a bulk room/thread snapshot so
    // that sender avatars are fetched lazily (only for the visible rows) instead
    // of for the entire history.
    virtual void prep_row_media_(const Event& ev, bool fetch_avatars = true)
    {
        ensure_row_media_(ev, fetch_avatars);
    }
    // Concrete: only the active account's prefs set the pending restore room.
    virtual void handle_account_prefs_updated_ui_(std::string user_id,
                                                  std::string json);
    // MSC4278: re-read the global media-preview config into the Settings
    // mirror (active account only) and refresh gating + room list.
    virtual void handle_media_preview_config_updated_ui_(std::string user_id,
                                                         std::string json);
    // Callback from media_preview_config_async: parse config_json and apply.
    void handle_media_preview_config_fetched_ui_(std::uint64_t request_id,
                                                 std::string config_json);
    // Callback from room_media_preview_override_async: store override, fetch media.
    void handle_room_preview_override_ready_ui_(std::uint64_t request_id,
                                                std::string override_json);
    // Callback from fetch_room_security_state_async: push the already-typed
    // state into room_view_'s RoomSettingsView via set_security_state, if
    // that view is still open and showing the room this request was for.
    void handle_room_security_state_ready_ui_(std::uint64_t request_id,
                                              tesseract::RoomSecurityState state);
    // Callback from set_or_delete_profile_field_async.
    void handle_profile_field_result_ui_(std::uint64_t request_id,
                                         std::string key, bool ok,
                                         std::string message);
    // Callback from get_extended_profile_async / resolve_user_profile_async.
    // Dispatches to panel (user panel), resolve map (quick-switcher), or own
    // profile based on which pending map contains request_id.
    void handle_extended_profile_ready_ui_(std::uint64_t request_id,
                                           std::string profile_json);
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
    // Concrete: if the waveform is for the displayed room, push it to the
    // message list. Same for every shell, so implemented in the base.
    virtual void handle_voice_waveform_ready_ui_(std::string room_id,
                                                 std::string event_id,
                                                 std::vector<std::uint16_t> waveform);

#ifdef TESSERACT_CALLS_ENABLED
    // ── MatrixRTC call event hooks ────────────────────────────────────────────
    // Called on the UI thread by EventHandlerBase after marshalling.
    // Default ShellBase implementations update call_session_ state.
    // Layer 5 shells override to update IncomingCallBanner / CallOverlay.

    // A remote participant opened a call slot in a room we are in.
    virtual void handle_rtc_invitation_ui_(std::string room_id,
                                           std::string slot_id,
                                           std::string caller_user_id,
                                           std::string call_intent,
                                           std::uint64_t lifetime_ms,
                                           std::string notification_event_id);
    virtual void handle_rtc_participant_joined_ui_(std::uint64_t session_id,
                                                   RtcParticipantInfo info);
    virtual void handle_rtc_participant_left_ui_(std::uint64_t session_id,
                                                 std::string participant_id);
    virtual void handle_rtc_participant_updated_ui_(std::uint64_t session_id,
                                                    RtcParticipantInfo info);
    virtual void handle_rtc_session_ended_ui_(std::uint64_t session_id,
                                              std::string reason);
    // Decoded, pre-multiplied BGRA video frame from a remote participant.
    // bgra was pre-converted from RGBA on the worker thread in EventHandlerBase;
    // the shared_ptr allows zero-copy handoff to ParticipantTile.
    virtual void handle_rtc_video_frame_ui_(
        std::uint64_t session_id,
        const std::string& participant_id,
        std::uint32_t width,
        std::uint32_t height,
        std::shared_ptr<std::vector<std::uint8_t>> bgra);

    // Screen share frame from a remote participant. Routed to the call overlay
    // using participant_id + ":screen" as the tile key.
    virtual void handle_rtc_screen_frame_ui_(
        std::uint64_t session_id,
        const std::string& participant_id,
        std::uint32_t width,
        std::uint32_t height,
        std::shared_ptr<std::vector<std::uint8_t>> bgra);

    // Start/stop local screen capture and the LiveKit screen track.
    void start_screen_share_();
    void stop_screen_share_();

    // Final step of start_screen_share_(): configures and starts the capture
    // object with the chosen source, then wires it to the live session.
    // Called either directly (single source) or from the picker callback.
    void do_start_screen_share_(const std::string& source_id,
                                 std::unique_ptr<tk::ScreenCapture> cap);

    // Thread-safe entry point for incoming PCM audio from the worker thread.
    // AudioPlayback::push_frame() is documented thread-safe; the mutex protects
    // the call_audio_output_ pointer itself from concurrent reset on teardown.
    void push_call_audio_bgnd_(const std::int16_t* samples,
                                std::size_t sample_count,
                                std::uint32_t sample_rate,
                                std::uint32_t num_channels);
    // Factory hook: each concrete shell returns its platform audio output sink.
    // Returns nullptr on platforms where playback is not yet implemented.
    virtual std::unique_ptr<tk::AudioPlayback> make_call_audio_output_() = 0;

    // Factory hook: each concrete shell creates its call pop-out window.
    virtual CallWindowBase* create_call_window_() = 0;

    // Returns the single active CallOverlayWidget regardless of mode
    // (Popout → call_window_, Docked/DockedExpanded/Floating → main_app_).
    views::CallOverlayWidget* active_call_overlay_() const;

    // Tear down the current overlay, switch to the requested mode, remount,
    // rewire all callbacks, and persist the new mode to Settings.
    void on_call_overlay_mode_requested_(views::CallOverlayWidget::Mode m);

    // Persist the new float position to Settings and request relayout.
    void on_call_float_position_changed_(float x, float y);

    // Overlay configuration that must survive mode switches (docked ↔ floating ↔
    // popout). Initialised at call start; each remount reads from this struct
    // rather than re-deriving state from scattered sources.
    using CallOverlayState = views::CallOverlayWidget::OverlayState;

protected:
    std::unique_ptr<CallSession>                call_session_;
    std::unique_ptr<tk::VideoCapture>           call_video_capture_;
    std::unique_ptr<tk::ScreenCapture>          screen_capture_;
    // Background worker that fills in screen-picker tile thumbnails (see
    // start_screen_share_()). Joined before starting a new one and in
    // ~ShellBase() — it captures `this` (for post_to_ui_alive_), so it must
    // not be allowed to outlive the object the way a detached thread could.
    std::thread                                 screen_thumb_worker_;
    // Guarded by call_audio_mutex_: accessed by worker threads via
    // push_call_audio_bgnd_() and reset on the UI thread at call teardown.
    std::mutex                                  call_audio_mutex_;
    std::unique_ptr<tk::AudioPlayback>          call_audio_output_;
    std::unique_ptr<CallWindowBase>             call_window_;
    CallOverlayState                            call_overlay_state_;
    // Tracks the notification_event_id of the current pending ring invitation.
    // Non-empty means a notification-path invite is showing; member-state invites
    // for the same room are suppressed to avoid duplicate banners.
    std::string rtc_pending_notification_id_;
#endif // TESSERACT_CALLS_ENABLED

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

    // Apply all notification-content privacy gates in-place. Call from
    // handle_notification_ui_() before constructing the Notification.
    // Combines:
    //   - image-preview gate (clears image_bytes when disabled / locked)
    //   - hide-content toggle (replaces sender/room/body with generic
    //     strings and clears both avatar_bytes and image_bytes)
    void apply_notification_redaction_(std::string& sender,
                                       std::string& room_name,
                                       std::string& body,
                                       std::vector<uint8_t>& avatar_bytes,
                                       std::vector<uint8_t>& image_bytes) const
    {
        if (!notification_image_allowed_())
        {
            image_bytes.clear();
        }
        if (tesseract::Settings::instance().notification_hide_content)
        {
            sender = "Tesseract";
            room_name.clear();
            body = "New message";
            avatar_bytes.clear();
            image_bytes.clear();
        }
    }
    // Show `msg` in the platform status bar for `auto_clear_ms` milliseconds,
    // then restore the sync-status text. `auto_clear_ms <= 0` → the message
    // persists until the next status change (e.g. an update notification).
    // `allow_links` opts into markdown-style "[label](url)" hyperlink parsing
    // (see app/status_links.h) — pass it ONLY for app-authored text. It defaults
    // to false so server/error-sourced messages (subscribe / sync / sign-out
    // failures whose tail is a homeserver string) can never inject a clickable
    // link. Safe to call from any thread.
    void show_status_message_(std::string msg, int auto_clear_ms = 4000,
                              bool allow_links = false);

    // Segment a status message for the shells' status-bar renderers: links only
    // when the current message opted in (status_message_allows_links_), else a
    // single plain segment. Shells call this instead of parse_status_links so
    // the opt-in gate lives in one place.
    std::vector<tesseract::StatusSegment>
    parse_status_message_(const std::string& msg) const;
    // Set on the UI thread by show_status_message_ immediately before
    // on_show_status_message_ui_ runs (its only caller), so reading it there is
    // race-free.
    bool status_message_allows_links_ = false;

    // Called by show_status_message_ on the UI thread to display the message.
    virtual void on_show_status_message_ui_(const std::string& /*msg*/) {}
    // Called when the auto-clear timer fires to restore normal sync status.
    virtual void on_restore_status_ui_() {}
    // True while a persistent status override is showing; shells check this
    // in refresh_sync_status() to avoid overwriting it with "Connected".
    bool has_status_override_() const { return status_override_active_; }

    // Called after push_room_list_state_() — shell refreshes its sync-status display.
    virtual void on_room_list_state_ui_()
    {
    }

    // Trigger a one-shot background update check once sync first reaches the
    // Running state. Guarded internally so repeated calls are safe.
    void trigger_update_check_();
    // Called whenever last_inflight_ or last_room_list_state_ changes so the
    // status-bar dot can be repainted with the new combined request count.
    virtual void on_inflight_ui_()
    {
    }

    // True when the inflight ring animation should be running (count >= 2).
    bool inflight_needs_anim_() const { return inflight_total_() >= 2u; }

    // Current rotation phase in [0,1) for the inflight ring.
    float inflight_spin_phase_() const { return spin_accum_phase_; }

    // Advance the ring phase accumulator. Call on every animation tick and
    // whenever the inflight count changes so the phase stays continuous.
    void spin_tick_(std::int64_t now_ms)
    {
        if (spin_last_tick_ms_ > 0 && inflight_needs_anim_())
        {
            const float dt = static_cast<float>(now_ms - spin_last_tick_ms_);
            spin_accum_phase_ =
                std::fmod(spin_accum_phase_ +
                              dt * tk::inflight_revs_per_ms(inflight_total_()),
                          1.0f);
        }
        spin_last_tick_ms_ = now_ms;
    }

    // Combined in-flight count: extra requests + 1 for the sync long-poll
    // when it is running. Used by both the color helper and tooltip text.
    std::uint32_t inflight_total_() const
    {
        const bool sync_on =
            last_room_list_state_ == RoomListState::Running ||
            last_room_list_state_ == RoomListState::Recovering;
        return last_inflight_ + (sync_on ? 1u : 0u);
    }

    // Returns the color for the in-flight dot. Green ≤1 (sync only), amber
    // 2–10 (extra activity), red >10 (heavy load).
    tk::Color inflight_dot_color_() const
    {
        const std::uint32_t total = inflight_total_();
        if (total <= 1u) return tk::Color::rgb(0x40BF4D); // green
        if (total <= 10u) return tk::Color::rgb(0xF2B21A); // amber
        return tk::Color::rgb(0xE03838);                   // red
    }

    // Queue depth helpers for the in-flight tooltip.
    size_t pool_pending_count_()     const { return pool_.pending_count(); }
    size_t mut_pool_pending_count_() const { return mut_pool_.pending_count(); }
    size_t pending_media_count_()    const { return pending_media_.size(); }

    // Wire both worker pools to re-fire on_inflight_ui_() on every queue-depth
    // change. Call once from each shell's setup, after the UI is constructed.
    void init_pool_callbacks_();

    /// Called on the UI thread after `server_info_` has been populated.
    /// Override in shells that gate UI elements on server capabilities.
    virtual void on_server_info_ready_ui_() {}

    /// Called on the UI thread after `own_extended_profile_` has been
    /// refreshed. Override to push the profile into the platform settings
    /// widget (e.g. `settings_widget_->set_extended_profile(...)`).
    virtual void on_own_extended_profile_ready_ui_() {}

    /// Called on the UI thread after a set_profile_field / delete_profile_field
    /// call completes. Override to clear the busy state and surface errors.
    virtual void on_profile_field_result_ui_(const std::string& /*key*/,
                                              bool /*ok*/,
                                              const std::string& /*error*/) {}

    // Called on the UI thread after space_children_cache_ has been refreshed.
    // Each shell overrides to call refresh_room_list_() or its wrapper.
    virtual void on_space_children_cache_ready_ui_() {}

    // Called on the UI thread after unjoined summaries for space_id arrive.
    // Each shell overrides to call refresh_room_list_() or its wrapper.
    virtual void on_space_unjoined_summaries_ready_ui_(
        const std::string& /*space_id*/) {}

    // Called after handle_room_action_complete_ui_() processes a Join action.
    // ok=true means the join succeeded; room_id is the canonical joined room ID.
    virtual void on_join_room_outcome_ui_(bool /*ok*/,
                                          const std::string& /*room_id*/) {}

    // Fetch space children for every space in rooms_ on a worker thread and
    // post the result into space_children_cache_, then fire
    // on_space_children_cache_ready_ui_(). Idempotent w.r.t. rooms_ contents.
    void update_space_children_cache_();

    // Fetch MSC3266 summaries for unjoined children of space_id via the
    // worker pool. Stores in unjoined_summaries_cache_ and fires
    // on_space_unjoined_summaries_ready_ui_().
    void fetch_single_room_summary_(const std::string& space_id,
                                    const std::string& room_id);

    // Cancel all in-flight unjoined-room summary fetches and reset per-space
    // fetch state. Call whenever the user exits a space.
    void cancel_unjoined_summaries_();

    // ── Extended profile (MSC4133) helpers ────────────────────────────────────

    /// Fetch the signed-in user's extended profile on a worker thread.
    /// Stores the result in own_extended_profile_ and fires
    /// on_own_extended_profile_ready_ui_() on the UI thread.
    void fetch_own_extended_profile_async_();

    /// Write (or delete when value_json == "null") a single profile field on
    /// a worker thread. Fires on_profile_field_result_ui_() on the UI thread.
    void handle_profile_field_change_(const std::string& key,
                                       const std::string& value_json);

    /// Fetch another user's extended profile on a worker thread and push the
    /// result into panel via set_extended_profile(). panel must outlive the
    /// async call (guaranteed because it is owned by the main widget tree).
    void fetch_user_extended_profile_async_(const std::string& user_id,
                                             views::UserProfilePanel* panel);

    // Returns cached summaries if present; triggers a fetch and returns {}
    // otherwise. Call from refresh_room_list() while drilled into a space.
    const std::vector<tesseract::RoomSummary>&
    get_cached_unjoined_summaries_(const std::string& space_id);

    // For each space row in `rooms`, replace its notification_count and
    // highlight_count with the aggregate of its children's counts (from
    // space_children_cache_ and rooms_). Non-space rows are left unchanged.
    void apply_space_child_counts_(std::vector<RoomInfo>& rooms) const;

    /// Reset server-info state on logout / account-switch. Call this instead of
    /// touching server_info_ and server_info_fetch_started_ directly from shells.
    void reset_server_info_()
    {
        server_info_fetch_started_ = false;
        server_info_ = tesseract::ServerInfo{};
        own_extended_profile_ = {};
    }

    /// Fire an async server-info fetch. Result arrives via
    /// handle_server_info_async_ready_ui_(). Only fetches once per session.
    void begin_server_info_fetch_()
    {
        if (server_info_fetch_started_ || !client_)
            return;
        server_info_fetch_started_ = true;
        client_->get_server_info_async(next_request_id_++);
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
    void dismiss_encryption_setup_after_verification_();
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

    // ── Presence (receive-side) ───────────────────────────────────────────────
    // Maps bare Matrix user ID → last-received PresenceState.
    // Keyed only on IDs we've ever received a presence event for.
    std::unordered_map<std::string, PresenceState> user_presence_;

    // Called on the UI thread by EventHandlerBase. Updates user_presence_ and
    // triggers a room-list repaint when the changed user is a DM counterpart.
    void handle_presence_changed_ui_(const std::string& user_id,
                                     PresenceState state);

    // Look up the presence state for user_id. Returns Offline when unknown.
    PresenceState presence_for_(const std::string& user_id) const;

    // ── Presence (send-side) ──────────────────────────────────────────────────
    // Owned PresenceTracker. Constructed lazily on first
    // `RoomListState::Running` so we only start publishing presence once we're
    // actually synced. Shells feed it activity, focus, periodic ticks, and
    // logout via the public notify_* helpers below.
    std::unique_ptr<PresenceTracker> presence_tracker_;

    // Owned update checker. Created and triggered once when sync first reaches
    // RoomListState::Running; destroyed (and not recreated) on logout.
    std::unique_ptr<IUpdateChecker> update_checker_;
    bool update_check_triggered_ = false;

    // Called by the shell when the user does something in our window —
    // either a Host input event (wired through Host::set_on_user_activity)
    // or any other user-initiated action.
    void notify_user_activity_();

    // Called by the shell on window focus gained/lost.
    void notify_window_active_(bool active);

    // Called periodically (~every 30 s) by the shell. No-op when no tracker.
    void notify_presence_tick_();

    // Called from the shell's logout path before stop_sync(). Synchronously
    // pushes Offline to the homeserver (best-effort, bounded by a short
    // worker-thread timeout) so contacts see us go offline immediately.
    void notify_presence_logout_();

    // Construct presence_tracker_ and fire its initial sync_started transition.
    // Idempotent. Called lazily from notify_user_activity_() once the sliding-
    // sync handshake has settled (RoomListState::Running).
    void start_presence_tracking_();

    // Called from each shell when the "Send and receive presence status" toggle
    // changes. Persists the setting, enables/disables the Rust polling loop,
    // and starts or stops the PresenceTracker accordingly.
    void handle_send_presence_toggle_(bool enabled);

    // Toggle handler for the "Index messages for search" Privacy setting.
    // Persists the setting and calls Client::set_search_indexing_enabled() on
    // every logged-in account (enable → lazy backfill; disable → clear index).
    void handle_index_messages_toggle_(bool enabled);

    // Toggle handler for the "Show room join/leave events" Appearance
    // setting. Persists the setting, applies it to the active account's
    // client, and re-subscribes the currently-open room (when any) so the
    // change is reflected immediately instead of waiting for the next room
    // switch.
    void handle_show_membership_events_toggle_(bool enabled);

#ifdef TESSERACT_GITHUB_REPO
    // Persists the "check for updates automatically" preference.
    void handle_check_for_updates_toggle_(bool enabled);
#endif

    // Resume live search indexing for a freshly-synced account if the global
    // "index messages for search" preference is enabled. Called after start_sync.
    void apply_search_indexing_pref_(tesseract::AccountSession& session);

    // Apply the persisted "show room join/leave events" preference to a
    // freshly-synced account's Rust client. Called after start_sync so the
    // very first room subscription already reflects the setting instead of
    // defaulting to the Rust-side AtomicBool's off default. Non-blocking.
    void apply_membership_events_pref_(tesseract::AccountSession& session);

    // ── Search-index stats (Settings panel) ───────────────────────────────
    // Each shell points `settings_view_` at its shared SettingsView once, and
    // calls start_/stop_ when its Settings panel opens/closes. The refresh
    // fetches stats from the active account's client and pushes them to the
    // view, re-arming a slow poll while the history backfill runs.
    void start_search_index_stats_poll_();
    void stop_search_index_stats_poll_();
    void refresh_search_index_stats_();
    // Borrowed pointer to the shell's shared SettingsView (named distinctly so
    // it never shadows a shell's own `settings_view_` member). Each shell sets
    // it once.
    tesseract::views::SettingsView* stats_settings_view_ = nullptr;
    bool search_stats_panel_open_ = false;
    /// On-disk index size computed once via `dbstat` when the Settings panel
    /// opens; injected into `SearchIndexStats` before passing to the UI so the
    /// expensive B-tree walk is not repeated on every 2-second poll tick.
    std::uint64_t cached_index_bytes_ = 0;

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

    // If `room_id` is open in a secondary (pop-out) window, raise it and return
    // true so the caller skips opening/selecting the room in the main app.
    bool focus_secondary_window_(const std::string& room_id);
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

    // Notify every open pop-out window that media for cache_key has arrived, so
    // it can invalidate row heights and repaint. Mirrors on_url_preview_ready_'s
    // secondary-window fan-out. cache_key is not room-scoped (the same mxc can
    // appear in several rooms), so notify all secondary windows.
    void notify_secondary_media_ready_(const std::string& cache_key,
                                       MediaKind kind);

    // Override in a platform shell to instantiate the concrete RoomWindow
    // subclass. The subclass constructor must call finish_init_() before
    // returning. Default: no-op (secondary windows are inert until a shell
    // overrides this).
    virtual RoomWindowBase*
    create_secondary_room_window_(const std::string& /*room_id*/)
    {
        return nullptr;
    }

    // Returns the available work areas for all connected screens (usable area
    // excluding dock/taskbar) in a top-left-origin coordinate space.
    // Used by clamp_to_screens_() to validate saved window geometry.
    // Platform shells override this; the default returns an empty list, which
    // causes clamp_to_screens_() to fall back to platform-native centering.
    virtual std::vector<tk::Rect> get_screen_work_areas_() const
    {
        return {};
    }

    // Validate saved geometry against available screens. If the title-bar area
    // ({saved.x, saved.y, saved.w, 50}) doesn't intersect any screen work area,
    // the window is re-centred on screens[0] with the saved (or default) size
    // clamped to 90% of the screen. Returns a geometry with valid=false when
    // saved.valid is false (caller uses its own platform default).
    static Settings::WindowGeometry clamp_to_screens_(
        const Settings::WindowGeometry& saved,
        int default_w, int default_h,
        const std::vector<tk::Rect>& screens);

    // Debounce a Settings::save_to_disk() call by 500 ms so rapid window
    // resize/move events don't flood the file with tiny writes.
    void save_settings_debounced_();

    // ── Concrete helpers ──────────────────────────────────────────────────────

    // Enqueue fn() on the shared-read pool (pool_, 2 threads).
    // Use for &self FFI calls and CPU/disk work that holds no ffi_mu lock.
    void run_async_(std::function<void()> fn);

    // Enqueue fn() on the single-thread mutable pool (mut_pool_, 1 thread).
    // Use for every &mut ClientFfi call (anything that takes MUT_FFI in
    // client.cpp). The 1-thread pool serialises them naturally so ffi_mu
    // is never waited on by more than one thread at a time.
    void run_async_mut_(std::function<void()> fn);

    // Avatar / media prefetch — each method is idempotent (dedup via the
    // media_fetches_in_flight_ set + cache-presence check).
    void ensure_room_avatar_(const RoomInfo& r);
    // group_id = 0 (default) for account-wide callers (quick switcher roster,
    // invites) whose avatars aren't tied to the active room. Timeline-row
    // callers pass the row's room group so leaving the room cancels them.
    void ensure_user_avatar_(const std::string& mxc, std::uint64_t group_id = 0);

    // Non-blocking voice/audio byte provider for the playback path. Returns the
    // clip's bytes if already warmed (moving them out of voice_bytes_cache_),
    // otherwise kicks a one-shot async download (fetch_media_async) and returns
    // empty; `on_ready` fires on the UI thread when the download lands so the
    // caller can repaint and the user can replay. Replaces the blocking
    // fetch_source_bytes that previously froze the UI on an uncached clip.
    std::vector<std::uint8_t>
    voice_bytes_or_fetch_(const std::string& token,
                          std::function<void()> on_ready);
    // `group_id` is the cancellation group for the download (room switch drops
    // a room's pending media). Defaults to 0 (never cancelled) for non-timeline
    // callers (avatar/preview prefetch); timeline callers pass the room group.
    void ensure_media_image_(const std::string& url, int max_w, int max_h,
                             std::uint64_t group_id = 0,
                             MediaKind kind = MediaKind::MediaImage);

    // Fetch + decode the full-resolution image for the lightbox viewer into
    // viewer_fullres_ (keyed by the plain source token / avatar mxc), then
    // relayout the main surface and every pop-out. Guards on empty / already
    // cached / animated (animated falls back to ensure_media_image_ so the GIF
    // keeps animating from anim_cache_) / known-decode-failed / in-flight — the
    // latter three keyed by fullres_key_(). Uses a DISTINCT disk + in-flight key
    // namespace (fullres_key_) from the inline ensure_media_image_ path so the
    // 320px inline entry can never pre-empt the full-res decode. group 0 so a
    // room switch does not cancel an open lightbox load.
    void ensure_viewer_fullres_(const std::string& url);

    // Worker-thread decode of full-res viewer bytes at kViewerFullresMax, then
    // UI-thread store into viewer_fullres_ (FIFO-evicting) + relayout main
    // surface + every pop-out. `persist` writes the bytes to the namespaced
    // ("fullres:") disk cache (network path) before decoding.
    void decode_fullres_and_store_(std::string url, std::string fkey,
                                   std::vector<std::uint8_t> bytes, bool persist);

    // Shared image-viewer provider: full-res first, then the existing
    // anim → image → thumbnail fallthrough. Used by both the main-window
    // viewers (wire_main_app_viewers_) and the pop-out viewers
    // (RoomWindowBase::shell_image_).
    const tk::Image* viewer_image_lookup_(const std::string& mxc);

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
    void fetch_media_pipeline_(std::string cache_key, std::string disk_key,
                               std::string inflight_key, std::uint64_t group_id,
                               tesseract::Client::MediaReqKind kind,
                               std::string source, std::uint32_t w,
                               std::uint32_t h, bool animated,
                               MediaKind out_kind);

    // Fetch a server-scaled thumbnail (w×h) for an inline media preview into
    // thumbnail_cache_ (or anim_cache_ if it decodes animated). Mirrors
    // ensure_media_image_ but uses the /thumbnail endpoint; the in-flight and
    // media_disk_cache_ keys are size-namespaced via thumb_key() so a thumbnail
    // and a full-size fetch of the same mxc never collide. `animated` requests
    // an animated thumbnail where the server supports it (MSC2705).
    void ensure_media_thumbnail_(const std::string& url, int w, int h,
                                 bool animated, std::uint64_t group_id = 0);

    // Size-namespaced cache key for thumbnail fetches (disk + in-flight set).
    static std::string thumb_key(const std::string& key, int w, int h)
    {
        return "t" + std::to_string(w) + "x" + std::to_string(h) + ":" + key;
    }

    // Disk-cache + in-flight key for the full-resolution viewer fetch.
    // Namespaced so it never collides with the inline ensure_media_image_
    // entry (plain url) on media_disk_cache_ or media_fetches_in_flight_.
    static std::string fullres_key_(const std::string& url)
    {
        return "fullres:" + url;
    }

    // Disk-cache key for a GIF strip's source bytes (the original MP4/WebP/GIF
    // uploaded when the user picks it). Namespaced so a Klipy CDN URL never
    // collides with an mxc:// media key in the shared media_disk_cache_.
    static std::string gif_src_disk_key_(const std::string& url)
    {
        return "gifsrc:" + url;
    }

    // Prefetch avatars for every entry in invites_ (inviter avatar for DMs,
    // room avatar for group invites) so the room-list invite rows render them.
    void ensure_invite_avatars_();

    // Sticker / animated-media lookup: anim_cache_ → tk_images_ fallback.
    // shell_sticker_ kicks an ensure_media_image_(mxc, 64, 64) fetch on miss
    // (used by RoomListView's sticker_provider, where the row hasn't yet
    // pre-warmed the cache).
    const tk::Image* shell_sticker_(const std::string& mxc);

    // Provider-lambda factories. Each shell wires these onto its native
    // pickers / dialogs / popups instead of re-spelling the identical lookup
    // bodies. All capture `this`; safe for the shell's lifetime.

    // Avatar lookup: tk_avatars_ only (used by account picker, join-room
    // dialog, mention popup, settings view).
    std::function<const tk::Image*(const std::string&)>
    make_avatar_image_provider_()
    {
        return [this](const std::string& mxc) -> const tk::Image*
        { return account_manager_.thumbnail_cache().peek(mxc); };
    }

    // Static-image lookup: image_cache_ only (used by the shortcode popup).
    std::function<const tk::Image*(const std::string&)>
    make_static_image_provider_()
    {
        return [this](const std::string& url) -> const tk::Image*
        { return account_manager_.image_cache().peek(url); };
    }

    // Animated-frame → static-image lookup + fetch-on-miss: like
    // make_static_image_provider_ above, but also kicks off ensure_media_image_
    // as a side effect when the cache misses, and — like
    // make_picker_image_provider_ — checks anim_cache_ first so an animated
    // WebP/GIF resolves to its current frame instead of never rendering (the
    // fetch/decode pipeline behind ensure_media_image_ already detects and
    // decodes multi-frame content into anim_cache_ regardless of caller; this
    // just needs to read it). Used by NativeTextArea::set_image_resolver (the
    // Windows BetterText compose box's inline custom-emoji rendering), which
    // — unlike the shortcode popup — has no separate "prefetch the visible
    // suggestions" step to rely on before the image is actually needed, and by
    // ImagePackEditorView's pack-tile provider (RoomSettingsView's Emojis &
    // Stickers tab).
    std::function<const tk::Image*(const std::string&)>
    make_static_image_provider_with_fetch_(int max_w, int max_h)
    {
        return [this, max_w, max_h](const std::string& url) -> const tk::Image*
        {
            if (const auto* f = account_manager_.anim_cache().current_frame(url))
            {
                start_anim_tick_();
                return f;
            }
            if (const auto* img = account_manager_.image_cache().peek(url))
            {
                return img;
            }
            ensure_media_image_(url, max_w, max_h);
            return nullptr;
        };
    }

    // Emoji / sticker picker lookup: animated frame → static → kick an async
    // fetch on miss. The (cache_key, source_token) signature matches the
    // shared EmojiPicker/StickerPicker ImageProvider alias.
    std::function<const tk::Image*(const std::string&, const std::string&)>
    make_picker_image_provider_(bool is_sticker)
    {
        return [this, is_sticker](const std::string& cache_key,
                                  const std::string&) -> const tk::Image*
        {
            if (const auto* f = account_manager_.anim_cache().current_frame(cache_key))
            {
                start_anim_tick_();
                return f;
            }
            if (const auto* img = account_manager_.image_cache().peek(cache_key))
            {
                return img;
            }
            ensure_picker_image_(cache_key, is_sticker);
            return nullptr;
        };
    }

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
    // sticker_fetches_in_flight_ (true). io pool reads media_disk_cache_; on a
    // miss the network download runs as a non-blocking fetch_media_async (bulk
    // lane, group 0) so it never pins a pool thread. The decode runs on the io
    // pool via decode_and_finalize_picker_ → finalize_picker_image_ (UI).
    void ensure_picker_image_(const std::string& url, bool is_sticker);

    // Decode `bytes` for a picker image OFF the UI thread, optionally persisting
    // them to the disk cache first (`persist` — true for a fresh network fetch,
    // false for a disk-cache hit), then post finalize_picker_image_ on the UI
    // thread. Shared by the disk-hit and network-completion branches of
    // ensure_picker_image_.
    void decode_and_finalize_picker_(std::string url, bool is_sticker,
                                     std::vector<std::uint8_t> bytes,
                                     bool persist);

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
    // Pass fetch_avatars=false in bulk-load paths to suppress avatar prefetch.
    void ensure_row_media_(const Event& ev, bool fetch_avatars = true);
    // Overload for rows that have already been converted to MessageRowData
    // (used by the lazy visible-range callback for off-screen events).
    void ensure_row_media_(const views::MessageRowData& row,
                           bool fetch_avatars = true);

    // The timeline's visible rows changed (scroll / room enter / data update):
    // raise the priority of the still-pending media fetches backing the now-
    // visible rows so they download ahead of the off-screen backlog. `keys` are
    // the visible rows' media fetch tokens (what the view's image_provider looks
    // up), as reported by MessageListView::on_visible_range_changed. Keys with
    // no in-flight fetch (already cached, or never requested) are skipped.
    void on_visible_rows_changed_(const std::vector<std::string>& keys);

    // Map visible media tokens → the request_ids still fetching them, dropping
    // keys with no live request. Split out of on_visible_rows_changed_ so the
    // key→request resolution is unit-testable without a live Client.
    std::vector<std::uint64_t>
    resolve_visible_request_ids_(const std::vector<std::string>& keys) const;

    // ── MSC4278 media-preview gating helpers ──────────────────────────────────
    // True when media in `room_id` should auto-load given the global + per-room
    // config. Resolves Mode::Private against the cached room join_rule (an
    // unknown / public join rule suppresses previews in Private mode).
    bool should_auto_preview_(const std::string& room_id) const;
    // True when a media item in `room_id` should auto-load, given its sender.
    // The user's own media (is_own) is exempt from public-room suppression in
    // Private mode (but not Off — "off means off"); see media_preview_policy.h.
    bool media_allowed_(const std::string& room_id, bool is_own) const;
    // Resolve the effective preview mode for `room_id` (global config overlaid
    // with the per-room override) and report its cached join_rule.
    tesseract::Settings::MediaPreviews
    effective_preview_mode_(const std::string& room_id,
                            std::string& join_rule_out) const;
    // True when `event_id` in `room_id` should be rendered as a click-to-load
    // placeholder (preview suppressed AND not individually revealed). `is_own`
    // carries the exemption for the user's own media.
    bool media_preview_hidden_(const std::string& room_id,
                               const std::string& event_id, bool is_own) const;
    // Ensure the per-room override + join_rule for `room_id` is cached; kicks an
    // async fetch on a cache miss. Call on room switch.
    void ensure_room_preview_override_(const std::string& room_id);
    // Kick the media fetch for one revealed row (mirrors ensure_row_media_'s
    // image/sticker/video branch). Called from the message list's reveal click.
    void reveal_media_fetch_(const views::MessageRowData& row);
    // Wire the message list's MSC4278 hidden-media predicate + reveal callback.
    // Each shell calls this once after creating room_view_ (and pop-out windows
    // call it on their own list).
    void wire_media_preview_gating_(views::MessageListView* ml);
    // Apply a settings-UI change to the MSC4278 config: update the Settings
    // mirror, write it back to account-data, fetch newly-allowed media, and
    // repaint. Each shell wires SettingsView::on_media_previews_changed /
    // on_invite_avatars_changed to this.
    void apply_media_preview_config_(tesseract::Settings::MediaPreviews mode,
                                     bool invite_avatars);
    // Called once, on the UI thread, after a successful Accept commit whose
    // RoomSettingsChanges.media_override was populated (see
    // apply_room_settings_, which performs the actual server write on the
    // worker thread). Optimistically updates room_preview_overrides_ (so
    // effective_preview_mode_ reflects the new value immediately), re-fetches
    // any media that just became allowed in the open room, and repaints.
    // Each of the five on_accept completion callbacks calls this — never
    // called on every combo pick (that would violate the "nothing applies
    // until Accept" contract every other room-settings field follows).
    void commit_room_media_preview_override_(
        const std::string& room_id, bool has_override,
        tesseract::MediaPreviewConfig::Mode mode);
    // Push the effective per-room override (from room_preview_overrides_,
    // defaulting to "no override" on a cache miss) into RoomSettingsView's
    // Media tab, if that view is currently open and showing `room_id`. Called
    // right after RoomSettingsView::open() (see each shell's
    // on_room_settings_opened wiring) and again from
    // handle_room_preview_override_ready_ui_, so a fetch that resolves after
    // the dialog is already open still updates the combo instead of leaving
    // it stuck on open()'s "Use global default" placeholder.
    void seed_room_media_section_(const std::string& room_id);

    // Kick an async GET /state fetch (Client::fetch_room_security_state_
    // async) for the four Security & Privacy tab fields and track its
    // request_id in pending_security_state_requests_. No-op if not logged
    // in. Called from each on_room_settings_opened handler, right after
    // set_security_field_permissions/seed_room_media_section_ — the result
    // lands in handle_room_security_state_ready_ui_, which pushes it into
    // RoomSettingsView via set_security_state if the dialog is still open.
    void fetch_room_security_state_(const std::string& room_id);

    // ── Emojis & Stickers tab (ImagePackEditorView), initial-testing
    // placement — see RoomSettingsView::set_image_pack_*. This view has no
    // Client dependency, so ShellBase fetches and pushes data in, mirroring
    // seed_room_media_section_'s shape. list_image_packs()/list_pack_images()
    // are cached local reads (no network round-trip), so unlike
    // fetch_room_security_state_ these are synchronous — no request_id
    // bookkeeping needed. Called from each shell's on_room_settings_opened
    // handler, right after fetch_room_security_state_. `target` is whichever
    // RoomSettingsView instance is asking — room_view_->room_settings_view()
    // for a normal room, or main_app_->space_root()->settings_view() for a
    // space root; image packs are ordinary room state, so a space's own
    // packs are seeded the same way.
    void seed_image_pack_tab_(const std::string& room_id,
                             views::RoomSettingsView* target);
    // Wired (alongside on_accept) to each RoomSettingsView instance's
    // on_image_pack_images_needed — fired once per pack (every pack is
    // shown at once now, not just a single "selected" one) — pushes that
    // pack's images into `target`.
    void handle_image_pack_images_needed_(const std::string& pack_id,
                                          views::RoomSettingsView* target);
    // Wired to each RoomSettingsView instance's on_image_pack_pending_image_added —
    // decodes a dropped/pasted image off-thread (decode_image_ is safe to
    // call from a worker) and pushes the local preview back into `target`
    // once ready.
    void handle_image_pack_pending_image_added_(std::uint64_t local_id,
                                                std::vector<uint8_t> bytes,
                                                std::string mime,
                                                views::RoomSettingsView* target);
    // Same decode-off-thread-then-post-back shape as
    // handle_image_pack_pending_image_added_ above, targeting the global
    // Settings "Emojis & Stickers" tab's personal-pack editor instead of a
    // per-room tab.
    void handle_user_pack_pending_image_added_(std::uint64_t local_id,
                                               std::vector<uint8_t> bytes,
                                               std::string mime,
                                               views::UserPackEditor* target);

    // Estimate how many trailing rows of a freshly-loaded snapshot could
    // plausibly be on screen, for build_rows_()'s synchronous media-prefetch
    // window. Real per-row heights (text wrap, inline images) aren't known
    // until the new rows are laid out, so this uses the message list's
    // current (stable, content-independent) viewport height divided by a
    // deliberately small per-row estimate — biased to overestimate rather
    // than under-fetch. Any row this window misses still gets its media via
    // on_visible_rows_changed_ once the real layout runs.
    std::size_t media_prefetch_window_() const;

    // Build MessageRowData rows from an event snapshot: prep media, request
    // reply details, make_row_data. Used by every shell's timeline-reset and
    // message handlers (primary + secondary-window paths).
    std::vector<views::MessageRowData>
    build_rows_(const EventList& snapshot);
    // macOS hands primary-path events across the ObjC boundary as raw
    // pointers; this overload serves that path.
    std::vector<views::MessageRowData>
    build_rows_(const std::vector<Event*>& snapshot);

    // Secondary-window fan-out (primary-window mutation stays per-shell).
    void dispatch_timeline_reset_secondary_(
        const std::string& room_id,
        const EventList& snapshot);
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

    // Update the invites cache and call on_invites_updated_() for the active account.
    void push_invites_(std::string user_id, std::vector<InviteInfo> invites);

    // Return a pointer to the InviteInfo for room_id, or nullptr when not found.
    const InviteInfo* find_invite_(const std::string& room_id) const;

    // Accept / decline / block a pending invitation asynchronously.
    // Each dispatches the SDK call on a worker thread and posts the result
    // back to the UI thread. accept navigates to the room on success;
    // decline and block remove the invite from the local list immediately.
    void accept_invite_async_(const std::string& room_id);
    void decline_invite_async_(const std::string& room_id);
    void block_invite_async_(const std::string& room_id,
                             const std::string& inviter_id);

    // Slash-command async handlers — called from dispatch_room_send_ after the
    // command prefix is identified. Each enqueues async SDK work, so they must
    // run on the UI thread.
    void leave_room_command_(const std::string& room_id);
    void join_room_command_(const std::string& room_id_or_alias);
    void invite_user_command_(const std::string& room_id,
                              const std::string& user_id);

    // Result of dispatch_room_send_. When `handled_as_command` is true a slash
    // command consumed the input (and `send_result` is unset/default); the
    // caller should clear its composer unconditionally. When false the input
    // was a normal send and `send_result` carries the dispatch_compose_send
    // outcome so the caller can clear on success or surface the error.
    struct RoomSendOutcome
    {
        bool handled_as_command = false;
        tesseract::Result send_result;
    };

    // Unified slash-command dispatch ladder shared by every composer send path
    // (the four shells' on_send handlers and RoomWindowBase::send_message_).
    // Recognizes the no-arg /myroomavatar (native file picker via
    // pick_and_set_room_avatar_), /leave, /join <room>, /invite <user>; any
    // other input falls through to dispatch_compose_send (which itself handles
    // /me, /shrug, /myroomnick, /myroomavatar <uri>, /spoiler and normal text).
    // Must be called on the UI thread; the command branches enqueue async work
    // via the existing ShellBase helpers.
    RoomSendOutcome dispatch_room_send_(const std::string& room_id,
                                        const std::string& body,
                                        const std::string& formatted_body);

    // Recompute the aggregate from per_account_rooms_ and fire
    // on_tray_unread_changed_ / on_dock_badge_changed_ only when the values
    // differ from the last call.  Called from push_rooms_ and mark_room_read_.
    void notify_tray_unread_();

    // Sum notification_count across every room in every signed-in account.
    uint64_t compute_dock_notification_count_() const;

    // find_existing_dm() against the active account's cached rooms_.
    std::string find_existing_dm_(const std::string& user_id) const;

    // Async: compute cache directory sizes on a worker thread, then invoke
    // callback(local, sdk, memory, mem_hits, mem_misses, disk_hits,
    // disk_misses) on the UI thread. No-op when not signed in.
    void compute_cache_sizes_(
        std::function<void(uint64_t local, uint64_t sdk, uint64_t memory,
                           uint64_t mem_hits, uint64_t mem_misses,
                           uint64_t disk_hits, uint64_t disk_misses)>
            callback);

    // Async: delete all on-disk caches best-effort (media files, waveform DB,
    // SDK event store), clear in-memory image maps, reinit the waveform store,
    // restart the SDK so it opens fresh SQLite stores, then call
    // recompute_callback with fresh sizes. No-op when not signed in.
    void clear_all_caches_(
        std::function<void(uint64_t local, uint64_t sdk, uint64_t memory,
                           uint64_t mem_hits, uint64_t mem_misses,
                           uint64_t disk_hits, uint64_t disk_misses)>
            recompute_callback);

    // Stop sync, rebuild the matrix-sdk Client against the on-disk session JSON
    // (which reopens fresh SQLite stores), then restart sync. Must be called on
    // the UI thread. No-op when not signed in or session JSON is missing.
    void restart_sdk_();

public:
    // Pure function: returns {has_unread, has_highlight} computed across every
    // account's room list. has_unread is true iff some room has
    // notification_count > 0; has_highlight is true iff some room has
    // highlight_count > 0. Exposed as a public static so the unit test can
    // exercise it without standing up a real shell.
    static std::pair<bool, bool> compute_tray_unread(
        const std::unordered_map<std::string, std::vector<RoomInfo>>& by_account);

    // Pure function: returns the room id of an existing 1:1 DM with `user_id`
    // (the first room marked direct whose counterpart matches), or empty when
    // none is found. Lets a shell switch to an already-open DM synchronously
    // instead of waiting on the async get_or_create_dm round-trip. Exposed as a
    // public static so the lookup can be unit-tested without a live shell.
    static std::string find_existing_dm(const std::vector<RoomInfo>& rooms,
                                        const std::string&           user_id);

protected:

    // Fire-and-forget: write per-room notification mode push rules to the server.
    void set_room_notification_mode_(const std::string& room_id,
                                      const std::string& mode);

    // Fire-and-forget: toggle the room's m.favourite / m.lowpriority tag.
    // The two are mutually exclusive server-side.
    void set_room_favourite_(const std::string& room_id, bool value);
    void set_room_low_priority_(const std::string& room_id, bool value);

    // Mark pagination as complete for room_id.
    void push_paginate_result_(std::string room_id, bool reached_start);

    // Scroll the room message list to event_id, paginating backwards until
    // found. Stores a deferred scroll in the MessageListView so arrange()
    // applies it after row_offsets_ are rebuilt each pass.
    void try_scroll_to_room_event_(const std::string& event_id);

    // ── Message search (Ctrl+Shift+F global overlay) ──────────────────────
    // Issue a global FTS query on the active account (allocates a request_id,
    // tracked in search_pending_queries_). Results land in handle_search_*_ui_.
    void handle_search_query_(const std::string& query);
    void handle_search_results_ui_(std::uint64_t request_id,
                                   std::vector<tesseract::SearchHit> results);
    void handle_search_failed_ui_(std::uint64_t request_id,
                                  const std::string& message);
    // Open the result's room and scroll/highlight the matching event.
    void handle_forward_done_ui_(std::uint64_t request_id);
    void handle_forward_failed_ui_(std::uint64_t      request_id,
                                   const std::string& message);
    void handle_search_result_activated_(const std::string& room_id,
                                         const std::string& event_id);

    // Monotonic id for the latest issued search; stale responses are dropped.
    std::uint64_t search_request_id_ = 0;
    // request_id → the query string it was issued for (so a response can be
    // tagged for the overlay's stale-drop check); erased on completion.
    std::unordered_map<std::uint64_t, std::string> search_pending_queries_;

    // ── Per-room "find in conversation" search (Ctrl+F / Cmd+F) ──────────
    void handle_in_room_search_query_(const std::string& query);
    void handle_in_room_search_results_ui_(std::uint64_t request_id,
                                           std::vector<tesseract::SearchHit> results);
    void handle_in_room_search_failed_ui_(std::uint64_t request_id,
                                          const std::string& message);
    void in_room_search_navigate_(int delta);
    void set_in_room_search_paginate_(bool enabled);
    void in_room_search_focus_current_();
    void in_room_search_maybe_paginate_(bool at_oldest_boundary);
    void in_room_search_apply_highlights_();
    void in_room_search_clear_();
    // Returns the active RoomSearchBar, or nullptr when unavailable.
    views::RoomSearchBar* in_room_search_bar_() const;

    std::uint64_t in_room_search_request_id_ = 0;
    std::unordered_map<std::uint64_t, std::string> in_room_search_pending_;
    std::string   in_room_search_room_id_;
    // Non-null while a popout window's search is active; nullptr means the
    // main window's room_view_ is the search target.
    views::RoomView*  in_room_search_active_rv_  = nullptr;
    RoomWindowBase*   in_room_search_active_win_ = nullptr;
    std::vector<tesseract::SearchHit> in_room_search_matches_;
    int           in_room_search_current_           = -1;
    bool          in_room_search_paginate_          = false;
    // Set when pagination was triggered by the in-room search; cleared when
    // handle_paginate_result_ui_ re-runs the query.
    bool          in_room_search_rerun_on_paginate_ = false;
    // When true, the next results delivery should focus the oldest match (index 0).
    // Set when UP-at-oldest triggers back-pagination.
    bool          in_room_search_goto_oldest_       = false;
    // Set alongside in_room_search_rerun_on_paginate_ so the results handler
    // can detect a paginate-triggered re-run and continue if no new matches.
    bool          in_room_search_paginate_rerun_    = false;
    int           in_room_search_prev_match_count_  = 0;

    // Event ID we are currently paginating towards (empty when idle).
    std::string pending_scroll_room_event_id_;

    // MSC3030: begin a focused-timeline subscription centred on event_id.
    void begin_focused_subscription_(const std::string& room_id,
                                     const std::string& event_id);

    // MSC3030 jump-to-date: resolve ts_ms to an event and begin a focused
    // subscription. Shared handler wired from all four platform shells via
    // room_view_->on_date_jump. The 1-arg overload uses current_room_id_;
    // the 2-arg overload is used by RoomWindowBase for popout windows.
    void handle_date_jump_(std::uint64_t ts_ms);
    void handle_date_jump_(const std::string& room_id, std::uint64_t ts_ms);

    // MSC3030: clear stale focused-timeline state when (re-)entering a room via
    // the live room-selection path.  Must be called before subscribe_room() so
    // that the subsequent handle_timeline_reset_ui_() sees is_focused == false.
    void clear_focused_state_(const std::string& room_id);

    // Restore a saved tab session: populates tabs_ from room_ids (filtered to
    // those present in rooms_ and not spaces), sets the active tab to
    // active_room_id, then fires on_tab_state_changed_ui_() once.
    // Returns true when at least one tab was found and the session was applied.
    bool try_restore_tab_session_(const std::vector<std::string>& room_ids,
                                  const std::string& active_room_id);

    // MSC3030: paginate forward in a focused timeline; switches to live when done.
    void request_forward_history_(const std::string& room_id);

    // MSC3030: tear down focused state and re-subscribe live.
    void return_to_live_(const std::string& room_id);

    // ── Room media gallery ("Media (N)" row → RoomMediaView overlay) ──────
    // The gallery reuses the room's already-active Timeline subscription
    // (no dedicated Rust/FFI surface) and filters raw pagination batches to
    // Image/Video client-side, so a single scroll-to-top gesture may need
    // several backend round-trips in a media-sparse room. These are the only
    // hooks: everything else (mounting, the count on RoomInfoPanel, the
    // click→lightbox wiring) lives in RoomMediaView/RoomView/RoomInfoPanel or
    // the per-shell click handlers (mirroring room_view_->on_image_clicked).

    // Seeds the overlay from room_view_'s already-synced messages() and opens
    // it. Called from RoomView::on_media_view_requested (wired once in
    // wire_main_app_widget_, shared by all shells).
    void open_room_media_view_(const std::string& room_id);
    // Cancels the gallery's dedicated media-fetch group and clears
    // media_view_room_id_. Called from RoomMediaView::on_close.
    void close_room_media_view_();
    // Issues one more paginate_back_async batch for the gallery's room,
    // sharing pagination_[room_id] with the main timeline so the two can
    // never race. Called from RoomMediaView::on_load_older_media and,
    // internally, by handle_paginate_result_ui_'s retry/accumulate chain.
    void request_media_view_pagination_back_(const std::string& room_id);

    // Non-zero only while the gallery is open; the room it's open for.
    std::string   media_view_room_id_;
    // Distinct from active_media_group_ so closing the gallery or switching
    // rooms cancels only its own in-flight thumbnail fetches.
    std::uint64_t media_view_group_ = 0;
    // Remaining automatic paginate_back_async retries for the current
    // scroll-to-top gesture (reset in open_room_media_view_ and each time
    // on_load_older_media fires a *new* gesture — see RoomMediaView.cpp).
    int media_view_retries_left_ = 0;
    // Media rows the most recent prepend batch added to the gallery. Read
    // (and reset to 0) by handle_paginate_result_ui_ right after use.
    int media_view_last_round_media_count_ = 0;
    // request_id of the gallery's own currently in-flight paginate_back_async
    // call, or 0 if none. Lets close_room_media_view_() cancel it on the Rust
    // side (client_->cancel_paginate_back) instead of merely abandoning it —
    // otherwise the tokio task keeps running and, if the gallery is reopened
    // for the same room before it resolves, its stale result can be
    // misattributed to the new session (room_id is the only correlation key
    // handle_paginate_result_ui_ has for the gallery). Cleared here and by
    // handle_paginate_result_ui_ once the request resolves on its own.
    std::uint64_t media_view_pending_request_id_ = 0;
    static constexpr int kMediaViewMinPerRound = 6;  // ~one grid row
    static constexpr int kMediaViewMaxRetries  = 4;  // 4*kPaginationBatch raw events/gesture
    // Cap on concurrent media fetches the gallery's image provider will
    // kick off. A dense thumbnail grid can make dozens of cells newly
    // visible in one paint pass (opening the gallery, a big scroll) — far
    // more than the sparser main timeline ever shows at once. Kicking a
    // fetch for all of them simultaneously floods the disk-read/decode
    // pipeline and lands a burst of UI-thread completions (cache store +
    // repaint) in a tight window, which reads as stutter/freeze even
    // though each individual hop is off-thread. Checked against the
    // *global* media_fetches_in_flight_ size (not gallery-scoped) —
    // simpler than per-group bookkeeping and the goal is just "don't pile
    // more concurrent work onto an already-busy pipeline," regardless of
    // source. Cells beyond the cap stay on the placeholder; each
    // completion's repaint re-evaluates them, so the backlog drains
    // itself in waves as slots free up rather than needing an explicit
    // retry/timer mechanism.
    static constexpr std::size_t kMaxConcurrentMediaFetches = 8;

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

    // One-time check after push_rooms_(): if recovery is Disabled or Incomplete,
    // raise the encryption-setup overlay. Guards on encryption_setup_shown_ and
    // encryption_setup_dismissed_ so the overlay is shown at most once per session.
    void check_encryption_setup_();

private:
    // intentionally empty — all other state is protected so shells can reset it
    // on logout / account-switch without needing friend declarations.
};

} // namespace tesseract
