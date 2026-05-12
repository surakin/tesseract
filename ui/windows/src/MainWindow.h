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

#include <tesseract/client.h>
#include <tesseract/event_handler.h>
#include <tesseract/visual.h>

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/host_win32.h"
#include "views/ComposeBar.h"
#include "views/EmojiPicker.h"
#include "views/format.h"
#include "views/MessageListView.h"
#include "views/RecoveryBanner.h"
#include "views/RoomListView.h"

#include <memory>
#include <string>
#include <unordered_map>
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

namespace win32 {

class LoginView;

/// Win32 event handler: marshals Rust callbacks onto the UI thread via PostMessage.
class EventHandler final : public tesseract::IEventHandler {
public:
    explicit EventHandler(HWND hwnd) : hwnd_(hwnd) {}

    void on_timeline_reset(const std::string& room_id,
                            std::vector<std::unique_ptr<tesseract::Event>> snapshot) override;
    void on_message_inserted(const std::string& room_id,
                              std::size_t index,
                              std::unique_ptr<tesseract::Event> event) override;
    void on_message_updated(const std::string& room_id,
                             std::size_t index,
                             std::unique_ptr<tesseract::Event> event) override;
    void on_message_removed(const std::string& room_id,
                             std::size_t index) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;

private:
    HWND hwnd_;
};

// ---------------------------------------------------------------------------

class MainWindow {
    // Owner-drawn sidebar widgets need access to MainWindow state for paint.
    friend LRESULT CALLBACK room_header_wnd_proc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK user_strip_wnd_proc (HWND, UINT, WPARAM, LPARAM);

public:
    static bool register_class(HINSTANCE hInst);
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    explicit MainWindow(HINSTANCE hInst);
    ~MainWindow();

    bool create(int nCmdShow);

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

    void on_tesseract_timeline_reset(PostedTimelineReset* payload);
    void on_tesseract_message_inserted(PostedMessageEvent* payload);
    void on_tesseract_message_updated(PostedMessageEvent* payload);
    void on_tesseract_message_removed(PostedMessageEvent* payload);
    void on_tesseract_paginate_done(std::string* room_id, bool reached_start);
    void on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms);
    void refresh_room_list();
    /// Kick off back-pagination on a worker thread.
    void request_more_history(const std::string& room_id);

    // Convert a polymorphic SDK Event into MessageRowData for the shared
    // MessageListView; pre-fetch any referenced media into tk_images_.
    tesseract::views::MessageRowData to_row_data(const tesseract::Event& ev);
    void ensure_room_avatar(const tesseract::RoomInfo& r);
    void ensure_user_avatar_tk(const std::string& mxc);
    void ensure_media_image(const std::string& url, int max_w, int max_h);

    void on_reconnect();
    void on_auth_error(bool soft_logout);
    void on_backup_progress(tesseract::BackupProgress* progress);
    void maybe_show_recovery_banner();
    void on_recovery_verify_clicked();
    void on_recovery_dismiss_clicked();
    void on_recover_done(bool ok, std::wstring msg);
    void populate_user_strip();
    void do_logout();
    // Resolve any media bytes the row references and decode them into
    // tk::Images held in `tk_avatars_` / `tk_images_`. Shared by every
    // positional-callback path (insert / update / reset).
    void ensure_row_media(const tesseract::Event& ev);
    void clear_messages();
    void update_room_header(const tesseract::RoomInfo& info);

    // Room and message rendering are owned by the shared widget tree —
    // see RoomListView / MessageListView. The legacy DRAWITEM hooks
    // are gone.

    // ── Emoji picker ────────────────────────────────────────────────────
    // A floating WS_POPUP HWND parents a tk::win32::Surface that paints
    // the shared tesseract::views::EmojiPicker; selection routes back to
    // insert_emoji_at_cursor.
    void   ensure_emoji_picker_created();
    void   toggle_emoji_picker();
    /// Open the emoji picker anchored to a sub-rect inside `parent_hwnd`
    /// (rect is in parent client coords). Used for the reaction "+" chip.
    void   popup_emoji_at_rect(HWND parent_hwnd, tk::Rect local_rect);
    void   insert_emoji_at_cursor(const std::string& glyph);

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

    Gdiplus::Bitmap* get_room_avatar(const std::string& room_id);
    Gdiplus::Bitmap* get_user_avatar(const std::string& mxc_url);
    void draw_circle_bitmap(Gdiplus::Graphics& g, Gdiplus::Bitmap* bmp,
                             int x, int y, int size);
    void draw_initials_circle(Gdiplus::Graphics& g, const std::string& name,
                               int x, int y, int size);
    static void fill_rounded_rect(Gdiplus::Graphics& g, Gdiplus::Brush& brush,
                                   float x, float y, float w, float h, float r);

    static constexpr int kRoomAvatarSize = tesseract::visual::kRoomAvatarSize;
    static constexpr int kMsgAvatarSize  = tesseract::visual::kMsgAvatarSize;
    static constexpr int kRoomHeaderH    = 60;
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
    HWND      hSideSep_    = nullptr;   // 1px vertical separator at x=ROOM_W
    HWND      hRoomHeader_ = nullptr;
    std::unique_ptr<tk::win32::Surface>            msg_surface_;
    tesseract::views::MessageListView*             message_list_view_ = nullptr; // borrowed
    // Compose bar — tk::win32::Surface hosting the shared ComposeBar
    // with a NativeTextArea overlay (multi-line EDIT under the hood).
    std::unique_ptr<tk::win32::Surface>            compose_surface_;
    tesseract::views::ComposeBar*                   compose_shared_   = nullptr;  // borrowed
    std::unique_ptr<tk::NativeTextArea>             compose_text_area_;
    HWND      hEmojiPicker_ = nullptr;       // floating WS_POPUP host
    std::unique_ptr<tk::win32::Surface>     emoji_picker_surface_;
    tesseract::views::EmojiPicker*           emoji_picker_shared_ = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>    emoji_picker_search_field_;
    HWND      hStatus_     = nullptr;

    // Recovery banner — shared widget on a tk::win32::Surface. Key
    // input is a NativeTextField overlay (Win32 EDIT under the hood).
    std::unique_ptr<tk::win32::Surface>      recovery_surface_;
    tesseract::views::RecoveryBanner*         recovery_shared_   = nullptr; // borrowed
    std::unique_ptr<tk::NativeTextField>     recovery_key_field_;
    bool      recovery_banner_visible_   = false;
    bool      recovery_banner_dismissed_ = false;
    bool      recovery_in_flight_        = false;

    HWND             hUserStrip_     = nullptr;
    std::string      my_display_name_;
    std::string      my_avatar_url_;
    Gdiplus::Bitmap* user_avatar_bmp_ = nullptr;  // owned; null = use initials

    tesseract::Client                client_;
    std::unique_ptr<EventHandler>    event_handler_;
    std::vector<tesseract::RoomInfo> rooms_;
    tesseract::RoomInfo              current_room_info_;
    std::string                      current_room_id_;
    std::string                      my_user_id_;
    std::vector<std::string>         space_stack_;

    // Per-room back-pagination state (mirrors Qt/GTK/macOS).
    struct PaginationState { bool in_flight = false; bool reached_start = false; };
    std::unordered_map<std::string, PaginationState> pagination_;
    static constexpr std::uint16_t kPaginationBatch = 50;

    ULONG_PTR  gdiplus_token_ = 0;
    // GDI+ bitmap caches for the legacy native paint paths that the
    // migration still relies on: the room-header avatar (drawn in
    // room_header_wnd_proc) and the user-strip avatar (drawn in
    // user_strip_wnd_proc). Room-list + message-list avatars and
    // inline media flow through the tk::Image caches below.
    std::unordered_map<std::string, Gdiplus::Bitmap*> avatar_cache_;
    std::unordered_map<std::string, Gdiplus::Bitmap*> user_avatar_cache_;

    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_avatars_;
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> tk_images_;

    static constexpr const wchar_t* CLASS_NAME  = L"TesseractMainWnd";
    static constexpr int            IDC_SIDE_SEPARATOR   = 112;
    static constexpr int            IDM_LOGOUT           = 120;
};

} // namespace win32
