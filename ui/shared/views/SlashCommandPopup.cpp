#include "views/SlashCommandPopup.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include <algorithm>

namespace tesseract::views
{

void SlashCommandPopup::set_suggestions(std::vector<SlashCommandSuggestion> s)
{
    suggestions_ = std::move(s);
    // Preselect the first match so Tab/Enter accepts the top result without
    // the user having to press Down first. Stays -1 when the list is empty.
    selected_index_ = suggestions_.empty() ? -1 : 0;
    hovered_index_  = -1;
    pressed_index_  = -1;
}

void SlashCommandPopup::set_selected_index(int index)
{
    if (suggestions_.empty())
    {
        selected_index_ = -1;
        return;
    }
    // -1 clears selection (matching ShortcodePopup); otherwise clamp into range.
    if (index < 0)
    {
        selected_index_ = -1;
    }
    else
    {
        selected_index_ = std::clamp(index, 0, visible_rows() - 1);
    }
}

tk::Size SlashCommandPopup::measure(tk::LayoutCtx&, tk::Size)
{
    return {kWidth, kRowHeight * float(visible_rows())};
}

void SlashCommandPopup::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

void SlashCommandPopup::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;
    int n = visible_rows();

    // Opaque background base — prevents transparent bleed-through on hover rows
    ctx.canvas.fill_rect(bounds_, pal.bg);

    for (int i = 0; i < n; ++i)
    {
        tk::Rect row{bounds_.x, bounds_.y + float(i) * kRowHeight, bounds_.w,
                     kRowHeight};

        // Background: selected > hovered > normal (normal == pal.bg already filled)
        if (i == selected_index_)
        {
            ctx.canvas.fill_rect(row, pal.sidebar_selected);
        }
        else if (i == hovered_index_)
        {
            ctx.canvas.fill_rect(row, pal.subtle_hover);
        }

        const auto& s = suggestions_[std::size_t(i)];

        // Primary text: "/name <args_hint>" — top half of row with 6px left margin
        std::string primary = "/" + s.name;
        if (!s.args_hint.empty())
        {
            primary += " " + s.args_hint;
        }
        tk::TextStyle pst{};
        pst.role   = tk::FontRole::Body;
        pst.halign = tk::TextHAlign::Leading;
        pst.valign = tk::TextVAlign::Top;
        auto pl = ctx.factory.build_text(primary, pst);
        if (pl)
        {
            tk::Size psz = pl->measure();
            // Place primary text in the upper half of the row, vertically
            // centred within that half (kRowHeight/2 = 22px per half).
            float ly = row.y + (kRowHeight * 0.5f - psz.h) * 0.5f;
            ctx.canvas.draw_text(*pl, {row.x + 6.0f, ly}, pal.text_primary);
        }

        // Secondary text: description — bottom half of row, muted colour
        if (!s.description.empty())
        {
            tk::TextStyle sst{};
            sst.role   = tk::FontRole::Small;
            sst.halign = tk::TextHAlign::Leading;
            sst.valign = tk::TextVAlign::Top;
            auto sl = ctx.factory.build_text(s.description, sst);
            if (sl)
            {
                tk::Size ssz = sl->measure();
                float half   = kRowHeight * 0.5f;
                float ly     = row.y + half + (half - ssz.h) * 0.5f;
                ctx.canvas.draw_text(*sl, {row.x + 6.0f, ly}, pal.text_muted);
            }
        }

        // Row separator (except after last row)
        if (i < n - 1)
        {
            tk::Rect sep{row.x, row.y + row.h - 1.0f, row.w, 1.0f};
            ctx.canvas.fill_rect(sep, pal.separator);
        }
    }

    // 1px border around the entire popup
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, bounds_.w, 1.0f},
                         pal.separator);
    ctx.canvas.fill_rect(
        {bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f},
        pal.separator);
    ctx.canvas.fill_rect({bounds_.x, bounds_.y, 1.0f, bounds_.h},
                         pal.separator);
    ctx.canvas.fill_rect(
        {bounds_.x + bounds_.w - 1.0f, bounds_.y, 1.0f, bounds_.h},
        pal.separator);
}

bool SlashCommandPopup::on_pointer_down(tk::Point local)
{
    pressed_index_ = row_at(local.y);
    return pressed_index_ >= 0;
}

void SlashCommandPopup::on_pointer_up(tk::Point local, bool inside_self)
{
    if (!inside_self)
    {
        pressed_index_ = -1;
        return;
    }
    int r = row_at(local.y);
    if (r >= 0 && r == pressed_index_ && r < (int)suggestions_.size())
    {
        if (on_accepted)
        {
            on_accepted(suggestions_[std::size_t(r)]);
        }
    }
    pressed_index_ = -1;
}

bool SlashCommandPopup::on_pointer_move(tk::Point local)
{
    int prev      = hovered_index_;
    hovered_index_ = row_at(local.y);
    return hovered_index_ != prev;
}

void SlashCommandPopup::on_pointer_leave()
{
    hovered_index_ = -1;
}

bool SlashCommandPopup::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    if (dy == 0.0f || visible_rows() == 0)
        return false;
    int delta = dy > 0.0f ? 1 : -1;
    int next  = std::clamp(selected_index_ + delta, 0, visible_rows() - 1);
    if (next == selected_index_)
        return false;
    selected_index_ = next;
    return true;
}

} // namespace tesseract::views
