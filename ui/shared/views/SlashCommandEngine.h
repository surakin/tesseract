#pragma once
#include <tesseract/bot_command.h>

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
    /// False for an MSC4391 bot-advertised command. Built-ins always win a
    /// name collision against a bot command (MSC4391 client-precedence
    /// rule) — see `SlashCommandEngine::lookup`.
    bool is_builtin = true;
    /// Bot's mxid — empty for a built-in. Used for `m.mentions` on send and
    /// for popup row disambiguation when two bots expose the same command
    /// name.
    std::string bot_sender_id;
    std::string bot_display_name;
    /// The full parameter list — empty for a built-in. Stashed here so the
    /// controller has it after `accept()` without a second lookup.
    std::vector<tesseract::CommandParameter> bot_parameters;
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
    /// `tesseract::available_commands()`) plus `bot_commands` (already
    /// filtered to the active room and joined senders by the caller — see
    /// `Client::list_room_bot_commands`) by name prefix. Exact match first
    /// (built-ins before bot commands), then prefix matches in registry
    /// order (built-ins), then bot commands. A bot command whose name
    /// collides with a built-in is dropped (MSC4391 client-precedence
    /// rule); two different bots exposing the same command name both
    /// survive, distinguished by `bot_sender_id`/`bot_display_name`.
    /// `!valid` bot command descriptions are skipped entirely. Returns at
    /// most `max_results` suggestions. Empty prefix returns the full list.
    /// The default is comfortably above the registry size — filtering and
    /// display windowing are decoupled, so this is not a practical cap; the
    /// popup handles scrolling/windowing for the visible rows itself.
    std::vector<SlashCommandSuggestion>
    lookup(std::string_view prefix,
           const std::vector<tesseract::CommandDescription>& bot_commands,
           int max_results = 64) const;
};

} // namespace tesseract::views
