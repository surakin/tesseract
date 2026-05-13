#pragma once
#include <tesseract/notifier.h>
#include <windows.h>
#include <string>

namespace win32 {

class Win32Notifier final : public tesseract::INotifier {
public:
    explicit Win32Notifier(HWND hwnd) : hwnd_(hwnd) {}
    void notify(const tesseract::Notification& n) override;

private:
    HWND hwnd_;
    static std::wstring build_toast_xml(const std::string& sender,
                                         const std::string& room_name,
                                         const std::string& body);
};

} // namespace win32
