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

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Custom window messages
constexpr UINT WM_TESSERACT_MESSAGE        = WM_APP + 1;
constexpr UINT WM_TESSERACT_ROOMS          = WM_APP + 2;
constexpr UINT WM_TESSERACT_SYNC_ERROR     = WM_APP + 3;
constexpr UINT WM_TESSERACT_TIMELINE_RESET = WM_APP + 4;
constexpr UINT WM_TESSERACT_RECONNECT      = WM_APP + 5;
constexpr UINT WM_TESSERACT_AUTH_ERROR     = WM_APP + 6;
constexpr UINT WM_TESSERACT_BACKUP_PROGRESS = WM_APP + 7;
constexpr UINT WM_TESSERACT_RECOVER_DONE    = WM_APP + 8;

namespace win32 {

class LoginView;

/// Win32 event handler: marshals Rust callbacks onto the UI thread via PostMessage.
class EventHandler final : public tesseract::IEventHandler {
public:
    explicit EventHandler(HWND hwnd) : hwnd_(hwnd) {}

    void on_message(tesseract::Event* ev) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description,
                       bool soft_logout) override;
    void on_timeline_reset(const std::string& room_id) override;
    void on_session_saved(const std::string& session_json) override;
    void on_backup_progress(const tesseract::BackupProgress& progress) override;

private:
    HWND hwnd_;
};

// ---------------------------------------------------------------------------

struct MessageData {
    std::string          event_id;
    std::string          room_id;
    std::string          body;
    std::string          sender;
    std::string          sender_name;
    std::string          sender_avatar_url;
    uint64_t             timestamp  = 0;
    bool                 is_own     = false;
    tesseract::EventType type       = tesseract::EventType::Text;
    std::vector<tesseract::Reaction> reactions;
    /// Populated during `draw_message_item`, consumed by the WM_LBUTTONDOWN
    /// hit-test on the message list. Coordinates are relative to the row's
    /// top-left and become stale on the next paint of the same item — that's
    /// fine: every reaction click happens after a paint that has already
    /// recorded the chip rects for that row.
    mutable std::vector<std::pair<RECT, std::string>> chip_rects;
};

// ---------------------------------------------------------------------------

class MainWindow {
    // Owner-drawn sidebar widgets need access to MainWindow state for paint.
    friend LRESULT CALLBACK room_header_wnd_proc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK user_strip_wnd_proc (HWND, UINT, WPARAM, LPARAM);

public:
    static bool register_class(HINSTANCE hInst);
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK input_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                                 UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK msg_list_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                                    UINT_PTR, DWORD_PTR);

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
    void on_room_selected(int index);
    void on_tesseract_message(tesseract::Event* ev);
    void on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms);
    void on_tesseract_timeline_reset(std::string* room_id);

    void on_reconnect();
    void on_auth_error(bool soft_logout);
    void on_backup_progress(tesseract::BackupProgress* progress);
    void maybe_show_recovery_banner();
    void on_recovery_verify_clicked();
    void on_recovery_dismiss_clicked();
    void on_recover_done(bool ok, std::wstring msg);
    void layout_recovery_banner(int w);
    void populate_user_strip();
    void do_logout();
    void append_message(const tesseract::Event& ev);
    void clear_messages();
    void update_room_header(const tesseract::RoomInfo& info);

    int  compute_message_height(size_t idx);
    void draw_room_item(DRAWITEMSTRUCT* dis);
    void draw_message_item(DRAWITEMSTRUCT* dis);

    // ── Emoji picker ────────────────────────────────────────────────────
    static LRESULT CALLBACK emoji_picker_wnd_proc(HWND, UINT, WPARAM, LPARAM);
    void   register_emoji_class();
    void   ensure_emoji_picker_created();
    void   toggle_emoji_picker();
    void   on_emoji_search_changed();
    void   refresh_emoji_grid();              // rebuild from category or search
    void   show_emoji_category(int tab_idx);
    void   show_emoji_search_results(const std::string& query);
    void   pick_emoji_at(int row, int col);   // grid hit-test target
    void   pick_emoji_tab(int idx);
    void   draw_emoji_grid_item(DRAWITEMSTRUCT* dis);
    void   draw_emoji_tab_item (DRAWITEMSTRUCT* dis);
    void   insert_emoji_at_cursor(const std::string& glyph);

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
    static constexpr int kReactionH      = tesseract::visual::kReactionChipHeight;
    static constexpr int kReactionPad    = tesseract::visual::kReactionChipGap;
    static constexpr int kUserStripH     = tesseract::visual::kUserStripHeight;

    HINSTANCE hInst_;
    HWND      hwnd_       = nullptr;
    std::unique_ptr<LoginView> login_view_;
    bool      login_visible_ = false;
    HWND      hRoomList_   = nullptr;
    HWND      hRoomHeader_ = nullptr;
    HWND      hMsgList_    = nullptr;
    HWND      hInput_      = nullptr;
    HWND      hSend_       = nullptr;
    HWND      hEmoji_      = nullptr;
    HWND      hEmojiPicker_ = nullptr;   // floating WS_POPUP window
    HWND      hEmojiSearch_ = nullptr;   // EDIT inside the picker
    HWND      hEmojiGrid_   = nullptr;   // owner-drawn LISTBOX inside the picker
    HWND      hEmojiTabs_   = nullptr;   // owner-drawn LISTBOX bottom strip
    std::vector<std::string>          emoji_view_;   // current grid contents (UTF-8)
    std::vector<std::string>          emoji_tabs_;   // tab strip glyphs
    int                               emoji_tab_idx_ = 1;  // default: Smileys & People
    HWND      hStatus_     = nullptr;
    HWND      hRecoveryBanner_ = nullptr;
    HWND      hRecoveryLabel_  = nullptr;
    HWND      hRecoveryKeyEdit_ = nullptr;
    HWND      hRecoveryVerify_ = nullptr;
    HWND      hRecoveryDismiss_ = nullptr;
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
    std::vector<MessageData>         messages_;
    tesseract::RoomInfo              current_room_info_;
    std::string                      current_room_id_;
    std::string                      my_user_id_;

    ULONG_PTR  gdiplus_token_ = 0;
    std::unordered_map<std::string, Gdiplus::Bitmap*> avatar_cache_;
    std::unordered_map<std::string, Gdiplus::Bitmap*> user_avatar_cache_;

    static constexpr const wchar_t* CLASS_NAME  = L"TesseractMainWnd";
    static constexpr int            IDC_ROOMLIST = 101;
    static constexpr int            IDC_MSGLIST  = 102;
    static constexpr int            IDC_INPUT    = 103;
    static constexpr int            IDC_SEND     = 104;
    static constexpr int            IDC_EMOJI    = 105;
    static constexpr int            IDC_EMOJI_PICKER_SEARCH = 106;
    static constexpr int            IDC_EMOJI_PICKER_GRID   = 107;
    static constexpr int            IDC_EMOJI_PICKER_TABS   = 108;
    static constexpr int            IDC_RECOVERY_KEY     = 109;
    static constexpr int            IDC_RECOVERY_VERIFY  = 110;
    static constexpr int            IDC_RECOVERY_DISMISS = 111;
    static constexpr int            IDM_LOGOUT           = 120;
};

} // namespace win32
