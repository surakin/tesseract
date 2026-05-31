#include "Win32Notifier.h"

#include "winrt_coroutine_shim.h" // must precede any <winrt/...> include

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Data.Xml.Dom.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

namespace WUN = winrt::Windows::UI::Notifications;
namespace WDX = winrt::Windows::Data::Xml::Dom;

namespace win32
{

Win32Notifier::Win32Notifier(HWND hwnd, std::string user_id)
    : hwnd_(hwnd),
      user_id_(std::move(user_id)),
      hwnd_box_(std::make_shared<std::atomic<HWND>>(hwnd))
{
}

Win32Notifier::~Win32Notifier()
{
    // Signal any outstanding toast Activated handlers that the window is gone.
    // Each handler holds its own shared_ptr to the box, so this store is safe
    // even as they may still run on a thread-pool thread.
    if (hwnd_box_)
    {
        hwnd_box_->store(nullptr);
    }
}

// Converts a UTF-8 std::string to std::wstring via Win32 MultiByteToWideChar.
static std::wstring to_wide(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0)
    {
        return {};
    }
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Escapes the five XML special characters in a string intended for element text.
static std::wstring xml_escape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'&':
            out += L"&amp;";
            break;
        case L'<':
            out += L"&lt;";
            break;
        case L'>':
            out += L"&gt;";
            break;
        case L'"':
            out += L"&quot;";
            break;
        case L'\'':
            out += L"&apos;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// Wide → UTF-8 (inverse of to_wide), for percent-encoding a file path.
static std::string to_utf8(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    if (n <= 0)
    {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// Build a `file:///` URI for the toast `src` attribute. The toast XML
// loader needs a percent-encoded URI, not a raw Win32 path: encode the
// UTF-8 bytes, keeping the unreserved set plus '/' and ':' (the drive
// colon must stay literal — `file:///C:/...`).
static std::wstring to_file_uri(const std::filesystem::path& p)
{
    std::wstring native = p.wstring();
    for (auto& c : native)
    {
        if (c == L'\\')
        {
            c = L'/';
        }
    }
    const std::string u8 = to_utf8(native);

    static const char* hex = "0123456789ABCDEF";
    std::wstring uri = L"file:///";
    for (unsigned char ch : u8)
    {
        bool unreserved = (ch >= 'A' && ch <= 'Z') ||
                          (ch >= 'a' && ch <= 'z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                          ch == '.' || ch == '~' || ch == '/' || ch == ':';
        if (unreserved)
        {
            uri += static_cast<wchar_t>(ch);
        }
        else
        {
            uri += L'%';
            uri += static_cast<wchar_t>(hex[(ch >> 4) & 0xF]);
            uri += static_cast<wchar_t>(hex[ch & 0xF]);
        }
    }
    return uri;
}

// Persist the room-avatar bytes to a stable per-room temp file and return
// its path. Reused (overwritten) per room so the temp dir doesn't grow
// without bound; the avatar for a given room rarely changes. Returns an
// empty path when there are no bytes or the write fails (→ no <image>,
// the toast falls back to the app icon). `kind` ("avatar" / "image")
// keeps the avatar and message-picture temp files from colliding for the
// same room. Reused (overwritten) per (room, kind) so the temp dir stays
// bounded.
static std::filesystem::path
write_notif_image_temp(const std::vector<std::uint8_t>& bytes,
                       const std::string& room_id, const char* kind)
{
    if (bytes.empty())
    {
        return {};
    }

    const char* ext = ".png";
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
        bytes[2] == 0xFF)
    {
        ext = ".jpg";
    }
    else if (bytes.size() >= 4 && bytes[0] == 'G' && bytes[1] == 'I' &&
             bytes[2] == 'F' && bytes[3] == '8')
    {
        ext = ".gif";
    }
    else if (bytes.size() >= 12 && bytes[0] == 'R' && bytes[1] == 'I' &&
             bytes[2] == 'F' && bytes[3] == 'F' && bytes[8] == 'W' &&
             bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
    {
        ext = ".webp";
    }

    std::error_code ec;
    std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / L"Tesseract" / L"notif";
    if (ec)
    {
        return {};
    }
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        return {};
    }

    std::filesystem::path file =
        dir /
        (std::to_string(std::hash<std::string>{}(room_id)) + "-" + kind + ext);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return {};
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        return {};
    }
    return file;
}

std::wstring Win32Notifier::build_toast_xml(const std::string& sender,
                                            const std::string& room_name,
                                            const std::string& body,
                                            const std::wstring& avatar_uri,
                                            const std::wstring& image_uri)
{
    std::string preview_u8 = body;
    if (body.size() > 120)
    {
        // Walk back from byte 120 to the start of the current UTF-8 sequence
        // so we never split a multi-byte character.
        std::size_t cut = 120;
        while (cut > 0 && (body[cut] & 0xC0) == 0x80)
        {
            --cut;
        }
        preview_u8 = body.substr(0, cut) + "\xe2\x80\xa6";
    }

    const std::wstring wsender = xml_escape(to_wide(sender));
    const std::wstring wroom = xml_escape(to_wide(room_name));
    const std::wstring wpreview = xml_escape(to_wide(preview_u8));

    std::wostringstream xml;
    xml << L"<toast>"
           L"<visual><binding template=\"ToastGeneric\">"
        << L"<text>" << wsender << L"</text>";
    if (room_name != sender)
    {
        xml << L"<text>" << wroom << L"</text>";
    }
    xml << L"<text>" << wpreview << L"</text>";
    if (!avatar_uri.empty())
    {
        xml << L"<image placement=\"appLogoOverride\" hint-crop=\"circle\" "
               L"src=\""
            << xml_escape(avatar_uri) << L"\"/>";
    }
    // Message image / sticker: a large inline picture below the text.
    if (!image_uri.empty())
    {
        xml << L"<image src=\"" << xml_escape(image_uri) << L"\"/>";
    }
    xml << L"</binding></visual>"
           L"</toast>";
    return xml.str();
}

void Win32Notifier::notify(const tesseract::Notification& n)
{
    try
    {
        std::wstring avatar_uri;
        if (std::filesystem::path p =
                write_notif_image_temp(n.avatar_bytes, n.room_id, "avatar");
            !p.empty())
        {
            avatar_uri = to_file_uri(p);
        }
        std::wstring image_uri;
        if (std::filesystem::path p =
                write_notif_image_temp(n.image_bytes, n.room_id, "image");
            !p.empty())
        {
            image_uri = to_file_uri(p);
        }

        WDX::XmlDocument doc;
        doc.LoadXml(build_toast_xml(n.sender, n.room_name, n.body, avatar_uri,
                                    image_uri));

        auto notifier = WUN::ToastNotificationManager::CreateToastNotifier(
            L"io.gnomos.Tesseract");
        auto toast = WUN::ToastNotification(doc);

        // Capture room_id + user_id for the click handler (runs on a WinRT thread-pool thread).
        HWND hwnd = hwnd_;
        std::string room_id = n.room_id;
        std::string user_id = user_id_;
        toast.Activated(winrt::Windows::Foundation::TypedEventHandler<
                        WUN::ToastNotification,
                        winrt::Windows::Foundation::IInspectable>{
            [hwnd, room_id,
             user_id](const WUN::ToastNotification&,
                      const winrt::Windows::Foundation::IInspectable&)
            {
                if (IsWindow(hwnd))
                {
                    PostMessage(hwnd, WM_TESSERACT_NOTIFY_CLICK, 0,
                                reinterpret_cast<LPARAM>(
                                    new NotifyClickPayload{room_id, user_id}));
                }
            }});

        notifier.Show(toast);
    }
    catch (const winrt::hresult_error&)
    {
        // Toast infrastructure unavailable or AUMID not yet visible to the
        // notification system; silently swallow so the caller stays stable.
    }
}

} // namespace win32
