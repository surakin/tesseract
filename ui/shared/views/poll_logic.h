#pragma once
// Pure poll (MSC3381) logic: composer-draft validation, vote-selection
// toggling, and status text. No painting, no widgets.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tesseract::views
{

struct MessageRowData;

struct PollDraft
{
    std::string question;
    std::vector<std::string> options;
    bool multiple = false;
    bool hide_results = false;
};

inline constexpr std::size_t kPollMinOptions = 2;
inline constexpr std::size_t kPollMaxOptions = 20;

// Trim the question and options, dropping blank options.
PollDraft normalize_poll_draft(PollDraft d);
bool poll_draft_valid(const PollDraft& normalized);
// multiple ? options.size() : 1
std::uint32_t poll_draft_max_selections(const PollDraft& normalized);

// Selection after the user clicks `clicked`; nullopt means "no change".
std::optional<std::vector<std::string>>
next_poll_selection(const std::vector<std::string>& current, const std::string& clicked,
                    std::uint32_t max_selections);

// Rounded percentage; 0 when total == 0.
int poll_percent(std::uint32_t votes, std::uint32_t total);

std::string poll_status_text(const MessageRowData& m);
// Secondary hint line ("Select one option"); empty once the poll ended.
std::string poll_hint_text(const MessageRowData& m);

// True when the card accepts votes: not ended, vote callback wired (the thread
// panel leaves it unset, so its cards are read-only), the event is confirmed
// by the server (has an id, not a pending local echo).
bool poll_card_interactive(const MessageRowData& m, bool callbacks_set);
// End-poll button: only while open, for the poll's creator or a moderator.
bool poll_show_end_button(const MessageRowData& m, bool can_redact_others);

} // namespace tesseract::views
