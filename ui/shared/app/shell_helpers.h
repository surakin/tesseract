#pragma once

// Pure helpers lifted out of ShellBase.cpp (no `this`, no shell state). Where
// the shells or tests call one through ShellBase (clamp_to_screens_,
// compute_tray_unread, account_has_unread, find_existing_dm), ShellBase keeps
// a one-line forwarding static.

#include <tesseract/client.h>
#include <tesseract/settings.h>
#include <tesseract/types.h>
#include "app/PresenceTracker.h"
#include "tk/canvas.h"
#include "tk/theme.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tesseract::shell_helpers
{

// Re-centres saved window geometry on the first screen when its title-bar
// strip is off every screen work area; returns it unchanged otherwise.
Settings::WindowGeometry clamp_to_screens(const Settings::WindowGeometry& saved, int default_w,
                                          int default_h, const std::vector<tk::Rect>& screens);

// (any unread, any highlight) across every account's rooms.
std::pair<bool, bool>
compute_tray_unread(const std::unordered_map<std::string, std::vector<RoomInfo>>& by_account);
bool account_has_unread(const std::vector<RoomInfo>& rooms);

// One page of an MSC2666 mutual-rooms reply, parsed from the SDK's JSON
// `{joined, count, next_batch, error}`. Malformed input yields an `error`.
struct MutualRoomsPage
{
    std::vector<std::string> joined;
    int count = 0; ///< total shared rooms the server reports
    std::string next_batch;
    std::string error;
};
MutualRoomsPage parse_mutual_rooms_json(const std::string& json);

// Room id of the existing DM with user_id, or empty.
std::string find_existing_dm(const std::vector<RoomInfo>& rooms, const std::string& user_id);

// The text an m.gallery event carries when the user typed no caption. The
// event's body is a required field and what clients without galleries show, so
// it is "Sent N items" in the sender's language.
std::string gallery_caption_or_fallback(const std::string& caption, std::size_t item_count);

// Server name of a `#alias:server` room alias as a one-element `via` list;
// empty for anything else (room ids, malformed input). A room reached through
// its alias is joined by the resolved room id, which may carry no usable server
// part (room v12), so the alias's server is the only route to remember.
std::vector<std::string> alias_server_via(const std::string& alias);

// "A is typing…" / "A and B are typing…" / "A, B and N others are typing…".
std::string format_typing_text(const std::vector<std::string>& names);

tesseract::Settings::MediaPreviews mode_to_settings(tesseract::MediaPreviewConfig::Mode m);
tesseract::PresenceState to_client_presence(PresenceTracker::State s);
tk::AccentTheme to_tk_accent(tesseract::Settings::ThemeAccent accent);

// Write a secret to `path` owner-only (0600 on POSIX); sets `error` on failure.
bool write_private_text_file(const std::string& path, const std::string& text, std::string& error);

// Quote `arg` for a Windows command line, so CommandLineToArgvW and the C runtime
// read it back as one argument. An embedded quote is escaped, and each backslash
// that precedes a quote, or the closing quote, is doubled. Other text is left as
// it is, so an ordinary value gets the same string it always did.
std::wstring quote_command_line_arg(std::wstring_view arg);

// Map tile disk cache. A cached tile is served only when it is a PNG: an entry
// that is empty or not a PNG (a crash mid-write, an older build) is removed, so
// it is fetched again rather than shown for ever. Returns empty on a miss.
std::vector<std::uint8_t> read_tile_disk(const std::filesystem::path& path);

// Stores `png` under a temporary name and renames it into place, so a crash
// never leaves a truncated tile under the real name. Refuses anything that is
// not a PNG, so a tile-server error page is never kept. Returns false on failure.
bool write_tile_disk(const std::filesystem::path& path, const std::vector<std::uint8_t>& png);

} // namespace tesseract::shell_helpers
