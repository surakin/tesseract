#include "views/SlashCommands.h"

#include "tk/i18n.h"

#include <tesseract/client.h>
#include <tesseract/markdown.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace tesseract
{

namespace
{

// Escape a value for embedding inside a double-quoted HTML attribute.
std::string attr_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

} // namespace

std::vector<std::string> tokenize_args(std::string_view text)
{
    std::vector<std::string> args;
    std::string cur;
    bool in_token = false;
    char quote = '\0';
    for (char c : text)
    {
        if (quote != '\0')
        {
            if (c == quote)
            {
                quote = '\0';
            }
            else
            {
                cur.push_back(c);
            }
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
    {
        args.push_back(std::move(cur));
    }
    return args;
}

std::optional<SpoilerMessage> build_spoiler_message(std::string_view args)
{
    // Trim leading whitespace.
    std::size_t i = 0;
    while (i < args.size() && (args[i] == ' ' || args[i] == '\t'))
    {
        ++i;
    }
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
            while (rs < re && (r[rs] == ' ' || r[rs] == '\t'))
            {
                ++rs;
            }
            while (re > rs && (r[re - 1] == ' ' || r[re - 1] == '\t'))
            {
                --re;
            }
            reason.assign(r.substr(rs, re - rs));
            has_reason = true;
            args.remove_prefix(close + 1);
            // Trim whitespace between ")" and the content.
            std::size_t j = 0;
            while (j < args.size() && (args[j] == ' ' || args[j] == '\t'))
            {
                ++j;
            }
            args.remove_prefix(j);
        }
    }

    // Trim trailing whitespace from the content.
    std::size_t end = args.size();
    while (end > 0 && (args[end - 1] == ' ' || args[end - 1] == '\t'))
    {
        --end;
    }
    args = args.substr(0, end);

    if (args.empty())
    {
        return std::nullopt;
    }

    SpoilerMessage msg;
    const std::string inner = markdown_inline_to_html(args);
    if (has_reason)
    {
        msg.body = tk::trf(tk::tr("(Spoiler: {0})"), {reason}) + " " + std::string(args);
        msg.formatted_body =
            "<span data-mx-spoiler=\"" + attr_escape(reason) + "\">" + inner + "</span>";
    }
    else
    {
        msg.body = tk::tr("(Spoiler)") + " " + std::string(args);
        msg.formatted_body = "<span data-mx-spoiler>" + inner + "</span>";
    }
    return msg;
}

std::string strip_command_from_html(const std::string& html, std::string_view command)
{
    std::string_view h = html;
    std::string prefix;
    if (h.starts_with("<p>"))
    {
        prefix = "<p>";
        h.remove_prefix(3);
    }
    const std::string literal = "/" + std::string(command);
    if (!h.starts_with(literal))
    {
        return {};
    }
    h.remove_prefix(literal.size());
    if (!h.empty() && h.front() == ' ')
    {
        h.remove_prefix(1);
    }
    return prefix + std::string(h);
}

std::string append_to_html(const std::string& html, const std::string& tail)
{
    constexpr std::string_view kClose = "</p>";
    if (html.size() >= kClose.size() && html.ends_with(kClose))
    {
        return html.substr(0, html.size() - kClose.size()) + tail + std::string(kClose);
    }
    return html + tail;
}

namespace
{

// What follows "/<name>" in `body` and one separating space, or "" when `body`
// doesn't start with the command. The pill-aware body keeps display names.
std::string body_after_command(const std::string& body, std::string_view name)
{
    const std::string literal = "/" + std::string(name);
    if (!body.starts_with(literal))
    {
        return {};
    }
    std::string_view rest = std::string_view(body).substr(literal.size());
    if (!rest.empty() && rest.front() == ' ')
    {
        rest.remove_prefix(1);
    }
    return std::string(rest);
}

// `/shrug [text]`: the typed text (keeping formatting and pills) followed by the
// shrug emoticon; with no text, just the emoticon.
Result send_shrug(Client& client, const std::string& room_id, const SlashCommandInvocation& inv)
{
    const std::string suffix = body_after_command(inv.body, "shrug");
    std::string text = "\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF";
    if (!suffix.empty())
    {
        text = suffix + " " + text;
    }
    std::string html;
    if (!suffix.empty() && !inv.formatted_body.empty())
    {
        const std::string typed = strip_command_from_html(inv.formatted_body, "shrug");
        if (!typed.empty())
        {
            html = append_to_html(typed, " \xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF");
        }
    }
    return client.send_message(room_id, text, html);
}

// `/me <action>` -> m.emote. An empty action does nothing and reports success so
// the composer still clears.
Result send_me(Client& client, const std::string& room_id, const SlashCommandInvocation& inv)
{
    const std::string action = body_after_command(inv.body, "me");
    if (action.empty())
    {
        return Result{true, ""};
    }
    // The composer wraps markdown output in <p>, which strip_command_from_html
    // looks through, so "/me *waves*" keeps its emphasis. A formatted body that
    // doesn't start with the command is dropped rather than sent mismatched.
    std::string emote_formatted;
    if (!inv.formatted_body.empty())
    {
        emote_formatted = strip_command_from_html(inv.formatted_body, "me");
    }
    return client.send_emote(room_id, action, emote_formatted);
}

// `/slap <target>` -> m.emote, the classic IRC trout slap.
Result send_slap(Client& client, const std::string& room_id, const SlashCommandInvocation& inv)
{
    if (inv.args_text.empty())
    {
        return Result{true, ""};
    }
    return client.send_emote(
        room_id, tk::trf(tk::tr("slaps {0} around a bit with a large trout"), {inv.args_text}), "");
}

// `/spoiler [(reason)] <text>` -> m.text with a `data-mx-spoiler` span (MSC2010).
Result send_spoiler(Client& client, const std::string& room_id, const SlashCommandInvocation& inv)
{
    auto msg = build_spoiler_message(inv.args_text);
    if (!msg)
    {
        return Result{true, ""}; // whitespace-only: no-op that still clears the composer
    }
    return client.send_message(room_id, msg->body, msg->formatted_body);
}

// `/myroomnick <name>`: a blank name must not reach the client (an empty display
// name clears it).
Result send_myroomnick(Client& client, const std::string& room_id,
                       const SlashCommandInvocation& inv)
{
    if (inv.args_text.empty())
    {
        return Result{false, tk::tr("no display name given; use /myroomnick <name>")};
    }
    return client.set_user_room_display_name(room_id, inv.args_text);
}

bool starts_with_ci(const std::string& s, std::string_view prefix)
{
    return s.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), s.begin(),
                      [](char a, char b)
                      {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

// `/myroomavatar <mxc_uri | http(s) url | reset>`. The no-argument form opens a
// file picker and is handled by the shell before this is reached; if it arrives
// anyway, report an error rather than calling the client with an empty URI.
Result send_myroomavatar(Client& client, const std::string& room_id,
                         const SlashCommandInvocation& inv)
{
    const std::string& arg = inv.args_text;
    if (arg.empty())
    {
        return Result{false, tk::tr("no image URL provided; use /myroomavatar "
                                    "alone to open the image picker")};
    }
    if (arg.size() == 5 && starts_with_ci(arg, "reset"))
    {
        return client.reset_user_room_avatar(room_id);
    }
    if (starts_with_ci(arg, "mxc://"))
    {
        return client.set_user_room_avatar(room_id, arg);
    }
    if (starts_with_ci(arg, "http://") || starts_with_ci(arg, "https://"))
    {
        return client.set_user_room_avatar_from_url(room_id, arg);
    }
    return Result{false, tk::tr("expected an mxc:// or http(s) image URL, or \"reset\"")};
}

std::optional<std::string> myroomavatar_success(const SlashCommandInvocation& inv)
{
    if (inv.args_text.empty())
    {
        return std::nullopt;
    }
    std::string lower = inv.args_text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return lower == "reset" ? tk::tr("Avatar reset for this room")
                            : tk::tr("Avatar updated for this room");
}

} // namespace

const std::vector<SlashCommandDescriptor>& available_commands()
{
    static const std::vector<SlashCommandDescriptor> kCommands = {
        {"me", "<action>", tk::tr("Send an action message"), send_me, nullptr},
        {"shrug", "", tk::tr("Append \xC2\xAF\\_(ツ)_/\xC2\xAF"), send_shrug, nullptr},
        {"slap", "<target>", tk::tr("Slap someone with a large trout"), send_slap, nullptr},
        {"spoiler", "[(reason)] <text>", tk::tr("Send a hidden spoiler message"), send_spoiler,
         nullptr},
        {"myroomnick", "<name>", tk::tr("Set your display name in this room"), send_myroomnick,
         nullptr},
        {"myroomavatar", "[mxc_uri|url|reset]", tk::tr("Set or reset your avatar in this room"),
         send_myroomavatar, myroomavatar_success},
        {"join", "<#room:server>", tk::tr("Join a room by alias or ID"), nullptr, nullptr},
        {"leave", "", tk::tr("Leave the current room"), nullptr, nullptr},
        {"invite", "<@user:server> [reason]", tk::tr("Invite a user to the current room"), nullptr,
         nullptr},
        {"dm", "<@user:server>", tk::tr("Open a direct message with a user"), nullptr, nullptr},
        {"gif", "<search>", tk::tr("Search for a GIF to send"), nullptr, nullptr},
        {"sticker", "<search>", tk::tr("Search for a sticker to send"), nullptr, nullptr},
        {"selfie", "", tk::tr("Take a selfie and attach it"), nullptr, nullptr},
        {"location", "", tk::tr("Share your current location"), nullptr, nullptr},
        {"poll", "", tk::tr("Create a poll"), nullptr, nullptr},
    };
    return kCommands;
}

const SlashCommandDescriptor* find_command(std::string_view name)
{
    for (const auto& c : available_commands())
    {
        if (c.name == name)
        {
            return &c;
        }
    }
    return nullptr;
}

std::optional<SlashCommandInvocation> parse_slash_command(const std::string& plain,
                                                          const std::string& body,
                                                          const std::string& formatted_body)
{
    if (plain.size() < 2 || plain.front() != '/')
    {
        return std::nullopt;
    }
    const auto name_end = plain.find_first_of(" \t", 1);
    const std::string name =
        plain.substr(1, name_end == std::string::npos ? name_end : name_end - 1);
    if (!find_command(name))
    {
        return std::nullopt;
    }
    SlashCommandInvocation inv;
    inv.name = name;
    if (name_end != std::string::npos)
    {
        const auto first = plain.find_first_not_of(" \t", name_end);
        if (first != std::string::npos)
        {
            const auto last = plain.find_last_not_of(" \t");
            inv.args_text = plain.substr(first, last - first + 1);
        }
    }
    inv.args = tokenize_args(inv.args_text);
    inv.body = body;
    inv.formatted_body = formatted_body;
    return inv;
}

std::optional<std::string> slash_success_message(const std::string& plain)
{
    const auto inv = parse_slash_command(plain, plain, {});
    if (!inv)
    {
        return std::nullopt;
    }
    const auto* desc = find_command(inv->name);
    return desc && desc->success_message ? desc->success_message(*inv) : std::nullopt;
}

Result dispatch_compose_send(Client& client, const std::string& room_id, const std::string& body,
                             const std::string& formatted_body,
                             const std::string& url_previews_json, const std::string& plain_text)
{
    if (const auto inv =
            parse_slash_command(plain_text.empty() ? body : plain_text, body, formatted_body))
    {
        if (const auto* desc = find_command(inv->name); desc && desc->send)
        {
            return desc->send(client, room_id, *inv);
        }
    }
    return client.send_message(room_id, body, formatted_body, url_previews_json);
}

// ---------------------------------------------------------------------------
// MSC4391 in-room bot commands
// ---------------------------------------------------------------------------

namespace
{

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
    for (char c : t)
    {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "true" || lower == "yes" || lower == "1")
    {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "0")
    {
        out = false;
        return true;
    }
    return false;
}

// Strictly whole numbers only (optional leading '-') — MSC4391 has no float
// type (matrix.org canonical JSON can't encode one), so "3.14"/"3.0" are
// rejected outright rather than truncated.
bool parse_integer_token(const std::string& t, long long& out)
{
    if (t.empty())
    {
        return false;
    }
    std::size_t idx = 0;
    try
    {
        out = std::stoll(t, &idx);
    }
    catch (...)
    {
        return false;
    }
    if (idx != t.size())
    {
        return false;
    }
    // Canonical JSON only carries integers up to 2^53-1 exactly.
    constexpr long long kMaxSafe = (1LL << 53) - 1;
    return out >= -kMaxSafe && out <= kMaxSafe;
}

bool looks_like_user_id(const std::string& t)
{
    return Client::is_valid_matrix_id('@', t);
}
bool looks_like_room_id(const std::string& t)
{
    return Client::is_valid_matrix_id('!', t);
}
bool looks_like_room_alias(const std::string& t)
{
    return Client::is_valid_matrix_id('#', t);
}
bool looks_like_event_id(const std::string& t)
{
    return Client::is_valid_matrix_id('$', t);
}
// Server names have no sigil — loosely accept anything with no whitespace
// and at least one '.' or ':' (port), matching the shape of a DNS name or
// literal IP, without pulling in a full grammar checker for v1.
bool looks_like_server_name(const std::string& t)
{
    if (t.empty() || t.find_first_of(" \t") != std::string::npos)
    {
        return false;
    }
    return t.find('.') != std::string::npos || t.find(':') != std::string::npos;
}

// A `room_id` / `event_id` argument value (MSC4391): `{type, id, via?}` for a
// room, `{type, id, event_id, via?}` for an event, where `id` is always the
// room. Accepts a bare `!room` / `#alias` (room) or `$event` (event, in the
// current room), or a matrix.to / `matrix:` permalink carrying `via`. Aliases
// and missing `via` are resolved by the SDK at send time.
std::optional<nlohmann::json> coerce_reference(const std::string& t,
                                               const std::string& current_room_id, bool want_event)
{
    std::string room;
    std::string event;
    std::vector<std::string> via;
    if (want_event && looks_like_event_id(t))
    {
        room = current_room_id;
        event = t;
    }
    else if (!want_event && (looks_like_room_id(t) || looks_like_room_alias(t)))
    {
        room = t;
    }
    else
    {
        const auto link = Client::parse_matrix_link(t);
        using Kind = Client::MatrixLink::Kind;
        if (want_event && link.kind == Kind::Event)
        {
            room = link.primary;
            event = link.event_id;
        }
        else if (!want_event && (link.kind == Kind::Room || link.kind == Kind::RoomAlias))
        {
            room = link.primary;
        }
        else
        {
            return std::nullopt;
        }
        via = link.via;
    }
    if (room.empty() || (want_event && event.empty()))
    {
        return std::nullopt;
    }
    nlohmann::json obj;
    obj["type"] = want_event ? "event_id" : "room_id";
    obj["id"] = room;
    if (want_event)
    {
        obj["event_id"] = event;
    }
    if (!via.empty())
    {
        obj["via"] = via;
    }
    return obj;
}

std::optional<nlohmann::json> coerce_primitive(const std::string& t, ParamPrimitiveType ty,
                                               const std::string& current_room_id)
{
    switch (ty)
    {
    case ParamPrimitiveType::String:
        return nlohmann::json(t);
    case ParamPrimitiveType::Integer:
    {
        long long v;
        if (!parse_integer_token(t, v))
        {
            return std::nullopt;
        }
        return nlohmann::json(v);
    }
    case ParamPrimitiveType::Boolean:
    {
        bool v;
        if (!parse_bool_token(t, v))
        {
            return std::nullopt;
        }
        return nlohmann::json(v);
    }
    case ParamPrimitiveType::UserId:
        return looks_like_user_id(t) ? std::optional(nlohmann::json(t)) : std::nullopt;
    case ParamPrimitiveType::RoomAlias:
        return looks_like_room_alias(t) ? std::optional(nlohmann::json(t)) : std::nullopt;
    case ParamPrimitiveType::ServerName:
        return looks_like_server_name(t) ? std::optional(nlohmann::json(t)) : std::nullopt;
    case ParamPrimitiveType::RoomId:
        return coerce_reference(t, current_room_id, /*want_event=*/false);
    case ParamPrimitiveType::EventId:
        return coerce_reference(t, current_room_id, /*want_event=*/true);
    }
    return std::nullopt;
}

std::optional<nlohmann::json> coerce_scalar(const std::string& t, const ParamSchema& schema,
                                            const std::string& current_room_id);

std::optional<nlohmann::json> coerce_literal(const std::string& t,
                                             const std::string& literal_value_json,
                                             ParamLiteralType lt,
                                             const std::string& current_room_id)
{
    ParamPrimitiveType prim = lt == ParamLiteralType::String    ? ParamPrimitiveType::String
                              : lt == ParamLiteralType::Integer ? ParamPrimitiveType::Integer
                                                                : ParamPrimitiveType::Boolean;
    auto coerced = coerce_primitive(t, prim, current_room_id);
    if (!coerced)
    {
        return std::nullopt;
    }
    auto expected = nlohmann::json::parse(literal_value_json, nullptr, false);
    if (expected.is_discarded() || *coerced != expected)
    {
        return std::nullopt;
    }
    return coerced;
}

std::optional<nlohmann::json> coerce_scalar(const std::string& t, const ParamSchema& schema,
                                            const std::string& current_room_id)
{
    switch (schema.kind)
    {
    case ParamSchemaKind::Primitive:
        return coerce_primitive(t, schema.primitive_type, current_room_id);
    case ParamSchemaKind::Literal:
        return coerce_literal(t, schema.literal_value_json, schema.literal_type, current_room_id);
    case ParamSchemaKind::Union:
        for (const auto& variant : schema.union_variants)
        {
            if (auto v = coerce_scalar(t, variant, current_room_id))
            {
                return v;
            }
        }
        return std::nullopt;
    case ParamSchemaKind::Array:
    case ParamSchemaKind::Invalid:
        return std::nullopt;
    }
    return std::nullopt;
}

// A whole parameter's token: arrays comma-split into scalars first, matching
// the "one token, comma-separated" array convention. Tokens come from
// tokenize_args, so a quoted span arrives unquoted: for a string parameter
// `"hello, big world"` is one argument verbatim (no comma splitting); for an
// array parameter the same span is comma-split and each element keeps its
// inner spaces ("hello", " big world").
std::optional<nlohmann::json> coerce_token(const std::string& t, const ParamSchema& schema,
                                           const std::string& current_room_id)
{
    if (schema.kind == ParamSchemaKind::Array)
    {
        if (!schema.array_item)
        {
            return std::nullopt;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& piece : split_comma(t))
        {
            auto v = coerce_scalar(piece, *schema.array_item, current_room_id);
            if (!v)
            {
                return std::nullopt;
            }
            arr.push_back(*v);
        }
        return arr;
    }
    return coerce_scalar(t, schema, current_room_id);
}

} // namespace

std::optional<std::string> coerce_bot_command_argument(const std::string& token,
                                                       const ParamSchema& schema,
                                                       const std::string& current_room_id)
{
    auto v = coerce_token(token, schema, current_room_id);
    if (!v)
    {
        return std::nullopt;
    }
    return v->dump();
}

std::string describe_param_type(const ParamSchema& schema)
{
    switch (schema.kind)
    {
    case ParamSchemaKind::Primitive:
        switch (schema.primitive_type)
        {
        case ParamPrimitiveType::String:
            return tk::tr("text");
        case ParamPrimitiveType::Integer:
            return tk::tr("number");
        case ParamPrimitiveType::Boolean:
            return tk::tr("yes/no");
        case ParamPrimitiveType::UserId:
            return tk::tr("user");
        case ParamPrimitiveType::ServerName:
            return tk::tr("server");
        case ParamPrimitiveType::RoomAlias:
        case ParamPrimitiveType::RoomId:
            return tk::tr("room");
        case ParamPrimitiveType::EventId:
            return tk::tr("event");
        }
        return tk::tr("text");
    case ParamSchemaKind::Array:
        return schema.array_item ? tk::trf(tk::tr("{0} (comma-separated)"),
                                           {describe_param_type(*schema.array_item)})
                                 : tk::tr("list");
    case ParamSchemaKind::Union:
    {
        std::string joined;
        for (const auto& v : schema.union_variants)
        {
            if (!joined.empty())
            {
                joined += '/';
            }
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

std::string next_bot_command_arg_hint(const CommandDescription& desc, std::size_t tokens_typed)
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

std::string bot_command_fallback_body(const std::string& command,
                                      const std::vector<std::string>& tokens)
{
    std::string body = "/" + command;
    for (const auto& t : tokens)
    {
        // Re-wrap tokens that tokenize_args unquoted so the body re-parses to
        // the same tokens: double quotes, or single quotes if it holds a `"`.
        if (t.empty() || t.find_first_of(" \t\r\n\"'") != std::string::npos)
        {
            const char q = t.find('"') != std::string::npos ? '\'' : '"';
            body += std::string(" ") + q + t + q;
        }
        else
        {
            body += " " + t;
        }
    }
    return body;
}

BotCommandSendResult dispatch_bot_command_send(Client& client, const std::string& room_id,
                                               const CommandDescription& desc,
                                               const std::string& args_text)
{
    std::vector<std::string> tokens = tokenize_args(args_text);
    BotCommandMatchResult match = client.match_bot_command_arguments(desc, tokens);
    if (!match.ok)
    {
        if (!match.error_parameter_key.empty())
        {
            return {false,
                    tk::trf(tk::tr("Missing required argument: {0}"), {match.error_parameter_key})};
        }
        return {false, tk::tr("Too many arguments")};
    }

    nlohmann::json args_obj = nlohmann::json::object();
    for (std::size_t i = 0; i < desc.parameters.size(); ++i)
    {
        const auto& p = desc.parameters[i];
        if (!match.tokens_by_parameter[i])
        {
            continue; // omitted trailing optional parameter
        }
        auto coerced = coerce_token(*match.tokens_by_parameter[i], p.schema, room_id);
        if (!coerced)
        {
            return {false, tk::trf(tk::tr("{0} must be a valid {1}"),
                                   {p.key, describe_param_type(p.schema)})};
        }
        args_obj[p.key] = *coerced;
    }

    // Fallback body for clients/bots that don't render structured commands —
    // reconstructs the typed command line.
    const std::string body = bot_command_fallback_body(desc.command, tokens);
    std::vector<std::string> mentions = {desc.sender};
    Result r = client.send_bot_command(room_id, body, "", mentions, desc.command, args_obj.dump());
    if (!r.ok)
    {
        return {false, r.message};
    }
    return {true, ""};
}

namespace
{

std::string display_argument_value(const nlohmann::json& v)
{
    if (v.is_string())
    {
        return v.get<std::string>();
    }
    if (v.is_object())
    {
        // room_id / event_id reference: show the thing it points at.
        const auto type = v.find("type");
        const char* key =
            (type != v.end() && type->is_string() && type->get<std::string>() == "event_id")
                ? "event_id"
                : "id";
        const auto it = v.find(key);
        if (it != v.end() && it->is_string())
        {
            return it->get<std::string>();
        }
        return v.dump();
    }
    if (v.is_array())
    {
        std::string out;
        for (const auto& item : v)
        {
            if (!out.empty())
            {
                out += ", ";
            }
            out += display_argument_value(item);
        }
        return out;
    }
    return v.dump();
}

} // namespace

std::string format_bot_command_display(const std::string& command,
                                       const std::string& arguments_json)
{
    if (command.empty())
    {
        return {};
    }
    std::string out = "/" + command;
    if (arguments_json.empty())
    {
        return out;
    }
    const auto args = nlohmann::json::parse(arguments_json, nullptr, /*allow_exceptions=*/false);
    if (!args.is_object())
    {
        return out;
    }
    for (const auto& [name, value] : args.items())
    {
        out += " " + name + ": " + display_argument_value(value);
    }
    return out;
}

bool is_user_id(const std::string& text)
{
    return looks_like_user_id(text);
}

} // namespace tesseract
