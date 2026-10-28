#include "searchable_picker.h"
#include "chevron.h"

#include "access_tree.h"
#include "theme.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <utility>

namespace tk
{

namespace
{
constexpr float kRadius = tesseract::visual::kRadiusSM;
constexpr float kPickerChevronW = 24.0f; // ▾ cell to the right of the field
} // namespace

// ---------------------------------------------------------------------------
// SearchablePicker::DropdownList — the popup surface's root widget. Owns
// only row painting + mouse hit-testing/hover; keyboard nav and commit logic
// stay in SearchablePicker (the field never loses focus while this is open).
// ---------------------------------------------------------------------------

class SearchablePicker::DropdownList : public Widget,
                                      public WidgetRowAccessibility
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
        scroll_y_ = 0.0f;
    }

    // Centres `row` in the viewport on the next arrange() (the popup has no
    // size yet when it first opens).
    void center_on_row_when_sized(int row)
    {
        pending_center_row_ = row;
        if (bounds_.h > 0.0f) // already sized: apply now
            arrange_pending_center_();
    }

    // Scrolls just enough to bring `row` fully into view (keyboard nav).
    void scroll_to_row(int row)
    {
        if (row < 0 || bounds_.h <= 0.0f)
            return;
        const float top = static_cast<float>(row) * row_h_;
        if (top < scroll_y_)
            scroll_y_ = top;
        else if (top + row_h_ > scroll_y_ + bounds_.h)
            scroll_y_ = top + row_h_ - bounds_.h;
        clamp_scroll_();
    }

    void on_fonts_changed() override { row_layouts_.clear(); }

    void set_hovered(int row)
    {
        hovered_ = row;
    }

    std::function<void(std::size_t filtered_index)> on_row_activated;
    std::function<void(int row)> on_hover_changed; // mouse-driven only
    std::function<std::string(std::size_t entry_index)> label_provider;
    std::function<void()> on_scrolled; // wheel or scrollbar drag moved the list

    Size measure(LayoutCtx&, Size constraints) override
    {
        const float w = constraints.w > 0 ? constraints.w : width_;
        return {w, static_cast<float>(entries_.size()) * row_h_};
    }

    void arrange(LayoutCtx&, Rect bounds) override
    {
        bounds_ = bounds;
        arrange_pending_center_();
    }

    // The list can hold far more rows than the popup shows (every installed
    // font, say): it scrolls with the wheel and paints only the visible rows.
    bool on_wheel(Point /*local*/, float /*dx*/, float dy,
                  bool /*is_touchpad*/) override
    {
        if (max_scroll_() <= 0.0f)
            return false;
        scroll_y_ += dy;
        clamp_scroll_();
        if (on_scrolled)
            on_scrolled();
        return true;
    }

    void paint(PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rounded_rect(bounds_, kRadius, pal.chrome_bg);

        if (row_layouts_.size() < entries_.size())
            row_layouts_.resize(entries_.size());

        const int count = static_cast<int>(entries_.size());
        const int first = std::max(0, static_cast<int>(scroll_y_ / row_h_));
        const int last =
            std::min(count, static_cast<int>((scroll_y_ + bounds_.h) / row_h_) + 1);
        {
            ClipScope clip(ctx.canvas, bounds_, kRadius);
            for (int i = first; i < last; ++i)
            {
                const float ry = bounds_.y + row_h_ * static_cast<float>(i) - scroll_y_;
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
            // Thin scrollbar thumb while there is more than fits.
            if (max_scroll_() > 0.0f)
            {
                const Rect t = thumb_rect_();
                ctx.canvas.fill_rounded_rect(
                    {t.x + t.w - kThumbW - 3.0f, t.y, kThumbW, t.h}, kThumbW * 0.5f,
                    dragging_thumb_ ? pal.text_muted : pal.border_strong);
            }
        }
        ctx.canvas.stroke_rounded_rect(bounds_, kRadius, pal.popup_border, 1.0f);
    }

    bool on_pointer_down(Point local) override
    {
        if (over_scrollbar_(local))
        {
            const Rect t = thumb_rect_();
            // Grab the thumb where pressed; a press on the bare track grabs
            // its middle and jumps there.
            thumb_grab_ = (local.y >= t.y && local.y < t.y + t.h)
                              ? local.y - t.y
                              : t.h * 0.5f;
            dragging_thumb_ = true;
            drag_thumb_to_(local.y);
            return true;
        }
        return hit_row_(local) >= 0;
    }

    void on_pointer_drag(Point local) override
    {
        if (dragging_thumb_)
            drag_thumb_to_(local.y);
    }

    void on_pointer_up(Point local, bool /*inside_self*/) override
    {
        if (dragging_thumb_)
        {
            dragging_thumb_ = false;
            if (on_scrolled)
                on_scrolled();
            return;
        }
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

    // ── tk::WidgetRowAccessibility ─────────────────────────────────────
    Role access_role() const override { return Role::List; }
    std::size_t access_row_count() const override { return entries_.size(); }
    Role access_role_for_widget_row(std::size_t index) const override
    {
        return index < entries_.size() ? Role::ListItem : Role::None;
    }
    std::string access_name_for_widget_row(std::size_t index) const override
    {
        return (index < entries_.size() && label_provider)
                   ? label_provider(entries_[index])
                   : std::string{};
    }
    bool access_activate_widget_row(std::size_t index) override
    {
        if (index >= entries_.size() || !on_row_activated)
            return false;
        on_row_activated(index); // filtered index, same as on_pointer_up
        return true;
    }
    Rect access_rect_for_widget_row(std::size_t index) const override
    {
        if (index >= entries_.size())
            return {};
        return {bounds_.x,
                bounds_.y + row_h_ * static_cast<float>(index) - scroll_y_,
                bounds_.w, row_h_};
    }

private:
    static constexpr float kThumbW = 5.0f;
    static constexpr float kScrollZoneW = 14.0f; // grab area, right edge

    Rect thumb_rect_() const
    {
        const float content_h = static_cast<float>(entries_.size()) * row_h_;
        const float thumb_h =
            std::max(24.0f, bounds_.h * bounds_.h / std::max(content_h, 1.0f));
        const float range = max_scroll_();
        const float thumb_y =
            bounds_.y + (range > 0.0f ? (bounds_.h - thumb_h) * (scroll_y_ / range)
                                      : 0.0f);
        return {bounds_.x + bounds_.w - kScrollZoneW, thumb_y, kScrollZoneW,
                thumb_h};
    }

    bool over_scrollbar_(Point local) const
    {
        return max_scroll_() > 0.0f && local.x >= bounds_.x + bounds_.w - kScrollZoneW &&
               local.x < bounds_.x + bounds_.w && local.y >= bounds_.y &&
               local.y < bounds_.y + bounds_.h;
    }

    // Moves the list so the thumb's grab point sits under `y`.
    void drag_thumb_to_(float y)
    {
        const Rect t = thumb_rect_();
        const float travel = bounds_.h - t.h;
        if (travel <= 0.0f)
            return;
        const float frac = (y - thumb_grab_ - bounds_.y) / travel;
        scroll_y_ = frac * max_scroll_();
        clamp_scroll_();
        if (on_scrolled)
            on_scrolled();
    }

    int hit_row_(Point local) const
    {
        if (over_scrollbar_(local))
            return -1;
        if (local.x < bounds_.x || local.x >= bounds_.x + bounds_.w)
            return -1;
        if (local.y < bounds_.y || local.y >= bounds_.y + bounds_.h)
            return -1;
        const int idx =
            static_cast<int>((local.y - bounds_.y + scroll_y_) / row_h_);
        return (idx >= 0 && idx < static_cast<int>(entries_.size())) ? idx : -1;
    }

    void arrange_pending_center_()
    {
        if (pending_center_row_ >= 0 && bounds_.h > 0.0f)
        {
            scroll_y_ = static_cast<float>(pending_center_row_) * row_h_ -
                        (bounds_.h - row_h_) * 0.5f;
            pending_center_row_ = -1;
        }
        clamp_scroll_();
    }

    float max_scroll_() const
    {
        return std::max(
            0.0f, static_cast<float>(entries_.size()) * row_h_ - bounds_.h);
    }
    void clamp_scroll_() { scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_()); }

    float scroll_y_ = 0.0f;
    int   pending_center_row_ = -1;
    bool  dragging_thumb_ = false;
    float thumb_grab_ = 0.0f; // pointer offset within the thumb while dragging

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
            if (on_submit_text && !row_chosen_)
            {
                const std::string text = field_->text();
                bool exact = false;
                const std::size_t n = entry_count_();
                for (std::size_t i = 0; i < n && !exact; ++i)
                    exact = entry_key_(i) == text;
                if (!text.empty() && !exact)
                {
                    set_expanded_(false);
                    set_hovered_(-1);
                    on_submit_text(text);
                    return;
                }
            }
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
    {
        field_->set_enabled(enabled);
        // Dim the text so a disabled picker reads as inert (no theme yet = keep).
        if (text_color_set_)
            field_->set_text_color(enabled ? text_color_ : text_color_muted_);
    }
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
        filtered_.push_back(i); // every match; the popup scrolls
    }
    set_hovered_(filtered_.empty() ? -1 : 0);
    row_chosen_ = false;
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
        row_chosen_ = true;
        set_hovered_(hovered_row_ <= 0 ? static_cast<int>(filtered_.size()) - 1
                                       : hovered_row_ - 1);
        if (dropdown_)
            dropdown_->scroll_to_row(hovered_row_);
        return true;
    case NavKey::Down:
        row_chosen_ = true;
        set_hovered_((hovered_row_ + 1) % static_cast<int>(filtered_.size()));
        if (dropdown_)
            dropdown_->scroll_to_row(hovered_row_);
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
            // Matches kRadius, which DropdownList::paint() draws its own
            // rounded background with — see ComboBox's identical
            // set_corner_radius() call for why this is needed.
            popup_->set_corner_radius(kRadius);
            auto list = std::make_unique<DropdownList>(row_h_, width_);
            dropdown_ = list.get();
            dropdown_->on_row_activated = [this](std::size_t idx) { commit_row_(idx); };
            dropdown_->on_hover_changed = [this](int row)
            {
                row_chosen_ = row >= 0;
                set_hovered_(row);
            };
            dropdown_->on_scrolled = [this]
            {
                if (popup_)
                    popup_->request_repaint();
            };
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
    const float h = static_cast<float>(std::min<std::size_t>(
                        filtered_.size(), static_cast<std::size_t>(max_rows_))) *
                    row_h_;
    // Anchored to the whole control (field + chevron cell).
    const float w = std::max(width_, bounds_.w);
    popup_->set_rect(bounds_, {w, h});
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
    const float chev_w = std::min(kPickerChevronW, bounds.w);
    field_rect_   = {bounds.x, bounds.y, bounds.w - chev_w, bounds.h};
    chevron_rect_ = {bounds.x + bounds.w - chev_w, bounds.y, chev_w, bounds.h};
    if (field_)
        field_->arrange(ctx, field_rect_);
    reposition_popup_();
}

namespace
{
bool contains(const Rect& r, Point p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}
} // namespace

void SearchablePicker::paint_before_children(PaintCtx& ctx)
{
    if (chevron_rect_.w <= 0.0f)
        return;
    const auto& pal = ctx.theme.palette;
    // One border around the whole control (field + chevron), the same colour
    // as tk::ComboBox's, so it reads as a single dropdown field.
    if (enabled_ && (chevron_pressed_ || chevron_hovered_))
        ctx.canvas.fill_rect({chevron_rect_.x, chevron_rect_.y + 1.0f,
                              chevron_rect_.w - 1.0f, chevron_rect_.h - 2.0f},
                             chevron_pressed_ ? pal.subtle_pressed
                                              : pal.subtle_hover);
    ctx.canvas.stroke_rounded_rect(bounds_, kRadius,
                                   enabled_ ? pal.border : pal.border.with_alpha(pal.border.a / 2));
    draw_chevron_down(ctx.canvas, chevron_rect_,
                      enabled_ ? pal.text_secondary : pal.text_muted);
}

bool SearchablePicker::on_pointer_down(Point local)
{
    if (!enabled_)
        return false;
    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    if (!contains(chevron_rect_, w))
        return false;
    chevron_pressed_ = true;
    return true;
}

void SearchablePicker::on_pointer_up(Point local, bool inside_self)
{
    if (!chevron_pressed_)
        return;
    chevron_pressed_ = false;
    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    if (!inside_self || !contains(chevron_rect_, w) || !field_)
        return;
    if (expanded_)
    {
        collapse_();
        return;
    }
    // Focus the field first so a later blur dismisses the list, then show
    // every entry rather than only those matching the committed text.
    field_->set_focused(true);
    refilter_("");
    set_expanded_(!filtered_.empty());
    // Start on the committed value, centred in the viewport.
    for (std::size_t i = 0; i < filtered_.size(); ++i)
    {
        if (entry_key_(filtered_[i]) == value_)
        {
            set_hovered_(static_cast<int>(i));
            if (dropdown_)
                dropdown_->center_on_row_when_sized(static_cast<int>(i));
            reposition_popup_();
            break;
        }
    }
}

bool SearchablePicker::on_pointer_move(Point local)
{
    if (!enabled_)
        return false;
    const Point w{local.x + bounds_.x, local.y + bounds_.y};
    const bool on = contains(chevron_rect_, w);
    if (on != chevron_hovered_)
    {
        chevron_hovered_ = on;
        if (auto* h = host())
            h->request_repaint_rect(chevron_rect_);
    }
    return on;
}

void SearchablePicker::on_pointer_leave()
{
    chevron_hovered_ = false;
    chevron_pressed_ = false;
}

void SearchablePicker::on_theme_changed(const Theme& t)
{
    text_color_set_   = true;
    text_color_       = t.palette.text_primary;
    text_color_muted_ = t.palette.text_muted;
    if (field_)
        field_->set_text_color(enabled_ ? text_color_ : text_color_muted_);
    if (popup_)
        popup_->set_theme(t);
}

} // namespace tk
