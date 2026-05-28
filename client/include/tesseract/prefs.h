#pragma once
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
};

/// Parse / serialize helpers for the `im.gnomos.tesseract` JSON content object.
/// Only needs to handle the simple `{"last_room":"!id:host"}` shape; no external
/// JSON library is required because room IDs never contain `"` or `\`.
namespace Prefs
{

/// Decode `json` (raw event content) into a `PrefsData`.
/// Unknown keys are silently ignored; missing keys use zero-values.
PrefsData parse(const std::string& json);

/// Encode `p` as a JSON object suitable for `Client::save_prefs_json()`.
std::string serialize(const PrefsData& p);

} // namespace Prefs

} // namespace tesseract
