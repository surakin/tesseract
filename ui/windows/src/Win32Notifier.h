#pragma once
#include <tesseract/notifier.h>
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

// Posted by Win32Notifier to the main window when a toast is activated.
// lParam is a heap-allocated NotifyClickPayload* (deleted by wnd_proc).
constexpr UINT WM_TESSERACT_NOTIFY_CLICK = WM_APP + 18;

namespace win32
{

// Payload for WM_TESSERACT_NOTIFY_CLICK. Allocated on the heap by
// Win32Notifier and deleted by the wnd_proc handler.
struct NotifyClickPayload
{
    std::string room_id;
    std::string user_id;
};

class Win32Notifier final : public tesseract::INotifier
{
public:
    Win32Notifier(HWND hwnd, std::string user_id);
    ~Win32Notifier() override;
    void notify(const tesseract::Notification& n) override;

private:
    HWND hwnd_;
    std::string user_id_;
    // Shared with every toast's Activated handler (which runs on a WinRT
    // thread-pool thread, possibly after this notifier and its window are
    // gone). The handler reads the HWND through this box; the destructor
    // zeroes it, so a late activation never posts to a destroyed or recycled
    // window — the old captured-HWND + IsWindow() check was a TOCTOU race.
    std::shared_ptr<std::atomic<HWND>> hwnd_box_;
    static std::wstring build_toast_xml(const std::string& sender,
                                        const std::string& room_name,
                                        const std::string& body,
                                        const std::wstring& avatar_uri,
                                        const std::wstring& image_uri);
};

} // namespace win32
