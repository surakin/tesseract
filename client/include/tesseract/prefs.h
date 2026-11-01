#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace tesseract
{

/// Per-account user preferences, persisted as the `im.gnomos.tesseract`
/// Matrix global account-data event so they follow the user across devices.
struct PrefsData
{
    std::string last_room; ///< Active tab room ID. Empty when none.
    /// All open tab room IDs in visual order; includes last_room.
    /// Empty on first launch or when migrating from old prefs.
    std::vector<std::string> open_rooms;
    /// Room IDs the user has locally marked "not actually bridged", overriding
    /// the MSC2346 auto-detection (`RoomInfo::is_bridged`) for that room. See
    /// `ShellBase::room_effectively_bridged_()`. Empty by default.
    std::vector<std::string> bridge_not_bridged_overrides;
    /// Default emoji skin tone as an `emoji::skin_tone_key()` string
    /// ("medium", …). Empty = default (yellow) presentation.
    std::string emoji_skin_tone;
    /// Most-recently-visited room IDs (front = most recent), feeding the
    /// Ctrl+Tab MRU switcher and the quick switcher's "Recent" strip.
    std::vector<std::string> recent_rooms;
    /// Room-directory servers the user has browsed, most recent first, at
    /// most `kMaxDirectoryServers`. Only servers whose directory answered.
    std::vector<std::string> directory_servers;
    /// Space room IDs the user pinned to the space picker sidebar, in the
    /// order the tabs are shown. Pinned spaces are hidden from the room list.
    std::vector<std::string> pinned_spaces;
};

/// Cap on the saved `directory_servers` list.
inline constexpr std::size_t kMaxDirectoryServers = 8;

/// Parse / serialize helpers for the `im.gnomos.tesseract` JSON content object.
namespace Prefs
{

/// Decode `json` (raw event content) into a `PrefsData`.
/// Unknown keys are silently ignored; missing keys use zero-values.
PrefsData parse(const std::string& json);

/// Encode `p` as a JSON object suitable for `Client::save_prefs_json()`.
/// When `base_json` is a JSON object (normally the currently stored event
/// content), `p`'s fields are overlaid onto it so keys this build doesn't
/// know about survive the save instead of being wiped.
std::string serialize(const PrefsData& p, const std::string& base_json = {});

/// Build the room-layout prefs to persist after a room switch / tab change.
/// `last_room` is the active room; `open_room_ids` are the open tabs in visual
/// order. When no tabs are open the active room is used as the sole open room
/// (so the layout always round-trips). PrefsData carries only these two fields,
/// so this fully reconstructs what to persist — no prior load is needed.
PrefsData room_layout(const std::string& last_room, const std::vector<std::string>& open_room_ids);

/// `p` with the room-layout fields forgotten (active room, open tabs, recent
/// rooms); every other field is kept. Used when the layout is reset but the
/// account's other synced preferences must survive (Clear Cache).
PrefsData without_layout(PrefsData p);

/// Returns `saved` with `server` moved (or added) to the front, matched
/// case-insensitively, trimmed to `kMaxDirectoryServers`. An empty `server`
/// leaves the list unchanged.
std::vector<std::string> add_directory_server(std::vector<std::string> saved,
                                              const std::string& server);

/// The servers the room browser offers: `saved` first, then `homeserver` and
/// the built-in defaults (activematrixrooms.com, matrixrooms.info) that are
/// not already listed, with the whole list trimmed to `kMaxDirectoryServers`.
/// Defaults drop off the end once enough servers have been saved. An empty
/// `homeserver` is skipped.
std::vector<std::string> directory_server_choices(const std::vector<std::string>& saved,
                                                  const std::string& homeserver);

} // namespace Prefs

} // namespace tesseract
