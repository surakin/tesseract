#include "views/SlashCommandEngine.h"
#include "app/SlashCommands.h"

namespace tesseract::views
{

namespace
{

constexpr bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Compact positional hint shown next to "/name" in the popup row, e.g.
// "<target_room> <timeout_seconds> [apply_to_policy] <target_users...>".
// Mirrors the v1 trailing-optional, one-token-per-parameter convention
// `Client::match_bot_command_arguments` implements (array params take one
// comma-separated token, hence the "...").
std::string build_bot_args_hint(const tesseract::CommandDescription& d)
{
    std::string hint;
    for (const auto& p : d.parameters)
    {
        if (!hint.empty())
        {
            hint += ' ';
        }
        std::string token = p.key;
        if (p.schema.kind == tesseract::ParamSchemaKind::Array)
        {
            token += "...";
        }
        hint += p.optional ? ("[" + token + "]") : ("<" + token + ">");
    }
    return hint;
}

bool shadowed_by_builtin(const std::vector<SlashCommandSuggestion>& out,
                         const std::string& name)
{
    for (const auto& s : out)
    {
        if (s.is_builtin && s.name == name)
        {
            return true;
        }
    }
    return false;
}

}  // namespace

std::optional<SlashCommandMatch>
SlashCommandEngine::find_prefix(std::string_view text, int cursor_byte_pos) const
{
    // Activate only when the slash is at position 0 (start of composer)
    // and the cursor is at end-of-text. Anything else is a literal slash
    // in a message — pass through.
    if (text.empty() || text[0] != '/') return std::nullopt;
    if (cursor_byte_pos != (int)text.size()) return std::nullopt;

    // All chars after the leading '/' must be name chars (no spaces,
    // no digits, no punctuation). The first space terminates the popup
    // because the user has moved on to typing args.
    for (std::size_t i = 1; i < text.size(); ++i)
    {
        if (!is_name_char(text[i])) return std::nullopt;
    }
    SlashCommandMatch m;
    m.start = 0;
    m.end = (int)text.size();
    m.prefix = std::string(text.substr(1));
    return m;
}

std::vector<SlashCommandSuggestion>
SlashCommandEngine::lookup(std::string_view prefix,
                           const std::vector<tesseract::CommandDescription>& bot_commands,
                           int max_results) const
{
    std::vector<SlashCommandSuggestion> out;
    const auto& all = ::tesseract::available_commands();

    auto push_bot = [&](const tesseract::CommandDescription& d)
    {
        SlashCommandSuggestion s;
        s.name = d.command;
        s.args_hint = build_bot_args_hint(d);
        s.description = d.description;
        s.is_builtin = false;
        s.bot_sender_id = d.sender;
        s.bot_display_name = d.sender_display_name;
        s.bot_parameters = d.parameters;
        out.push_back(std::move(s));
    };

    // Pass 1: exact match — built-in first (only one possible), then every
    // bot command with this exact name not shadowed by that built-in.
    for (const auto& c : all)
    {
        if (c.name == prefix)
        {
            out.push_back({c.name, c.args_hint, c.description});
            break;
        }
    }
    for (const auto& d : bot_commands)
    {
        if (!d.valid || d.command != prefix) continue;
        if (shadowed_by_builtin(out, d.command)) continue;
        push_bot(d);
    }

    // Pass 2: prefix matches — built-ins in registry order, skipping the
    // exact one, then bot commands.
    for (const auto& c : all)
    {
        if ((int)out.size() >= max_results) break;
        if (c.name == prefix) continue;  // already added
        if (c.name.size() < prefix.size()) continue;
        if (c.name.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back({c.name, c.args_hint, c.description});
    }
    for (const auto& d : bot_commands)
    {
        if ((int)out.size() >= max_results) break;
        if (!d.valid || d.command == prefix) continue;  // already added above
        if (d.command.size() < prefix.size()) continue;
        if (d.command.compare(0, prefix.size(), prefix) != 0) continue;
        if (shadowed_by_builtin(out, d.command)) continue;
        push_bot(d);
    }
    return out;
}

} // namespace tesseract::views
