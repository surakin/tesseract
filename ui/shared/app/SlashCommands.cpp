#include "SlashCommands.h"

#include <tesseract/client.h>
#include <tesseract/markdown.h>

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

// Escape a value for embedding inside a double-quoted HTML attribute.
std::string attr_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '"': out += "&quot;"; break;
        default:  out += c; break;
        }
    }
    return out;
}

}  // namespace

bool is_slash_command_no_arg(const std::string& body, const char* cmd)
{
    const std::size_t cmd_len    = std::char_traits<char>::length(cmd);
    const std::size_t prefix_len = cmd_len + 1; // +1 for '/'
    if (body.size() < prefix_len || body[0] != '/')
        return false;
    if (body.compare(1, cmd_len, cmd) != 0)
        return false;
    // Everything after the command name must be absent or whitespace-only.
    return body.size() == prefix_len ||
           body.find_first_not_of(" \t", prefix_len) == std::string::npos;
}

std::optional<std::string> parse_slash_arg(const std::string& body,
                                            const char* cmd)
{
    // Build the required prefix: "/<cmd> "
    std::string prefix = std::string("/") + cmd + " ";
    if (body.size() <= prefix.size())
        return std::nullopt;
    if (body.compare(0, prefix.size(), prefix) != 0)
        return std::nullopt;
    std::string_view arg(body.c_str() + prefix.size(),
                         body.size() - prefix.size());
    const auto start = arg.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return std::nullopt;
    const auto end = arg.find_last_not_of(" \t");
    return std::string(arg.substr(start, end - start + 1));
}

std::optional<SpoilerMessage> build_spoiler_message(std::string_view args)
{
    // Trim leading whitespace.
    std::size_t i = 0;
    while (i < args.size() && (args[i] == ' ' || args[i] == '\t')) ++i;
    args.remove_prefix(i);

    // Optional leading "(reason)" — only when the parens are balanced.
    std::string reason;
    bool has_reason = false;
    if (!args.empty() && args.front() == '(')
    {
        const auto close = args.find(')');
        if (close != std::string_view::npos)
        {
            std::string_view r = args.substr(1, close - 1);
            // Trim the reason.
            std::size_t rs = 0, re = r.size();
            while (rs < re && (r[rs] == ' ' || r[rs] == '\t')) ++rs;
            while (re > rs && (r[re - 1] == ' ' || r[re - 1] == '\t')) --re;
            reason.assign(r.substr(rs, re - rs));
            has_reason = true;
            args.remove_prefix(close + 1);
            // Trim whitespace between ")" and the content.
            std::size_t j = 0;
            while (j < args.size() && (args[j] == ' ' || args[j] == '\t')) ++j;
            args.remove_prefix(j);
        }
    }

    // Trim trailing whitespace from the content.
    std::size_t end = args.size();
    while (end > 0 && (args[end - 1] == ' ' || args[end - 1] == '\t')) --end;
    args = args.substr(0, end);

    if (args.empty())
    {
        return std::nullopt;
    }

    SpoilerMessage msg;
    const std::string inner = markdown_inline_to_html(args);
    if (has_reason)
    {
        msg.body = "(Spoiler: " + reason + ") " + std::string(args);
        msg.formatted_body =
            "<span data-mx-spoiler=\"" + attr_escape(reason) + "\">" + inner +
            "</span>";
    }
    else
    {
        msg.body = "(Spoiler) " + std::string(args);
        msg.formatted_body = "<span data-mx-spoiler>" + inner + "</span>";
    }
    return msg;
}

const std::vector<SlashCommandDescriptor>& available_commands()
{
    static const std::vector<SlashCommandDescriptor> kCommands = {
        {"me",           "<action>",          "Send an action message"},
        {"shrug",        "",                  "Append \xC2\xAF\\_(ツ)_/\xC2\xAF"},
        {"slap",         "<target>",          "Slap someone with a large trout"},
        {"spoiler",      "[(reason)] <text>", "Send a hidden spoiler message"},
        {"myroomnick",   "<name>",            "Set your display name in this room"},
        {"myroomavatar", "[mxc_uri]",         "Set your avatar in this room"},
        {"join",         "<#room:server>",    "Join a room by alias or ID"},
        {"leave",        "",                  "Leave the current room"},
        {"invite",       "<@user:server>",    "Invite a user to the current room"},
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

    // `/slap <target>` → m.emote, the classic IRC trout slap. Match is
    // case-sensitive and requires a space after `slap` (so `/slapfoo` is sent
    // as plain text).
    if (const char* target = strip_prefix(body, "/slap "))
    {
        std::string name = target;
        const auto first = name.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            // Whitespace-only target — no-op that still clears the composer,
            // matching how an empty `/me ` behaves.
            return Result{true, ""};
        }
        const auto last = name.find_last_not_of(" \t");
        name = name.substr(first, last - first + 1);

        std::string emote_body =
            "slaps " + name + " around a bit with a large trout";
        return client.send_emote(room_id, emote_body, "");
    }

    // `/spoiler [(reason)] <text>` → m.text with a `data-mx-spoiler` span
    // (MSC2010). Match is case-sensitive and requires a space after `spoiler`.
    if (const char* args = strip_prefix(body, "/spoiler "))
    {
        auto msg = build_spoiler_message(args);
        if (!msg)
        {
            // Whitespace-only content — no-op that still clears the composer,
            // matching how an empty `/me ` behaves.
            return Result{true, ""};
        }
        return client.send_message(room_id, msg->body, msg->formatted_body);
    }

    // `/myroomnick <name>` — set room-specific display name.
    if (const char* name = strip_prefix(body, "/myroomnick "))
        return client.set_room_display_name(room_id, name);

    // `/myroomavatar <mxc_uri>` — set room-specific avatar to an explicit mxc.
    // The no-argument form is intercepted by callers before this function is
    // reached; if it arrives here anyway (e.g. empty suffix from the popup),
    // return an error rather than calling set_room_avatar with an empty URI.
    if (const char* sfx = strip_prefix(body, "/myroomavatar"))
    {
        // sfx must start with whitespace or be end-of-string so that
        // "/myroomavatarabc" is not misidentified as this command.
        if (*sfx == '\0' || *sfx == ' ' || *sfx == '\t')
        {
            while (*sfx == ' ' || *sfx == '\t') ++sfx; // skip separator
            if (*sfx == '\0')
                return Result{false, "no mxc_uri provided; use /myroomavatar "
                                     "alone to open the image picker"};
            return client.set_room_avatar(room_id, sfx);
        }
    }

    return client.send_message(room_id, body, formatted_body);
}

}  // namespace tesseract
