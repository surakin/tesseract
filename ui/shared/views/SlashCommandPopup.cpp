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
    reset_transient_state_();
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

void SlashCommandPopup::paint_row(tk::PaintCtx& ctx, const tk::Rect& row,
                                  size_t index, bool /*selected*/,
                                  bool /*hovered*/)
{
    const auto& pal = ctx.theme.palette;
    const auto& s   = suggestions_[index];

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
}

} // namespace tesseract::views
