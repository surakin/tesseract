#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tesseract/client.h>

namespace tesseract
{

class Client;

// One parsed built-in command invocation. `args_text` / `args` come from the
// composer's plain text (mentions as raw Matrix IDs); `body` / `formatted_body`
// are the pill-aware message form, for commands that send text (/me, /shrug).
struct SlashCommandInvocation
{
    std::string name;              // canonical name, e.g. "invite"
    std::string args_text;         // plain text after "/name", trimmed
    std::vector<std::string> args; // tokenize_args(args_text)
    std::string body;
    std::string formatted_body;
};

struct SlashCommandDescriptor
{
    std::string name;        // canonical name without leading slash, e.g. "me"
    std::string args_hint;   // e.g. "<action>" or "" for argless commands
    std::string description; // one-line user-facing description
    // Handler that runs on the send worker (keeps send order; a failure is
    // reported through the normal unsent-message path). Null for commands the
    // shell handles itself (ShellBase's slash UI handlers: /join, /poll, ...).
    std::function<Result(Client&, const std::string& room_id, const SlashCommandInvocation&)> send;
    // Optional one-line confirmation shown after `send` succeeded, for commands
    // whose effect would otherwise be silent.
    std::function<std::optional<std::string>(const SlashCommandInvocation&)> success_message;
};

// The one table of built-in slash commands. The popup lists it and dispatch
// routes through it: add a command here. Stable order - the popup uses this
// vector's order when nothing has been typed yet. Lifetime: static.
const std::vector<SlashCommandDescriptor>& available_commands();

// The registry entry named `name` (no slash), or nullptr.
const SlashCommandDescriptor* find_command(std::string_view name);

// Parses "/name[ args]" from the composer's plain text. nullopt when `plain`
// is not a known built-in (an unknown "/foo" is sent as ordinary text). Names
// are case-sensitive and end at whitespace, so "/menu" is not "/me".
std::optional<SlashCommandInvocation> parse_slash_command(const std::string& plain,
                                                          const std::string& body,
                                                          const std::string& formatted_body);

// True if `text` has the shape of a Matrix user ID ("@local:server", no whitespace).
bool is_user_id(const std::string& text);

// Splits `text` on spaces/tabs, treating a `"..."` or `'...'` span as part of
// one token (quotes stripped, no escapes). An unterminated quote runs to the
// end of the text. `""` yields an empty token. Shared by parse_slash_command and
// the bot-command argument path.
std::vector<std::string> tokenize_args(std::string_view text);

// The plain + HTML pair produced by `/spoiler`.
struct SpoilerMessage
{
    std::string body;           // plain-text fallback, prefixed "(Spoiler…)"
    std::string formatted_body; // <span data-mx-spoiler[="reason"]>…</span>
};

/// `html` (the composer's rendered `formatted_body`) with the leading
/// "/<command>" and one following space removed, so a command's text keeps its
/// formatting and mention pills. A leading `<p>` is kept. Empty when `html`
/// doesn't start with the command, or is empty.
std::string strip_command_from_html(const std::string& html, std::string_view command);

/// `html` with `tail` added at the end of its last paragraph (or at the end
/// when there is none).
std::string append_to_html(const std::string& html, const std::string& tail);

// Build a spoiler message from the text following the `/spoiler ` prefix. A
// leading `(reason)` (balanced parens) sets the spoiler reason; everything
// after it is the hidden content, rendered through inline markdown. Returns
// std::nullopt when the content is whitespace-only so the caller can no-op
// (clearing the composer) like an empty `/me `. Exposed for unit testing.
std::optional<SpoilerMessage> build_spoiler_message(std::string_view args);

// One-line confirmation to show after the slash command in `plain` succeeded,
// for commands whose only visible effect would otherwise be silent
// (`/myroomavatar <arg>`). nullopt for anything else (including plain messages).
std::optional<std::string> slash_success_message(const std::string& plain);

// Dispatch a composer send to the SDK. `plain_text` (the composer's plain text,
// mentions as raw IDs; empty means `body`) is parsed with parse_slash_command;
// when it names a built-in with a `send` handler that handler runs, otherwise
// `body` / `formatted_body` are sent as a normal `m.text` message. Commands the
// shell handles itself (/join, /poll, ...) and unknown "/foo" fall through to
// the verbatim send: the homeserver shows them as literal text, like other
// Matrix clients do.
//
// `url_previews_json` (MSC4095 bundled previews from
// `Client::generate_url_previews`) is attached only to the final verbatim
// `send_message` fallthrough; command-transformed bodies never carry it.
//
// The returned `Result` is the underlying `Client` call's result, so callers
// can clear the composer on success.
Result dispatch_compose_send(Client& client, const std::string& room_id, const std::string& body,
                             const std::string& formatted_body,
                             const std::string& url_previews_json = "",
                             const std::string& plain_text = "");

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
std::string next_bot_command_arg_hint(const CommandDescription& desc, std::size_t tokens_typed);

// Result of attempting to send an MSC4391 bot command invocation.
struct BotCommandSendResult
{
    bool ok = false;
    // Localized, user-facing message describing the validation failure.
    // Empty when `ok`.
    std::string error;
};

// Coerce one typed argument token to the JSON value `schema` expects, as a
// compact JSON string; nullopt when the token does not fit. `current_room_id`
// is the room a bare `$event` token refers to. Exposed for tests; the send
// path uses it via `dispatch_bot_command_send`.
std::optional<std::string> coerce_bot_command_argument(const std::string& token,
                                                       const ParamSchema& schema,
                                                       const std::string& current_room_id);

// Fallback plain-text body for a structured bot command: "/command" plus the
// tokens, re-quoting any that contain whitespace or quote characters (double
// quotes, or single quotes when the token itself holds a `"`).
std::string bot_command_fallback_body(const std::string& command,
                                      const std::vector<std::string>& tokens);

// Tokenize `args_text` (whitespace-delimited; an array-typed parameter takes
// a single comma-separated token — see Client::match_bot_command_arguments's
// doc comment for the full v1 positional convention), match the tokens
// against `desc.parameters`, coerce each to its declared schema type, and
// send via `Client::send_bot_command`. Does not touch composer/UI state —
// the caller (SlashCommandController) owns clearing the composer on success
// and keeping the error visible (not clearing) on failure.
BotCommandSendResult dispatch_bot_command_send(Client& client, const std::string& room_id,
                                               const CommandDescription& desc,
                                               const std::string& args_text);

// One-line display of a received MSC4391 invocation for the timeline:
// "/command key: value key2: value2". Room/event reference objects show their
// `id` / `event_id`, arrays are comma-joined, strings are shown as-is, other
// scalars in their JSON form. `arguments_json` that is not a JSON object is
// treated as no arguments. Empty when `command` is empty.
std::string format_bot_command_display(const std::string& command,
                                       const std::string& arguments_json);

} // namespace tesseract
