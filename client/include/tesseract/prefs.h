#pragma once
#include <string>

namespace tesseract {

/// Lightweight key/value store for non-sensitive user preferences that
/// should survive restarts. Each preference is a plain UTF-8 text file
/// in `config_dir()`. Currently stores:
///
///   last_room.txt  — the Matrix room-ID the user had open when they quit.
///
/// Unlike `SessionStore`, nothing here is secret — no atomic-rename dance
/// is needed; a direct overwrite is acceptable.
class Prefs {
public:
    /// Returns the last-open room ID, or an empty string if not set.
    static std::string load_last_room();

    /// Persists `room_id` as the last-open room. A no-op (but safe) if
    /// `room_id` is empty. Creates the config directory if missing.
    static void save_last_room(const std::string& room_id);
};

} // namespace tesseract
