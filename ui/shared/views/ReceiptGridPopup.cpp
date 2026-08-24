#include "ReceiptGridPopup.h"

#include "format.h"
#include "media_utils.h"
#include "tk/theme.h"

#include <tesseract/visual.h>

#include <algorithm>

namespace tesseract::views
{

// ─────────────────────────────────────────────────────────────────────────
//  Grid adapter — paints one avatar cell per hidden receipt.
// ─────────────────────────────────────────────────────────────────────────

class ReceiptGridPopup::GridAdapter : public tk::GridAdapter
{
public:
    explicit GridAdapter(ReceiptGridPopup& owner) : owner_(owner)
    {
    }

    std::size_t count() const override
    {
        return owner_.entries_.size();
    }

    void paint_cell(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool /*selected*/, bool /*hovered*/) override
    {
        const auto& rr = owner_.entries_[index];
        const tk::Image* img = (owner_.provider_ && !rr.avatar_url.empty())
                                    ? owner_.provider_(rr.avatar_url)
                                    : nullptr;
        tk::Point centre{bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f};
        draw_avatar(ctx.canvas, img, centre, std::min(bounds.w, bounds.h),
                    rr.display_name.empty() ? rr.user_id : rr.display_name,
                    ctx.theme.palette.avatar_initials_bg,
                    ctx.theme.palette.avatar_initials_text);
    }

private:
    ReceiptGridPopup& owner_;
};

// ─────────────────────────────────────────────────────────────────────────
//  ReceiptGridPopup
// ─────────────────────────────────────────────────────────────────────────

ReceiptGridPopup::ReceiptGridPopup()
    : grid_adapter_(std::make_unique<GridAdapter>(*this))
{
    auto grid = tk::create_widget<tk::GridView>(this);
    grid->set_adapter(grid_adapter_.get());
    grid_ = add_child(std::move(grid));
    set_visible(false);
}

void ReceiptGridPopup::set_image_provider(ImageProvider p)
{
    provider_ = std::move(p);
    invalidate_image_cache();
}

void ReceiptGridPopup::invalidate_image_cache()
{
    if (grid_)
    {
        grid_->invalidate_data();
    }
}

void ReceiptGridPopup::set_entries(std::vector<tesseract::ReadReceipt> entries)
{
    entries_ = std::move(entries);

    const int rows_needed =
        static_cast<int>((entries_.size() + kCols - 1) / kCols);
    const int visible_rows = std::max(1, std::min(rows_needed, kMaxVisibleRows));
    natural_size_ = {
        kCols * kCellSize + (kCols - 1) * kCellGap + 2 * kPadding,
        visible_rows * kCellSize + (visible_rows - 1) * kCellGap + 2 * kPadding,
    };

    if (grid_)
    {
        grid_->invalidate_data();
        grid_->scroll_to_top();
    }
}

void ReceiptGridPopup::open_at(tk::Rect world_rect)
{
    bounds_ = world_rect;
    needs_arrange_ = true;
}

// ─────────────────────────────────────────────────────────────────────────
//  Layout
// ─────────────────────────────────────────────────────────────────────────

tk::Size ReceiptGridPopup::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void ReceiptGridPopup::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (grid_)
    {
        grid_->set_cell_size(kCellSize, kCellSize);
        grid_->set_spacing(kCellGap, kCellGap);
        grid_->set_padding(tk::Edges::all(kPadding));
        grid_->arrange(ctx, bounds_);
    }
}

void ReceiptGridPopup::paint(tk::PaintCtx& ctx)
{
    constexpr float kCardRadius = tesseract::visual::kRadiusMD;
    ctx.canvas.fill_rounded_rect(bounds_, kCardRadius, ctx.theme.palette.chrome_bg);

    if (grid_)
    {
        grid_->paint(ctx);
    }

    // Per-cell tooltip: name + hh:mm, same content the inline disc tooltip
    // shows. Re-evaluated every frame — this widget doesn't get a
    // hover-transition event of its own; GridView tracks hovered_index_
    // itself. from_popup=true because this popup is itself the currently
    // registered popup (see Host::show_tooltip's doc comment), so its own
    // tooltip must bypass the "no tooltip while a popup is open" gate.
    if (host())
    {
        std::string tip;
        if (grid_ && grid_->hovered_index() >= 0 &&
            static_cast<std::size_t>(grid_->hovered_index()) < entries_.size())
        {
            const auto& rr =
                entries_[static_cast<std::size_t>(grid_->hovered_index())];
            tip = rr.display_name.empty() ? rr.user_id : rr.display_name;
            const std::string ts = format_hhmm(rr.timestamp_ms);
            if (!ts.empty())
            {
                tip += "  " + ts;
            }
        }
        if (!tip.empty())
        {
            host()->show_tooltip(grid_, tip,
                                 grid_->rect_at(grid_->hovered_index()),
                                 /*from_popup=*/true);
        }
        else
        {
            host()->hide_tooltip(grid_);
        }
    }

    ctx.canvas.stroke_rounded_rect(bounds_, kCardRadius,
                                   ctx.theme.palette.popup_border, 1.0f);
}

void ReceiptGridPopup::paint_overlay(tk::PaintCtx& ctx)
{
    if (bounds_.w <= 0.0f || bounds_.h <= 0.0f)
    {
        return;
    }
    if (needs_arrange_)
    {
        tk::LayoutCtx lctx{ctx.factory, ctx.theme};
        arrange(lctx, bounds_);
        needs_arrange_ = false;
    }
    paint(ctx);
}

// ─────────────────────────────────────────────────────────────────────────
//  Input
// ─────────────────────────────────────────────────────────────────────────

bool ReceiptGridPopup::on_key_down(const tk::KeyEvent& event)
{
    if (event.key == tk::Key::Escape)
    {
        on_popup_dismiss();
        return true;
    }
    return false;
}

void ReceiptGridPopup::on_popup_dismiss()
{
    if (on_dismiss)
    {
        on_dismiss();
    }
}

} // namespace tesseract::views
