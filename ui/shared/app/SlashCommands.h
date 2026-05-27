#pragma once

#include <string>
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
