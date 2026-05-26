#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
// GdiplusTypes.h calls unqualified min()/max(); with NOMINMAX the Win32
// macros are gone, so std::min/std::max must be brought into scope (a bare
// <algorithm> is not enough — the names there are std-qualified) before it.
#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/session_store.h>
#include <tesseract/visual.h>

#include "app/ShellBase.h"
#include "app/EventHandlerBase.h"
#include "tk/anim_image_cache.h"
#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_win32.h"
#include "views/EmojiPicker.h"
#include "views/format.h"
#include "views/MainAppWidget.h"
#include "views/StickerPicker.h"
#include "views/JoinRoomView.h"
#include "views/ShortcodeEngine.h"
#include "views/ShortcodePopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"

#include "views/AccountPicker.h"
#include "views/SettingsView.h"
#include "app/SettingsController.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Custom window messages
constexpr UINT WM_TESSERACT_MESSAGE_INSERTED = WM_APP + 1;
constexpr UINT WM_TESSERACT_ROOMS = WM_APP + 2;
constexpr UINT WM_TESSERACT_SYNC_ERROR = WM_APP + 3;
constexpr UINT WM_TESSERACT_TIMELINE_RESET = WM_APP + 4;
constexpr UINT WM_TESSERACT_RECONNECT = WM_APP + 5;
constexpr UINT WM_TESSERACT_AUTH_ERROR = WM_APP + 6;
constexpr UINT WM_TESSERACT_BACKUP_PROGRESS = WM_APP + 7;
constexpr UINT WM_TESSERACT_RECOVER_DONE = WM_APP + 8;
constexpr UINT WM_TESSERACT_MESSAGE_UPDATED = WM_APP + 9;
constexpr UINT WM_TESSERACT_PAGINATE_DONE = WM_APP + 10;
constexpr UINT WM_TESSERACT_MESSAGE_REMOVED = WM_APP + 11;
constexpr UINT WM_TESSERACT_IMAGE_PACKS = WM_APP + 12;
// WM_APP + 13 was WM_TESSERACT_STICKER_BYTES; the sticker picker now uses
// the shared ShellBase async image-cache path (ensure_picker_image_).
constexpr UINT WM_TESSERACT_MEDIA_BYTES = WM_APP + 14;
constexpr UINT WM_TESSERACT_SUBSCRIBE_DONE = WM_APP + 15;
constexpr UINT WM_TESSERACT_ACCOUNT_PREFS = WM_APP + 16;
constexpr UINT WM_TESSERACT_NOTIFY = WM_APP + 17;
// WM_APP + 18 = WM_TESSERACT_NOTIFY_CLICK, defined in Win32Notifier.h
constexpr UINT WM_TESSERACT_VIDEO_BYTES = WM_APP + 19;
// WM_APP + 20 is taken by Win32TrayIcon on its hidden helper HWND.
constexpr UINT WM_TESSERACT_ROOM_LIST_STATE = WM_APP + 21;
constexpr UINT WM_TESSERACT_POST_TO_UI = WM_APP + 22;
constexpr UINT WM_TESSERACT_JUMP_DONE = WM_APP + 23;
constexpr UINT WM_TESSERACT_JOIN_ROOM_LOOKUP_DONE = WM_APP + 25;
constexpr UINT WM_TESSERACT_JOIN_ROOM_DONE = WM_APP + 26;
constexpr UINT WM_TESSERACT_FILE_BYTES = WM_APP + 27;

namespace win32
{

class Win32Notifier; // defined in Win32Notifier.h, included by MainWindow.cpp
class Win32TrayIcon; // defined in Win32TrayIcon.h, included by MainWindow.cpp

class LoginView;

// ---------------------------------------------------------------------------

class MainWindow : public tesseract::ShellBase
{
public:
    static bool register_class(HINSTANCE hInst);
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    explicit MainWindow(HINSTANCE hInst);
    ~MainWindow();

    bool create(int nCmdShow);

    // MediaKind is inherited from tesseract::ShellBase.
    // RoomAvatar → tk_avatars_, relayout main_app_surface_
    // UserAvatar → tk_avatars_, invalidate main_app_surface_
    // MediaImage → anim_cache_/tk_images_[url], invalidate main_app_surface_

    // ── EventHandlerBase UI-thread hook overrides (Win32) ────────────────────
    void handle_timeline_reset_ui_(
        std::string room_id,
        tesseract::EventList snapshot) override;
    void
    handle_message_inserted_ui_(std::string room_id, std::size_t index,
                                std::unique_ptr<tesseract::Event> ev) override;
    void
    handle_message_updated_ui_(std::string room_id, std::size_t index,
                               std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_removed_ui_(std::string room_id,
                                    std::size_t index) override;
    void handle_sync_error_ui_(std::string context, std::string user_id,
                               std::string description,
                               bool soft_logout) override;
    void
    handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void refresh_pickers_packs_() override;
    void handle_verification_request_ui_(std::string flow_id,
                                         std::string user_id,
                                         std::string device_id,
                                         bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(std::string flow_id,
                                           std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_notification_ui_(std::string user_id, std::string room_id,
                                 std::string room_name, std::string sender,
                                 std::string body, bool is_mention,
                                 std::vector<uint8_t> avatar_bytes,
                                 std::vector<uint8_t> image_bytes) override;
    void on_room_list_state_ui_() override;
    void on_server_info_ready_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;

    // Returns the user-chosen path, or L"" if cancelled.
    // Also called by RoomWindow for save dialogs in popout windows.
    std::wstring show_save_dialog_(const std::wstring& suggested,
                                   const wchar_t* filter);
    void wire_key_dialog_callbacks_();

private:
    void on_create(HWND hwnd);
    void on_destroy();
    void on_size(int w, int h);
    void start_login();
    void on_login_succeeded();
    void show_login_view();
    void show_main_content();
    void open_settings_();
    void close_settings_();
    void on_send_clicked();
    void on_room_selected(const std::string& room_id);
    // Posted-message payloads — see WM_TESSERACT_* constants above. The
    // posting code transfers ownership of each heap-allocated payload to
    // the receiving handler.
    struct PostedTimelineReset
    {
        std::string room_id;
        tesseract::EventList snapshot;
    };
    struct PostedMessageEvent
    {
        std::string room_id;
        std::size_t index;
        std::unique_ptr<tesseract::Event> event; // null for "removed"
    };
    struct JumpDonePayload
    {
        bool ok;
        std::string room_id;
        std::string event_id;  // valid when ok == true
        std::string error_msg; // valid when ok == false
    };
    struct NotificationPayload
    {
        std::string room_id;
        std::string room_name;
        std::string sender;
        std::string body;
        std::string user_id; // which AccountSession fired this
        bool is_mention;
        std::vector<uint8_t> avatar_bytes;
        std::vector<uint8_t> image_bytes; // already privacy-gated
    };
    struct RoomsPayload
    {
        std::string user_id;
        std::vector<tesseract::RoomInfo> rooms;
    };

    void on_tesseract_notify(const NotificationPayload* p);
    void request_attention_();
    void navigate_to_room(const std::string& room_id);
    void on_tesseract_timeline_reset(PostedTimelineReset* payload);
    void on_tesseract_message_inserted(PostedMessageEvent* payload);
    void on_tesseract_message_updated(PostedMessageEvent* payload);
    void on_tesseract_message_removed(PostedMessageEvent* payload);
    void on_tesseract_paginate_done(std::string* room_id, bool reached_start);
    void on_tesseract_subscribe_done(std::string* room_id, bool reached_start);
    void openJumpToDateDialog();
    void on_tesseract_jump_done(JumpDonePayload* p);
    void on_tesseract_rooms(RoomsPayload* payload);
    void refresh_room_list();
    /// Kick off back-pagination on a worker thread.
    void request_more_history(const std::string& room_id);

    // ensure_room_avatar_, ensure_user_avatar_, ensure_media_image_,
    // and ensure_reply_details_ are inherited from tesseract::ShellBase.

    void on_reconnect(const std::string& user_id);
    void on_space_back();
    void on_auth_error(const std::string& user_id, bool soft_logout);
    void on_backup_progress(tesseract::BackupProgress* progress);
    void on_room_list_state(tesseract::RoomListState state);
    void refresh_sync_status();
    void switch_active_account(int new_idx);
    void begin_add_account();
    void logout_active_account();
    void on_login_cancelled();
    void rebuild_account_picker();
    void open_account_picker();
    // last_room_list_state_, last_backup_state_, last_imported_keys_,
    // sync_progress_shown_, and recovery_banner_dismissed_ are inherited
    // from tesseract::ShellBase.
    UINT_PTR sync_status_debounce_timer_id_ = 0;
    static constexpr UINT_PTR kSyncStatusDebounceTimerId = 0xA1B2;
    void maybe_show_recovery_banner();
    void on_recovery_verify_clicked();
    void on_recovery_dismiss_clicked();
    void on_recover_done(bool ok, std::wstring msg);
    void populate_user_strip();
    // Resolve any media bytes the row references and decode them into
    // tk::Images held in `tk_avatars_` / `tk_images_`. Shared by every
    // positional-callback path (insert / update / reset).
    void ensure_row_media(const tesseract::Event& ev);
    void clear_messages();
    // Room and message rendering are owned by the shared widget tree —
    // see RoomListView / MessageListView. The legacy DRAWITEM hooks
    // are gone.

    // ── Emoji picker ────────────────────────────────────────────────────
    // A floating WS_POPUP HWND parents a tk::win32::Surface that paints
    // the shared tesseract::views::EmojiPicker; selection routes back to
    // insert_emoji_at_cursor.
    void ensure_emoji_picker_created();
    void toggle_emoji_picker();
    void refresh_emoji_picker();
    /// Open the emoji picker anchored to a sub-rect inside `parent_hwnd`
    /// (rect is in parent client coords). Used for the reaction "+" chip.
    void popup_emoji_at_rect(HWND parent_hwnd, tk::Rect local_rect);
    void popup_sticker_at_rect(HWND parent_hwnd, tk::Rect local_rect);
    void insert_emoji_at_cursor(const std::string& glyph);
    void pick_emoticon_at_cursor(const tesseract::ImagePackImage& img);

    // ── Sticker picker ───────────────────────────────────────────────────
    // Parallel to the emoji picker. A floating WS_POPUP HWND parents a
    // tk::win32::Surface that paints the shared
    // tesseract::views::StickerPicker; selection routes through
    // Client::send_sticker.
    void ensure_sticker_picker_created();
    void toggle_sticker_picker();
    void refresh_sticker_picker();

    // ── Join room dialog ─────────────────────────────────────────────────
    // A centred WS_POPUP HWND hosts JoinRoomView. Lookup and join run on
    // worker threads and post WM_TESSERACT_JOIN_ROOM_LOOKUP_DONE /
    // WM_TESSERACT_JOIN_ROOM_DONE back to the UI thread.
    void ensure_join_room_created();
    void open_join_room_dialog();

    // When non-empty, the next emoji selection routes through
    // `Client::send_reaction` for this event_id rather than into compose.
    std::string pending_reaction_event_id_;

    void apply_default_font(HWND);                // SegoeUI / SegoeUI Variable
    void on_system_theme_changed();               // re-apply DWM + invalidate
    void paint_main_background(HDC, const RECT&); // compose card etc.
    void show_user_context_menu_(int screen_x, int screen_y);

    static constexpr int kEmojiCellW = 36;
    static constexpr int kEmojiCols = 8;
    static constexpr int kEmojiPickW = kEmojiCellW * kEmojiCols + 16; // ~304
    static constexpr int kEmojiPickH = 320;

    static constexpr int kStickerPickW = 360;
    static constexpr int kStickerPickH = 420;
    static constexpr int kJoinRoomPickW =
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredW);
    static constexpr int kJoinRoomPickH =
        static_cast<int>(tesseract::views::JoinRoomView::kPreferredH);

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize = tesseract::visual::kMsgAvatarSize;
    static constexpr int kRoomRowH = tesseract::visual::kRoomRowHeight;
    static constexpr int kMsgRowPad = tesseract::visual::kMsgRowVerticalPad;
    static constexpr int kMsgMaxWidth = 520; // matches Qt

    HINSTANCE hInst_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> branding_surface_;
    bool branding_visible_ = true;
    std::unique_ptr<LoginView> login_view_;
    bool login_visible_ = false;

    // Single surface hosting the full MainAppWidget tree.
    // main_app_ / room_view_ live in ShellBase (assigned in setup).
    std::unique_ptr<tk::win32::Surface> main_app_surface_;

    // Settings surface — full-window sibling of main_app_surface_ and login_view_.
    std::unique_ptr<tk::win32::Surface> settings_surface_;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
    bool settings_visible_ = false;
    std::unique_ptr<tesseract::SettingsController> settings_controller_;
    std::unique_ptr<tk::NativeTextField> settings_name_field_;

    // Borrowed sub-view pointers (extracted from main_app_ for convenience).
    tesseract::views::RoomListView* room_list_view_ = nullptr;
    tesseract::views::RecoveryBanner* recovery_shared_ = nullptr;
    tesseract::views::VerificationBanner* verif_shared_ = nullptr;
    tesseract::views::ImageViewerOverlay* img_viewer_ = nullptr;
    tesseract::views::VideoViewerOverlay* vid_viewer_ = nullptr;

    // Native overlays hosted on main_app_surface_.
    std::unique_ptr<tk::NativeTextField> room_search_field_;
    std::unique_ptr<tk::NativeTextArea> room_text_area_;
    std::unique_ptr<tk::NativeTextArea> topic_text_area_;
    std::unique_ptr<tk::NativeTextField> recovery_key_field_;

    // ── Shortcode popup ───────────────────────────────────────────────────
    tesseract::views::ShortcodeEngine shortcode_engine_;
    tesseract::views::ShortcodeMatch shortcode_active_match_{};
    std::vector<tesseract::views::ShortcodeSuggestion>
        shortcode_current_suggestions_;

    HWND shortcode_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> shortcode_popup_surface_;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;

    void show_shortcode_popup_(
        const std::vector<tesseract::views::ShortcodeSuggestion>& suggestions,
        tk::Rect cursor_rect);
    void hide_shortcode_popup_();
    bool shortcode_popup_visible_() const
    {
        return shortcode_popup_hwnd_ && IsWindowVisible(shortcode_popup_hwnd_);
    }

    // ── @mention popup ────────────────────────────────────────────────────
    std::vector<tesseract::RoomMember> cached_room_members_;
    HWND mention_popup_hwnd_ = nullptr;
    std::unique_ptr<tk::win32::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;
    void show_mention_popup_(tk::Rect cursor_rect, int rows);
    void hide_mention_popup_();
    bool mention_popup_visible_() const
    {
        return mention_popup_hwnd_ && IsWindowVisible(mention_popup_hwnd_);
    }

    HWND hEmojiPicker_ = nullptr; // floating WS_POPUP host
    std::unique_ptr<tk::win32::Surface> emoji_picker_surface_;
    tesseract::views::EmojiPicker* emoji_picker_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField> emoji_picker_search_field_;
    HWND hStickerPicker_ = nullptr; // floating WS_POPUP host
    std::unique_ptr<tk::win32::Surface> sticker_picker_surface_;
    tesseract::views::StickerPicker* sticker_picker_shared_ =
        nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField> sticker_picker_search_field_;
    POINT picker_track_pos_ = {}; // main-window top-left; kept in sync for picker delta tracking
    void reposition_visible_pickers_(int dx, int dy);
    HWND hTopicTooltip_ = nullptr; // tracking tooltip for truncated room topics
    std::wstring topic_tooltip_text_; // backing store for TTM_UPDATETIPTEXTW

    HWND hJoinRoom_ = nullptr; // centred WS_POPUP host
    std::unique_ptr<tk::win32::Surface> join_room_surface_;
    tesseract::views::JoinRoomView* join_room_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField> join_room_alias_field_;
    uint32_t join_room_gen_ =
        0; // incremented on each open; guards stale callbacks

    HWND hStatus_ = nullptr;

    bool recovery_banner_visible_ = false;
    // recovery_banner_dismissed_ is inherited from tesseract::ShellBase.
    bool recovery_in_flight_ = false;

    bool verif_banner_visible_ = false;

    // Multi-account state: accounts_, active_account_index_, client_,
    // event_handler_, per_account_rooms_, pending_login_client_,
    // pending_login_temp_dir_, pending_login_is_add_account_,
    // add_account_return_idx_ are inherited from tesseract::ShellBase.

    // Account-picker popup (WS_POPUP HWND hosting AccountPicker surface).
    HWND hAccountPicker_ = nullptr;
    std::unique_ptr<tk::win32::Surface> account_picker_surface_;
    tesseract::views::AccountPicker* account_picker_ = nullptr; // borrowed

    std::unique_ptr<Win32TrayIcon> tray_;
    bool quitting_ = false;
    // rooms_, current_room_id_, pending_restore_room_, space_stack_
    // are inherited from tesseract::ShellBase.

    // pagination_ and kPaginationBatch are inherited from tesseract::ShellBase.

    ULONG_PTR gdiplus_token_ = 0;

    // tk_avatars_, tk_images_, anim_cache_, voice_prefetched_,
    // video_thumb_in_flight_, reply_details_requested_, media_fetches_in_flight_,
    // sticker_fetches_in_flight_ are inherited from tesseract::ShellBase.

    /// Promote `url`'s entry in `tk_images_` to an animated cache if the
    /// bytes turn out to be multi-frame; otherwise leave the static entry
    /// in place. Idempotent + safe to call after either cache already has
    /// the URL. Starts the frame-tick timer when the first animated
    /// entry lands.
    void try_load_animation(const std::string& url,
                            std::span<const std::uint8_t> bytes);
    /// WM_TIMER handler — advances every entry whose `next_advance_ms`
    /// has passed and triggers a single repaint of the message surface +
    /// sticker picker when at least one frame changed.
    void on_anim_tick();

    // run_async_, shutting_down_, workers_mu_, workers_cv_, workers_in_flight_
    // are inherited from tesseract::ShellBase.

    static constexpr UINT_PTR kAnimTimerId = 0xA01u;
    static constexpr UINT_PTR kSearchDebounceTimer = 3;
    static constexpr UINT_PTR kScrollDebounceTimerId = 4;
    static constexpr UINT_PTR kVerifDoneTimerId = 5;
    static constexpr UINT_PTR kMarkReadTimerId = 6;
    static constexpr UINT_PTR kStatusClearTimerId = 7;
    static constexpr UINT_PTR kPresenceTickTimerId = 8;
    static constexpr UINT kAnimTimerHz = 16; // ~60 fps
    bool anim_timer_running_ = false;
    std::string pending_search_text_;

    // ShellBase virtual hooks (Win32 implementations).
    void navigate_to_room_(const std::string& room_id) override
    {
        navigate_to_room(room_id);
    }
    void apply_theme_ui_(const tk::Theme& t) override;
    tk::ThemeMode os_color_scheme_() const override;
    void post_to_ui_(std::function<void()> fn) override;
    void request_relayout_() override;
    void request_repaint_() override;
    void on_rooms_updated_() override;
    void on_invites_updated_() override;
    void on_space_children_cache_ready_ui_() override;
    void on_tray_unread_changed_(bool has_unread,
                                 bool has_highlight) override;

    // Tab management hooks.
    void on_tab_state_changed_ui_() override;
    float get_message_scroll_fraction_() override;
    void set_message_scroll_fraction_(float t) override;
    std::string get_compose_draft_() override;
    void set_compose_draft_(const std::string&) override;
    tesseract::RoomWindowBase*
    create_secondary_room_window_(const std::string& room_id) override;
    void on_media_bytes_ready_(const std::string& cache_key, MediaKind kind,
                               std::vector<uint8_t> bytes) override;
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes, int max_w,
                               int max_h) override;
    std::int64_t monotonic_ms_() override;
    void start_anim_tick_() override;
    void stop_anim_tick_() override;
    void repaint_anim_frame_() override;
    void repaint_pickers_() override;
    void generate_video_thumbnail_(const std::string& event_id,
                                   const std::string& video_url) override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;

    // Extract thumbnail, dimensions, and duration from raw bytes on a
    // background thread; posts result back via post_to_ui_.
    void extract_media_info_(std::uint32_t pending_gen,
                             std::vector<std::uint8_t> bytes,
                             std::string mime);

    static constexpr const wchar_t* CLASS_NAME = L"TesseractMainWnd";
    static constexpr int IDM_LOGOUT = 120;
    static constexpr int IDM_ADD_ACCOUNT = 121;
    static constexpr int IDM_QUIT = 122;
};

} // namespace win32
