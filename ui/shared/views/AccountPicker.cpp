#include "AccountPicker.h"

#include "tk/host.h"
#include "tk/scroll_view.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{

// Stacks the account rows top to bottom at their natural height. Lives inside
// the picker's ScrollView, which measures it unbounded and scrolls it.
class RowColumn : public tk::Widget
{
protected:
    RowColumn() = default;
    TK_WIDGET_FACTORY_FRIEND(RowColumn)

public:
    tk::Size measure(tk::LayoutCtx& lc, tk::Size constraints) override
    {
        float total_h = 0;
        for (const auto& c : children())
        {
            total_h += c->measure(lc, {constraints.w, 0}).h;
        }
        return {constraints.w, total_h};
    }

    void arrange(tk::LayoutCtx& lc, tk::Rect bounds) override
    {
        bounds_ = bounds;
        float y = bounds.y;
        for (const auto& c : children())
        {
            const float h = c->measure(lc, {bounds.w, 0}).h;
            c->arrange(lc, {bounds.x, y, bounds.w, h});
            y += h;
        }
    }
};

} // namespace

AccountPicker::AccountPicker()
{
    auto scroll = tk::create_widget<tk::ScrollView>(this);
    scroll->on_layout_changed = [this]()
    {
        // The picker is built detached (std::make_unique) by every shell, so
        // its own host() is null; the Surface's RootWidget carries the Host.
        if (auto* root = get_root_widget(); root && root->host())
        {
            root->host()->mark_needs_relayout();
        }
    };
    column_ = scroll->set_child(tk::create_widget<RowColumn>(scroll.get()));
    scroll_ = add_child(std::move(scroll));
}

void AccountPicker::set_entries(std::vector<AccountEntry> entries)
{
    entries_ = std::move(entries);
    rebuild_rows();
}

void AccountPicker::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
    for (auto* row : rows_)
    {
        row->set_image_provider(image_provider_);
    }
}

void AccountPicker::bind_row_(UserInfo& row, const AccountEntry& e)
{
    row.set_display_name(e.display_name);
    row.set_user_id(e.user_id);
    row.set_avatar_url(e.avatar_url);
    row.set_active_indicator(e.active);
    row.set_notification_dot(e.has_unread);

    // Row *position* does not track account *identity* across calls (e.g.
    // logging out of one account and into a different one can leave the
    // total count unchanged), so the per-row callbacks capturing a user_id
    // are re-bound on every update, not just the visible fields.
    const std::string uid = e.user_id;
    row.on_primary = [this, uid](tk::Point)
    {
        if (on_select)
        {
            on_select(uid);
        }
    };
    row.on_avatar_needed = [this](const std::string& mxc)
    {
        if (on_avatar_needed)
        {
            on_avatar_needed(mxc);
        }
    };
}

void AccountPicker::rebuild_rows()
{
    // The shells keep one picker alive and call set_entries() on every open,
    // so the account count can change between calls (login / logout).
    if (rows_.size() == entries_.size())
    {
        for (size_t i = 0; i < rows_.size(); ++i)
        {
            bind_row_(*rows_[i], entries_[i]);
        }
        return;
    }

    column_->clear_children();
    rows_.clear();
    scroll_->scroll_to_top();
    rows_.reserve(entries_.size());
    for (const auto& e : entries_)
    {
        auto row = std::make_unique<UserInfo>();
        if (image_provider_)
        {
            row->set_image_provider(image_provider_);
        }
        bind_row_(*row, e);
        rows_.push_back(column_->add_child(std::move(row)));
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

tk::Size AccountPicker::measure(tk::LayoutCtx& lc, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    float total_h = 0;
    const size_t visible = std::min(rows_.size(), kMaxVisibleRows);
    for (size_t i = 0; i < visible; ++i)
    {
        total_h += rows_[i]->measure(lc, {w, 0}).h;
    }
    return {w, total_h};
}

void AccountPicker::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    bounds_ = bounds;
    scroll_->arrange(lc, bounds);
}

void AccountPicker::paint_before_children(tk::PaintCtx& ctx)
{
    // Rounded floating card — the host window is chrome-free (Qt6 translucent,
    // GTK4 flat popover, Win32 rounded region, macOS NSPopover frame), so this
    // is the popup's visible frame. Mirrors GifPopup / QuickSwitcher. Inset by
    // half a pixel so the whole 1px border sits inside bounds_ (the host clips
    // to bounds_).
    constexpr float kCardRadius = 8.0f;
    const tk::Rect card{bounds_.x + 0.5f, bounds_.y + 0.5f, bounds_.w - 1.0f,
                        bounds_.h - 1.0f};
    ctx.canvas.fill_rounded_rect(card, kCardRadius,
                                 ctx.theme.palette.sidebar_bg);
    ctx.canvas.stroke_rounded_rect(card, kCardRadius,
                                   ctx.theme.palette.popup_border, 1.0f);
}

} // namespace tesseract::views
