#include "views/SlashCommandController.h"

#include "app/SlashCommands.h"

#include <tesseract/client.h>

#include <algorithm>
#include <utility>

namespace tesseract::views
{

SlashCommandController::SlashCommandController(tk::TextArea* text_area,
                                              SlashCommandPopup* popup,
                                              Hooks hooks)
    : text_area_(text_area), popup_(popup), hooks_(std::move(hooks))
{
    if (popup_)
    {
        popup_->on_accepted = [this](SlashCommandSuggestion s) { accept(s); };
        popup_->on_dismissed = [this] { hide(); };
    }
}

SlashCommandController::~SlashCommandController() = default;

namespace
{

// Count whitespace-delimited tokens in `s` that are already "closed" by a
// trailing space — a token still being typed (no trailing whitespace yet)
// doesn't count, so the hint keeps pointing at the parameter currently
// being entered rather than jumping ahead on the first keystroke.
std::size_t completed_token_count(const std::string& s)
{
    std::size_t count = 0;
    bool in_token = false;
    for (char c : s)
    {
        bool space = (c == ' ' || c == '\t');
        if (!space && !in_token)
        {
            in_token = true;
        }
        else if (space && in_token)
        {
            in_token = false;
            ++count;
        }
    }
    return count;
}

}  // namespace

bool SlashCommandController::on_text_changed(const std::string& text, int cursor)
{
    if (!text_area_ || !popup_)
    {
        return false;
    }

    if (active_bot_command_)
    {
        std::string prefix = "/" + active_bot_command_->name + " ";
        if (text.rfind(prefix, 0) == 0)
        {
            update_arg_hint(text.substr(prefix.size()));
            return true;
        }
        // User backspaced past the command name (or retyped over it) — exit
        // arg-collection mode and fall through to normal handling below.
        hide();
    }

    auto m = engine_.find_prefix(text, cursor);
    if (!m)
    {
        if (visible_)
        {
            hide();
        }
        return false;
    }
    auto bot_cmds = hooks_.bot_commands ? hooks_.bot_commands()
                                        : std::vector<tesseract::CommandDescription>{};
    auto items = engine_.lookup(m->prefix, bot_cmds);
    if (items.empty())
    {
        // No prefix match — show the full command list instead of hiding, so
        // the user can still browse/pick one.
        items = engine_.lookup("", bot_cmds);
    }
    if (items.empty())
    {
        if (visible_)
        {
            hide();
        }
        return false;
    }
    popup_->set_suggestions(std::move(items)); // preselects row 0
    if (hooks_.show)
    {
        hooks_.show(text_area_->cursor_rect(), popup_->visible_rows());
    }
    visible_ = true;
    return true;
}

bool SlashCommandController::on_nav(tk::NavKey nk)
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    if (active_bot_command_)
    {
        // No suggestion list while collecting args — only Escape does
        // anything (cancels back to a plain composer).
        if (nk == tk::NavKey::Escape)
        {
            hide();
            return true;
        }
        return false;
    }
    int cur = popup_->selected_index();
    int n = (int)popup_->total_rows();
    if (n <= 0)
    {
        return true;
    }
    int next = cur;
    switch (nk)
    {
    case tk::NavKey::Up:
        next = std::max(0, cur - 1);
        break;
    case tk::NavKey::Down:
        next = std::min(n - 1, cur + 1);
        break;
    case tk::NavKey::Tab:
    {
        int sel = popup_->selected_index();
        if (sel >= 0 && sel < (int)popup_->total_rows())
        {
            accept(popup_->suggestion_at(sel));
        }
        else
        {
            hide();
        }
        return true;
    }
    case tk::NavKey::ShiftTab:
        return false;
    case tk::NavKey::Escape:
        hide();
        return true;
    default:
        // Left/Right: let the caret move.
        return false;
    }
    popup_->set_selected_index(next);
    if (hooks_.repaint)
    {
        hooks_.repaint();
    }
    return true;
}

bool SlashCommandController::on_submit()
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    if (active_bot_command_)
    {
        return submit_bot_command();
    }
    int sel = popup_->selected_index();
    if (sel >= 0 && sel < (int)popup_->total_rows())
    {
        accept(popup_->suggestion_at(sel));
        return true;
    }
    hide();
    return false;
}

void SlashCommandController::accept(const SlashCommandSuggestion& s)
{
    if (!text_area_)
    {
        return;
    }

    if (!s.is_builtin)
    {
        // Bot command — always autocomplete to "/name " and enter
        // arg-collection mode; never dispatched arg-less even when it
        // declares zero parameters (one code path for every bot command).
        std::string body = "/" + s.name + " ";
        text_area_->replace_range(
            0, static_cast<int>(text_area_->text().size()), body);
        active_bot_command_ = s;
        update_arg_hint(std::string{});
        return;
    }

    hide();
    if (s.args_hint.empty())
    {
        // /selfie — open camera overlay instead of sending a message.
        if (s.name == "selfie")
        {
            text_area_->set_text("");
            if (hooks_.clear_composer)
                hooks_.clear_composer();
            if (hooks_.on_selfie)
                hooks_.on_selfie();
            return;
        }

        // /location — fetch and send the device's current location instead of
        // sending a message.
        if (s.name == "location")
        {
            text_area_->set_text("");
            if (hooks_.clear_composer)
                hooks_.clear_composer();
            if (hooks_.on_location)
                hooks_.on_location();
            return;
        }

        // No args — dispatch immediately, then clear the composer.
        tesseract::Client* c = hooks_.client ? hooks_.client() : nullptr;
        std::string rid = hooks_.room_id ? hooks_.room_id() : std::string{};
        if (!c || rid.empty())
        {
            // Account torn down while the popup was open — nothing to send.
            return;
        }
        std::string body = "/" + s.name;
        (void)tesseract::dispatch_compose_send(*c, rid, body, std::string{});
        text_area_->set_text("");
        if (hooks_.clear_composer)
        {
            hooks_.clear_composer();
        }
    }
    else
    {
        // Needs args — autocomplete to `/name ` and leave the composer open for
        // the user to type arguments. replace_range (not set_text) keeps the
        // caret after the trailing space and the shared composer state in sync.
        std::string body = "/" + s.name + " ";
        text_area_->replace_range(
            0, static_cast<int>(text_area_->text().size()), body);
    }
}

void SlashCommandController::update_arg_hint(const std::string& args_text,
                                             const std::string& error)
{
    if (!active_bot_command_ || !popup_)
    {
        return;
    }
    tesseract::CommandDescription desc;
    desc.command = active_bot_command_->name;
    desc.parameters = active_bot_command_->bot_parameters;

    bool is_error = !error.empty();
    std::string text = is_error
                           ? error
                           : tesseract::next_bot_command_arg_hint(
                                 desc, completed_token_count(args_text));
    popup_->show_hint(text, is_error);
    if (hooks_.show)
    {
        hooks_.show(text_area_->cursor_rect(), 1);
    }
    visible_ = true;
    if (hooks_.repaint)
    {
        hooks_.repaint();
    }
}

bool SlashCommandController::submit_bot_command()
{
    if (!active_bot_command_ || !text_area_)
    {
        return false;
    }
    tesseract::Client* c = hooks_.client ? hooks_.client() : nullptr;
    std::string rid = hooks_.room_id ? hooks_.room_id() : std::string{};
    if (!c || rid.empty())
    {
        // Account torn down while the popup was open — nothing to send.
        hide();
        return true;
    }

    std::string prefix = "/" + active_bot_command_->name + " ";
    std::string text = text_area_->text();
    std::string args_text =
        text.rfind(prefix, 0) == 0 ? text.substr(prefix.size()) : std::string{};

    tesseract::CommandDescription desc;
    desc.command = active_bot_command_->name;
    desc.sender = active_bot_command_->bot_sender_id;
    desc.parameters = active_bot_command_->bot_parameters;

    auto result = tesseract::dispatch_bot_command_send(*c, rid, desc, args_text);
    if (!result.ok)
    {
        // Keep the composer text intact and show the error in place of the
        // hint — the MSC's whole point is not regressing "no feedback on
        // bad input", so a failed send must never silently no-op.
        update_arg_hint(args_text, result.error);
        return true;
    }

    text_area_->set_text("");
    if (hooks_.clear_composer)
    {
        hooks_.clear_composer();
    }
    hide();
    return true;
}

void SlashCommandController::hide()
{
    active_bot_command_.reset();
    if (popup_)
    {
        popup_->clear_hint();
    }
    if (!visible_)
    {
        return;
    }
    visible_ = false;
    if (hooks_.hide)
    {
        hooks_.hide();
    }
}

} // namespace tesseract::views
