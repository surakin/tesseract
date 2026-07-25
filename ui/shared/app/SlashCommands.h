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

// ---------------------------------------------------------------------------
// MSC4391 in-room bot commands
// ---------------------------------------------------------------------------

// Human-readable label for a parameter's type, used in the argument-entry
// hint and in validation error messages — never shows the raw schema type
// string (e.g. "user_id") to the user. Localized via tk::tr.
std::string describe_param_type(const ParamSchema& schema);

// Build the single-line hint shown while the user is typing positional
// arguments for `desc` (the composer already contains "/command_name ").
// `tokens_typed` is the count of *completed* whitespace-delimited tokens
// (a trailing in-progress token, i.e. text after the last space, doesn't
// count — the user is still typing it). Returns a "next expected parameter"
// hint (e.g. "timeout_seconds: number"), or a ready-to-send hint once every
// parameter has been provided. Purely informational — see
// Client::match_bot_command_arguments's doc comment for why this doesn't
// attempt to be a fully accurate live validator.
std::string next_bot_command_arg_hint(const CommandDescription& desc,
                                      std::size_t tokens_typed);

// Result of attempting to send an MSC4391 bot command invocation.
struct BotCommandSendResult
{
    bool ok = false;
    // Localized, user-facing message describing the validation failure.
    // Empty when `ok`.
    std::string error;
};

// Tokenize `args_text` (whitespace-delimited; an array-typed parameter takes
// a single comma-separated token — see Client::match_bot_command_arguments's
// doc comment for the full v1 positional convention), match the tokens
// against `desc.parameters`, coerce each to its declared schema type, and
// send via `Client::send_bot_command`. Does not touch composer/UI state —
// the caller (SlashCommandController) owns clearing the composer on success
// and keeping the error visible (not clearing) on failure.
BotCommandSendResult dispatch_bot_command_send(Client& client,
                                               const std::string& room_id,
                                               const CommandDescription& desc,
                                               const std::string& args_text);

}  // namespace tesseract
