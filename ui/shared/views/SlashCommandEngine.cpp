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
SlashCommandEngine::lookup(std::string_view prefix, int max_results) const
{
    std::vector<SlashCommandSuggestion> out;
    const auto& all = ::tesseract::available_commands();

    // Pass 1: exact match (only one possible).
    for (const auto& c : all)
    {
        if (c.name == prefix)
        {
            out.push_back({c.name, c.args_hint, c.description});
            break;
        }
    }
    // Pass 2: prefix matches in registry order, skipping the exact one.
    for (const auto& c : all)
    {
        if ((int)out.size() >= max_results) break;
        if (c.name == prefix) continue;  // already added
        if (c.name.size() < prefix.size()) continue;
        if (c.name.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back({c.name, c.args_hint, c.description});
    }
    return out;
}

} // namespace tesseract::views
