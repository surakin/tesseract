#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <algorithm>  // std::min/std::max — must precede gdiplus.h with NOMINMAX
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
#include "views/ImageViewerOverlay.h"
#include "views/VideoViewerOverlay.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"
#include "views/RoomView.h"
#include "views/VerificationBanner.h"
#include "views/StickerPicker.h"

#include "views/AccountPicker.h"

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
constexpr UINT WM_TESSERACT_ROOMS            = WM_APP + 2;
constexpr UINT WM_TESSERACT_SYNC_ERROR       = WM_APP + 3;
constexpr UINT WM_TESSERACT_TIMELINE_RESET   = WM_APP + 4;
constexpr UINT WM_TESSERACT_RECONNECT        = WM_APP + 5;
constexpr UINT WM_TESSERACT_AUTH_ERROR       = WM_APP + 6;
constexpr UINT WM_TESSERACT_BACKUP_PROGRESS  = WM_APP + 7;
constexpr UINT WM_TESSERACT_RECOVER_DONE     = WM_APP + 8;
constexpr UINT WM_TESSERACT_MESSAGE_UPDATED  = WM_APP + 9;
constexpr UINT WM_TESSERACT_PAGINATE_DONE    = WM_APP + 10;
constexpr UINT WM_TESSERACT_MESSAGE_REMOVED  = WM_APP + 11;
constexpr UINT WM_TESSERACT_IMAGE_PACKS      = WM_APP + 12;
constexpr UINT WM_TESSERACT_STICKER_BYTES    = WM_APP + 13;
constexpr UINT WM_TESSERACT_MEDIA_BYTES      = WM_APP + 14;
constexpr UINT WM_TESSERACT_SUBSCRIBE_DONE   = WM_APP + 15;
constexpr UINT WM_TESSERACT_ACCOUNT_PREFS    = WM_APP + 16;
constexpr UINT WM_TESSERACT_NOTIFY           = WM_APP + 17;
// WM_APP + 18 = WM_TESSERACT_NOTIFY_CLICK, defined in Win32Notifier.h
constexpr UINT WM_TESSERACT_VIDEO_BYTES      = WM_APP + 19;
// WM_APP + 20 is taken by Win32TrayIcon on its hidden helper HWND.
constexpr UINT WM_TESSERACT_ROOM_LIST_STATE  = WM_APP + 21;
constexpr UINT WM_TESSERACT_POST_TO_UI       = WM_APP + 22;
constexpr UINT WM_TESSERACT_JUMP_DONE        = WM_APP + 23;

namespace win32 {

class Win32Notifier; // defined in Win32Notifier.h, included by MainWindow.cpp
class Win32TrayIcon; // defined in Win32TrayIcon.h, included by MainWindow.cpp

class LoginView;

// ---------------------------------------------------------------------------

class MainWindow : public tesseract::ShellBase {
    // Owner-drawn sidebar widgets need access to MainWindow state for paint.
    friend LRESULT CALLBACK user_strip_wnd_proc (HWND, UINT, WPARAM, LPARAM);

public:
    static bool register_class(HINSTANCE hInst);
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    explicit MainWindow(HINSTANCE hInst);
    ~MainWindow();

    bool create(int nCmdShow);

    // MediaKind is inherited from tesseract::ShellBase.
    // RoomAvatar → tk_avatars_, relayout room_surface_ + chat_surface_
    // UserAvatar → tk_avatars_, invalidate chat_surface_
    // MediaImage → anim_cache_/tk_images_[url], invalidate chat_surface_

    // ── EventHandlerBase UI-thread hook overrides (Win32) ─────────────────────
    void handle_timeline_reset_ui_(
        std::string room_id,
        std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void handle_message_inserted_ui_(
        std::string room_id, std::size_t index,
        std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_updated_ui_(
        std::string room_id, std::size_t index,
        std::unique_ptr<tesseract::Event> ev) override;
    void handle_message_removed_ui_(
        std::string room_id, std::size_t index) override;
    void handle_sync_error_ui_(
        std::string context, std::string user_id,
        std::string description, bool soft_logout) override;
    void handle_backup_progress_ui_(tesseract::BackupProgress progress) override;
    void handle_image_packs_updated_ui_() override;
    void handle_verification_request_ui_(
        std::string flow_id, std::string user_id,
        std::string device_id, bool incoming) override;
    void handle_sas_ready_ui_(
        std::string flow_id,
        std::vector<tesseract::VerificationEmoji> emojis) override;
    void handle_verification_done_ui_(std::string flow_id) override;
    void handle_verification_cancelled_ui_(
        std::string flow_id, std::string reason) override;
    void handle_verification_state_ui_(bool is_verified) override;
    void handle_account_prefs_updated_ui_(
        std::string user_id, std::string json) override;
    void handle_notification_ui_(
        std::string user_id, std::string room_id,
        std::string room_name, std::string sender,
        std::string body, bool is_mention,
        std::vector<uint8_t> avatar_bytes) override;
    void on_room_list_state_ui_() override;
    void update_typing_bar_(const std::string& text, bool visible) override;
    void on_url_preview_ready_(const std::string& url,
                               const tesseract::Client::UrlPreview& preview) override;

private:
    void on_create(HWND hwnd);
    void on_destroy();
    void on_size(int w, int h);
    void start_login();
    void on_login_succeeded();
    void show_login_view();
    void show_main_content();
    void on_send_clicked();
    void on_room_selected(const std::string& room_id);
    // Posted-message payloads — see WM_TESSERACT_* constants above. The
    // posting code transfers ownership of each heap-allocated payload to
    // the receiving handler.
    struct PostedTimelineReset {
        std::string                                     room_id;
        std::vector<std::unique_ptr<tesseract::Event>>  snapshot;
    };
    struct PostedMessageEvent {
        std::string                       room_id;
        std::size_t                       index;
        std::unique_ptr<tesseract::Event> event;   // null for "removed"
    };
    struct JumpDonePayload {
        bool        ok;
        std::string room_id;
        std::string event_id;   // valid when ok == true
        std::string error_msg;  // valid when ok == false
    };
    struct NotificationPayload {
        std::string room_id;
        std::string room_name;
        std::string sender;
        std::string body;
        std::string user_id;   // which AccountSession fired this
        bool        is_mention;
    };
    struct RoomsPayload {
        std::string                     user_id;
        std::vector<tesseract::RoomInfo> rooms;
    };

    void on_tesseract_notify(const NotificationPayload* p);
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
    UINT_PTR                 sync_status_debounce_timer_id_ = 0;
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
    void   ensure_emoji_picker_created();
    void   toggle_emoji_picker();
    void   refresh_emoji_picker();
    /// Open the emoji picker anchored to a sub-rect inside `parent_hwnd`
    /// (rect is in parent client coords). Used for the reaction "+" chip.
    void   popup_emoji_at_rect(HWND parent_hwnd, tk::Rect local_rect);
    void   insert_emoji_at_cursor(const std::string& glyph);

    // ── Sticker picker ───────────────────────────────────────────────────
    // Parallel to the emoji picker. A floating WS_POPUP HWND parents a
    // tk::win32::Surface that paints the shared
    // tesseract::views::StickerPicker; selection routes through
    // Client::send_sticker.
    void   ensure_sticker_picker_created();
    void   toggle_sticker_picker();
    void   refresh_sticker_picker();

    // When non-empty, the next emoji selection routes through
    // `Client::send_reaction` for this event_id rather than into compose.
    std::string                      pending_reaction_event_id_;

    void   apply_default_font(HWND);     // SegoeUI / SegoeUI Variable
    void   on_system_theme_changed();    // re-apply DWM + invalidate
    void   paint_main_background(HDC, const RECT&);  // compose card etc.

    static constexpr int kEmojiCellW = 36;
    static constexpr int kEmojiCols  = 8;
    static constexpr int kEmojiPickW = kEmojiCellW * kEmojiCols + 16;  // ~304
    static constexpr int kEmojiPickH = 320;

    static constexpr int kStickerPickW = 360;
    static constexpr int kStickerPickH = 420;

    Gdiplus::Bitmap* get_user_avatar(const std::string& mxc_url);
    void draw_circle_bitmap(Gdiplus::Graphics& g, Gdiplus::Bitmap* bmp,
                             int x, int y, int size);
    void draw_initials_circle(Gdiplus::Graphics& g, const std::string& name,
                               int x, int y, int size);
    static void fill_rounded_rect(Gdiplus::Graphics& g, Gdiplus::Brush& brush,
                                   float x, float y, float w, float h, float r);

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize  = tesseract::visual::kMsgAvatarSize;
    static constexpr int kSpaceNavBarH   = 36;
    static constexpr int kRoomRowH       = tesseract::visual::kRoomRowHeight;
    static constexpr int kMsgRowPad      = tesseract::visual::kMsgRowVerticalPad;
    static constexpr int kMsgMaxWidth    = 520;            // matches Qt
    static constexpr int kUserStripH     = tesseract::visual::kUserStripHeight;

    HINSTANCE hInst_;
    HWND      hwnd_       = nullptr;
    std::unique_ptr<LoginView> login_view_;
    bool      login_visible_ = false;
    std::unique_ptr<tk::win32::Surface>            room_surface_;
    tesseract::views::RoomListView*                room_list_view_   = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>           room_search_field_;
    HWND      hSideSep_      = nullptr;   // 1px vertical separator at x=ROOM_W
    HWND      hSpaceNavBack_ = nullptr;   // ← button shown when inside a space
    HWND      hSpaceNavLabel_= nullptr;   // space name label next to back button
    // Combined chat area: RoomHeader + MessageListView + typing + ComposeBar.
    std::unique_ptr<tk::win32::Surface>            chat_surface_;
    tesseract::views::RoomView*                    room_view_         = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextArea>            room_text_area_;
    HWND      hEmojiPicker_ = nullptr;       // floating WS_POPUP host
    std::unique_ptr<tk::win32::Surface>     emoji_picker_surface_;
    tesseract::views::EmojiPicker*           emoji_picker_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>    emoji_picker_search_field_;
    HWND      hStickerPicker_ = nullptr;     // floating WS_POPUP host
    std::unique_ptr<tk::win32::Surface>     sticker_picker_surface_;
    tesseract::views::StickerPicker*         sticker_picker_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>    sticker_picker_search_field_;

    // Full-window image/sticker lightbox overlay (WS_CHILD of hwnd_).
    std::unique_ptr<tk::win32::Surface>      img_viewer_surface_;
    tesseract::views::ImageViewerOverlay*    img_viewer_ = nullptr;  // borrowed

    // Full-window video lightbox overlay (WS_CHILD of hwnd_).
    std::unique_ptr<tk::win32::Surface>      vid_viewer_surface_;
    tesseract::views::VideoViewerOverlay*    vid_viewer_ = nullptr;  // borrowed

    HWND      hStatus_            = nullptr;

    // Recovery banner — shared widget on a tk::win32::Surface. Key
    // input is a NativeTextField overlay (Win32 EDIT under the hood).
    std::unique_ptr<tk::win32::Surface>      recovery_surface_;
    tesseract::views::RecoveryBanner*         recovery_shared_   = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>     recovery_key_field_;
    bool      recovery_banner_visible_   = false;
    // recovery_banner_dismissed_ is inherited from tesseract::ShellBase.
    bool      recovery_in_flight_        = false;

    // Verification banner — shared widget on a tk::win32::Surface. Initially
    // hidden; shown by handle_verification_state_ui_ when is_verified=false.
    std::unique_ptr<tk::win32::Surface>      verif_surface_;
    tesseract::views::VerificationBanner*    verif_shared_      = nullptr; // borrowed
    bool      verif_banner_visible_      = false;

    HWND             hUserStrip_     = nullptr;
    HWND             hUserIdLabel_   = nullptr;  // Matrix ID second line
    // my_display_name_, my_avatar_url_, my_user_id_ are inherited from ShellBase.
    Gdiplus::Bitmap* user_avatar_bmp_ = nullptr;  // owned; null = use initials

    // Multi-account state: accounts_, active_account_index_, client_,
    // event_handler_, per_account_rooms_, pending_login_client_,
    // pending_login_temp_dir_, pending_login_is_add_account_,
    // add_account_return_idx_ are inherited from tesseract::ShellBase.

    // Account-picker popup (WS_POPUP HWND hosting AccountPicker surface).
    HWND                                         hAccountPicker_  = nullptr;
    std::unique_ptr<tk::win32::Surface>          account_picker_surface_;
    tesseract::views::AccountPicker*             account_picker_  = nullptr;  // borrowed

    std::unique_ptr<Win32TrayIcon>   tray_;
    bool                              quitting_ = false;
    // rooms_, current_room_id_, pending_restore_room_, space_stack_
    // are inherited from tesseract::ShellBase.

    // pagination_ and kPaginationBatch are inherited from tesseract::ShellBase.

    ULONG_PTR  gdiplus_token_ = 0;
    // GDI+ bitmap cache for the user-strip avatar (drawn in user_strip_wnd_proc).
    // Room-list + message-list avatars and inline media now flow through the
    // tk::Image caches below.
    std::unordered_map<std::string, Gdiplus::Bitmap*> user_avatar_cache_;

    // URL preview cache: keyed by URL, populated by on_url_preview_ready_.
    std::unordered_map<std::string, tesseract::views::UrlPreviewData> url_preview_data_;

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
    /// Async fetch path used by the sticker picker for stickers that
    /// haven't been seen in any message yet. Deduplicates against
    /// `sticker_fetches_in_flight_`; on landing, posts
    /// WM_TESSERACT_STICKER_BYTES to the UI thread which decodes
    /// (animated or static), caches, and invalidates the picker.
    void request_sticker_image(const std::string& cache_key);

    // run_async_, shutting_down_, workers_mu_, workers_cv_, workers_in_flight_
    // are inherited from tesseract::ShellBase.

    static constexpr UINT_PTR kAnimTimerId           = 0xA01u;
    static constexpr UINT_PTR kSearchDebounceTimer   = 3;
    static constexpr UINT_PTR kScrollDebounceTimerId = 4;
    static constexpr UINT_PTR kVerifDoneTimerId      = 5;
    static constexpr UINT     kAnimTimerHz          = 16;     // ~60 fps
    bool anim_timer_running_ = false;
    std::string pending_search_text_;

    // ShellBase virtual hooks (Win32 implementations).
    void post_to_ui_(std::function<void()> fn) override;
    void on_rooms_updated_() override;
    void on_media_bytes_ready_(const std::string& cache_key,
                                MediaKind kind,
                                std::vector<uint8_t> bytes) override;
    void generate_video_thumbnail_(const std::string& event_id,
                                    const std::string& video_url) override;
    void cache_rgba_image_(const std::string& key, int w, int h,
                           std::vector<uint8_t> rgba) override;

    static constexpr const wchar_t* CLASS_NAME  = L"TesseractMainWnd";
    static constexpr int            IDC_SIDE_SEPARATOR   = 112;
    static constexpr int            IDC_SPACE_BACK       = 113;
    static constexpr int            IDM_LOGOUT           = 120;
    static constexpr int            IDM_ADD_ACCOUNT      = 121;
};

} // namespace win32
