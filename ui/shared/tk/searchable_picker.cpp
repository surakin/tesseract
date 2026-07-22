#include "searchable_picker.h"

#include "theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <utility>

namespace tk
{

namespace
{
constexpr float kRadius = tesseract::visual::kRadiusSM;
} // namespace

// ---------------------------------------------------------------------------
// SearchablePicker::DropdownList — the popup surface's root widget. Owns
// only row painting + mouse hit-testing/hover; keyboard nav and commit logic
// stay in SearchablePicker (the field never loses focus while this is open).
// ---------------------------------------------------------------------------

class SearchablePicker::DropdownList : public Widget
{
public:
    // Public, plain-constructible (like MentionPopup/ShortcodePopup/etc.) —
    // this is always a popup surface's *root* widget, mounted via
    // PopupSurfaceHandle::set_root() (which internally establishes its host
    // association), never a regular add_child() tree member.
    DropdownList(float row_h, float width) : row_h_(row_h), width_(width) {}

    // Data mutators only — repaint/relayout is the owning SearchablePicker's
    // job via the PopupSurfaceHandle directly (this widget's own host() is
    // always null: it's mounted detached via PopupSurfaceHandle::set_root(),
    // constructed before any host association exists to capture — see
    // PopupSurfaceHandle::request_repaint()'s doc comment in host.h).
    void set_entries(std::vector<std::size_t> entries)
    {
        entries_ = std::move(entries);
        row_layouts_.clear();
    }

    void set_hovered(int row)
    {
        hovered_ = row;
    }

    std::function<void(std::size_t filtered_index)> on_row_activated;
    std::function<void(int row)> on_hover_changed; // mouse-driven only
    std::function<std::string(std::size_t entry_index)> label_provider;

    Size measure(LayoutCtx&, Size constraints) override
    {
        const float w = constraints.w > 0 ? constraints.w : width_;
        return {w, static_cast<float>(entries_.size()) * row_h_};
    }

    void arrange(LayoutCtx&, Rect bounds) override
    {
        bounds_ = bounds;
    }

    void paint(PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rounded_rect(bounds_, kRadius, pal.chrome_bg);

        if (row_layouts_.size() < entries_.size())
            row_layouts_.resize(entries_.size());

        for (int i = 0; i < static_cast<int>(entries_.size()); ++i)
        {
            const float ry = bounds_.y + row_h_ * static_cast<float>(i);
            const Rect row{bounds_.x, ry, bounds_.w, row_h_};

            if (i == hovered_)
                ctx.canvas.fill_rect(row, pal.subtle_hover);

            auto& layout = row_layouts_[static_cast<std::size_t>(i)];
            if (!layout && label_provider)
            {
                TextStyle st{};
                st.role      = FontRole::Body;
                st.halign    = TextHAlign::Leading;
                st.trim      = TextTrim::Ellipsis;
                st.max_width = row.w - 16.0f;
                layout = ctx.factory.build_text(
                    label_provider(entries_[static_cast<std::size_t>(i)]), st);
            }
            if (layout)
            {
                const Size sz = layout->measure();
                const float ty = ry + (row_h_ - sz.h) * 0.5f;
                ctx.canvas.draw_text(*layout, {row.x + 8.0f, ty}, pal.text_primary);
            }
        }
    }

    bool on_pointer_down(Point local) override
    {
        return hit_row_(local) >= 0;
    }

    void on_pointer_up(Point local, bool /*inside_self*/) override
    {
        const int idx = hit_row_(local);
        if (idx >= 0 && on_row_activated)
            on_row_activated(static_cast<std::size_t>(idx));
    }

    bool on_pointer_move(Point local) override
    {
        const int idx = hit_row_(local);
        if (idx == hovered_)
            return false;
        hovered_ = idx;
        if (on_hover_changed)
            on_hover_changed(idx);
        return true;
    }

    void on_pointer_leave() override
    {
        if (hovered_ != -1 && on_hover_changed)
            on_hover_changed(-1);
        hovered_ = -1;
    }

private:
    int hit_row_(Point local) const
    {
        if (local.x < bounds_.x || local.x >= bounds_.x + bounds_.w)
            return -1;
        const int idx = static_cast<int>((local.y - bounds_.y) / row_h_);
        return (idx >= 0 && idx < static_cast<int>(entries_.size())) ? idx : -1;
    }

    std::vector<std::size_t> entries_;
    mutable std::vector<std::unique_ptr<TextLayout>> row_layouts_;
    int hovered_ = -1;

    float row_h_;
    float width_;
};

// ---------------------------------------------------------------------------
// SearchablePicker
// ---------------------------------------------------------------------------

void SearchablePicker::init_(float field_h, float row_h, float width,
                             int max_rows, std::string placeholder)
{
    field_h_  = field_h;
    row_h_    = row_h;
    width_    = width;
    max_rows_ = max_rows;

    auto field = create_widget<TextField>(this, field_h_);
    field->set_placeholder(std::move(placeholder));
    field_ = add_child(std::move(field));

    field_->set_on_changed(
        [this](const std::string& text)
        {
            refilter_(text);
            set_expanded_(!filtered_.empty());
        });

    field_->set_on_submit(
        [this]
        {
            if (hovered_row_ >= 0 &&
                hovered_row_ < static_cast<int>(filtered_.size()))
            {
                commit_row_(static_cast<std::size_t>(hovered_row_));
            }
            else if (!filtered_.empty())
            {
                commit_row_(0);
            }
            else
            {
                collapse_();
            }
        });

    field_->push_popup_nav(
        [this](NavKey key)
        {
            return handle_nav_(key);
        });

    // Dismiss on blur — the dropdown's own rows never take keyboard focus
    // (DropdownList doesn't override focusable(), and both backends'
    // popup-surface implementations are built non-focusable/NoFocus), so
    // this only fires for a genuine "user clicked/tabbed away" transition,
    // never as a side effect of clicking a row.
    field_->set_on_focus_changed(
        [this](bool focused)
        {
            if (!focused)
                collapse_();
            if (on_focus_changed_cb_)
                on_focus_changed_cb_(focused);
        });
}

void SearchablePicker::set_on_focus_changed(std::function<void(bool)> cb)
{
    on_focus_changed_cb_ = std::move(cb);
}

void SearchablePicker::set_value(std::string value)
{
    value_ = std::move(value);
    if (field_)
        field_->set_text(display_for_(value_));
}

std::string SearchablePicker::display_for_(const std::string& key) const
{
    const std::size_t n = entry_count_();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (entry_key_(i) == key)
            return entry_display_(i);
    }
    return key;
}

void SearchablePicker::set_enabled(bool enabled)
{
    Widget::set_enabled(enabled);
    if (field_)
        field_->set_enabled(enabled);
    if (!enabled)
        collapse_();
}

void SearchablePicker::set_visible(bool v)
{
    Widget::set_visible(v);
    if (field_)
        field_->set_visible(v);
    if (!v)
        collapse_();
}

void SearchablePicker::set_compact(bool compact)
{
    if (field_)
        field_->set_compact(compact);
}

void SearchablePicker::refilter_(const std::string& query)
{
    filtered_.clear();
    std::vector<std::pair<int, std::size_t>> ranked;
    const std::size_t n = entry_count_();
    for (std::size_t i = 0; i < n; ++i)
    {
        const int r = match_rank_(i, query);
        if (r >= 0)
            ranked.emplace_back(r, i);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [r, i] : ranked)
    {
        if (static_cast<int>(filtered_.size()) >= max_rows_)
            break;
        filtered_.push_back(i);
    }
    set_hovered_(filtered_.empty() ? -1 : 0);
    if (dropdown_)
    {
        dropdown_->set_entries(filtered_);
        // Shrink/grow-to-fit on every keystroke as the match count changes,
        // mirroring the other search popups' behavior.
        reposition_popup_();
    }
}

void SearchablePicker::collapse_()
{
    set_expanded_(false);
    set_hovered_(-1);
    if (field_)
        field_->set_text(display_for_(value_)); // revert any uncommitted typing
}

void SearchablePicker::commit_row_(std::size_t filtered_index)
{
    if (filtered_index >= filtered_.size())
        return;
    value_ = entry_key_(filtered_[filtered_index]);
    collapse_(); // also pushes the committed value's display text into the field
    if (on_changed)
        on_changed(value_);
}

void SearchablePicker::set_hovered_(int row)
{
    hovered_row_ = row;
    if (dropdown_)
    {
        dropdown_->set_hovered(row);
        // Needed for keyboard-driven changes (handle_nav_'s Up/Down): unlike
        // a mouse hover, no pointer event is dispatched to the popup's own
        // surface to trigger its usual automatic repaint.
        if (popup_)
            popup_->request_repaint();
    }
}

bool SearchablePicker::handle_nav_(NavKey key)
{
    if (!expanded_ || filtered_.empty())
        return false;
    switch (key)
    {
    case NavKey::Up:
        set_hovered_(hovered_row_ <= 0 ? static_cast<int>(filtered_.size()) - 1
                                       : hovered_row_ - 1);
        return true;
    case NavKey::Down:
        set_hovered_((hovered_row_ + 1) % static_cast<int>(filtered_.size()));
        return true;
    case NavKey::Escape:
        collapse_();
        return true;
    default:
        return false;
    }
}

void SearchablePicker::set_expanded_(bool expanded)
{
    if (expanded_ == expanded)
        return;
    expanded_ = expanded;

    auto* h = host();
    if (!h)
        return;

    if (expanded)
    {
        if (!popup_)
        {
            popup_ = h->make_popup_surface();
            if (!popup_)
                return;
            auto list = std::make_unique<DropdownList>(row_h_, width_);
            dropdown_ = list.get();
            dropdown_->on_row_activated = [this](std::size_t idx) { commit_row_(idx); };
            dropdown_->on_hover_changed = [this](int row) { set_hovered_(row); };
            dropdown_->label_provider = [this](std::size_t idx) { return entry_label_(idx); };
            popup_->set_root(std::move(list));
        }
        dropdown_->set_entries(filtered_);
        dropdown_->set_hovered(hovered_row_);
        reposition_popup_();
        popup_->set_visible(true);
    }
    else if (popup_)
    {
        popup_->set_visible(false);
    }
}

void SearchablePicker::reposition_popup_()
{
    if (!popup_ || !expanded_)
        return;
    const float h = static_cast<float>(filtered_.size()) * row_h_;
    const float w = std::max(width_, field_rect_.w);
    popup_->set_rect(field_rect_, {w, h});
}

// ---- layout ----------------------------------------------------------------

Size SearchablePicker::measure(LayoutCtx&, Size constraints)
{
    const float w = constraints.w > 0 ? std::min(constraints.w, width_) : width_;
    return {w, field_h_};
}

void SearchablePicker::arrange(LayoutCtx& ctx, Rect bounds)
{
    bounds_     = bounds;
    field_rect_ = bounds;
    if (field_)
        field_->arrange(ctx, field_rect_);
    reposition_popup_();
}

void SearchablePicker::on_theme_changed(const Theme& t)
{
    if (field_)
        field_->set_text_color(t.palette.text_primary);
    if (popup_)
        popup_->set_theme(t);
}

} // namespace tk
