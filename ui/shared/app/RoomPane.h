#pragma once
#include "app/ThreadPanelController.h"
#include "views/GifController.h"
#include "views/MentionController.h"
#include "views/MessageListView.h"
#include "views/RoomView.h"
#include "views/ShortcodeController.h"
#include "views/SlashCommandController.h"
#include "tk/host.h"
#include "tk/weak_self.h"
#include <tesseract/image_pack.h>
#include <tesseract/mentions.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tesseract::views
{
class ImageViewerOverlay;
class VideoViewerOverlay;
class ForwardRoomPicker;
class RoomMediaView;
} // namespace tesseract::views

namespace tesseract
{

class ShellBase;
class Client;

// RoomPane owns the per-room display state that both the main window's
// currently-shown room and every pop-out room window need: the RoomView
// wiring, compose/send/edit/react/pin SDK operations, the thread panel, and
// the popup-hooks/geometry helpers shared by the four composer autocomplete
// controllers (mention/slash/shortcode/gif). It is held by composition, not
// inheritance — RoomWindowBase (pop-outs) and ShellBase (the main window)
// each hold one RoomPane and inject their platform-bound behavior (repaint,
// relayout, clipboard/toast/encode-for-send) via Deps at construction.
//
// Unlike RoomWindowBase (which owns exactly one room for its whole
// lifetime), RoomPane supports retarget() so the main window can swap which
// room it displays without reconstructing the pane. Pop-outs simply never
// call retarget().
class RoomPane : public tk::EnableWeakSelf<RoomPane>
{
    // tests/cpp/test_shell_media_view_pagination.cpp: exposes exactly the
    // private gallery-pagination state that test suite pokes directly,
    // mirroring the ShellBase test doubles' `using ShellBase::field;`
    // pattern elsewhere (unavailable here since RoomPane is held by
    // composition, not inherited).
    friend struct RoomPaneMediaViewTestAccess;
    // tests/cpp/test_shell_send_sticker.cpp: exposes the private
    // thread-panel state (thread_panel_/thread_root_/room_view_) that suite
    // pokes directly to exercise send_sticker_'s thread-routing branch.
    friend struct RoomPaneStickerTestAccess;
    // tests/cpp/test_shell_reply_retry.cpp: exposes the private room_view_
    // that suite pokes directly to exercise ensure_reply_details_/
    // retry_stale_reply_previews_ against a real MessageListView.
    friend struct RoomPaneReplyRetryTestAccess;

public:
    // Platform-bound behavior, injected once at construction. Every
    // subclass/shell already implements the underlying primitives
    // (ShellBase::request_repaint_/request_relayout_, tk::Host's
    // clipboard/toast/encode methods) — Deps just routes them here instead
    // of through virtual dispatch, since RoomPane is concrete.
    struct Deps
    {
        ShellBase* shell = nullptr;
        tk::Host* host = nullptr; // encode_for_send/clipboard/toast/popup-surface
        std::function<void()> repaint;
        std::function<void()> relayout;
        // Grab keyboard focus on this pane's surface after opening the
        // image/video lightbox (Qt's setFocus(), GTK's
        // gtk_widget_grab_focus(), etc.) so key handling — Escape to close,
        // arrow-key navigation — reaches the surface instead of whatever
        // previously had focus (e.g. the compose text area). Only the main
        // window wires this (each platform's MainWindow/MainWindowController
        // constructor); pop-outs leave it at the no-op default, matching
        // their pre-existing behavior of never grabbing focus on lightbox
        // open.
        std::function<void()> grab_surface_focus = [] {};
        // Only meaningful for pop-out window chrome (native title bar). The
        // main window updates its own OS window title directly from each
        // platform's onRoomSelected/on_rooms_updated_/on_left_room handlers
        // instead of through this callback (RoomPane never calls it for the
        // main window — finish_init() and on_room_info_updated(), the only
        // call sites, are pop-out-only paths), so its Deps leaves this at
        // the no-op default.
        std::function<void(const std::string&)> update_window_title =
            [](const std::string&) {};
        // Called with the room_id when the user successfully leaves it via
        // this pane's RoomView. Pop-outs set this to schedule_self_close_()
        // (RoomWindowBase-owned, since closing a whole native window is a
        // pop-out-only concept). The main window sets this to clear its
        // currently-displayed room/selection when the room left is the one
        // it's showing — that bookkeeping is main-window-only (pop-outs have
        // no "currently selected room" concept to clear), which is why it's
        // injected here rather than handled inside RoomPane itself.
        std::function<void(const std::string&)> on_left_room =
            [](const std::string&) {};
        // Called every time this pane issues an in-room search query.
        // ShellBase::in_room_search_active_win_ (a RoomWindowBase*, used to
        // relayout a pop-out's own surface when a search result lands) is a
        // pop-out-only concept RoomPane has no business setting directly —
        // pop-outs set this to `[this]{ shell_->in_room_search_active_win_ =
        // this; }`; the main window leaves it at the no-op default, matching
        // its existing behavior of never setting that field.
        std::function<void()> on_search_activated = [] {};
        // Put the hosting native window into / out of OS full-screen, driven by
        // the image/video lightbox full-screen toggle. The main window wires
        // this to ShellBase::set_window_fullscreen_ (each platform's
        // MainWindow/MainWindowController constructor). Pop-outs leave it at
        // the no-op default and wire the overlay's on_request_fullscreen
        // directly to their own native window instead (closing a pop-out over
        // a full-screen toggle is a per-window concern).
        std::function<void(bool)> set_window_fullscreen = [](bool) {};
    };

    // Borrowed widget-tree pointers, set once via attach() right after the
    // caller builds its widget tree. Mirrors RoomWindowBase's previous
    // pattern of the subclass assigning room_view_/img_viewer_/vid_viewer_
    // directly before calling wire_room_view_()/finish_init_().
    struct Widgets
    {
        views::RoomView* room_view = nullptr; // required
        views::ImageViewerOverlay* img_viewer = nullptr;
        views::VideoViewerOverlay* vid_viewer = nullptr;
        views::ForwardRoomPicker* forward_picker = nullptr;
        views::RoomMediaView* room_media_view = nullptr;
        std::function<void()> focus_forward_picker_field = [] {};
        std::function<void()> hide_forward_picker_field = [] {};
    };

    RoomPane(Deps deps, std::string room_id);
    ~RoomPane();

    // Populate widgets_ and wire the shared RoomView providers/callbacks.
    // Call once, after the caller's widget tree exists.
    void attach(Widgets w);

    // Register with shell_, acquire the initial room state, and seed the
    // view. Pop-outs call this once, right after attach(), mirroring the old
    // RoomWindowBase::finish_init_(). The main window does NOT call this —
    // ShellBase already seeds room_view_ itself via the existing room-switch
    // path, and finish_init()'s registry/subscription side-effects are
    // pop-out-only concepts owned by RoomWindowBase, not RoomPane.
    void finish_init();

    // Point this pane at a different room without reconstructing it — the
    // main window's tab-switch hook. Deliberately minimal: see RoomPane.cpp
    // for why a full re-seed isn't needed.
    void retarget(const std::string& new_room_id);

    // ── Per-room compose drafts ──────────────────────────────────────────
    // Unsent compose-bar content (text + at most one staged attachment)
    // stashed when the user navigates away from a room, keyed by room id
    // (not tab index — survives a tab being closed/reopened or the room
    // being reached via a plain room-list click with no tab involved).
    // In-memory only; not persisted to disk or account data. Only ever
    // populated on the main window's pane (main_room_pane_) — a pop-out
    // never retarget()s, so it never needs a draft stashed/restored.
    //
    // Snapshot compose_bar()'s current text + pending attachment into this
    // pane's own draft map (erasing any existing entry if there is nothing
    // to save), and remove the attachment from the live widget. Call with
    // the OLD room id before leaving it.
    void save_compose_draft_(const std::string& room_id);
    // Re-apply a previously saved draft for room_id into compose_bar(), if
    // one exists. No-op (leaves the widget in its current, cleared state)
    // when nothing was saved for this room. Call with the NEW room id after
    // set_room()/retarget() — and after the caller's own unconditional
    // compose-bar reset (clear_reply/clear_editing/clear_compose_text),
    // which this does NOT do itself (each platform shell still owns that
    // reset choreography, which has platform-specific extras — e.g. Win32's
    // deferred focus-on-show, macOS's typing-text reset — this only handles
    // the "is there a draft to restore" half).
    void apply_compose_draft_(const std::string& room_id);
    // Drop every saved draft — call on account switch/logout.
    void clear_compose_drafts_() { room_compose_drafts_.clear(); }

    const std::string& room_id() const { return room_id_; }
    views::RoomView* room_view() const { return room_view_; }

    // Number of rows currently withheld from room_view_ by the
    // kSwitchDisplayCap trim (see withheld_older_rows_ below). Live SDK
    // update/insert/remove indices are relative to the full, untrimmed
    // timeline, so a caller forwarding one of those straight to room_view_
    // must first subtract this to land on the right row.
    std::size_t withheld_count() const { return withheld_older_rows_.size(); }

    // A weak handle to this pane itself. .lock() returns the live RoomPane*,
    // or nullptr once this pane is gone.
    using tk::EnableWeakSelf<RoomPane>::weak_self;

    // Wraps fn so it only runs while this pane is still alive. Exposed
    // publicly (guarded() is otherwise protected) so external owners — a
    // platform pop-out window whose own lifetime tracks this pane's, e.g.
    // the GIF strip's image provider — can guard a callback that touches
    // their own state without duplicating a liveness token of their own.
    using tk::EnableWeakSelf<RoomPane>::guarded;

    // Push this pane's own room's pinned_events + can_pin bit, and the
    // redact-others (delete-others'-messages) permission, to room_view_,
    // looking up the RoomInfo in shell_->rooms_. Called by the owner (e.g.
    // ShellBase::push_rooms_/after_active_room_changed_ for the main
    // window's pane) on every sync tick and room switch. When the room is
    // not yet in the cache, clears all of them so the banner hides and the
    // delete-others affordance disappears.
    void refresh_pinned_for_current_room_();

    // Called by the owner (ShellBase/RoomWindowBase) on the UI thread when
    // SDK events arrive for this pane's current room.
    void on_room_info_updated(const RoomInfo& r);
    // Returns true if this reset was treated as a room switch (first
    // display of this room, or a re-population of a previously-emptied
    // view — e.g. logout -> login into the same room). Callers that need
    // extra room-switch-only bookkeeping (pagination/scroll restore) branch
    // on this.
    bool on_timeline_reset(std::vector<views::MessageRowData> rows);
    // idx is a genuine SDK live-Insert-diff index (relative to the full,
    // untrimmed timeline) — always translated past any withheld tail (see
    // withheld_count()). Backward-pagination prepends and live tail
    // appends are NOT expressed through this — they carry no real SDK
    // index at all, and go through on_message_prepended()/
    // on_message_appended() below instead.
    void on_message_inserted(std::size_t idx, views::MessageRowData row);
    // Insert at the front of what this pane currently displays (a
    // backward-pagination batch, delivered one row at a time).
    void on_message_prepended(views::MessageRowData row);
    // Insert at the end of what this pane currently displays (a live
    // tail-append batch, delivered one row at a time).
    void on_message_appended(views::MessageRowData row);
    void on_message_updated(std::size_t idx, views::MessageRowData row);
    void on_message_removed(std::size_t idx);
    void on_typing_changed(const std::string& text, bool visible);

    // Thread view delivery — called by the owner when SDK events arrive for
    // the thread this pane has open (thread_root() matches).
    const std::string& thread_root() const { return thread_root_; }
    ThreadPanelController::ThreadPanel thread_panel() const { return thread_panel_; }
    ThreadPanelController::ThreadPanel thread_panel_prev() const { return thread_panel_prev_; }
    // Reset the thread-list backfill guards — called by the owner on every
    // room switch, since each new room starts with an unknown thread history.
    void reset_thread_backfill() { thread_ctl_.reset_backfill(); }
    void apply_thread_reset_(std::vector<views::MessageRowData> rows);
    void apply_thread_prepend_(std::vector<views::MessageRowData> rows);
    void apply_thread_append_(std::vector<views::MessageRowData> rows);
    void apply_thread_insert_(std::size_t index, views::MessageRowData row);
    void apply_thread_update_(std::size_t index, views::MessageRowData row);
    void apply_thread_remove_(std::size_t index);
    // Apply the side-effects of a thread-panel transition: subscribe/
    // unsubscribe on the client, update thread_panel_ state, drive
    // room_view_->set_thread_panel(), and refresh the thread-list snapshot
    // when entering List mode.
    void apply_thread_transition_(const ThreadPanelController::ThreadTransition& t);
    // Kick a thread-list pagination pass using this pane's own controller.
    void paginate_threads_();

    // ── MSC3030 (focused-timeline / jump-to-date) ────────────────────────
    // begin/request/return operate on shell_->pagination_[room_id] — shared
    // per-room state, not this pane's own — so room_id need not be this
    // pane's own room_id_ (mirrors ensure_reply_details_'s general form:
    // any live RoomPane can serve as the call's entry point for a room
    // other than the one it displays, e.g. ShellBase's room-media-gallery
    // pagination, global-search jump-to-result, and permalink navigation
    // all route through main_room_pane_ this way).
    void begin_focused_subscription_(const std::string& room_id,
                                     const std::string& event_id);
    // Resolve ts_ms to an event in this pane's own room and begin a focused
    // subscription centred on it.
    void handle_date_jump_(std::uint64_t ts_ms);
    // General form, for a room other than this pane's own.
    void handle_date_jump_(const std::string& room_id, std::uint64_t ts_ms);
    // Paginate forward in room_id's focused timeline; switches to live when done.
    void request_forward_history_(const std::string& room_id);
    // Tear down room_id's focused state and re-subscribe live.
    void return_to_live_(const std::string& room_id);

    // Fire a synchronous SDK call to fetch reply-to metadata for event_id in
    // this pane's own room_id_ main timeline.
    void ensure_reply_details_(const std::string& event_id);
    // General form: room_id/thread_root need not be this pane's own room_id_
    // — thread_root non-empty resolves within that thread's own timeline
    // instead, and room_id can be any room (e.g. when building rows destined
    // for a different pane's room, or a pinned-open gallery on a room this
    // pane no longer displays). Only touches shared ShellBase state
    // (shell_->client_/shell_->reply_details_requested_), never this pane's
    // own room_id_, so any live RoomPane can serve as the call's entry point.
    void ensure_reply_details_(const std::string& room_id,
                               const std::string& event_id,
                               const std::string& thread_root);

    // ensure_reply_details_() only ever resolves a reply preview against
    // whatever's locally loaded (or reachable over the network) at the
    // moment it's called, and shell_->reply_details_requested_ dedups it to
    // at most one attempt per event_id for the rest of the session —
    // matrix-sdk-ui never re-resolves an in-reply-to preview on its own once
    // that attempt has run (see InReplyToDetails::new /
    // fetch_in_reply_to_details upstream). So if the quoted event wasn't
    // loaded yet the first time (or the one-shot fetch otherwise came back
    // empty), the quote block is stuck showing the "unavailable" placeholder
    // forever, even after the quoted message itself later scrolls into view
    // via backward pagination. Call this whenever `new_event_ids` lands in
    // this pane's own room's main timeline (live insert/append or pagination
    // prepend): any already-rendered, still-unresolved reply row whose
    // target is now among them gets its dedup entry cleared and a fresh
    // fetch reissued.
    void retry_stale_reply_previews_(const std::vector<std::string>& new_event_ids);
    // General form of the above: re-scans an arbitrary MessageListView (a
    // thread's own embedded list, or another pane's) instead of always this
    // pane's own room_view_->message_list(). thread_root is forwarded to
    // ensure_reply_details_ so the retried fetch resolves against the right
    // timeline.
    void retry_stale_reply_previews_(views::MessageListView* list,
                                     const std::string& room_id,
                                     const std::string& thread_root,
                                     const std::vector<std::string>& new_event_ids);

    // Resolve event_id's reply-to target within this pane's currently-open
    // thread (no-op if no thread is open). Delegates to this pane's own
    // ensure_reply_details_ general form with this pane's own room_id_/
    // thread_root_/room_view_ instead of the main window's
    // current_room_id_/room_view_ — lets a pop-out's own thread panel share
    // the same dedup-guarded fetch.
    void ensure_thread_reply_details_(const std::string& event_id);
    // The thread root is also an ordinary row in the main timeline, which
    // very likely already resolved its own reply-quote there (the user
    // typically saw it before ever opening the thread) — matrix-sdk-ui's
    // thread-focused fetch_details_for_event can't resolve a target outside
    // the thread, so re-fetching from the thread's own timeline never
    // completes. Copies the already-resolved in_reply_to_* fields from the
    // main list's copy of thread_root_ into the thread list's copy instead
    // of fetching. Returns true if it found a resolved copy to use (no-op,
    // returns false, if no thread is open, the root isn't in the main list,
    // it's unresolved there too, or the thread's copy is already resolved).
    bool sync_thread_root_reply_from_main_list_();
    // thread_root_ when a thread panel is open and has a root (so a send
    // should land in the thread), else empty (so it lands in the room).
    // Shared by every composer send path — sticker, image, video, audio,
    // file — that takes an optional trailing thread_root the same way
    // send_thread_sticker/send_image_async/etc. do.
    const std::string& active_thread_root_for_send_() const
    {
        static const std::string kEmpty;
        return (thread_panel_ == ThreadPanel::Open && !thread_root_.empty())
                   ? thread_root_
                   : kEmpty;
    }
    // Re-check this pane's open thread for already-rendered reply rows whose
    // target is among new_event_ids (no-op if no thread is open).
    void retry_stale_thread_reply_previews_(
        const std::vector<std::string>& new_event_ids);

    // Fan-in for async message-forward completions — the owner's forward_event
    // request_id is process-global, so it checks every open pane until one
    // recognizes the id. Returns true if this pane issued request_id.
    bool handle_forward_done_(std::uint64_t request_id);
    bool handle_forward_failed_(std::uint64_t request_id,
                                const std::string& message);

    // ── Popup-hooks helpers ─────────────────────────────────────────────
    // Fill every Hooks field derivable from shell/room state. The caller
    // still builds show/hide/repaint (and on_selfie for mention/slash, which
    // needs a main-window-only overlay) locally, since the popup surface
    // handle type stays per-platform/per-window.
    void wire_mention_hooks_(views::MentionPopup* popup,
                             views::MentionController::Hooks& hooks);
    void wire_slash_hooks_(views::SlashCommandController::Hooks& hooks);
    void wire_shortcode_hooks_(views::ShortcodePopup* popup,
                               views::ShortcodeController::Hooks& hooks);
    void wire_gif_hooks_(views::GifController::Hooks& hooks);

    // Position + show a "row list" composer popup (mention/slash/shortcode)
    // anchored at the text cursor. Replaces every show_mention_popup_/
    // show_slash_popup_/show_shortcode_popup_ body across all platforms.
    static void position_dropdown_popup_(tk::PopupSurfaceHandle* popup,
                                         tk::Rect cursor_local, int rows,
                                         float row_height, float width);

    // Build the outgoing (body, formatted_body) pair from the live compose
    // text area's mention/emoticon draft. Falls back to {fallback_body, ""}
    // when there is no text area or its draft is empty.
    tesseract::MarkdownResult draft_outgoing_message_(
        const std::string& fallback_body);

    // Look up a cached URL-preview card for `url`, or nullptr.
    const views::UrlPreviewData* preview_lookup_(const std::string& url);

    // SDK operation helpers — forward to shell_->client_. All must be called
    // on the UI thread.
    tesseract::Client* shell_client_() const;

    void send_message_(const std::string& body);
    void send_message_(const std::string& body,
                       const std::string& formatted_body);
    void send_reply_(const std::string& reply_event_id,
                     const std::string& body);
    void send_sticker_(const std::string& body, const std::string& image_url,
                       const std::string& info_json);
    void send_current_location_();
    void send_edit_(const std::string& event_id, const std::string& new_body,
                    bool is_caption = false);
    void delete_event_(const std::string& event_id);
    void toggle_reaction_(const std::string& event_id, const std::string& key,
                          const std::string& source_mxc);
    void send_receipt_(const std::string& event_id);
    void send_typing_notice_(bool typing);
    void retry_send_(const std::string& txn_id);
    void abort_send_(const std::string& txn_id);
    void pin_event_(const std::string& event_id);
    void unpin_event_(const std::string& event_id);
    // Resolve/create a DM with user_id, then open (or focus) a SEPARATE
    // window for it — this pane's own room_id_ stays put.
    void open_dm_(std::string user_id);
    // Open/close this pane's room-media gallery for its own room_id_.
    void open_room_media_view_();
    void close_room_media_view_();
    void request_pagination_back_();
    // Load an older batch of the currently-open thread's messages. Wired to
    // the embedded MessageListView's on_near_top (see apply_thread_transition_)
    // — subscribe_thread only ever fetches the newest kPaginationBatch replies
    // itself, so this is the only way to reach anything older.
    void paginate_thread_back_();

    // ── Gallery pagination ───────────────────────────────────────────────
    // Ported from ShellBase so pop-outs and the main window share one
    // implementation: fires the dedicated paginate_media_view_back_async
    // SDK call and automatically retries/accumulates until the gallery's
    // viewport is actually filled (or reached_start / kMediaViewMaxRetries),
    // paced against the separate diff-streaming delivery task so it doesn't
    // race ahead of what's actually rendered (see kMediaViewMaxRenderGap).
    void request_media_view_pagination_back_();
    // Wired to the gallery widget's own scroll-to-top trigger.
    void on_media_view_load_older_(const std::string& room_id);
    // Completion callback for a paginate_media_view_back_async request this
    // pane issued — routed here by
    // ShellBase::handle_media_view_paginate_result_ui_, which owns the
    // request_id -> RoomPane* correlation since the SDK callback has no
    // per-window addressing of its own.
    void handle_media_view_paginate_result_(std::uint64_t request_id, bool ok,
                                            bool reached_start,
                                            std::uint64_t media_count);
    // Completion callback for a load_room_media_page_async request this pane
    // issued (persistent media index, no network). Routed here by
    // ShellBase::handle_room_media_page_ui_. Seeds / extends the gallery from
    // the DB; only once the index is drained does paging fall through to the
    // network retry loop above.
    void handle_room_media_page_(std::uint64_t request_id,
                                 std::vector<tesseract::MediaIndexRow> rows,
                                 bool reached_db_end, std::uint64_t total);
    // Issue the next gallery page: from the persistent index while it still
    // has older rows, otherwise (index drained + history not exhausted) a
    // network paginate_media_view_back_async round.
    void request_media_view_next_page_();
    // Resumes an automatic fill round that was deferred because rendering
    // had fallen too far behind the authoritative SDK-side count. Called
    // after this pane's gallery actually renders more rows.
    void maybe_resume_media_view_pagination_(bool force);
    // Feeds a newly-arrived (or single-event backward-paginated) Image/Video
    // row into this pane's gallery if it's currently open for
    // event_room_id — checked independently of this pane's own
    // room_id_/retarget state, so the main window's gallery can stay pinned
    // open on a room the user has since navigated away from. `prepend`
    // distinguishes a backward-pagination arrival (insert at the head) from
    // a genuinely new live message (append at the tail).
    void feed_gallery_live_(const std::string& event_room_id,
                            views::MessageRowData row, bool prepend);
    // Batched counterpart of feed_gallery_live_(..., /*prepend=*/true), for
    // callers (the main window's handle_messages_prepended_ui_) that already
    // have a whole backward-pagination batch instead of per-event delivery.
    void feed_gallery_prepend_batch_(const std::string& event_room_id,
                                     std::vector<views::MessageRowData> rows);
    // Replaces the whole known set (e.g. a reconnect re-subscribe delivering
    // a fresh timeline reset), if this pane's gallery is currently open for
    // event_room_id.
    void feed_gallery_reset_(const std::string& event_room_id,
                             std::vector<views::MessageRowData> rows);
    // Read-only: the room the currently-open gallery is showing (empty if
    // closed). Lets ShellBase's tab-switch handler decide whether to
    // auto-close a gallery left open on a room the user is navigating away
    // from, without needing friend access to RoomPane's private state.
    const std::string& media_view_room_id() const { return media_view_room_id_; }

    // Image cache accessors — friend access to ShellBase protected members.
    const tk::Image* shell_avatar_(const std::string& mxc) const;
    const tk::Image* shell_image_(const std::string& mxc) const;
    // Resolves a room member's avatar for a received or restored mention
    // pill: user id -> cached_room_members_' avatar mxc -> cached thumbnail,
    // kicking a fetch on miss. Shared by set_mention_avatar_provider's
    // lambda (wire_room_view_) and apply_compose_draft_'s pill replay, so
    // a restored composer mention shows the same avatar a received one
    // would.
    const tk::Image* mention_avatar_for_user_(const std::string& user_id) const;
    void shell_show_status_message_(std::string msg, int auto_clear_ms = 4000);

    // This pane's own shortcode-popup suggestion source (personal + this
    // pane's own room_id_ + subscribed rooms).
    std::vector<tesseract::ImagePackImage> shell_emoticons_() const;
    // Every Space (direct and ancestor) that this pane's room_id_ is in.
    std::vector<std::string> shell_parent_spaces_for_room_() const;
    // Trigger an async fetch+decode of a media image into the shell cache.
    void shell_ensure_media_image_(const std::string& url, int w, int h);

    // Render one GIF strip cell via the shell's backend-specific provider.
    const tk::Image* shell_gif_strip_image_(const GifResult& result,
                                            const std::function<void()>& repaint);
    // Source bytes the GIF strip cached on fetch.
    std::vector<std::uint8_t> shell_cached_gif_bytes_(const std::string& url);

    // Kick off an async task on the shell's worker-thread pool.
    void run_async_(std::function<void()> fn);
    // Like run_async_, but routed to the shell's mutation pool.
    void run_async_mut_(std::function<void()> fn);
    // Post a callback to the UI thread (from any thread).
    void post_to_ui_(std::function<void()> fn);

    // Fetch source_json bytes on a background thread and write them to
    // dest_path on the UI thread. No-op if bytes are empty (fetch failed).
    void save_source_to_file_(std::string source_json, std::string dest_path);
    // Fetch source_json bytes and hand them to on_ready.
    void fetch_source_bytes_(
        const std::string& src,
        std::function<void(std::vector<std::uint8_t>)> on_ready);
    // Checks the disk cache for a previously fully-downloaded copy of `src`
    // first (async, off the UI thread) — a hit plays instantly via
    // load_bytes() with no network call at all. On a miss, delegates to
    // fetch_and_play_video_uncached_(). Called by on_video_clicked once
    // vid_viewer_->open() has already shown the thumbnail/spinner.
    void fetch_and_play_video_(std::string src);
    // Cancels any fetch still in flight under vid_fetch_group_, then fetches
    // and plays `src` in vid_viewer_ — streaming if a small classification
    // prefix says the container is fast-start and the video player backend
    // supports it, otherwise the classic full-buffer fetch + load_bytes().
    // Either path caches the complete result on success (see
    // kVideoCacheMaxBytes) so a later re-open hits the cache above.
    void fetch_and_play_video_uncached_(std::string src);
    // Fetch source_json bytes and place the decoded image on the clipboard.
    void copy_source_to_clipboard_(std::string source_json);
    // Look up event_id's raw JSON and place it on the clipboard.
    void copy_event_source_to_clipboard_(std::string event_id);

    // Trigger fetch of a full-res image into the shared tk_images_ cache.
    void ensure_viewer_image_(const std::string& url);

private:
    void wire_room_view_();
    // Shared body for finish_init()/on_room_info_updated()/
    // refresh_pinned_for_current_room_(): push r's pinned_events to
    // room_view_ and (re)check the can-pin/can-redact-others permissions.
    // The permission check is dispatched off the UI thread (see .cpp) so a
    // slow first check for a room never blocks this pane's first paint.
    void refresh_pinned_(const RoomInfo& r);
    tk::TextArea* compose_text_area_() const
    {
        return room_view_ ? room_view_->compose_bar()->text_area() : nullptr;
    }
    views::ForwardRoomPicker* forward_picker_() const
    {
        return widgets_.forward_picker;
    }
    void focus_forward_picker_field_() const
    {
        if (widgets_.focus_forward_picker_field)
            widgets_.focus_forward_picker_field();
    }
    void hide_forward_picker_field_() const
    {
        if (widgets_.hide_forward_picker_field)
            widgets_.hide_forward_picker_field();
    }
    views::RoomMediaView* room_media_view_() const
    {
        return widgets_.room_media_view;
    }

    Deps deps_;
    Widgets widgets_;
    // Aliases cached from deps_/widgets_ once at construction/attach() time
    // (never reassigned outside those two points) purely to keep the ported
    // method bodies below textually identical to their RoomWindowBase
    // originals — every `shell_->`/`room_view_->`/`img_viewer_->` call site
    // in this class reads the same as it did before the port.
    ShellBase* shell_ = nullptr;
    views::RoomView* room_view_ = nullptr;
    views::ImageViewerOverlay* img_viewer_ = nullptr;
    views::VideoViewerOverlay* vid_viewer_ = nullptr;

    std::string room_id_;
    // See save_compose_draft_/apply_compose_draft_ above.
    struct RoomComposeDraft
    {
        std::string text;
        int cursor_byte_pos = 0;
        std::optional<views::ComposeBar::PendingAttachment> pending;
        // Structured segments (from TextArea::composer_draft()), captured
        // alongside `text` so restoring a draft with a mention/emoticon pill
        // can replay it through insert_mention()/insert_emoticon() rather
        // than round-tripping through the lossy plain-text set_text() path,
        // which degrades every pill to a bare placeholder character and
        // never rebuilds it. Empty when the composer had no text_area (or
        // no pills) at save time — apply_compose_draft_ falls back to
        // set_text(text) in that case.
        std::vector<tesseract::MentionSeg> segments;
    };
    std::unordered_map<std::string, RoomComposeDraft> room_compose_drafts_;
    // A mention restored into the composer (see apply_compose_draft_) whose
    // avatar wasn't cached yet at restore time. Retried on a post_to_ui_
    // repost chain (see schedule_mention_avatar_retry_) until the avatar
    // arrives (then patched in place via TextArea::refresh_mention_avatar,
    // no text touched) or retries run out. Dropped without patching if
    // room_id no longer matches room_id_ (the user switched to yet another
    // room before this one's avatar landed — if they come back to this
    // room again, apply_compose_draft_ re-attempts fresh).
    struct PendingMentionAvatar
    {
        std::string room_id;
        std::string user_id;
        int retries_left = 20;
    };
    std::vector<PendingMentionAvatar> pending_mention_avatars_;
    // Queues one retry_pending_mention_avatars_ call via post_to_ui_ if one
    // isn't already in flight (post_to_ui_ implementations — g_idle_add /
    // QueuedConnection / PostMessage / dispatch_async — all defer to the
    // next run-loop turn, so this can't recurse synchronously).
    void schedule_mention_avatar_retry_();
    void retry_pending_mention_avatars_();
    bool mention_avatar_retry_scheduled_ = false;
    // Non-zero group id for this pane's video-viewer full-file fetch, so it
    // can be cancelled independently of room-switch cancellation (which uses
    // ShellBase::active_media_group_) and without colliding with any other
    // pane's — see ShellBase::alloc_media_group_(). Allocated once in the
    // constructor.
    std::uint64_t vid_fetch_group_ = 0;
    // Room-switch member-list cache backing the received-mention-pill avatar
    // provider — holds names + avatar_urls only.
    std::vector<tesseract::RoomMember> cached_room_members_;
    std::string cached_members_room_;
    // Liveness token specifically for ShellBase::extract_drop_media_(), whose
    // virtual signature (implemented per-platform-shell) expects a
    // std::shared_ptr<bool> that stays safely dereferenceable even after this
    // RoomPane is destroyed — unlike EnableWeakSelf's weak_flag(), which
    // aliases this object's own memory and must be re-locked after every
    // deferred hop. Kept as its own independently-heap-allocated flag (the
    // old pattern) rather than migrating extract_drop_media_'s cross-shell
    // contract in this pass.
    std::shared_ptr<bool> media_extract_alive_ = std::make_shared<bool>(true);
    // First timeline reset after construction/retarget = initial fill (gate
    // the display like a room switch); later resets are reconnect/gappy
    // refreshes of the room already shown (refresh in place, no blank).
    bool displayed_once_ = false;
    // Rows trimmed off a room-switch timeline reset's front (oldest-first,
    // matching the reset's own ordering) when it exceeds
    // ShellBase::kSwitchDisplayCap, so tk::ListView doesn't have to measure
    // the whole snapshot synchronously on first paint. Drained one pagination
    // batch at a time by request_pagination_back_() before it ever falls
    // through to a real SDK paginate_back_with_status call. Always cleared at
    // the start of on_timeline_reset(), switch or not, so no stale buffer
    // from a previous room can leak.
    std::vector<views::MessageRowData> withheld_older_rows_;
    // Dedup for on_visible_range_changed's lazy media fetch.
    std::unordered_set<std::string> visible_media_prepped_;
    // In-flight message forwards started from THIS pane's picker, keyed by
    // the same process-global request_id space as ShellBase::pending_forwards_.
    std::unordered_map<std::uint64_t, std::string> pending_forwards_;
    // Media-fetch group id for this pane's gallery, distinct from
    // media_group_for_room_(room_id_) so cancel_media_group_ on gallery
    // close never touches unrelated fetches.
    std::uint64_t media_view_group_ = 0;
    // The room the currently-open gallery is showing. Snapshotted at open
    // time and NOT re-derived from room_id_ afterward, since the main
    // window's gallery can stay pinned open on a room the user has since
    // navigated away from (room_id_ tracks whatever's currently displayed;
    // this doesn't). For pop-outs the two are always equal, since room_id_
    // never changes underneath them.
    std::string media_view_room_id_;
    std::uint64_t media_view_pending_request_id_ = 0;
    int media_view_retries_left_ = 0;
    bool media_view_paginate_pending_ = false;
    std::uint64_t media_view_known_media_count_ = 0;
    // DB-first gallery paging: an in-flight load_room_media_page_async id (0 =
    // none), the oldest ts_ms delivered so far (cursor for the next page), and
    // whether the persistent index has been drained for this room (past which
    // on_media_view_load_older_ falls through to network pagination).
    std::uint64_t media_view_db_request_id_ = 0;
    std::uint64_t media_view_db_oldest_ts_ = 0;
    bool media_view_db_exhausted_ = false;
    static constexpr std::uint32_t kMediaViewDbPage = 120;

    // Thread-panel state, scoped to this pane.
    using ThreadPanel = ThreadPanelController::ThreadPanel;
    using ThreadTrigger = ThreadPanelController::ThreadTrigger;
    ThreadPanel thread_panel_ = ThreadPanel::Closed;
    ThreadPanel thread_panel_prev_ = ThreadPanel::Closed;
    std::string thread_root_;
    ThreadPanelController thread_ctl_;
    // Separate guard for the open thread's own message backfill (paginate_
    // thread_back_) — distinct from thread_ctl_, which guards the thread-
    // *list* panel's backfill and resets on a different schedule (room
    // switch vs. every thread-panel transition).
    ThreadPanelController thread_msg_ctl_;
};

} // namespace tesseract
