#include "Win32Notifier.h"

#include "winrt_coroutine_shim.h"  // must precede any <winrt/...> include

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Data.Xml.Dom.h>

#include <sstream>

namespace WUN = winrt::Windows::UI::Notifications;
namespace WDX = winrt::Windows::Data::Xml::Dom;

namespace win32 {

Win32Notifier::Win32Notifier(HWND hwnd, std::string user_id)
    : hwnd_(hwnd), user_id_(std::move(user_id))
{}

// Converts a UTF-8 std::string to std::wstring via Win32 MultiByteToWideChar.
static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Escapes the five XML special characters in a string intended for element text.
static std::wstring xml_escape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'&':  out += L"&amp;";  break;
            case L'<':  out += L"&lt;";   break;
            case L'>':  out += L"&gt;";   break;
            case L'"':  out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default:    out += c;         break;
        }
    }
    return out;
}

std::wstring Win32Notifier::build_toast_xml(const std::string& sender,
                                             const std::string& room_name,
                                             const std::string& body)
{
    std::string preview_u8 = body;
    if (body.size() > 120) {
        // Walk back from byte 120 to the start of the current UTF-8 sequence
        // so we never split a multi-byte character.
        std::size_t cut = 120;
        while (cut > 0 && (body[cut] & 0xC0) == 0x80) --cut;
        preview_u8 = body.substr(0, cut) + "\xe2\x80\xa6";
    }

    const std::wstring wsender    = xml_escape(to_wide(sender));
    const std::wstring wroom      = xml_escape(to_wide(room_name));
    const std::wstring wpreview   = xml_escape(to_wide(preview_u8));

    std::wostringstream xml;
    xml << L"<toast>"
           L"<visual><binding template=\"ToastGeneric\">"
        << L"<text>" << wsender << L"</text>";
    if (room_name != sender)
        xml << L"<text>" << wroom << L"</text>";
    xml << L"<text>" << wpreview << L"</text>"
           L"</binding></visual>"
           L"</toast>";
    return xml.str();
}

void Win32Notifier::notify(const tesseract::Notification& n)
{
    try {
        WDX::XmlDocument doc;
        doc.LoadXml(build_toast_xml(n.sender, n.room_name, n.body));

        auto notifier = WUN::ToastNotificationManager::CreateToastNotifier(
            L"io.gnomos.Tesseract");
        auto toast = WUN::ToastNotification(doc);

        // Capture room_id + user_id for the click handler (runs on a WinRT thread-pool thread).
        HWND        hwnd    = hwnd_;
        std::string room_id = n.room_id;
        std::string user_id = user_id_;
        toast.Activated(
            winrt::Windows::Foundation::TypedEventHandler<
                WUN::ToastNotification,
                winrt::Windows::Foundation::IInspectable>{
                [hwnd, room_id, user_id](const WUN::ToastNotification&,
                                          const winrt::Windows::Foundation::IInspectable&) {
                    if (IsWindow(hwnd))
                        PostMessage(hwnd, WM_TESSERACT_NOTIFY_CLICK, 0,
                                    reinterpret_cast<LPARAM>(
                                        new NotifyClickPayload{room_id, user_id}));
                }});

        notifier.Show(toast);
    } catch (const winrt::hresult_error&) {
        // Toast infrastructure unavailable or AUMID not yet visible to the
        // notification system; silently swallow so the caller stays stable.
    }
}

} // namespace win32
