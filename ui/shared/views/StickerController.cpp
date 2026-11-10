#include "views/StickerController.h"

#include "tk/i18n.h"

#include <utility>

namespace tesseract::views
{

StickerController::StickerController(StickerPopup* popup, Hooks hooks)
    : popup_(popup), hooks_(std::move(hooks))
{
    if (popup_)
    {
        popup_->on_accepted = [this](const tesseract::ImagePackImage& s)
        {
            accept(s);
        };
        popup_->on_dismissed = [this]
        {
            hide();
        };
    }
}

StickerController::~StickerController()
{
    invalidate_weak_self();
}

bool StickerController::on_text_changed(const std::string& text)
{
    auto query = engine_.match(text);
    if (!query)
    {
        matched_ = false;
        hide();
        return false;
    }
    matched_ = true;
    if (!popup_)
    {
        return true;
    }
    if (query->empty())
    {
        // Nothing typed yet: no strip, but the command still owns the composer.
        hide();
        return true;
    }

    auto results = hooks_.search ? hooks_.search(*query) : std::vector<tesseract::ImagePackImage>{};
    if (results.empty())
    {
        popup_->set_status(tk::tr("No stickers found"));
    }
    else
    {
        popup_->set_results(std::move(results));
    }
    show_popup_();
    return true;
}

void StickerController::show_popup_()
{
    visible_ = true;
    if (hooks_.show)
    {
        hooks_.show();
    }
    if (hooks_.repaint)
    {
        hooks_.repaint();
    }
}

bool StickerController::on_nav(tk::NavKey nk)
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    switch (nk)
    {
    case tk::NavKey::Right:
    case tk::NavKey::Down:
    case tk::NavKey::Tab:
        popup_->move_selection(+1);
        break;
    case tk::NavKey::Left:
    case tk::NavKey::Up:
    case tk::NavKey::ShiftTab:
        popup_->move_selection(-1);
        break;
    case tk::NavKey::Escape:
        hide();
        return true;
    }
    repaint_selection_();
    return true;
}

bool StickerController::on_submit()
{
    if (!matched_)
    {
        return false;
    }
    if (visible_ && popup_)
    {
        if (const auto* sel = popup_->selected())
        {
            accept(*sel);
        }
    }
    return true; // never send the "/sticker …" text itself
}

void StickerController::accept(const tesseract::ImagePackImage& sticker)
{
    // Copy before hide(): `sticker` points into the popup's result storage,
    // which hide() frees.
    const tesseract::ImagePackImage s = sticker;
    hide();
    if (hooks_.clear_composer)
    {
        hooks_.clear_composer();
    }
    if (hooks_.send)
    {
        hooks_.send(s);
    }
}

void StickerController::repaint_selection_()
{
    if (hooks_.repaint_selection)
    {
        hooks_.repaint_selection();
    }
    else if (hooks_.repaint)
    {
        hooks_.repaint();
    }
}

void StickerController::hide()
{
    if (!visible_)
    {
        return;
    }
    visible_ = false;
    if (popup_)
    {
        popup_->set_results({});
    }
    if (hooks_.hide)
    {
        hooks_.hide();
    }
}

} // namespace tesseract::views
