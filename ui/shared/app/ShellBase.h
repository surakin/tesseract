#pragma once
#include <tesseract/account_session.h>
#include <tesseract/autostart.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/image_pack.h>
#include <tesseract/paths.h>
#include <tesseract/power_monitor.h>
#include <tesseract/screen_lock.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>
#include <tesseract/visual.h>
#include <tesseract/waveform_cache.h>
#include "app/AccountManager.h"
#include "app/AurUpdateChecker.h"
#include "app/GithubUpdateChecker.h"
#include "app/PresenceTracker.h"
#include "app/PowerPolicy.h"
#include "app/HistoryExportController.h"
#include "app/SettingsController.h"
#include "app/status_links.h"
#include "app/ThreadPanelController.h"
#include "app/UpdateChecker.h"
#include "tk/audio_capture.h"
#include "tk/audio_playback.h"
#include <tesseract/call_session.h>
#include "tk/video_capture.h"
#include "tk/screen_capture.h"
#include "tk/location_provider.h"
#include "app/CallWindowBase.h"
#include "views/CallOverlayWidget.h"
#include "tk/canvas.h"
#include "tk/inflight_dot.h"
#include "tk/interval_timer.h"
#include "tk/theme.h"
#include "tk/weak_self.h"
#include "app/RoomWindowBase.h"
#include "views/ComposeBar.h"
#include "views/EncryptionSetupOverlay.h"
#include "views/MessageListView.h"
#include "views/QuickSwitcher.h"
#include "views/RoomListView.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <atomic>
#include <filesystem>
#include <functional>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
class RoomHeader;
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
class ShellBase : public tk::EnableWeakSelf<ShellBase>
{
    // EventHandlerBase calls post_to_ui_, push_rooms_, push_room_list_state_,
    // and all the handle_*_ui_ / on_room_list_state_ui_ virtuals from lambdas
    // captured on the worker thread — these are protected but EventHandlerBase
    // is not a subclass, so grant friendship.
    friend class EventHandlerBase;
    // RoomWindowBase accesses client_, rooms_, current_room_id_,
    // pagination_, my_user_id_, and post_to_ui_ from its helper methods.
    friend class RoomWindowBase;
    // RoomPane accesses the same protected state as RoomWindowBase above —
    // it's the shared per-room display logic both RoomWindowBase (pop-outs)
    // and ShellBase itself (main window) hold by composition. Once
    // RoomWindowBase is fully retrofitted to delegate to a RoomPane member,
    // the friend class RoomWindowBase grant above can likely be narrowed or
    // removed in favor of this one.
    friend class RoomPane;

public:
    explicit ShellBase(AccountManager& account_manager);
    virtual ~ShellBase();

    // Bring this window to the foreground and give it keyboard focus.
    // Platform implementations: Qt6 = raise()+activateWindow(), GTK4 = gtk_window_present(),
    // Win32 = SetForegroundWindow(), macOS = makeKeyAndOrderFront:.
    virtual void raise_and_activate_() = 0;

    // Put this window into / out of OS full-screen (title bar + taskbar/dock
    // hidden). Driven by the media viewer overlays' full-screen toggle via
    // RoomPane::Deps::set_window_fullscreen. Mirrors raise_and_activate_():
    // Qt6 = showFullScreen()/showNormal(), GTK4 = gtk_window_(un)fullscreen(),
    // Win32 = borderless-to-monitor, macOS = -toggleFullScreen:. Default no-op
    // for shells with no window (tests).
    virtual void set_window_fullscreen_(bool /*on*/) {}

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

    // ── MatrixRTC call control (Layer 4) ─────────────────────────────────────
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

    // True if room_id already has a tab open in THIS window (tabs_).
    bool room_open_in_tab(const std::string& room_id) const;

    // True if room_id is already open in a pop-out window (secondary_windows_).
    bool room_open_in_window(const std::string& room_id) const;

    // Room-list row context menu's "Leave room" action: prompts for
    // confirmation (via main_app_'s shared ConfirmDialog) before calling
    // leave_room_command_. room_id need not be the currently-displayed room.
    void confirm_leave_room_(const std::string& room_id);

    // Close the tab for room_id. No-op if room_id isn't an open tab.
    // Closing the only open tab deselects to the "no active room" empty
    // state (RoomView falls back to showing BrandView) rather than leaving
    // it open — the same state already used on a fresh login, when leaving
    // the last-remaining tab's room, and during account switch/SDK restart.
    void tab_close(const std::string& room_id);

    // Ctrl/⌘+click on a tab: pop the room out into its own native window and
    // close the tab. Closes first so current_room_id_ moves off the room before
    // the new window's acquire_room_subscription_ runs (it then takes its own
    // SDK subscription rather than relying on the main window).
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
        AccountDataSave,
        ThreadSearch,
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

    // Push the current room's pinned_events + can_pin bit, and the
    // redact-others (delete-others'-messages) permission, to room_view_,
    // looking up the RoomInfo in the rooms_ cache. Called from push_rooms_
    // (per sync tick) and after_active_room_changed_ (per room switch). When
    // the room is not yet in the cache, clears all of them so the banner
    // hides and the delete-others affordance disappears.
    void refresh_pinned_for_current_room_();
    // Compute and apply calls-button visibility for one room header:
    // requires server support, a non-bridged room, and either the current
    // user's PL permitting org.matrix.msc3401.call.member, or the user
    // already being in a call for that room (so they can still hang up if
    // their permission was revoked mid-call). Called for room_view_ and
    // every secondary window's header whenever room state, server info, or
    // the active room changes.
    void update_call_btn_visibility_(views::RoomHeader* header, const std::string& room_id);
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

    // Mark the account-data-backed prefs (currently just the room layout —
    // active room + open tabs — but this debounce+dirty-flag machinery is
    // generic, so future im.gnomos.tesseract fields can reuse it) as changed
    // and (re)start the debounced save timer — see persist_room_layout_pref_().
    // Called from after_active_room_changed_() so every tab_open/tab_select/
    // tab_close (and the account-switch path, which clears current_room_id_/
    // tabs_ without calling after_active_room_changed_, and so correctly does
    // NOT re-save the outgoing account's layout as empty) schedules a save.
    // try_restore_tab_session_() also runs through after_active_room_changed_(),
    // which re-schedules a save of the layout it just loaded — harmless (same
    // content, coalesced by the debounce like any other save) rather than
    // worth special-casing out.
    void schedule_account_data_save_();

    // True from the moment schedule_account_data_save_() (re)arms the
    // debounce timer until persist_room_layout_pref_()'s save actually
    // executes. Checked at window-close so a graceful shutdown with nothing
    // unsaved skips the network round-trip entirely.
    bool account_data_dirty_ = false;

    // Persist the current room-layout prefs (active room + open tabs) for the
    // logged-in account. Builds the layout fresh from current_room_id_ + tabs_
    // (PrefsData carries only these). Two calling contexts:
    //  - via the DebounceSlot::AccountDataSave timer armed by
    //    schedule_account_data_save_() — fire-and-forget (Client::
    //    save_prefs_json), so routine mid-session saves never block the UI
    //    thread.
    //  - from on_window_closing_(), which passes `blocking=true` only when
    //    this is the last open window (about to end the process — see its
    //    doc comment for why). blocking=true cancels any still-pending
    //    debounce and, if the layout was dirty, calls Client::
    //    save_prefs_json_blocking() so the write is confirmed sent (or
    //    definitively times out) before shutdown proceeds — closing the gap
    //    where save_prefs's untracked spawned task could lose a race against
    //    process exit and silently drop the last-open-room save.
    void persist_room_layout_pref_(bool blocking = false);

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
    Client* client_ = nullptr;               // non-owning alias
    IEventHandler* event_handler_ = nullptr; // non-owning alias

    std::unordered_map<std::string, std::vector<RoomInfo>> per_account_rooms_;
    std::unordered_map<std::string, std::vector<InviteInfo>> per_account_invites_;
    std::unordered_map<std::string, std::vector<KnockedRoomInfo>> per_account_my_knocks_;

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

    // Owns the per-account history-export controller. Rebuilt on every
    // login/account switch via ensure_history_export_controller_(); unlike
    // settings_controller_, binding a shell's native folder-picker dialog
    // is optional (show_save_folder_dialog defaults unset) so all four
    // shells compile without touching this until they wire the "Export
    // History" trigger.
    std::unique_ptr<tesseract::HistoryExportController> history_export_controller_;

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
    // Server-name routing hints (`?via=` from a matrix.to / `matrix:` permalink),
    // keyed by the permalink's room id or alias. Stashed by open_matrix_link()
    // and consumed by join_room_command_ / knock_room_command_ /
    // lookup_room_command_ so a federated room the homeserver doesn't already
    // know stays reachable. A stale entry is harmless and tiny.
    std::unordered_map<std::string, std::vector<std::string>> pending_join_via_;

    // ── Active-account identity ───────────────────────────────────────────────
    std::string my_user_id_;
    std::string my_display_name_;
    std::string my_avatar_url_;

    // ── Tab state ─────────────────────────────────────────────────────────────
    struct TabState
    {
        std::string room_id;
        float scroll_offset = 0.f; // fractional [0,1]: 0=top, 1=bottom
    };
    std::vector<TabState> tabs_;
    size_t active_tab_idx_ = 0;

    // ── Per-room compose drafts ───────────────────────────────────────────────
    // Unsent compose-bar content (text + at most one staged attachment)
    // stashed when the user navigates away from a room, keyed by room id
    // (not tab index — survives a tab being closed/reopened or the room
    // being reached via a plain room-list click with no tab involved).
    // In-memory only; not persisted to disk or account data.
    struct RoomComposeDraft
    {
        std::string text;
        int cursor_byte_pos = 0;
        std::optional<views::ComposeBar::PendingAttachment> pending;
    };
    std::unordered_map<std::string, RoomComposeDraft> room_compose_drafts_;
    // Snapshot compose_bar()'s current text + pending attachment into
    // room_compose_drafts_[room_id] (erasing any existing entry if there is
    // nothing to save), and remove the attachment from the live widget.
    // Call with the OLD room id before leaving it.
    void save_room_compose_draft_(const std::string& room_id);
    // Re-apply a previously saved draft for room_id into compose_bar(), if
    // one exists. No-op (leaves the widget in its current, cleared state)
    // when nothing was saved for this room. Call with the NEW room id after
    // set_room()/retarget().
    void apply_room_compose_draft_(const std::string& room_id);

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
    // ── Room knocking (MSC2403) ──────────────────────────────────────────────
    // Rooms the active account has knocked on and is still awaiting a
    // decision for — mirrors invites_ exactly (global snapshot, refreshed by
    // push_my_knocks_ on every on_my_knocks_updated_ tick).
    std::vector<KnockedRoomInfo> my_knocks_;
    // Pending knock requests for whichever room's admin-side "Requests to
    // join" panel is currently open. Only one room is ever subscribed at a
    // time (subscribe on panel open, unsubscribe on panel close/room
    // switch), so a single cache — not a per-room map — suffices.
    std::string knock_requests_panel_room_id_;
    std::vector<KnockRequestInfo> current_room_knock_requests_;
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
    // Bumped on every lookup_room_command_ call (Join tab of AddRoomView);
    // captured by the posted continuation so a stale get_room_summary()
    // result from a superseded lookup is discarded. Mirrors
    // unjoined_fetch_gen_'s exact idiom.
    std::uint64_t join_room_lookup_gen_ = 0;
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
    // Set while the full-window app-settings page (SettingsView) replaces the
    // main app surface. While set, compose_window_title_() stays plain
    // "Tesseract" instead of "Tesseract - <room>".
    bool app_settings_open_ = false;
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
    // Tracks the knock currently shown in the KnockStatusCard so on_cancel
    // targets the right room. Empty when no card is shown.
    std::string current_knock_status_room_id_;
    /// Rooms to restore on next on_rooms_updated_: [0] is the active tab,
    /// [1..N] are background tabs. Cleared once fully consumed.
    std::vector<std::string> pending_restore_rooms_;
    /// Counts push_rooms_() ticks where pending_restore_rooms_ was still
    /// non-empty (i.e. background-warming dispatches were skipped in favor
    /// of the restore — see push_rooms_'s restore_pending gate). The
    /// previously-active room can legitimately never reappear (left/kicked
    /// from another device since last session), which would otherwise gate
    /// backfill/bridge-check/prefetch for the rest of the session; this caps
    /// how long that gate holds before giving up and letting them resume
    /// regardless. Reset whenever pending_restore_rooms_ is (re)populated.
    int restore_gate_ticks_ = 0;
    static constexpr int kRestoreGateMaxTicks = 20;
    /// Pop-out room IDs to reopen after the room list becomes available.
    /// Populated from Settings::popout_windows at session restore time.
    std::vector<std::string> pending_restore_popouts_;

    // Populate pending_restore_popouts_ from Settings::popout_windows, once
    // per session restore (idempotent: no-op when already populated).
    // Platform shells call this right after setting pending_restore_rooms_.
    void populate_pending_restore_popouts_();
    std::vector<std::string> space_stack_;

    // Current room-list search query (empty when search is inactive). Owned by
    // the shared search-field wiring in wire_main_app_widget_(); read by
    // is_room_search_active_().
    std::string room_search_text_;

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

    // The main window's own currently-displayed-room pane, sharing the same
    // per-room wiring/SDK-operation logic every pop-out uses via
    // RoomWindowBase. Constructed once (right after wire_main_app_widget_)
    // and retargeted on every tab switch — see RoomPane::retarget(). The sole
    // source of truth for room_view_'s image/video viewer callbacks (the
    // former wire_main_app_viewers_ duplicate was removed).
    std::unique_ptr<RoomPane> main_room_pane_;

    // Last visibility state applied by update_video_playback_suspension_(),
    // so it's a no-op when called redundantly (e.g. from both changeEvent and
    // showEvent on the same transition).
    bool video_playback_suspended_ = false;

    // MSC2545 emoticon flat list — every pack's images, unfiltered by room.
    // Rebuilt on handle_image_packs_updated_ui_. Used only by
    // shortcode_for_mxc_ (rendering an *existing* reaction chip's label,
    // which must resolve regardless of room scope) — the shortcode popup's
    // own candidate list is emoticons_for_room_(), not this.
    std::vector<tesseract::ImagePackImage> cached_emoticons_;
    // Same images as cached_emoticons_, grouped by their owning pack — the
    // source emoticons_for_room_ filters per composer without re-fetching
    // from the SDK. Rebuilt alongside cached_emoticons_.
    std::vector<std::pair<tesseract::ImagePack, std::vector<tesseract::ImagePackImage>>>
        emoticon_packs_;

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
    // Dedup guard for fetch_reply_details, keyed by event_id — not room_id,
    // so unlike pagination_/last_sent_receipt_ it can't be pruned when a
    // room ages out of the warm-subscription LRU. ensure_reply_details_()
    // bounds it directly (full clear once oversized, mirroring
    // voice_bytes_cache_'s cap) instead.
    std::unordered_set<std::string> reply_details_requested_;
    std::unordered_set<std::string> media_fetches_in_flight_;
    std::unordered_set<std::string> sticker_fetches_in_flight_;
    std::unordered_set<std::string> emoji_fetches_in_flight_;
    std::unordered_set<std::string> tile_fetches_in_flight_;
    // OSM tile URLs that failed to fetch, suppressing retries this session.
    // Cleared on account switch / logout / "Clear Cache" — see the matching
    // comment on url_previews_ below.
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

    // Cancellation groups of every currently-open room-media gallery (the main
    // window's plus any pop-outs'). A gallery fetches thumbnails under its own
    // salted group (RoomPane::media_view_group_) so closing it cancels only its
    // own downloads; without this set, fetch_media_pipeline_'s should_deliver_
    // gate — which otherwise only passes group 0 or active_media_group_ — would
    // drop every one of those thumbnails as "stale". Maintained by RoomPane
    // open_/close_room_media_view_ (UI thread only). Rarely more than one entry.
    std::unordered_set<std::uint64_t> active_media_view_groups_;

    // Stable non-zero group id for a room's media (so a switch cancels the right
    // set). 0 is reserved for ungrouped / never-cancelled requests.
    static std::uint64_t media_group_for_room_(const std::string& room_id)
    {
        if (room_id.empty())
            return 0;
        std::uint64_t h = std::hash<std::string>{}(room_id);
        return h == 0 ? 1 : h;
    }

    // Hand out a fresh non-zero group id for a caller that needs one
    // independent of any room (e.g. a RoomPane's own video-viewer fetch,
    // which must be cancellable without being tied to room-switch
    // cancellation — see RoomPane::vid_fetch_group_).
    std::uint64_t next_media_group_id_ = 1;
    std::uint64_t alloc_media_group_()
    {
        return next_media_group_id_++;
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

    // Correlates an outstanding fetch_source_stream_async call with its
    // UI-thread callbacks. Deliberately a SEPARATE map from pending_media_:
    // that struct's on_bytes/on_preview tagging already assumes exactly-one
    // completion, and every one-shot caller (avatars, thumbnails, URL tiles,
    // GIFs) would need to reason about a third shape for no benefit. A
    // streaming request instead fires on_chunk zero or more times, then
    // exactly one of on_done/on_failed — never both, never neither, unless
    // the request is cancelled (cancel_media_group_ below erases it with
    // neither running, same "late callback is a no-op" contract as
    // pending_media_).
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
    std::unordered_map<std::uint64_t, PendingMediaStream> pending_media_streams_;

    // Allocate a request_id and register a streaming completion. Returns the
    // id to pass to client_->fetch_source_stream_async. Shares
    // next_media_req_id_'s counter with begin_media_req_/
    // begin_url_preview_req_ so request ids stay globally unique across
    // every pending-request map.
    std::uint64_t begin_media_stream_req_(
        std::uint64_t group_id,
        std::function<void(std::vector<std::uint8_t>&&, std::uint64_t)> on_chunk,
        std::function<void()> on_done,
        std::function<void(std::uint8_t)> on_failed)
    {
        std::uint64_t id = next_media_req_id_++;
        pending_media_streams_[id] = PendingMediaStream{
            group_id, std::move(on_chunk), std::move(on_done),
            std::move(on_failed)};
        on_inflight_ui_();
        return id;
    }

    // Abort and drop every pending media request in `group_id` (room switch,
    // or a RoomPane's own video-viewer cancel-on-close/cancel-before-reopen).
    // Runs each dropped request's on_cancel so its dedup key is freed and the
    // media can be re-requested when the room is re-entered. Streaming
    // requests are simply dropped with neither on_done nor on_failed run —
    // they have no dedup key to free, and a subsequent late on_media_chunk
    // for this request_id becomes a no-op lookup miss in
    // handle_media_chunk_ui_.
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
        for (auto it = pending_media_streams_.begin();
             it != pending_media_streams_.end();)
        {
            if (it->second.group_id == group_id)
                it = pending_media_streams_.erase(it);
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
    // Cleared on account switch (switch_active_account_impl_), logout
    // (logout_active_account_impl_), and explicit "Clear Cache"
    // (clear_all_caches_) — otherwise keyed by URL rather than room/account,
    // so nothing else would ever prune these and they'd grow for the life of
    // the process.
    std::unordered_map<std::string, tesseract::Client::UrlPreview>
        url_previews_;
    std::unordered_set<std::string> url_preview_in_flight_;

    // Decoded UrlPreviewData (title/description/image_mxc + dims) cached for
    // every URL the SDK has resolved. Populated by each shell's
    // on_url_preview_ready_ and looked up by RoomWindowBase::preview_lookup_
    // for both main-window and pop-out room views. Cleared at the same three
    // checkpoints as url_previews_ above.
    std::unordered_map<std::string, tesseract::views::UrlPreviewData>
        url_preview_data_;

    // ── BlurHash decode dedup ────────────────────────────────────────────────
    // Cleared at the same three checkpoints as url_previews_ above.
    std::unordered_set<std::string> blurhash_attempted_;

    // ── Server capabilities ───────────────────────────────────────────────────
    tesseract::ServerInfo server_info_;        ///< populated after first sync
    bool server_info_fetch_started_ = false;  ///< guards begin_server_info_fetch_
    /// Last fetched own extended profile (MSC4133). Populated by
    /// fetch_own_extended_profile_async_() after server_info_ confirms support,
    /// and kept in sync thereafter by handle_profile_field_result_ui_ applying
    /// each successful write directly (see pending_profile_field_writes_)
    /// rather than re-fetching — a re-fetch right after a write isn't
    /// guaranteed to observe it, which was silently reverting sibling fields.
    tesseract::ExtendedProfile own_extended_profile_;
    // request_id → {key, value_json} for in-flight set_or_delete_profile_field_async
    // calls, consumed by handle_profile_field_result_ui_ to apply the write
    // straight into own_extended_profile_ on success.
    std::unordered_map<std::uint64_t, std::pair<std::string, std::string>>
        pending_profile_field_writes_;

    // ── Per-member gendered-pronoun cache (MSC4247 grammatical_gender) ───────
    // Lazily populated — only for user_ids that actually appear in one of the
    // narrow membership-narration cases that use a pronoun (see
    // MessageListView::membership_expanded_phrase's KnockRetracted case and
    // membership_summary_phrase's singleton-group case). Never a room-wide or
    // timeline-wide preload of every member's profile.
    // user_id → resolved possessive pronoun word ("her"/"its"/"their").
    std::unordered_map<std::string, std::string> member_gender_cache_;
    // Dedupes concurrent requests for the same user_id.
    std::unordered_set<std::string> member_gender_inflight_;
    // request_id → user_id for in-flight get_extended_profile_async calls
    // made on behalf of request_member_pronoun_ui_.
    std::unordered_map<std::uint64_t, std::string> pending_member_gender_requests_;

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

    /// Set while the persistent status override has a click action — today,
    /// exactly the "export in progress" message set by
    /// ensure_history_export_controller_(). At most one persistent-status
    /// "owner" exists at a time (the single-export-at-a-time rule), so a
    /// single slot is enough; null means a status-bar click is a no-op.
    /// Each shell's native status-bar click handler calls this directly
    /// when non-null (see e.g. MainWindow.cpp's status_bar_wnd_proc).
    std::function<void()> on_persistent_status_activate_;

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
        // Set once the one-time post-subscribe initial-history fill (see
        // start_room_subscription_) has run for this room's current warm
        // subscription — regardless of whether it reached reached_start.
        // Prevents re-firing that fill on every switch back into an
        // already-subscribed room; cleared along with the rest of this
        // struct when the room ages out of the warm-LRU (prune_warm_
        // subscriptions_), so a fresh subscription gets its own fill.
        bool initial_fill_done = false;
    };
    std::unordered_map<std::string, PaginationState> pagination_;

    // Correlation map for in-flight async paginations.
    // request_id → room_id; cleared in handle_paginate_result_ui_.
    // true = backward paginate, false = forward paginate.
    std::unordered_map<std::uint64_t, std::pair<std::string, bool>>
        pending_paginates_;
    std::uint64_t next_paginate_id_ = 1;

    // request_id -> the RoomPane that issued a paginate_media_view_back_async
    // request (main_room_pane_ or a pop-out's own pane_). Every RoomPane
    // shares this shell's pending_paginates_/next_paginate_id_ above (there's
    // one underlying SDK-side pagination cursor per room regardless of how
    // many UI surfaces are viewing it), but IEventHandler::
    // on_media_view_paginate_result has no per-window addressing of its own
    // — this map exists purely so handle_media_view_paginate_result_ui_ can
    // route the result back to the pane that's actually waiting on it.
    // Entries are removed both on normal resolution and on
    // RoomPane::close_room_media_view_'s cancel path.
    std::unordered_map<std::uint64_t, RoomPane*> media_view_paginate_owners_;

    // ── Async room actions ────────────────────────────────────────────────────
    // These two types are public (stateless descriptors) so tests can name them;
    // MSVC does not honor a derived-class `using` re-export of a protected nested
    // enum the way GCC/Clang do.
public:
    enum class RoomActionKind { Accept, Join, Leave, Create, Knock, AcceptKnock };
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
    // Pruned alongside pagination_ in prune_warm_subscriptions_() when a room
    // ages out of the warm-subscription LRU.
    std::unordered_map<std::string, std::string> last_sent_receipt_;
    static constexpr std::uint16_t kPaginationBatch = 50;
    // Larger one-time batch for the initial fill on a room's first subscribe
    // this session (see start_room_subscription_ / PaginationState::
    // initial_fill_done). Pagination is store-first (matrix-sdk only reaches
    // the network at a genuine gap), so a bigger count pulls more already-
    // cached history straight from disk — enough to fill a maximized desktop
    // window, at most one server round-trip. Scroll-up increments keep using
    // kPaginationBatch.
    static constexpr std::uint16_t kInitialFillBatch = 100;

    // Cap on how many of a room-switch's timeline-reset rows are handed to
    // MessageListView::set_messages() at once. Bigger than a realistic
    // viewport's worth of rows (kInitialFillBatch above is the store-backed
    // fill target this is measured against), so the cap is only ever hit on
    // a warm room the user previously scrolled deep into this session — the
    // excess rows are held in RoomPane::withheld_older_rows_ and drained via
    // the ordinary scroll-up pagination path (see request_pagination_back_),
    // one small batch at a time, instead of forcing tk::ListView to measure
    // the entire snapshot synchronously on first paint.
    static constexpr std::size_t kSwitchDisplayCap = 150;

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

    // ── Idle-TTL timeline eviction ────────────────────────────────────────────
    // Orthogonal to the warm-subscription LRU above: that mechanism permanently
    // exempts the active room, every open tab, every pop-out-pinned room, and
    // every favorite from eviction, so a long session with a few tabs/favorites
    // open still accumulates unbounded live timelines. This tier evicts by
    // elapsed idle time instead of by count, and applies uniformly regardless
    // of tab/favorite/pinned status — the only exemption is genuinely being
    // on-screen right now (the active room in this window, or the room shown
    // in an open pop-out window). Reuses the same teardown primitive as the
    // warm-LRU (unsubscribe_room), so a room evicted here simply does a normal
    // cold resubscribe + initial fill next time it's actually viewed.
    static constexpr std::chrono::minutes kIdleTimelineTtl{30};
    // room_id -> steady_clock time this room was last seen on-screen. Only
    // written by sweep_idle_timelines_ itself, which stamps every currently-
    // visible room on each tick (self-refreshing) — this is what lets a room
    // that's only ever shown via a pop-out (never the main window's active
    // room) still get a real timestamp, so it starts its idle countdown from
    // the moment it's no longer visible rather than being permanently exempt.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        room_last_active_;
    // (room_id, thread_root_event_id) -> same, for thread_timelines.
    std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point>
        thread_last_active_;
    // Pure selection: rooms in last_active but not in currently_visible whose
    // last-active timestamp is older than `now - ttl`.
    std::vector<std::string> select_idle_room_evictions_(
        const std::unordered_map<std::string, std::chrono::steady_clock::time_point>&
            last_active,
        const std::unordered_set<std::string>& currently_visible,
        std::chrono::steady_clock::time_point now,
        std::chrono::minutes ttl);
    // Same shape as select_idle_room_evictions_, keyed by (room_id, thread_root).
    std::vector<std::pair<std::string, std::string>> select_idle_thread_evictions_(
        const std::map<std::pair<std::string, std::string>,
                       std::chrono::steady_clock::time_point>& last_active,
        const std::set<std::pair<std::string, std::string>>& currently_visible,
        std::chrono::steady_clock::time_point now,
        std::chrono::minutes ttl);
    // Builds the currently-visible room/thread sets, calls the two selection
    // functions above, unsubscribes every evicted room/thread, and erases
    // their bookkeeping state (pagination_, last_sent_receipt_,
    // room_last_active_ / thread_last_active_). Called from the existing
    // presence tick — see notify_presence_tick_ — so it needs no timer of
    // its own.
    void sweep_idle_timelines_();

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

        // Block up to `timeout` for every currently-queued-or-executing task
        // to finish, without stopping the pool or joining its threads (unlike
        // drain(), new work posted afterward — e.g. a re-login in the same
        // process — still runs normally). Returns true if the pool went idle
        // in time, false on timeout (a genuinely stuck task just means the
        // caller proceeds anyway, same bounded-wait philosophy as
        // AccountManager::wait_until_drained).
        bool wait_idle(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lk(mu_);
            return cv_.wait_for(lk, timeout, [this]
            {
                return in_flight_.load(std::memory_order_relaxed) == 0;
            });
        }

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
        // Tracks tasks that are queued OR currently executing — unlike
        // pending_, only reaches 0 once a task has actually finished running
        // (see the worker loop), which is what wait_idle() needs: a task can
        // hold a stray shared_ptr<AccountSession> for as long as it's
        // executing, well after it left the queue.
        std::atomic<size_t>               in_flight_{0};
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

    // ── Display scale ────────────────────────────────────────────────────────

    // The main window's device-pixel scale, last pushed by
    // set_current_scale_() — used to size thumbnail requests (see
    // ensure_room_avatar_/ensure_user_avatar_/ensure_media_thumbnail_) so
    // they stay sharp on HiDPI displays. Tracks the main window's scale
    // specifically; popout RoomWindow/CallWindow content can legitimately
    // sit on a different-scale monitor (same per-window-scale precedent as
    // tk::Widget::apply_scale_change — see MainWindow.cpp's WM_DPICHANGED
    // handler), so this is deliberately not popout-aware.
    float current_scale_ = 1.0f;

    // Called by each shell once at startup (with a freshly-queried native
    // scale) and again from the main surface's set_on_scale_changed()
    // callback whenever the display's scale changes live. A changed scale
    // invalidates every cached thumbnail/avatar — they were fetched from
    // the server at the old pixel size — so this clears both in-memory
    // caches rather than leaving stale, wrong-size entries to linger
    // indefinitely (thumbnail_cache()/image_cache() key purely by mxc/url,
    // no size encoded, so a stale small entry would otherwise satisfy
    // every future contains() check forever). DPI changes are rare, so a
    // full flush + natural re-fetch on next paint is the simple, safe
    // choice over rekeying every cache entry by requested size.
    void set_current_scale_(float scale);

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

    // Send a sticker to current_room_id_, routing into the open thread panel
    // when one is active (mirrors the branch every shell's sticker-picker
    // on_selected duplicated). When the compose bar has a pending reply, it
    // is attached as an m.in_reply_to relation and cleared afterward —
    // mirrors wire_voice_capture_'s on_stopped handling of reply_event_id.
    void send_sticker_(const std::string& body, const std::string& image_url,
                       const std::string& info_json);

    // Core handler: fast path (existing DM), in-flight dedup, loading state,
    // async get_or_create_dm, navigate on success. Always called on UI thread.
    void handle_open_dm_(const std::string& user_id, const std::string& reason = "");

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

    // ── GNOME Shell / KRunner search-provider registration ─────────────────
    // Registers this shell's rooms/known_users providers + activation
    // callbacks into account_manager_.search_backend() so the Linux D-Bus
    // adapters (GtkSearchProviderGtk, QtKRunnerPlugin — see SearchBackend.h)
    // can query/activate across every open window. Called once from
    // wire_main_app_widget_; unregistered in the destructor.
    void register_search_backend_();
    std::optional<SearchBackend::Handle> search_backend_handle_;

    // Platform screen-lock probe for the notification-image privacy gate.
    // Defaults to the fail-safe Null impl until the concrete shell installs
    // a real one via set_screen_lock_().
    std::unique_ptr<IScreenLock> screen_lock_ =
        std::make_unique<NullScreenLock>();

    // ── Low power mode ────────────────────────────────────────────────────
    // Platform battery / energy-saver probe. Defaults to the Null impl (both
    // signals false → Auto behaves as "not low power") until the concrete
    // shell installs a real one via set_power_monitor_().
    std::unique_ptr<IPowerMonitor> power_monitor_ =
        std::make_unique<NullPowerMonitor>();
    // Resolves Settings::low_power_pref + the two monitor signals into the
    // effective "low power mode is active" boolean, with debounce. Must exist
    // from startup so a persisted `On` takes effect on the first sync.
    PowerPolicy power_policy_;
    // Mirrors the window-active bit PresenceTracker also tracks; needed here
    // because resolve_presence_polling_() folds it together with low-power.
    bool last_window_active_ = true;

    // Install the platform power monitor (called once by the concrete shell
    // at startup, mirroring set_screen_lock_()). Wires its change callback and
    // seeds the policy with the current signals.
    void set_power_monitor_(std::unique_ptr<IPowerMonitor> pm);

    bool low_power_active() const { return power_policy_.active(); }

    // Whether this machine has a battery — gates whether the Low Power Mode
    // setting is shown at all. False on a desktop / mini PC.
    bool low_power_available() const
    {
        return power_monitor_ && power_monitor_->has_battery();
    }

    // Read power_monitor_ and feed power_policy_; schedule a one-shot tick when
    // a debounce is armed so the flip doesn't wait for the 30 s presence tick.
    void refresh_low_power_signals_();

    // Settings → General → "Low power mode" radio handler: persist the pref and
    // hand it to power_policy_ (which applies On/Off immediately).
    void set_low_power_preference_(tesseract::Settings::LowPowerPreference pref);

    // power_policy_.on_mode_change target: start / stop the suspendable work
    // across every account.
    void apply_low_power_mode_(bool active);

    // Per-account apply on login: if low power is already active, tell the new
    // account's client to enter it. Mirrors apply_membership_events_pref_.
    void apply_low_power_pref_(tesseract::Client& client);

    // Sole writer of the SDK's DM-presence-poll knob. Folds together the
    // send_presence user setting, window-active state and low-power mode.
    void resolve_presence_polling_();

    // Each shell overrides to show / hide its subtle status-bar indicator;
    // default no-op covers headless / test builds.
    virtual void on_low_power_mode_ui_(bool /*active*/) {}

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
        // True when any_restore_failed is true *because* the cold-start
        // pre-flight tk::Host::is_network_available() check reported no OS-
        // level connectivity, rather than a real restore/auth/server error —
        // lets each shell pick LoginView::show_offline_error() over
        // show_restore_error() so raw backend detail isn't shown for a plain
        // "you're offline" case.
        bool        network_unavailable = false;
        std::string active_uid;                 // uid to make active (empty when none)
    };

    // One restored account's blocking-I/O output, computed off the UI thread
    // by restore_all_accounts_blocking_(). Touches no ShellBase state — only a
    // freshly-restored Client, its native event bridge, and the plain data
    // read from the client — so it's safe to build on mut_pool_'s worker
    // thread (bridge construction and start_sync are both confirmed
    // background-safe: bridges self-marshal callbacks to the UI thread via
    // post_to_ui_, and start_sync's cost is a blocking Rust-side call, not a
    // UI-toolkit one). `bridge` is declared before `client` to mirror
    // AccountSession's destruction-order invariant (the bridge must outlive
    // the client's tokio runtime teardown). The genuinely UI-thread-affine
    // remainder (notifier install, pref application, AccountManager mutation)
    // happens afterwards, on the UI thread, in finish_restore_accounts_ui_().
    struct RestoredAccountIO
    {
        std::unique_ptr<IEventHandler> bridge;
        std::string                 user_id;
        std::unique_ptr<Client>     client; // set_data_dir()'d + restore_session()'d
        std::string                 display_name;
        std::string                 avatar_url;
        std::string                 last_room;
        std::vector<std::string>    open_rooms;
    };

    // Output of the blocking half of startup restore.
    struct RestoreIOResult
    {
        std::vector<RestoredAccountIO> accounts;
        bool        any_restore_failed = false;
        std::string restore_error;
        // See RestoreResult::network_unavailable above — same meaning,
        // computed here and copied through by finish_restore_accounts_ui_().
        bool        network_unavailable = false;
        std::string active_user_id_hint; // index.active_user_id (may name an
                                          // account that failed to restore)
    };

    // Blocking half of restore: legacy-layout migration, index load, and per-
    // account Client construction / restore_session / identity+prefs fetch,
    // plus make_account_bridge_ + start_sync (both confirmed background-safe
    // — see RestoredAccountIO). Calls the virtual make_account_bridge_ hook
    // (so it can't be static), but otherwise touches only SessionStore
    // statics and locally-owned objects — no mutable ShellBase state — so
    // it's safe to call from any thread, including mut_pool_'s worker
    // thread. This is deliberately where the expensive per-account work
    // lives: restore_session and start_sync both block on real Rust-side
    // I/O/setup, so keeping them off the UI thread is the whole point of the
    // async entry point below.
    //
    // `network_available` is a pre-computed, UI-thread result of
    // tk::Host::is_network_available() (Host isn't reachable from this
    // worker-thread-safe method itself — see restore_all_accounts_async_'s
    // doc comment). When false, every stored account is short-circuited
    // straight to a failed/network_unavailable result without attempting
    // the always-network-bound Client::restore_session() call (see
    // sdk/src/oauth.rs's build_configured_client(), which performs
    // well-known discovery unconditionally). Defaults to true so existing
    // callers (tests, restore_all_accounts_()) keep today's always-attempt
    // behavior.
    RestoreIOResult restore_all_accounts_blocking_(bool network_available = true);

    // UI-thread finish half: consumes a RestoreIOResult and does the
    // remaining, genuinely UI-thread-affine steps — the pref-apply calls,
    // install_account_notifier_ / install_account_up_connector_, and
    // account_manager_.add_account. (Bridge construction and start_sync
    // already happened in restore_all_accounts_blocking_(), off the UI
    // thread.) Mutates account_manager_ and other shell state; UI-thread
    // only.
    RestoreResult finish_restore_accounts_ui_(RestoreIOResult&& io);

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
    // finish-login decision. UI-thread only. Implemented as a composition of
    // restore_all_accounts_blocking_() + finish_restore_accounts_ui_() — kept
    // as a synchronous single-call entry point for callers (e.g. tests) that
    // don't need the async form below.
    RestoreResult restore_all_accounts_();

    // Async startup entry point: runs restore_all_accounts_blocking_() on
    // mut_pool_ (off the UI thread, so the slow SQLite/crypto-store open
    // during Client::restore_session doesn't freeze the window), then hops
    // back to the UI thread (post_to_ui_alive_-guarded) to run
    // finish_restore_accounts_ui_() and invoke `done` with the resulting
    // RestoreResult — the same shape restore_all_accounts_() returns
    // synchronously. Fires on_startup_restore_progress_ui_() with a status
    // string synchronously before the worker starts, and again with an empty
    // string right before `done` runs. UI-thread only to call; `done` itself
    // runs on the UI thread.
    //
    // `network_available`: see restore_all_accounts_blocking_'s doc comment
    // — callers query tk::Host::is_network_available() on the UI thread
    // themselves (this method never touches Host) and pass the result in.
    // Kept as a trailing defaulted parameter (not leading) so existing
    // single-argument call sites/tests compile unchanged.
    void restore_all_accounts_async_(std::function<void(RestoreResult)> done,
                                     bool network_available = true);

    // Called on the UI thread with a short, localized, generic status string
    // (e.g. "Restoring session…") describing startup account-restore
    // progress; an empty string means "done/clear". No per-account detail —
    // deliberately coarse. Default no-op; shells override to forward the
    // text (and drive a spinner) on their BrandView, the only surface
    // visible during restore. Not to be confused with the unrelated
    // on_restore_status_ui_() below (a status-bar-override-timer clear).
    virtual void on_startup_restore_progress_ui_(const std::string& /*status_text*/) {}

    // ── Add-account login finalize ────────────────────────────────────────────
    // Outcome of finalize_login_async_(): lets each shell run the native finish
    // (or the native duplicate-reject UI) without re-deriving state. On a
    // successful add,
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

    // Blocking half of add-account finalize: exports the pending client's
    // session, drops the pending client (releasing SQLite handles), renames
    // the pending temp dir → the final per-account dir (copy+remove fallback
    // for cross-filesystem), saves the account JSON, reopens a fresh Client
    // at the final path, restore_session + caches display name / avatar /
    // prefs, and builds the per-account bridge (make_account_bridge_) +
    // start_sync — all confirmed background-safe (see RestoredAccountIO),
    // and all genuinely blocking (file I/O, restore_session, start_sync), so
    // this runs on mut_pool_'s worker thread via finalize_login_async_()
    // below. Takes the pending client / temp dir by value (moved in by the
    // caller) rather than reading pending_login_client_ /
    // pending_login_temp_dir_ directly, since the UI thread must not touch
    // those once handed off. On success, io.session holds a fully-populated
    // AccountSession (bridge + client + prefs, sync already started) still
    // missing its native notifier / up_connector and not yet added to
    // account_manager_ — the UI half finishes that. On failure, io.session
    // is null and io.result carries the reason.
    struct FinalizeLoginIO
    {
        FinalizeLoginResult                  result;
        std::unique_ptr<AccountSession>      session; // null unless result.ok
    };
    FinalizeLoginIO finalize_login_blocking_(
        std::unique_ptr<Client> pending_client,
        std::filesystem::path   pending_temp_dir);

    // Async, platform-agnostic core of each shell's on_login_succeeded, run
    // after OAuth completes for a NEWLY added account on
    // pending_login_client_. On the UI thread: fetches the user_id and
    // rejects (rejected_duplicate) if account_manager_.find(uid) — resolving
    // `done` synchronously in both the empty-client and duplicate cases, no
    // worker hop needed. Otherwise moves pending_login_client_ /
    // pending_login_temp_dir_ out and dispatches finalize_login_blocking_()
    // onto mut_pool_; the post_to_ui_alive_-guarded continuation installs the
    // native notifier (install_account_notifier_) and Linux-only UnifiedPush
    // connector (install_account_up_connector_), adds the account, updates
    // the on-disk index (active = the new uid), and invokes `done`. Does NOT
    // touch native widgets (login-view dismiss, surface switch, status bar)
    // — the shell does the native finish using the result passed to `done`.
    // The shell must call set_client(nullptr) on its login view BEFORE
    // calling this when it owns a raw alias to pending_login_client_ (it is
    // moved out here). UI-thread only to call; `done` itself runs on the UI
    // thread.
    void finalize_login_async_(std::function<void(FinalizeLoginResult)> done);

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
        std::string logged_out_uid;        // the uid that was signed out
        std::string next_uid;              // the surviving uid switched to (if any)
        // No `ok` field: client_->logout() now runs on mut_pool_ (it can take
        // several seconds — see the call site's comment), so its result isn't
        // known by the time this function returns. A failure still surfaces
        // via show_status_message_ once the background call completes; no
        // caller across any of the four shells (or the tests) read this
        // field's old synchronous value, so dropping it is not a behavior
        // change for any of them.
    };

    // Bound on how long logout_active_account_impl_() (and, symmetrically,
    // finalize_login_async_()'s second-line-of-defense check) will block the
    // UI thread waiting for a just-logged-out session's mut_pool_ teardown
    // barrier to fire (see AccountManager::wait_until_drained). mut_pool_'s
    // worker makes progress independently of the UI thread during this wait
    // (a real OS thread, not something the UI event loop needs to pump), so
    // this never risks a deadlock — only a bounded stall, and only when a
    // stale worker was genuinely stuck despite request_stop().
    static constexpr std::chrono::milliseconds kAccountDrainTimeout{2000};

    // Platform-agnostic teardown for each shell's logoutActiveAccount /
    // logout_active_account / _logoutActiveAccount. Run on the active account; a
    // no-op (logged_out=false) when there is none. It:
    //   - captures the active uid;
    //   - calls client_->request_stop() FIRST, so any run_async_mut_ worker
    //     already queued or mid-flight against this client (a cancellable
    //     block_on — poll_presence_now, subscribe_room, send_message, ...)
    //     unblocks immediately instead of running its own HTTP timeout/retry
    //     budget while the drain below waits on it;
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
    //   - marks the uid draining (AccountManager::mark_draining) BEFORE removing
    //     it from AccountManager, so there is no window where it's neither
    //     findable nor flagged; removes the account, resets active_account_ / the
    //     client_ / event_handler_ aliases, and the agnostic visible state
    //     (rooms_/invites_/current_invite_/space_stack_/identity/pagination/…);
    //   - posts a barrier task to mut_pool_ (via run_async_mut_) that drops the
    //     session's last reference and clears the draining flag — mut_pool_ is a
    //     strict single-thread FIFO, so this is guaranteed to run only after every
    //     earlier-queued-or-in-flight task that captured the session has finished
    //     — then bound-waits on it (AccountManager::wait_until_drained,
    //     kAccountDrainTimeout) so the old session's SQLite-backed store is either
    //     fully closed, or the wait has at least given request_stop() a fair
    //     chance to unblock it, before this function returns;
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

    // Liveness-guarded post_to_ui_: only invokes fn() if this ShellBase is
    // still alive when the continuation runs on the UI thread. Use this for
    // any continuation enqueued from a worker body or an SDK callback that
    // dereferences `this`/members, so a continuation queued before a
    // spawned/secondary window (or account-switch) teardown safely no-ops
    // instead of using freed state.
    void post_to_ui_alive_(std::function<void()> fn)
    {
        post_to_ui_(guarded(std::move(fn)));
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

    // Returns true while a room-list search is active (search text is non-empty).
    // Drives refresh_room_list_()'s suppression of space-child filtering during
    // search (all rooms are shown unfiltered). Backed by room_search_text_,
    // which is owned by the shared search-field wiring in wire_main_app_widget_().
    bool is_room_search_active_() const { return !room_search_text_.empty(); }

    // Compute the filtered room list and push it to the shared RoomListView.
    // Shells call this from their own refreshRoomList() wrapper to avoid
    // duplicating the space-child filter and unread-override logic.
    void refresh_room_list_();

    // Wire every SettingsView callback whose body is pure Settings
    // persistence or a forward into an existing ShellBase handler — i.e.
    // everything that does NOT need a Surface/Host/native dialog. Each shell
    // calls this once, right after constructing its SettingsView, then wires
    // only what's left: on_close/on_logout/on_reset_identity (differ in how
    // the settings surface is dismissed), on_tab_changed (needs the shell's
    // Surface), and audio/camera/mic device enumeration (needs tk::Host —
    // though the *_changed callbacks themselves are wired here).
    void wire_settings_view_(views::SettingsView* view);

    // Wire the SettingsView/SettingsController callbacks shared by every
    // shell's bind_settings_controller_() override: avatar upload/remove
    // delegation, extended-profile field changes, and the non-UI part of
    // SettingsController::on_avatar_changed (sidebar refresh goes through
    // the existing refresh_user_strip_() virtual). Each shell still wires
    // set_request_repaint (needs its own Surface) and the native dialog /
    // image-pack-provider callbacks itself before calling this. `relayout`,
    // if set, is invoked after the avatar url is pushed into the view —
    // shells pass their own `[this]{ surface_->relayout(); }` since the
    // Surface type differs per platform.
    void wire_settings_controller_common_(views::SettingsView* view,
                                          tesseract::SettingsController* ctrl,
                                          std::function<void()> relayout = {});

    // Show the chat-panel root view for a joined space. No-op if the room is
    // unknown or is not a space.
    void show_space_root_(const std::string& space_id);

    // Called after rooms_ is updated — shell refreshes the room-list widget.
    virtual void on_rooms_updated_() = 0;

    // Called after the active room is inserted into the shared shell's MRU.
    // Platform integrations may mirror that list into an OS-native surface.
    virtual void on_recent_room_visited_(const RoomInfo&) {}

    // Raise the encryption-setup modal overlay in the appropriate mode.
    // Each platform shell implements this to show EncryptionSetupOverlay
    // as a full-window overlay on its MainAppWidget. Pure virtual so every
    // shell is required to implement it (Tasks 9–12).
    virtual void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode mode) = 0;

    // Start the MSC4108 QR grant login flow: wires all callbacks on QRGrantView
    // and shows the overlay. QRGrantView owns its check-code tk::TextField
    // directly, so no shell-side native-field wiring is needed here.
    void start_qr_grant_overlay();

    // Called when the forward picker opens: shell focuses its native text field.
    // Called when the forward picker closes: shell hides its native text field.
    virtual void focus_forward_picker_field_() {}
    virtual void hide_forward_picker_field_() {}

    // Wires every platform-agnostic callback on the encryption-setup overlay
    // (recovery/verify actions, clipboard, layout/dismiss) — EncryptionSetupOverlay
    // owns its passphrase/key tk::TextFields directly and reads them itself,
    // so this only needs a Host for the clipboard callback. `host` must
    // outlive the overlay.
    void wire_encryption_setup_callbacks_(views::EncryptionSetupOverlay& ov,
                                          tk::Host&                      host);

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

    // Called after my_knocks_ is updated — shell refreshes the "Requests to
    // Join" room-list section. Mirrors on_invites_updated_().
    virtual void on_my_knocks_updated_()
    {
    }

    // Called after current_room_knock_requests_ changes — either a fresh
    // pull from Client::list_knock_requests (handle_knock_requests_updated_ui_)
    // or a local optimistic edit (decline_knock_request_async_ et al).
    // Implemented directly in ShellBase.cpp (not per-shell like
    // on_invites_updated_) since it only needs main_app_, which every shell
    // already exposes uniformly.
    void on_knock_requests_panel_updated_();

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
    // no thumbnail. Concrete: checks media_disk_cache_ first (so a thumbnail
    // generated in a prior session never re-triggers a video download/decode),
    // and on a miss delegates the platform-specific work to
    // extract_video_first_frame_jpeg_. See ShellBase.cpp for the flow.
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& source_token);

    // Dedup wrapper around generate_video_thumbnail_: inserts into
    // video_thumb_in_flight_ and only calls through on a fresh insert. Every
    // caller wanting a video thumbnail generated (initial prefetch, lazy
    // scroll fetch, user reveal, and paint-time cache-miss self-heal) should
    // go through this rather than touching video_thumb_in_flight_ directly.
    void request_video_thumbnail_(const std::string& event_id,
                                  const std::string& source_token);

    // Worker-thread helper for generate_video_thumbnail_: persists `bytes`
    // (an encoded still image, JPEG or PNG) to media_disk_cache_ under
    // `disk_key` when `persist` is true, decodes it via decode_image_, and
    // stores the result in image_cache_ under `mem_key`.
    void decode_and_cache_video_thumbnail_(std::string mem_key,
                                           std::string disk_key,
                                           std::vector<std::uint8_t> bytes,
                                           bool persist);

    // Platform-specific half of generate_video_thumbnail_: fetch the video
    // (prefix-then-fallback — see fetch_source_prefix_async), decode frame
    // zero with the platform's native video decoder, encode it to a compact
    // still-image format (JPEG or PNG — decode_image_ handles either), and
    // invoke `cb` with those bytes (empty vector on any failure). `cb` may be
    // invoked from any thread; generate_video_thumbnail_ re-marshals as
    // needed. Default is a no-op (empty result) for shells without a
    // video-decode pipeline.
    virtual void extract_video_first_frame_jpeg_(
        const std::string& /*event_id*/, const std::string& /*source_token*/,
        std::function<void(std::vector<std::uint8_t>)> cb)
    {
        cb({});
    }

    // Drag-and-drop media probe. Each shell overrides this to detect gif/webp
    // animation and extract a video/audio thumbnail + duration off the UI
    // thread, then posts update_pending_attachment() back to `target` (a
    // pop-out window's compose bar, guarded by `alive`) or, when `target` is
    // null, the main window's compose bar. Default no-op so a shell can opt
    // out; pop-out windows call this through ShellBase so the same platform
    // probe serves the main and secondary windows. Pairs with
    // views::route_file_drop_to_compose_bar, which invokes it for
    // gif/webp/video/audio.
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

    // (Re)construct history_export_controller_ with the two standard
    // callbacks (post_to_ui_ / run_async_). Unlike
    // ensure_settings_controller_, there is no matching bind_*_ pure
    // virtual: show_save_folder_dialog stays unset (begin() is then a
    // no-op) until a shell explicitly wires it, so all four shells compile
    // untouched until they add the "Export History" trigger.
    void ensure_history_export_controller_();

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

    // Persist the Emojis & Stickers tab's staged changes (see
    // apply_room_settings_'s `changes.image_packs` branch, which calls
    // this): uploads any brand-new image's bytes via `upload_media` first
    // (a single image's upload failure drops only that image, recorded as
    // an error, and does not abort the rest of that pack's save), then
    // calls `Client::save_room_pack` once per staged pack (a wholesale
    // replace, not an upsert — see ImagePackEditorResult's own doc
    // comment) and `Client::remove_room_pack` once per
    // `removed_state_keys` entry. Returns one error string per failure,
    // prefixed `"image_packs.<what>: "`, for the caller to join into the
    // same aggregate error `apply_room_settings_` already builds for every
    // other field. Blocks — call from a worker thread.
    static std::vector<std::string> apply_image_pack_changes_(
        tesseract::Client* client, const views::ImagePackEditorResult& result);

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

    // Edge-detects the main window's visibility (keyed off
    // is_main_window_visible_(), NOT any_window_visible_() — pop-out windows
    // don't report their own visibility yet, so any_window_visible_() would
    // never see the "hidden" edge while one is open) and pauses/resumes the
    // main room view's inline autoplay video accordingly. No-op if the
    // visibility state hasn't changed since the last call. Each shell calls
    // this from every native show/hide/minimize/restore hook it has (see
    // start_anim_tick_() call sites for the existing resume-side equivalents).
    void update_video_playback_suspension_();

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

    // ── Main-window title ─────────────────────────────────────────────────────
    // The title for the current state: "Tesseract - <room>" when a room is
    // active and settings is closed; otherwise plain "Tesseract".
    std::string compose_window_title_() const;
    // Recompute compose_window_title_() and push it to the OS window through
    // apply_window_title_ui_(). Call after current_room_id_ changes.
    void refresh_window_title_();
    // Toggle app_settings_open_ and refresh the title.
    void set_app_settings_open_(bool open);
    // Per-shell: set the native OS window title. Default no-op (test shells).
    virtual void apply_window_title_ui_(const std::string& /*title*/)
    {
    }

    // ── Tab state hooks ───────────────────────────────────────────────────────
    // Called after tabs_ and current_room_id_ have been updated. The shell must:
    //   1. Sync the TabBar widget (add/remove/set_active).
    //   2. Show/hide TabBar; set RoomHeader condensed mode.
    //   3. Restore compose_draft for the newly active tab.
    // main_room_pane_->retarget(current_room_id_) (called at each tab_* site
    // that updates current_room_id_, before this hook runs) plus the next
    // handle_timeline_reset_ui_ call handle the room-switch display gate —
    // no action needed here.
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
    // Called when the knock-request watcher for `room_id` reports a change.
    // Re-fetches via Client::list_knock_requests and refreshes the
    // KnockRequestsPanel if it is currently showing that room; ignored
    // otherwise (a stale poke from a room whose panel was just closed).
    virtual void handle_knock_requests_updated_ui_(std::string room_id);
    // Completion for an async fetch_media_async download. Looks up the pending
    // request by id (ignoring late callbacks for cancelled/superseded requests)
    // and runs its registered bytes-completion. Concrete shared logic.
    void handle_media_ready_ui_(std::uint64_t request_id,
                                std::vector<std::uint8_t> bytes);
    // Delivery for an async fetch_source_stream_async download. Looks up the
    // pending stream by id (ignoring late callbacks for cancelled requests)
    // and runs on_chunk (status 0), or on_done/on_failed and erases the
    // entry on a terminal status (1/2/3). See PendingMediaStream's doc
    // comment.
    void handle_media_chunk_ui_(std::uint64_t request_id,
                                std::vector<std::uint8_t> chunk,
                                std::uint8_t status,
                                std::uint64_t total_size);
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
    // Progress/completion for Client::start_room_export_async, forwarded to
    // history_export_controller_ when present. Non-empty default bodies
    // (not pure virtual) so all four shells keep compiling untouched until
    // they wire the export trigger — see ensure_history_export_controller_().
    void handle_room_export_progress_ui_(const tesseract::RoomExportProgress& progress);
    void handle_room_export_complete_ui_(std::uint64_t request_id, bool ok,
                                         bool cancelled, bool reached_start,
                                         std::string out_path,
                                         std::uint64_t events_written,
                                         std::uint64_t bytes_written,
                                         std::string message);
    void handle_room_action_complete_ui_(std::uint64_t request_id, bool ok,
                                         std::string joined_room_id,
                                         std::string message);
    void handle_upload_complete_ui_(std::uint64_t request_id, bool ok,
                                    std::string message);
    void handle_upload_progress_ui_(std::uint64_t request_id,
                                    std::uint64_t current_bytes,
                                    std::uint64_t total_bytes);
    virtual void on_upload_progress_ui_(std::uint64_t /*request_id*/,
                                        std::uint64_t /*current_bytes*/,
                                        std::uint64_t /*total_bytes*/)
    {
    }
    virtual void on_upload_finished_ui_(std::uint64_t /*request_id*/,
                                        bool /*ok*/)
    {
    }
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

    // Fired via IEventHandler::on_bot_commands_updated when the cached set
    // of MSC4391 bot commands for `room_id` changes. Concrete: no-op unless
    // `room_id` is the active room, in which case it calls
    // `on_active_room_bot_commands_changed_ui_()` — each shell overrides
    // that no-op to refresh its own SlashCommandController's popup, since
    // (unlike ComposeBar/RoomView) that controller is shell-owned, not
    // shared — see SlashCommandController.h's doc comment.
    virtual void handle_bot_commands_updated_ui_(std::string room_id);

    // Per-shell hook: refresh any currently-open slash-command popup for the
    // active room so a bot posting/editing/retracting a command description
    // while the popup is open updates live. Default no-op (e.g. a shell
    // with no live composer at the moment, or one that hasn't wired the
    // bot-command autocomplete stack yet).
    virtual void on_active_room_bot_commands_changed_ui_()
    {
    }

    // Look up the bare shortcode (no surrounding colons) for an mxc:// URI
    // by scanning cached_emoticons_. Used by the MSC4027 reaction path so
    // image-reaction chips can be rendered as `:shortcode:` and the chip-
    // re-tap toggle can carry the shortcode out on the wire. Returns an
    // empty string when the mxc isn't in any of the user's emoticon packs.
    std::string shortcode_for_mxc_(const std::string& mxc) const;

    // The shortcode-popup candidate list for a composer showing `room_id`
    // (personal pack + that room's own pack + every ancestor space's own
    // pack + every subscribed room's pack — same visibility rule as
    // views::order_picker_packs, via views::is_pack_picker_visible, but
    // without the picker-tab ordering since shortcode lookup ranks by text
    // match). Computed fresh per call (cheap: at most a few hundred
    // entries, plus a small parent_spaces_for_room_ scan) since different
    // open composers — main window vs. each pop-out — may be showing
    // different rooms; RoomWindowBase::shell_emoticons_() calls this with
    // its own persistent room_id_, not the main window's current_room_id_.
    std::vector<tesseract::ImagePackImage>
    emoticons_for_room_(const std::string& room_id) const;

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
                            std::vector<uint8_t> /*image_bytes*/,
                            std::string /*event_id*/)
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

    // Fetch the device's current OS location (tk::LocationProvider) and send
    // it to `room_id` as an m.location event. Called from each shell's
    // on_location hook when the user accepts /location. Reports failure via
    // show_status_message_; sends immediately on success, no confirmation.
    void send_current_location_(std::string room_id);

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
    std::unique_ptr<tk::LocationProvider>       location_provider_;
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

    // Install the platform screen-lock probe (called once by the concrete
    // shell at startup, mirroring the per-account INotifier injection).
    void set_screen_lock_(std::unique_ptr<IScreenLock> sl)
    {
        if (sl)
        {
            screen_lock_ = std::move(sl);
        }
    }

    // Platform "launch at login" registration for the General settings tab.
    // Defaults to the safe Null impl until the concrete shell installs a
    // real one via set_autostart_(); tests/headless builds keep the Null
    // impl and simply can't enable autostart, which is correct for them.
    std::unique_ptr<IAutostart> autostart_ = std::make_unique<NullAutostart>();

    // Install the platform autostart impl (called once by the concrete
    // shell at startup, mirroring set_screen_lock_() above).
    void set_autostart_(std::unique_ptr<IAutostart> autostart)
    {
        if (autostart)
        {
            autostart_ = std::move(autostart);
        }
    }

    // Settings → General → "Launch at login" toggle handler. Attempts the
    // OS-level registration/unregistration; on success, mirrors the result
    // into Settings::launch_at_login (bookkeeping cache used for
    // early-startup gating, see launch_args.h). On failure, leaves OS state
    // untouched and re-pushes the actual queried state back to the
    // checkbox so the UI never shows a value the OS didn't accept.
    void handle_launch_at_login_toggle_(bool enabled)
    {
        if (autostart_->set_enabled(enabled))
        {
            tesseract::Settings::instance().launch_at_login = enabled;
            tesseract::Settings::instance().save_to_disk(tesseract::config_dir());
        }
        else
        {
            on_launch_at_login_pref_ui_(autostart_->is_enabled());
        }
    }

    // Query the real OS "launch at login" state and push it into the
    // General-tab checkbox. Called each time the Settings view opens so the
    // checkbox self-heals if Settings::launch_at_login (the bookkeeping cache
    // load_persisted_settings() seeds it from) drifted — e.g. the user removed
    // the login item outside the app. IAutostart::is_enabled() can be a
    // synchronous OS/IPC call (macOS SMAppService round-trips to smd), so it
    // runs on the worker pool rather than blocking the UI thread mid-open.
    // autostart_ is installed once at startup and never reassigned, and the
    // pool is drained before ~ShellBase, so the raw `this` capture is safe.
    void refresh_launch_at_login_pref_()
    {
        run_async_(
            [this]
            {
                const bool enabled = autostart_->is_enabled();
                post_to_ui_alive_([this, enabled]
                                  { on_launch_at_login_pref_ui_(enabled); });
            });
    }

    // Called after a failed handle_launch_at_login_toggle_() to re-push the
    // actual OS state into the Settings General checkbox. Each shell
    // overrides to forward into its SettingsWidget/SettingsView instance;
    // default no-op covers headless/test builds.
    virtual void on_launch_at_login_pref_ui_(bool /*enabled*/) {}
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
    // Called by a platform notifier's activation/response callback when the
    // user submitted inline reply text from an OS notification. Resolves the
    // AccountSession that owns `user_id` via account_manager_ (does NOT touch
    // active_account_ or navigate — a background reply must not disturb
    // whatever account/room is currently showing, matching macOS's
    // non-foregrounding action and KDE's own reply UX). Sends as a threaded
    // reply when `event_id` is non-empty, else falls back to a plain
    // message. Failures are reported via a follow-up notification (see
    // notify_reply_failed_), not show_status_message_, since the triggering
    // notification may belong to a different account/window than whichever
    // one is currently focused.
    void send_notification_reply_(std::string user_id, std::string room_id,
                                  std::string event_id, std::string text);

    // Builds a synthetic failure Notification (reusing the account's
    // existing INotifier::notify(), same as any other notification) to tell
    // the user a quick reply didn't go out. `reason` is the already-
    // localized detail shown as the notification body.
    void notify_reply_failed_(const std::string& user_id,
                              const std::string& room_id, std::string reason);

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
    // ok=true means the join succeeded; room_id is the canonical joined room
    // ID; message is the SDK failure message (empty on success). Resets
    // RoomPreviewView's Join button on failure and — if AddRoomView's Join
    // tab triggered this action — closes the dialog on success or surfaces
    // the failure in JoinRoomView on failure. No shell needs to override
    // this; it's virtual only so a shell could extend it if ever needed.
    virtual void on_join_room_outcome_ui_(bool ok, const std::string& room_id,
                                          const std::string& message);

    // Called after handle_room_action_complete_ui_() processes a Create
    // action — the CreateRoomView-flavoured counterpart of
    // on_join_room_outcome_ui_ above. Closes the dialog and navigates to the
    // new room on success, or surfaces the failure in CreateRoomView.
    virtual void on_create_room_outcome_ui_(bool ok, const std::string& room_id,
                                            const std::string& message);

    // Fetch space children for every space in rooms_ on a worker thread and
    // post the result into space_children_cache_, then fire
    // on_space_children_cache_ready_ui_(). Idempotent w.r.t. rooms_ contents.
    void update_space_children_cache_();

    // Every space (direct and ancestor) containing `room_id`, derived by
    // repeatedly inverting space_children_cache_ (space_id -> joined
    // children): find room_id's direct parent space(s), then treat each as
    // a "room" and find *their* parent space(s), and so on, so a Space
    // nested inside another Space is included too. A visited-set guards
    // against a cyclical (misconfigured) space hierarchy. Empty if room_id
    // is empty or in no cached space's children. Synchronous, no I/O — safe
    // to call from the UI thread.
    std::vector<std::string> parent_spaces_for_room_(const std::string& room_id) const;

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

    /// Resolve user_id's grammatical-gender pronoun word for gendered
    /// membership-narration text (see the member_gender_cache_ comment
    /// above). No-op if already cached or a fetch is already in flight for
    /// this user_id — callers (MessageListView, via the shell) should call
    /// this only from a currently-visible row that actually needs a pronoun,
    /// never as a bulk/room-wide prefetch. Result arrives via
    /// on_member_pronoun_ready_ui_(user_id).
    void request_member_pronoun_ui_(const std::string& user_id);

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

    // Drives run_image_gc_() every ~2 s (built on post_to_ui_after_, so no
    // per-shell timer wiring). start()ed at the end of wire_main_app_widget_;
    // its dtor (and ~ShellBase) stop it. See tk::IntervalTimer.
    tk::IntervalTimer media_sweep_timer_{
        [this](int ms, std::function<void()> fn)
        { post_to_ui_after_(ms, std::move(fn)); },
        2000, [this] { run_image_gc_(); }};

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

    // Generational mark-and-sweep of the decoded (L0) image caches — the
    // on-screen retention mechanism. Driven every ~2 s by media_sweep_timer_
    // (and from notify_presence_tick_ as a backstop). Gated on recent user
    // activity via AccountManager::image_gc_should_run(), which also dedupes
    // across windows. See the impl for the cycle.
    void run_image_gc_();
    // Unclipped full repaint of this shell's main + pop-out surfaces (the GC
    // mark pass — makes every visible widget re-peek() its images).
    void force_full_repaint_all_surfaces_();

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

#ifdef TESSERACT_UPDATE_CHECKS
    // Persists the "check for updates automatically" preference.
    void handle_check_for_updates_toggle_(bool enabled);
#endif

    // Toggle handler for the "Use historical MSC2545 compatibility" Advanced
    // setting. Persists the setting and calls
    // Client::set_msc2545_legacy_compat() on the active account's client,
    // which synchronously rebuilds the image-pack cache so the change takes
    // effect immediately (no restart needed).
    void handle_msc2545_legacy_compat_toggle_(bool enabled);

    // Toggle handler for the "Enable developer mode" Advanced setting.
    // Persists the setting only — no behavior gated on it yet.
    void handle_developer_mode_toggle_(bool enabled);

    // Handler for the Appearance → Layout combobox. Persists the choice and
    // forces every open timeline (main window, thread panel, pop-out room
    // windows) to re-measure with the matching row renderer — no restart.
    void handle_message_layout_changed_(tesseract::Settings::MessageLayout layout);

#ifdef TESSERACT_CRASH_HANDLER_ENABLED
    // Toggle handler for the "Save a local crash report" Advanced/Diagnostics
    // setting. Persists the setting and calls
    // tesseract::set_crash_reporting_enabled() so the change takes effect
    // immediately, without a restart.
    void handle_crash_reporting_toggle_(bool enabled);
#endif

    // Toggle handler for the "Send Google Maps / OpenStreetMap links as
    // locations" Media setting. Persists the setting only — the flag is
    // read directly from Settings::instance() by dispatch_room_send_ at
    // send time.
    void handle_send_maps_urls_as_location_toggle_(bool enabled);

    // Resume live search indexing for a freshly-restored account's client if
    // the global "index messages for search" preference is enabled. Called
    // right after restore_session/start_sync, on whichever thread is doing
    // that work (background restore/finalize) — genuinely non-blocking
    // (fire-and-forget spawn on the Rust side), confirmed safe either way.
    void apply_search_indexing_pref_(tesseract::Client& client);

    // Apply the persisted "show room join/leave events" preference to a
    // freshly-restored account's Rust client. Called right after
    // restore_session/start_sync so the very first room subscription already
    // reflects the setting instead of defaulting to the Rust-side AtomicBool's
    // off default. A plain atomic store on the Rust side — non-blocking.
    void apply_membership_events_pref_(tesseract::Client& client);

    // Apply the persisted "Use historical MSC2545 compatibility" preference
    // to a freshly-restored account's Rust client. Called right after
    // restore_session/start_sync so the first image-pack rebuild already
    // reflects the setting instead of defaulting to the Rust-side AtomicBool's
    // on default (harmless when the setting is already on, but needed for a
    // session where the user previously turned it off). NOT non-blocking:
    // Client::set_msc2545_legacy_compat() synchronously rebuilds the image-pack
    // cache (a per-room network-bound fetch) before returning, so this must
    // only ever be called from a background thread (restore_all_accounts_
    // blocking_ / finalize_login_blocking_), never the UI thread — confirmed
    // by a ~2.6s stall measured on the UI thread before this was moved.
    void apply_msc2545_legacy_compat_pref_(tesseract::Client& client);

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
    // Close & destroy every owned pop-out window (each ~RoomWindowBase runs
    // unregister_room_window_ + release_room_subscription_) and forget them
    // permanently: drop the Settings::popout_windows entries and clear
    // pending_restore_popouts_ so nothing reopens. Used by the "Clear all
    // caches" reset. UI thread only.
    void close_all_popouts_();

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
    // anim → image → thumbnail fallthrough. Used by every RoomPane's
    // img_viewer_/vid_viewer_ image_provider (main window and pop-outs
    // alike), via RoomPane::shell_image_.
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

    // Compressed-bytes cache (L1) in front of media_disk_cache_ (L2), keyed by
    // the same disk-cache key. Safe to call from the media io pool
    // (compressed_cache() is internally synchronised; each media_disk_cache_ op
    // is filesystem-atomic on a distinct key). Every media fetch/decode path
    // goes through these instead of touching media_disk_cache_ directly.
    //   load: L1 hit → return it; else disk read, populating L1 on a disk hit.
    //   store: write both tiers.  evict: drop from both tiers.
    std::vector<std::uint8_t> load_media_bytes_(const std::string& key) const;
    void store_media_bytes_(const std::string& key,
                            const std::vector<std::uint8_t>& bytes) const;
    void evict_media_bytes_(const std::string& key) const;

    // Fetch a server-scaled thumbnail (w×h) for an inline media preview into
    // thumbnail_cache_ (or anim_cache_ if it decodes animated). Mirrors
    // ensure_media_image_ but uses the /thumbnail endpoint; the in-flight and
    // media_disk_cache_ keys are size-namespaced via thumb_key() so a thumbnail
    // and a full-size fetch of the same mxc never collide. `animated` requests
    // an animated thumbnail where the server supports it (MSC2705).
    void ensure_media_thumbnail_(const std::string& url, int w, int h,
                                 bool animated, std::uint64_t group_id = 0);

    // Size-namespaced cache key for thumbnail fetches (disk + in-flight set).
    // Canonical format lives in tesseract::visual::thumb_key (also used by
    // the desktop search D-Bus adapters, which aren't ShellBase subclasses).
    static std::string thumb_key(const std::string& key, int w, int h)
    {
        return tesseract::visual::thumb_key(key, w, h);
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
    // (RoomPane::wire_room_view_ owns those, via main_room_pane_) nor
    // non-provider callbacks (on_room_selected, on_scroll, on_search_clear,
    // etc.) — those touch shell-specific state and stay in the per-shell ctor.
    void wire_main_app_widget_(views::MainAppWidget* app);

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

    // ensure_reply_details_() only ever resolves a reply preview against
    // whatever's locally loaded (or reachable over the network) at the
    // moment it's called, and reply_details_requested_ dedups it to at most
    // one attempt per event_id for the rest of the session — matrix-sdk-ui
    // never re-resolves an in-reply-to preview on its own once that attempt
    // has run (see InReplyToDetails::new / fetch_in_reply_to_details
    // upstream). So if the quoted event wasn't loaded yet the first time
    // (or the one-shot fetch otherwise came back empty), the quote block is
    // stuck showing the "unavailable" placeholder forever, even after the
    // quoted message itself later scrolls into view via backward
    // pagination. Call this whenever `new_event_ids` lands in the current
    // room's main timeline (live insert/append or pagination prepend): any
    // already-rendered, still-unresolved reply row whose target is now
    // among them gets its dedup entry cleared and a fresh fetch reissued.
    void retry_stale_reply_previews_(const std::vector<std::string>& new_event_ids);

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
    // For a backward-pagination batch (prepend) / live tail-append batch —
    // unlike dispatch_message_inserted_secondary_ above, these carry no SDK
    // index at all (position is always "front"/"back" of what's currently
    // displayed), so there's no index parameter to plumb through.
    void dispatch_message_prepended_secondary_(const std::string& room_id,
                                               const Event& ev);
    void dispatch_message_appended_secondary_(const std::string& room_id,
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

    // Update the my_knocks_ cache and call on_my_knocks_updated_() for the
    // active account. Mirrors push_invites_.
    void push_my_knocks_(std::string user_id, std::vector<KnockedRoomInfo> knocks);

    // Return a pointer to the KnockedRoomInfo for room_id, or nullptr when
    // not found.
    const KnockedRoomInfo* find_my_knock_(const std::string& room_id) const;

    // Send a knock (MSC2403) request for room_id_or_alias with an optional
    // reason. Dispatches on a worker thread; result delivered via
    // handle_room_action_complete_ui_ (RoomActionKind::Knock).
    // `via` supplies extra routing server names (a permalink's `?via=`); when
    // empty, join_room_command_ / knock_room_command_ fall back to any hints
    // stashed by open_matrix_link() for this room id/alias.
    void knock_room_command_(const std::string& room_id_or_alias,
                             const std::string& reason,
                             std::vector<std::string> via = {});

    // Retract a pending knock — just leave_room_command_ under the hood,
    // since Room::leave() already handles the Knocked membership state.
    void retract_knock_command_(const std::string& room_id);

    // Subscribe/unsubscribe the admin-side KnockRequestsPanel to room_id's
    // live knock-request watcher. Called on panel open/close and room
    // switch; safe to call unsubscribe when nothing is subscribed.
    void subscribe_knock_requests_panel_(const std::string& room_id);
    void unsubscribe_knock_requests_panel_();

    // Accept / decline / decline-and-ban a pending knock request
    // asynchronously, mirroring accept_invite_async_/decline_invite_async_.
    // accept has no navigation (the admin isn't joining anything); decline
    // and decline-and-ban remove the request from
    // current_room_knock_requests_ immediately (optimistic UI).
    void accept_knock_request_async_(const std::string& room_id,
                                     const std::string& user_id);
    void decline_knock_request_async_(const std::string& room_id,
                                      const std::string& user_id);
    void decline_and_ban_knock_request_async_(const std::string& room_id,
                                              const std::string& user_id,
                                              const std::string& reason);

    // Slash-command async handlers — called from dispatch_room_send_ after the
    // command prefix is identified. Each enqueues async SDK work, so they must
    // run on the UI thread.
    void leave_room_command_(const std::string& room_id);
    void join_room_command_(const std::string& room_id_or_alias,
                            std::vector<std::string> via = {});
    void invite_user_command_(const std::string& room_id,
                              const std::string& user_id,
                              const std::string& reason = "");

    // AddRoomView's Join tab: async MSC3266 room summary lookup (no async
    // get_room_summary exists, so this dispatches the blocking call on a
    // worker thread itself, guarded by join_room_lookup_gen_). Populates
    // main_app_->add_room_view()->join_view() with the result.
    void lookup_room_command_(const std::string& room_id_or_alias);

    // AddRoomView's Create tab: dispatches Client::create_room_async;
    // outcome delivered via on_create_room_outcome_ui_.
    void create_room_command_(const RoomCreateOptions& options);

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

    // Async: delete all on-disk caches best-effort (media files, waveform DB),
    // clear in-memory image maps, reinit the waveform store, then hand off to
    // restart_sdk_begin_() for the full SDK wipe + in-place re-restore and UI
    // rebuild, which calls recompute_callback with fresh sizes when it lands.
    // Refuses (status message, no-op) while a call or device-verification is in
    // flight. No-op when not signed in.
    void clear_all_caches_(
        std::function<void(uint64_t local, uint64_t sdk, uint64_t memory,
                           uint64_t mem_hits, uint64_t mem_misses,
                           uint64_t disk_hits, uint64_t disk_misses)>
            recompute_callback);

    // "Clear all caches" reset, modelled on a logout+login: tear down the
    // account's whole UI (close pop-outs, forget the tab layout, empty the room
    // list and per-account caches) on the UI thread, then on mut_pool_ run the
    // blocking SDK sequence (push an empty tab layout, stop_sync, clear_caches,
    // restore_session [retried once], start_sync), then rebuild the UI via
    // refresh_account_ui_after_switch_() and recompute the cache sizes. Must be
    // called on the UI thread. No-op when not signed in or the session JSON is
    // missing.
    void restart_sdk_begin_(
        std::function<void(uint64_t local, uint64_t sdk, uint64_t memory,
                           uint64_t mem_hits, uint64_t mem_misses,
                           uint64_t disk_hits, uint64_t disk_misses)>
            recompute_callback);

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

    // Invoked by each shell's native status-bar click handling when a click
    // lands on the bar but not on a hyperlink segment. Public (unlike
    // on_persistent_status_activate_ itself) because that click handling is
    // typically a free function tied to a native window class, not a
    // ShellBase member — see e.g. MainWindow.cpp's status_bar_wnd_proc. A
    // no-op when nothing currently claims the persistent-status slot.
    void trigger_persistent_status_click_()
    {
        if (on_persistent_status_activate_)
        {
            auto cb = on_persistent_status_activate_;
            cb();
        }
    }

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

    // ── Find-in-thread search (ThreadView's own search bar) ──────────────
    // Deliberately not a generalization of the in-room search machinery
    // above: threads have no equivalent "paginate older history while
    // searching" concept (a thread's messages are already fully loaded via
    // subscribe_thread, not lazily backfilled like the main room), and
    // thread panels never appear in popout windows — so this is a smaller,
    // self-contained state set rather than a third mode threaded through
    // every in_room_search_* function above.
    void handle_thread_search_query_(const std::string& query);
    void handle_thread_search_results_ui_(std::uint64_t request_id,
                                          std::vector<tesseract::SearchHit> results);
    void handle_thread_search_failed_ui_(std::uint64_t request_id,
                                         const std::string& message);
    void thread_search_navigate_(int delta);
    void thread_search_focus_current_();
    void thread_search_apply_highlights_();
    void thread_search_clear_();
    // Returns the open thread's search bar, or nullptr when unavailable.
    views::RoomSearchBar* thread_search_bar_() const;

    std::uint64_t thread_search_request_id_ = 0;
    std::unordered_map<std::uint64_t, std::string> thread_search_pending_;
    std::vector<tesseract::SearchHit> thread_search_matches_;
    int thread_search_current_ = -1;

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
    // several backend round-trips in a media-sparse room. Opening/closing,
    // pagination, and retry/accumulate state all live on RoomPane now
    // (RoomPane::open_room_media_view_ etc.) — used identically by the main
    // window's main_room_pane_ and every pop-out's own pane_, so this class
    // only needs the one thing a per-pane object structurally can't provide
    // itself: routing IEventHandler::on_media_view_paginate_result (which
    // has no per-window addressing of its own) back to whichever RoomPane
    // actually issued the request.
    //
    // Completion callback for paginate_media_view_back_async. Looks up
    // media_view_paginate_owners_ and forwards to the owning RoomPane's
    // handle_media_view_paginate_result_, which decides whether to fire
    // another round based on an authoritative Image/Video count read
    // directly from the SDK's timeline — see RoomPane.cpp and
    // paginate_media_view_back_async's doc comment for why this replaced an
    // earlier design that raced against the separate diff-streaming task.
    void handle_media_view_paginate_result_ui_(std::uint64_t request_id,
                                               bool ok, bool reached_start,
                                               std::uint64_t media_count,
                                               std::string message);

    // Completion callback for load_room_media_page_async. Shares the
    // media_view_paginate_owners_ map + next_paginate_id_ id space with the
    // network path, but a DB-page request is NEVER entered in
    // pending_paginates_ — the two completion paths stay strictly separate so
    // a DB request id can't be misrouted into the network retry loop.
    void handle_room_media_page_ui_(std::uint64_t request_id,
                                    std::vector<tesseract::MediaIndexRow> rows,
                                    bool reached_db_end, std::uint64_t total);

    // Per-gesture safety cap, not the primary stop condition — that's
    // reached_start / kMediaViewMinTotal (see RoomPane::
    // handle_media_view_paginate_result_). A media-sparse-relative-to-
    // volume room can legitimately need far more than a handful of rounds;
    // this cap exists purely to bound a single gesture against a
    // pathological room that never reports reached_start (e.g. an SDK-side
    // edge case), not to limit normal "keep going until there's enough to
    // show" behavior.
    static constexpr int kMediaViewMaxRetries = 200; // 200*kPaginationBatch raw events/gesture
    // Fallback floor used only when RoomMediaView::estimated_capacity()
    // reports 0 — i.e. the widget hasn't been arranged with a real viewport
    // yet (its very first open() this session; see that method's doc
    // comment). Once real geometry is known, the actual target is
    // estimated_capacity() itself (the widget's true grid capacity), not
    // this constant — using a fixed small number as the *real* target was
    // the bug: it stopped pagination as soon as a handful of items turned
    // up, long before the real (often much larger) viewport was full.
    static constexpr std::uint64_t kMediaViewMinTotal = 6;
    // Max allowed gap between RoomPane's own media_view_known_media_count_
    // and the gallery widget's actually-rendered item_count() before the
    // automatic chain pauses firing further rounds. Keeps the slower
    // diff-streaming task (see paginate_media_view_back_async's doc comment
    // — it does per-event async work: sender-profile resolution,
    // formatting, receipts, search indexing, far more than a pagination
    // round's often-local-store-only work) from falling arbitrarily far
    // behind a fast run of rounds. Without this, dozens of rounds' worth of
    // raw events pile up unconverted, rendering looks stalled, then
    // everything appears at once ("huge bunch") once it drains.
    static constexpr std::uint64_t kMediaViewMaxRenderGap = 24;
    // Safety-net upper bound on how long a paused (render-gap-limited) chain
    // waits for RoomPane::feed_gallery_live_/feed_gallery_prepend_batch_ to
    // signal catch-up before firing anyway. Should essentially never trigger
    // in practice — the streaming task always makes forward progress — but
    // bounds worst-case latency against any edge case that stalls rendering
    // entirely.
    static constexpr int kMediaViewPauseFallbackMs = 3000;
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
