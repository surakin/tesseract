#include "SlashCommands.h"

#include "tk/i18n.h"

#include <tesseract/client.h>
#include <tesseract/markdown.h>

#include <nlohmann/json.hpp>

#include <cctype>

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

std::optional<std::vector<std::string>> parse_slash_args(
    const std::string& body, const char* cmd)
{
    std::string prefix = std::string("/") + cmd + " ";
    if (body.size() <= prefix.size())
        return std::nullopt;
    if (body.compare(0, prefix.size(), prefix) != 0)
        return std::nullopt;
    std::string_view rest(body.c_str() + prefix.size(),
                          body.size() - prefix.size());

    std::vector<std::string> args;
    std::string cur;
    bool in_token = false;
    char quote = '\0';
    for (char c : rest)
    {
        if (quote != '\0')
        {
            if (c == quote)
                quote = '\0';
            else
                cur.push_back(c);
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            in_token = true;
            continue;
        }
        if (c == ' ' || c == '\t')
        {
            if (in_token)
            {
                args.push_back(std::move(cur));
                cur.clear();
                in_token = false;
            }
            continue;
        }
        cur.push_back(c);
        in_token = true;
    }
    if (in_token)
        args.push_back(std::move(cur));
    return args;
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
        {"me",           "<action>",          tk::tr("Send an action message")},
        {"shrug",        "",                  tk::tr("Append \xC2\xAF\\_(ツ)_/\xC2\xAF")},
        {"slap",         "<target>",          tk::tr("Slap someone with a large trout")},
        {"spoiler",      "[(reason)] <text>", tk::tr("Send a hidden spoiler message")},
        {"myroomnick",   "<name>",            tk::tr("Set your display name in this room")},
        {"myroomavatar", "[mxc_uri]",         tk::tr("Set your avatar in this room")},
        {"join",         "<#room:server>",    tk::tr("Join a room by alias or ID")},
        {"leave",        "",                  tk::tr("Leave the current room")},
        {"invite",       "<@user:server> [reason]", tk::tr("Invite a user to the current room")},
        {"gif",          "<search>",          tk::tr("Search for a GIF to send")},
        {"selfie",       "",                  tk::tr("Take a selfie and attach it")},
        {"location",     "",                  tk::tr("Share your current location")},
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
        return client.set_user_room_display_name(room_id, name);

    // `/myroomavatar <mxc_uri>` — set room-specific avatar to an explicit mxc.
    // The no-argument form is intercepted by callers before this function is
    // reached; if it arrives here anyway (e.g. empty suffix from the popup),
    // return an error rather than calling set_user_room_avatar with an empty URI.
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
            return client.set_user_room_avatar(room_id, sfx);
        }
    }

    return client.send_message(room_id, body, formatted_body);
}

// ---------------------------------------------------------------------------
// MSC4391 in-room bot commands
// ---------------------------------------------------------------------------

namespace
{

std::vector<std::string> split_whitespace(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split_comma(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        auto comma = s.find(',', start);
        if (comma == std::string::npos)
        {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

bool parse_bool_token(const std::string& t, bool& out)
{
    std::string lower;
    lower.reserve(t.size());
    for (char c : t) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "true" || lower == "yes" || lower == "1") { out = true; return true; }
    if (lower == "false" || lower == "no" || lower == "0") { out = false; return true; }
    return false;
}

// Strictly whole numbers only (optional leading '-') — MSC4391 has no float
// type (matrix.org canonical JSON can't encode one), so "3.14"/"3.0" are
// rejected outright rather than truncated.
bool parse_integer_token(const std::string& t, long long& out)
{
    if (t.empty()) return false;
    std::size_t idx = 0;
    try
    {
        out = std::stoll(t, &idx);
    }
    catch (...)
    {
        return false;
    }
    return idx == t.size();
}

bool looks_like_user_id(const std::string& t)
{
    return t.size() > 1 && t.front() == '@' && t.find(':') != std::string::npos;
}
bool looks_like_room_id(const std::string& t)
{
    return t.size() > 1 && t.front() == '!' && t.find(':') != std::string::npos;
}
bool looks_like_room_alias(const std::string& t)
{
    return t.size() > 1 && t.front() == '#' && t.find(':') != std::string::npos;
}
bool looks_like_event_id(const std::string& t)
{
    return t.size() > 1 && t.front() == '$';
}
// Server names have no sigil — loosely accept anything with no whitespace
// and at least one '.' or ':' (port), matching the shape of a DNS name or
// literal IP, without pulling in a full grammar checker for v1.
bool looks_like_server_name(const std::string& t)
{
    if (t.empty() || t.find_first_of(" \t") != std::string::npos) return false;
    return t.find('.') != std::string::npos || t.find(':') != std::string::npos;
}

std::optional<nlohmann::json> coerce_primitive(const std::string& t, ParamPrimitiveType ty)
{
    switch (ty)
    {
    case ParamPrimitiveType::String:
        return nlohmann::json(t);
    case ParamPrimitiveType::Integer:
    {
        long long v;
        if (!parse_integer_token(t, v)) return std::nullopt;
        return nlohmann::json(v);
    }
    case ParamPrimitiveType::Boolean:
    {
        bool v;
        if (!parse_bool_token(t, v)) return std::nullopt;
        return nlohmann::json(v);
    }
    case ParamPrimitiveType::UserId:
        return looks_like_user_id(t) ? std::optional(nlohmann::json(t)) : std::nullopt;
    case ParamPrimitiveType::RoomAlias:
        return looks_like_room_alias(t) ? std::optional(nlohmann::json(t)) : std::nullopt;
    case ParamPrimitiveType::ServerName:
        return looks_like_server_name(t) ? std::optional(nlohmann::json(t)) : std::nullopt;
    }
    return std::nullopt;
}

// `room_id`/`event_id` object-typed values — MSC4391 shape is `{id, via?}`
// (room_id) or `{id, via?, event_id}` (event_id). `via` is left empty; v1's
// text-entry UX has no server-list picker to source it from.
std::optional<nlohmann::json> coerce_object(const std::string& t, ParamObjectType ty)
{
    if (ty == ParamObjectType::RoomId)
    {
        if (!looks_like_room_id(t) && !looks_like_room_alias(t)) return std::nullopt;
        nlohmann::json obj;
        obj["id"] = t;
        return obj;
    }
    if (!looks_like_event_id(t)) return std::nullopt;
    nlohmann::json obj;
    obj["id"] = t;
    obj["event_id"] = t;
    return obj;
}

std::optional<nlohmann::json> coerce_scalar(const std::string& t, const ParamSchema& schema);

std::optional<nlohmann::json> coerce_literal(const std::string& t,
                                             const std::string& literal_value_json,
                                             ParamLiteralType lt)
{
    ParamPrimitiveType prim = lt == ParamLiteralType::String   ? ParamPrimitiveType::String
                             : lt == ParamLiteralType::Integer ? ParamPrimitiveType::Integer
                                                                : ParamPrimitiveType::Boolean;
    auto coerced = coerce_primitive(t, prim);
    if (!coerced) return std::nullopt;
    auto expected = nlohmann::json::parse(literal_value_json, nullptr, false);
    if (expected.is_discarded() || *coerced != expected) return std::nullopt;
    return coerced;
}

std::optional<nlohmann::json> coerce_scalar(const std::string& t, const ParamSchema& schema)
{
    switch (schema.kind)
    {
    case ParamSchemaKind::Primitive:
        return coerce_primitive(t, schema.primitive_type);
    case ParamSchemaKind::Object:
        return coerce_object(t, schema.object_type);
    case ParamSchemaKind::Literal:
        return coerce_literal(t, schema.literal_value_json, schema.literal_type);
    case ParamSchemaKind::Union:
        for (const auto& variant : schema.union_variants)
        {
            if (auto v = coerce_scalar(t, variant)) return v;
        }
        return std::nullopt;
    case ParamSchemaKind::Array:
    case ParamSchemaKind::Invalid:
        return std::nullopt;
    }
    return std::nullopt;
}

// A whole parameter's token: arrays comma-split into scalars first, matching
// the v1 "one whitespace token, comma-separated" array convention.
std::optional<nlohmann::json> coerce_token(const std::string& t, const ParamSchema& schema)
{
    if (schema.kind == ParamSchemaKind::Array)
    {
        if (!schema.array_item) return std::nullopt;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& piece : split_comma(t))
        {
            auto v = coerce_scalar(piece, *schema.array_item);
            if (!v) return std::nullopt;
            arr.push_back(*v);
        }
        return arr;
    }
    return coerce_scalar(t, schema);
}

}  // namespace

std::string describe_param_type(const ParamSchema& schema)
{
    switch (schema.kind)
    {
    case ParamSchemaKind::Primitive:
        switch (schema.primitive_type)
        {
        case ParamPrimitiveType::String:     return tk::tr("text");
        case ParamPrimitiveType::Integer:    return tk::tr("number");
        case ParamPrimitiveType::Boolean:    return tk::tr("yes/no");
        case ParamPrimitiveType::UserId:     return tk::tr("user");
        case ParamPrimitiveType::ServerName: return tk::tr("server");
        case ParamPrimitiveType::RoomAlias:  return tk::tr("room");
        }
        return tk::tr("text");
    case ParamSchemaKind::Object:
        return schema.object_type == ParamObjectType::RoomId ? tk::tr("room")
                                                              : tk::tr("event");
    case ParamSchemaKind::Array:
        return schema.array_item
                   ? tk::trf(tk::tr("{0} (comma-separated)"),
                            {describe_param_type(*schema.array_item)})
                   : tk::tr("list");
    case ParamSchemaKind::Union:
    {
        std::string joined;
        for (const auto& v : schema.union_variants)
        {
            if (!joined.empty()) joined += "/";
            joined += describe_param_type(v);
        }
        return joined.empty() ? tk::tr("text") : joined;
    }
    case ParamSchemaKind::Literal:
        return tk::tr("text");
    case ParamSchemaKind::Invalid:
        return tk::tr("text");
    }
    return tk::tr("text");
}

std::string next_bot_command_arg_hint(const CommandDescription& desc,
                                      std::size_t tokens_typed)
{
    if (tokens_typed >= desc.parameters.size())
    {
        return tk::tr("Press Enter to send");
    }
    const auto& p = desc.parameters[tokens_typed];
    std::string label = tk::trf(tk::tr("{0}: {1}"), {p.key, describe_param_type(p.schema)});
    if (p.optional)
    {
        label += " (" + tk::tr("optional") + ")";
    }
    return label;
}

BotCommandSendResult dispatch_bot_command_send(Client& client,
                                               const std::string& room_id,
                                               const CommandDescription& desc,
                                               const std::string& args_text)
{
    std::vector<std::string> tokens = split_whitespace(args_text);
    BotCommandMatchResult match = client.match_bot_command_arguments(desc, tokens);
    if (!match.ok)
    {
        if (!match.error_parameter_key.empty())
        {
            return {false, tk::trf(tk::tr("Missing required argument: {0}"),
                                   {match.error_parameter_key})};
        }
        return {false, tk::tr("Too many arguments")};
    }

    nlohmann::json args_obj = nlohmann::json::object();
    for (std::size_t i = 0; i < desc.parameters.size(); ++i)
    {
        const auto& p = desc.parameters[i];
        if (!match.tokens_by_parameter[i])
        {
            continue;  // omitted trailing optional parameter
        }
        auto coerced = coerce_token(*match.tokens_by_parameter[i], p.schema);
        if (!coerced)
        {
            return {false, tk::trf(tk::tr("{0} must be a valid {1}"),
                                   {p.key, describe_param_type(p.schema)})};
        }
        args_obj[p.key] = *coerced;
    }

    // Fallback body for clients/bots that don't render structured commands —
    // reconstructs the typed command line.
    std::string body = "/" + desc.command;
    for (const auto& t : tokens)
    {
        body += " " + t;
    }
    std::vector<std::string> mentions = {desc.sender};
    Result r = client.send_bot_command(room_id, body, "", mentions, desc.command,
                                       args_obj.dump());
    if (!r.ok)
    {
        return {false, r.message};
    }
    return {true, ""};
}

}  // namespace tesseract
