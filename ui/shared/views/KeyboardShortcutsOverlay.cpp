#include "KeyboardShortcutsOverlay.h"

#include "media_utils.h" // rect_contains
#include "shortcut_registry.h"

#include "tk/canvas.h"
#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/scroll_view.h"
#include "tk/theme.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace tesseract::views
{

namespace
{

// One shortcut: its description on the left, a key-cap chip per chord on
// the right. Painted text (not focusable): the overlay is read-only, and a
// screen reader gets "description, keys" from the StaticText node.
class ShortcutRow : public tk::Widget
{
protected:
    ShortcutRow(std::string description, std::vector<std::string> keys)
        : description_(std::move(description)), keys_(std::move(keys))
    {
    }
    TK_WIDGET_FACTORY_FRIEND(ShortcutRow)

public:
    tk::Size measure(tk::LayoutCtx& lc, tk::Size constraints) override
    {
        layout_(lc, constraints.w);
        return {constraints.w, height_};
    }

    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override
    {
        bounds_ = bounds;
        layout_(lc, bounds.w);
    }

    void paint(tk::PaintCtx& ctx) override
    {
        auto& cv = ctx.canvas;
        const auto& pal = ctx.theme.palette;
        // Side by side: both centred in the row. Stacked (narrow): the
        // description on top, the chips on their own line below it.
        const float desc_h = desc_layout_ ? desc_layout_->measure().h : 0.0f;
        const float desc_y =
            stacked_ ? bounds_.y + kRowPadY : bounds_.y + (height_ - desc_h) * 0.5f;
        const float chips_y =
            stacked_ ? desc_y + desc_h + kStackGap : bounds_.y + (height_ - chip_h_) * 0.5f;
        if (desc_layout_)
        {
            cv.draw_text(*desc_layout_, {bounds_.x, desc_y}, pal.text_primary);
        }
        // Chips are right-aligned, in chord order, never left of the row.
        float x = std::max(bounds_.x, bounds_.x + bounds_.w - chips_w_);
        for (const auto& chip : chip_layouts_)
        {
            const tk::Size s = chip->measure();
            const tk::Rect r{x, chips_y + (chip_h_ - (s.h + kChipPadY * 2)) * 0.5f,
                             s.w + kChipPadX * 2, s.h + kChipPadY * 2};
            cv.fill_rounded_rect(r, 4.0f, pal.compose_card_bg);
            cv.stroke_rounded_rect(r, 4.0f, pal.border_strong, 1.0f);
            cv.draw_text(*chip, {r.x + kChipPadX, r.y + kChipPadY}, pal.text_primary);
            x += r.w + kChipGap;
        }
    }

    void on_theme_changed(const tk::Theme& t) override
    {
        tk::Widget::on_theme_changed(t);
        laid_out_w_ = -1.0f; // font scale may have changed
    }

    tk::Role access_role() const override
    {
        return tk::Role::StaticText;
    }
    std::string access_name() const override
    {
        std::string keys;
        for (const auto& k : keys_)
        {
            if (!keys.empty())
            {
                keys += " / ";
            }
            keys += k;
        }
        return tk::trf(tk::tr("{0}, {1}"), {description_, keys});
    }

private:
    void layout_(tk::LayoutCtx& lc, float w)
    {
        if (w == laid_out_w_ && desc_layout_)
        {
            return;
        }
        laid_out_w_ = w;
        chip_layouts_.clear();
        chips_w_ = 0.0f;
        chip_h_ = 0.0f;
        for (const auto& k : keys_)
        {
            tk::TextStyle st{};
            st.role = tk::FontRole::UiSemibold;
            auto layout = lc.factory.build_text(k, st);
            if (!layout)
            {
                continue;
            }
            const tk::Size s = layout->measure();
            if (!chip_layouts_.empty())
            {
                chips_w_ += kChipGap;
            }
            chips_w_ += s.w + kChipPadX * 2;
            chip_h_ = std::max(chip_h_, s.h + kChipPadY * 2);
            chip_layouts_.push_back(std::move(layout));
        }
        // Too narrow to keep a readable description beside the chips: put
        // the chips on their own line instead of over the text.
        stacked_ = w - chips_w_ - kDescGap < kMinDescW;
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        st.wrap = true;
        st.max_width = std::max(1.0f, stacked_ ? w : w - chips_w_ - kDescGap);
        desc_layout_ = lc.factory.build_text(description_, st);
        const float desc_h = desc_layout_ ? desc_layout_->measure().h : 0.0f;
        height_ =
            (stacked_ ? desc_h + kStackGap + chip_h_ : std::max(desc_h, chip_h_)) + kRowPadY * 2;
    }

    std::string description_;
    std::vector<std::string> keys_;
    tk::CachedTextLayout desc_layout_;
    std::vector<std::unique_ptr<tk::TextLayout>> chip_layouts_;
    float laid_out_w_ = -1.0f;
    float chips_w_ = 0.0f;
    float chip_h_ = 0.0f;
    float height_ = 0.0f;
    bool stacked_ = false;

    static constexpr float kChipPadX = 6.0f;
    static constexpr float kChipPadY = 2.0f;
    static constexpr float kChipGap = 6.0f;
    static constexpr float kDescGap = 16.0f;
    static constexpr float kRowPadY = 5.0f;
    static constexpr float kStackGap = 4.0f;
    static constexpr float kMinDescW = 160.0f;
};

// Stacks the group headings and rows top to bottom at their natural height
// (the ScrollView measures it unbounded and scrolls it). Rows stop short of
// the right edge by the scrollbar gutter so the thumb never covers a chip.
class ShortcutColumn : public tk::Widget
{
protected:
    ShortcutColumn() = default;
    TK_WIDGET_FACTORY_FRIEND(ShortcutColumn)

public:
    tk::Size measure(tk::LayoutCtx& lc, tk::Size constraints) override
    {
        const float w = row_w_(constraints.w);
        float h = 0.0f;
        for (const auto& c : children())
        {
            h += c->measure(lc, {w, 0}).h + gap_before_(c.get());
        }
        return {constraints.w, h};
    }

    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override
    {
        bounds_ = bounds;
        const float w = row_w_(bounds.w);
        float y = bounds.y;
        for (const auto& c : children())
        {
            y += gap_before_(c.get());
            const float h = c->measure(lc, {w, 0}).h;
            c->arrange(lc, {bounds.x, y, w, h});
            y += h;
        }
    }

private:
    static float row_w_(float w)
    {
        return std::max(0.0f, w - tk::ScrollableBase::kScrollbarGutter);
    }

    // Space above every heading but the first.
    float gap_before_(const tk::Widget* w) const
    {
        const bool heading = dynamic_cast<const tk::Label*>(w) != nullptr;
        return heading && w != children().front().get() ? 16.0f : 0.0f;
    }
};

} // namespace

KeyboardShortcutsOverlay::KeyboardShortcutsOverlay()
{
    auto scroll = tk::create_widget<tk::ScrollView>(this);
    scroll->on_layout_changed = [this]()
    {
        if (auto* h = host())
        {
            h->mark_needs_relayout();
        }
    };
    scroll_ = add_child(std::move(scroll));
    build_rows_();

    close_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Close"),
        [this]()
        {
            close();
        },
        tk::Button::Variant::Primary));

    // Closed-by-default overlay (see ConfirmDialog).
    set_visible(false);
}

KeyboardShortcutsOverlay::~KeyboardShortcutsOverlay() = default;

void KeyboardShortcutsOverlay::build_rows_()
{
    auto column = tk::create_widget<ShortcutColumn>(scroll_);
    std::optional<ShortcutGroup> group;
    for (const auto& def : shortcuts())
    {
        if (group != def.group)
        {
            group = def.group;
            auto heading = tk::create_widget<tk::Label>(column.get(), group_title(def.group),
                                                        tk::FontRole::SenderName);
            column->add_child(std::move(heading));
        }
        std::vector<std::string> keys;
        keys.reserve(def.chords.size());
        for (const auto& chord : def.chords)
        {
            keys.push_back(tk::chord_label(chord));
        }
        column->add_child(
            tk::create_widget<ShortcutRow>(column.get(), tk::tr(def.description), std::move(keys)));
    }
    scroll_->set_child(std::move(column));
}

std::string KeyboardShortcutsOverlay::access_name() const
{
    return tk::tr("Keyboard Shortcuts");
}

void KeyboardShortcutsOverlay::open()
{
    if (open_)
    {
        return;
    }
    open_ = true;
    press_backdrop_ = false;
    set_visible(true);
    scroll_->scroll_to_top();
    pending_focus_ = true;
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

void KeyboardShortcutsOverlay::close()
{
    if (!open_)
    {
        return;
    }
    open_ = false;
    press_backdrop_ = false;
    pending_focus_ = false;
    set_visible(false);
    if (on_layout_changed)
    {
        on_layout_changed();
    }
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size KeyboardShortcutsOverlay::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void KeyboardShortcutsOverlay::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;
    backdrop_rect_ = bounds;

    const float card_w = std::max(0.0f, std::min(kCardMaxW, bounds.w - kMargin * 2));
    const float text_w = std::max(0.0f, card_w - kCardPad * 2);

    if (!title_layout_ || title_w_ != text_w)
    {
        title_w_ = text_w;
        tk::TextStyle st{};
        st.role = tk::FontRole::Title;
        st.trim = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        title_layout_ = lc.factory.build_text(tk::tr("Keyboard Shortcuts"), st);
    }

    // The card is as tall as the list needs, up to the surface (and a
    // comfortable reading height); the list scrolls beyond that.
    const float chrome_h = kCardPad * 2 + kTitleH + kGap * 2 + kBtnH;
    const float list_natural_h =
        scroll_->child()
            ? scroll_->child()->measure(lc, {text_w + tk::ScrollableBase::kScrollbarGutter, 0}).h
            : 0.0f;
    const float max_h = std::max(0.0f, std::min(kCardMaxH, bounds.h - kMargin * 2));
    const float card_h = std::min(chrome_h + list_natural_h, max_h);
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f, bounds.y + (bounds.h - card_h) * 0.5f,
                  card_w, card_h};

    const float list_y = card_rect_.y + kCardPad + kTitleH + kGap;
    const float btn_y = card_rect_.y + card_rect_.h - kCardPad - kBtnH;
    const float list_h = std::max(0.0f, btn_y - kGap - list_y);
    // The list reaches into the card's right padding by the scrollbar
    // gutter (which its rows keep clear), so the thumb sits in the padding
    // and the rows still line up with the title and the Close button.
    scroll_->arrange(lc, {card_rect_.x + kCardPad, list_y,
                          text_w + tk::ScrollableBase::kScrollbarGutter, list_h});

    const tk::Size btn_sz = close_btn_->measure(lc, {-1.0f, kBtnH});
    const float btn_w = std::max(btn_sz.w, 88.0f);
    close_btn_->arrange(lc, {card_rect_.x + card_rect_.w - kCardPad - btn_w, btn_y, btn_w, kBtnH});
}

// ── paint ─────────────────────────────────────────────────────────────────

void KeyboardShortcutsOverlay::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_)
    {
        return;
    }
    // Focus the Close button once the overlay is laid out, and after
    // MainAppWidget's own paint pass cleared focus for the newly opened
    // modal — so keys stop reaching the widget focused behind it.
    if (pending_focus_)
    {
        pending_focus_ = false;
        if (auto* h = host())
        {
            h->request_focus(close_btn_);
        }
    }
    auto& cv = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 120});
    cv.fill_rounded_rect(card_rect_, 8.0f, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, 8.0f, pal.border, 1.0f);
    if (title_layout_)
    {
        const float th = title_layout_->measure().h;
        cv.draw_text(*title_layout_,
                     {card_rect_.x + kCardPad, card_rect_.y + kCardPad + (kTitleH - th) * 0.5f},
                     pal.text_primary);
    }
}

// ── input ─────────────────────────────────────────────────────────────────

bool KeyboardShortcutsOverlay::on_pointer_down(tk::Point local)
{
    if (!open_)
    {
        return false;
    }
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (rect_contains(card_rect_, w))
    {
        return true; // the card itself is inert; its children got first pick
    }
    press_backdrop_ = true;
    return true;
}

void KeyboardShortcutsOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
    {
        return;
    }
    press_backdrop_ = false;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (inside_self && !rect_contains(card_rect_, w))
    {
        close();
    }
}

bool KeyboardShortcutsOverlay::on_wheel(tk::Point, float, float, bool)
{
    // The list already had the wheel (dispatch_wheel tries children first);
    // reaching here means it was at an end, or the pointer is over the
    // backdrop. Modal — never let it scroll what's behind.
    return open_;
}

void KeyboardShortcutsOverlay::on_theme_changed(const tk::Theme& t)
{
    tk::Widget::on_theme_changed(t);
    title_layout_.reset(); // font scale may have changed
}

bool KeyboardShortcutsOverlay::on_key_down(const tk::KeyEvent& event)
{
    if (!open_ || event.ctrl || event.alt || event.meta)
    {
        return false;
    }
    if (event.key == tk::Key::Up || event.key == tk::Key::Down)
    {
        constexpr float kLine = 40.0f;
        const tk::Rect b = scroll_->bounds();
        scroll_->on_wheel({b.w * 0.5f, b.h * 0.5f}, 0.0f,
                          event.key == tk::Key::Down ? kLine : -kLine, false);
        return true;
    }
    return scroll_->scroll_by_key(event);
}

} // namespace tesseract::views
