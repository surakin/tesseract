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

Result dispatch_compose_send(Client& client,
                             const std::string& room_id,
                             const std::string& body,
                             const std::string& formatted_body)
{
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
