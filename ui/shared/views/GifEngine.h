#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace tesseract::views
{

/// Stateless helper that recognises a `/gif <query>` composer command.
/// Unlike the shortcode/mention engines (which match mid-text), `/gif` owns the
/// whole composer: the command is active only when the entire text is
/// `"/gif "` followed by a non-empty query. Drives the inline GIF result strip.
class GifEngine
{
public:
    /// Returns the trimmed search query when `text` is a `/gif <query>` command
    /// with at least one non-whitespace query character, or `nullopt` when it
    /// is not a `/gif` command (or has no query yet — e.g. bare `"/gif "`).
    std::optional<std::string> match(std::string_view text) const;
};

} // namespace tesseract::views
