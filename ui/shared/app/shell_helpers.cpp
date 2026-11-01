#include "app/shell_helpers.h"

#include "tk/i18n.h"
#include "views/map_tiles.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tesseract::shell_helpers
{

MutualRoomsPage parse_mutual_rooms_json(const std::string& json)
{
    MutualRoomsPage page;
    const auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object())
    {
        page.error = "invalid response";
        return page;
    }
    if (const auto e = j.find("error"); e != j.end() && e->is_string())
    {
        page.error = e->get<std::string>();
    }
    if (const auto b = j.find("next_batch"); b != j.end() && b->is_string())
    {
        page.next_batch = b->get<std::string>();
    }
    if (const auto c = j.find("count"); c != j.end() && c->is_number_integer())
    {
        page.count = c->get<int>();
    }
    if (const auto a = j.find("joined"); a != j.end() && a->is_array())
    {
        for (const auto& id : *a)
        {
            if (id.is_string())
            {
                page.joined.push_back(id.get<std::string>());
            }
        }
    }
    return page;
}

std::string gallery_caption_or_fallback(const std::string& caption, std::size_t item_count)
{
    if (!caption.empty())
    {
        return caption;
    }
    return tk::trf(tk::trn("Sent {0} item", "Sent {0} items", static_cast<long>(item_count)),
                   {std::to_string(item_count)});
}

Settings::WindowGeometry clamp_to_screens(const Settings::WindowGeometry& saved, int default_w,
                                          int default_h, const std::vector<tk::Rect>& screens)
{
    if (!saved.valid)
    {
        return {};
    }

    // Check whether the title-bar strip overlaps any screen work area.
    const float kTitleH = 50.f;
    const float sx = static_cast<float>(saved.x);
    const float sy = static_cast<float>(saved.y);
    const float sw = static_cast<float>(saved.w);
    bool on_screen = false;
    for (const auto& s : screens)
    {
        const float ox = std::max(sx, s.x);
        const float oy = std::max(sy, s.y);
        const float ow = std::min(sx + sw, s.x + s.w) - ox;
        const float oh = std::min(sy + kTitleH, s.y + s.h) - oy;
        if (ow > 0.f && oh > 0.f)
        {
            on_screen = true;
            break;
        }
    }
    if (on_screen)
    {
        return saved;
    }

    // Re-centre on the first available screen with the saved size.
    const tk::Rect fallback{0.f, 0.f, static_cast<float>(default_w), static_cast<float>(default_h)};
    const tk::Rect& primary = screens.empty() ? fallback : screens[0];
    const int w = saved.w > 0 ? std::min(saved.w, static_cast<int>(primary.w * 0.9f))
                              : std::min(default_w, static_cast<int>(primary.w * 0.9f));
    const int h = saved.h > 0 ? std::min(saved.h, static_cast<int>(primary.h * 0.9f))
                              : std::min(default_h, static_cast<int>(primary.h * 0.9f));
    Settings::WindowGeometry result;
    result.x = static_cast<int>(primary.x) + (static_cast<int>(primary.w) - w) / 2;
    result.y = static_cast<int>(primary.y) + (static_cast<int>(primary.h) - h) / 2;
    result.w = w;
    result.h = h;
    result.valid = true;
    return result;
}

std::pair<bool, bool>
compute_tray_unread(const std::unordered_map<std::string, std::vector<RoomInfo>>& by_account)
{
    bool has_unread = false;
    bool has_highlight = false;
    for (const auto& [_uid, rooms] : by_account)
    {
        for (const auto& r : rooms)
        {
            if (r.notification_count > 0)
            {
                has_unread = true;
            }
            if (r.highlight_count > 0)
            {
                has_highlight = true;
            }
            if (has_unread && has_highlight)
            {
                return {true, true};
            }
        }
    }
    return {has_unread, has_highlight};
}

bool account_has_unread(const std::vector<RoomInfo>& rooms)
{
    for (const auto& r : rooms)
    {
        if (r.notification_count > 0)
        {
            return true;
        }
    }
    return false;
}

std::string find_existing_dm(const std::vector<RoomInfo>& rooms, const std::string& user_id)
{
    if (user_id.empty())
    {
        return {};
    }
    for (const auto& r : rooms)
    {
        if (r.is_direct && r.dm_counterpart_user_id == user_id)
        {
            return r.id;
        }
    }
    return {};
}

std::vector<std::string> alias_server_via(const std::string& alias)
{
    const auto first = alias.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || alias[first] != '#')
    {
        return {};
    }
    const auto colon = alias.find(':', first);
    if (colon == std::string::npos)
    {
        return {};
    }
    const auto last = alias.find_last_not_of(" \t\r\n");
    if (last <= colon)
    {
        return {};
    }
    return {alias.substr(colon + 1, last - colon)};
}

std::string format_typing_text(const std::vector<std::string>& names)
{
    if (names.empty())
    {
        return {};
    }
    if (names.size() == 1)
    {
        return tk::trf(tk::tr("{0} is typing\xe2\x80\xa6"), {names[0]});
    }
    if (names.size() == 2)
    {
        return tk::trf(tk::tr("{0} and {1} are typing\xe2\x80\xa6"), {names[0], names[1]});
    }
    const long others = static_cast<long>(names.size() - 2);
    return tk::trf(tk::trn("{0}, {1} and {2} other are typing\xe2\x80\xa6",
                           "{0}, {1} and {2} others are typing\xe2\x80\xa6", others),
                   {names[0], names[1], std::to_string(others)});
}

// Resolve a MediaPreviewConfig::Mode → the Settings mirror enum (identical
// order, but kept explicit so the two stay decoupled).
tesseract::Settings::MediaPreviews mode_to_settings(tesseract::MediaPreviewConfig::Mode m)
{
    switch (m)
    {
    case tesseract::MediaPreviewConfig::Mode::Off:
        return tesseract::Settings::MediaPreviews::Off;
    case tesseract::MediaPreviewConfig::Mode::Private:
        return tesseract::Settings::MediaPreviews::Private;
    case tesseract::MediaPreviewConfig::Mode::On:
    default:
        return tesseract::Settings::MediaPreviews::On;
    }
}

// Map PresenceTracker's enum to the Client::PresenceState the FFI accepts.
tesseract::PresenceState to_client_presence(PresenceTracker::State s)
{
    switch (s)
    {
    case PresenceTracker::State::Online:
        return PresenceState::Online;
    case PresenceTracker::State::Unavailable:
        return PresenceState::Unavailable;
    case PresenceTracker::State::Offline:
        return PresenceState::Offline;
    }
    return PresenceState::Offline;
}

tk::AccentTheme to_tk_accent(tesseract::Settings::ThemeAccent accent)
{
    using SA = tesseract::Settings::ThemeAccent;
    switch (accent)
    {
    case SA::Forest:
        return tk::AccentTheme::Forest;
    case SA::Sunset:
        return tk::AccentTheme::Sunset;
    case SA::Violet:
        return tk::AccentTheme::Violet;
    case SA::System:
        return tk::AccentTheme::System;
    case SA::Blue:
        break;
    }
    return tk::AccentTheme::Blue;
}

std::wstring quote_command_line_arg(std::wstring_view arg)
{
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (c == L'"')
        {
            // Backslashes before a quote are doubled, and the quote is escaped.
            out.append(backslashes * 2 + 1, L'\\');
        }
        else
        {
            out.append(backslashes, L'\\');
        }
        out.push_back(c);
        backslashes = 0;
    }
    // Backslashes before the closing quote are doubled too.
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// Write `text` (a secret — the recovery key) to the UTF-8 `path`. On POSIX
// the file is owner-only (0600) from the moment it exists — including when
// overwriting an existing file — and failing to make it so is an error, not
// a silently world-readable key. Windows has no mode bits; files in the
// user's profile already inherit per-user ACLs.
bool write_private_text_file(const std::string& path, const std::string& text, std::string& error)
{
#ifdef _WIN32
    namespace fs = std::filesystem;
    const fs::path p(reinterpret_cast<const char8_t*>(path.c_str()));
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (f)
    {
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (f)
    {
        f.close();
    }
    if (!f)
    {
        error = tk::tr("The file couldn't be written.");
        return false;
    }
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        error = std::strerror(errno);
        return false;
    }
    auto fail = [&](int err)
    {
        error = std::strerror(err);
        ::close(fd);
        return false;
    };
    // O_CREAT's mode only applies to a new file; tighten an existing one
    // before any of the secret goes in.
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
    {
        return fail(errno);
    }
    const char* data = text.data();
    std::size_t left = text.size();
    while (left > 0)
    {
        const ssize_t n = ::write(fd, data, left);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return fail(errno);
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0)
    {
        error = std::strerror(errno);
        return false;
    }
    return true;
#endif
}

std::vector<std::uint8_t> read_tile_disk(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return bytes;
    }
    {
        std::ifstream f(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (!views::is_png_bytes(bytes))
    {
        std::filesystem::remove(path, ec);
        bytes.clear();
    }
    return bytes;
}

bool write_tile_disk(const std::filesystem::path& path, const std::vector<std::uint8_t>& png)
{
    if (!views::is_png_bytes(png))
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        return false;
    }
    auto part = path;
    part += ".part";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(png.data()),
                static_cast<std::streamsize>(png.size()));
        f.close();
        if (!f)
        {
            std::filesystem::remove(part, ec);
            return false;
        }
    }
    std::filesystem::rename(part, path, ec);
    if (ec)
    {
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

} // namespace tesseract::shell_helpers
