#include "views/SlashCommandController.h"

#include "app/SlashCommands.h"

#include <tesseract/client.h>

#include <algorithm>
#include <utility>

namespace tesseract::views
{

SlashCommandController::SlashCommandController(tk::NativeTextArea* text_area,
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

bool SlashCommandController::on_text_changed(const std::string& text, int cursor)
{
    if (!text_area_ || !popup_)
    {
        return false;
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
    auto items = engine_.lookup(m->prefix);
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
    int cur = popup_->selected_index();
    int n = popup_->visible_rows();
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
        if (sel >= 0 && sel < popup_->visible_rows())
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
    int sel = popup_->selected_index();
    if (sel >= 0 && sel < popup_->visible_rows())
    {
        accept(popup_->suggestion_at(sel));
        return true;
    }
    hide();
    return false;
}

void SlashCommandController::accept(const SlashCommandSuggestion& s)
{
    hide();
    if (!text_area_)
    {
        return;
    }
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

void SlashCommandController::hide()
{
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
