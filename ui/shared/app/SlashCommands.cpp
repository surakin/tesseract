#include "SlashCommands.h"

#include <tesseract/client.h>

namespace tesseract
{

namespace
{

// `prefix` matched against `s` (case-sensitive). Returns the suffix on a
// match, or `nullptr` if `s` does not start with `prefix`.
const char* strip_prefix(const std::string& s, const char* prefix)
{
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (s.size() < n) return nullptr;
    if (s.compare(0, n, prefix) != 0) return nullptr;
    return s.c_str() + n;
}

}  // namespace

const std::vector<SlashCommandDescriptor>& available_commands()
{
    static const std::vector<SlashCommandDescriptor> kCommands = {
        {"me",    "<action>", "Send an action message"},
        {"shrug", "",         "Append \xC2\xAF\\_(ツ)_/\xC2\xAF"},
    };
    return kCommands;
}

Result dispatch_compose_send(Client& client,
                             const std::string& room_id,
                             const std::string& body,
                             const std::string& formatted_body)
{
    // `/shrug` (no args) — append the shrug emoticon to whatever the user
    // typed in front of the slash. With no leading text it sends just the
    // emoticon. Sent as plain text via the normal send_message path so it
    // threads / replies correctly.
    {
        const char* suffix = nullptr;
        if (body == "/shrug")
        {
            suffix = "";
        }
        else if (const char* s = strip_prefix(body, "/shrug "))
        {
            suffix = s;
        }
        if (suffix)
        {
            std::string text = "\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF";
            if (*suffix != '\0')
            {
                text = std::string(suffix) + " " + text;
            }
            return client.send_message(room_id, text, "");
        }
    }

    // `/me <action>` → m.emote. Match is case-sensitive and requires a space
    // after `me` (matching Element / other clients); typing `/menu` is
    // therefore sent as plain text.
    if (const char* action = strip_prefix(body, "/me "))
    {
        std::string emote_body = action;
        // Strip the same prefix from formatted_body when present. If the
        // formatted body does NOT start with the literal `/me ` (e.g. because
        // it begins with an HTML tag introduced by markdown) we drop it
        // rather than emit a mismatched body / formatted_body pair.
        std::string emote_formatted;
        if (!formatted_body.empty())
        {
            if (const char* f = strip_prefix(formatted_body, "/me "))
            {
                emote_formatted = f;
            }
        }
        if (emote_body.empty())
        {
            // An empty `/me ` does nothing; report success so the composer
            // still clears (the user removed their action, that's intended).
            return Result{true, ""};
        }
        return client.send_emote(room_id, emote_body, emote_formatted);
    }
    return client.send_message(room_id, body, formatted_body);
}

}  // namespace tesseract
