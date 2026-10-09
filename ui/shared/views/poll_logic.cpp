#include "views/poll_logic.h"

#include "text_util.h"
#include "tk/i18n.h"
#include "views/MessageListView.h"

#include <algorithm>
#include <set>

namespace tesseract::views
{

PollDraft normalize_poll_draft(PollDraft d)
{
    d.question = text::trim(d.question);
    std::vector<std::string> kept;
    for (auto& o : d.options)
    {
        std::string t = text::trim(o);
        if (!t.empty())
            kept.push_back(std::move(t));
    }
    d.options = std::move(kept);
    return d;
}

bool poll_draft_valid(const PollDraft& d)
{
    if (d.question.empty() || d.options.size() < kPollMinOptions ||
        d.options.size() > kPollMaxOptions)
        return false;
    std::set<std::string> seen(d.options.begin(), d.options.end());
    return seen.size() == d.options.size();
}

std::uint32_t poll_draft_max_selections(const PollDraft& d)
{
    return d.multiple ? static_cast<std::uint32_t>(d.options.size()) : 1u;
}

std::optional<std::vector<std::string>>
next_poll_selection(const std::vector<std::string>& current, const std::string& clicked,
                    std::uint32_t max_selections)
{
    if (clicked.empty())
        return std::nullopt;
    const auto it = std::find(current.begin(), current.end(), clicked);
    const bool selected = it != current.end();
    if (max_selections <= 1)
    {
        if (selected && current.size() == 1)
            return std::nullopt;
        return std::vector<std::string>{clicked};
    }
    std::vector<std::string> next = current;
    if (selected)
    {
        next.erase(next.begin() + (it - current.begin()));
        return next;
    }
    if (current.size() >= max_selections)
        return std::nullopt;
    next.push_back(clicked);
    return next;
}

int poll_percent(std::uint32_t votes, std::uint32_t total)
{
    if (total == 0)
        return 0;
    return static_cast<int>((100.0 * votes) / total + 0.5);
}

std::string poll_status_text(const MessageRowData& m)
{
    const std::string n = std::to_string(m.poll_total_votes);
    if (m.poll_ended)
        return tk::trf(tk::trn("Poll ended · {0} vote", "Poll ended · {0} votes", m.poll_total_votes),
                       {n});
    if (m.poll_results_visible)
        return tk::trf(tk::trn("{0} vote", "{0} votes", m.poll_total_votes), {n});
    return tk::tr("Results will be shown when the poll ends");
}

std::string poll_hint_text(const MessageRowData& m)
{
    if (m.poll_ended)
        return {};
    // A foreign poll may declare more selections than it has answers.
    const auto max = std::min<std::size_t>(m.poll_max_selections, m.poll_answers.size());
    if (max <= 1)
        return tk::tr("Select one option");
    return tk::trf(tk::trn("Select up to {0} option", "Select up to {0} options",
                           static_cast<long>(max)),
                   {std::to_string(max)});
}

bool poll_card_interactive(const MessageRowData& m, bool callbacks_set)
{
    return callbacks_set && !m.poll_ended && !m.event_id.empty() &&
           m.pending_state == MessageRowData::PendingState::None;
}

bool poll_show_end_button(const MessageRowData& m, bool can_redact_others)
{
    return !m.poll_ended && (m.is_own || can_redact_others);
}

} // namespace tesseract::views
