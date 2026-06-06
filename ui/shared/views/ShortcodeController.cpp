#include "views/ShortcodeController.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

namespace
{
const std::vector<tesseract::ImagePackImage>& empty_emoticons()
{
    static const std::vector<tesseract::ImagePackImage> kEmpty;
    return kEmpty;
}
} // namespace

ShortcodeController::ShortcodeController(tk::NativeTextArea* text_area,
                                        ShortcodePopup* popup, Hooks hooks)
    : text_area_(text_area), popup_(popup), hooks_(std::move(hooks))
{
    if (popup_)
    {
        popup_->on_accepted = [this](ShortcodeSuggestion s) { accept(s); };
        popup_->on_dismissed = [this] { hide(); };
    }
}

ShortcodeController::~ShortcodeController() = default;

bool ShortcodeController::on_text_changed(const std::string& text, int cursor)
{
    if (!text_area_ || !popup_)
    {
        return false;
    }

    const std::vector<tesseract::ImagePackImage>& packs =
        hooks_.emoticons ? hooks_.emoticons() : empty_emoticons();

    // Auto-expand: ":smile:" followed by a space / EOT → replace with the glyph.
    if (auto complete = engine_.find_complete(text, cursor))
    {
        auto hits = engine_.lookup(complete->prefix, packs, 1);
        std::string r = (!hits.empty() && !hits.front().glyph.empty())
                            ? hits.front().glyph
                            : ":" + complete->prefix + ":";
        text_area_->replace_range(complete->start, complete->end, r);
        hide();
        return true;
    }

    // Popup: ":gri" → show suggestions once at least two chars are typed.
    auto m = engine_.find_prefix(text, cursor);
    if (!m || m->prefix.size() < static_cast<std::size_t>(kMinPrefix))
    {
        if (visible_)
        {
            hide();
        }
        return false;
    }
    suggestions_ = engine_.lookup(m->prefix, packs);
    if (suggestions_.empty())
    {
        if (visible_)
        {
            hide();
        }
        return false;
    }
    active_match_ = *m;
    if (hooks_.fetch_image)
    {
        for (const auto& sugg : suggestions_)
        {
            if (!sugg.emoticon.url.empty())
            {
                hooks_.fetch_image(sugg.emoticon.url);
            }
        }
    }
    popup_->set_suggestions(suggestions_); // preselects row 0
    if (hooks_.show)
    {
        hooks_.show(text_area_->cursor_rect(), popup_->visible_rows());
    }
    visible_ = true;
    return true;
}

bool ShortcodeController::on_nav(tk::NavKey nk)
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
        if (cur >= 0 && cur < static_cast<int>(suggestions_.size()))
        {
            accept(suggestions_[cur]);
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

bool ShortcodeController::on_submit()
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    int sel = popup_->selected_index();
    if (sel >= 0 && sel < static_cast<int>(suggestions_.size()))
    {
        accept(suggestions_[sel]);
        return true;
    }
    hide();
    return false;
}

void ShortcodeController::accept(const ShortcodeSuggestion& s)
{
    std::string r = s.glyph.empty() ? ":" + s.shortcode + ":" : s.glyph;
    replace_with(r);
}

void ShortcodeController::replace_with(const std::string& r)
{
    if (text_area_)
    {
        text_area_->replace_range(active_match_.start, active_match_.end, r);
    }
    hide();
}

void ShortcodeController::hide()
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
