#include "CreatePollDialog.h"

#include "media_utils.h" // rect_contains

#include "tk/theme.h"

#include <algorithm>
#include <utility>

namespace tesseract::views
{

namespace
{
constexpr float kRowGap   = 8.0f;
constexpr float kRemoveW  = 32.0f;
constexpr float kAddBtnH  = 32.0f;
} // namespace

// Stacks the option rows then the "Add option" button at natural height; the
// ScrollView measures it unbounded and scrolls it. Rows stop short of the
// right edge by the scrollbar gutter so the thumb never covers a field.
class CreatePollDialog::OptionsColumn : public tk::Widget
{
protected:
    OptionsColumn() = default;
    TK_WIDGET_FACTORY_FRIEND(OptionsColumn)

public:
    std::vector<Row>* rows = nullptr;
    tk::Button* add_btn = nullptr;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        const float n = rows ? static_cast<float>(rows->size()) : 0.0f;
        float h = n * (kFieldH + kRowGap);
        if (add_btn && add_btn->own_visible())
            h += kAddBtnH;
        else
            h = std::max(0.0f, h - kRowGap);
        return {constraints.w, h};
    }

    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override
    {
        bounds_ = bounds;
        const float w = std::max(0.0f, bounds.w - tk::ScrollableBase::kScrollbarGutter);
        float y = bounds.y;
        if (rows)
        {
            for (std::size_t i = 0; i < rows->size(); ++i)
            {
                auto& r = (*rows)[i];
                const bool has_remove = i >= kPollMinOptions;
                float fw = w;
                if (r.remove)
                {
                    r.remove->set_visible(has_remove);
                    if (has_remove)
                    {
                        fw = std::max(0.0f, w - kRemoveW - kRowGap);
                        r.remove->arrange(lc, {bounds.x + w - kRemoveW, y, kRemoveW, kFieldH});
                    }
                }
                if (r.field)
                    r.field->arrange(lc, {bounds.x, y, fw, kFieldH});
                y += kFieldH + kRowGap;
            }
        }
        if (add_btn && add_btn->own_visible())
        {
            const tk::Size s = add_btn->measure(lc, {-1.0f, kAddBtnH});
            add_btn->arrange(lc, {bounds.x, y, std::min(w, std::max(s.w, 120.0f)), kAddBtnH});
        }
    }
};

CreatePollDialog::CreatePollDialog()
{
    if (host())
    {
        auto q = tk::create_widget<tk::TextField>(this, kFieldH);
        q->set_placeholder(tk::tr("Question"));
        q->set_accessible_name(tk::tr("Question"));
        q->set_on_changed([this](const std::string&) { refresh_state_(); });
        q->set_on_submit([this]() {
            if (!rows_.empty() && rows_[0].field)
                rows_[0].field->set_focused(true);
        });
        q->push_popup_nav([this](tk::NavKey k) {
            if (k != tk::NavKey::Escape)
                return false;
            close();
            return true;
        });
        question_ = add_child(std::move(q));
    }

    auto scroll = tk::create_widget<tk::ScrollView>(this);
    scroll->on_layout_changed = [this]() {
        if (auto* h = host())
            h->mark_needs_relayout();
    };
    scroll_ = add_child(std::move(scroll));

    auto column = tk::create_widget<OptionsColumn>(scroll_);
    column->rows = &rows_;
    column_ = column.get();
    scroll_->set_child(std::move(column));

    add_btn_ = column_->add_child(tk::create_widget<tk::Button>(
        column_, tk::tr("Add option"), [this]() { add_option(); },
        tk::Button::Variant::Subtle));
    column_->add_btn = add_btn_;

    multiple_ = add_child(
        tk::create_widget<tk::CheckButton>(this, tk::tr("Allow multiple answers")));
    hide_results_ = add_child(tk::create_widget<tk::CheckButton>(
        this, tk::tr("Hide results until the poll ends")));
    hint_ = add_child(tk::create_widget<tk::Label>(
        this, tk::tr("Enter a question and at least two different options"),
        tk::FontRole::Caption));

    cancel_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Cancel"), [this]() { close(); }, tk::Button::Variant::Subtle));
    create_btn_ = add_child(tk::create_widget<tk::Button>(
        this, tk::tr("Create"), [this]() { submit_(); }, tk::Button::Variant::Primary));

    add_row_();
    add_row_();
    refresh_state_();

    set_visible(false);
}

// ── state ─────────────────────────────────────────────────────────────────

PollDraft CreatePollDialog::current_draft() const
{
    PollDraft d;
    if (question_)
        d.question = question_->text();
    for (const auto& r : rows_)
        d.options.push_back(r.field ? r.field->text() : std::string());
    d.multiple     = multiple_ && multiple_->checked();
    d.hide_results = hide_results_ && hide_results_->checked();
    return d;
}

bool CreatePollDialog::create_enabled() const
{
    return poll_draft_valid(normalize_poll_draft(current_draft()));
}

void CreatePollDialog::refresh_state_()
{
    const bool valid = create_enabled();
    if (create_btn_ && create_btn_->enabled() != valid)
        create_btn_->set_enabled(valid);
    if (hint_)
        hint_->set_visible(open_ && !valid);
    if (add_btn_)
        add_btn_->set_visible(rows_.size() < kPollMaxOptions);
}

void CreatePollDialog::relabel_rows_()
{
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        if (!rows_[i].field)
            continue;
        const auto label = tk::trf(tk::tr("Option {0}"), {std::to_string(i + 1)});
        rows_[i].field->set_placeholder(label);
        rows_[i].field->set_accessible_name(label);
    }
}

void CreatePollDialog::add_row_()
{
    Row row;
    if (host())
    {
        auto f = tk::create_widget<tk::TextField>(column_, kFieldH);
        tk::TextField* raw = f.get();
        f->set_on_changed([this](const std::string&) { refresh_state_(); });
        f->set_on_submit([this, raw]() { on_option_submit_(raw); });
        f->push_popup_nav([this](tk::NavKey k) {
            if (k != tk::NavKey::Escape)
                return false;
            close();
            return true;
        });
        row.field = column_->add_child(std::move(f));
    }
    tk::TextField* field = row.field;
    auto rm = tk::create_widget<tk::Button>(
        column_, "\xC3\x97", std::function<void()>{}, tk::Button::Variant::Subtle);
    rm->set_accessible_name(tk::tr("Remove option"));
    rm->set_on_click([this, field]() {
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].field == field)
            {
                remove_option(i);
                return;
            }
    });
    row.remove = column_->add_child(std::move(rm));
    rows_.push_back(row);
}

void CreatePollDialog::add_option()
{
    if (rows_.size() >= kPollMaxOptions)
        return;
    add_row_();
    relabel_rows_();
    refresh_state_();
    if (auto* h = host())
        h->mark_needs_relayout();
}

void CreatePollDialog::remove_option(std::size_t index)
{
    if (index >= rows_.size() || rows_.size() <= kPollMinOptions)
        return;
    Row r = rows_[index];
    const bool had_focus = r.field && r.field->has_focus();
    rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
    if (r.field)
    {
        r.field->set_focused(false);
        column_->remove_child(r.field);
    }
    if (had_focus && !rows_.empty())
    {
        const std::size_t n = std::min(index, rows_.size() - 1);
        if (rows_[n].field)
            rows_[n].field->set_focused(true);
    }
    if (r.remove)
        column_->remove_child(r.remove);
    relabel_rows_();
    refresh_state_();
    if (auto* h = host())
        h->mark_needs_relayout();
}

void CreatePollDialog::on_option_submit_(tk::TextField* from)
{
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        if (rows_[i].field != from)
            continue;
        if (i + 1 == rows_.size())
        {
            if (rows_.size() >= kPollMaxOptions)
                return;
            add_option();
            reveal_row_ = rows_.size() - 1; // revealed at the next arrange()
            if (auto* f = rows_.back().field)
                f->set_focused(true);
        }
        else if (rows_[i + 1].field)
        {
            rows_[i + 1].field->set_focused(true);
        }
        return;
    }
}

void CreatePollDialog::submit_()
{
    if (!open_ || !create_enabled())
        return;
    auto d  = normalize_poll_draft(current_draft());
    auto cb = std::move(on_create_);
    close();
    if (cb)
        cb(std::move(d));
}

// ── open / close ──────────────────────────────────────────────────────────

void CreatePollDialog::open(std::function<void(PollDraft)> on_create,
                            std::string room_name)
{
    room_name_ = std::move(room_name);
    const bool was_open = open_;

    // Reset to a pristine form.
    while (rows_.size() > kPollMinOptions)
        remove_option(rows_.size() - 1);
    if (question_)
        question_->set_text("");
    for (auto& r : rows_)
        if (r.field)
            r.field->set_text("");
    multiple_->set_checked(false);
    hide_results_->set_checked(false);
    scroll_->scroll_to_top();
    relabel_rows_();

    on_create_ = std::move(on_create);
    open_      = true;
    set_visible(true);
    press_backdrop_ = false;
    title_layout_.reset();
    refresh_state_();

    if (question_)
        question_->set_focused(true);
    if (!was_open && on_layout_changed)
        on_layout_changed();
}

void CreatePollDialog::close()
{
    const bool was_open = open_;
    open_ = false;
    set_visible(false);
    on_create_ = nullptr;
    press_backdrop_ = false;
    if (question_)
        question_->set_focused(false);
    for (auto& r : rows_)
        if (r.field)
            r.field->set_focused(false);
    // Create/checkboxes may hold canvas focus; don't leave it on hidden widgets.
    if (auto* h = host())
        for (tk::Widget* w : {static_cast<tk::Widget*>(create_btn_),
                              static_cast<tk::Widget*>(cancel_btn_),
                              static_cast<tk::Widget*>(multiple_),
                              static_cast<tk::Widget*>(hide_results_)})
            if (w && w->has_focus())
                h->clear_focus();
    if (was_open && on_layout_changed)
        on_layout_changed();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size CreatePollDialog::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void CreatePollDialog::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_        = bounds;
    backdrop_rect_ = bounds;

    refresh_state_();

    const float card_w = std::max(0.0f, std::min(kCardW, bounds.w - kMargin * 2));
    const float inner_w = std::max(0.0f, card_w - kCardPad * 2);

    if (!title_layout_ || title_w_ != inner_w)
    {
        title_w_ = inner_w;
        tk::TextStyle st{};
        st.role      = tk::FontRole::Title;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = inner_w;
        title_layout_ = lc.factory.build_text(title_text_(), st);
    }
    const float title_h =
        title_layout_ ? std::max(title_layout_->measure().h, kTitleH) : kTitleH;

    const bool hint_shown = hint_ && hint_->own_visible();
    const float fixed_h = kCardPad * 2 + title_h + kGap + kFieldH + kGap /*scroll gap*/ +
                          kGap + kCheckH * 2 +
                          (hint_shown ? kGap + kHintH : 0.0f) + kGap + kBtnH;
    const float natural_list_h =
        column_->measure(lc, {inner_w, 1'000'000.0f}).h;
    const float max_card_h = std::max(0.0f, bounds.h - kMargin * 2);
    const float list_h =
        std::max(0.0f, std::min(natural_list_h, max_card_h - fixed_h));
    const float card_h = std::min(fixed_h + list_h, bounds.h);
    card_rect_ = {bounds.x + (bounds.w - card_w) * 0.5f,
                  bounds.y + (bounds.h - card_h) * 0.5f, card_w, card_h};

    const float x = card_rect_.x + kCardPad;
    float y       = card_rect_.y + kCardPad + title_h + kGap;

    if (question_)
        question_->arrange(lc, {x, y, inner_w, kFieldH});
    y += kFieldH + kGap;

    // The list reaches into the card's right padding by the scrollbar gutter
    // (which its rows keep clear) so the thumb sits in the padding.
    scroll_->arrange(lc, {x, y, inner_w + tk::ScrollableBase::kScrollbarGutter, list_h});
    // A just-added row is revealed now that it has real bounds. Re-arranging
    // the scroll view after moving it is the one extra pass; the column is
    // re-laid at the new offset (never Widget::arrange on arranged children).
    if (reveal_row_ < rows_.size())
    {
        if (auto* f = rows_[reveal_row_].field)
        {
            const float before = scroll_->scroll_y();
            scroll_->scroll_into_view(f->bounds());
            if (scroll_->scroll_y() != before)
                scroll_->arrange(lc, scroll_->bounds());
        }
    }
    reveal_row_ = static_cast<std::size_t>(-1);
    y += list_h + kGap;

    multiple_->arrange(lc, {x, y, inner_w, kCheckH});
    y += kCheckH;
    hide_results_->arrange(lc, {x, y, inner_w, kCheckH});
    y += kCheckH;
    if (hint_shown)
    {
        y += kGap;
        hint_->arrange(lc, {x, y, inner_w, kHintH});
    }

    const float btns_y = card_rect_.y + card_rect_.h - kCardPad - kBtnH;
    const float btn_w_min = 88.0f;
    const tk::Size cs = create_btn_->measure(lc, {-1.0f, kBtnH});
    const tk::Size xs = cancel_btn_->measure(lc, {-1.0f, kBtnH});
    const float create_w = std::max(cs.w, btn_w_min);
    const float cancel_w = std::max(xs.w, btn_w_min);
    const float create_x = card_rect_.x + card_rect_.w - kCardPad - create_w;
    const float cancel_x = create_x - kBtnGap - cancel_w;
    cancel_btn_->arrange(lc, {cancel_x, btns_y, cancel_w, kBtnH});
    create_btn_->arrange(lc, {create_x, btns_y, create_w, kBtnH});
}

void CreatePollDialog::on_theme_changed(const tk::Theme& t)
{
    tk::Widget::on_theme_changed(t);
    title_layout_.reset();
    if (question_)
        question_->set_text_color(t.palette.text_primary);
    for (auto& r : rows_)
        if (r.field)
            r.field->set_text_color(t.palette.text_primary);
}

// ── paint ─────────────────────────────────────────────────────────────────

void CreatePollDialog::paint_before_children(tk::PaintCtx& ctx)
{
    if (!open_)
        return;
    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;

    cv.fill_rect(backdrop_rect_, tk::Color{0, 0, 0, 120});
    cv.fill_rounded_rect(card_rect_, 8.0f, pal.chrome_bg);
    cv.stroke_rounded_rect(card_rect_, 8.0f, pal.border, 1.0f);

    if (title_layout_)
        cv.draw_text(*title_layout_, {card_rect_.x + kCardPad, card_rect_.y + kCardPad},
                     pal.text_primary);
}

// ── pointer events ────────────────────────────────────────────────────────

bool CreatePollDialog::on_pointer_down(tk::Point local)
{
    if (!open_)
        return false;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (rect_contains(card_rect_, w))
        return true; // the card itself is inert; its children got first pick
    press_backdrop_ = true;
    return true;
}

void CreatePollDialog::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!press_backdrop_)
        return;
    press_backdrop_ = false;
    if (!inside_self)
        return;
    const tk::Point w{local.x + bounds().x, local.y + bounds().y};
    if (!rect_contains(card_rect_, w))
        close();
}

} // namespace tesseract::views
