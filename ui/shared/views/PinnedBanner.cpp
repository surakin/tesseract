#include "PinnedBanner.h"

#include "icons.h"
#include "sdk_text.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/theme.h"

namespace tesseract::views
{

namespace
{

// UTF-8-safe truncation (mirrors ThreadListView::truncate_utf8): clip to
// `max_bytes` bytes, then back off until we're past the next UTF-8 start
// byte so we never split a code-point. Folds newlines to single spaces
// because previews are single-line. Named distinctly from ThreadListView's
// identical helper (not just anonymous-namespace-local) because this repo's
// unity build concatenates multiple .cpp files into one translation unit,
// where two same-named functions in two textually-separate anonymous
// namespaces still collide.
std::string pinned_banner_truncate_utf8(std::string s, std::size_t max_bytes)
{
    for (char& c : s)
    {
        if (c == '\n' || c == '\r')
        {
            c = ' ';
        }
    }
    if (s.size() <= max_bytes)
    {
        return s;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    s.resize(cut);
    s += "...";
    return s;
}

// The banner body: a full-height Subtle button that only paints its
// hover/press fill — PinnedBanner draws the preview text over it.
class PinBodyButton : public tk::Button
{
protected:
    PinBodyButton() : tk::Button("", {}, tk::Button::Variant::Subtle)
    {
    }
    TK_WIDGET_FACTORY_FRIEND(PinBodyButton)

    bool paints_content() const override
    {
        return false;
    }
};

} // namespace

PinnedBanner::PinnedBanner()
{
    body_btn_ = add_child(tk::create_widget<PinBodyButton>(this));
    body_btn_->set_on_click(
        [this]
        {
            if (current_index_ < pins_.size() && on_jump_to)
            {
                on_jump_to(pins_[current_index_].event_id);
            }
        });

    up_btn_ = add_child(tk::create_widget<tk::Button>(
        this, std::string{},
        [this]
        {
            step_(-1);
        },
        tk::Button::Variant::Icon));
    up_btn_->set_icon(kChevronUpSvg, 14.0f);
    up_btn_->set_accessible_name(tk::tr("Previous pinned message"));

    down_btn_ = add_child(tk::create_widget<tk::Button>(
        this, std::string{},
        [this]
        {
            step_(1);
        },
        tk::Button::Variant::Icon));
    down_btn_->set_icon(kChevronDownSvg, 14.0f);
    down_btn_->set_accessible_name(tk::tr("Next pinned message"));
    sync_buttons_();
}

std::string PinnedBanner::current_preview() const
{
    if (current_index_ >= pins_.size())
    {
        return {};
    }
    const auto& p = pins_[current_index_];
    const std::string preview = sdk_text::localize(p.body_preview);
    if (p.sender_name.empty())
    {
        return preview;
    }
    if (preview.empty())
    {
        return p.sender_name;
    }
    return tk::trf(tk::tr("{0}: {1}"), {p.sender_name, preview});
}

std::string PinnedBanner::access_name() const
{
    return tk::tr("Pinned messages");
}

std::string PinnedBanner::access_description() const
{
    if (pins_.size() < 2)
    {
        return {};
    }
    return tk::trf(tk::tr("{0} of {1}"),
                   {std::to_string(current_index_ + 1), std::to_string(pins_.size())});
}

void PinnedBanner::step_(int delta)
{
    if (delta < 0 && current_index_ > 0)
    {
        --current_index_;
    }
    else if (delta > 0 && current_index_ + 1 < pins_.size())
    {
        ++current_index_;
    }
    else
    {
        return;
    }
    sync_buttons_();
    if (host())
    {
        host()->request_repaint();
    }
}

// Visibility, names, and enabled state for the current pin set / index.
void PinnedBanner::sync_buttons_()
{
    const bool any = !pins_.empty();
    const bool many = pins_.size() > 1;
    body_btn_->set_visible(any);
    up_btn_->set_visible(many);
    down_btn_->set_visible(many);
    up_btn_->set_enabled(current_index_ > 0);
    down_btn_->set_enabled(current_index_ + 1 < pins_.size());
    body_btn_->set_accessible_name(
        any ? tk::trf(tk::tr("Jump to pinned message: {0}"), {current_preview()}) : std::string());
}

void PinnedBanner::set_pins(std::vector<tesseract::PinnedEvent> pins)
{
    pins_ = std::move(pins);
    if (pins_.empty())
    {
        current_index_ = 0;
    }
    else if (current_index_ >= pins_.size())
    {
        current_index_ = pins_.size() - 1;
    }
    sync_buttons_();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size PinnedBanner::measure(tk::LayoutCtx& /*ctx*/, tk::Size c)
{
    // Zero-height when empty so RoomView's arrange skips us cleanly.
    return tk::Size{c.w, pins_.empty() ? 0.0f : kBannerH};
}

void PinnedBanner::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);
    if (pins_.empty())
    {
        body_rect_ = {};
        return;
    }
    // Chevrons stacked vertically on the right; body fills the rest.
    const float right = bounds.x + bounds.w;
    const tk::Rect up_rect{right - kChevronPad - kChevronSz, bounds.y + 2.0f, kChevronSz,
                           kChevronSz};
    const tk::Rect down_rect{right - kChevronPad - kChevronSz, bounds.y + 2.0f + kChevronSz,
                             kChevronSz, kChevronSz};
    // Leave room for the chevron column + counter strip on the right.
    const float reserved = kChevronSz + 2.0f * kChevronPad + kCounterW;
    body_rect_ = {bounds.x, bounds.y, bounds.w - reserved, bounds.h};
    up_btn_->set_min_size({kChevronSz, kChevronSz});
    down_btn_->set_min_size({kChevronSz, kChevronSz});
    up_btn_->arrange(lc, up_rect);
    down_btn_->arrange(lc, down_rect);
    body_btn_->arrange(lc, body_rect_);
}

// ── paint ─────────────────────────────────────────────────────────────────

void PinnedBanner::paint(tk::PaintCtx& ctx)
{
    if (pins_.empty())
    {
        return;
    }

    auto& cv = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    // Banner background uses chrome_bg so it visually reads as a strip of
    // chrome rather than chat content. A 1px separator at the bottom keeps
    // it visually distinct from the message list below.
    cv.fill_rect(bounds_, pal.chrome_bg);
    cv.fill_rect({bounds_.x, bounds_.bottom() - 1.0f, bounds_.w, 1.0f}, pal.separator);

    // Body button's hover / press fill under the preview text.
    body_btn_->paint(ctx);

    // "<sender>: <body>" preview, truncated UTF-8-safely.
    std::string preview = pinned_banner_truncate_utf8(current_preview(), 80);

    tk::TextStyle body_style{};
    body_style.role = tk::FontRole::Body;
    body_style.wrap = false;
    body_style.trim = tk::TextTrim::Ellipsis;
    body_style.max_width = body_rect_.w - 2.0f * kPadX;
    auto body_layout = ctx.factory.build_text(preview, body_style);
    if (body_layout)
    {
        const tk::Size sz = body_layout->measure();
        const float ty = body_rect_.y + (body_rect_.h - sz.h) * 0.5f;
        cv.draw_text(*body_layout, {body_rect_.x + kPadX, ty}, pal.text_primary);
    }

    tk::TextStyle small_style{};
    small_style.role = tk::FontRole::Small;
    small_style.wrap = false;

    // Counter strip ("i/n") + chevrons only when more than one pin exists.
    if (pins_.size() > 1)
    {
        const std::string counter =
            std::to_string(current_index_ + 1) + "/" + std::to_string(pins_.size());
        auto counter_layout = ctx.factory.build_text(counter, small_style);
        if (counter_layout)
        {
            const tk::Size sz = counter_layout->measure();
            const float cx = body_rect_.x + body_rect_.w + (kCounterW - sz.w) * 0.5f;
            const float cy = bounds_.y + (bounds_.h - sz.h) * 0.5f;
            cv.draw_text(*counter_layout, {cx, cy}, pal.text_secondary);
        }

        up_btn_->paint(ctx);
        down_btn_->paint(ctx);
    }
}

} // namespace tesseract::views
