#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <tesseract/client.h>
#include <tesseract/event_handler.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

// Custom window messages
constexpr UINT WM_TESSERACT_MESSAGE    = WM_APP + 1;
constexpr UINT WM_TESSERACT_ROOMS      = WM_APP + 2;
constexpr UINT WM_TESSERACT_SYNC_ERROR = WM_APP + 3;

namespace win32 {

struct PendingMessage {
    tesseract::Message msg;
};

/// Win32 event handler: marshals Rust callbacks onto the UI thread via PostMessage.
class EventHandler final : public tesseract::IEventHandler {
public:
    explicit EventHandler(HWND hwnd) : hwnd_(hwnd) {}

    void on_message(const tesseract::Message& msg) override;
    void on_rooms_updated(const std::vector<tesseract::RoomInfo>& rooms) override;
    void on_sync_error(const std::string& context,
                       const std::string& description) override;
    void on_session_saved(const std::string& session_json) override;

private:
    HWND hwnd_;
};

// ---------------------------------------------------------------------------

class MainWindow {
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
    void on_login_clicked();
    void on_send_clicked();
    void on_room_selected(int index);
    void on_tesseract_message(tesseract::Message* msg);
    void on_tesseract_rooms(std::vector<tesseract::RoomInfo>* rooms);

    void layout_controls();
    void append_message(const std::string& sender, const std::string& body);

    // ---- handles ----
    HINSTANCE hInst_;
    HWND      hwnd_       = nullptr;
    HWND      hRoomList_  = nullptr;
    HWND      hMsgView_   = nullptr;
    HWND      hInput_     = nullptr;
    HWND      hSend_      = nullptr;
    HWND      hStatus_    = nullptr;

    // ---- state ----
    tesseract::Client             client_;
    std::unique_ptr<EventHandler>    event_handler_;
    std::vector<tesseract::RoomInfo>    rooms_;
    std::string                      current_room_id_;

    static constexpr const wchar_t* CLASS_NAME = L"TesseractMainWnd";
    static constexpr int            IDC_ROOMLIST = 101;
    static constexpr int            IDC_MSGVIEW  = 102;
    static constexpr int            IDC_INPUT    = 103;
    static constexpr int            IDC_SEND     = 104;
};

} // namespace win32
