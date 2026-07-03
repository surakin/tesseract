#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tesseract/client.h>

namespace tesseract
{

class Client;

struct SlashCommandDescriptor
{
    std::string name;         // canonical name without leading slash, e.g. "me"
    std::string args_hint;    // e.g. "<action>" or "" for argless commands
    std::string description;  // one-line user-facing description
};

// Returns the canonical command list. Stable order — the popup uses this
// vector's order when nothing has been typed yet. Lifetime: static.
const std::vector<SlashCommandDescriptor>& available_commands();

// Returns true when `body` is the slash command `cmd` with no non-whitespace
// argument: "/cmd", "/cmd ", and "/cmd  " all return true; "/cmdfoo" returns
// false (different command). Shells use this to intercept commands that open a
// native dialog before dispatching to dispatch_compose_send.
bool is_slash_command_no_arg(const std::string& body, const char* cmd);

// If `body` starts with "/<cmd> " and has at least one non-whitespace
// character following, returns the trimmed argument string. Returns nullopt
// when the command prefix doesn't match or the argument is blank.
std::optional<std::string> parse_slash_arg(const std::string& body,
                                            const char* cmd);

// The plain + HTML pair produced by `/spoiler`.
struct SpoilerMessage
{
    std::string body;            // plain-text fallback, prefixed "(Spoiler…)"
    std::string formatted_body;  // <span data-mx-spoiler[="reason"]>…</span>
};

// Build a spoiler message from the text following the `/spoiler ` prefix. A
// leading `(reason)` (balanced parens) sets the spoiler reason; everything
// after it is the hidden content, rendered through inline markdown. Returns
// std::nullopt when the content is whitespace-only so the caller can no-op
// (clearing the composer) like an empty `/me `. Exposed for unit testing.
std::optional<SpoilerMessage> build_spoiler_message(std::string_view args);

// Dispatch a composer send to the SDK. If `body` matches a recognized slash
// command, routes to the corresponding `Client` method; otherwise it is sent
// as a normal `m.text` message.
//
// Currently recognized:
//   - `/me <action>` → `Client::send_emote`. The `/me ` prefix is stripped
//     from both `body` and `formatted_body` (when the formatted body also
//     starts with `/me `). An empty action after stripping returns a
//     no-op `Result` (ok with no side effect).
//   - `/shrug` (with optional trailing text) → `Client::send_message`
//     with the text suffixed by `¯\_(ツ)_/¯`. With no trailing text it
//     sends just the emoticon.
//   - `/myroomnick <name>` → `Client::set_user_room_display_name`.
//   - `/myroomavatar <mxc_uri>` → `Client::set_user_room_avatar` with an
//     explicit mxc:// URI. The no-argument form `/myroomavatar` is NOT
//     handled here — callers must intercept it before calling this function
//     and open a platform file picker instead.
//
// Slash commands not recognized here (e.g. `/foo`) fall through to
// `send_message` and are transmitted verbatim — the homeserver will display
// them as literal text, matching what other Matrix clients do for unknown
// slash commands.
//
// The returned `Result` is the underlying `Client` call's result, so callers
// can clear the composer on success.
Result dispatch_compose_send(Client& client,
                             const std::string& room_id,
                             const std::string& body,
                             const std::string& formatted_body);

}  // namespace tesseract
