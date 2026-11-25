#pragma once

// The warning before a room's world runs its scripts: who published it, what it is called, its
// hash, and Run once / Always run this world / Don't run. Built on ConfirmDialog (modal, keyboard
// operable, accessible); leaving it any other way than the first two buttons is "Don't run".

#include <functional>
#include <string>
#include <string_view>

namespace tesseract::views
{

class ConfirmDialog;

enum class ConsentChoice
{
    RunOnce,
    AlwaysRun,
    DontRun
};

// What the dialog and the reload banner say about a world. The values come from a state event any
// room member with the power level could write: sanitize_world_text before showing them.
struct WorldConsentRequest
{
    std::string name;
    std::string sender; // the event's authenticated sender
    std::string hash;
};

// A Matrix user id may be 255 characters: do not cut a sender shorter.
constexpr std::size_t kMaxSenderChars = 255;

// Text safe to show: control characters and the bidi / zero-width formatting characters are
// removed, invalid UTF-8 becomes U+FFFD, and the result is cut to `max_chars` code points (an
// ellipsis marks the cut).
std::string sanitize_world_text(std::string_view text, std::size_t max_chars = 64);

// Opens `dialog` for `request`; `answer` is called exactly once with the user's choice.
void open_world_consent_dialog(ConfirmDialog& dialog, const WorldConsentRequest& request,
                               std::function<void(ConsentChoice)> answer);

} // namespace tesseract::views
