#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

struct SlashCommandMatch
{
    int start;          ///< UTF-8 byte offset of the leading '/'.
    int end;            ///< UTF-8 byte offset one past the last typed char.
    std::string prefix; ///< Characters typed after '/' (no slash).
};

struct SlashCommandSuggestion
{
    std::string name;         ///< Canonical name without leading slash.
    std::string args_hint;    ///< e.g. "<action>".
    std::string description;  ///< One-line user-facing description.
};

/// Stateless helper — all inputs passed per call; no members.
class SlashCommandEngine
{
public:
    /// Returns a match when `text` is a slash-command prefix at the start
    /// of the composer: `^/[A-Za-z_]*$` with the cursor at end-of-text.
    /// Returns nullopt for empty text, mid-message slashes, slashes
    /// followed by a space (args entered), or non-letter chars after '/'.
    std::optional<SlashCommandMatch>
    find_prefix(std::string_view text, int cursor_byte_pos) const;

    /// Filter the canonical command list (from
    /// `tesseract::available_commands()`) by name prefix. Exact match
    /// first, then prefix matches in registry order. Returns at most
    /// `max_results` suggestions. Empty prefix returns the full list.
    std::vector<SlashCommandSuggestion>
    lookup(std::string_view prefix, int max_results = 8) const;
};

} // namespace tesseract::views
