#pragma once
#include <tesseract/settings.h>

#include <string>

namespace tesseract::app
{

// Pure decision for MSC4278 media-preview suppression.
//
// Returns true when an inline media item (image / video / sticker) should be
// shown and fetched, false when it should be suppressed behind a click-to-load
// placeholder.
//
// Rules:
//   - revealed (the user already clicked the placeholder) → always shown
//   - On      → always shown
//   - Off     → always suppressed, including the user's own uploads
//               ("off means off" — no exceptions)
//   - Private → shown in non-public rooms; in a public room only the user's
//               own media is shown, everyone else's is suppressed
//
// `join_rule` is the room's join rule ("public", "invite", "restricted", …);
// an empty / unknown value is treated as public, per MSC4278. `is_own` is true
// when the event was sent by the logged-in user — their own media is never a
// privacy/safety/bandwidth concern to them, so it is exempt from the
// public-room suppression that Private mode applies to other senders.
inline bool media_allowed(tesseract::Settings::MediaPreviews mode,
                          const std::string& join_rule, bool is_own,
                          bool revealed)
{
    if (revealed)
    {
        return true;
    }
    switch (mode)
    {
    case tesseract::Settings::MediaPreviews::On:
        return true;
    case tesseract::Settings::MediaPreviews::Off:
        return false;
    case tesseract::Settings::MediaPreviews::Private:
    {
        const bool public_room = join_rule.empty() || join_rule == "public";
        return !public_room || is_own;
    }
    }
    return true;
}

} // namespace tesseract::app
