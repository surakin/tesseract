#include "views/sdk_text.h"

#include "tk/i18n.h"

#include <cstdlib>

namespace tesseract::views::sdk_text
{

namespace
{
constexpr char kMark = '\x01';

std::string counted(const char* one, const char* many, const std::string& arg)
{
    const long n = std::strtol(arg.c_str(), nullptr, 10);
    return tk::trf(tk::trn(one, many, n), {arg});
}
} // namespace

bool is_code(std::string_view text, std::string_view name)
{
    if (text.empty() || text.front() != kMark)
        return false;
    text.remove_prefix(1);
    return text.substr(0, text.find(kMark)) == name;
}

std::string localize(std::string_view text)
{
    if (text.empty() || text.front() != kMark)
        return std::string(text);

    text.remove_prefix(1);
    std::string name(text.substr(0, text.find(kMark)));
    std::string arg;
    if (const auto at = text.find(kMark); at != std::string_view::npos)
        arg = std::string(text.substr(at + 1));

    // Previews of non-text messages.
    if (name == "image")
        return tk::tr("(image)");
    if (name == "file")
        return tk::tr("(file)");
    if (name == "voice")
        return tk::tr("(voice)");
    if (name == "audio")
        return tk::tr("(audio)");
    if (name == "video")
        return tk::tr("(video)");
    if (name == "gallery")
        return tk::tr("(gallery)");
    if (name == "message")
        return tk::tr("(message)");
    if (name == "sticker")
        return tk::tr("(sticker)");
    if (name == "deleted")
        return tk::tr("(deleted)");
    if (name == "unavailable")
        return tk::tr("(unavailable)");
    if (name == "poll")
        return arg.empty() ? tk::tr("(poll)") : tk::trf(tk::tr("(poll) {0}"), {arg});

    // Why a message couldn't be decrypted.
    if (name == "utd_sent_before_join")
        return tk::tr("🔒 Sent before you joined this room");
    if (name == "utd_identity_changed")
        return tk::tr("🔒 Sender's identity changed since you verified them");
    if (name == "utd_unsigned_device")
        return tk::tr("🔒 Sender's device is not signed");
    if (name == "utd_unknown_device")
        return tk::tr("🔒 Sender's device is unknown");
    if (name == "utd_backup_off")
        return tk::tr("🔒 History unavailable (key backup is off)");
    if (name == "utd_withheld_insecure")
        return tk::tr("🔒 Sender blocked this device");
    if (name == "utd_withheld_sender")
        return tk::tr("🔒 Key was not shared with this device");
    if (name == "utd_verify_device")
        return tk::tr("🔒 Verify this device to access history");
    if (name == "utd_unknown")
        return tk::tr("🔒 Unable to decrypt");

    // What a change to the pinned messages did (shown after the sender's name).
    if (name == "pin_cleared")
        return tk::tr("cleared all pinned messages");
    if (name == "pin_added")
        return tk::tr("pinned a message");
    if (name == "pin_removed")
        return tk::tr("unpinned a message");
    if (name == "pin_added_n")
        return counted("pinned {0} message", "pinned {0} messages", arg);
    if (name == "pin_removed_n")
        return counted("unpinned {0} message", "unpinned {0} messages", arg);
    if (name == "pin_changed")
        return tk::tr("changed the pinned messages");

    return name; // a code from a newer SDK: better its name than a control character
}

} // namespace tesseract::views::sdk_text
