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

namespace win32 {

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

private:
    HWND hwnd_;
};

// ---------------------------------------------------------------------------

struct MessageData {
    std::string          body;
    std::string          sender;
    std::string          sender_name;
    std::string          sender_avatar_url;
    uint64_t             timestamp  = 0;
    bool                 is_own     = false;
    tesseract::EventType type       = tesseract::EventType::Text;
};

// ---------------------------------------------------------------------------

class MainWindow {
public:
    static bool register_class(HINSTANCE hInst);
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK input_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                                 UINT_PTR, DWORD_PTR);

    explicit MainWindow(HINSTANCE hInst);
    ~MainWindow();

    bool create(int nCmdShow);

private:
    void on_create(HWND hwnd);
    void on_destroy();
    void on_size(int w, int h);
    void on_login_clicked();
    void on_send_clicked();
    void on_room_selected(int index);
    void on_tesseract_message(tesseract::Event* ev);
    void on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms);
    void on_tesseract_timeline_reset(std::string* room_id);

    void on_reconnect();
    void on_auth_error(bool soft_logout);
    void append_message(const tesseract::Event& ev);
    void clear_messages();

    int  compute_message_height(size_t idx);
    void draw_room_item(DRAWITEMSTRUCT* dis);
    void draw_message_item(DRAWITEMSTRUCT* dis);

    Gdiplus::Bitmap* get_room_avatar(const std::string& room_id);
    Gdiplus::Bitmap* get_user_avatar(const std::string& mxc_url);
    void draw_circle_bitmap(Gdiplus::Graphics& g, Gdiplus::Bitmap* bmp,
                             int x, int y, int size);
    void draw_initials_circle(Gdiplus::Graphics& g, const std::string& name,
                               int x, int y, int size);
    static void fill_rounded_rect(Gdiplus::Graphics& g, Gdiplus::Brush& brush,
                                   float x, float y, float w, float h, float r);

    static constexpr int kRoomAvatarSize = 36;
    static constexpr int kMsgAvatarSize  = 32;
    static constexpr int kRoomRowH       = 62;
    static constexpr int kMsgRowPad      = 6;
    static constexpr int kBubblePadX     = 12;
    static constexpr int kBubblePadY     = 8;
    static constexpr int kBubbleRadius   = 12;
    static constexpr int kMaxBubbleWidth = 420;

    HINSTANCE hInst_;
    HWND      hwnd_       = nullptr;
    HWND      hRoomList_  = nullptr;
    HWND      hMsgList_   = nullptr;
    HWND      hInput_     = nullptr;
    HWND      hSend_      = nullptr;
    HWND      hStatus_    = nullptr;

    tesseract::Client                client_;
    std::unique_ptr<EventHandler>    event_handler_;
    std::vector<tesseract::RoomInfo> rooms_;
    std::vector<MessageData>         messages_;
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
};

} // namespace win32
