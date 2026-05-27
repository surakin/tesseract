#pragma once

#include <string>

#include <tesseract/client.h>

namespace tesseract
{

class Client;

// Dispatch a composer send to the SDK. If `body` matches a recognized slash
// command, routes to the corresponding `Client` method; otherwise it is sent
// as a normal `m.text` message.
//
// Currently recognized:
//   - `/me <action>` → `Client::send_emote`. The `/me ` prefix is stripped
//     from both `body` and `formatted_body` (when the formatted body also
//     starts with `/me `). An empty action after stripping returns a
//     no-op `Result` (ok with no side effect).
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
